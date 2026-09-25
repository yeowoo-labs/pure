#include "module-list.h"
#include "module-text.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

struct PureList {
    unsigned fields;
    uint8_t text_fields[16];
    PureCell **items;
    size_t count, capacity;
    PureDraft *draft; /* the stored copy, or NULL */
    size_t header;    /* draft bytes before the first item */
};

typedef enum { INSERTED, REMOVED, CHANGED } Change;
typedef struct {
    Change change;
    PureList *list;
    size_t at;
    unsigned field;
    PureCell *item; /* REMOVED: the removed item */
    PureCell old;   /* CHANGED: the replaced cell */
} Entry;

static void free_item(const PureList *list, PureCell *item) {
    if (!item) return;
    for (unsigned f = 0; f < list->fields; ++f)
        if (list->text_fields[f]) free(item[f].text);
    free(item);
}

PureList *pure_list_new(unsigned fields, const uint8_t *text_fields) {
    if (fields > 16) return NULL;
    PureList *list = calloc(1, sizeof(*list));
    if (!list) return NULL;
    list->fields = fields;
    memcpy(list->text_fields, text_fields, fields);
    return list;
}

void pure_list_free(PureList *list) {
    if (!list) return;
    for (size_t i = 0; i < list->count; ++i) free_item(list, list->items[i]);
    free(list->items);
    free(list);
}

size_t pure_list_count(const PureList *list) { return list->count; }

static bool log_change(PureUndo *undo, Entry entry) {
    if (undo->count == undo->capacity) {
        size_t capacity = undo->capacity ? 2 * undo->capacity : 16;
        Entry *entries = realloc(undo->entries, capacity * sizeof(*entries));
        if (!entries) return false;
        undo->entries = entries;
        undo->capacity = capacity;
    }
    ((Entry *)undo->entries)[undo->count++] = entry;
    return true;
}

static bool copy_cell(const PureList *list, unsigned field, PureCell from, PureCell *to) {
    *to = (PureCell){.num = list->text_fields[field] ? 0 : from.num};
    if (!list->text_fields[field] || !from.size) return true;
    to->text = malloc(from.size);
    if (!to->text) return false;
    memcpy(to->text, from.text, from.size);
    to->size = from.size;
    return true;
}

/* ---- The stored form: each field in order, a number as its binary64 bits
 * (little-endian), a text as a u32le length and its bytes. */

static size_t cell_bytes(const PureList *list, unsigned field, PureCell cell) {
    return list->text_fields[field] ? 4 + cell.size : 8;
}

static size_t item_bytes(const PureList *list, const PureCell *item) {
    size_t size = 0;
    for (unsigned f = 0; f < list->fields; ++f) size += cell_bytes(list, f, item[f]);
    return size;
}

/* Where an item starts in the draft. Linear in the items before it. */
static size_t item_offset(const PureList *list, size_t at) {
    size_t offset = list->header;
    for (size_t i = 0; i < at; ++i) offset += item_bytes(list, list->items[i]);
    return offset;
}

static unsigned char *encode_cell(const PureList *list, unsigned field, PureCell cell, unsigned char *out) {
    uint64_t bits = cell.size;
    unsigned width = 4;
    if (!list->text_fields[field]) { memcpy(&bits, &cell.num, sizeof(bits)); width = 8; }
    for (unsigned i = 0; i < width; ++i) *out++ = (unsigned char)(bits >> (8 * i));
    if (list->text_fields[field] && cell.size) { memcpy(out, cell.text, cell.size); out += cell.size; }
    return out;
}

/* Replace [offset, offset + remove) of the draft; nothing to do without one. */
static PureResult mirror(PureList *list, size_t offset, size_t remove, const unsigned char *bytes, size_t size) {
    if (!list->draft) return PURE_OK;
    return pure_draft_splice(list->draft, offset, remove, (PureSpan){bytes, size});
}

void pure_list_mirror(PureList *list, PureDraft *draft, size_t header) {
    list->draft = draft;
    list->header = header;
}

PureResult pure_list_write(PureList *list) {
    size_t size = 0;
    for (size_t i = 0; i < list->count; ++i) size += item_bytes(list, list->items[i]);
    unsigned char *bytes = size ? malloc(size) : NULL, *at = bytes;
    if (size && !bytes) return PURE_ALLOCATION;
    for (size_t i = 0; i < list->count; ++i)
        for (unsigned f = 0; f < list->fields; ++f) at = encode_cell(list, f, list->items[i][f], at);
    PureResult result = mirror(list, list->header, 0, bytes, size);
    free(bytes);
    return result;
}

PureResult pure_list_read(PureList *list, const unsigned char *bytes, size_t size) {
    PureCell cells[16];
    for (size_t at = 0; at < size;) {
        for (unsigned f = 0; f < list->fields; ++f) {
            size_t width = list->text_fields[f] ? 4 : 8;
            if (size - at < width) return PURE_STORE_ERROR;
            uint64_t bits = 0;
            for (size_t i = 0; i < width; ++i) bits |= (uint64_t)bytes[at + i] << (8 * i);
            at += width;
            if (!list->text_fields[f]) {
                cells[f] = (PureCell){0};
                memcpy(&cells[f].num, &bits, sizeof(bits));
                if (!isfinite(cells[f].num)) return PURE_STORE_ERROR;
                continue;
            }
            if (bits > size - at || !pure_utf8_valid((PureSpan){bytes + at, (size_t)bits}))
                return PURE_STORE_ERROR;
            cells[f] = (PureCell){0, (unsigned char *)bytes + at, (size_t)bits};
            at += (size_t)bits;
        }
        PureResult result = pure_list_insert(list, list->count, cells, NULL);
        if (result != PURE_OK) return result;
    }
    return PURE_OK;
}

static void place(PureList *list, size_t at, PureCell *item) {
    memmove(list->items + at + 1, list->items + at, (list->count - at) * sizeof(*list->items));
    list->items[at] = item;
    ++list->count;
}

static PureCell *take(PureList *list, size_t at) {
    PureCell *item = list->items[at];
    memmove(list->items + at, list->items + at + 1, (list->count - at - 1) * sizeof(*list->items));
    --list->count;
    return item;
}

PureResult pure_list_insert(PureList *list, size_t at, const PureCell *cells, PureUndo *undo) {
    if (at > list->count || list->count == PURE_LIST_ITEMS) return PURE_LIMIT;
    if (list->count == list->capacity) {
        size_t capacity = list->capacity ? 2 * list->capacity : 8;
        PureCell **items = realloc(list->items, capacity * sizeof(*items));
        if (!items) return PURE_ALLOCATION;
        list->items = items;
        list->capacity = capacity;
    }
    PureCell *item = calloc(list->fields ? list->fields : 1, sizeof(*item));
    if (!item) return PURE_ALLOCATION;
    for (unsigned f = 0; f < list->fields; ++f)
        if (!copy_cell(list, f, cells[f], &item[f])) { free_item(list, item); return PURE_ALLOCATION; }
    size_t size = list->draft ? item_bytes(list, item) : 0;
    unsigned char *bytes = size ? malloc(size) : NULL, *end = bytes;
    if (size && !bytes) { free_item(list, item); return PURE_ALLOCATION; }
    for (unsigned f = 0; bytes && f < list->fields; ++f) end = encode_cell(list, f, item[f], end);
    if (undo && !log_change(undo, (Entry){.change = INSERTED, .list = list, .at = at})) {
        free(bytes);
        free_item(list, item);
        return PURE_ALLOCATION;
    }
    PureResult result = mirror(list, item_offset(list, at), 0, bytes, size);
    free(bytes);
    if (result != PURE_OK) {
        if (undo) --undo->count;
        free_item(list, item);
        return result;
    }
    place(list, at, item);
    return PURE_OK;
}

PureResult pure_list_remove(PureList *list, size_t at, PureUndo *undo) {
    if (at >= list->count) return PURE_NOT_FOUND;
    if (!log_change(undo, (Entry){.change = REMOVED, .list = list, .at = at, .item = list->items[at]}))
        return PURE_ALLOCATION;
    PureResult result = mirror(list, item_offset(list, at), item_bytes(list, list->items[at]), NULL, 0);
    if (result != PURE_OK) { --undo->count; return result; }
    take(list, at);
    return PURE_OK;
}

PureResult pure_list_set(PureList *list, size_t at, unsigned field, PureCell value, PureUndo *undo) {
    if (at >= list->count) return PURE_NOT_FOUND;
    PureCell copy, *item = list->items[at];
    if (!copy_cell(list, field, value, &copy)) return PURE_ALLOCATION;
    size_t size = list->draft ? cell_bytes(list, field, copy) : 0;
    unsigned char *bytes = size ? malloc(size) : NULL;
    if (size && !bytes) { free(copy.text); return PURE_ALLOCATION; }
    if (bytes) encode_cell(list, field, copy, bytes);
    if (!log_change(undo, (Entry){.change = CHANGED, .list = list, .at = at, .field = field, .old = item[field]})) {
        free(bytes);
        free(copy.text);
        return PURE_ALLOCATION;
    }
    size_t offset = list->draft ? item_offset(list, at) : 0;
    for (unsigned f = 0; list->draft && f < field; ++f) offset += cell_bytes(list, f, item[f]);
    PureResult result = mirror(list, offset, cell_bytes(list, field, item[field]), bytes, size);
    free(bytes);
    if (result != PURE_OK) {
        --undo->count;
        free(copy.text);
        return result;
    }
    item[field] = copy;
    return PURE_OK;
}

PureCell pure_list_get(const PureList *list, size_t at, unsigned field) {
    return list->items[at][field];
}

void pure_undo_rollback(PureUndo *undo, size_t mark) {
    Entry *entries = undo->entries;
    for (size_t i = undo->count; i-- > mark;) {
        Entry *e = &entries[i];
        if (e->change == INSERTED) free_item(e->list, take(e->list, e->at));
        else if (e->change == REMOVED) place(e->list, e->at, e->item); /* capacity is still there */
        else {
            if (e->list->text_fields[e->field]) free(e->list->items[e->at][e->field].text);
            e->list->items[e->at][e->field] = e->old;
        }
    }
    undo->count = mark;
}

void pure_undo_commit(PureUndo *undo) {
    Entry *entries = undo->entries;
    for (size_t i = 0; i < undo->count; ++i) {
        Entry *e = &entries[i];
        if (e->change == REMOVED) free_item(e->list, e->item);
        else if (e->change == CHANGED && e->list->text_fields[e->field]) free(e->old.text);
    }
    undo->count = 0;
}

void pure_undo_free(PureUndo *undo) {
    free(undo->entries);
    *undo = (PureUndo){0};
}
