#define _XOPEN_SOURCE 700
/* O_NOFOLLOW is hidden by Darwin's strict X/Open namespace. */
#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif
/* Optional terminal document editor. The program and store effect are the
 * same ones used by the CLI and HTTP hosts; terminal state stays here. */
#include "runtime.h"
#include "store.h"
#include "store-port.h"
#ifdef PURE_EMBEDDED_PROGRAM
#include "embedded-plan.h"
#endif
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <locale.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#include <wchar.h>

typedef struct {
    char name[65];
} DocumentKey;

typedef struct {
    PureRuntime runtime;
    PureWork work[2];
    const PureProgram *program;
    const char *store;
    char key[65], prompt[65], message[160];
    unsigned char *text;
    size_t capacity, gap_start, gap_end, scroll_row, prefix, prompt_size;
    /* Logical byte position and its visual coordinate in anchor_columns. */
    size_t anchor_position, anchor_row, anchor_column, anchor_columns;
    DocumentKey *documents;
    size_t document_count, document_selected, document_scroll;
    int64_t revision;
    bool exists, dirty, prompt_open, list_open, confirm_quit, confirm_reload, anchor_valid;
    struct termios original_term;
} Editor;

static volatile sig_atomic_t stopping, resized, wake_write = -1;

static void wake_signal(int signal_number) {
    int saved_errno = errno;
    if (signal_number == SIGWINCH) resized = 1;
    else stopping = 1;
    if (wake_write >= 0) {
        const char byte = 1;
        if (write((int)wake_write, &byte, 1) < 0) { /* full: its reader is woken already */ }
    }
    errno = saved_errno;
}

static bool valid_key(const char *key) {
    size_t length = strlen(key);
    if (!length || length > 64) return false;
    for (size_t i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)key[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
    }
    return true;
}

static int compare_document_key(const void *left, const void *right) {
    return strcmp(((const DocumentKey *)left)->name, ((const DocumentKey *)right)->name);
}

static void close_list(Editor *editor) {
    free(editor->documents);
    editor->documents = NULL;
    editor->document_count = editor->document_selected = editor->document_scroll = 0;
    editor->prompt_size = 0;
    editor->prompt[0] = 0;
    editor->list_open = false;
}

/* Names are already sorted; find the first key with this prefix in-place. */
static bool find_list_prefix(Editor *editor) {
    if (!editor->prompt_size) return true;
    size_t first = 0, end = editor->document_count;
    while (first < end) {
        size_t middle = first + (end - first) / 2;
        if (strcmp(editor->documents[middle].name, editor->prompt) < 0) first = middle + 1;
        else end = middle;
    }
    if (first == editor->document_count ||
        strncmp(editor->documents[first].name, editor->prompt, editor->prompt_size)) return false;
    editor->document_selected = first;
    return true;
}

typedef struct {
    DocumentKey *items;
    size_t count, capacity;
    bool complete;
} KeyList;

/* Bound the optional browser to 8,192 names (520 KiB of key storage). */
static bool collect_key(void *context, const char *key) {
    KeyList *list = context;
    if (list->count == 8192) { list->complete = false; return false; }
    if (list->count == list->capacity) {
        size_t next = list->capacity ? list->capacity * 2 : 32;
        DocumentKey *grown = realloc(list->items, next * sizeof(*grown));
        if (!grown) { list->complete = false; return false; }
        list->items = grown;
        list->capacity = next;
    }
    memcpy(list->items[list->count++].name, key, strlen(key) + 1);
    return true;
}

/* List names from the selected byte store, not from the execution core.
 * Opening a name still goes through the store's checked READ path. */
static bool open_list(Editor *editor) {
    KeyList list = {.complete = true};
    if (pure_store_keys(editor->store, collect_key, &list) != PURE_OK || !list.complete) {
        free(list.items);
        snprintf(editor->message, sizeof(editor->message), "Document list exceeds 8,192 keys or cannot be read");
        return false;
    }
    DocumentKey *items = list.items;
    size_t count = list.count;
    if (count > 1) qsort(items, count, sizeof(*items), compare_document_key);
    close_list(editor);
    editor->documents = items;
    editor->document_count = count;
    for (size_t i = 0; i < count; ++i)
        if (!strcmp(items[i].name, editor->key)) { editor->document_selected = i; break; }
    editor->list_open = true;
    return true;
}

static const char *result_name(PureResult result) {
    switch (result) {
    case PURE_NOT_FOUND: return "not found";
    case PURE_CONFLICT: return "version conflict";
    case PURE_BUSY: return "busy";
    case PURE_BUDGET: return "request budget exceeded";
    case PURE_LIMIT: return "document limit or invalid range";
    case PURE_UNCERTAIN: return "durability unknown; inspect the stored revision before retrying";
    case PURE_DENIED: return "operation denied";
    case PURE_ALLOCATION: return "allocation failed";
    default: return "operation failed";
    }
}

static bool edit_layout(const PureProgram *program, size_t *prefix) {
    if (!program->splice || !program->write.to_end || program->write_step ||
        program->input_steps || !program->splice_offset.from_u32le ||
        !program->splice_remove.from_u32le || program->read.offset ||
        !program->read.to_end || program->read_required) return false;
    size_t start = program->splice_offset.value, remove = program->splice_remove.value;
    *prefix = program->write.offset;
    if (*prefix > PURE_OBJECT_LIMIT || program->input_required > *prefix ||
        start > *prefix || remove > *prefix || *prefix - start < 4 ||
        *prefix - remove < 4) return false;
    return start < remove ? remove - start >= 4 : start - remove >= 4;
}

static void put32(unsigned char *to, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) to[i] = (unsigned char)(value >> (8 * i));
}

static PureOutcome finish(Editor *editor, PureTicket ticket) {
    PureOutcome result;
    while (!pure_runtime_result(ticket, &result))
        if (!pure_runtime_step(&editor->runtime)) return (PureOutcome){.status = PURE_INVALID};
    return result;
}

static size_t length(const Editor *editor) {
    return editor->capacity - (editor->gap_end - editor->gap_start);
}

static unsigned char byte_at(const Editor *editor, size_t index) {
    return editor->text[index < editor->gap_start ? index :
                        index + editor->gap_end - editor->gap_start];
}

/* Skip printable ASCII directly within one side of the gap. Its terminal
 * width is always one cell; UTF-8, controls and tabs use the glyph path. */
static size_t plain_run(const Editor *editor, size_t position, size_t maximum) {
    size_t available = position < editor->gap_start ? editor->gap_start - position
                       : length(editor) - position;
    if (available > maximum) available = maximum;
    const unsigned char *bytes = editor->text + (position < editor->gap_start
                                 ? position : position + editor->gap_end - editor->gap_start);
    size_t count = 0;
    while (count < available && bytes[count] >= 32 && bytes[count] < 127) ++count;
    return count;
}

static void advance_plain(size_t count, size_t columns, size_t *row, size_t *column) {
    size_t cells = *column + count;
    *row += cells / columns;
    *column = cells % columns;
}

static void changed_at(Editor *editor, size_t position) {
    if (editor->anchor_valid && position < editor->anchor_position)
        editor->anchor_valid = false;
}

static void move_gap(Editor *editor, size_t position) {
    if (position < editor->gap_start) {
        size_t count = editor->gap_start - position;
        memmove(editor->text + editor->gap_end - count, editor->text + position, count);
        editor->gap_start -= count;
        editor->gap_end -= count;
    } else if (position > editor->gap_start) {
        size_t count = position - editor->gap_start;
        memmove(editor->text + editor->gap_start, editor->text + editor->gap_end, count);
        editor->gap_start += count;
        editor->gap_end += count;
    }
}

static bool reserve_gap(Editor *editor, size_t needed) {
    if (editor->gap_end - editor->gap_start >= needed) return true;
    size_t used = length(editor);
    if (needed > PURE_OBJECT_LIMIT - used) return false;
    size_t target = editor->capacity * 2;
    if (target < used + needed + 256) target = used + needed + 256;
    if (target > PURE_OBJECT_LIMIT + 256) target = PURE_OBJECT_LIMIT + 256;
    unsigned char *fresh = malloc(target);
    if (!fresh) return false;
    size_t right = editor->capacity - editor->gap_end;
    memcpy(fresh, editor->text, editor->gap_start);
    memcpy(fresh + target - right, editor->text + editor->gap_end, right);
    free(editor->text);
    editor->text = fresh;
    editor->gap_end = target - right;
    editor->capacity = target;
    return true;
}

static bool insert_byte(Editor *editor, unsigned char byte) {
    if (length(editor) >= PURE_OBJECT_LIMIT || !reserve_gap(editor, 1)) return false;
    changed_at(editor, editor->gap_start);
    editor->text[editor->gap_start++] = byte;
    editor->dirty = true;
    return true;
}

static size_t previous_codepoint(const Editor *editor, size_t position) {
    if (!position) return 0;
    --position;
    while (position && (byte_at(editor, position) & 0xc0) == 0x80) --position;
    return position;
}

static size_t next_codepoint(const Editor *editor, size_t position) {
    size_t count = length(editor);
    if (position == count) return count;
    ++position;
    while (position < count && (byte_at(editor, position) & 0xc0) == 0x80) ++position;
    return position;
}

static bool valid_text_span(PureSpan span) {
    for (size_t position = 0; position < span.size;) {
        mbstate_t state = {0};
        wchar_t character;
        size_t used = mbrtowc(&character, (const char *)span.data + position,
                              span.size - position, &state);
        if (used == (size_t)-1 || used == (size_t)-2 || !used ||
            (character < 32 && character != L'\n' && character != L'\t') ||
            character == 127 ||
            (character != L'\n' && character != L'\t' && wcwidth(character) < 0)) return false;
        position += used;
    }
    return true;
}

static bool read_document(Editor *editor, const char *key, bool require_existing) {
    PureTicket ticket;
    PureResult admitted = pure_runtime_accept(&editor->runtime, PURE_INPUT_READ,
                                               key, 0, 0, &ticket);
    if (admitted != PURE_OK) {
        snprintf(editor->message, sizeof(editor->message), "Read rejected: %s", result_name(admitted));
        return false;
    }
    PureOutcome result = finish(editor, ticket);
    if (require_existing && result.status == PURE_NOT_FOUND) {
        snprintf(editor->message, sizeof(editor->message), "Document disappeared; refresh the list");
        pure_runtime_release(ticket);
        return false;
    }
    if (result.status != PURE_OK && result.status != PURE_NOT_FOUND) {
        snprintf(editor->message, sizeof(editor->message), "Read failed: %s", result_name(result.status));
        pure_runtime_release(ticket);
        return false;
    }
    size_t size = result.status == PURE_OK ? result.body.size : 0;
    if (size > PURE_OBJECT_LIMIT || (size && !result.body.data) ||
        !valid_text_span(result.body)) {
        snprintf(editor->message, sizeof(editor->message), "Document is too large or is not plain UTF-8 text");
        pure_runtime_release(ticket);
        return false;
    }
    size_t capacity = size + 256;
    unsigned char *fresh = malloc(capacity);
    if (!fresh) {
        snprintf(editor->message, sizeof(editor->message), "Could not allocate document buffer");
        pure_runtime_release(ticket);
        return false;
    }
    if (size) memcpy(fresh, result.body.data, size);
    pure_runtime_release(ticket);
    free(editor->text);
    editor->text = fresh;
    editor->capacity = capacity;
    editor->gap_start = size;
    editor->gap_end = capacity;
    editor->scroll_row = 0;
    editor->anchor_valid = false;
    editor->revision = result.status == PURE_OK ? result.revision : 0;
    editor->exists = result.status == PURE_OK;
    editor->dirty = false;
    editor->confirm_reload = false;
    memcpy(editor->key, key, strlen(key) + 1);
    snprintf(editor->message, sizeof(editor->message), "%s %zu bytes · revision %" PRId64,
             editor->exists ? "Opened" : "New document", size, editor->revision);
    return true;
}

static void save_document(Editor *editor) {
    if (!editor->dirty && editor->exists) {
        snprintf(editor->message, sizeof(editor->message), "No changes to save");
        return;
    }
    PureTicket baseline;
    PureResult admitted = pure_runtime_accept(&editor->runtime, PURE_INPUT_READ,
                                               editor->key, 0, 0, &baseline);
    if (admitted != PURE_OK) {
        snprintf(editor->message, sizeof(editor->message), "Read rejected: %s", result_name(admitted));
        return;
    }
    PureOutcome old = finish(editor, baseline);
    if (old.status != PURE_OK && old.status != PURE_NOT_FOUND) {
        snprintf(editor->message, sizeof(editor->message), "Could not inspect stored version: %s", result_name(old.status));
        pure_runtime_release(baseline);
        return;
    }
    int64_t stored_revision = old.status == PURE_OK ? old.revision : 0;
    if (stored_revision != editor->revision) {
        snprintf(editor->message, sizeof(editor->message), "Version conflict: stored %" PRId64 "; draft preserved",
                 stored_revision);
        pure_runtime_release(baseline);
        return;
    }
    size_t old_size = old.status == PURE_OK ? old.body.size : 0;
    size_t draft_size = length(editor);
    size_t prefix = 0;
    while (prefix < old_size && prefix < draft_size &&
           old.body.data[prefix] == byte_at(editor, prefix)) ++prefix;
    while (prefix && prefix < old_size && (old.body.data[prefix] & 0xc0) == 0x80) --prefix;
    size_t suffix = 0;
    while (suffix < old_size - prefix && suffix < draft_size - prefix &&
           old.body.data[old_size - suffix - 1] == byte_at(editor, draft_size - suffix - 1)) ++suffix;
    while (suffix && (((old.body.data[old_size - suffix] & 0xc0) == 0x80) ||
                      ((byte_at(editor, draft_size - suffix) & 0xc0) == 0x80))) --suffix;
    size_t remove = old_size - prefix - suffix;
    size_t insert = draft_size - prefix - suffix;
    if (!remove && !insert && editor->exists) {
        editor->dirty = false;
        snprintf(editor->message, sizeof(editor->message), "No changes to save");
        pure_runtime_release(baseline);
        return;
    }
    if (prefix > UINT32_MAX || remove > UINT32_MAX || insert > PURE_OBJECT_LIMIT - editor->prefix) {
        snprintf(editor->message, sizeof(editor->message), "Edit exceeds selected program limit");
        pure_runtime_release(baseline);
        return;
    }
    PureTicket write_ticket;
    admitted = pure_runtime_accept(&editor->runtime, PURE_INPUT_WRITE, editor->key,
                                   editor->revision, editor->prefix + insert, &write_ticket);
    if (admitted != PURE_OK) {
        snprintf(editor->message, sizeof(editor->message), "Write rejected: %s", result_name(admitted));
        pure_runtime_release(baseline);
        return;
    }
    unsigned char *input = pure_runtime_ingress(write_ticket, NULL);
    memset(input, 0, editor->prefix);
    put32(input + editor->program->splice_offset.value, (uint32_t)prefix);
    put32(input + editor->program->splice_remove.value, (uint32_t)remove);
    for (size_t i = 0; i < insert; ++i) input[editor->prefix + i] = byte_at(editor, prefix + i);
    pure_runtime_release(baseline);
    if (!pure_runtime_ready(write_ticket)) {
        pure_runtime_release(write_ticket);
        snprintf(editor->message, sizeof(editor->message), "Could not publish edit");
        return;
    }
    PureOutcome result = finish(editor, write_ticket);
    pure_runtime_release(write_ticket);
    if (result.status != PURE_OK) {
        snprintf(editor->message, sizeof(editor->message), "Save failed: %s; draft preserved", result_name(result.status));
        return;
    }
    editor->revision = result.revision;
    editor->exists = true;
    editor->dirty = false;
    editor->confirm_quit = editor->confirm_reload = false;
    snprintf(editor->message, sizeof(editor->message), "Saved %zu-byte edit at byte %zu · revision %" PRId64,
             insert, prefix, result.revision);
}

static size_t glyph(const Editor *editor, size_t position, wchar_t *character,
                    int *width, char bytes[4], bool *valid) {
    unsigned char first = byte_at(editor, position);
    if (first < 128) {
        bytes[0] = (char)first;
        *character = (wchar_t)first;
        *width = 1;
        if ((first < 32 && first != '\n' && first != '\t') || first == 127 || !first) {
            *valid = false;
            bytes[0] = '?';
            *character = L'?';
        }
        return 1;
    }
    size_t available = length(editor) - position;
    if (available > 4) available = 4;
    for (size_t i = 0; i < available; ++i) bytes[i] = (char)byte_at(editor, position + i);
    mbstate_t state = {0};
    size_t used = mbrtowc(character, bytes, available, &state);
    if (used == (size_t)-1 || used == (size_t)-2 || !used ||
        (*character < 32 && *character != L'\n' && *character != L'\t') ||
        *character == 127) {
        *valid = false;
        *character = L'?';
        *width = 1;
        bytes[0] = '?';
        return 1;
    }
    *width = wcwidth(*character);
    if (*width < 0) {
        *valid = false;
        *character = L'?';
        *width = 1;
        bytes[0] = '?';
        return 1;
    }
    return used;
}

static bool text_valid(const Editor *editor) {
    for (size_t position = 0; position < length(editor);) {
        wchar_t character;
        int width;
        char bytes[4];
        bool valid = true;
        size_t used = glyph(editor, position, &character, &width, bytes, &valid);
        if (!valid) return false;
        position += used;
    }
    return true;
}

static void advance(wchar_t character, int width, size_t columns,
                    size_t *row, size_t *column) {
    if (character == L'\n') { ++*row; *column = 0; return; }
    if (character == L'\t') width = 4 - (int)(*column % 4);
    if (*column && *column + (size_t)width > columns) { ++*row; *column = 0; }
    *column += (size_t)width;
    if (*column >= columns) { ++*row; *column = 0; }
}

static void position_of(const Editor *editor, size_t target, size_t columns,
                        size_t *row, size_t *column) {
    size_t position = 0;
    *row = *column = 0;
    if (editor->anchor_valid && editor->anchor_columns == columns &&
        target >= editor->anchor_position) {
        position = editor->anchor_position;
        *row = editor->anchor_row;
        *column = editor->anchor_column;
    }
    for (; position < target;) {
        size_t run = plain_run(editor, position, target - position);
        if (run) {
            advance_plain(run, columns, row, column);
            position += run;
            continue;
        }
        wchar_t character;
        int width;
        char bytes[4];
        bool valid = true;
        size_t used = glyph(editor, position, &character, &width, bytes, &valid);
        advance(character, width, columns, row, column);
        position += used;
    }
}

static size_t closest_position(const Editor *editor, size_t target_row,
                               size_t target_column, size_t columns) {
    size_t row = 0, column = 0, position = 0, best = 0;
    bool found = false;
    if (editor->anchor_valid && editor->anchor_columns == columns &&
        target_row > editor->anchor_row) {
        row = editor->anchor_row;
        column = editor->anchor_column;
        position = editor->anchor_position;
    }
    for (; position < length(editor);) {
        if (row == target_row && column <= target_column) { best = position; found = true; }
        if (row > target_row || (row == target_row && column > target_column))
            return found ? best : position;
        wchar_t character;
        int width;
        char bytes[4];
        bool valid = true;
        size_t used = glyph(editor, position, &character, &width, bytes, &valid);
        advance(character, width, columns, &row, &column);
        position += used;
        if (row == target_row && column <= target_column) { best = position; found = true; }
    }
    return found ? best : length(editor);
}

static void dimensions(size_t *rows, size_t *columns) {
    struct winsize window = {0};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &window) || !window.ws_row || !window.ws_col) {
        *rows = 24; *columns = 80;
    } else { *rows = window.ws_row; *columns = window.ws_col; }
    if (*rows < 4) *rows = 4;
    if (*columns < 8) *columns = 8;
}

static void draw_list(Editor *editor, size_t rows, size_t columns) {
    size_t visible = rows - 3;
    bool matching = editor->document_count && (!editor->prompt_size ||
        !strncmp(editor->documents[editor->document_selected].name,
                 editor->prompt, editor->prompt_size));
    if (editor->document_selected < editor->document_scroll)
        editor->document_scroll = editor->document_selected;
    if (editor->document_selected >= editor->document_scroll + visible)
        editor->document_scroll = editor->document_selected - visible + 1;
    fputs("\033[?25l\033[H\033[2J", stdout);
    char header[80];
    snprintf(header, sizeof(header), " Pure | documents | %zu keys", editor->document_count);
    printf("%.*s", (int)columns, header);
    for (size_t row = 0; row < visible && editor->document_scroll + row < editor->document_count; ++row) {
        size_t index = editor->document_scroll + row;
        printf("\033[%zu;1H%c %.*s", row + 2,
               matching && index == editor->document_selected ? '>' : ' ',
               (int)(columns - 2), editor->documents[index].name);
    }
    printf("\033[%zu;1H%.*s", rows - 1, (int)columns,
           " Enter open  Type to find  Backspace clear  Esc back");
    char footer[230];
    if (editor->prompt_size)
        snprintf(footer, sizeof(footer), " Find: %.64s_  %.140s", editor->prompt,
                 matching ? editor->message : "No matching key");
    else if (editor->document_count)
        snprintf(footer, sizeof(footer), " %zu/%zu  %s",
                 editor->document_selected + 1, editor->document_count, editor->message);
    else snprintf(footer, sizeof(footer), " No stored documents; Ctrl+O creates a new key");
    printf("\033[%zu;1H%.*s", rows, (int)columns, footer);
    fflush(stdout);
}

static void draw(Editor *editor) {
    size_t rows, columns;
    dimensions(&rows, &columns);
    if (editor->list_open) { draw_list(editor, rows, columns); return; }
    if (editor->anchor_columns != columns) editor->anchor_valid = false;
    size_t content_rows = rows - 3;
    size_t cursor_row, cursor_column;
    position_of(editor, editor->gap_start, columns, &cursor_row, &cursor_column);
    if (cursor_row < editor->scroll_row) editor->scroll_row = cursor_row;
    if (cursor_row >= editor->scroll_row + content_rows)
        editor->scroll_row = cursor_row - content_rows + 1;
    fputs("\033[?25l\033[H\033[2J", stdout);
    char header[160];
    snprintf(header, sizeof(header), " Pure | %s | revision %" PRId64 "%s | %zu bytes",
             editor->key, editor->revision, editor->dirty ? " *" : "", length(editor));
    printf("%.*s", (int)columns, header);
    size_t row = 0, column = 0, position = 0, drawn_row = SIZE_MAX;
    size_t anchor_row = editor->scroll_row > content_rows
                        ? editor->scroll_row - content_rows : 0;
    bool keep_anchor = editor->anchor_valid && editor->anchor_row <= editor->scroll_row &&
                       editor->scroll_row - editor->anchor_row <= 2 * content_rows;
    if (editor->anchor_valid && editor->anchor_row <= editor->scroll_row) {
        row = editor->anchor_row;
        column = editor->anchor_column;
        position = editor->anchor_position;
    }
    if (!keep_anchor) editor->anchor_valid = false;
    for (; position < length(editor);) {
        if (!editor->anchor_valid && row >= anchor_row) {
            editor->anchor_position = position;
            editor->anchor_row = row;
            editor->anchor_column = column;
            editor->anchor_columns = columns;
            editor->anchor_valid = true;
        }
        if (row >= editor->scroll_row + content_rows) break;
        if (row < editor->scroll_row) {
            size_t remaining = length(editor) - position;
            size_t rows_to_view = editor->scroll_row - row;
            size_t cells_to_view = rows_to_view > (remaining + column) / columns
                                   ? remaining : rows_to_view * columns - column;
            size_t run = plain_run(editor, position, cells_to_view);
            if (run) {
                advance_plain(run, columns, &row, &column);
                position += run;
                continue;
            }
        }
        wchar_t character;
        int width;
        char bytes[4];
        bool valid = true;
        size_t used = glyph(editor, position, &character, &width, bytes, &valid);
        size_t next_row = row, next_column = column;
        size_t visible_width = character == L'\t' ? 4 - column % 4 : (size_t)width;
        if (character != L'\n' && column && column + visible_width > columns) {
            ++row;
            column = 0;
        }
        if (row >= editor->scroll_row && row < editor->scroll_row + content_rows &&
            character != L'\n') {
            if (drawn_row != row) {
                printf("\033[%zu;1H", row - editor->scroll_row + 2);
                drawn_row = row;
            }
            if (character == L'\t') {
                int spaces = 4 - (int)(column % 4);
                while (spaces--) putchar(' ');
            } else fwrite(bytes, 1, used, stdout);
        }
        row = next_row;
        column = next_column;
        advance(character, width, columns, &row, &column);
        position += used;
    }
    printf("\033[%zu;1H%.*s", rows - 1, (int)columns,
           " Ctrl+S save  Ctrl+O open  Ctrl+R reload  Ctrl+Q quit  arrows move");
    char footer[230];
    if (editor->prompt_open)
        snprintf(footer, sizeof(footer), " Open key: %s_  (Enter to open, Esc to cancel)", editor->prompt);
    else snprintf(footer, sizeof(footer), " %s", editor->message);
    printf("\033[%zu;1H%.*s", rows, (int)columns, footer);
    if (editor->prompt_open) {
        size_t prompt_column = editor->prompt_size + strlen(" Open key: ") + 1;
        if (prompt_column > columns) prompt_column = columns;
        printf("\033[%zu;%zuH", rows, prompt_column);
    } else printf("\033[%zu;%zuH", cursor_row - editor->scroll_row + 2,
                  cursor_column + 1);
    fputs("\033[?25h", stdout);
    fflush(stdout);
}

static int read_byte(int wake_read, int timeout_ms) {
    struct pollfd fds[2] = {{.fd = STDIN_FILENO, .events = POLLIN},
                            {.fd = wake_read, .events = POLLIN}};
    int ready = poll(fds, 2, timeout_ms);
    if (ready < 0) return errno == EINTR ? -2 : -1;
    if (!ready) return -3;
    if (fds[1].revents) {
        char buffer[32];
        while (read(wake_read, buffer, sizeof(buffer)) > 0) { }
        return -2;
    }
    unsigned char byte;
    return read(STDIN_FILENO, &byte, 1) == 1 ? byte : -1;
}

static void move_vertical(Editor *editor, int delta) {
    size_t rows, columns, row, column;
    dimensions(&rows, &columns);
    (void)rows;
    position_of(editor, editor->gap_start, columns, &row, &column);
    if (delta < 0 && !row) return;
    move_gap(editor, closest_position(editor, delta < 0 ? row - 1 : row + 1,
                                      column, columns));
}

static void handle_escape(Editor *editor, int wake_read) {
    int a = read_byte(wake_read, 30);
    if (a != '[') {
        if (editor->list_open) close_list(editor);
        editor->prompt_open = false;
        return;
    }
    int b = read_byte(wake_read, 30);
    if (editor->list_open) {
        if (b == 'A' || b == 'B' || b == 'H' || b == 'F') {
            editor->prompt_size = 0;
            editor->prompt[0] = 0;
        }
        if (b == 'A' && editor->document_selected) --editor->document_selected;
        else if (b == 'B' && editor->document_selected + 1 < editor->document_count)
            ++editor->document_selected;
        else if (b == 'H') editor->document_selected = 0;
        else if (b == 'F' && editor->document_count)
            editor->document_selected = editor->document_count - 1;
        return;
    }
    if (b == 'A') move_vertical(editor, -1);
    else if (b == 'B') move_vertical(editor, 1);
    else if (b == 'C') move_gap(editor, next_codepoint(editor, editor->gap_start));
    else if (b == 'D') move_gap(editor, previous_codepoint(editor, editor->gap_start));
    else if (b == 'H') {
        size_t position = editor->gap_start;
        while (position && byte_at(editor, position - 1) != '\n') --position;
        move_gap(editor, position);
    } else if (b == 'F') {
        size_t position = editor->gap_start;
        while (position < length(editor) && byte_at(editor, position) != '\n') ++position;
        move_gap(editor, position);
    } else if (b == '3' && read_byte(wake_read, 30) == '~') {
        size_t next = next_codepoint(editor, editor->gap_start);
        if (next > editor->gap_start) {
            changed_at(editor, editor->gap_start);
            editor->gap_end += next - editor->gap_start;
            editor->dirty = true;
        }
    }
}

static void handle_key(Editor *editor, int key, int wake_read) {
    if (key < 0) return;
    if (editor->prompt_open) {
        if (key == 27) {
            if (read_byte(wake_read, 30) == '[') {
                int next = read_byte(wake_read, 30);
                if (next >= '0' && next <= '9') (void)read_byte(wake_read, 30);
            } else editor->prompt_open = false;
            return;
        }
        if (key == 127 || key == 8) {
            if (editor->prompt_size) editor->prompt[--editor->prompt_size] = 0;
            return;
        }
        if (key == 13 || key == 10) {
            if (valid_key(editor->prompt)) (void)read_document(editor, editor->prompt, false);
            else snprintf(editor->message, sizeof(editor->message), "Key must be 1–64 ASCII letters, digits, _ or -");
            editor->prompt_open = false;
            return;
        }
        if (editor->prompt_size < 64 &&
            ((key >= 'a' && key <= 'z') || (key >= 'A' && key <= 'Z') ||
             (key >= '0' && key <= '9') || key == '_' || key == '-')) {
            editor->prompt[editor->prompt_size++] = (char)key;
            editor->prompt[editor->prompt_size] = 0;
        }
        return;
    }
    if (editor->list_open) {
        if (key == 27) { handle_escape(editor, wake_read); return; }
        if ((key == 127 || key == 8) && editor->prompt_size) {
            editor->prompt[--editor->prompt_size] = 0;
            (void)find_list_prefix(editor);
            return;
        }
        if (editor->prompt_size < 64 &&
            ((key >= 'a' && key <= 'z') || (key >= 'A' && key <= 'Z') ||
             (key >= '0' && key <= '9') || key == '_' || key == '-')) {
            editor->prompt[editor->prompt_size++] = (char)key;
            editor->prompt[editor->prompt_size] = 0;
            (void)find_list_prefix(editor);
            return;
        }
        if ((key == 13 || key == 10) && editor->document_count &&
            (!editor->prompt_size || !strncmp(editor->documents[editor->document_selected].name,
                                              editor->prompt, editor->prompt_size))) {
            if (read_document(editor, editor->documents[editor->document_selected].name, true))
                close_list(editor);
            return;
        }
        if (key == 12) { (void)open_list(editor); return; }
        if (key == 15) close_list(editor);
        else if (key != 17 && key != 3) return;
    }
    if (key == 17 || key == 3) {
        if (editor->dirty && !editor->confirm_quit) {
            editor->confirm_quit = true;
            snprintf(editor->message, sizeof(editor->message), "Unsaved draft; press Ctrl+Q again to discard and quit");
        } else stopping = 1;
        return;
    }
    if (key != 18) editor->confirm_reload = false;
    if (key != 17) editor->confirm_quit = false;
    if (key == 19) { if (text_valid(editor)) save_document(editor);
        else snprintf(editor->message, sizeof(editor->message), "Document contains invalid UTF-8 or control bytes");
        return; }
    if (key == 18) {
        if (editor->dirty && !editor->confirm_reload) {
            editor->confirm_reload = true;
            snprintf(editor->message, sizeof(editor->message), "Unsaved draft; press Ctrl+R again to discard and reload");
        } else (void)read_document(editor, editor->key, false);
        return;
    }
    if (key == 15) {
        if (editor->dirty) snprintf(editor->message, sizeof(editor->message), "Save or reload the draft before opening another key");
        else { editor->prompt_open = true; editor->prompt_size = 0; editor->prompt[0] = 0; }
        return;
    }
    if (key == 12) {
        if (editor->dirty)
            snprintf(editor->message, sizeof(editor->message), "Save or reload the draft before browsing documents");
        else (void)open_list(editor);
        return;
    }
    if (key == 27) { handle_escape(editor, wake_read); return; }
    if (key == 127 || key == 8) {
        size_t before = previous_codepoint(editor, editor->gap_start);
        if (before < editor->gap_start) {
            changed_at(editor, before);
            editor->gap_start = before;
            editor->dirty = true;
        }
        return;
    }
    if (key == 13) key = '\n';
    if (key == '\n' || key == '\t' || key >= 32) {
        if (!insert_byte(editor, (unsigned char)key))
            snprintf(editor->message, sizeof(editor->message), "Document reached the byte limit");
    }
}

static bool nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

static bool raw_terminal(const struct termios *original) {
    struct termios raw = *original;
    raw.c_iflag &= (tcflag_t)~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= (tcflag_t)~OPOST;
    raw.c_cflag |= CS8;
    raw.c_lflag &= (tcflag_t)~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    return tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == 0;
}

int main(int argc, char **argv) {
#ifdef PURE_EMBEDDED_PROGRAM
    if (argc != 4 || strcmp(argv[1], "--store") || !valid_key(argv[3])) {
        fputs("usage: pure-edit-tui-fixed --store DIRECTORY KEY\n", stderr);
        return 2;
    }
    const char *store_path = argv[2], *key = argv[3];
    const PureProgram *program = &pure_embedded_program;
#else
    if (argc != 6 || strcmp(argv[1], "--program") || strcmp(argv[3], "--store") ||
        !valid_key(argv[5])) {
        fputs("usage: pure-edit-tui --program FILE --store DIRECTORY KEY\n", stderr);
        return 2;
    }
    const char *store_path = argv[4], *key = argv[5];
    PureProgram loaded;
    char error[160];
    if (!pure_program_load(argv[2], &loaded, error, sizeof(error))) {
        fprintf(stderr, "Program: %s\n", error);
        return 2;
    }
    const PureProgram *program = &loaded;
#endif
    size_t prefix;
    if (!edit_layout(program, &prefix)) {
        fputs("Program needs a complete-document read and two u32le splice fields with trailing insertion bytes\n", stderr);
        return 2;
    }
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        fputs("terminal editor requires a TTY\n", stderr);
        return 2;
    }
    setlocale(LC_CTYPE, "");
    Editor editor = {.store = store_path, .program = program, .prefix = prefix};
    if (pure_store_init(editor.store) != PURE_OK ||
        !pure_runtime_init(&editor.runtime, editor.work, 2, program,
                           pure_store_host(editor.store), 4u * 1024u * 1024u) ||
        !read_document(&editor, key, false)) {
        fprintf(stderr, "Could not open document: %s\n", editor.message);
        free(editor.text);
        return 2;
    }
    int wake[2];
    if (pipe(wake) || !nonblocking(wake[0]) || !nonblocking(wake[1]) ||
        tcgetattr(STDIN_FILENO, &editor.original_term)) {
        perror("terminal setup");
        free(editor.text);
        return 2;
    }
    wake_write = wake[1];
    struct sigaction action = {.sa_handler = wake_signal};
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) || sigaction(SIGTERM, &action, NULL) ||
        sigaction(SIGWINCH, &action, NULL)) {
        perror("sigaction");
        wake_write = -1;
        close(wake[0]);
        close(wake[1]);
        free(editor.text);
        return 2;
    }
    if (!raw_terminal(&editor.original_term)) {
        perror("raw terminal");
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &editor.original_term);
        wake_write = -1;
        close(wake[0]);
        close(wake[1]);
        free(editor.text);
        return 2;
    }
    fputs("\033[?1049h", stdout);
    while (!stopping) {
        draw(&editor);
        int key = read_byte(wake[0], -1);
        if (resized) resized = 0;
        if (key == -1) { stopping = 1; break; }
        handle_key(&editor, key, wake[0]);
    }
    fputs("\033[?25h\033[?1049l", stdout);
    fflush(stdout);
    wake_write = -1;
    close(wake[0]);
    close(wake[1]);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &editor.original_term);
    pure_runtime_stop(&editor.runtime);
    close_list(&editor);
    free(editor.text);
    return 0;
}
