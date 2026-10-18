#define _POSIX_C_SOURCE 200809L
#include "module-store.h"
#include "store.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Test host for a verified module: reads commands from stdin and prints one
 * JSON snapshot per command. Not a product interface. With --store the app's
 * states live in that tree store: it starts from them, and frame changes are
 * saved at the end of input or by save. --batch N defers saving and saves
 * every N commands (and at the end), so N events share one commit.
 *   down NAME | up NAME | frame [N] | type NAME HEX|- | message NAME HEX|- | tick NAME | time MS | replace PATH | save | show
 *   budget N (the work an event may do) | memory N (what the states may hold) | work (the last event's work)
 * (027's 4d: down and up a key's press and release, type a text's, tick a timer's and frame a frame's message, each by
 * the input bound so; type gives a text input its bytes, message a message input its payload, laid out as the module says;
 * message NAME @PATH gives it the bytes of the file at PATH, of any size.)
 * Text states and text views print as JSON strings; cursors as byte positions;
 * lists as arrays of objects (a bytes field as a string of its hex digits); indices as positions; a table view as an
 * array with an object of its columns per row. A failed view or cell is null. */
/* time MS sets the time events see (milliseconds since 1970); tick NAME is a timer's tick. */

static bool load(const char *path, PureModule *module) {
    char error[160];
    if (pure_module_read(path, module, error, sizeof(error)) == PURE_OK) return true;
    fprintf(stderr, "%s: %s\n", path, error);
    return false;
}

static bool json_span(void *context, PureSpan span) {
    (void)context;
    for (size_t i = 0; i < span.size; ++i) {
        unsigned char c = span.data[i];
        if (c == '"' || c == '\\') printf("\\%c", c);
        else if (c < 0x20) printf("\\u%04x", c);
        else putchar(c);
    }
    return true;
}

/* A text view is collected first: it may fail after some bytes. */
typedef struct { unsigned char bytes[1 << 16]; size_t used; bool fits; } Collected;

static bool collect(void *context, PureSpan span) {
    Collected *c = context;
    c->fits = c->fits && span.size <= sizeof(c->bytes) - c->used;
    if (c->fits && span.size) memcpy(c->bytes + c->used, span.data, span.size);
    if (c->fits) c->used += span.size;
    return true;
}

static void show_cell(const PureModule *m, uint32_t field, PureCell cell, bool kept);

/* A list kept past an event holds a list field's value in its items' stored form (Pure 027's stage 3): its items. */
static void show_value_items(const PureModule *m, uint32_t record, const unsigned char *bytes, size_t size) {
    const PureRecord *r = &m->record[record];
    PureCell cells[PURE_RECORD_FIELDS];
    putchar('[');
    for (size_t at = 0, item = 0; pure_items_next(m, record, (PureSpan){bytes, size}, &at, cells); ++item) {
        printf("%s{", item ? "," : "");
        for (unsigned f = 0; f < r->count; ++f) {
            printf("%s\"%s\":", f ? "," : "", m->record_field_names[r->first + f]);
            show_cell(m, r->first + f, cells[f], true);
        }
        putchar('}');
    }
    putchar(']');
}

static void show_hex(PureSpan bytes) {
    putchar('"');
    for (size_t i = 0; i < bytes.size; ++i) printf("%02x", bytes.data[i]);
    putchar('"');
}

static void show_cell(const PureModule *m, uint32_t field, PureCell cell, bool kept) {
    uint8_t type = m->record_field_type[field];
    if (type == PURE_RECORD_BOOL) printf("%s", cell.num ? "true" : "false");
    else if (type == PURE_RECORD_I64) printf("%" PRId64, cell.i64);
    else if (type == PURE_RECORD_NUMBER) printf("%.17g", cell.num);
    else if (type == PURE_RECORD_LIST && kept) show_value_items(m, (uint32_t)m->record_field_default[field].i64, cell.text, cell.size);
    else if (type == PURE_RECORD_LIST) printf("%" PRId64, cell.i64); /* an event's list: its number among the event's */
    else if (type == PURE_RECORD_BYTES) show_hex((PureSpan){cell.text, cell.size});
    else {
        putchar('"');
        json_span(NULL, (PureSpan){cell.text, cell.size});
        putchar('"');
    }
}

static void show_list(const PureModule *m, const PureList *list, const PureRecord *record, bool kept) {
    putchar('[');
    for (size_t at = 0; at < pure_list_count(list); ++at) {
        printf("%s{", at ? "," : "");
        for (unsigned f = 0; f < record->count; ++f) {
            printf("%s\"%s\":", f ? "," : "", m->record_field_names[record->first + f]);
            show_cell(m, record->first + f, pure_list_get(list, at, f), kept);
        }
        putchar('}');
    }
    putchar(']');
}

/* A number, bool, i64, text or bytes view, or a column at a row (a list column's cell as its items). */
static void show_value(PureApp *app, uint32_t view, size_t row) {
    const PureModule *m = app->module;
    static Collected text;
    text.used = 0;
    text.fits = true;
    double value;
    int64_t integer;
    bool column = m->view_table[view];
    if (m->view_type[view] == PURE_VIEW_I64) {
        if (!(column ? pure_app_cell_i64(app, view, row, &integer) : pure_app_view_i64(app, view, &integer))) printf("null");
        else printf("%" PRId64, integer);
    } else if (m->view_type[view] == PURE_VIEW_TEXT || m->view_type[view] == PURE_VIEW_BYTES) {
        PureResult result = column ? pure_app_cell_text(app, view, row, collect, &text)
                                   : pure_app_view_text(app, view, collect, &text);
        if (result != PURE_OK || !text.fits) printf("null");
        else if (m->view_type[view] == PURE_VIEW_BYTES) show_hex((PureSpan){text.bytes, text.used});
        else {
            putchar('"');
            json_span(NULL, (PureSpan){text.bytes, text.used});
            putchar('"');
        }
    } else if (m->view_type[view] == PURE_VIEW_LIST) {
        PureSpan items;
        if (pure_app_cell_items(app, view, row, &items) != PURE_OK) printf("null");
        else show_value_items(m, m->view_record[view] - 1u, items.data, items.size);
    } else if (!(column ? pure_app_cell(app, view, row, &value) : pure_app_view(app, view, &value))) printf("null");
    else if (m->view_type[view] == PURE_VIEW_BOOL) printf("%s", value ? "true" : "false");
    else printf("%.17g", value);
}

/* The frames that ran, and the device events no message takes (a release, a frame): a surface's events all the same. */
static unsigned long long ticks, untaken;

static void show(PureApp *app, const char *failure) {
    const PureModule *m = app->module;
    printf("{\"states\":{");
    for (uint32_t i = 0; i < m->states; ++i) {
        printf("%s\"%s\":", i ? "," : "", m->state_names[i]);
        if (m->state_type[i] == PURE_STATE_TEXT) {
            putchar('"');
            pure_draft_read(app->texts[i], 0, pure_draft_size(app->texts[i]), json_span, NULL);
            putchar('"');
        } else if (m->state_type[i] == PURE_STATE_LIST)
            show_list(m, app->lists[i], &m->record[m->state_ref[i]], !(m->state_flags[i] & PURE_LIST_ROWS >> 8));
        else if (m->state_type[i] == PURE_STATE_BOOL) printf("%s", app->values[i].num ? "true" : "false");
        else if (m->state_type[i] == PURE_STATE_I64) printf("%" PRId64, app->values[i].i64);
        else printf("%.17g", app->values[i].num);
    }
    printf("},\"views\":{");
    bool first = true;
    for (uint32_t i = 0; i < m->views; ++i) {
        if (m->view_table[i]) continue;
        printf("%s\"%s\":", first ? "" : ",", m->view_names[i]);
        first = false;
        if (m->view_type[i] != PURE_VIEW_LIST) { show_value(app, i, 0); continue; }
        putchar('[');
        for (size_t row = 0; row < pure_app_rows(app, i); ++row) {
            printf("%s{", row ? "," : "");
            for (uint32_t c = 0, shown = 0; c < m->views; ++c) {
                if (m->view_table[c] != i + 1) continue;
                printf("%s\"%s\":", shown++ ? "," : "", m->view_names[c]);
                show_value(app, c, row);
            }
            putchar('}');
        }
        putchar(']');
    }
    printf("},\"tick\":%llu,\"successfulEvents\":%llu,\"fault\":\"%s\",\"error\":%s%s%s}\n",
           ticks, (unsigned long long)app->events + untaken, pure_fault_name(app->fault),
           failure ? "\"" : "", failure ? failure : "null", failure ? "\"" : "");
}

/* The input bound so (027's 4d) with the key of input key (a press's release), or -1. */
static int bound(const PureModule *m, uint8_t binding, int key) {
    for (uint32_t i = 0; i < m->inputs; ++i)
        if (m->input[i].binding == binding && (key < 0 || !strcmp(m->input_keys[i], m->input_keys[key]))) return (int)i;
    return -1;
}

static int hex(char c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

int main(int argc, char **argv) {
    static PureModule modules[2];
    bool stored = argc >= 4 && !strcmp(argv[2], "--store");
    long batch = argc == 6 && stored && !strcmp(argv[4], "--batch") ? strtol(argv[5], NULL, 10) : 0;
    bool usage = argc != 2 && !(stored && argc == 4) && !(batch > 0 && batch <= 100000);
    if (usage || !load(argv[1], &modules[0])) {
        if (usage) fputs("pure-run MODULE [--store PATH [--batch N]] < commands\n", stderr);
        return 2;
    }
    if (stored && pure_store_init(argv[3]) != PURE_OK) { fprintf(stderr, "cannot open store %s\n", argv[3]); return 2; }
    unsigned active = 0;
    PureApp app;
    PureResult started = pure_app_start(&app, &modules[active], stored ? pure_tree_data(argv[3]) : (PureData){0});
    if (started != PURE_OK) { fprintf(stderr, "cannot start (%d)\n", (int)started); return 2; }
    /* Frames wait for the next event's save (or the end's, or save): a stored app saves when the rest come. */
    if (stored && !pure_app_defer(&app, true)) { fputs("cannot defer saving\n", stderr); return 2; }
    long unsaved = 0;
    static char line[1 << 16];
    static unsigned char text[1 << 15];
    int status = 0;
    while (fgets(line, sizeof(line), stdin)) {
        char command[16] = "", argument[256] = "", data[sizeof(line)] = "";
        const char *failure = NULL;
        line[strcspn(line, "\r\n")] = 0;
        if (sscanf(line, "%15s %255s %65535s", command, argument, data) < 1) continue;
        bool frame = false;
        if (!strcmp(command, "down") || !strcmp(command, "up")) { /* a key's press, or its release when a message takes it */
            int input = pure_module_input(app.module, argument);
            if (input < 0 || app.module->input[input].binding != PURE_BIND_PRESS) failure = "unknown input";
            else if (command[0] == 'u') input = bound(app.module, PURE_BIND_RELEASE, input);
            if (!failure && input < 0) ++untaken, app.fault = PURE_FAULT_NONE; /* an event that runs nothing */
            else if (!failure && pure_app_message(&app, (uint32_t)input, (PureSpan){0}) != PURE_OK) failure = "event failed";
        } else if (!strcmp(command, "type") || !strcmp(command, "message")) {
            int input = pure_module_input(app.module, argument);
            size_t length = strcmp(data, "-") ? strlen(data) / 2 : 0;
            for (size_t i = 0; i < length && length * 2 == strlen(data); ++i)
                text[i] = (unsigned char)(hex(data[2 * i]) << 4 | hex(data[2 * i + 1]));
            PureSpan given = {text, length};
            unsigned char *read = NULL; /* message NAME @PATH: the file's bytes */
            if (data[0] == '@') {
                FILE *file = fopen(data + 1, "rb");
                long size = file && !fseek(file, 0, SEEK_END) ? ftell(file) : -1;
                if (size >= 0 && !fseek(file, 0, SEEK_SET) && (read = malloc(size ? (size_t)size : 1)) &&
                    fread(read, 1, (size_t)size, file) == (size_t)size)
                    given = (PureSpan){read, (size_t)size};
                else failure = "cannot read the file";
                if (file) fclose(file);
            }
            if (failure) {
            } else if (input < 0) failure = "unknown input";
            else if (pure_app_message(&app, (uint32_t)input, given) != PURE_OK) failure = "event failed";
            free(read);
        } else if (!strcmp(command, "tick")) {
            int input = pure_module_input(app.module, argument);
            if (input < 0) failure = "unknown input";
            else if (pure_app_message(&app, (uint32_t)input, (PureSpan){0}) != PURE_OK) failure = "event failed";
        } else if (!strcmp(command, "time")) {
            char *end;
            double now = strtod(argument, &end);
            if (!*argument || *end || !(now >= 0 && now < 1e15)) failure = "time";
            else app.now = now;
        } else if (!strcmp(command, "frame")) {
            long count = *argument ? strtol(argument, NULL, 10) : 1;
            int input = bound(app.module, PURE_BIND_FRAME, -1);
            double dt = 1.0 / 60.0; /* its one field */
            frame = true;
            if (count < 1 || count > 600) failure = "frame count";
            for (long i = 0; !failure && i < count; ++i) {
                if (input >= 0 && pure_app_message(&app, (uint32_t)input, (PureSpan){(const unsigned char *)&dt, 8}) != PURE_OK)
                    failure = "event failed";
                else if (++ticks, input < 0) ++untaken, app.fault = PURE_FAULT_NONE;
            }
        } else if (!strcmp(command, "replace")) {
            if (!load(argument, &modules[!active])) failure = "invalid module";
            else if (pure_app_replace(&app, &modules[!active]) != PURE_OK) failure = "replace failed";
            else active = !active;
        } else if (!strcmp(command, "save")) {
            if (pure_app_save(&app) != PURE_OK) failure = "save failed";
        } else if (!strcmp(command, "budget")) { /* the work each event may do (0: the default) */
            char *end;
            unsigned long long budget = strtoull(argument, &end, 10);
            if (!*argument || *end) failure = "budget";
            else pure_app_work(&app, budget);
        } else if (!strcmp(command, "memory")) { /* what the states may hold (0: no budget) */
            char *end;
            unsigned long long bytes = strtoull(argument, &end, 10);
            if (!*argument || *end) failure = "memory";
            else pure_app_memory(&app, (size_t)bytes);
        } else if (!strcmp(command, "work")) printf("{\"work\":%llu}\n", (unsigned long long)app.work); /* the last event's */
        else if (strcmp(command, "show")) failure = "unknown command";
        if (batch && strcmp(command, "show") && ++unsaved == batch) {
            unsaved = 0;
            if (pure_app_save(&app) != PURE_OK) failure = "save failed";
        } else if (stored && !batch && !frame && strcmp(command, "show") && pure_app_save(&app) != PURE_OK)
            failure = "save failed";
        if (failure) status = 1;
        show(&app, failure);
    }
    if (pure_app_save(&app) != PURE_OK) { fputs("cannot save\n", stderr); status = 1; }
    pure_app_stop(&app);
    return status;
}
