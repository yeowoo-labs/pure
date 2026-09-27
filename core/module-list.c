#include "module-list.h"
#include "module-text.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* A list's items live in a persistent B+tree: leaves hold up to FANOUT
 * items, inner nodes up to FANOUT children, and every node knows how many
 * items lie below it, their stored bytes and the first of them. So an item
 * is found by position, a keyed list is searched by key and an item's place
 * in the stored form is computed in O(log n), whatever the list's length; a
 * run of positions is read through a finger (the last leaf a thread read).
 *
 * Nodes, like items, carry the list's share count when they were made. One
 * made before the last share may be read by a shared copy (a snapshot), so
 * it never changes again: the list copies the nodes on the way to a change
 * (the path) and retires the ones it replaced, as it does items. A share is
 * then a root and a count, however long the list. */
enum { FANOUT = 32, DEPTH = 16, SPARE_NODES = 8 };

typedef struct Node {
    uint64_t made;       /* the list's share count when made: shared once below it */
    uint32_t count;      /* items (a leaf) or children */
    bool leaf;
    size_t items, bytes; /* items below, and their stored bytes */
    PureCell *first;     /* the first item below; NULL when there is none */
    void *slot[FANOUT];  /* a leaf's items (PureCell *), an inner node's children (Node *) */
} Node;

struct PureList {
    unsigned fields;
    uint8_t text_fields[16];
    Node *root;           /* NULL when no item was ever placed */
    size_t count;
    PureDraft *draft;     /* the stored copy, or NULL */
    size_t header;        /* draft bytes before the first item */
    uint64_t id, version; /* which list, and a number every change moves on */
    uint64_t shares;      /* copies made so far: an item or node made before the last one is shared */
    PureRetired *retired; /* where shared items and nodes go when the list lets them go */
    Node *spare;          /* nodes let go that no copy read, kept for the next ones (linked by slot[0]) */
    unsigned spares;
    unsigned refs;        /* a shared copy's references */
    bool copy;            /* a shared copy: it owns none of its nodes or items */
};

typedef enum { INSERTED, REMOVED, CHANGED, REPLACED } Change;
typedef struct {
    Change change;
    PureList *list;
    size_t at;
    unsigned field;
    PureCell *item; /* REMOVED: the removed item; REPLACED: the shared item a copy replaced */
    PureCell old;   /* CHANGED: the replaced cell */
} Entry;
typedef struct { void *pointer; uint16_t text; uint8_t fields; bool node; } Retired;

static uint64_t lists_made;

/* The leaf a thread read last, and where it starts: a list read position by
 * position (a view's rows, a fold) finds each next item there. It holds for
 * the list (its id) as it was (its version) and no other. */
static _Thread_local struct { uint64_t id, version; const Node *leaf; size_t start; } finger;

/* An item's cells follow the list's share count when the item was made. One
 * made before the last share may be read by a shared copy, so it never
 * changes again (a change copies it) and is retired instead of freed. */
static PureCell *new_item(const PureList *list) {
    uint64_t *made = calloc(1, sizeof(*made) + (list->fields ? list->fields : 1) * sizeof(PureCell));
    if (!made) return NULL;
    *made = list->shares;
    return (PureCell *)(made + 1);
}

static bool shared(const PureList *list, const PureCell *item) { return ((const uint64_t *)item)[-1] < list->shares; }
static bool shared_node(const PureList *list, const Node *node) { return node->made < list->shares; }
static Node *child(const Node *node, unsigned i) { return node->slot[i]; }
static PureCell *item_in(const Node *leaf, unsigned i) { return leaf->slot[i]; }

static void free_cells(PureCell *item, unsigned fields, unsigned text) {
    for (unsigned f = 0; f < fields; ++f)
        if (text >> f & 1) free(item[f].text);
    free((uint64_t *)item - 1);
}

static unsigned text_mask(const PureList *list) {
    unsigned text = 0;
    for (unsigned f = 0; f < list->fields; ++f) text |= (unsigned)(list->text_fields[f] != 0) << f;
    return text;
}

static void free_item(const PureList *list, PureCell *item) {
    if (item) free_cells(item, list->fields, text_mask(list));
}

/* Kept until the copies that may read it are gone (pure_retired_free). */
static void retire(PureList *list, void *pointer, bool node) {
    PureRetired *retired = list->retired;
    if (retired->count == retired->capacity) {
        size_t capacity = retired->capacity ? 2 * retired->capacity : 16;
        Retired *grown = realloc(retired->entries, capacity * sizeof(*grown));
        if (!grown) return; /* kept for good rather than freed while read */
        retired->entries = grown;
        retired->capacity = capacity;
    }
    ((Retired *)retired->entries)[retired->count++] =
        (Retired){pointer, node ? 0 : (uint16_t)text_mask(list), node ? 0 : (uint8_t)list->fields, node};
}

/* The list no longer has item: free it, or retire it while a copy may read it. */
static void let_go(PureList *list, PureCell *item) {
    if (!shared(list, item)) free_item(list, item);
    else retire(list, item, false);
}

void pure_retired_free(PureRetired *retired) {
    Retired *entries = retired->entries;
    for (size_t i = 0; i < retired->count; ++i) {
        if (entries[i].node) free(entries[i].pointer);
        else free_cells(entries[i].pointer, entries[i].fields, entries[i].text);
    }
    free(entries);
    *retired = (PureRetired){0};
}

static Node *new_node(PureList *list, bool leaf) {
    Node *node = list->spare;
    if (node) {
        list->spare = node->slot[0];
        --list->spares;
    } else if (!(node = malloc(sizeof(*node))))
        return NULL;
    node->made = list->shares;
    node->count = 0;
    node->leaf = leaf;
    node->items = node->bytes = 0;
    node->first = NULL;
    return node;
}

/* A node the list no longer has (its items and children dealt with):
 * retired while a copy may read it, else kept for the next or freed. */
static void drop_node(PureList *list, Node *node) {
    if (shared_node(list, node)) retire(list, node, true);
    else if (list->spares < SPARE_NODES) {
        node->slot[0] = list->spare;
        list->spare = node;
        ++list->spares;
    } else
        free(node);
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

/* A node's summary, from its items or children. */
static void sum(const PureList *list, Node *node) {
    size_t items = 0, bytes = 0;
    for (unsigned i = 0; i < node->count; ++i) {
        if (node->leaf) ++items, bytes += item_bytes(list, item_in(node, i));
        else items += child(node, i)->items, bytes += child(node, i)->bytes;
    }
    node->items = items;
    node->bytes = bytes;
    node->first = !node->count ? NULL : node->leaf ? item_in(node, 0) : child(node, 0)->first;
}

/* ---- Finding positions */

typedef struct { Node *node; unsigned index; } Step; /* a node on the way, and the child taken */

/* The leaf holding position at (the end is in the last leaf), and at's
 * place in it. */
static const Node *leaf_of(const PureList *list, size_t at, size_t *within) {
    if (finger.leaf && finger.id == list->id && finger.version == list->version && at >= finger.start &&
        at < finger.start + finger.leaf->count) {
        *within = at - finger.start;
        return finger.leaf;
    }
    const Node *node = list->root;
    size_t start = 0;
    while (!node->leaf) {
        unsigned i = 0;
        while (i + 1 < node->count && at - start >= child(node, i)->items) start += child(node, i)->items, ++i;
        node = child(node, i);
    }
    finger.id = list->id, finger.version = list->version, finger.leaf = node, finger.start = start;
    *within = at - start;
    return node;
}

/* Where an item starts in the draft: the bytes of the children before it,
 * level by level. */
static size_t item_offset(const PureList *list, size_t at) {
    size_t offset = list->header;
    const Node *node = list->root;
    if (!node) return offset;
    while (!node->leaf) {
        unsigned i = 0;
        while (i + 1 < node->count && at >= child(node, i)->items) {
            at -= child(node, i)->items;
            offset += child(node, i)->bytes;
            ++i;
        }
        node = child(node, i);
    }
    for (unsigned i = 0; i < at && i < node->count; ++i) offset += item_bytes(list, item_in(node, i));
    return offset;
}

/* The node at *place, the list's own to change: a copy (the node retired)
 * while a shared copy of the list may read it. NULL without memory. A copy
 * moves the version on even when the change it is for does not happen: the
 * list no longer has the nodes its shared copies have. */
static Node *own(PureList *list, Node **place) {
    Node *node = *place;
    if (!shared_node(list, node)) return node;
    Node *mine = new_node(list, node->leaf);
    if (!mine) return NULL;
    uint64_t made = mine->made;
    memcpy(mine, node, sizeof(*mine));
    mine->made = made;
    retire(list, node, true);
    *place = mine;
    ++list->version;
    return mine;
}

/* The path to the leaf holding position at, each node the list's own, and
 * at's place in the leaf: the number of steps, 0 without memory (the list
 * is as it was: copies replace what they copy). */
static unsigned descend(PureList *list, size_t at, Step path[DEPTH], unsigned *within) {
    Node **place = &list->root;
    for (unsigned depth = 0; depth < DEPTH; ++depth) {
        Node *node = own(list, place);
        if (!node) return 0;
        if (node->leaf) {
            path[depth] = (Step){node, 0};
            *within = (unsigned)at;
            return depth + 1;
        }
        unsigned i = 0;
        while (i + 1 < node->count && at >= child(node, i)->items) at -= child(node, i)->items, ++i;
        path[depth] = (Step){node, i};
        place = (Node **)&node->slot[i];
    }
    return 0;
}

static void sum_path(const PureList *list, const Step *path, unsigned depth) {
    for (unsigned d = depth; d-- > 0;) sum(list, path[d].node);
}

/* ---- Placing and removing */

/* A place for an item: the path to it, and the nodes a split may take (one
 * a level and a root), made before anything changes so that placing cannot
 * fail. */
typedef struct {
    Step path[DEPTH];
    unsigned depth, within, extras, used;
    Node *extra[DEPTH + 1];
} Place;

static bool prepare(PureList *list, size_t at, Place *p) {
    if (!list->root && !(list->root = new_node(list, true))) return false;
    p->extras = p->used = 0;
    if (!(p->depth = descend(list, at, p->path, &p->within))) return false;
    if (p->path[p->depth - 1].node->count < FANOUT) return true;
    for (; p->extras <= p->depth; ++p->extras)
        if (!(p->extra[p->extras] = new_node(list, false))) {
            while (p->extras) drop_node(list, p->extra[--p->extras]);
            return false;
        }
    return true;
}

static void release_extras(PureList *list, Place *p) {
    while (p->extras > p->used) drop_node(list, p->extra[--p->extras]);
}

/* pointer into the node at the end of the path, before index: a full node
 * splits in two halves and the new half goes into its parent, up to a new
 * root. */
static void place(PureList *list, Place *p, void *pointer) {
    unsigned index = p->within;
    for (unsigned d = p->depth; d-- > 0;) {
        Node *node = p->path[d].node;
        if (node->count < FANOUT) {
            memmove(&node->slot[index + 1], &node->slot[index], (node->count - index) * sizeof(void *));
            node->slot[index] = pointer;
            ++node->count;
            sum_path(list, p->path, d + 1);
            break;
        }
        void *all[FANOUT + 1];
        memcpy(all, node->slot, index * sizeof(void *));
        all[index] = pointer;
        memcpy(all + index + 1, node->slot + index, (FANOUT - index) * sizeof(void *));
        Node *right = p->extra[p->used++];
        right->leaf = node->leaf;
        node->count = (FANOUT + 1) / 2;
        right->count = FANOUT + 1 - node->count;
        memcpy(node->slot, all, node->count * sizeof(void *));
        memcpy(right->slot, all + node->count, right->count * sizeof(void *));
        sum(list, node);
        sum(list, right);
        if (!d) { /* the root split: a new one above both halves */
            Node *root = p->extra[p->used++];
            root->leaf = false;
            root->slot[0] = node;
            root->slot[1] = right;
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

/* The item at the leaf's within, out of the tree (the caller keeps it): an
 * emptied node leaves its parent, a node that fits with a neighbour merges
 * into one (when the neighbour can be made the list's own), and a root with
 * one child gives way to it. */
static void take_out(PureList *list, Step *path, unsigned depth, unsigned within) {
    Node *leaf = path[depth - 1].node;
    memmove(&leaf->slot[within], &leaf->slot[within + 1], (leaf->count - within - 1) * sizeof(void *));
    --leaf->count;
    unsigned d = depth - 1;
    for (;; --d) {
        Node *node = path[d].node;
        sum(list, node);
        if (!d) break;
        Node *parent = path[d - 1].node;
        unsigned at = path[d - 1].index;
        if (!node->count) {
            memmove(&parent->slot[at], &parent->slot[at + 1], (parent->count - at - 1) * sizeof(void *));
            --parent->count;
            drop_node(list, node);
            continue;
        }
        unsigned other = at + 1 < parent->count ? at + 1 : at ? at - 1 : at;
        Node *neighbour = other != at && node->count + child(parent, other)->count <= FANOUT
                              ? own(list, (Node **)&parent->slot[other]) : NULL;
        if (!neighbour) break;
        Node *left = other < at ? neighbour : node, *right = other < at ? node : neighbour;
        unsigned left_at = other < at ? other : at;
        memcpy(&left->slot[left->count], right->slot, right->count * sizeof(void *));
        left->count += right->count;
        sum(list, left);
        memmove(&parent->slot[left_at + 1], &parent->slot[left_at + 2], (parent->count - left_at - 2) * sizeof(void *));
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

PureList *pure_list_new(unsigned fields, const uint8_t *text_fields) {
    if (fields > 16) return NULL;
    PureList *list = calloc(1, sizeof(*list));
    if (!list) return NULL;
    list->fields = fields;
    memcpy(list->text_fields, text_fields, fields);
    list->id = __atomic_add_fetch(&lists_made, 1, __ATOMIC_RELAXED);
    return list;
}

/* Every node and item below node let go (retired where a copy may read them). */
static void let_go_tree(PureList *list, Node *node) {
    if (!node) return;
    for (unsigned i = 0; i < node->count; ++i) {
        if (node->leaf) let_go(list, item_in(node, i));
        else let_go_tree(list, child(node, i));
    }
    if (shared_node(list, node)) retire(list, node, true);
    else free(node);
}

void pure_list_free(PureList *list) {
    if (!list) return;
    if (list->copy) {
        if (__atomic_sub_fetch(&list->refs, 1, __ATOMIC_ACQ_REL)) return;
    } else {
        let_go_tree(list, list->root);
        while (list->spare) {
            Node *next = list->spare->slot[0];
            free(list->spare);
            list->spare = next;
        }
    }
    free(list);
}

size_t pure_list_count(const PureList *list) { return list->count; }

PureList *pure_list_share(PureList *list, PureRetired *retired) {
    PureList *copy = malloc(sizeof(*copy));
    if (!copy) return NULL;
    *copy = (PureList){.fields = list->fields, .root = list->root, .count = list->count, .id = list->id,
                       .version = list->version, .refs = 1, .copy = true};
    memcpy(copy->text_fields, list->text_fields, sizeof(copy->text_fields));
    list->retired = retired;
    ++list->shares;
    return copy;
}

void pure_list_hold(PureList *copy) { __atomic_add_fetch(&copy->refs, 1, __ATOMIC_RELAXED); }

bool pure_list_current(const PureList *copy, const PureList *list) {
    return copy->id == list->id && copy->version == list->version;
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

static bool copy_cell(const PureList *list, unsigned field, PureCell from, PureCell *to) {
    *to = (PureCell){.num = list->text_fields[field] ? 0 : from.num};
    if (!list->text_fields[field] || !from.size) return true;
    to->text = malloc(from.size);
    if (!to->text) return false;
    memcpy(to->text, from.text, from.size);
    to->size = from.size;
    return true;
}

static unsigned char *encode_cell(const PureList *list, unsigned field, PureCell cell, unsigned char *out) {
    uint64_t bits = cell.size;
    unsigned width = 4;
    if (!list->text_fields[field]) { memcpy(&bits, &cell.num, sizeof(bits)); width = 8; }
    for (unsigned i = 0; i < width; ++i) *out++ = (unsigned char)(bits >> (8 * i));
    if (list->text_fields[field] && cell.size) { memcpy(out, cell.text, cell.size); out += cell.size; }
    return out;
}

/* Replace remove bytes of the draft, within bytes into the item at `at` (or
 * after the last item), with bytes; nothing to do without a draft. */
static PureResult mirror(PureList *list, size_t at, size_t within, size_t remove, const unsigned char *bytes,
                         size_t size) {
    if (!list->draft) return PURE_OK;
    return pure_draft_splice(list->draft, item_offset(list, at) + within, remove, (PureSpan){bytes, size});
}

void pure_list_mirror(PureList *list, PureDraft *draft, size_t header) {
    list->draft = draft;
    list->header = header;
}

/* Every item in order, to visit (false stops it). */
static bool visit_items(const Node *node, bool (*visit)(void *, PureCell *), void *context) {
    for (unsigned i = 0; node && i < node->count; ++i)
        if (node->leaf ? !visit(context, item_in(node, i)) : !visit_items(child(node, i), visit, context)) return false;
    return true;
}

typedef struct { const PureList *list; unsigned char *at; } Encoding;

static bool encode_item(void *context, PureCell *item) {
    Encoding *e = context;
    for (unsigned f = 0; f < e->list->fields; ++f) e->at = encode_cell(e->list, f, item[f], e->at);
    return true;
}

PURE_COLD PureResult pure_list_write(PureList *list) {
    size_t size = list->root ? list->root->bytes : 0;
    unsigned char *bytes = size ? malloc(size) : NULL;
    if (size && !bytes) return PURE_ALLOCATION;
    Encoding encoding = {list, bytes};
    visit_items(list->root, encode_item, &encoding);
    PureResult result = mirror(list, 0, 0, 0, bytes, size);
    free(bytes);
    return result;
}

PURE_COLD PureResult pure_list_read(PureList *list, const unsigned char *bytes, size_t size) {
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

PureResult pure_list_insert(PureList *list, size_t at, const PureCell *cells, PureUndo *undo) {
    if (at > list->count || list->count == PURE_LIST_ITEMS) return PURE_LIMIT;
    PureCell *item = new_item(list);
    if (!item) return PURE_ALLOCATION;
    for (unsigned f = 0; f < list->fields; ++f)
        if (!copy_cell(list, f, cells[f], &item[f])) { free_item(list, item); return PURE_ALLOCATION; }
    size_t size = list->draft ? item_bytes(list, item) : 0;
    unsigned char *bytes = size ? malloc(size) : NULL, *end = bytes;
    Place p;
    if ((size && !bytes) || !prepare(list, at, &p)) {
        free(bytes);
        free_item(list, item);
        return PURE_ALLOCATION;
    }
    for (unsigned f = 0; bytes && f < list->fields; ++f) end = encode_cell(list, f, item[f], end);
    if (undo && !log_change(undo, (Entry){.change = INSERTED, .list = list, .at = at})) {
        release_extras(list, &p);
        free(bytes);
        free_item(list, item);
        return PURE_ALLOCATION;
    }
    PureResult result = mirror(list, at, 0, 0, bytes, size);
    free(bytes);
    if (result != PURE_OK) {
        if (undo) --undo->count;
        release_extras(list, &p);
        free_item(list, item);
        return result;
    }
    place(list, &p, item);
    return PURE_OK;
}

PureResult pure_list_remove(PureList *list, size_t at, PureUndo *undo) {
    if (at >= list->count) return PURE_NOT_FOUND;
    Step path[DEPTH];
    unsigned within, depth = descend(list, at, path, &within);
    if (!depth) return PURE_ALLOCATION;
    PureCell *item = item_in(path[depth - 1].node, within);
    if (!log_change(undo, (Entry){.change = REMOVED, .list = list, .at = at, .item = item})) return PURE_ALLOCATION;
    PureResult result = mirror(list, at, 0, item_bytes(list, item), NULL, 0);
    if (result != PURE_OK) { --undo->count; return result; }
    take_out(list, path, depth, within);
    return PURE_OK;
}

/* A shared item's copy with every field but one (left empty). */
static PureCell *copy_item(const PureList *list, const PureCell *item, unsigned except) {
    PureCell *fresh = new_item(list);
    for (unsigned f = 0; fresh && f < list->fields; ++f)
        if (f != except && !copy_cell(list, f, item[f], &fresh[f])) {
            free_item(list, fresh);
            fresh = NULL;
        }
    return fresh;
}

PureResult pure_list_set(PureList *list, size_t at, unsigned field, PureCell value, PureUndo *undo) {
    if (at >= list->count) return PURE_NOT_FOUND;
    Step path[DEPTH];
    unsigned within, depth = descend(list, at, path, &within);
    PureCell copy;
    if (!depth || !copy_cell(list, field, value, &copy)) return PURE_ALLOCATION;
    Node *leaf = path[depth - 1].node;
    PureCell *item = item_in(leaf, within);
    PureCell *fresh = shared(list, item) ? copy_item(list, item, field) : item;
    size_t size = list->draft ? cell_bytes(list, field, copy) : 0;
    unsigned char *bytes = size ? malloc(size) : NULL;
    Entry entry = fresh == item ? (Entry){.change = CHANGED, .list = list, .at = at, .field = field, .old = item[field]} :
                                  (Entry){.change = REPLACED, .list = list, .at = at, .item = item};
    if (!fresh || (size && !bytes) || !log_change(undo, entry)) {
        free(bytes);
        free(copy.text);
        if (fresh != item) free_item(list, fresh);
        return PURE_ALLOCATION;
    }
    if (bytes) encode_cell(list, field, copy, bytes);
    size_t offset = 0;
    for (unsigned f = 0; f < field; ++f) offset += cell_bytes(list, f, item[f]);
    PureResult result = mirror(list, at, offset, cell_bytes(list, field, item[field]), bytes, size);
    free(bytes);
    if (result != PURE_OK) {
        --undo->count;
        free(copy.text);
        if (fresh != item) free_item(list, fresh);
        return result;
    }
    fresh[field] = copy;
    leaf->slot[within] = fresh;
    sum_path(list, path, depth);
    ++list->version;
    return PURE_OK;
}

PureCell pure_list_get(const PureList *list, size_t at, unsigned field) {
    size_t within;
    return item_in(leaf_of(list, at, &within), (unsigned)within)[field];
}

/* How two cells of a field order: numbers by value (-0 and +0 are one key),
 * texts by their bytes. */
static int order(const PureList *list, unsigned field, PureCell a, PureCell b) {
    if (!list->text_fields[field]) return (a.num > b.num) - (a.num < b.num);
    size_t shorter = a.size < b.size ? a.size : b.size;
    int bytes = shorter ? memcmp(a.text, b.text, shorter) : 0;
    return bytes ? (bytes > 0) - (bytes < 0) : (a.size > b.size) - (a.size < b.size);
}

/* Down the child whose first item's key is the last at or below key (the
 * first child when none is), then the leaf's first item not below it. */
size_t pure_list_seek(const PureList *list, unsigned field, PureCell key, bool *found) {
    const Node *node = list->root;
    size_t start = 0;
    *found = false;
    if (!node || !list->count) return 0;
    while (!node->leaf) {
        unsigned low = 1, high = node->count;
        while (low < high) {
            unsigned middle = low + (high - low) / 2;
            if (order(list, field, child(node, middle)->first[field], key) <= 0) low = middle + 1;
            else high = middle;
        }
        for (unsigned i = 0; i + 1 < low; ++i) start += child(node, i)->items;
        node = child(node, low - 1);
    }
    unsigned low = 0, high = node->count;
    while (low < high) {
        unsigned middle = low + (high - low) / 2;
        if (order(list, field, item_in(node, middle)[field], key) < 0) low = middle + 1;
        else high = middle;
    }
    *found = low < node->count && !order(list, field, item_in(node, low)[field], key);
    return start + low;
}

/* Positions sorted by their items' field, stably (spare holds count). */
PURE_COLD static void merge_sort(const PureList *list, unsigned field, PureCell *const *items, size_t *at, size_t *spare,
                                 size_t count) {
    if (count < 2) return;
    size_t half = count / 2, i = 0, j = half, k = 0;
    merge_sort(list, field, items, at, spare, half);
    merge_sort(list, field, items, at + half, spare, count - half);
    while (i < half && j < count)
        spare[k++] = order(list, field, items[at[j]][field], items[at[i]][field]) < 0 ? at[j++] : at[i++];
    while (i < half) spare[k++] = at[i++];
    while (j < count) spare[k++] = at[j++];
    memcpy(at, spare, count * sizeof(*at));
}

typedef struct { PureCell **items; size_t count; } Gathered;

static bool gather_item(void *context, PureCell *item) {
    Gathered *g = context;
    g->items[g->count++] = item;
    return true;
}

PURE_COLD PureResult pure_list_sorted(const PureList *list, unsigned field, PureList **out, size_t *moved) {
    *out = NULL;
    Gathered gathered = {list->count ? malloc(list->count * sizeof(PureCell *)) : NULL, 0};
    if (list->count && !gathered.items) return PURE_ALLOCATION;
    visit_items(list->root, gather_item, &gathered);
    PureCell **items = gathered.items;
    bool ordered = true;
    for (size_t i = 1; i < list->count && ordered; ++i) ordered = order(list, field, items[i - 1][field], items[i][field]) < 0;
    if (ordered) {
        free(items);
        return PURE_OK;
    }
    size_t *at = malloc(2 * list->count * sizeof(*at));
    if (!at) {
        free(items);
        return PURE_ALLOCATION;
    }
    for (size_t i = 0; i < list->count; ++i) at[i] = i;
    merge_sort(list, field, items, at, at + list->count, list->count);
    PureResult result = PURE_OK;
    for (size_t i = 1; i < list->count && result == PURE_OK; ++i)
        if (!order(list, field, items[at[i - 1]][field], items[at[i]][field])) result = PURE_CONFLICT;
    PureList *sorted = result == PURE_OK ? pure_list_new(list->fields, list->text_fields) : NULL;
    if (result == PURE_OK && !sorted) result = PURE_ALLOCATION;
    for (size_t i = 0; result == PURE_OK && i < list->count; ++i) {
        result = pure_list_insert(sorted, i, items[at[i]], NULL);
        moved[at[i]] = i;
    }
    free(at);
    free(items);
    if (result != PURE_OK) {
        pure_list_free(sorted);
        return result;
    }
    *out = sorted;
    return PURE_OK;
}

/* Undoing needs no memory but for a node an item goes back into (the event
 * took one out and the tree was since reshaped): without it, the process
 * stops rather than keep half an event (the store keeps the last save). */
void pure_undo_rollback(PureUndo *undo, size_t mark) {
    Entry *entries = undo->entries;
    for (size_t i = undo->count; i-- > mark;) {
        Entry *e = &entries[i];
        PureList *list = e->list;
        Step path[DEPTH];
        unsigned within, depth;
        if (e->change == REMOVED) {
            Place p;
            if (!prepare(list, e->at, &p)) abort();
            place(list, &p, e->item);
            continue;
        }
        if (!(depth = descend(list, e->at, path, &within))) abort();
        Node *leaf = path[depth - 1].node;
        if (e->change == INSERTED) {
            PureCell *item = item_in(leaf, within);
            take_out(list, path, depth, within);
            free_item(list, item);
            continue;
        }
        PureCell *item = item_in(leaf, within);
        if (e->change == REPLACED) {
            free_item(list, item);
            leaf->slot[within] = e->item;
        } else {
            if (list->text_fields[e->field]) free(item[e->field].text);
            item[e->field] = e->old;
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
        if (e->change == REMOVED || e->change == REPLACED) let_go(e->list, e->item);
        else if (e->change == CHANGED && e->list->text_fields[e->field]) free(e->old.text);
    }
    undo->count = 0;
}

void pure_undo_free(PureUndo *undo) {
    free(undo->entries);
    *undo = (PureUndo){0};
}
