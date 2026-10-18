#include "module-save.h"
#include "module-text.h"
#include "log.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Stored forms. values: "PVS1", then per state {u8 type, u8 name length,
 * name, binary64 bits (little-endian)}. A text state: its UTF-8 bytes.
 *
 * A list lives in an image and in the log (core/log.h). Its image, the file
 * image-NAME beside the store: "PLI1", u8 field count, per field {u8 0 number
 * | 1 text, u8 name length, name}, u64le the number of the save it holds,
 * then its items (module-list.h). The log has an entry for each save that
 * changed lists: per list changed, {u8 name length, the name, u32le length,
 * the records of its changes (module-list.h) in its image's fields}. A save
 * with such an entry that also commits the tree (numbers, texts, links)
 * stores its number there as the state "seq": the entry counts
 * only when the tree holds its number (or a later one), so both are kept or
 * neither. A start
 * reads each list's image and makes again the changes logged after the save
 * it holds. A list gets an image before any change of it is logged (a new
 * one, one stored before images, one whose record changed: in its new
 * fields), and once its logged changes outgrow its items (and JOURNAL_MIN) a
 * checkpoint writes its image anew from a snapshot; the log then lets go the
 * entries every image holds. Before images a list was the tree's value
 * "list-NAME": "PLS2" (the header, u64le the bytes of its items, its items,
 * then records) or "PLS1" (the header, then items); a start reads it, writes
 * its image and empties it. */
static const unsigned char values_magic[4] = {'P', 'V', 'S', '1'}, image_magic[4] = {'P', 'L', 'I', '1'},
                           list_magic[4] = {'P', 'L', 'S', '2'}, list_magic_1[4] = {'P', 'L', 'S', '1'};
/* The outbox's image names the fields its sends were laid out with: "PLO1"
 * in the place of "PLI1", and after the save it holds u8 outputs, each {u8
 * name length, name, u8 field count, each field's {u8 type, u8 name length,
 * name}} (the module's outputs when it was written). The sends logged after
 * it are laid out so too: a module whose outputs differ writes it again
 * first. So what they mean moves with them, whatever the links hold. */
static const unsigned char sends_magic[4] = {'P', 'L', 'O', '1'};
/* Logged changes a list keeps before its image is written again: at least
 * JOURNAL_MIN, else as many bytes as its items; and whatever the lists, the
 * log is kept within as many bytes as all their items (and LOG_KEPT) by
 * writing the image that holds it back. */
#ifdef PURE_STORE_TEST
enum { JOURNAL_MIN = 64 };
#define LOG_KEPT (UINT64_C(4) << 10)
unsigned long pure_save_test_checkpoints;
#else
enum { JOURNAL_MIN = 1 << 20 };
#define LOG_KEPT (UINT64_C(64) << 20)
#endif
/* links: "PLK4"; u8 outputs, each {u8 name length, name, u8 field count, each
 * field's {u8 type, u8 name length, name}, u64le last number} (the module's
 * outputs, then those it dropped, without fields). "PLK3" kept the fields'
 * types alone. Before the outbox list, "PLK2" kept the sent
 * messages after them, to the end, each {u8 output (in the table), u64le
 * number, u32le size, payload}; before the heard list, "PLK1" also kept the
 * numbers heard between the two: u16le sources, each {u8 length, the name,
 * u64le last number heard}. A start moves both to their lists.
 *
 * The runtime's lists (module.h, PureApp) are stored as list states are, by
 * names no state has (a state's is an identifier): the numbers heard
 * ("0heard": source text, seq number) and the outbox ("0sent": output text,
 * seq number, payload bytes). What only grows with the sources a node hears
 * from, or with the sends its receivers have not stored yet, so costs a
 * save only the records of its changes. */
static const unsigned char links_magic[4] = {'P', 'L', 'K', '4'}, links_magic_3[4] = {'P', 'L', 'K', '3'},
                           links_magic_2[4] = {'P', 'L', 'K', '2'}, links_magic_1[4] = {'P', 'L', 'K', '1'};
typedef struct { const char *name; unsigned fields; uint8_t kinds[3]; char names[3][PURE_MODULE_NAME]; } RuntimeList;
static const RuntimeList runtime_lists[PURE_LIST_RUNTIME] = {
    {"0heard", 2, {1, 0}, {"source", "seq"}},
    {"0sent", 3, {1, 0, 2}, {"output", "seq", "payload"}},
};
static const PureVersion unsaved = {UINT64_MAX, UINT64_MAX};

static unsigned char *put_le(unsigned char *at, uint64_t value, unsigned width) {
    for (unsigned i = 0; i < width; ++i) *at++ = (unsigned char)(value >> (8 * i));
    return at;
}

static uint64_t get_le(const unsigned char *at, unsigned width) {
    uint64_t value = 0;
    for (unsigned i = 0; i < width; ++i) value |= (uint64_t)at[i] << (8 * i);
    return value;
}

bool pure_save_enabled(const PureApp *app) { return app->data.state && app->data.save; }

static PureResult open_state(PureApp *app, const char *prefix, const char *name, PureDraft **out) {
    char key[64];
    snprintf(key, sizeof(key), "%s%s", prefix, name);
    return app->data.state(app->data.context, key, out);
}

/* The whole value of a draft, contiguous; free it. */
typedef struct { unsigned char *bytes; size_t used; } Whole;

static bool gather(void *context, PureSpan span) {
    Whole *w = context;
    memcpy(w->bytes + w->used, span.data, span.size);
    w->used += span.size;
    return true;
}

static PureResult read_whole(const PureDraft *draft, unsigned char **out, size_t *size) {
    *size = pure_draft_size(draft);
    Whole w = {malloc(*size ? *size : 1), 0};
    if (!w.bytes) return PURE_ALLOCATION;
    PureResult result = pure_draft_read(draft, 0, *size, gather, &w);
    if (result != PURE_OK) { free(w.bytes); return result; }
    *out = w.bytes;
    return PURE_OK;
}

/* One named entry of a stored form: u8 kind, u8 name length, the name (an
 * identifier), then width more bytes. */
PURE_COLD static bool named(const unsigned char *bytes, size_t size, size_t *at, size_t width, uint8_t *kind,
                  char name[PURE_MODULE_NAME]) {
    if (size - *at < 2) return false;
    size_t length = bytes[*at + 1];
    if (size - *at - 2 < length + width || !length || length >= PURE_MODULE_NAME) return false;
    for (size_t i = 0; i < length; ++i) {
        unsigned char c = bytes[*at + 2 + i];
        bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
        if (!letter && !(i && c >= '0' && c <= '9')) return false;
    }
    *kind = bytes[*at];
    memcpy(name, bytes + *at + 2, length);
    name[length] = 0;
    *at += 2 + length;
    return true;
}

PURE_COLD PureResult pure_save_open_text(PureApp *app, const PureModule *m, uint32_t state, PureDraft **out,
                               PureVersion *saved) {
    PureResult result = open_state(app, "text-", m->state_names[state], out);
    if (result != PURE_OK) return result;
    *saved = pure_draft_version(*out);
    if (!pure_draft_revision(*out)) {
        uint32_t constant = m->state_ref[state];
        PureSpan initial = {m->constant_data + m->constant_offset[constant], m->constant_length[constant]};
        result = pure_draft_splice(*out, 0, pure_draft_size(*out), initial);
        *saved = unsaved;
    } else {
        unsigned char *bytes;
        size_t size;
        result = read_whole(*out, &bytes, &size);
        if (result == PURE_OK) {
            if (!pure_utf8_valid((PureSpan){bytes, size})) result = PURE_STORE_ERROR;
            free(bytes);
        }
    }
    if (result != PURE_OK) { pure_draft_close(*out); *out = NULL; }
    return result;
}

/* ---- Lists: images and the log */

/* A runtime list's image header at most (its three fields at most); and what any of m's takes at most (027's 4f-3a): a
 * record graph's, or the outbox's naming m's outputs. */
enum { RUNTIME_HEADER = 4 + 1 + 3 * (2 + PURE_MODULE_NAME) + 8 };
static size_t header_room(const PureModule *m) {
    size_t records = 6 + 8 + 1, outbox = RUNTIME_HEADER + 1;
    for (uint32_t r = 0; r < m->records; ++r) records += 1 + (size_t)m->record[r].count * (3 + PURE_MODULE_NAME);
    for (uint32_t i = 0; i < m->inputs; ++i)
        if (m->input[i].kind == PURE_KIND_OUTPUT)
            outbox += 2 + PURE_MODULE_NAME + (size_t)m->input[i].field_count * (2 + PURE_MODULE_NAME);
    return records > outbox ? records : outbox;
}

static size_t header_of(unsigned count, const uint8_t *text, const char (*names)[PURE_MODULE_NAME], uint64_t seq,
                        unsigned char *out) {
    size_t at = 0;
    memcpy(out, image_magic, 4);
    at = 4;
    out[at++] = (unsigned char)count;
    for (unsigned f = 0; f < count; ++f) {
        out[at++] = text[f];
        out[at++] = (unsigned char)strlen(names[f]);
        memcpy(out + at, names[f], strlen(names[f]));
        at += strlen(names[f]);
    }
    return (size_t)(put_le(out + at, seq, 8) - out);
}

/* A list record's fields as header_of writes them, a list field's with the number of its items' record after its name,
 * and, once any field is a list's (027's stage 3), the graph's other records after them: a u8 count, each record a u8
 * count of fields and its fields so (PureLayout). */
static unsigned char *layout_fields(const PureLayout *l, unsigned at, unsigned char *out) {
    for (unsigned f = 0; f < l->record[at].count; ++f) {
        size_t length = strlen(l->record[at].names[f]);
        *out++ = l->record[at].kinds[f];
        *out++ = (unsigned char)length;
        memcpy(out, l->record[at].names[f], length);
        out += length;
        if (l->record[at].kinds[f] == PURE_CELL_LIST) *out++ = l->record[at].items[f];
    }
    return out;
}

/* 0 without room for the record's graph. */
static size_t image_header(const PureModule *m, uint32_t record, uint64_t seq, unsigned char *out) {
    PureLayout l;
    pure_layout_begin(&l);
    if (!pure_layout_of(m, record, &l)) {
        pure_layout_done(&l);
        return 0;
    }
    memcpy(out, image_magic, 4);
    out[4] = (unsigned char)l.record[0].count;
    unsigned char *at = layout_fields(&l, 0, out + 5);
    bool lists = m->record_nested[record];
    if (lists) *at++ = (unsigned char)(l.records - 1);
    for (unsigned k = 1; lists && k < l.records; ++k) {
        *at++ = (unsigned char)l.record[k].count;
        at = layout_fields(&l, k, at);
    }
    pure_layout_done(&l);
    return (size_t)(put_le(at, seq, 8) - out);
}

/* Past a record's count fields as layout_fields wrote them (from at; read_fields checked them): where they end. */
static size_t skip_fields(const unsigned char *bytes, size_t size, size_t at, unsigned count) {
    for (unsigned f = 0; f < count && at + 2 <= size; ++f) {
        uint8_t kind = bytes[at];
        at += 2 + bytes[at + 1] + (kind == PURE_CELL_LIST);
    }
    return at <= size ? at : 0;
}

/* A record's fields as layout_fields wrote them (at, moved past them), into l's record k: false when not well formed
 * (no field twice, kinds a record's, a list's items' record among the graph's: below records). */
static bool read_fields(const unsigned char *bytes, size_t size, size_t *at, unsigned count, PureLayout *l, unsigned k,
                        unsigned records, bool *lists) {
    l->record[k].count = count;
    for (unsigned f = 0; f < count; ++f) {
        if (!named(bytes, size, at, 0, &l->record[k].kinds[f], l->record[k].names[f])) return false;
        uint8_t kind = l->record[k].kinds[f];
        if (kind > PURE_CELL_LIST) return false;
        for (unsigned g = 0; g < f; ++g) if (!strcmp(l->record[k].names[g], l->record[k].names[f])) return false;
        l->record[k].items[f] = 0;
        if (kind != PURE_CELL_LIST) continue;
        if (*at >= size || bytes[*at] >= records) return false;
        l->record[k].items[f] = bytes[(*at)++];
        *lists = true;
    }
    return true;
}

PureList *pure_runtime_list_new(uint32_t place) {
    const RuntimeList *r = &runtime_lists[place - PURE_LIST_HEARD];
    return pure_list_new(r->fields, r->kinds);
}

static size_t runtime_header(uint32_t place, uint64_t seq, unsigned char *out) {
    const RuntimeList *r = &runtime_lists[place - PURE_LIST_HEARD];
    return header_of(r->fields, r->kinds, r->names, seq, out);
}

/* The outbox's image header: m's outputs named after the save it holds. */
static size_t outbox_header(const PureModule *m, uint64_t seq, unsigned char *out) {
    size_t at = runtime_header(PURE_LIST_OUTBOX, seq, out);
    memcpy(out, sends_magic, 4);
    unsigned char *count = out + at++;
    *count = 0;
    for (uint32_t i = 0; i < m->inputs; ++i) {
        if (m->input[i].kind != PURE_KIND_OUTPUT) continue;
        ++*count;
        size_t length = strlen(m->input_names[i]);
        out[at++] = (unsigned char)length;
        memcpy(out + at, m->input_names[i], length);
        at += length;
        out[at++] = (unsigned char)m->input[i].field_count;
        for (uint32_t f = 0; f < m->input[i].field_count; ++f) {
            const PureField *field = &m->field[m->input[i].field_start + f];
            size_t named = strlen(field->name);
            out[at++] = field->type;
            out[at++] = (unsigned char)named;
            memcpy(out + at, field->name, named);
            at += named;
        }
    }
    return at;
}

/* What an output's sends were laid out with, as a store names it: its
 * fields (laid out), or their types alone (named false: links before PLK4). */
typedef struct { char name[PURE_MODULE_NAME]; PureField fields[PURE_MESSAGE_FIELDS]; uint32_t count; bool named; } SentShape;

/* The outputs an outbox image names (at, size bytes): how many, or -1 when
 * they are not well formed; shapes (when not NULL) takes them. */
static int read_outputs(const unsigned char *at, size_t size, SentShape *shapes, size_t *used) {
    size_t k = 0;
    if (size < 1) return -1;
    unsigned outputs = at[k++];
    for (unsigned t = 0; t < outputs; ++t) {
        size_t length = k < size ? at[k] : 0;
        if (!length || length >= PURE_MODULE_NAME || size - k < 2 + length) return -1;
        unsigned fields = at[k + 1 + length];
        if (fields > PURE_MESSAGE_FIELDS) return -1;
        if (shapes) {
            memcpy(shapes[t].name, at + k + 1, length);
            shapes[t].name[length] = 0;
            shapes[t].count = fields;
            shapes[t].named = true;
        }
        k += 2 + length;
        for (unsigned f = 0; f < fields; ++f) {
            size_t named = size - k >= 2 ? at[k + 1] : 0;
            if (size - k < 2 || !named || named >= PURE_MODULE_NAME || size - k < 2 + named || !at[k] ||
                at[k] == PURE_FIELD_ROWS || at[k] > PURE_FIELD_I64) /* an output sends no rows */
                return -1;
            if (shapes) {
                shapes[t].fields[f] = (PureField){.type = at[k]};
                memcpy(shapes[t].fields[f].name, at + k + 2, named);
            }
            k += 2 + named;
        }
        if (shapes) pure_fields_lay_out(shapes[t].fields, fields);
    }
    *used = k;
    return (int)outputs;
}

/* A stored list by its place: a list state, or one of the runtime's. */
static PureList *stored_list(const PureApp *app, uint32_t i) {
    if (i == PURE_LIST_HEARD) return app->heard;
    if (i == PURE_LIST_OUTBOX) return app->outbox;
    return i < app->module->states && app->module->state_type[i] == PURE_STATE_LIST &&
                   !pure_state_transient(app->module, i) ? app->lists[i] : NULL;
}

static const char *stored_name(const PureApp *app, uint32_t i) {
    return i >= PURE_LIST_HEARD ? runtime_lists[i - PURE_LIST_HEARD].name : app->module->state_names[i];
}

static const char *directory_of(const PureApp *app) {
    return app->data.directory ? app->data.directory(app->data.context) : NULL;
}

/* A list's changes as the log's entries had them at the start: the save, and
 * the records, in the list's image's fields (valid while the log is open). */
typedef struct { uint64_t seq; PureSpan records; } Logged;
struct PureStartLog {
    struct Named { char name[PURE_MODULE_NAME]; Logged *changes; size_t count, capacity; } *lists;
    size_t count, capacity;
    uint64_t tree, last; /* the tree's last save; the last entry that counts */
    bool stopped, damaged;
    SentShape *sent;     /* the outputs the outbox's image names (PLO1), sent_count of them */
    int sent_count;      /* -1: its image names none */
};

static struct Named *named_list(struct PureStartLog *index, const char *name, size_t length, bool add) {
    for (size_t i = 0; i < index->count; ++i)
        if (strlen(index->lists[i].name) == length && !memcmp(index->lists[i].name, name, length)) return &index->lists[i];
    if (!add) return NULL;
    if (index->count == index->capacity) {
        size_t capacity = index->capacity ? 2 * index->capacity : 8;
        struct Named *grown = realloc(index->lists, capacity * sizeof(*grown));
        if (!grown) return NULL;
        index->lists = grown;
        index->capacity = capacity;
    }
    struct Named *n = &index->lists[index->count++];
    *n = (struct Named){{0}, NULL, 0, 0};
    memcpy(n->name, name, length);
    return n;
}

/* Each entry, while its save counts (a save that committed the tree counts
 * when the tree holds its number): its lists' records, by list. */
static bool index_entry(void *context, uint64_t seq, bool tree, PureSpan payload) {
    struct PureStartLog *index = context;
    if (tree && seq > index->tree) {
        index->stopped = true;
        return false;
    }
    for (size_t at = 0; at < payload.size;) {
        size_t length = payload.data[at++];
        if (!length || length >= PURE_MODULE_NAME || payload.size - at < length + 4) { index->damaged = true; return false; }
        const char *name = (const char *)payload.data + at;
        size_t records = (size_t)get_le(payload.data + at + length, 4);
        at += length + 4;
        if (records > payload.size - at) { index->damaged = true; return false; }
        struct Named *list = named_list(index, name, length, true);
        if (!list) { index->damaged = true; return false; }
        if (list->count == list->capacity) {
            size_t capacity = list->capacity ? 2 * list->capacity : 16;
            Logged *grown = realloc(list->changes, capacity * sizeof(*grown));
            if (!grown) { index->damaged = true; return false; }
            list->changes = grown;
            list->capacity = capacity;
        }
        list->changes[list->count++] = (Logged){seq, {payload.data + at, records}};
        at += records;
    }
    index->last = seq;
    return true;
}

void pure_save_close_start(PureApp *app) {
    struct PureStartLog *index = app->start_log;
    if (!index) return;
    for (size_t i = 0; i < index->count; ++i) free(index->lists[i].changes);
    free(index->lists);
    free(index->sent);
    free(index);
    app->start_log = NULL;
}

PURE_COLD PureResult pure_save_open_log(PureApp *app) {
    const char *directory = directory_of(app);
    if (!directory) return PURE_OK;
    PureResult result = pure_log_open(directory, &app->log);
    if (result == PURE_OK) result = open_state(app, "", "seq", &app->stored_seq);
    struct PureStartLog *index = result == PURE_OK ? calloc(1, sizeof(*index)) : NULL;
    if (result == PURE_OK && !index) result = PURE_ALLOCATION;
    if (index) index->sent_count = -1;
    if (result == PURE_OK && pure_draft_revision(app->stored_seq)) {
        unsigned char *bytes;
        size_t size;
        result = read_whole(app->stored_seq, &bytes, &size);
        if (result == PURE_OK) {
            if (size == 8) index->tree = get_le(bytes, 8);
            else result = PURE_STORE_ERROR;
            free(bytes);
        }
    }
    if (result == PURE_OK) result = pure_log_read(app->log, 0, index_entry, index);
    if (result == PURE_OK && index->damaged) result = PURE_STORE_ERROR;
    if (result != PURE_OK) {
        app->start_log = index;
        pure_save_close_start(app);
        return result;
    }
    /* The entries after the last that counts are no longer the log's. */
    if (index->stopped) pure_log_end(app->log, index->last);
    app->seq = index->tree > index->last ? index->tree : index->last;
    app->start_log = index;
    return PURE_OK;
}

/* The log keeps the entries some image does not hold yet. */
void pure_save_release(PureApp *app) {
    if (!app->log) return;
    uint64_t kept = app->seq + 1;
    for (size_t i = 0; i < pure_slots(app); ++i)
        if (app->logged_since[i] && app->logged_since[i] < kept) kept = app->logged_since[i];
    pure_log_release(app->log, kept - 1);
}

/* An image's bytes as the store writes them: its header, then its items a
 * chunk at a time (one item at least, whatever its size). */
typedef struct {
    const PureList *list;
    size_t at, capacity;
    const unsigned char *header;
    size_t header_size;
    bool started;
    unsigned char *buffer;
} Imaging;

static bool image_chunk(void *context, PureSpan *chunk) {
    Imaging *i = context;
    if (!i->started) {
        i->started = true;
        *chunk = (PureSpan){i->header, i->header_size};
        return true;
    }
    size_t needed, length = pure_list_encode_part(i->list, &i->at, i->buffer, i->capacity, &needed);
    if (!length && needed) {
        unsigned char *grown = realloc(i->buffer, needed);
        if (!grown) return false;
        i->buffer = grown;
        i->capacity = needed;
        length = pure_list_encode_part(i->list, &i->at, i->buffer, i->capacity, &needed);
    }
    *chunk = (PureSpan){i->buffer, length};
    return true;
}

static PureResult write_image(const char *directory, const char *name, const unsigned char *header, size_t header_size,
                              const PureList *list) {
    Imaging imaging = {list, 0, 256 << 10, header, header_size, false, malloc(256 << 10)};
    PureResult result = imaging.buffer ? pure_image_write(directory, name, image_chunk, &imaging) : PURE_ALLOCATION;
    free(imaging.buffer);
    return result;
}

PureResult pure_save_image(PureApp *app, const PureModule *m, uint32_t state, const PureList *list) {
    unsigned char *header = malloc(header_room(m));
    if (!header) return PURE_ALLOCATION;
    size_t header_size = image_header(m, m->state_ref[state], app->seq, header);
    PureResult result = header_size ? write_image(directory_of(app), m->state_names[state], header, header_size, list)
                                    : PURE_ALLOCATION;
    free(header);
    return result;
}

PURE_COLD PureResult pure_save_open_list(PureApp *app, const PureModule *m, uint32_t state, PureList **out,
                                         PureDraft **legacy, PureSaved *saved) {
    uint32_t record = m->state_ref[state];
    const PureRecord *r = &m->record[record];
    const char *name = m->state_names[state];
    *out = NULL;
    *legacy = NULL;
    *saved = (PureSaved){0};
    if (!directory_of(app)) return PURE_STORE_ERROR;
    /* Its image, else what the tree kept before images. */
    unsigned char *bytes = NULL;
    size_t size = 0;
    PureResult result = pure_image_read(directory_of(app), name, &bytes, &size);
    bool image = result == PURE_OK;
    if (result == PURE_NOT_FOUND) {
        result = open_state(app, "list-", name, legacy);
        if (result == PURE_OK && pure_draft_revision(*legacy) && pure_draft_size(*legacy))
            result = read_whole(*legacy, &bytes, &size);
    }
    /* The stored record graph: its fields' kinds (PURE_CELL_...: a record's, its lists' PURE_CELL_LIST) and names, and
     * the records of the lists in its fields (an image's alone, 027's stage 3). */
    PureLayout stored_layout, layout;
    pure_layout_begin(&stored_layout), pure_layout_begin(&layout);
    stored_layout.records = 1;
    unsigned count = 0;
    size_t at = 0, items = 0;
    uint64_t holds = 0;
    bool stored = result == PURE_OK && bytes, first = stored && !image && size >= 4 && !memcmp(bytes, list_magic_1, 4);
    if (stored) {
        const unsigned char *magic = image ? image_magic : first ? list_magic_1 : list_magic;
        bool ok = size >= 5 && !memcmp(bytes, magic, 4) && bytes[4] <= PURE_RECORD_FIELDS, lists = false;
        count = ok ? bytes[4] : 0;
        at = 5;
        ok = ok && read_fields(bytes, size, &at, count, &stored_layout, 0, 256, &lists) && (image || !lists);
        if (ok && lists) { /* the graph's other records */
            ok = at < size && pure_layout_room(&stored_layout, 1u + bytes[at]);
            stored_layout.records = ok ? 1u + bytes[at++] : 1;
            for (unsigned k = 1; ok && k < stored_layout.records; ++k)
                ok = at < size && bytes[at] <= PURE_RECORD_FIELDS &&
                     read_fields(bytes, size, &(size_t){at + 1}, bytes[at], &stored_layout, k, stored_layout.records, &lists) &&
                     (at = skip_fields(bytes, size, at + 1, bytes[at])) != 0;
            for (unsigned f = 0; ok && f < count; ++f) ok = stored_layout.record[0].items[f] < stored_layout.records;
        }
        if (ok && image) { /* the save it holds, then its items */
            ok = size - at >= 8;
            holds = ok ? get_le(bytes + at, 8) : 0;
            at += 8;
            items = size - at;
        } else if (ok && !first) {
            ok = size - at >= 8 && (items = (size_t)get_le(bytes + at, 8)) <= size - at - 8;
            at += 8;
        } else items = size - at;
        if (!ok) result = PURE_STORE_ERROR;
    }
    PureList *from = NULL;
    if (result == PURE_OK && stored) {
        uint8_t held[PURE_RECORD_FIELDS]; /* a list field's value held as bytes */
        for (unsigned f = 0; f < count; ++f)
            held[f] = stored_layout.record[0].kinds[f] == PURE_CELL_LIST ? PURE_CELL_BYTES : stored_layout.record[0].kinds[f];
        from = pure_list_new(count, held);
        result = from ? pure_list_read(from, bytes + at, items) : PURE_ALLOCATION;
        if (result == PURE_OK && !image) result = pure_list_replay(from, bytes + at + items, size - at - items);
    }
    free(bytes);
    /* Then the changes logged after the save it holds: only an image's
     * list has any (one is written before any change of a list is logged). */
    struct Named *logged = app->start_log ? named_list(app->start_log, name, strlen(name), false) : NULL;
    for (size_t k = 0; result == PURE_OK && logged && k < logged->count; ++k) {
        const Logged *change = &logged->changes[k];
        if (change->seq <= holds) continue;
        if (!image) result = PURE_STORE_ERROR;
        else if ((result = pure_list_replay(from, change->records.data, change->records.size)) == PURE_OK) {
            if (!saved->since) saved->since = change->seq;
            saved->logged += change->records.size;
        }
    }
    if (result == PURE_OK && !pure_layout_of(m, record, &layout)) result = PURE_ALLOCATION;
    bool same = stored && result == PURE_OK && pure_layout_same(&stored_layout, &layout);
    if (result == PURE_OK && same && image) {
        pure_layout_done(&stored_layout), pure_layout_done(&layout);
        *out = from;
        saved->image = holds;
        return PURE_OK;
    }
    if (result == PURE_OK) {
        /* New, stored before images, or stored with another record (its items
         * move by field name): its image is written before anything else of it. */
        if (same) {
            *out = from;
            from = NULL;
        } else if (from) result = pure_list_migrate(out, from, &stored_layout, m, record, true);
        else {
            uint8_t own[PURE_RECORD_FIELDS];
            for (unsigned f = 0; f < r->count; ++f)
                own[f] = pure_record_kind(m, r->first + f) == PURE_CELL_LIST ? PURE_CELL_BYTES : pure_record_kind(m, r->first + f);
            if (!(*out = pure_list_new(r->count, own))) result = PURE_ALLOCATION;
        }
    }
    pure_list_free(from);
    pure_layout_done(&stored_layout), pure_layout_done(&layout);
    if (result != PURE_OK) {
        pure_list_free(*out);
        pure_draft_close(*legacy);
        *out = NULL;
        *legacy = NULL;
        return result;
    }
    *saved = (PureSaved){.rewrite = true};
    /* What the tree kept goes: the image holds it now. */
    if (*legacy && pure_draft_size(*legacy)) result = pure_draft_splice(*legacy, 0, pure_draft_size(*legacy), (PureSpan){0});
    else {
        pure_draft_close(*legacy);
        *legacy = NULL;
    }
    return result;
}

PURE_COLD PureResult pure_save_open_runtime(PureApp *app, uint32_t place, PureList **out, PureSaved *saved) {
    const char *name = runtime_lists[place - PURE_LIST_HEARD].name;
    *saved = (PureSaved){0};
    *out = NULL;
    if (!directory_of(app)) return PURE_STORE_ERROR;
    unsigned char *bytes = NULL, header[RUNTIME_HEADER];
    size_t size = 0, length = runtime_header(place, 0, header) - 8, at = length + 8;
    PureResult result = pure_image_read(directory_of(app), name, &bytes, &size);
    bool image = result == PURE_OK;
    bool named = image && place == PURE_LIST_OUTBOX && size >= 4 && !memcmp(bytes, sends_magic, 4);
    bool formed = image && size >= at && (named || !memcmp(bytes, image_magic, 4)) &&
                  !memcmp(bytes + 4, header + 4, length - 4);
    uint64_t holds = formed ? get_le(bytes + length, 8) : 0;
    if (formed && named) { /* the outputs its sends were laid out with */
        size_t used = 0;
        int outputs = read_outputs(bytes + at, size - at, NULL, &used);
        SentShape *shapes = outputs > 0 ? calloc((size_t)outputs, sizeof(*shapes)) : NULL;
        if (outputs < 0 || (outputs > 0 && !shapes)) formed = false;
        else if (app->start_log) {
            read_outputs(bytes + at, size - at, shapes, &used);
            free(app->start_log->sent);
            app->start_log->sent = shapes, app->start_log->sent_count = outputs;
            shapes = NULL;
        }
        free(shapes);
        at += used;
    }
    if (image && !formed) result = PURE_STORE_ERROR;
    else if (image || result == PURE_NOT_FOUND) {
        result = (*out = pure_runtime_list_new(place)) ? PURE_OK : PURE_ALLOCATION;
        if (result == PURE_OK && image) result = pure_list_read(*out, bytes + at, size - at);
    }
    free(bytes);
    /* Then the changes logged after the save it holds (an image is written
     * before any change of the list is logged). */
    struct Named *logged = app->start_log ? named_list(app->start_log, name, strlen(name), false) : NULL;
    for (size_t k = 0; result == PURE_OK && logged && k < logged->count; ++k) {
        const Logged *change = &logged->changes[k];
        if (change->seq <= holds) continue;
        if (!image) result = PURE_STORE_ERROR;
        else if ((result = pure_list_replay(*out, change->records.data, change->records.size)) == PURE_OK) {
            if (!saved->since) saved->since = change->seq;
            saved->logged += change->records.size;
        }
    }
    if (result != PURE_OK) {
        pure_list_free(*out);
        *out = NULL;
        return result;
    }
    if (image) saved->image = holds;
    else saved->rewrite = true;
    return PURE_OK;
}

PureResult pure_save_runtime_image(PureApp *app, const PureModule *m, uint32_t place, const PureList *list) {
    unsigned char *header = malloc(header_room(m));
    if (!header) return PURE_ALLOCATION;
    size_t header_size = place == PURE_LIST_OUTBOX ? outbox_header(m, app->seq, header) :
                                                     runtime_header(place, app->seq, header);
    PureResult result = write_image(directory_of(app), runtime_lists[place - PURE_LIST_HEARD].name, header, header_size,
                                    list ? list : stored_list(app, place));
    free(header);
    return result;
}

/* ---- Checkpoints */

struct PureCheckpoint {
    PureSnapshot *snapshot;
    uint32_t state;
    char name[PURE_MODULE_NAME], *directory;
    unsigned char *header; /* header_room's */
    size_t header_size, logged;
    uint64_t seq, since;
};

void pure_app_background_checkpoints(PureApp *app, bool background) { app->background_checkpoints = background; }

PureCheckpoint *pure_app_checkpoint(PureApp *app) {
    const PureModule *m = app->module;
    if (!app->log || app->checkpoint || app->open || app->uncertain || app->reading) return NULL;
    /* A list whose logged changes outgrew its items, or, when the log grew
     * past LOG_KEPT, the one that holds it back most: the heard list is one. */
    int due = -1, oldest = -1; /* slots */
    uint64_t items = 0;
    for (size_t i = 0; i < pure_slots(app) && due < 0; ++i) {
        const PureList *list = stored_list(app, pure_place(app, i));
        size_t size = list ? pure_list_size(list) : 0;
        items += size;
        if (!list || !app->logged_since[i]) continue;
        if (app->logged[i] > (size > JOURNAL_MIN ? size : JOURNAL_MIN)) due = (int)i;
        else if (oldest < 0 || app->logged_since[i] < app->logged_since[oldest]) oldest = (int)i;
    }
    int slot = due >= 0 ? due : pure_log_bytes(app->log) > (items > LOG_KEPT ? items : LOG_KEPT) ? oldest : -1;
    if (slot < 0) return NULL;
    uint32_t pick = pure_place(app, (size_t)slot);
    PureCheckpoint *c = calloc(1, sizeof(*c));
    const char *directory = directory_of(app);
    size_t length = strlen(directory) + 1;
    if (c && (c->directory = malloc(length))) memcpy(c->directory, directory, length);
    if (c) c->header = malloc(header_room(m));
    if (!c || !c->directory || !c->header ||
        !(c->snapshot = pure_save_snapshot(app, pick >= PURE_LIST_HEARD ? pick : UINT32_MAX))) {
        if (c) free(c->directory), free(c->header);
        free(c);
        return NULL;
    }
    c->state = pick;
    snprintf(c->name, sizeof(c->name), "%s", stored_name(app, pick));
    c->seq = app->seq;
    c->header_size = pick == PURE_LIST_OUTBOX ? outbox_header(m, c->seq, c->header) :
                     pick >= PURE_LIST_HEARD ? runtime_header(pick, c->seq, c->header) :
                                               image_header(m, m->state_ref[pick], c->seq, c->header);
    if (!c->header_size) { /* no room for its record's graph: none now */
        pure_snapshot_release(c->snapshot);
        free(c->directory), free(c->header), free(c);
        return NULL;
    }
    c->logged = app->logged[slot];
    c->since = app->logged_since[slot];
    app->checkpoint = c;
    return c;
}

PureResult pure_checkpoint_write(PureCheckpoint *c) {
    return write_image(c->directory, c->name, c->header, c->header_size, pure_snapshot_list(c->snapshot, c->state));
}

void pure_app_checkpointed(PureApp *app, PureCheckpoint *c, PureResult result) {
    if (!c) return;
    size_t i = pure_slot(app, c->state);
    if (result == PURE_OK && app->checkpoint == c) {
        /* The image holds every change logged up to its save: those logged
         * since stay (counted from the save after it). */
        app->image_seq[i] = c->seq;
        app->logged[i] -= c->logged;
        app->logged_since[i] = app->logged[i] ? c->seq + 1 : 0;
        pure_save_release(app);
#ifdef PURE_STORE_TEST
        ++pure_save_test_checkpoints;
#endif
    }
    if (app->checkpoint == c) app->checkpoint = NULL;
    pure_snapshot_release(c->snapshot);
    free(c->directory), free(c->header);
    free(c);
}

static bool scalar(uint8_t type) {
    return type == PURE_STATE_NUM || type == PURE_STATE_BOOL || type == PURE_STATE_I64 || type == PURE_STATE_CURSOR ||
           type == PURE_STATE_INDEX;
}

/* A stored position still fits its state: a cursor on a character boundary
 * of its text, an index on an item of its list (or 0). */
PURE_COLD static bool fits(const PureModule *m, PureDraft *const *texts, PureList *const *lists, uint32_t state, double value) {
    if (!isfinite(value) || value < 0 || value != floor(value)) return false;
    if (m->state_type[state] == PURE_STATE_INDEX) {
        size_t count = pure_list_count(lists[m->state_ref[state]]);
        return value <= (count ? (double)(count - 1) : 0);
    }
    const PureDraft *text = texts[m->state_ref[state]];
    size_t size = pure_draft_size(text), at = (size_t)value;
    if (value > (double)size) return false;
    if (at == size) return true;
    unsigned char byte = 0;
    Whole w = {&byte, 0};
    return pure_draft_read(text, at, 1, gather, &w) == PURE_OK && (byte & 0xC0) != 0x80;
}

static size_t encode_values(const PureModule *m, const PureValue *values, unsigned char *out) {
    size_t at = 4;
    memcpy(out, values_magic, 4);
    for (uint32_t i = 0; i < m->states; ++i) {
        if (!scalar(m->state_type[i]) || pure_state_transient(m, i)) continue;
        size_t length = strlen(m->state_names[i]);
        uint64_t bits;
        memcpy(&bits, &values[i], sizeof(bits));
        out[at++] = m->state_type[i];
        out[at++] = (unsigned char)length;
        memcpy(out + at, m->state_names[i], length);
        at += length;
        for (unsigned b = 0; b < 8; ++b) out[at++] = (unsigned char)(bits >> (8 * b));
    }
    return at;
}

/* The values' bytes at most for states of them (and as many for a few on the stack). */
static size_t values_bytes(uint32_t states) { return 4 + (size_t)states * (2 + PURE_MODULE_NAME + 8); }
enum { FEW_STATES = 64, FEW_VALUES_BYTES = 4 + FEW_STATES * (2 + PURE_MODULE_NAME + 8) };

PURE_COLD PureResult pure_save_open_values(PureApp *app, const PureModule *m, PureValue *values, const int *kept,
                                 PureDraft *const *texts, PureList *const *lists) {
    bool scalars = false, fresh = false;
    for (uint32_t i = 0; i < m->states; ++i)
        if (scalar(m->state_type[i]) && !pure_state_transient(m, i)) {
            scalars = true;
            fresh = fresh || kept[i] < 0;
        }
    if (!scalars) return PURE_OK;
    PureResult result = app->stored_values ? PURE_OK : open_state(app, "", "values", &app->stored_values);
    if (result != PURE_OK || !fresh || !pure_draft_revision(app->stored_values)) return result;
    unsigned char *bytes;
    size_t size;
    if ((result = read_whole(app->stored_values, &bytes, &size)) != PURE_OK) return result;
    bool ok = size >= 4 && !memcmp(bytes, values_magic, 4);
    for (size_t at = 4; ok && at < size;) {
        char name[PURE_MODULE_NAME];
        uint8_t type;
        if (!(ok = named(bytes, size, &at, 8, &type, name))) break;
        uint64_t bits = 0;
        for (unsigned b = 0; b < 8; ++b) bits |= (uint64_t)bytes[at + b] << (8 * b);
        at += 8;
        int state = pure_module_state(m, name);
        if (state < 0 || kept[state] >= 0 || !scalar(type) || pure_state_transient(m, (uint32_t)state)) continue;
        /* A number, a bool or an i64, each as the state's type holds it when it fits (pure_cell_as); a position where it fits. */
        uint8_t from = pure_value_kind(type), to = pure_value_kind(m->state_type[state]);
        PureCell cell = {.i64 = (int64_t)bits};
        if (from != 0xFF && to != 0xFF ? pure_cell_fits(from, cell) && pure_cell_as(from, to, &cell)
                                       : m->state_type[state] == type && fits(m, texts, lists, (uint32_t)state, cell.num))
            values[state].i64 = cell.i64;
    }
    free(bytes);
    return ok ? PURE_OK : PURE_STORE_ERROR;
}

static bool same_version(PureVersion a, PureVersion b) { return a.ref == b.ref && a.bytes == b.bytes; }

/* The links as stored; NULL when memory runs out. Each output: its name,
 * its fields (each its type and name: what its sends' bytes mean, so a
 * module that orders or names them otherwise is refused while sends wait),
 * and the last number it sent; then the outputs carried from modules
 * before, with none. */
static unsigned char *encode_links(const PureApp *app, size_t *size) {
    const PureModule *m = app->module;
    unsigned outputs = 0;
    size_t total = 4 + 1;
    for (uint32_t i = 0; i < m->inputs; ++i)
        if (m->input[i].kind == PURE_KIND_OUTPUT) {
            ++outputs;
            total += 2 + strlen(m->input_names[i]) + 8;
            for (uint32_t f = 0; f < m->input[i].field_count; ++f)
                total += 2 + strlen(m->field[m->input[i].field_start + f].name);
        }
    for (unsigned c = 0; c < app->carried_count; ++c) total += 2 + strlen(app->carried[c].name) + 8;
    unsigned char *bytes = malloc(total), *at = bytes;
    if (!bytes) return NULL;
    memcpy(at, links_magic, 4);
    at += 4;
    *at++ = (unsigned char)(outputs + app->carried_count);
    for (uint32_t i = 0; i < m->inputs; ++i) {
        if (m->input[i].kind != PURE_KIND_OUTPUT) continue;
        size_t length = strlen(m->input_names[i]);
        *at++ = (unsigned char)length;
        memcpy(at, m->input_names[i], length);
        at += length;
        *at++ = (unsigned char)m->input[i].field_count;
        for (uint32_t f = 0; f < m->input[i].field_count; ++f) {
            const PureField *field = &m->field[m->input[i].field_start + f];
            size_t named = strlen(field->name);
            *at++ = field->type;
            *at++ = (unsigned char)named;
            memcpy(at, field->name, named);
            at += named;
        }
        at = put_le(at, app->last_sent[i], 8);
    }
    for (unsigned c = 0; c < app->carried_count; ++c) {
        size_t length = strlen(app->carried[c].name);
        *at++ = (unsigned char)length;
        memcpy(at, app->carried[c].name, length);
        at += length;
        *at++ = 0;
        at = put_le(at, app->carried[c].last, 8);
    }
    *size = (size_t)(at - bytes);
    return bytes;
}

/* A number heard, as links kept it before the heard list. */
PURE_COLD static PureResult heard_before(PureApp *app, const unsigned char *source, size_t length, uint64_t seq) {
    PureCell key = {.text = (unsigned char *)source, .size = length};
    bool found;
    size_t at = pure_list_seek(app->heard, 0, key, &found);
    if (!found) return pure_list_insert(app->heard, at, (PureCell[2]){key, {.num = (double)seq}}, NULL);
    if (pure_list_get(app->heard, at, 1).num >= (double)seq) return PURE_OK;
    return pure_list_set(app->heard, at, 1, (PureCell){.num = (double)seq}, NULL);
}

/* A send, as links kept it before the outbox list: at the outbox's end. */
PURE_COLD static PureResult sent_before(PureApp *app, const char *output, uint64_t seq, const unsigned char *payload,
                                        size_t size) {
    PureCell item[3] = {{.text = (unsigned char *)output, .size = strlen(output)}, {.num = (double)seq},
                        {.text = (unsigned char *)payload, .size = size}};
    return pure_list_insert(app->outbox, pure_list_count(app->outbox), item, NULL);
}

/* The stored links of a starting app: outputs by name (an output whose
 * fields changed or that the module lacks refuses the start while it has
 * sends left in the outbox: CONFLICT; one without sends keeps its number
 * for later); the sources and sends links kept before the runtime's lists
 * go to them (heard_moved, sent_moved: they need images). Links kept
 * before PLK4 have their fields' types alone. */
/* The shape of the output of that name among shapes, or NULL. */
static const SentShape *sent_shape(const SentShape *shapes, unsigned count, const char *name) {
    for (unsigned t = 0; t < count; ++t)
        if (!strcmp(shapes[t].name, name)) return &shapes[t];
    return NULL;
}

/* Whether sends laid out as was are laid out as m's output i: the same
 * fields, names and types in order (types alone when was names none). */
static bool same_shape(const SentShape *was, const PureModule *m, uint32_t i) {
    if (was->count != m->input[i].field_count) return false;
    for (uint32_t f = 0; f < was->count; ++f) {
        const PureField *field = &m->field[m->input[i].field_start + f];
        if (field->type != was->fields[f].type || (was->named && strcmp(field->name, was->fields[f].name))) return false;
    }
    return true;
}

PURE_COLD PureResult pure_save_open_links(PureApp *app, const PureModule *m, bool *heard_moved, bool *sent_moved) {
    *heard_moved = *sent_moved = false;
    PureResult result = app->stored_links ? PURE_OK : open_state(app, "", "links", &app->stored_links);
    if (result != PURE_OK || !pure_draft_revision(app->stored_links)) return result;
    unsigned char *bytes;
    size_t size;
    if ((result = read_whole(app->stored_links, &bytes, &size)) != PURE_OK) return result;
    int output[256];
    bool fits[256];
    char names[256][PURE_MODULE_NAME];
    size_t at = 5;
    bool first = size >= 5 && !memcmp(bytes, links_magic_1, 4), second = size >= 5 && !memcmp(bytes, links_magic_2, 4),
         named = size >= 5 && !memcmp(bytes, links_magic, 4),
         known = first || second || named || (size >= 5 && !memcmp(bytes, links_magic_3, 4));
    unsigned outputs = known ? bytes[4] : 0;
    result = known ? PURE_OK : PURE_STORE_ERROR;
    SentShape *linked = calloc(outputs ? outputs : 1, sizeof(*linked)); /* what each was sent with, as the links say */
    if (!linked) result = PURE_ALLOCATION;
    for (unsigned t = 0; t < outputs && result == PURE_OK; ++t) {
        char *name = names[t];
        size_t length = at < size ? bytes[at] : 0;
        if (!length || length >= PURE_MODULE_NAME || size - at < 2 + length) { result = PURE_STORE_ERROR; break; }
        memcpy(name, bytes + at + 1, length);
        name[length] = 0;
        unsigned fields = bytes[at + 1 + length];
        at += 2 + length;
        int i = pure_module_input(m, name);
        output[t] = i >= 0 && m->input[i].kind == PURE_KIND_OUTPUT ? i : -1;
        fits[t] = output[t] >= 0 && m->input[i].field_count == fields;
        memcpy(linked[t].name, name, length + 1);
        linked[t].count = fields;
        linked[t].named = named;
        if (fields > PURE_MESSAGE_FIELDS) result = PURE_STORE_ERROR;
        for (unsigned f = 0; f < fields && result == PURE_OK; ++f) { /* its type, then (PLK4) its name */
            size_t named_length = named && size - at >= 2 ? bytes[at + 1] : 0;
            if (size - at < (named ? 2 + named_length : 1) || named_length >= PURE_MODULE_NAME) {
                result = PURE_STORE_ERROR;
                break;
            }
            const PureField *field = fits[t] ? &m->field[m->input[i].field_start + f] : NULL;
            fits[t] = field && field->type == bytes[at] &&
                      (!named || (strlen(field->name) == named_length && !memcmp(field->name, bytes + at + 2, named_length)));
            linked[t].fields[f] = (PureField){.type = bytes[at]};
            if (named) memcpy(linked[t].fields[f].name, bytes + at + 2, named_length);
            at += named ? 2 + named_length : 1;
        }
        if (result == PURE_OK) pure_fields_lay_out(linked[t].fields, fields);
        if (result != PURE_OK || size - at < 8) { result = PURE_STORE_ERROR; break; }
        uint64_t last = get_le(bytes + at, 8);
        at += 8;
        if (output[t] >= 0) app->last_sent[output[t]] = last;
        else if (last) {
            struct PureCarried *grown = realloc(app->carried, (app->carried_count + 1) * sizeof(*grown));
            if (!grown) { result = PURE_ALLOCATION; break; }
            app->carried = grown;
            memcpy(grown[app->carried_count].name, name, length + 1);
            grown[app->carried_count++].last = last;
        }
    }
    unsigned sources = result == PURE_OK && first && size - at >= 2 ? (unsigned)get_le(bytes + at, 2) : 0;
    if (result == PURE_OK && first && size - at < 2) result = PURE_STORE_ERROR;
    at += first ? 2 : 0;
    for (unsigned h = 0; h < sources && result == PURE_OK; ++h) {
        size_t length = at < size ? bytes[at] : 0;
        if (!length || length > PURE_SOURCE || size - at < 1 + length + 8) { result = PURE_STORE_ERROR; break; }
        uint64_t seq = get_le(bytes + at + 1 + length, 8);
        if (seq > PURE_SEQ_MOST) { result = PURE_STORE_ERROR; break; }
        result = heard_before(app, bytes + at + 1, length, seq);
        at += 1 + length + 8;
        *heard_moved = true;
        app->links_changed = true; /* stored again without them */
    }
    while (result == PURE_OK && (first || second) && at < size) { /* sends kept before the outbox list */
        if (size - at < 13 || bytes[at] >= outputs) { result = PURE_STORE_ERROR; break; }
        unsigned t = bytes[at];
        uint64_t seq = get_le(bytes + at + 1, 8);
        size_t length = (size_t)get_le(bytes + at + 9, 4);
        at += 13;
        if (size - at < length || seq > PURE_SEQ_MOST) { result = PURE_STORE_ERROR; break; }
        result = sent_before(app, names[t], seq, bytes + at, length);
        at += length;
        *sent_moved = true;
        app->links_changed = true;
    }
    if (result == PURE_OK && !first && !second && at != size) result = PURE_STORE_ERROR;
    /* Every send left goes out on an output of this module, laid out with
     * its fields: one laid out otherwise (as the outbox's image names them,
     * else as the links do) is recast by name. */
    bool imaged = app->start_log && app->start_log->sent_count >= 0;
    const SentShape *shapes = imaged ? app->start_log->sent : linked;
    unsigned shape_count = imaged ? (unsigned)app->start_log->sent_count : outputs;
    for (size_t k = 0, count = pure_list_count(app->outbox); result == PURE_OK && k < count; ++k) {
        PureCell cell = pure_list_get(app->outbox, k, 0);
        char name[PURE_MODULE_NAME] = "";
        if (cell.size < PURE_MODULE_NAME) memcpy(name, cell.text, cell.size);
        int i = pure_module_input(m, name);
        const SentShape *was = sent_shape(shapes, shape_count, name);
        if (i < 0 || m->input[i].kind != PURE_KIND_OUTPUT) {
            snprintf(app->why, sizeof(app->why), "sends wait on the output '%s', which this module does not have", name);
            result = PURE_CONFLICT;
        } else if (!was) result = PURE_STORE_ERROR; /* sent on an output its store does not name */
        else if (!same_shape(was, m, (uint32_t)i)) {
            PureCell payload = pure_list_get(app->outbox, k, 2);
            unsigned char *made = NULL;
            size_t made_size = 0;
            if (!was->named) {
                snprintf(app->why, sizeof(app->why),
                         "sends wait on '%s', laid out with fields its store names by type alone: start the old module once",
                         name);
                result = PURE_CONFLICT;
            } else if ((result = pure_send_recast(was->fields, was->count, &m->field[m->input[i].field_start],
                                                  m->input[i].field_count, (PureSpan){payload.text, payload.size},
                                                  &made, &made_size)) == PURE_OK) {
                result = pure_list_set(app->outbox, k, 2, (PureCell){.text = made, .size = made_size}, NULL);
                *sent_moved = true;
            }
            free(made);
        }
    }
    /* Sends from now on are laid out with this module's fields: an image
     * naming others is written again, naming these. */
    for (uint32_t i = 0; result == PURE_OK && i < m->inputs && !*sent_moved; ++i) {
        if (m->input[i].kind != PURE_KIND_OUTPUT) continue;
        const SentShape *was = sent_shape(shapes, shape_count, m->input_names[i]);
        *sent_moved = !was || !same_shape(was, m, i);
    }
    free(linked);
    free(bytes);
    app->sent_saved = pure_list_count(app->outbox);
    memcpy(app->saved_sent, app->last_sent, m->inputs * sizeof(*app->saved_sent));
    return result;
}

/* The lists' changes since the last save, as one log entry: per list changed,
 * its name and its records. NULL with size 0 when no list changed. */
static unsigned char *list_changes(const PureApp *app, size_t *size) {
    *size = 0;
    for (size_t s = 0; s < pure_slots(app); ++s) {
        uint32_t i = pure_place(app, s);
        const PureList *list = stored_list(app, i);
        if (list && pure_list_changes(list).size) *size += 1 + strlen(stored_name(app, i)) + 4 + pure_list_changes(list).size;
    }
    unsigned char *payload = *size ? malloc(*size) : NULL, *at = payload;
    for (size_t s = 0; payload && s < pure_slots(app); ++s) {
        uint32_t i = pure_place(app, s);
        const PureList *list = stored_list(app, i);
        PureSpan changes = list ? pure_list_changes(list) : (PureSpan){0};
        if (!changes.size) continue;
        size_t length = strlen(stored_name(app, i));
        *at++ = (unsigned char)length;
        memcpy(at, stored_name(app, i), length);
        at = put_le(at + length, changes.size, 4);
        memcpy(at, changes.data, changes.size);
        at += changes.size;
    }
    return payload;
}

PureResult pure_save_commit(PureApp *app, const PureValue *values) {
    if (!pure_save_enabled(app)) return PURE_OK; /* the states live in memory alone */
    /* A save that may or may not have been stored: until the app starts again
     * from what the store has, no save follows it. */
    if (app->uncertain) return PURE_CONFLICT;
    const PureModule *m = app->module;
    /* A draft a state at most, and three more: on the stack for a few states (027's 4f-2). */
    PureDraft *few_drafts[FEW_STATES + 3], **drafts = few_drafts;
    uint32_t few_states[FEW_STATES + 3], *states = few_states;
    if (m->states > FEW_STATES && (!(drafts = malloc((m->states + 3) * sizeof(*drafts))) |
                                   !(states = malloc((m->states + 3) * sizeof(*states))))) {
        if (drafts != few_drafts) free(drafts);
        if (states != few_states) free(states);
        return PURE_ALLOCATION;
    }
    size_t count = 0;
    for (uint32_t i = 0; i < m->states; ++i) {
        PureDraft *draft = pure_state_transient(m, i) ? NULL :
                           m->state_type[i] == PURE_STATE_TEXT ? app->texts[i] :
                           m->state_type[i] == PURE_STATE_LIST ? app->list_drafts[i] : NULL;
        if (draft && !same_version(pure_draft_version(draft), app->saved[i])) {
            states[count] = i;
            drafts[count++] = draft;
        }
    }
    /* Numbers, cursors and indices: stored again only when their bytes change. */
    PureVersion values_before = {0, 0}, links_before = {0, 0}, seq_before = {0, 0};
    bool values_changed = false, links_written = false, seq_written = false;
    PureResult result = PURE_OK;
    unsigned char few_encoded[FEW_VALUES_BYTES], *encoded = few_encoded;
    if (app->stored_values && m->states > FEW_STATES && !(encoded = malloc(values_bytes(m->states)))) result = PURE_ALLOCATION;
    if (app->stored_values && result == PURE_OK) {
        unsigned char *stored;
        size_t length = encode_values(m, values, encoded), size = 0;
        result = read_whole(app->stored_values, &stored, &size);
        if (result == PURE_OK) {
            values_changed = size != length || memcmp(stored, encoded, length);
            free(stored);
        }
        if (values_changed) {
            values_before = pure_draft_version(app->stored_values);
            pure_draft_hold(app->stored_values, true);
            result = pure_draft_splice(app->stored_values, 0, size, (PureSpan){encoded, length});
            states[count] = UINT32_MAX;
            drafts[count++] = app->stored_values;
        }
    }
    /* The links, written whole when they changed. */
    if (result == PURE_OK && app->links_changed) {
        result = app->stored_links ? PURE_OK : open_state(app, "", "links", &app->stored_links);
        size_t length;
        unsigned char *encoded = result == PURE_OK ? encode_links(app, &length) : NULL;
        if (result == PURE_OK && !encoded) result = PURE_ALLOCATION;
        if (result == PURE_OK) {
            links_before = pure_draft_version(app->stored_links);
            pure_draft_hold(app->stored_links, true);
            links_written = true;
            result = pure_draft_splice(app->stored_links, 0, pure_draft_size(app->stored_links), (PureSpan){encoded, length});
            states[count] = UINT32_MAX;
            drafts[count++] = app->stored_links;
        }
        free(encoded);
    }
    /* The lists' changes go to the log, one entry a save. A save that also
     * commits the tree stores its number there: its entry counts only with it. */
    size_t payload_size = 0;
    unsigned char *payload = result == PURE_OK ? list_changes(app, &payload_size) : NULL;
    if (payload_size && !payload) result = PURE_ALLOCATION;
    bool tree = count;
    uint64_t seq = app->seq + 1;
    if (result == PURE_OK && tree && payload_size && app->log) {
        result = app->stored_seq ? PURE_OK : open_state(app, "", "seq", &app->stored_seq);
        if (result == PURE_OK) {
            unsigned char number[8];
            put_le(number, seq, 8);
            seq_before = pure_draft_version(app->stored_seq);
            pure_draft_hold(app->stored_seq, true);
            seq_written = true;
            result = pure_draft_splice(app->stored_seq, 0, pure_draft_size(app->stored_seq), (PureSpan){number, 8});
            states[count] = UINT32_MAX;
            drafts[count++] = app->stored_seq;
        }
    }
    if (result == PURE_OK && payload_size) {
        result = app->log ? pure_log_append(app->log, seq, tree, (PureSpan){payload, payload_size}) : PURE_STORE_ERROR;
        if (result == PURE_UNCERTAIN) app->uncertain = true;
    }
    free(payload);
    if (result == PURE_OK && tree) {
        result = app->data.save(app->data.context, drafts, count);
        /* A commit that may yet be stored leaves the app apart from its store,
         * whatever the save changed: no save follows until it starts again.
         * One that failed takes back the logged entry, which is on the device. */
        if (result == PURE_UNCERTAIN) app->uncertain = true;
        else if (result != PURE_OK && payload_size) pure_log_retract(app->log);
    }
    if (result == PURE_OK) {
        for (size_t k = 0; k < count; ++k)
            if (states[k] != UINT32_MAX) app->saved[states[k]] = pure_draft_version(drafts[k]);
        for (size_t i = 0; i < pure_slots(app); ++i) {
            PureList *list = stored_list(app, pure_place(app, i));
            PureSpan changes = list ? pure_list_changes(list) : (PureSpan){0};
            if (!changes.size) continue;
            app->logged[i] += changes.size;
            if (!app->logged_since[i]) app->logged_since[i] = seq;
            pure_list_forget(list, 0);
        }
        if (tree || payload_size) app->seq = seq;
    }
    if (values_changed) {
        if (result != PURE_OK) pure_draft_restore(app->stored_values, values_before);
        pure_draft_hold(app->stored_values, false);
    }
    if (links_written) {
        if (result != PURE_OK) pure_draft_restore(app->stored_links, links_before);
        pure_draft_hold(app->stored_links, false);
    }
    if (seq_written) {
        if (result != PURE_OK) pure_draft_restore(app->stored_seq, seq_before);
        pure_draft_hold(app->stored_seq, false);
    }
    if (encoded != few_encoded) free(encoded);
    if (drafts != few_drafts) free(drafts), free(states);
    return result;
}
