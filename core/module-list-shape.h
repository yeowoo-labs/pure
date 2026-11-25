#ifndef PURE_MODULE_LIST_SHAPE_H
#define PURE_MODULE_LIST_SHAPE_H

#include "module-list.h"

/* A list as it lies in memory. It is module-list.c's own, and is shown here for one reader besides: what reads and
 * writes a typed buffer's cells where they lie, through the functions at the end of this file (the executor's steps
 * of a buffer, core/module.c's buffer_step). Nothing else outside module-list.c reads a field of it. */
struct Node;

/* Where a leaf keeps each field's cells, and what an item stores. */
typedef struct {
    unsigned fields, texts;
    uint8_t kinds[16];      /* each field's (PURE_CELL_...) */
    uint8_t text_field[16]; /* the text fields, in order */
    uint32_t column[16];    /* where each field's cells start in a leaf */
    size_t leaf_size;
    size_t number_bytes;    /* an item's numbers, stored */
    int key;                /* the number field whose order the list keeps (its leads), or -1 */
} PureListLayout;

/* What a list keeps its items in (pure_list_form). */
enum { PURE_FORM_TREE, PURE_FORM_FLAT, PURE_FORM_DENSE, PURE_FORM_HASHED };

struct PureList {
    PureListLayout layout;
    struct Node *root;           /* NULL when the list has no item */
    size_t count, bytes;  /* items, and their stored bytes */
    /* A stored list's changes since they were last taken, as records
     * (pure_list_replay); none are kept while journaled is false. */
    bool journaled;
    unsigned char *journal;
    size_t journal_size, journal_capacity;
    uint64_t id, version; /* which list, and a number every change moves on */
    uint64_t shares;      /* the last copy's epoch: a node or text made before it is shared */
    PureRetired *retired; /* where shared nodes and texts go when the list lets them go */
    struct Node **spare[2];      /* inner nodes and leaves let go that no copy reads, kept for the next ones */
    unsigned spares[2], spare_room[2];
    unsigned needed[2];   /* inner nodes and leaves it made lately: halved each time the retired come back */
    unsigned refs;        /* a shared copy's references */
    bool copy;            /* a shared copy: it owns none of its nodes or texts */
    /* Its keys as a set, beside the tree (pure_list_hashed; NULL: none):
     * while every key is a whole number below BITMAP_KEYS, a bit per number
     * (member_capacity words); else a key's bits (-0 as +0) by open
     * addressing (member_capacity slots), VACANT where none is. */
    uint64_t *members;
    size_t member_capacity, member_count;
    bool bitmap;
    /* An event's list may keep its items in arrays rather than a tree
     * (pure_list_arrays, docs/design/021): FLAT, each field's cells side by
     * side in the list's order; DENSE (keyed by small whole numbers, its keys
     * as bits), each field's cells at its key; HASHED (keyed by numbers that
     * came out of order), each field's cells at its key's slot in its
     * members' table (as a DENSE list's at its key). What the arrays cannot answer at
     * once (a position in a DENSE or HASHED list; a change logged to be
     * undone, a record, a share) makes them FLAT, or a tree, first. */
    uint8_t form;
    bool arrays;       /* it may be FLAT or DENSE: an event's list */
    bool borrowed;     /* FLAT, its one column a message's rows where they are (pure_list_borrow): not its to free or change */
    bool pinned;       /* a typed buffer's (pure_list_pin): its cells 8 bytes whatever they hold, FLAT whatever its size */
    uint8_t flattened; /* DENSE made FLAT since it was cleared: twice, and it stays FLAT or a tree */
    void *columns[16]; /* each field's cells */
    uint8_t width[16]; /* the bytes of a cell in each: array_width */
    size_t room;       /* the rows the columns hold: FLAT items, DENSE keys */
    size_t moved;      /* FLAT: items moved by changes inside it since it was cleared */
    /* A stored list keyed by a number and searched by key often at one
     * version is packed when an event ends (pure_list_settle, docs/design
     * 022): FLAT, its columns a Pack's, which its copies share and nothing
     * changes (a change makes the list a tree again first). searched counts
     * its searches by key at searched_version; aligned is the fewest
     * trailing zero bits of a whole key searched then (64: none). */
    struct Pack *pack;
    uint64_t searched, searched_version;
    uint8_t aligned;
    /* Last, apart from what is read often: a DENSE list's keys as last counted (dense_keys, docs/design/023):
     * whether they are every number below its count (full: a key is its
     * position), else the keys below each word of its bits (ranks); for the
     * keys it had when keys_seen was keys_token (its key count, and removes:
     * keys taken out or the bits begun again, so a token never comes back). */
    uint64_t removes, keys_seen;
    bool full;
    uint32_t *ranks;
    size_t ranks_room;
    int text_key;      /* the text field it is keyed by (its layout's key -1), or -1: HASHED by its texts' hashes (023) */
};

/* ---- A typed buffer's items where they are
 *
 * A typed buffer is an event's list of one number, bool or i64 that the load judged one (module.h's list_buffer) and
 * that was pinned when it was made (pure_list_pin): from then until it is freed it is empty, or it is FLAT with each
 * item's 8 bytes side by side in its one column, in the list's order (a number's or a bool's as a double's, an
 * i64's its own). It grows and shrinks at its end alone (the judgment's rule), so nothing in it ever moves; nothing
 * narrows it, and no size makes it a tree. */

/* Its column (its count the list's), or NULL where the list is not held so: an empty one that has no column yet, or is
 * a snapshot's copy; and any list the judgment or the pin got wrong. */
static inline int64_t *pure_buffer_cells(const PureList *list) {
    return list->pinned && list->form == PURE_FORM_FLAT ? (int64_t *)list->columns[0] : NULL;
}

/* The item at a position within it set to bits (a number's, a bool's or an i64's): as pure_list_set of it. */
static inline void pure_buffer_set(PureList *list, int64_t *cells, size_t at, int64_t bits) {
    cells[at] = bits;
    ++list->version;
}

/* An item of bits at its end, as pure_list_insert there; false where its column has no room for one more (the list's
 * own function grows it, or refuses the item past PURE_LIST_ITEMS). */
static inline bool pure_buffer_append(PureList *list, int64_t *cells, int64_t bits) {
    if (list->count >= list->room || list->count >= PURE_LIST_ITEMS) return false;
    cells[list->count++] = bits;
    list->bytes += list->layout.number_bytes;
    ++list->version;
    return true;
}

/* Its last item taken off (it has one), as pure_list_remove of it. */
static inline void pure_buffer_drop(PureList *list) {
    --list->count;
    list->bytes -= list->layout.number_bytes;
    ++list->version;
}

#endif
