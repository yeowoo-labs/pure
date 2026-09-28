#include "module-list.h"
#include "module-text.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* A list's items live in a persistent B+tree: leaves hold up to FANOUT
 * items, inner nodes up to FANOUT children, and every node knows how many
 * items lie below it, their stored bytes and the leaf holding the first of
 * them. So an item is found by position, a keyed list is searched by key and
 * an item's place in the stored form is computed in O(log n), whatever the
 * list's length; a run of positions is read through a finger (the last leaf
 * a thread read) or a leaf at a time (pure_list_run).
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
enum { FANOUT = PURE_LIST_RUN, DEPTH = 16, SPARE_NODES = 8 };

typedef struct Node {
    uint64_t made;            /* the list's share count when made: shared once below it */
    uint32_t count;           /* items (a leaf) or children */
    bool leaf;
    size_t items, bytes;      /* items below, and their stored bytes */
    const struct Node *first; /* the leaf holding the first item below; NULL when there is none */
    struct Node *child[];     /* an inner node's children; a leaf's columns are here instead */
} Node;

enum { INNER_SIZE = sizeof(Node) + FANOUT * sizeof(Node *) };

/* Where a leaf keeps each field's cells, and what an item stores. */
typedef struct {
    unsigned fields, texts;
    uint8_t text_fields[16];
    uint8_t text_field[16]; /* the text fields, in order */
    uint32_t column[16];    /* where each field's cells start in a leaf */
    size_t leaf_size;
    size_t number_bytes;    /* an item's numbers, stored */
} Layout;

struct PureList {
    Layout layout;
    Node *root;           /* NULL when the list has no item */
    size_t count;
    PureDraft *draft;     /* the stored copy, or NULL */
    size_t header;        /* draft bytes before the first item */
    uint64_t id, version; /* which list, and a number every change moves on */
    uint64_t shares;      /* copies made so far: a node or text made before the last one is shared */
    PureRetired *retired; /* where shared nodes and texts go when the list lets them go */
    Node *spare[2];       /* inner nodes and leaves let go that no copy read, kept for the next ones (linked by first) */
    unsigned spares[2];
    unsigned refs;        /* a shared copy's references */
    bool copy;            /* a shared copy: it owns none of its nodes or texts */
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
typedef struct { void *pointer; bool text; } Retired;

static uint64_t lists_made;

/* The leaf a thread read last, and where it starts: a list read position by
 * position (a view's rows, a fold) finds each next item there. It holds for
 * the list (its id) as it was (its version) and no other. */
static _Thread_local struct { uint64_t id, version; const Node *leaf; size_t start; } finger;

static Node *child(const Node *node, unsigned i) { return node->child[i]; }
static double *numbers(const Layout *l, const Node *leaf, unsigned f) { return (double *)((char *)leaf + l->column[f]); }
static PureSpan *texts(const Layout *l, const Node *leaf, unsigned f) { return (PureSpan *)((char *)leaf + l->column[f]); }

static PureCell cell_at(const Layout *l, const Node *leaf, unsigned i, unsigned f) {
    if (!l->text_fields[f]) return (PureCell){numbers(l, leaf, f)[i], NULL, 0};
    PureSpan text = texts(l, leaf, f)[i];
    return (PureCell){0, (unsigned char *)text.data, text.size};
}

static void put_cells(const Layout *l, Node *leaf, unsigned i, const PureCell *cells) {
    for (unsigned f = 0; f < l->fields; ++f)
        if (l->text_fields[f]) texts(l, leaf, f)[i] = (PureSpan){cells[f].text, cells[f].size};
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
        size_t width = l->text_fields[f] ? sizeof(PureSpan) : sizeof(double);
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

/* Kept until the copies that may read it are gone (pure_retired_free). */
static void retire(PureList *list, void *pointer, bool text) {
    PureRetired *retired = list->retired;
    if (retired->count == retired->capacity) {
        size_t capacity = retired->capacity ? 2 * retired->capacity : 16;
        Retired *grown = realloc(retired->entries, capacity * sizeof(*grown));
        if (!grown) return; /* kept for good rather than freed while read */
        retired->entries = grown;
        retired->capacity = capacity;
    }
    ((Retired *)retired->entries)[retired->count++] = (Retired){pointer, text};
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

static Node *new_node(PureList *list, bool leaf) {
    Node *node = list->spare[leaf];
    if (node) {
        list->spare[leaf] = (Node *)node->first;
        --list->spares[leaf];
    } else if (!(node = malloc(leaf ? list->layout.leaf_size : INNER_SIZE)))
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
    if (shared_node(list, node)) retire(list, node, false);
    else if (list->spares[node->leaf] < SPARE_NODES) {
        node->first = list->spare[node->leaf];
        list->spare[node->leaf] = node;
        ++list->spares[node->leaf];
    } else
        free(node);
}

/* ---- The stored form: each field in order, a number as its binary64 bits
 * (little-endian), a text as a u32le length and its bytes. */

static size_t cell_bytes(const Layout *l, unsigned field, PureCell cell) { return l->text_fields[field] ? 4 + cell.size : 8; }

static size_t item_bytes(const Layout *l, const Node *leaf, unsigned i) {
    size_t size = l->number_bytes;
    for (unsigned t = 0; t < l->texts; ++t) size += 4 + texts(l, leaf, l->text_field[t])[i].size;
    return size;
}

/* A node's summary, from its items or children. */
static void sum(const PureList *list, Node *node) {
    const Layout *l = &list->layout;
    size_t items = 0, bytes = 0;
    if (node->leaf) {
        items = node->count;
        bytes = items * l->number_bytes;
        for (unsigned t = 0; t < l->texts; ++t) {
            const PureSpan *column = texts(l, node, l->text_field[t]);
            for (unsigned i = 0; i < node->count; ++i) bytes += 4 + column[i].size;
        }
    } else
        for (unsigned i = 0; i < node->count; ++i) items += child(node, i)->items, bytes += child(node, i)->bytes;
    node->items = items;
    node->bytes = bytes;
    node->first = !node->count ? NULL : node->leaf ? node : child(node, 0)->first;
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
    const Layout *l = &list->layout;
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
    if (!l->texts) return offset + (at < node->count ? at : node->count) * l->number_bytes;
    for (unsigned i = 0; i < at && i < node->count; ++i) offset += item_bytes(l, node, i);
    return offset;
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
        unsigned i = 0;
        while (i + 1 < node->count && at >= child(node, i)->items) at -= child(node, i)->items, ++i;
        path[depth] = (Step){node, i};
        place = &node->child[i];
    }
    return 0;
}

static void sum_path(const PureList *list, const Step *path, unsigned depth) {
    for (unsigned d = depth; d-- > 0;) sum(list, path[d].node);
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

static bool prepare(PureList *list, size_t at, Place *p) {
    if (!list->root && !(list->root = new_node(list, true))) return false;
    p->extras = p->used = 0;
    p->appending = at == list->count;
    if (!(p->depth = descend(list, at, p->path, &p->within))) return false;
    if (p->path[p->depth - 1].node->count < FANOUT) return true;
    for (; p->extras <= p->depth; ++p->extras)
        if (!(p->extra[p->extras] = new_node(list, !p->extras))) {
            while (p->extras) drop_node(list, p->extra[--p->extras]);
            return false;
        }
    return true;
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

PureList *pure_list_new(unsigned fields, const uint8_t *text_fields) {
    if (fields > 16) return NULL;
    PureList *list = calloc(1, sizeof(*list));
    if (!list) return NULL;
    Layout *l = &list->layout;
    l->fields = fields;
    size_t at = sizeof(Node);
    for (unsigned f = 0; f < fields; ++f) {
        l->text_fields[f] = text_fields[f] != 0;
        l->column[f] = (uint32_t)at;
        at += FANOUT * (l->text_fields[f] ? sizeof(PureSpan) : sizeof(double));
        if (l->text_fields[f]) l->text_field[l->texts++] = (uint8_t)f;
        else l->number_bytes += 8;
    }
    l->leaf_size = at;
    list->id = __atomic_add_fetch(&lists_made, 1, __ATOMIC_RELAXED);
    return list;
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
        let_go_tree(list, list->root);
        for (unsigned kind = 0; kind < 2; ++kind)
            while (list->spare[kind]) {
                Node *next = (Node *)list->spare[kind]->first;
                free(list->spare[kind]);
                list->spare[kind] = next;
            }
    }
    free(list);
}

void pure_list_clear(PureList *list) {
    if (!list->count) return;
    let_go_tree(list, list->root);
    list->root = NULL;
    list->count = 0;
    ++list->version;
}

size_t pure_list_count(const PureList *list) { return list->count; }

PureList *pure_list_share(PureList *list, PureRetired *retired) {
    PureList *copy = malloc(sizeof(*copy));
    if (!copy) return NULL;
    *copy = (PureList){.layout = list->layout, .root = list->root, .count = list->count, .id = list->id,
                       .version = list->version, .refs = 1, .copy = true};
    list->retired = retired;
    ++list->shares;
    return copy;
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
    *to = (PureCell){.num = list->layout.text_fields[field] ? 0 : from.num};
    if (!list->layout.text_fields[field] || !from.size) return true;
    if (!(to->text = new_text(list, from.text, from.size))) return false;
    to->size = from.size;
    return true;
}

static unsigned char *encode_cell(const Layout *l, unsigned field, PureCell cell, unsigned char *out) {
    uint64_t bits = cell.size;
    unsigned width = 4;
    if (!l->text_fields[field]) { memcpy(&bits, &cell.num, sizeof(bits)); width = 8; }
    for (unsigned i = 0; i < width; ++i) *out++ = (unsigned char)(bits >> (8 * i));
    if (l->text_fields[field] && cell.size) { memcpy(out, cell.text, cell.size); out += cell.size; }
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

PURE_COLD PureResult pure_list_write(PureList *list) {
    size_t size = list->root ? list->root->bytes : 0;
    unsigned char *bytes = size ? malloc(size) : NULL;
    if (size && !bytes) return PURE_ALLOCATION;
    Encoding encoding = {&list->layout, bytes};
    visit_leaves(list->root, encode_leaf, &encoding);
    PureResult result = mirror(list, 0, 0, 0, bytes, size);
    free(bytes);
    return result;
}

PURE_COLD PureResult pure_list_read(PureList *list, const unsigned char *bytes, size_t size) {
    const Layout *l = &list->layout;
    PureCell cells[16];
    for (size_t at = 0; at < size;) {
        for (unsigned f = 0; f < l->fields; ++f) {
            size_t width = l->text_fields[f] ? 4 : 8;
            if (size - at < width) return PURE_STORE_ERROR;
            uint64_t bits = 0;
            for (size_t i = 0; i < width; ++i) bits |= (uint64_t)bytes[at + i] << (8 * i);
            at += width;
            if (!l->text_fields[f]) {
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
    const Layout *l = &list->layout;
    if (at > list->count || list->count == PURE_LIST_ITEMS) return PURE_LIMIT;
    PureCell item[16];
    unsigned owned = 0;
    for (; owned < l->fields; ++owned)
        if (!own_cell(list, owned, cells[owned], &item[owned])) break;
    size_t size = 0;
    for (unsigned f = 0; list->draft && f < l->fields; ++f) size += cell_bytes(l, f, cells[f]);
    unsigned char *bytes = size ? malloc(size) : NULL, *end = bytes;
    Place p;
    if (owned < l->fields || (size && !bytes) || !prepare(list, at, &p)) {
        free(bytes);
        for (unsigned f = 0; f < owned; ++f) free_text(item[f].text);
        return PURE_ALLOCATION;
    }
    for (unsigned f = 0; bytes && f < l->fields; ++f) end = encode_cell(l, f, item[f], end);
    if (undo && !log_change(undo, (Entry){.change = INSERTED, .list = list, .at = at})) {
        release_extras(list, &p);
        free(bytes);
        for (unsigned f = 0; f < l->fields; ++f) free_text(item[f].text);
        return PURE_ALLOCATION;
    }
    PureResult result = mirror(list, at, 0, 0, bytes, size);
    free(bytes);
    if (result != PURE_OK) {
        if (undo) --undo->count;
        release_extras(list, &p);
        for (unsigned f = 0; f < l->fields; ++f) free_text(item[f].text);
        return result;
    }
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
    PureCell *cells = undo ? log_cells(undo, l->fields) : NULL, kept[16];
    if (undo && (!cells || !log_change(undo, (Entry){.change = REMOVED, .list = list, .at = at, .cells = undo->cell_count})))
        return PURE_ALLOCATION;
    if (!cells) cells = kept;
    for (unsigned f = 0; f < l->fields; ++f) cells[f] = cell_at(l, leaf, within, f);
    PureResult result = mirror(list, at, 0, item_bytes(l, leaf, within), NULL, 0);
    if (result != PURE_OK) {
        if (undo) --undo->count;
        return result;
    }
    if (undo) undo->cell_count += l->fields;
    else let_go_cells(list, cells);
    take_out(list, path, depth, within);
    return PURE_OK;
}

PureResult pure_list_set(PureList *list, size_t at, unsigned field, PureCell value, PureUndo *undo) {
    const Layout *l = &list->layout;
    if (at >= list->count) return PURE_NOT_FOUND;
    Step path[DEPTH];
    unsigned within, depth = descend(list, at, path, &within);
    PureCell copy;
    if (!depth || !own_cell(list, field, value, &copy)) return PURE_ALLOCATION;
    Node *leaf = path[depth - 1].node;
    PureCell old = cell_at(l, leaf, within, field);
    size_t size = list->draft ? cell_bytes(l, field, copy) : 0;
    unsigned char *bytes = size ? malloc(size) : NULL;
    if ((size && !bytes) ||
        (undo && !log_change(undo, (Entry){.change = CHANGED, .list = list, .at = at, .field = field, .old = old}))) {
        free(bytes);
        free_text(copy.text);
        return PURE_ALLOCATION;
    }
    if (bytes) encode_cell(l, field, copy, bytes);
    size_t offset = 0;
    for (unsigned f = 0; f < field; ++f) offset += cell_bytes(l, f, cell_at(l, leaf, within, f));
    PureResult result = mirror(list, at, offset, cell_bytes(l, field, old), bytes, size);
    free(bytes);
    if (result != PURE_OK) {
        if (undo) --undo->count;
        free_text(copy.text);
        return result;
    }
    if (l->text_fields[field]) texts(l, leaf, field)[within] = (PureSpan){copy.text, copy.size};
    else numbers(l, leaf, field)[within] = copy.num;
    if (!undo) let_go_text(list, old.text);
    sum_path(list, path, depth);
    ++list->version;
    return PURE_OK;
}

PureCell pure_list_get(const PureList *list, size_t at, unsigned field) {
    size_t within;
    return cell_at(&list->layout, leaf_of(list, at, &within), (unsigned)within, field);
}

static void open_run(const Layout *l, const Node *leaf, PureRun *run) {
    run->count = leaf->count;
    for (unsigned f = 0; f < l->fields; ++f)
        if (l->text_fields[f]) run->column[f].texts = texts(l, leaf, f);
        else run->column[f].numbers = numbers(l, leaf, f);
}

bool pure_list_run(const PureList *list, size_t at, PureRun *run) {
    if (at >= list->count) return false;
    const Node *node = list->root;
    size_t start = 0;
    unsigned depth = 0;
    while (!node->leaf) {
        unsigned i = 0;
        while (i + 1 < node->count && at - start >= child(node, i)->items) start += child(node, i)->items, ++i;
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

/* How two cells of a field order: numbers by value (-0 and +0 are one key),
 * texts by their bytes. */
static int order(const Layout *l, unsigned field, PureCell a, PureCell b) {
    if (!l->text_fields[field]) return (a.num > b.num) - (a.num < b.num);
    size_t shorter = a.size < b.size ? a.size : b.size;
    int bytes = shorter ? memcmp(a.text, b.text, shorter) : 0;
    return bytes ? (bytes > 0) - (bytes < 0) : (a.size > b.size) - (a.size < b.size);
}

/* Down the child whose first item's key is the last at or below key (the
 * first child when none is), then the leaf's first item not below it. */
size_t pure_list_seek(const PureList *list, unsigned field, PureCell key, bool *found) {
    const Layout *l = &list->layout;
    const Node *node = list->root;
    size_t start = 0;
    *found = false;
    if (!node || !list->count) return 0;
    while (!node->leaf) {
        unsigned low = 1, high = node->count;
        while (low < high) {
            unsigned middle = low + (high - low) / 2;
            if (order(l, field, cell_at(l, child(node, middle)->first, 0, field), key) <= 0) low = middle + 1;
            else high = middle;
        }
        for (unsigned i = 0; i + 1 < low; ++i) start += child(node, i)->items;
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
    PureList *sorted = result == PURE_OK ? pure_list_new(l->fields, l->text_fields) : NULL;
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
            place(list, &p, undo->cells + e->cells);
            undo->cell_count = e->cells;
            continue;
        }
        if (!(depth = descend(list, e->at, path, &within))) abort();
        Node *leaf = path[depth - 1].node;
        if (e->change == INSERTED) {
            for (unsigned t = 0; t < l->texts; ++t) free_text(texts(l, leaf, l->text_field[t])[within].data);
            take_out(list, path, depth, within);
            continue;
        }
        if (l->text_fields[e->field]) {
            PureSpan *text = &texts(l, leaf, e->field)[within];
            free_text(text->data);
            *text = (PureSpan){e->old.text, e->old.size};
        } else
            numbers(l, leaf, e->field)[within] = e->old.num;
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

void pure_undo_free(PureUndo *undo) {
    free(undo->entries);
    free(undo->cells);
    *undo = (PureUndo){0};
}
