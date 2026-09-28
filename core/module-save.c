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
 * with such an entry that also commits the tree (numbers, texts, links,
 * documents) stores its number there as the state "seq": the entry counts
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
/* links: "PLK2"; u8 outputs, each {u8 name length, name, u8 field count, the
 * fields' types, u64le last number} (the module's outputs, then those it
 * dropped, without fields); then the sent messages to the end, each {u8
 * output (in the table), u64le number, u32le size, payload}. Before the
 * heard list, "PLK1" kept the numbers heard between the two: u16le sources,
 * each {u8 length, the name, u64le last number heard}; a start moves them to
 * the heard list.
 *
 * The heard list (module.h, PureApp) is stored as a list state is, by a name
 * no state has (a state's is an identifier), with the fields source (text)
 * and seq (number): the numbers of every source, which only grow with the
 * sources a node hears from, cost a save only the records of their changes. */
static const unsigned char links_magic[4] = {'P', 'L', 'K', '2'}, links_magic_1[4] = {'P', 'L', 'K', '1'};
static const char heard_name[] = "0heard", heard_names[2][PURE_MODULE_NAME] = {"source", "seq"};
static const uint8_t heard_text[2] = {1, 0};
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

enum { IMAGE_HEADER = 5 + PURE_RECORD_FIELDS * (2 + PURE_MODULE_NAME) + 8 };

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

static size_t image_header(const PureModule *m, uint32_t record, uint64_t seq, unsigned char *out) {
    const PureRecord *r = &m->record[record];
    return header_of(r->count, m->record_field_text + r->first, m->record_field_names + r->first, seq, out);
}

PureList *pure_heard_new(void) { return pure_list_new(2, heard_text); }

/* A stored list by its place: a list state, or the heard list (PURE_LIST_HEARD). */
static PureList *stored_list(const PureApp *app, uint32_t i) {
    if (i == PURE_LIST_HEARD) return app->heard;
    return i < app->module->states && app->module->state_type[i] == PURE_STATE_LIST ? app->lists[i] : NULL;
}

static const char *stored_name(const PureApp *app, uint32_t i) {
    return i == PURE_LIST_HEARD ? heard_name : app->module->state_names[i];
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
    for (uint32_t i = 0; i <= PURE_LIST_HEARD; ++i)
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
    unsigned char header[IMAGE_HEADER];
    size_t header_size = image_header(m, m->state_ref[state], app->seq, header);
    return write_image(directory_of(app), m->state_names[state], header, header_size, list);
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
    /* The stored record: its fields' kinds and names. */
    char names[PURE_RECORD_FIELDS][PURE_MODULE_NAME] = {{0}};
    uint8_t text[PURE_RECORD_FIELDS] = {0};
    unsigned count = 0;
    size_t at = 0, items = 0;
    uint64_t holds = 0;
    bool stored = result == PURE_OK && bytes, first = stored && !image && size >= 4 && !memcmp(bytes, list_magic_1, 4);
    if (stored) {
        const unsigned char *magic = image ? image_magic : first ? list_magic_1 : list_magic;
        bool ok = size >= 5 && !memcmp(bytes, magic, 4) && bytes[4] <= PURE_RECORD_FIELDS;
        count = ok ? bytes[4] : 0;
        at = 5;
        for (unsigned f = 0; ok && f < count; ++f) {
            ok = named(bytes, size, &at, 0, &text[f], names[f]) && text[f] <= 1;
            for (unsigned k = 0; k < f; ++k) ok = ok && strcmp(names[k], names[f]);
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
        from = pure_list_new(count, text);
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
    bool same = stored && count == r->count;
    for (unsigned f = 0; same && f < count; ++f)
        same = text[f] == m->record_field_text[r->first + f] && !strcmp(names[f], m->record_field_names[r->first + f]);
    if (result == PURE_OK && same && image) {
        *out = from;
        saved->image = holds;
        return PURE_OK;
    }
    if (result == PURE_OK) {
        /* New, stored before images, or stored with another record (its items
         * move by field name): its image is written before anything else of it. */
        const char *field_names[PURE_RECORD_FIELDS];
        for (unsigned f = 0; f < count; ++f) field_names[f] = names[f];
        if (same) {
            *out = from;
            from = NULL;
        } else if (from) result = pure_list_migrate(out, from, field_names, text, count, m, record);
        else if (!(*out = pure_list_new(r->count, m->record_field_text + r->first))) result = PURE_ALLOCATION;
    }
    pure_list_free(from);
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

PURE_COLD PureResult pure_save_open_heard(PureApp *app, PureList **out, PureSaved *saved) {
    *saved = (PureSaved){0};
    *out = NULL;
    if (!directory_of(app)) return PURE_STORE_ERROR;
    unsigned char *bytes = NULL, header[IMAGE_HEADER];
    size_t size = 0, length = header_of(2, heard_text, heard_names, 0, header) - 8;
    PureResult result = pure_image_read(directory_of(app), heard_name, &bytes, &size);
    bool image = result == PURE_OK;
    uint64_t holds = image && size >= length + 8 && !memcmp(bytes, header, length) ? get_le(bytes + length, 8) : 0;
    if (image && (size < length + 8 || memcmp(bytes, header, length))) result = PURE_STORE_ERROR;
    else if (image || result == PURE_NOT_FOUND) {
        result = (*out = pure_heard_new()) ? PURE_OK : PURE_ALLOCATION;
        if (result == PURE_OK && image) result = pure_list_read(*out, bytes + length + 8, size - length - 8);
    }
    free(bytes);
    /* Then the changes logged after the save it holds (an image is written
     * before any change of the list is logged). */
    struct Named *logged = app->start_log ? named_list(app->start_log, heard_name, strlen(heard_name), false) : NULL;
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

PureResult pure_save_heard_image(PureApp *app) {
    unsigned char header[IMAGE_HEADER];
    size_t header_size = header_of(2, heard_text, heard_names, app->seq, header);
    return write_image(directory_of(app), heard_name, header, header_size, app->heard);
}

/* ---- Checkpoints */

struct PureCheckpoint {
    PureSnapshot *snapshot;
    uint32_t state;
    char name[PURE_MODULE_NAME], *directory;
    unsigned char header[IMAGE_HEADER];
    size_t header_size, logged;
    uint64_t seq, since;
};

void pure_app_background_checkpoints(PureApp *app, bool background) { app->background_checkpoints = background; }

PureCheckpoint *pure_app_checkpoint(PureApp *app) {
    const PureModule *m = app->module;
    if (!app->log || app->checkpoint || app->open || app->uncertain || app->reading) return NULL;
    /* A list whose logged changes outgrew its items, or, when the log grew
     * past LOG_KEPT, the one that holds it back most: the heard list is one. */
    int due = -1, oldest = -1;
    uint64_t items = 0;
    for (uint32_t i = 0; i <= PURE_LIST_HEARD && due < 0; ++i) {
        const PureList *list = stored_list(app, i);
        size_t size = list ? pure_list_size(list) : 0;
        items += size;
        if (!list || !app->logged_since[i]) continue;
        if (app->logged[i] > (size > JOURNAL_MIN ? size : JOURNAL_MIN)) due = (int)i;
        else if (oldest < 0 || app->logged_since[i] < app->logged_since[oldest]) oldest = (int)i;
    }
    int pick = due >= 0 ? due : pure_log_bytes(app->log) > (items > LOG_KEPT ? items : LOG_KEPT) ? oldest : -1;
    if (pick < 0) return NULL;
    PureCheckpoint *c = calloc(1, sizeof(*c));
    const char *directory = directory_of(app);
    size_t length = strlen(directory) + 1;
    if (c && (c->directory = malloc(length))) memcpy(c->directory, directory, length);
    if (!c || !c->directory || !(c->snapshot = pure_save_snapshot(app, pick == PURE_LIST_HEARD))) {
        if (c) free(c->directory);
        free(c);
        return NULL;
    }
    c->state = (uint32_t)pick;
    snprintf(c->name, sizeof(c->name), "%s", stored_name(app, (uint32_t)pick));
    c->seq = app->seq;
    c->header_size = pick == PURE_LIST_HEARD ? header_of(2, heard_text, heard_names, c->seq, c->header) :
                                               image_header(m, m->state_ref[pick], c->seq, c->header);
    c->logged = app->logged[pick];
    c->since = app->logged_since[pick];
    app->checkpoint = c;
    return c;
}

PureResult pure_checkpoint_write(PureCheckpoint *c) {
    return write_image(c->directory, c->name, c->header, c->header_size, pure_snapshot_list(c->snapshot, c->state));
}

void pure_app_checkpointed(PureApp *app, PureCheckpoint *c, PureResult result) {
    if (!c) return;
    uint32_t i = c->state;
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
    free(c->directory);
    free(c);
}

static bool scalar(uint8_t type) {
    return type == PURE_STATE_NUM || type == PURE_STATE_CURSOR || type == PURE_STATE_INDEX;
}

/* A stored value still fits its state: a cursor on a character boundary of
 * its text, an index on an item of its list (or 0), a finite number. */
PURE_COLD static bool fits(const PureModule *m, PureDraft *const *texts, PureList *const *lists, uint32_t state, double value) {
    if (!isfinite(value)) return false;
    if (m->state_type[state] == PURE_STATE_NUM) return true;
    if (value < 0 || value != floor(value)) return false;
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

static size_t encode_values(const PureModule *m, const double *values, unsigned char *out) {
    size_t at = 4;
    memcpy(out, values_magic, 4);
    for (uint32_t i = 0; i < m->states; ++i) {
        if (!scalar(m->state_type[i])) continue;
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

enum { VALUES_BYTES = 4 + PURE_MODULE_STATES * (2 + PURE_MODULE_NAME + 8) };

PURE_COLD PureResult pure_save_open_values(PureApp *app, const PureModule *m, double *values, const int *kept,
                                 PureDraft *const *texts, PureList *const *lists) {
    bool scalars = false, fresh = false;
    for (uint32_t i = 0; i < m->states; ++i)
        if (scalar(m->state_type[i])) {
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
        double value;
        memcpy(&value, &bits, sizeof(value));
        int state = pure_module_state(m, name);
        if (state >= 0 && kept[state] < 0 && m->state_type[state] == type && scalar(type) &&
            fits(m, texts, lists, (uint32_t)state, value))
            values[state] = value;
    }
    free(bytes);
    return ok ? PURE_OK : PURE_STORE_ERROR;
}

static bool same_version(PureVersion a, PureVersion b) { return a.ref == b.ref && a.bytes == b.bytes; }

/* The links as stored; NULL when memory runs out. */
static unsigned char *encode_links(const PureApp *app, size_t *size) {
    const PureModule *m = app->module;
    unsigned outputs = 0;
    uint8_t table[PURE_MODULE_INPUTS];
    size_t total = 4 + 1;
    for (uint32_t i = 0; i < m->inputs; ++i)
        if (m->input[i].kind == PURE_KIND_OUTPUT) {
            table[i] = (uint8_t)outputs++;
            total += 2 + strlen(m->input_names[i]) + m->input[i].field_count + 8;
        }
    for (unsigned c = 0; c < app->carried_count; ++c) total += 2 + strlen(app->carried[c].name) + 8;
    for (size_t s = 0; s < app->sent_count; ++s)
        if (!app->sent[s].trimmed) total += 1 + 8 + 4 + app->sent[s].size;
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
        for (uint32_t f = 0; f < m->input[i].field_count; ++f) *at++ = m->field[m->input[i].field_start + f].type;
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
    for (size_t s = 0; s < app->sent_count; ++s) {
        const struct PureSent *sent = &app->sent[s];
        if (sent->trimmed) continue;
        *at++ = table[sent->output];
        at = put_le(at, sent->seq, 8);
        at = put_le(at, sent->size, 4);
        if (sent->size) memcpy(at, sent->payload, sent->size);
        at += sent->size;
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

/* The stored links of a starting app: outputs by name (an output whose
 * fields changed or that the module lacks refuses the start while it has
 * sends left: CONFLICT; one without sends keeps its number for later),
 * sources kept before the heard list, and sent messages. */
PURE_COLD PureResult pure_save_open_links(PureApp *app, const PureModule *m, bool *moved) {
    *moved = false;
    PureResult result = app->stored_links ? PURE_OK : open_state(app, "", "links", &app->stored_links);
    if (result != PURE_OK || !pure_draft_revision(app->stored_links)) return result;
    unsigned char *bytes;
    size_t size;
    if ((result = read_whole(app->stored_links, &bytes, &size)) != PURE_OK) return result;
    int output[256];
    bool fits[256];
    size_t at = 5;
    bool first = size >= 5 && !memcmp(bytes, links_magic_1, 4), known = first || (size >= 5 && !memcmp(bytes, links_magic, 4));
    unsigned outputs = known ? bytes[4] : 0;
    result = known ? PURE_OK : PURE_STORE_ERROR;
    for (unsigned t = 0; t < outputs && result == PURE_OK; ++t) {
        char name[PURE_MODULE_NAME];
        size_t length = at < size ? bytes[at] : 0;
        if (!length || length >= PURE_MODULE_NAME || size - at < 2 + length) { result = PURE_STORE_ERROR; break; }
        memcpy(name, bytes + at + 1, length);
        name[length] = 0;
        unsigned fields = bytes[at + 1 + length];
        at += 2 + length;
        if (size - at < fields + 8) { result = PURE_STORE_ERROR; break; }
        int i = pure_module_input(m, name);
        output[t] = i >= 0 && m->input[i].kind == PURE_KIND_OUTPUT ? i : -1;
        fits[t] = output[t] >= 0 && m->input[i].field_count == fields;
        for (unsigned f = 0; fits[t] && f < fields; ++f) fits[t] = m->field[m->input[i].field_start + f].type == bytes[at + f];
        uint64_t last = get_le(bytes + at + fields, 8);
        at += fields + 8;
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
        *moved = true;
        app->links_changed = true; /* stored again without them */
    }
    while (result == PURE_OK && at < size) {
        if (size - at < 13 || bytes[at] >= outputs) { result = PURE_STORE_ERROR; break; }
        unsigned t = bytes[at];
        uint64_t seq = get_le(bytes + at + 1, 8);
        size_t length = (size_t)get_le(bytes + at + 9, 4);
        at += 13;
        if (size - at < length) { result = PURE_STORE_ERROR; break; }
        if (!fits[t]) { result = PURE_CONFLICT; break; }
        if (app->sent_count == app->sent_capacity) {
            size_t capacity = app->sent_capacity ? 2 * app->sent_capacity : 16;
            struct PureSent *grown = realloc(app->sent, capacity * sizeof(*grown));
            if (!grown) { result = PURE_ALLOCATION; break; }
            app->sent = grown;
            app->sent_capacity = capacity;
        }
        unsigned char *payload = malloc(length ? length : 1);
        if (!payload) { result = PURE_ALLOCATION; break; }
        if (length) memcpy(payload, bytes + at, length);
        app->sent[app->sent_count++] = (struct PureSent){(uint32_t)output[t], seq, payload, length, false};
        at += length;
    }
    free(bytes);
    app->sent_durable = app->sent_count;
    memcpy(app->saved_sent, app->last_sent, sizeof(app->saved_sent));
    return result;
}

/* The lists' changes since the last save, as one log entry: per list changed,
 * its name and its records. NULL with size 0 when no list changed. */
static unsigned char *list_changes(const PureApp *app, size_t *size) {
    *size = 0;
    for (uint32_t i = 0; i <= PURE_LIST_HEARD; ++i) {
        const PureList *list = stored_list(app, i);
        if (list && pure_list_changes(list).size) *size += 1 + strlen(stored_name(app, i)) + 4 + pure_list_changes(list).size;
    }
    unsigned char *payload = *size ? malloc(*size) : NULL, *at = payload;
    for (uint32_t i = 0; payload && i <= PURE_LIST_HEARD; ++i) {
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

PureResult pure_save_commit(PureApp *app, const double *values, void *const *documents, size_t document_count,
                            int64_t *revisions) {
    if (!pure_save_enabled(app)) {
        /* Without a host that saves together, a save has one document at most. */
        PureResult result = PURE_OK;
        for (size_t i = 0; i < document_count && result == PURE_OK; ++i)
            result = app->data.commit(app->data.context, documents[i], &revisions[i]);
        return result;
    }
    /* A save that may or may not have been stored: until the app starts again
     * from what the store has, no save follows it. */
    if (app->uncertain) return PURE_CONFLICT;
    const PureModule *m = app->module;
    PureDraft *drafts[2 * PURE_MODULE_STATES + 3];
    uint32_t states[2 * PURE_MODULE_STATES + 3];
    size_t count = 0;
    for (uint32_t i = 0; i < m->states; ++i) {
        PureDraft *draft = m->state_type[i] == PURE_STATE_TEXT ? app->texts[i] :
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
    if (app->stored_values) {
        unsigned char encoded[VALUES_BYTES], *stored;
        size_t length = encode_values(m, values, encoded), size;
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
    bool tree = count || document_count;
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
        result = app->data.save(app->data.context, documents, document_count, drafts, count, revisions);
        /* The logged entry is on the device: taken back when the tree's commit
         * failed, or, when that may yet be stored, no save follows. */
        if (result != PURE_OK && payload_size) {
            if (result == PURE_UNCERTAIN) app->uncertain = true;
            else pure_log_retract(app->log);
        }
    }
    if (result == PURE_OK) {
        for (size_t k = 0; k < count; ++k)
            if (states[k] != UINT32_MAX) app->saved[states[k]] = pure_draft_version(drafts[k]);
        for (uint32_t i = 0; i <= PURE_LIST_HEARD; ++i) {
            PureList *list = stored_list(app, i);
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
    return result;
}
