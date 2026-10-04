#include "module-list.h"
#include "module-text.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* A list's items live in a persistent B+tree: leaves hold up to FANOUT
 * items, inner nodes up to FANOUT children, and every node knows how many
 * items lie below it and the leaf holding the first of them. An inner node
 * also keeps, beside its children, how many items lie below each and (in a
 * list keyed by a number field) each one's first key: its guide. So an item
 * is found by position, and a keyed list searched by key, reading one node a
 * level, in O(log n) whatever the list's length; a run of positions is read
 * through a finger (the last leaf a thread read) or a leaf at a time
 * (pure_list_run).
 *
 * A leaf keeps its items field by field: each field's cells side by side,
 * numbers as binary64 values and texts as spans of bytes the list owns. So
 * reading one field of items after one another reads memory in order, an
 * item takes no allocation of its own and a leaf is copied in one piece.
 *
 * Nodes carry the list's share count when they were made, and so do texts.
 * One made before the last share may be read by a shared copy (a snapshot),
 * so it never changes again: the list copies the nodes on the way to a change
 * (the path) and retires the ones it replaced, and retires a text it lets go
 * instead of freeing it. A share is then a root and a count, however long
 * the list. */
enum { FANOUT = PURE_LIST_RUN, DEPTH = 16, SPARE_NODES = 8, PREFETCHED = 1 << 17 };

typedef struct Node {
    uint64_t made;            /* the list's share count when made: shared once below it */
    uint32_t count;           /* items (a leaf) or children */
    bool leaf;
    size_t items;             /* items below */
    const struct Node *first; /* the leaf holding the first item below; NULL when there is none */
    double lead;              /* the first item's key, in a list keyed by a number field (else 0) */
    struct Node *child[];     /* an inner node's children, then its guide; a leaf's columns are here instead */
} Node;

/* An inner node's guide, after its children: the items before each child
 * and each child's lead, kept by sum; past the last child the counts and
 * keys are above any position and key. So a level is searched in one node,
 * by a binary search of fixed length whose steps are selections, not
 * branches (upper). */
typedef struct { uint32_t before[FANOUT]; double keys[FANOUT]; } Guide;

enum { INNER_SIZE = sizeof(Node) + FANOUT * sizeof(Node *) + sizeof(Guide) };

/* Where a leaf keeps each field's cells, and what an item stores. */
typedef struct {
    unsigned fields, texts;
    uint8_t kinds[16];      /* each field's (PURE_CELL_...) */
    uint8_t text_field[16]; /* the text fields, in order */
    uint32_t column[16];    /* where each field's cells start in a leaf */
    size_t leaf_size;
    size_t number_bytes;    /* an item's numbers, stored */
    int key;                /* the number field whose order the list keeps (its leads), or -1 */
} Layout;

struct PureList {
    Layout layout;
    Node *root;           /* NULL when the list has no item */
    size_t count, bytes;  /* items, and their stored bytes */
    /* A stored list's changes since they were last taken, as records
     * (pure_list_replay); none are kept while journaled is false. */
    bool journaled;
    unsigned char *journal;
    size_t journal_size, journal_capacity;
    uint64_t id, version; /* which list, and a number every change moves on */
    uint64_t shares;      /* the last copy's epoch: a node or text made before it is shared */
    PureRetired *retired; /* where shared nodes and texts go when the list lets them go */
    Node **spare[2];      /* inner nodes and leaves let go that no copy reads, kept for the next ones */
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
};

/* The log of an event's changes: a removed item's cells wait in the log's
 * cells (its texts owned by the log) until the event is undone or kept. */
typedef enum { INSERTED, REMOVED, CHANGED } Change;
typedef struct {
    Change change;
    unsigned field;
    PureList *list;
    size_t at;
    union {
        PureCell old; /* CHANGED: the replaced cell */
        size_t cells; /* REMOVED: where the item's cells start in the log's cells */
    };
} Entry;
/* A node or text let go while a copy may read it, and what the copies and
 * the lists ask of it (so neither reads it again). */
typedef struct {
    void *pointer;
    uint64_t made;  /* the share count it was made at */
    uint32_t size;  /* a node's */
    bool text, leaf;
} Retired;

static uint64_t lists_made;

/* The leaves a thread read last, and where they start, a few lists' (one
 * each): a list read position by position (a view's rows, a fold, a walk)
 * finds each next item there, and an item found by key is read there, while
 * the thread reads other lists between (a search reads three). Each holds
 * for its list (its id) as it was (its version) and no other. */
enum { FINGERS = 4 };
typedef struct { uint64_t id, version; const Node *leaf; size_t start; } Finger;
static _Thread_local Finger fingers[FINGERS];
static _Thread_local unsigned finger_turn;

/* The leaf a finger keeps that holds position at of the list as it is, or NULL. */
static const Node *fingered(const PureList *list, size_t at, size_t *within) {
    for (unsigned i = 0; i < FINGERS; ++i) {
        const Finger *f = &fingers[i];
        if (f->leaf && f->id == list->id && f->version == list->version && at >= f->start && at < f->start + f->leaf->count) {
            *within = at - f->start;
            if (i) { Finger hit = *f; fingers[i] = fingers[0]; fingers[0] = hit; } /* first looked at next */
            return fingers[0].leaf;
        }
    }
    return NULL;
}

/* A leaf of the list as it is, from start: kept in the list's finger, or the one used longest ago. */
static void finger_keep(const PureList *list, const Node *leaf, size_t start) {
    unsigned at = FINGERS;
    for (unsigned i = 0; i < FINGERS && at == FINGERS; ++i) if (fingers[i].id == list->id) at = i;
    if (at == FINGERS) at = finger_turn++ % FINGERS;
    fingers[at] = (Finger){list->id, list->version, leaf, start};
}

/* ---- A keyed list's keys as a set
 * Node numbers, ranks and counts are small whole numbers: as bits they fit
 * the caches (100,000 keys in 12.5 KB), where a table of them does not. */

enum { BITMAP_KEYS = 1 << 22, BITMAP_KEPT = 1 << 13 /* words a cleared bitmap keeps (64 KiB) */ };
static const uint64_t VACANT = UINT64_C(0x7FF8000000000001); /* a NaN: never a key */

static uint64_t key_bits(double key) {
    if (key == 0) key = 0; /* -0 and +0 are one key */
    uint64_t bits;
    memcpy(&bits, &key, sizeof(bits));
    return bits;
}

static bool small_key(double key) { return key >= 0 && key < BITMAP_KEYS && (double)(uint32_t)key == key; }

static size_t member_slot(uint64_t bits, size_t mask) {
    bits ^= bits >> 33;
    bits *= UINT64_C(0xff51afd7ed558ccd);
    bits ^= bits >> 33;
    return (size_t)bits & mask;
}

/* The set given up (memory ran out growing it): the list is searched without it. */
static void members_drop(PureList *list) {
    free(list->members);
    list->members = NULL;
    list->member_capacity = list->member_count = 0;
}

/* A new set, empty, as bits. */
static bool members_start(PureList *list) {
    list->members = calloc(1, sizeof(*list->members));
    list->member_capacity = list->members ? 1 : 0;
    list->member_count = 0;
    list->bitmap = true;
    return list->members != NULL;
}

static void member_put(PureList *list, uint64_t bits) {
    size_t mask = list->member_capacity - 1, at = member_slot(bits, mask);
    while (list->members[at] != VACANT && list->members[at] != bits) at = (at + 1) & mask;
    if (list->members[at] == VACANT) list->members[at] = bits, ++list->member_count;
}

/* A table of capacity slots holding the keys the set holds. False when memory runs out. */
static bool members_table(PureList *list, size_t capacity) {
    uint64_t *old = list->members, *table = malloc(capacity * sizeof(*table));
    if (!table) return false;
    size_t was = list->member_capacity;
    bool bits = list->bitmap;
    for (size_t i = 0; i < capacity; ++i) table[i] = VACANT;
    list->members = table, list->member_capacity = capacity, list->member_count = 0, list->bitmap = false;
    for (size_t i = 0; i < was; ++i) {
        if (!bits) { if (old[i] != VACANT) member_put(list, old[i]); continue; }
        for (uint64_t word = old[i]; word; word &= word - 1)
            member_put(list, key_bits((double)(i * 64 + (size_t)__builtin_ctzll(word))));
    }
    free(old);
    return true;
}

static void member_add(PureList *list, double key) {
    if (!list->members) return;
    if (list->bitmap && small_key(key)) {
        size_t bit = (size_t)key, word = bit / 64;
        if (word >= list->member_capacity) { /* the bits grown to hold it */
            size_t capacity = list->member_capacity;
            while (capacity <= word) capacity *= 2;
            uint64_t *grown = realloc(list->members, capacity * sizeof(*grown));
            if (!grown) { members_drop(list); return; }
            memset(grown + list->member_capacity, 0, (capacity - list->member_capacity) * sizeof(*grown));
            list->members = grown, list->member_capacity = capacity;
        }
        uint64_t mask = UINT64_C(1) << (bit % 64);
        list->member_count += !(list->members[word] & mask);
        list->members[word] |= mask;
        return;
    }
    size_t need = 2 * (list->member_count + 1), capacity = list->bitmap ? 64 : list->member_capacity;
    while (capacity < need) capacity *= 2;
    if ((list->bitmap || capacity != list->member_capacity) && !members_table(list, capacity)) { members_drop(list); return; }
    member_put(list, key_bits(key));
}

/* A key out of the set; in a table the keys after it in its run moved back (no gap left behind them). */
static void member_remove(PureList *list, double key) {
    if (!list->members) return;
    if (list->bitmap) {
        if (!small_key(key) || (size_t)key / 64 >= list->member_capacity) return;
        uint64_t *word = &list->members[(size_t)key / 64], mask = UINT64_C(1) << ((size_t)key % 64);
        list->member_count -= (*word & mask) != 0;
        *word &= ~mask;
        return;
    }
    uint64_t bits = key_bits(key);
    size_t mask = list->member_capacity - 1, at = member_slot(bits, mask);
    while (list->members[at] != bits) {
        if (list->members[at] == VACANT) return;
        at = (at + 1) & mask;
    }
    for (size_t next = (at + 1) & mask; list->members[next] != VACANT; next = (next + 1) & mask) {
        size_t home = member_slot(list->members[next], mask);
        /* next may fill the gap at when its home is not cyclically within (at, next] */
        bool stays = at <= next ? (home > at && home <= next) : (home > at || home <= next);
        if (!stays) list->members[at] = list->members[next], at = next;
    }
    list->members[at] = VACANT;
    --list->member_count;
}

static Node *child(const Node *node, unsigned i) { return node->child[i]; }
static Guide *guide(const Node *node) { return (Guide *)((char *)node + sizeof(Node) + FANOUT * sizeof(Node *)); }

/* How many of n (at least one) ascending values are below key. */
static unsigned lower(const double *values, unsigned n, double key) {
    const double *base = values;
    for (; n > 1; n -= n / 2) base = base[n / 2] < key ? base + n / 2 : base;
    return (unsigned)(base - values) + (*base < key);
}
/* The child of an inner node holding its item at (counted from the node's
 * first): its children after the first whose items begin at or before at,
 * counted rather than halved (no load waits for another; past the last
 * child the counts are above any position). The end is in the last child. */
static unsigned child_at(const Node *node, size_t at) {
    if (at >= node->items) return node->count - 1;
    const uint32_t *before = guide(node)->before;
    uint32_t n = 0, position = (uint32_t)at; /* every child, the first's 0 at or below any: a whole number of vectors */
    for (unsigned i = 0; i < FANOUT; ++i) n += before[i] <= position;
    return n - 1;
}
/* An inner node's last child whose lead is at or below key (the first when
 * none is): its leads after the first at or below key, counted (past the
 * last child they are above any key). */
static unsigned child_by_key(const Node *node, double key) {
    const double *keys = guide(node)->keys;
    uint64_t n = 0; /* every child's (a whole number of vectors), less the first's */
    for (unsigned i = 0; i < FANOUT; ++i) n += keys[i] <= key;
    return (unsigned)n - (keys[0] <= key);
}
static double *numbers(const Layout *l, const Node *leaf, unsigned f) { return (double *)((char *)leaf + l->column[f]); }
static int64_t *integers(const Layout *l, const Node *leaf, unsigned f) { return (int64_t *)((char *)leaf + l->column[f]); }
static PureSpan *texts(const Layout *l, const Node *leaf, unsigned f) { return (PureSpan *)((char *)leaf + l->column[f]); }

/* A number's, bool's or integer's cell is its 8 bytes, read as they are
 * (whichever it holds; the reader's kind says which). */
static PureCell cell_at(const Layout *l, const Node *leaf, unsigned i, unsigned f) {
    PureCell cell = {0};
    if (!pure_cell_text(l->kinds[f])) memcpy(&cell.i64, (const char *)leaf + l->column[f] + 8 * (size_t)i, 8);
    else {
        PureSpan text = texts(l, leaf, f)[i];
        cell.text = (unsigned char *)text.data, cell.size = text.size;
    }
    return cell;
}

static void put_cells(const Layout *l, Node *leaf, unsigned i, const PureCell *cells) {
    for (unsigned f = 0; f < l->fields; ++f)
        if (pure_cell_text(l->kinds[f])) texts(l, leaf, f)[i] = (PureSpan){cells[f].text, cells[f].size};
        else if (l->kinds[f] == PURE_CELL_INTEGER) integers(l, leaf, f)[i] = cells[f].i64;
        else numbers(l, leaf, f)[i] = cells[f].num;
}

/* count items or children of from, from start on, to at in to (they may overlap). */
static void move_items(const Layout *l, Node *to, unsigned at, const Node *from, unsigned start, unsigned count) {
    if (!count) return;
    if (!from->leaf) {
        memmove(&to->child[at], &from->child[start], count * sizeof(Node *));
        return;
    }
    for (unsigned f = 0; f < l->fields; ++f) {
        size_t width = pure_cell_text(l->kinds[f]) ? sizeof(PureSpan) : 8;
        memmove((char *)to + l->column[f] + at * width, (const char *)from + l->column[f] + start * width, count * width);
    }
}

/* A text's bytes, the list's own: after the list's share count when they
 * were made. One made before the last share may be read by a shared copy. */
static unsigned char *new_text(const PureList *list, const unsigned char *bytes, size_t size) {
    uint64_t *made = malloc(sizeof(*made) + size);
    if (!made) return NULL;
    *made = list->shares;
    memcpy(made + 1, bytes, size);
    return (unsigned char *)(made + 1);
}

static bool shared_text(const PureList *list, const unsigned char *text) { return ((const uint64_t *)text)[-1] < list->shares; }
static bool shared_node(const PureList *list, const Node *node) { return node->made < list->shares; }
static void free_text(const unsigned char *text) { if (text) free((uint64_t *)text - 1); }

/* Kept until the copies that may read it are gone (pure_retired_free), or
 * then taken back (pure_retired_reuse). */
static void retire(PureList *list, void *pointer, bool text) {
    PureRetired *retired = list->retired;
    if (retired->count == retired->capacity) {
        size_t capacity = retired->capacity ? 2 * retired->capacity : 16;
        Retired *grown = realloc(retired->entries, capacity * sizeof(*grown));
        if (!grown) return; /* kept for good rather than freed while read */
        retired->entries = grown;
        retired->capacity = capacity;
    }
    const Node *node = text ? NULL : pointer;
    ((Retired *)retired->entries)[retired->count++] =
        text ? (Retired){pointer, ((const uint64_t *)pointer)[-1], 0, true, false}
             : (Retired){pointer, node->made, node->leaf ? (uint32_t)list->layout.leaf_size : INNER_SIZE, false, node->leaf};
}

/* The list no longer has text: free it, or retire it while a copy may read it. */
static void let_go_text(PureList *list, const unsigned char *text) {
    if (!text) return;
    if (!shared_text(list, text)) free_text(text);
    else retire(list, (void *)text, true);
}

/* The texts of an item the list no longer has. */
static void let_go_cells(PureList *list, const PureCell *cells) {
    for (unsigned t = 0; t < list->layout.texts; ++t) let_go_text(list, cells[list->layout.text_field[t]].text);
}

void pure_retired_free(PureRetired *retired) {
    Retired *entries = retired->entries;
    for (size_t i = 0; i < retired->count; ++i) {
        if (entries[i].text) free_text(entries[i].pointer);
        else free(entries[i].pointer);
    }
    free(entries);
    *retired = (PureRetired){0};
}

/* Room for more entries in retired; false when memory runs out. */
static bool retired_room(PureRetired *retired, size_t more) {
    if (retired->count + more <= retired->capacity) return true;
    size_t capacity = 2 * retired->capacity > retired->count + more ? 2 * retired->capacity : retired->count + more;
    Retired *grown = realloc(retired->entries, capacity * sizeof(*grown));
    if (!grown) return false;
    retired->entries = grown;
    retired->capacity = capacity;
    return true;
}

void pure_retired_pass(PureRetired *from, uint64_t epoch, PureRetired *kept, PureRetired *gone) {
    Retired *entries = from->entries;
    size_t keeping = 0;
    for (size_t i = 0; kept && i < from->count; ++i) keeping += entries[i].made < epoch;
    bool room = (!keeping || retired_room(kept, keeping)) &&
                (!gone || keeping == from->count || retired_room(gone, from->count - keeping));
    for (size_t i = 0; i < from->count; ++i) {
        PureRetired *to = kept && entries[i].made < epoch ? kept : gone;
        if (room && to) ((Retired *)to->entries)[to->count++] = entries[i];
        /* else kept for good rather than freed while read (memory ran out) */
    }
    from->count = 0; /* its room stays, for what is retired next */
}

/* Keep node for the list's next of its kind; false when it keeps enough
 * already (or has no room for more). */
static bool keep_spare(PureList *list, Node *node, bool leaf, unsigned enough) {
    if (list->spares[leaf] >= enough) return false;
    if (list->spares[leaf] == list->spare_room[leaf]) {
        unsigned room = list->spare_room[leaf] ? 2 * list->spare_room[leaf] : SPARE_NODES;
        Node **grown = realloc(list->spare[leaf], room * sizeof(*grown));
        if (!grown) return false;
        list->spare[leaf] = grown;
        list->spare_room[leaf] = room;
    }
    list->spare[leaf][list->spares[leaf]++] = node;
    return true;
}

void pure_retired_reuse(PureRetired *returned, PureList *const *lists, unsigned count) {
    Retired *entries = returned->entries;
    for (size_t i = 0; i < returned->count; ++i) {
        const Retired *r = &entries[i];
        if (r->text) {
            free_text(r->pointer);
            continue;
        }
        bool kept = false; /* by a list of its size, as many as it made lately (at least a few) */
        for (unsigned l = 0; l < count && !kept; ++l) {
            PureList *list = lists[l];
            kept = list && (!r->leaf || list->layout.leaf_size == r->size) &&
                   keep_spare(list, r->pointer, r->leaf, list->needed[r->leaf] > SPARE_NODES ? list->needed[r->leaf] : SPARE_NODES);
        }
        if (!kept) free(r->pointer);
    }
    for (unsigned l = 0; l < count; ++l)
        if (lists[l]) lists[l]->needed[0] /= 2, lists[l]->needed[1] /= 2;
    returned->count = 0;
}

static Node *new_node(PureList *list, bool leaf) {
    Node *node;
    ++list->needed[leaf];
    if (list->spares[leaf]) node = list->spare[leaf][--list->spares[leaf]];
    else if (!(node = malloc(leaf ? list->layout.leaf_size : INNER_SIZE)))
        return NULL;
    node->made = list->shares;
    node->count = 0;
    node->leaf = leaf;
    node->items = 0;
    node->first = NULL;
    node->lead = 0;
    return node;
}

/* A node the list no longer has (its items and children dealt with):
 * retired while a copy may read it, else kept for the next or freed. */
static void drop_node(PureList *list, Node *node) {
    if (shared_node(list, node)) retire(list, node, false);
    else if (!keep_spare(list, node, node->leaf, SPARE_NODES)) free(node);
}

/* ---- The stored form: each field in order, a number as its binary64 bits
 * (little-endian), a text as a u32le length and its bytes. */

static size_t cell_bytes(const Layout *l, unsigned field, PureCell cell) { return pure_cell_text(l->kinds[field]) ? 4 + cell.size : 8; }

static size_t cells_bytes(const Layout *l, const PureCell *cells) {
    size_t size = l->number_bytes;
    for (unsigned t = 0; t < l->texts; ++t) size += 4 + cells[l->text_field[t]].size;
    return size;
}

static size_t item_bytes(const Layout *l, const Node *leaf, unsigned i) {
    size_t size = l->number_bytes;
    for (unsigned t = 0; t < l->texts; ++t) size += 4 + texts(l, leaf, l->text_field[t])[i].size;
    return size;
}

/* A node's summary, from its items or all its children. */
static void sum(const PureList *list, Node *node) {
    const Layout *l = &list->layout;
    if (node->leaf) {
        node->items = node->count;
        node->first = node->count ? node : NULL;
        node->lead = l->key >= 0 && node->count ? numbers(l, node, (unsigned)l->key)[0] : 0;
        return;
    }
    Guide *g = guide(node);
    size_t items = 0;
    for (unsigned i = 0; i < node->count; ++i) {
        const Node *below = child(node, i);
        g->before[i] = (uint32_t)items;
        g->keys[i] = below->lead;
        items += below->items;
    }
    for (unsigned i = node->count; i < FANOUT; ++i) g->before[i] = UINT32_MAX, g->keys[i] = INFINITY;
    node->items = items;
    node->first = node->count ? child(node, 0)->first : NULL;
    node->lead = node->count ? g->keys[0] : 0;
}

/* ---- Finding positions */

typedef struct { Node *node; unsigned index; } Step; /* a node on the way, and the child taken */

/* The leaf holding position at (the end is in the last leaf), and at's
 * place in it. */
static const Node *leaf_of(const PureList *list, size_t at, size_t *within) {
    const Node *kept = fingered(list, at, within);
    if (kept) return kept;
    const Node *node = list->root;
    size_t start = 0;
    while (!node->leaf) {
        unsigned i = child_at(node, at - start);
        start += guide(node)->before[i];
        node = child(node, i);
    }
    finger_keep(list, node, start);
    *within = at - start;
    return node;
}

/* The node at *place, the list's own to change: a copy (the node retired)
 * while a shared copy of the list may read it. NULL without memory. A copy
 * moves the version on even when the change it is for does not happen: the
 * list no longer has the nodes its shared copies have. A leaf's copy has the
 * texts of the leaf it copies: they stay shared, and are retired, not freed,
 * when the list lets them go. */
static Node *own(PureList *list, Node **place) {
    Node *node = *place;
    if (!shared_node(list, node)) return node;
    Node *mine = new_node(list, node->leaf);
    if (!mine) return NULL;
    uint64_t made = mine->made;
    memcpy(mine, node, node->leaf ? list->layout.leaf_size : INNER_SIZE);
    mine->made = made;
    if (mine->leaf) mine->first = mine->count ? mine : NULL;
    retire(list, node, false);
    *place = mine;
    ++list->version;
    return mine;
}

/* The path to the leaf holding position at, each node the list's own, and
 * at's place in the leaf: the number of steps, 0 without memory (the list
 * is as it was: copies replace what they copy). The nodes on the path then
 * name the leaf that holds their first item, a copied one too. */
static unsigned descend(PureList *list, size_t at, Step path[DEPTH], unsigned *within) {
    Node **place = &list->root;
    for (unsigned depth = 0; depth < DEPTH; ++depth) {
        Node *node = own(list, place);
        if (!node) return 0;
        if (node->leaf) {
            path[depth] = (Step){node, 0};
            *within = (unsigned)at;
            for (unsigned d = depth; d-- > 0;) path[d].node->first = child(path[d].node, 0)->first;
            return depth + 1;
        }
        unsigned i = child_at(node, at);
        at -= guide(node)->before[i];
        path[depth] = (Step){node, i};
        place = &node->child[i];
    }
    return 0;
}

/* After a change below the path's last node: that node summed whole, and
 * each above it through the one child on the path (the others are as they
 * were). */
static void sum_path(const PureList *list, const Step *path, unsigned depth) {
    if (!depth) return;
    sum(list, path[depth - 1].node);
    for (unsigned d = depth - 1; d-- > 0;) {
        Node *node = path[d].node;
        unsigned i = path[d].index;
        const Node *below = child(node, i);
        Guide *g = guide(node);
        size_t was = (i + 1 < node->count ? g->before[i + 1] : node->items) - g->before[i];
        if (below->items != was) {
            uint32_t moved = (uint32_t)(below->items - was); /* modulo 2^32: an item fewer is all ones */
            for (unsigned j = i + 1; j < node->count; ++j) g->before[j] += moved;
            node->items += below->items - was;
        }
        g->keys[i] = below->lead;
        node->first = child(node, 0)->first;
        node->lead = g->keys[0];
    }
}

/* ---- Placing and removing */

/* A place for an item: the path to it, and the nodes a split may take (a
 * leaf, then one a level above it and a root), made before anything changes
 * so that placing cannot fail. */
typedef struct {
    Step path[DEPTH];
    unsigned depth, within, extras, used;
    bool appending; /* at the end of the list */
    Node *extra[DEPTH + 1];
} Place;

/* The nodes a split may take, made now so that placing cannot fail. */
static bool prepare_extras(PureList *list, Place *p) {
    if (p->path[p->depth - 1].node->count < FANOUT) return true;
    for (; p->extras <= p->depth; ++p->extras)
        if (!(p->extra[p->extras] = new_node(list, !p->extras))) {
            while (p->extras) drop_node(list, p->extra[--p->extras]);
            return false;
        }
    return true;
}

static bool prepare(PureList *list, size_t at, Place *p) {
    if (!list->root && !(list->root = new_node(list, true))) return false;
    p->extras = p->used = 0;
    p->appending = at == list->count;
    if (!(p->depth = descend(list, at, p->path, &p->within))) return false;
    return prepare_extras(list, p);
}

/* The path to where key goes in a list kept in order of a number field (its
 * key), the nodes on it the list's own; where in the list that is, and
 * whether an item has the key there. False when memory runs out. */
static bool descend_key(PureList *list, double key, Place *p, size_t *at, bool *found) {
    const Layout *l = &list->layout;
    Node **place = &list->root;
    size_t start = 0;
    for (unsigned depth = 0; depth < DEPTH; ++depth) {
        Node *node = own(list, place);
        if (!node) return false;
        if (node->leaf) {
            const double *keys = numbers(l, node, (unsigned)l->key);
            unsigned low = node->count ? lower(keys, node->count, key) : 0;
            *found = low < node->count && keys[low] == key;
            p->path[depth] = (Step){node, 0};
            p->depth = depth + 1;
            p->within = low;
            *at = start + low;
            for (unsigned d = depth; d-- > 0;) p->path[d].node->first = child(p->path[d].node, 0)->first;
            return true;
        }
        const Guide *g = guide(node);
        unsigned taken = child_by_key(node, key);
        start += g->before[taken];
        p->path[depth] = (Step){node, taken};
        place = &node->child[taken];
    }
    return false;
}

static void release_extras(PureList *list, Place *p) {
    while (p->extras > p->used) drop_node(list, p->extra[--p->extras]);
}

/* An item's cells (the list owns its texts from then on) into the leaf at
 * the end of the path, before its within: a full node splits and the new
 * node goes into its parent, up to a new root. A node splits in halves, but
 * at the end of the list it stays full and the new one starts with the item
 * alone: a list that grows at its end (a history, a keyed list filled in key
 * order) fills its leaves. */
static void place(PureList *list, Place *p, const PureCell *cells) {
    const Layout *l = &list->layout;
    unsigned index = p->within;
    if (list->members) member_add(list, cells[l->key].num);
    Node *pointer = NULL; /* above the leaf: the node that goes in */
    for (unsigned d = p->depth; d-- > 0;) {
        Node *node = p->path[d].node;
        if (node->count < FANOUT) {
            move_items(l, node, index + 1, node, index, node->count - index);
            if (node->leaf) put_cells(l, node, index, cells);
            else node->child[index] = pointer;
            ++node->count;
            sum_path(list, p->path, d + 1);
            break;
        }
        unsigned keep = p->appending && index == FANOUT ? FANOUT : (FANOUT + 1) / 2;
        Node *right = p->extra[p->used++];
        if (index < keep) {
            move_items(l, right, 0, node, keep - 1, FANOUT - (keep - 1));
            move_items(l, node, index + 1, node, index, keep - 1 - index);
            if (node->leaf) put_cells(l, node, index, cells);
            else node->child[index] = pointer;
        } else {
            move_items(l, right, 0, node, keep, index - keep);
            if (node->leaf) put_cells(l, right, index - keep, cells);
            else right->child[index - keep] = pointer;
            move_items(l, right, index - keep + 1, node, index, FANOUT - index);
        }
        node->count = keep;
        right->count = FANOUT + 1 - keep;
        sum(list, node);
        sum(list, right);
        if (!d) { /* the root split: a new one above both halves */
            Node *root = p->extra[p->used++];
            root->child[0] = node;
            root->child[1] = right;
            root->count = 2;
            sum(list, root);
            list->root = root;
            break;
        }
        pointer = right;
        index = p->path[d - 1].index + 1;
    }
    release_extras(list, p);
    ++list->count;
    ++list->version;
}

/* The item at the leaf's within, out of the tree (the caller has its cells):
 * an emptied node leaves its parent, a node that fits with a neighbour
 * merges into one (when the neighbour can be made the list's own), and a
 * root with one child gives way to it. */
static void take_out(PureList *list, Step *path, unsigned depth, unsigned within) {
    const Layout *l = &list->layout;
    Node *leaf = path[depth - 1].node;
    if (list->members) member_remove(list, numbers(l, leaf, (unsigned)l->key)[within]);
    move_items(l, leaf, within, leaf, within + 1, leaf->count - within - 1);
    --leaf->count;
    unsigned d = depth - 1;
    for (;; --d) {
        Node *node = path[d].node;
        sum(list, node);
        if (!d) break;
        Node *parent = path[d - 1].node;
        unsigned at = path[d - 1].index;
        if (!node->count) {
            memmove(&parent->child[at], &parent->child[at + 1], (parent->count - at - 1) * sizeof(Node *));
            --parent->count;
            drop_node(list, node);
            continue;
        }
        unsigned other = at + 1 < parent->count ? at + 1 : at ? at - 1 : at;
        Node *neighbour = other != at && node->count + child(parent, other)->count <= FANOUT
                              ? own(list, &parent->child[other]) : NULL;
        if (!neighbour) break;
        Node *left = other < at ? neighbour : node, *right = other < at ? node : neighbour;
        unsigned left_at = other < at ? other : at;
        move_items(l, left, left->count, right, 0, right->count);
        left->count += right->count;
        sum(list, left);
        memmove(&parent->child[left_at + 1], &parent->child[left_at + 2], (parent->count - left_at - 2) * sizeof(Node *));
        --parent->count;
        drop_node(list, right);
    }
    sum_path(list, path, d);
    while (!list->root->leaf && list->root->count == 1) {
        Node *old = list->root;
        list->root = child(old, 0);
        drop_node(list, old);
    }
    if (!list->root->count) { /* no item left */
        drop_node(list, list->root);
        list->root = NULL;
    }
    --list->count;
    ++list->version;
}

/* ---- The list */

PureList *pure_list_new(unsigned fields, const uint8_t *kinds) {
    if (fields > 16) return NULL;
    PureList *list = calloc(1, sizeof(*list));
    if (!list) return NULL;
    Layout *l = &list->layout;
    l->fields = fields;
    size_t at = sizeof(Node);
    for (unsigned f = 0; f < fields; ++f) {
        l->kinds[f] = kinds[f];
        l->column[f] = (uint32_t)at;
        at += FANOUT * (pure_cell_text(kinds[f]) ? sizeof(PureSpan) : 8);
        if (pure_cell_text(kinds[f])) l->text_field[l->texts++] = (uint8_t)f;
        else l->number_bytes += 8;
    }
    l->leaf_size = at;
    l->key = -1;
    list->id = __atomic_add_fetch(&lists_made, 1, __ATOMIC_RELAXED);
    return list;
}

/* Every node below *place the list's own, summed again. */
static bool rekey(PureList *list, Node **place) {
    Node *node = own(list, place);
    if (!node) return false;
    for (unsigned i = 0; !node->leaf && i < node->count; ++i)
        if (!rekey(list, &node->child[i])) return false;
    sum(list, node);
    return true;
}

PureResult pure_list_keyed(PureList *list, int field) {
    Layout *l = &list->layout;
    int key = field >= 0 && (unsigned)field < l->fields && l->kinds[field] == PURE_CELL_NUMBER ? field : -1;
    if (key == l->key) return PURE_OK;
    l->key = key;
    if (list->root && !rekey(list, &list->root)) {
        l->key = -1; /* searched without leads, which some nodes lack */
        return PURE_ALLOCATION;
    }
    return PURE_OK;
}

/* Every node and text below node let go (retired where a copy may read them). */
static void let_go_tree(PureList *list, Node *node) {
    if (!node) return;
    const Layout *l = &list->layout;
    for (unsigned i = 0; i < node->count; ++i) {
        if (!node->leaf) let_go_tree(list, child(node, i));
        else for (unsigned t = 0; t < l->texts; ++t) let_go_text(list, texts(l, node, l->text_field[t])[i].data);
    }
    if (shared_node(list, node)) retire(list, node, false);
    else free(node);
}

void pure_list_free(PureList *list) {
    if (!list) return;
    if (list->copy) {
        if (__atomic_sub_fetch(&list->refs, 1, __ATOMIC_ACQ_REL)) return;
    } else {
        free(list->journal);
        free(list->members);
        let_go_tree(list, list->root);
        for (unsigned kind = 0; kind < 2; ++kind) {
            while (list->spares[kind]) free(list->spare[kind][--list->spares[kind]]);
            free(list->spare[kind]);
        }
    }
    free(list);
}

void pure_list_clear(PureList *list) {
    if (!list->count) return;
    let_go_tree(list, list->root);
    list->root = NULL;
    list->count = list->bytes = 0;
    ++list->version;
    if (!list->members) return;
    if (list->bitmap && list->member_capacity <= BITMAP_KEPT) { /* small bits are kept for the next event */
        memset(list->members, 0, list->member_capacity * sizeof(*list->members));
        list->member_count = 0;
        return;
    }
    members_drop(list); /* else bits again, from few */
    (void)members_start(list);
}

bool pure_list_hashed(PureList *list) {
    const Layout *l = &list->layout;
    if (list->members) return true;
    if (l->key < 0 || list->copy || !members_start(list)) return false;
    for (size_t at = 0; at < list->count && list->members; ++at) /* the keys it has now, once */
        member_add(list, pure_list_get(list, at, (unsigned)l->key).num);
    return list->members != NULL;
}

bool pure_list_contains(const PureList *list, double key, bool *found) {
    if (!list->members) return false;
    if (list->bitmap) {
        *found = small_key(key) && (size_t)key / 64 < list->member_capacity &&
                 list->members[(size_t)key / 64] >> ((size_t)key % 64) & 1;
        return true;
    }
    uint64_t bits = key_bits(key);
    size_t mask = list->member_capacity - 1, at = member_slot(bits, mask);
    while (list->members[at] != VACANT && list->members[at] != bits) at = (at + 1) & mask;
    *found = list->members[at] == bits;
    return true;
}

size_t pure_list_extra(const PureList *list) { return list->member_capacity * sizeof(*list->members); }

size_t pure_list_count(const PureList *list) { return list->count; }

PureList *pure_list_share(PureList *list, PureRetired *retired, uint64_t epoch) {
    PureList *copy = malloc(sizeof(*copy));
    if (!copy) return NULL;
    *copy = (PureList){.layout = list->layout, .root = list->root, .count = list->count, .bytes = list->bytes,
                       .id = list->id, .version = list->version, .refs = 1, .copy = true};
    list->retired = retired;
    list->shares = epoch > list->shares ? epoch : list->shares + 1;
    return copy;
}

void pure_list_mark(PureList *list, uint64_t epoch) {
    if (list->retired && epoch > list->shares) list->shares = epoch;
}

void pure_list_hold(PureList *copy) { __atomic_add_fetch(&copy->refs, 1, __ATOMIC_RELAXED); }

bool pure_list_current(const PureList *copy, const PureList *list) {
    return copy->id == list->id && copy->version == list->version;
}

void pure_list_stamp(const PureList *list, uint64_t *id, uint64_t *version) {
    *id = list->id;
    *version = list->version;
}

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

/* A cell the list can keep: a text's bytes copied (NULL when empty). */
static bool own_cell(const PureList *list, unsigned field, PureCell from, PureCell *to) {
    bool text = pure_cell_text(list->layout.kinds[field]);
    *to = (PureCell){.i64 = text ? 0 : from.i64}; /* a number's or an integer's bits */
    if (!text || !from.size) return true;
    if (!(to->text = new_text(list, from.text, from.size))) return false;
    to->size = from.size;
    return true;
}

static unsigned char *encode_cell(const Layout *l, unsigned field, PureCell cell, unsigned char *out) {
    bool text = pure_cell_text(l->kinds[field]);
    uint64_t bits = text ? cell.size : (uint64_t)cell.i64; /* a number's bits, or an integer's */
    unsigned width = text ? 4 : 8;
    for (unsigned i = 0; i < width; ++i) *out++ = (unsigned char)(bits >> (8 * i));
    if (text && cell.size) { memcpy(out, cell.text, cell.size); out += cell.size; }
    return out;
}

bool pure_cell_fits(uint8_t kind, PureCell cell) {
    return kind == PURE_CELL_INTEGER || (kind == PURE_CELL_BOOL ? cell.num == 0 || cell.num == 1 : isfinite(cell.num));
}

bool pure_cell_as(uint8_t from, uint8_t to, PureCell *cell) {
    if (from == to) return true;
    if (pure_cell_text(from) || pure_cell_text(to)) return false;
    if (to == PURE_CELL_INTEGER) { /* a whole number in range: -2^63 is, 2^63 is not */
        double v = cell->num;
        if (!(v >= -9223372036854775808.0 && v < 9223372036854775808.0) || v != floor(v)) return false;
        cell->i64 = (int64_t)v;
        return true;
    }
    if (from == PURE_CELL_INTEGER) {
        if (cell->i64 > INT64_C(9007199254740992) || cell->i64 < -INT64_C(9007199254740992)) return false;
        cell->num = (double)cell->i64;
    }
    if (to == PURE_CELL_BOOL) {
        if (cell->num != 0 && cell->num != 1) return false;
        cell->num = cell->num == 1; /* -0 is false as 0 is */
    }
    return true;
}

/* Whether a number cell holds what its kind may: a finite number, a bool 0 or 1. */
static bool fits(const Layout *l, unsigned field, double value) {
    return pure_cell_fits(l->kinds[field], (PureCell){.num = value});
}

/* A stored cell of field at *at in bytes (a text's bytes borrowed from
 * them): false when it is cut short, not finite (not 0 or 1 for a bool) or
 * not UTF-8. */
static bool decode_cell(const Layout *l, unsigned field, const unsigned char *bytes, size_t size, size_t *at,
                        PureCell *cell) {
    bool text = pure_cell_text(l->kinds[field]);
    size_t width = text ? 4 : 8;
    if (size - *at < width) return false;
    uint64_t bits = 0;
    for (size_t i = 0; i < width; ++i) bits |= (uint64_t)bytes[*at + i] << (8 * i);
    *at += width;
    *cell = (PureCell){0};
    if (!text) {
        cell->i64 = (int64_t)bits;
        return l->kinds[field] == PURE_CELL_INTEGER || fits(l, field, cell->num);
    }
    if (bits > size - *at || (l->kinds[field] == PURE_CELL_TEXT && !pure_utf8_valid((PureSpan){bytes + *at, (size_t)bits})))
        return false;
    *cell = (PureCell){.text = (unsigned char *)bytes + *at, .size = (size_t)bits};
    *at += (size_t)bits;
    return true;
}

/* ---- The journal: records of a stored list's changes, in the order they
 * were made. Each is a u8 kind and the position, then for an insert the
 * item's fields, for a set the u8 field and its cell. A record is written
 * compact (kinds 4-6): the position as a varint (LEB128), a number as the
 * varint 2 * zigzag(n) when it is an integer n below 2^53 in size (not -0),
 * else 1 and its binary64 bits (8 bytes, little-endian), an integer as the
 * varint zigzag(n), a text as a varint length and its bytes. Records stored
 * before (kinds 1-3, a list's tree value "PLS2") had a u32le position and
 * cells in stored form. */

enum { FIXED_INSERT = 1, FIXED_REMOVE = 2, FIXED_SET = 3, RECORD_INSERT = 4, RECORD_REMOVE = 5, RECORD_SET = 6,
       FIXED_HEAD = 5, RECORD_HEAD = 6, JOURNAL_KEPT = 1 << 20 };

static unsigned char *put_varint(unsigned char *out, uint64_t value) {
    for (; value >= 0x80; value >>= 7) *out++ = (unsigned char)(value | 0x80);
    *out++ = (unsigned char)value;
    return out;
}

static bool get_varint(const unsigned char *bytes, size_t size, size_t *at, uint64_t *value) {
    *value = 0;
    for (unsigned shift = 0; shift < 64 && *at < size; shift += 7) {
        unsigned char byte = bytes[(*at)++];
        *value |= (uint64_t)(byte & 0x7f) << shift;
        if (!(byte & 0x80)) return true;
    }
    return false;
}

/* A cell in compact form (the most it takes: compact_bound). */
static size_t compact_bound(const Layout *l, unsigned field, PureCell cell) {
    return pure_cell_text(l->kinds[field]) ? 5 + cell.size : 10;
}

static uint64_t zigzag(int64_t n) { return (uint64_t)n << 1 ^ (n < 0 ? UINT64_MAX : 0); }
static int64_t unzigzag(uint64_t z) { return z & 1 ? -(int64_t)(z >> 1) - 1 : (int64_t)(z >> 1); }

static unsigned char *put_compact(const Layout *l, unsigned field, PureCell cell, unsigned char *out) {
    if (l->kinds[field] == PURE_CELL_INTEGER) return put_varint(out, zigzag(cell.i64));
    if (pure_cell_text(l->kinds[field])) {
        out = put_varint(out, cell.size);
        if (cell.size) memcpy(out, cell.text, cell.size);
        return out + cell.size;
    }
    double v = cell.num;
    if (fabs(v) < 9007199254740992.0 && v == (double)(int64_t)v && !(v == 0 && signbit(v)))
        return put_varint(out, zigzag((int64_t)v) << 1);
    uint64_t bits;
    memcpy(&bits, &v, sizeof(bits));
    *out++ = 1;
    for (unsigned i = 0; i < 8; ++i) *out++ = (unsigned char)(bits >> (8 * i));
    return out;
}

static bool get_compact(const Layout *l, unsigned field, const unsigned char *bytes, size_t size, size_t *at,
                        PureCell *cell) {
    uint64_t value;
    *cell = (PureCell){0};
    if (!get_varint(bytes, size, at, &value)) return false;
    if (l->kinds[field] == PURE_CELL_INTEGER) {
        cell->i64 = unzigzag(value);
        return true;
    }
    if (pure_cell_text(l->kinds[field])) {
        if (value > size - *at || (l->kinds[field] == PURE_CELL_TEXT && !pure_utf8_valid((PureSpan){bytes + *at, (size_t)value})))
            return false;
        *cell = (PureCell){.text = (unsigned char *)bytes + *at, .size = (size_t)value};
        *at += (size_t)value;
        return true;
    }
    if (!(value & 1)) { /* an integer */
        if (value >> 1 >= UINT64_C(1) << 54) return false;
        cell->num = (double)unzigzag(value >> 1);
        return fits(l, field, cell->num);
    }
    if (value != 1 || size - *at < 8) return false;
    uint64_t bits = 0;
    for (unsigned i = 0; i < 8; ++i) bits |= (uint64_t)bytes[*at + i] << (8 * i);
    *at += 8;
    memcpy(&cell->num, &bits, sizeof(bits));
    return fits(l, field, cell->num);
}

/* Room for size more bytes at the journal's end, or NULL without memory. */
static unsigned char *journal_room(PureList *list, size_t size) {
    if (list->journal_capacity - list->journal_size < size) {
        size_t capacity = list->journal_capacity ? 2 * list->journal_capacity : 256;
        while (capacity - list->journal_size < size) capacity *= 2;
        unsigned char *grown = realloc(list->journal, capacity);
        if (!grown) return NULL;
        list->journal = grown;
        list->journal_capacity = capacity;
    }
    return list->journal + list->journal_size;
}

static unsigned char *record_head(unsigned char *out, unsigned kind, size_t at) {
    *out++ = (unsigned char)kind;
    return put_varint(out, at);
}

void pure_list_journal(PureList *list, bool journaled) {
    list->journaled = journaled;
    if (!journaled) pure_list_forget(list, 0);
}

PureSpan pure_list_changes(const PureList *list) { return (PureSpan){list->journal, list->journal_size}; }

void pure_list_forget(PureList *list, size_t size) {
    if (size >= list->journal_size) return;
    list->journal_size = size;
    if (!size && list->journal_capacity > JOURNAL_KEPT) { /* a bulk load's room goes back */
        free(list->journal);
        list->journal = NULL;
        list->journal_capacity = 0;
    }
}

/* Every leaf in order, to visit (false stops it). */
static bool visit_leaves(const Node *node, bool (*visit)(void *, const Node *), void *context) {
    if (!node) return true;
    if (node->leaf) return visit(context, node);
    for (unsigned i = 0; i < node->count; ++i)
        if (!visit_leaves(child(node, i), visit, context)) return false;
    return true;
}

typedef struct { const Layout *layout; unsigned char *at; } Encoding;

static bool encode_leaf(void *context, const Node *leaf) {
    Encoding *e = context;
    for (unsigned i = 0; i < leaf->count; ++i)
        for (unsigned f = 0; f < e->layout->fields; ++f) e->at = encode_cell(e->layout, f, cell_at(e->layout, leaf, i, f), e->at);
    return true;
}

size_t pure_list_size(const PureList *list) { return list->bytes; }

void pure_list_encode(const PureList *list, unsigned char *out) {
    Encoding encoding = {&list->layout, out};
    visit_leaves(list->root, encode_leaf, &encoding);
}

size_t pure_list_encode_part(const PureList *list, size_t *at, unsigned char *out, size_t capacity, size_t *needed) {
    const Layout *l = &list->layout;
    PureRun run;
    size_t used = 0;
    *needed = 0;
    if (!pure_list_run(list, *at, &run)) return 0;
    do {
        const Node *leaf = run.path[run.depth];
        for (unsigned i = (unsigned)(*at - run.start); i < leaf->count; ++i) {
            size_t size = item_bytes(l, leaf, i);
            if (size > capacity - used) {
                if (!used) *needed = size;
                return used;
            }
            unsigned char *end = out + used;
            for (unsigned f = 0; f < l->fields; ++f) end = encode_cell(l, f, cell_at(l, leaf, i, f), end);
            used += size;
            ++*at;
        }
    } while (pure_list_next(list, &run));
    return used;
}

/* An item's cells in stored form at *at in bytes. */
static bool decode_item(const Layout *l, const unsigned char *bytes, size_t size, size_t *at, PureCell *cells) {
    for (unsigned f = 0; f < l->fields; ++f)
        if (!decode_cell(l, f, bytes, size, at, &cells[f])) return false;
    return true;
}

PURE_COLD PureResult pure_list_read(PureList *list, const unsigned char *bytes, size_t size) {
    PureCell cells[16];
    for (size_t at = 0; at < size;) {
        if (!decode_item(&list->layout, bytes, size, &at, cells)) return PURE_STORE_ERROR;
        PureResult result = pure_list_insert(list, list->count, cells, NULL);
        if (result != PURE_OK) return result;
    }
    return PURE_OK;
}

/* A record's cell, compact or as stored before. */
static bool record_cell(const Layout *l, bool compact, unsigned field, const unsigned char *bytes, size_t size,
                        size_t *at, PureCell *cell) {
    return compact ? get_compact(l, field, bytes, size, at, cell) : decode_cell(l, field, bytes, size, at, cell);
}

PURE_COLD PureResult pure_list_replay(PureList *list, const unsigned char *bytes, size_t size) {
    const Layout *l = &list->layout;
    bool journaled = list->journaled;
    PureResult result = PURE_OK;
    list->journaled = false;
    for (size_t at = 0; at < size && result == PURE_OK;) {
        unsigned kind = bytes[at++];
        bool compact = kind >= RECORD_INSERT;
        uint64_t position = 0;
        if (compact) {
            if (!get_varint(bytes, size, &at, &position)) { result = PURE_STORE_ERROR; break; }
        } else {
            if (size - at < FIXED_HEAD - 1) { result = PURE_STORE_ERROR; break; }
            for (unsigned i = 0; i < 4; ++i) position |= (uint64_t)bytes[at + i] << (8 * i);
            at += FIXED_HEAD - 1;
        }
        PureCell cells[16];
        unsigned change = compact ? kind - (RECORD_INSERT - FIXED_INSERT) : kind;
        bool ok = true;
        if (change == FIXED_INSERT) {
            for (unsigned f = 0; ok && f < l->fields; ++f) ok = record_cell(l, compact, f, bytes, size, &at, &cells[f]);
            result = ok && position <= list->count ? pure_list_insert(list, (size_t)position, cells, NULL) : PURE_STORE_ERROR;
        } else if (change == FIXED_REMOVE)
            result = position < list->count ? pure_list_remove(list, (size_t)position, NULL) : PURE_STORE_ERROR;
        else if (change == FIXED_SET) {
            unsigned field = at < size ? bytes[at++] : l->fields;
            result = field < l->fields && position < list->count && record_cell(l, compact, field, bytes, size, &at, &cells[0]) ?
                     pure_list_set(list, (size_t)position, field, cells[0], NULL) : PURE_STORE_ERROR;
        } else
            result = PURE_STORE_ERROR;
    }
    list->journaled = journaled;
    return result;
}

PureResult pure_list_insert(PureList *list, size_t at, const PureCell *cells, PureUndo *undo) {
    const Layout *l = &list->layout;
    if (at > list->count || list->count == PURE_LIST_ITEMS) return PURE_LIMIT;
    PureCell item[16];
    unsigned owned = 0;
    for (; owned < l->fields; ++owned)
        if (!own_cell(list, owned, cells[owned], &item[owned])) break;
    size_t size = RECORD_HEAD;
    for (unsigned f = 0; list->journaled && f < l->fields; ++f) size += compact_bound(l, f, cells[f]);
    unsigned char *record = list->journaled ? journal_room(list, size) : NULL;
    Place p;
    if (owned < l->fields || (list->journaled && !record) || !prepare(list, at, &p)) {
        for (unsigned f = 0; f < owned; ++f) free_text(item[f].text);
        return PURE_ALLOCATION;
    }
    if (undo && !log_change(undo, (Entry){.change = INSERTED, .list = list, .at = at})) {
        release_extras(list, &p);
        for (unsigned f = 0; f < l->fields; ++f) free_text(item[f].text);
        return PURE_ALLOCATION;
    }
    if (record) {
        unsigned char *end = record_head(record, RECORD_INSERT, at);
        for (unsigned f = 0; f < l->fields; ++f) end = put_compact(l, f, item[f], end);
        list->journal_size += (size_t)(end - record);
    }
    list->bytes += cells_bytes(l, item);
    place(list, &p, item);
    return PURE_OK;
}

PureResult pure_list_add(PureList *list, const PureCell *cells, PureUndo *undo, size_t *at, bool *exists) {
    const Layout *l = &list->layout;
    *exists = false;
    if (l->key < 0) return PURE_INVALID; /* searched and inserted apart, then */
    if (list->members) { /* the set says at once when the key is there */
        bool found;
        if (pure_list_contains(list, cells[l->key].num, &found) && found) {
            *at = pure_list_seek(list, (unsigned)l->key, cells[l->key], exists);
            return PURE_OK;
        }
    }
    if (list->count == PURE_LIST_ITEMS) return PURE_LIMIT;
    if (!list->root && !(list->root = new_node(list, true))) return PURE_ALLOCATION;
    Place p = {.extras = 0, .used = 0};
    if (!descend_key(list, cells[l->key].num, &p, at, exists)) return PURE_ALLOCATION;
    if (*exists) return PURE_OK;
    p.appending = *at == list->count;
    PureCell item[16];
    unsigned owned = 0;
    for (; owned < l->fields; ++owned)
        if (!own_cell(list, owned, cells[owned], &item[owned])) break;
    size_t size = RECORD_HEAD;
    for (unsigned f = 0; list->journaled && f < l->fields; ++f) size += compact_bound(l, f, cells[f]);
    unsigned char *record = list->journaled ? journal_room(list, size) : NULL;
    if (owned < l->fields || (list->journaled && !record) || !prepare_extras(list, &p)) {
        for (unsigned f = 0; f < owned; ++f) free_text(item[f].text);
        return PURE_ALLOCATION;
    }
    if (undo && !log_change(undo, (Entry){.change = INSERTED, .list = list, .at = *at})) {
        release_extras(list, &p);
        for (unsigned f = 0; f < l->fields; ++f) free_text(item[f].text);
        return PURE_ALLOCATION;
    }
    if (record) {
        unsigned char *end = record_head(record, RECORD_INSERT, *at);
        for (unsigned f = 0; f < l->fields; ++f) end = put_compact(l, f, item[f], end);
        list->journal_size += (size_t)(end - record);
    }
    list->bytes += cells_bytes(l, item);
    place(list, &p, item);
    return PURE_OK;
}

/* Room for one more item's cells in the log. */
static PureCell *log_cells(PureUndo *undo, unsigned fields) {
    if (undo->cell_count + fields > undo->cell_capacity) {
        size_t capacity = undo->cell_capacity ? 2 * undo->cell_capacity : 64;
        while (capacity < undo->cell_count + fields) capacity *= 2;
        PureCell *cells = realloc(undo->cells, capacity * sizeof(*cells));
        if (!cells) return NULL;
        undo->cells = cells;
        undo->cell_capacity = capacity;
    }
    return undo->cells + undo->cell_count;
}

PureResult pure_list_remove(PureList *list, size_t at, PureUndo *undo) {
    const Layout *l = &list->layout;
    if (at >= list->count) return PURE_NOT_FOUND;
    Step path[DEPTH];
    unsigned within, depth = descend(list, at, path, &within);
    if (!depth) return PURE_ALLOCATION;
    Node *leaf = path[depth - 1].node;
    unsigned char *record = list->journaled ? journal_room(list, RECORD_HEAD) : NULL;
    PureCell *cells = undo ? log_cells(undo, l->fields) : NULL, kept[16];
    if ((list->journaled && !record) ||
        (undo && (!cells || !log_change(undo, (Entry){.change = REMOVED, .list = list, .at = at, .cells = undo->cell_count}))))
        return PURE_ALLOCATION;
    if (!cells) cells = kept;
    for (unsigned f = 0; f < l->fields; ++f) cells[f] = cell_at(l, leaf, within, f);
    if (record) list->journal_size += (size_t)(record_head(record, RECORD_REMOVE, at) - record);
    if (undo) undo->cell_count += l->fields;
    list->bytes -= cells_bytes(l, cells);
    if (!undo) let_go_cells(list, cells);
    take_out(list, path, depth, within);
    return PURE_OK;
}

/* A field no summary follows (any but the key) changes in its leaf alone:
 * the leaf the thread read last, when the list alone has it, else the leaf
 * at the end of the list's own path to the item. The thread's finger keeps
 * the leaf, so the item's other fields are read and changed there too. */
PureResult pure_list_set(PureList *list, size_t at, unsigned field, PureCell value, PureUndo *undo) {
    const Layout *l = &list->layout;
    if (at >= list->count) return PURE_NOT_FOUND;
    Step path[DEPTH];
    unsigned within, depth = 0;
    bool keyed = (int)field == l->key;
    Node *leaf;
    size_t found_within;
    const Node *kept = keyed ? NULL : fingered(list, at, &found_within);
    if (kept && !shared_node(list, kept)) {
        leaf = (Node *)kept;
        within = (unsigned)found_within;
    } else {
        if (!(depth = descend(list, at, path, &within))) return PURE_ALLOCATION;
        leaf = path[depth - 1].node;
    }
    PureCell copy;
    if (!own_cell(list, field, value, &copy)) return PURE_ALLOCATION;
    PureCell old = cell_at(l, leaf, within, field);
    size_t size = RECORD_HEAD + 1 + compact_bound(l, field, copy);
    unsigned char *record = list->journaled ? journal_room(list, size) : NULL;
    if ((list->journaled && !record) ||
        (undo && !log_change(undo, (Entry){.change = CHANGED, .list = list, .at = at, .field = field, .old = old}))) {
        free_text(copy.text);
        return PURE_ALLOCATION;
    }
    if (record) {
        unsigned char *end = record_head(record, RECORD_SET, at);
        *end++ = (unsigned char)field;
        list->journal_size += (size_t)(put_compact(l, field, copy, end) - record);
    }
    list->bytes += cell_bytes(l, field, copy) - cell_bytes(l, field, old);
    if (pure_cell_text(l->kinds[field])) texts(l, leaf, field)[within] = (PureSpan){copy.text, copy.size};
    else if (l->kinds[field] == PURE_CELL_INTEGER) integers(l, leaf, field)[within] = copy.i64;
    else numbers(l, leaf, field)[within] = copy.num;
    if (!undo) let_go_text(list, old.text);
    if (keyed) {
        sum_path(list, path, depth);
        if (list->members) member_remove(list, old.num), member_add(list, copy.num);
    }
    ++list->version;
    finger_keep(list, leaf, at - within);
    return PURE_OK;
}

PureCell pure_list_get(const PureList *list, size_t at, unsigned field) {
    size_t within;
    const Node *leaf = leaf_of(list, at, &within); /* first: an argument beside it would be read in either order */
    return cell_at(&list->layout, leaf, (unsigned)within, field);
}

static void open_run(const Layout *l, const Node *leaf, PureRun *run) {
    run->count = leaf->count;
    for (unsigned f = 0; f < l->fields; ++f)
        if (pure_cell_text(l->kinds[f])) run->column[f].texts = texts(l, leaf, f);
        else if (l->kinds[f] == PURE_CELL_INTEGER) run->column[f].integers = integers(l, leaf, f);
        else run->column[f].numbers = numbers(l, leaf, f);
}

bool pure_list_run(const PureList *list, size_t at, PureRun *run) {
    if (at >= list->count) return false;
    const Node *node = list->root;
    size_t start = 0;
    unsigned depth = 0;
    while (!node->leaf) {
        unsigned i = child_at(node, at - start);
        start += guide(node)->before[i];
        run->path[depth] = node;
        run->taken[depth++] = (uint8_t)i;
        node = child(node, i);
    }
    run->path[depth] = node;
    run->depth = depth;
    run->start = start;
    open_run(&list->layout, node, run);
    return true;
}

/* Up to the nearest node with a child after the one taken, then down its
 * first children to a leaf. The leaf after it (a sibling) is fetched ahead:
 * leaves copied at different times lie far apart in memory. */
bool pure_list_next(const PureList *list, PureRun *run) {
    size_t start = run->start + run->count;
    unsigned depth = run->depth;
    while (depth && run->taken[depth - 1] + 1u >= ((const Node *)run->path[depth - 1])->count) --depth;
    if (!depth) return false;
    const Node *parent = run->path[depth - 1], *node = child(parent, ++run->taken[depth - 1]);
    while (!node->leaf) {
        run->path[depth] = parent = node;
        run->taken[depth++] = 0;
        node = child(node, 0);
    }
    unsigned after = run->taken[depth - 1] + 1u;
    if (after < parent->count) {
        const char *leaf = (const char *)child(parent, after);
        for (size_t at = 0; at < list->layout.leaf_size; at += 64) __builtin_prefetch(leaf + at);
    }
    run->path[depth] = node;
    run->depth = depth;
    run->start = start;
    open_run(&list->layout, node, run);
    return true;
}

/* How two cells of a field order: numbers and integers by value (-0 and +0
 * are one key), texts by their bytes. */
static int order(const Layout *l, unsigned field, PureCell a, PureCell b) {
    if (l->kinds[field] == PURE_CELL_INTEGER) return (a.i64 > b.i64) - (a.i64 < b.i64);
    if (!pure_cell_text(l->kinds[field])) return (a.num > b.num) - (a.num < b.num);
    size_t shorter = a.size < b.size ? a.size : b.size;
    int bytes = shorter ? memcmp(a.text, b.text, shorter) : 0;
    return bytes ? (bytes > 0) - (bytes < 0) : (a.size > b.size) - (a.size < b.size);
}

/* Down the child whose first item's key is the last at or below key (the
 * first child when none is), then the leaf's first item not below it. The
 * list's own key field is read from each node's guide; another field from
 * the children's first leaves. */
size_t pure_list_seek(const PureList *list, unsigned field, PureCell key, bool *found) {
    const Layout *l = &list->layout;
    const Node *node = list->root;
    size_t start = 0;
    *found = false;
    if (!node || !list->count) return 0;
    if ((int)field == l->key) {
        /* A leaf a finger keeps whose keys reach round key holds its place
         * (a slice's high bound is near its low one, found just before). */
        for (unsigned i = 0; i < FINGERS; ++i) {
            const Finger *f = &fingers[i];
            if (!f->leaf || f->id != list->id || f->version != list->version || !f->leaf->count) continue;
            const double *keys = numbers(l, f->leaf, field);
            if (!(keys[0] <= key.num && key.num <= keys[f->leaf->count - 1])) continue;
            unsigned low = lower(keys, f->leaf->count, key.num);
            *found = low < f->leaf->count && keys[low] == key.num;
            return f->start + low;
        }
        /* Each level: the last child whose lead is at or below key (the
         * first when none is); then the leaf's items below key. */
        while (!node->leaf) {
            const Guide *g = guide(node);
            unsigned taken = child_by_key(node, key.num);
            start += g->before[taken];
            node = child(node, taken);
        }
        const double *keys = numbers(l, node, field);
        if (list->count > PREFETCHED) /* leaves past the caches: the leaf's keys fetched at once, not probe by probe */
            for (unsigned i = 0; i < node->count; i += 8) __builtin_prefetch(keys + i);
        unsigned low = lower(keys, node->count, key.num);
        *found = low < node->count && keys[low] == key.num;
        finger_keep(list, node, start); /* the item found is read there next */
        return start + low;
    }
    while (!node->leaf) {
        const Guide *g = guide(node);
        unsigned low = 1, high = node->count;
        while (low < high) {
            unsigned middle = low + (high - low) / 2;
            if (order(l, field, cell_at(l, child(node, middle)->first, 0, field), key) <= 0) low = middle + 1;
            else high = middle;
        }
        start += g->before[low - 1];
        node = child(node, low - 1);
    }
    unsigned low = 0, high = node->count;
    while (low < high) {
        unsigned middle = low + (high - low) / 2;
        if (order(l, field, cell_at(l, node, middle, field), key) < 0) low = middle + 1;
        else high = middle;
    }
    *found = low < node->count && !order(l, field, cell_at(l, node, low, field), key);
    return start + low;
}

/* An item where it is: its leaf and its place there. */
typedef struct { const Node *leaf; unsigned index; } Ref;

static PureCell ref_cell(const Layout *l, Ref r, unsigned field) { return cell_at(l, r.leaf, r.index, field); }

/* Positions sorted by their items' field, stably (spare holds count). */
PURE_COLD static void merge_sort(const Layout *l, unsigned field, const Ref *items, size_t *at, size_t *spare,
                                 size_t count) {
    if (count < 2) return;
    size_t half = count / 2, i = 0, j = half, k = 0;
    merge_sort(l, field, items, at, spare, half);
    merge_sort(l, field, items, at + half, spare, count - half);
    while (i < half && j < count)
        spare[k++] = order(l, field, ref_cell(l, items[at[j]], field), ref_cell(l, items[at[i]], field)) < 0 ? at[j++] : at[i++];
    while (i < half) spare[k++] = at[i++];
    while (j < count) spare[k++] = at[j++];
    memcpy(at, spare, count * sizeof(*at));
}

typedef struct { Ref *items; size_t count; } Gathered;

static bool gather_leaf(void *context, const Node *leaf) {
    Gathered *g = context;
    for (unsigned i = 0; i < leaf->count; ++i) g->items[g->count++] = (Ref){leaf, i};
    return true;
}

PURE_COLD PureResult pure_list_sorted(const PureList *list, unsigned field, PureList **out, size_t *moved) {
    const Layout *l = &list->layout;
    *out = NULL;
    Gathered gathered = {list->count ? malloc(list->count * sizeof(Ref)) : NULL, 0};
    if (list->count && !gathered.items) return PURE_ALLOCATION;
    visit_leaves(list->root, gather_leaf, &gathered);
    const Ref *items = gathered.items;
    bool ordered = true;
    for (size_t i = 1; i < list->count && ordered; ++i)
        ordered = order(l, field, ref_cell(l, items[i - 1], field), ref_cell(l, items[i], field)) < 0;
    if (ordered) {
        free(gathered.items);
        return PURE_OK;
    }
    size_t *at = malloc(2 * list->count * sizeof(*at));
    if (!at) {
        free(gathered.items);
        return PURE_ALLOCATION;
    }
    for (size_t i = 0; i < list->count; ++i) at[i] = i;
    merge_sort(l, field, items, at, at + list->count, list->count);
    PureResult result = PURE_OK;
    for (size_t i = 1; i < list->count && result == PURE_OK; ++i)
        if (!order(l, field, ref_cell(l, items[at[i - 1]], field), ref_cell(l, items[at[i]], field))) result = PURE_CONFLICT;
    PureList *sorted = result == PURE_OK ? pure_list_new(l->fields, l->kinds) : NULL;
    if (result == PURE_OK && !sorted) result = PURE_ALLOCATION;
    for (size_t i = 0; result == PURE_OK && i < list->count; ++i) {
        PureCell cells[16];
        for (unsigned f = 0; f < l->fields; ++f) cells[f] = ref_cell(l, items[at[i]], f);
        result = pure_list_insert(sorted, i, cells, NULL);
        moved[at[i]] = i;
    }
    free(at);
    free(gathered.items);
    if (result != PURE_OK) {
        pure_list_free(sorted);
        return result;
    }
    *out = sorted;
    return PURE_OK;
}

/* Undoing needs no memory but for a node an item goes back into (the event
 * took one out and the tree was since reshaped): without it, the process
 * stops rather than keep half an event (the store keeps the last save). The
 * texts undone were made during the event: no copy reads them. */
void pure_undo_rollback(PureUndo *undo, size_t mark) {
    Entry *entries = undo->entries;
    for (size_t i = undo->count; i-- > mark;) {
        Entry *e = &entries[i];
        PureList *list = e->list;
        const Layout *l = &list->layout;
        Step path[DEPTH];
        unsigned within, depth;
        if (e->change == REMOVED) {
            Place p;
            if (!prepare(list, e->at, &p)) abort();
            list->bytes += cells_bytes(l, undo->cells + e->cells);
            place(list, &p, undo->cells + e->cells);
            undo->cell_count = e->cells;
            continue;
        }
        if (!(depth = descend(list, e->at, path, &within))) abort();
        Node *leaf = path[depth - 1].node;
        if (e->change == INSERTED) {
            list->bytes -= item_bytes(l, leaf, within);
            for (unsigned t = 0; t < l->texts; ++t) free_text(texts(l, leaf, l->text_field[t])[within].data);
            take_out(list, path, depth, within);
            continue;
        }
        list->bytes += cell_bytes(l, e->field, e->old) - cell_bytes(l, e->field, cell_at(l, leaf, within, e->field));
        if (pure_cell_text(l->kinds[e->field])) {
            PureSpan *text = &texts(l, leaf, e->field)[within];
            free_text(text->data);
            *text = (PureSpan){e->old.text, e->old.size};
        } else if (l->kinds[e->field] == PURE_CELL_INTEGER)
            integers(l, leaf, e->field)[within] = e->old.i64;
        else {
            double now = numbers(l, leaf, e->field)[within];
            numbers(l, leaf, e->field)[within] = e->old.num;
            if (list->members && (int)e->field == l->key) member_remove(list, now), member_add(list, e->old.num);
        }
        sum_path(list, path, depth);
        ++list->version;
    }
    undo->count = mark;
}

void pure_undo_commit(PureUndo *undo) {
    Entry *entries = undo->entries;
    for (size_t i = 0; i < undo->count; ++i) {
        Entry *e = &entries[i];
        if (e->change == REMOVED) let_go_cells(e->list, undo->cells + e->cells);
        else if (e->change == CHANGED) let_go_text(e->list, e->old.text);
    }
    undo->count = undo->cell_count = 0;
}

size_t pure_undo_size(const PureUndo *undo) { return undo->count * sizeof(Entry) + undo->cell_count * sizeof(PureCell); }

void pure_undo_free(PureUndo *undo) {
    free(undo->entries);
    free(undo->cells);
    *undo = (PureUndo){0};
}
