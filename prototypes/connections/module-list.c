#include "module-list.h"
#include <stdlib.h>
#include <string.h>

struct PureList {
    unsigned fields;
    uint8_t text_fields[16];
    PureCell **items;
    size_t count, capacity;
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
    if (undo && !log_change(undo, (Entry){.change = INSERTED, .list = list, .at = at})) {
        free_item(list, item);
        return PURE_ALLOCATION;
    }
    place(list, at, item);
    return PURE_OK;
}

PureResult pure_list_remove(PureList *list, size_t at, PureUndo *undo) {
    if (at >= list->count) return PURE_NOT_FOUND;
    if (!log_change(undo, (Entry){.change = REMOVED, .list = list, .at = at, .item = list->items[at]}))
        return PURE_ALLOCATION;
    take(list, at);
    return PURE_OK;
}

PureResult pure_list_set(PureList *list, size_t at, unsigned field, PureCell value, PureUndo *undo) {
    if (at >= list->count) return PURE_NOT_FOUND;
    PureCell copy;
    if (!copy_cell(list, field, value, &copy)) return PURE_ALLOCATION;
    if (!log_change(undo, (Entry){.change = CHANGED, .list = list, .at = at, .field = field,
                                  .old = list->items[at][field]})) {
        free(copy.text);
        return PURE_ALLOCATION;
    }
    list->items[at][field] = copy;
    return PURE_OK;
}

PureCell pure_list_get(const PureList *list, size_t at, unsigned field) {
    return list->items[at][field];
}

void pure_undo_rollback(PureUndo *undo) {
    Entry *entries = undo->entries;
    for (size_t i = undo->count; i-- > 0;) {
        Entry *e = &entries[i];
        if (e->change == INSERTED) free_item(e->list, take(e->list, e->at));
        else if (e->change == REMOVED) place(e->list, e->at, e->item); /* capacity is still there */
        else {
            if (e->list->text_fields[e->field]) free(e->list->items[e->at][e->field].text);
            e->list->items[e->at][e->field] = e->old;
        }
    }
    undo->count = 0;
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
