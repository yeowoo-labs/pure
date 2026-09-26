/* Selected storage experiment: every document lives in one append-only file as
 * a persistent B-tree of byte extents. An edit writes only the leaves and the
 * path it changes, so inserting or deleting in a large document costs O(log n)
 * nodes instead of a whole-document copy. Unchanged extents are shared by old
 * and new versions. The document catalog is the same structure holding fixed
 * records sorted by key. Two alternating head slots publish a commit only after
 * its nodes are durable; readers take no lock and map one immutable prefix. */
#define _POSIX_C_SOURCE 200809L
#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif
#include "store.h"
#include "state.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

enum { FREE = 0xffff };
enum { SLOT = 64, DATA = 2 * SLOT, LEAF_MAX = 4096, LEAF_MIN = 1024,
       FAN_MAX = 32, FAN_MIN = 8, RECORD = 96, KEY = 64, HEIGHT_MAX = 12,
       BUFFER = 65536 };
#ifdef PURE_STORE_TEST
enum { COMPACT_SLACK = 65536 };
#else
enum { COMPACT_SLACK = 1048576 };
#endif
#define LEAF_BIT (UINT64_C(1) << 63)
#define MEMORY_BIT (UINT64_C(1) << 62)
#define LOCATION (MEMORY_BIT - 1)
static const unsigned char head_magic[8] = {'P','U','R','E','T','R','E','1'};
static const unsigned char branch_magic[4] = {'P','T','B','1'};
static const char tree_name[] = "pure.tree";
/* Threads of one process take turns as the writer: the file lock arbitrates
 * only between processes (it is the process's, and closing any descriptor of
 * the lock file drops it). */
static pthread_mutex_t writers = PTHREAD_MUTEX_INITIALIZER;
static uint64_t temporary_sequence;

/* A reference is a file offset, or an index into this edit's node arena.
 * bytes is the size of the whole subtree. Leaves hold raw document bytes. */
typedef struct { uint64_t ref, bytes; } Entry;
typedef struct { Entry *items; size_t count, capacity; } List;
typedef struct {
    unsigned char *bytes; /* leaf only */
    Entry *children;      /* branch only */
    size_t size;          /* leaf bytes, child count, or next free slot + 1 */
    unsigned height;      /* FREE for a reclaimed slot */
} Node;
/* One mapped version of the file, shared by the drafts of a store handle
 * and by whoever still sends extents from it. */
typedef struct PureMapping { const unsigned char *map; uint64_t end; unsigned refs; } Mapping;
typedef struct {
    const unsigned char *map;
    uint64_t end;
    Mapping *shared; /* map belongs to this shared mapping, not to the tree */
    Node *nodes;
    size_t count, capacity, free_head, free_count; /* free_head is slot + 1 */
    size_t bytes; /* payload held by arena nodes, including unreachable ones */
} Tree;
typedef struct { uint64_t seq, end, catalog, catalog_bytes, live; } Head;
typedef struct {
    int fd;
    uint64_t at; /* file offset of the next byte */
    size_t used;
    bool failed;
    unsigned char buffer[BUFFER];
} Writer;
typedef struct { PureSpan part[3]; size_t index, offset; } Source;

static bool fault(PureStoreStage stage) {
#ifdef PURE_STORE_TEST
    if (pure_store_test_fault(stage)) { errno = EIO; return true; }
#else
    (void)stage;
#endif
    return false;
}

static bool valid_key(const char *key) {
    if (!key) return false;
    size_t length = strlen(key);
    if (!length || length > KEY) return false;
    for (size_t i = 0; i < length; ++i) {
        char b = key[i];
        if (!((b >= 'a' && b <= 'z') || (b >= 'A' && b <= 'Z') ||
              (b >= '0' && b <= '9') || b == '-' || b == '_')) return false;
    }
    return true;
}

/* Runtime-owned values (app states) live under '.' followed by a valid key.
 * Document keys never start with '.', so the two never meet. */
static bool runtime_key(const char *key) {
    return key && key[0] == '.' && strlen(key) <= KEY && valid_key(key + 1);
}

static uint64_t decode64(const unsigned char *bytes) {
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) value |= (uint64_t)bytes[i] << (8 * i);
    return value;
}

static void encode64(unsigned char *bytes, uint64_t value) {
    for (unsigned i = 0; i < 8; ++i) bytes[i] = (unsigned char)(value >> (8 * i));
}

/* Detects a torn head slot; it is not a defence against a hostile file. */
static uint64_t checksum(const unsigned char *bytes, size_t length) {
    uint64_t hash = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < length; ++i) hash = (hash ^ bytes[i]) * UINT64_C(1099511628211);
    return hash;
}

static bool sync_fd(int fd) {
    int rc;
    do { rc = fsync(fd); } while (rc && errno == EINTR);
    return rc == 0;
}

static bool write_at(int fd, const unsigned char *bytes, size_t length, uint64_t offset) {
    while (length) {
        ssize_t n = pwrite(fd, bytes, length, (off_t)offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        bytes += (size_t)n;
        length -= (size_t)n;
        offset += (uint64_t)n;
    }
    return true;
}

static bool read_at(int fd, unsigned char *bytes, size_t length, uint64_t offset) {
    while (length) {
        ssize_t n = pread(fd, bytes, length, (off_t)offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        bytes += (size_t)n;
        length -= (size_t)n;
        offset += (uint64_t)n;
    }
    return true;
}

static void encode_head(unsigned char out[SLOT], const Head *head) {
    memset(out, 0, SLOT);
    memcpy(out, head_magic, sizeof(head_magic));
    encode64(out + 8, head->seq);
    encode64(out + 16, head->end);
    encode64(out + 24, head->catalog);
    encode64(out + 32, head->catalog_bytes);
    encode64(out + 40, head->live);
    encode64(out + 56, checksum(out, 56));
}

static bool decode_head(const unsigned char in[SLOT], Head *head) {
    if (memcmp(in, head_magic, sizeof(head_magic)) || decode64(in + 56) != checksum(in, 56))
        return false;
    *head = (Head){decode64(in + 8), decode64(in + 16), decode64(in + 24),
                   decode64(in + 32), decode64(in + 40)};
    return head->seq && head->end >= DATA && head->end <= (uint64_t)INT64_MAX &&
           head->end <= SIZE_MAX && head->catalog_bytes % RECORD == 0 &&
           !head->catalog == !head->catalog_bytes;
}

/* The newer valid slot wins; a torn newer slot leaves the previous commit. */
static bool read_head(int fd, Head *out) {
    unsigned char slots[2 * SLOT];
    Head a, b;
    if (!read_at(fd, slots, sizeof(slots), 0)) return false;
    bool first = decode_head(slots, &a), second = decode_head(slots + SLOT, &b);
    if (!first && !second) return false;
    *out = first && (!second || a.seq > b.seq) ? a : b;
    return true;
}

static bool map_tree(int fd, const Head *head, Tree *tree) {
    struct stat info;
    *tree = (Tree){0};
    if (fstat(fd, &info) || !S_ISREG(info.st_mode) || info.st_size < 0 ||
        (uint64_t)info.st_size < head->end) return false;
    void *map = mmap(NULL, (size_t)head->end, PROT_READ, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) return false;
    *tree = (Tree){.map = map, .end = head->end};
    return true;
}

static void release_mapping(Mapping *mapping) {
    if (!mapping || --mapping->refs) return;
    munmap((void *)mapping->map, (size_t)mapping->end);
    free(mapping);
}

static void release_tree(Tree *tree) {
    if (tree->shared) release_mapping(tree->shared);
    else if (tree->map) munmap((void *)tree->map, (size_t)tree->end);
    for (size_t i = 0; i < tree->count; ++i) {
        free(tree->nodes[i].bytes);
        free(tree->nodes[i].children);
    }
    free(tree->nodes);
    *tree = (Tree){0};
}

static const unsigned char *leaf_bytes(const Tree *tree, Entry entry) {
    if (!(entry.ref & LEAF_BIT) || !entry.bytes || entry.bytes > LEAF_MAX) return NULL;
    uint64_t at = entry.ref & LOCATION;
    if (entry.ref & MEMORY_BIT) {
        if (at >= tree->count) return NULL;
        const Node *node = &tree->nodes[at];
        return node->bytes && node->size == entry.bytes ? node->bytes : NULL;
    }
    if (at < DATA || at > tree->end || entry.bytes > tree->end - at) return NULL;
    return tree->map + at;
}

/* Returns the child count of a checked branch at height, or 0. */
static size_t children(const Tree *tree, Entry entry, unsigned height, Entry out[FAN_MAX]) {
    if ((entry.ref & LEAF_BIT) || !height) return 0;
    uint64_t at = entry.ref & LOCATION;
    size_t count;
    if (entry.ref & MEMORY_BIT) {
        if (at >= tree->count) return 0;
        const Node *node = &tree->nodes[at];
        if (node->bytes || node->height != height || !node->size || node->size > FAN_MAX) return 0;
        count = node->size;
        memcpy(out, node->children, count * sizeof(*out));
    } else {
        if (at < DATA || at % 8 || at > tree->end || tree->end - at < 8) return 0;
        const unsigned char *p = tree->map + at;
        count = (size_t)p[4] | (size_t)p[5] << 8;
        unsigned stored = (unsigned)p[6] | (unsigned)p[7] << 8;
        if (memcmp(p, branch_magic, sizeof(branch_magic)) || stored != height || !count ||
            count > FAN_MAX || (tree->end - at - 8) / 16 < count) return 0;
        for (size_t i = 0; i < count; ++i)
            out[i] = (Entry){decode64(p + 8 + 16 * i), decode64(p + 16 + 16 * i)};
    }
    uint64_t sum = 0;
    for (size_t i = 0; i < count; ++i) {
        bool leaf = (out[i].ref & LEAF_BIT) != 0;
        if (!out[i].bytes || out[i].bytes > UINT64_MAX - sum || leaf != (height == 1)) return 0;
        sum += out[i].bytes;
    }
    return sum == entry.bytes ? count : 0;
}

static bool root_height(const Tree *tree, Entry root, unsigned *height) {
    *height = 0;
    if (!root.bytes) return !root.ref;
    if (root.ref & LEAF_BIT) return true;
    uint64_t at = root.ref & LOCATION;
    if (root.ref & MEMORY_BIT) {
        if (at >= tree->count || tree->nodes[at].bytes) return false;
        *height = tree->nodes[at].height;
    } else {
        if (at < DATA || at > tree->end || tree->end - at < 8) return false;
        *height = (unsigned)tree->map[at + 6] | (unsigned)tree->map[at + 7] << 8;
    }
    return *height >= 1 && *height <= HEIGHT_MAX;
}

/* Visit a range as borrowed extents: 1 done, 0 damaged, -1 stopped by visit. */
static int visit_range(const Tree *tree, Entry entry, unsigned height, uint64_t offset,
                       uint64_t length, bool (*visit)(void *, PureSpan), void *context) {
    if (!height) {
        const unsigned char *bytes = leaf_bytes(tree, entry);
        if (!bytes || offset > entry.bytes || length > entry.bytes - offset) return 0;
        return visit(context, (PureSpan){bytes + offset, (size_t)length}) ? 1 : -1;
    }
    Entry kids[FAN_MAX];
    size_t count = children(tree, entry, height, kids);
    if (!count) return 0;
    uint64_t start = 0;
    for (size_t i = 0; i < count && length; ++i) {
        uint64_t end = start + kids[i].bytes;
        if (offset < end) {
            uint64_t inside = offset - start, take = kids[i].bytes - inside;
            if (take > length) take = length;
            int result = visit_range(tree, kids[i], height - 1, inside, take, visit, context);
            if (result != 1) return result;
            offset += take;
            length -= take;
        }
        start = end;
    }
    return length ? 0 : 1;
}

static bool copy_span(void *context, PureSpan span) {
    unsigned char **out = context;
    memcpy(*out, span.data, span.size);
    *out += span.size;
    return true;
}

static bool copy_range(const Tree *tree, Entry entry, unsigned height, uint64_t offset,
                       uint64_t length, unsigned char *out) {
    return !length || visit_range(tree, entry, height, offset, length, copy_span, &out) == 1;
}

/* The bytes of a range held by one leaf, or NULL when it spans leaves. */
static const unsigned char *single_leaf(const Tree *tree, Entry entry, unsigned height,
                                        uint64_t offset, uint64_t length) {
    while (height) {
        Entry kids[FAN_MAX];
        size_t count = children(tree, entry, height, kids), i = 0;
        uint64_t start = 0;
        if (!count) return NULL;
        while (i + 1 < count && offset >= start + kids[i].bytes) start += kids[i++].bytes;
        if (offset - start > kids[i].bytes || length > kids[i].bytes - (offset - start)) return NULL;
        entry = kids[i];
        offset -= start;
        --height;
    }
    const unsigned char *bytes = leaf_bytes(tree, entry);
    return bytes && offset <= entry.bytes && length <= entry.bytes - offset ? bytes + offset : NULL;
}

static bool push(List *list, Entry entry) {
    if (list->count == list->capacity) {
        size_t capacity = list->capacity ? 2 * list->capacity : 8;
        Entry *items = realloc(list->items, capacity * sizeof(*items));
        if (!items) return false;
        list->items = items;
        list->capacity = capacity;
    }
    list->items[list->count++] = entry;
    return true;
}

static size_t node_bytes(const Node *node) {
    return node->bytes ? node->size : node->size * sizeof(Entry);
}

static bool add_node(Tree *tree, Node node, uint64_t *index) {
    tree->bytes += node_bytes(&node);
    if (tree->free_head) {
        *index = tree->free_head - 1;
        tree->free_head = tree->nodes[*index].size;
        --tree->free_count;
        tree->nodes[*index] = node;
        return true;
    }
    if (tree->count == tree->capacity) {
        size_t capacity = tree->capacity ? 2 * tree->capacity : 64;
        Node *nodes = realloc(tree->nodes, capacity * sizeof(*nodes));
        if (!nodes) { tree->bytes -= node_bytes(&node); return false; }
        tree->nodes = nodes;
        tree->capacity = capacity;
    }
    tree->nodes[tree->count] = node;
    *index = tree->count++;
    return true;
}

static void take(Source *source, unsigned char *out, size_t length) {
    while (length) {
        PureSpan part = source->part[source->index];
        size_t left = part.size - source->offset;
        if (!left) { ++source->index; source->offset = 0; continue; }
        size_t n = left < length ? left : length;
        memcpy(out, part.data + source->offset, n);
        out += n;
        length -= n;
        source->offset += n;
    }
}

/* Balanced leaves: every leaf of a multi-leaf result holds >= LEAF_MAX / 2. */
static bool chunk_bytes(Tree *tree, Source *source, uint64_t total, List *out) {
    uint64_t pieces = (total + LEAF_MAX - 1) / LEAF_MAX;
    for (uint64_t i = 0; i < pieces; ++i) {
        size_t size = (size_t)(total / pieces + (i < total % pieces));
        unsigned char *bytes = malloc(size);
        uint64_t index;
        if (!bytes) return false;
        take(source, bytes, size);
        if (!add_node(tree, (Node){.bytes = bytes, .size = size}, &index)) { free(bytes); return false; }
        if (!push(out, (Entry){LEAF_BIT | MEMORY_BIT | index, size})) return false;
    }
    return true;
}

static bool chunk_entries(Tree *tree, const Entry *items, size_t count, unsigned height, List *out) {
    size_t pieces = (count + FAN_MAX - 1) / FAN_MAX;
    for (size_t i = 0, at = 0; i < pieces; ++i) {
        size_t size = count / pieces + (i < count % pieces);
        Entry *kids = malloc(size * sizeof(*kids));
        uint64_t sum = 0, index;
        if (!kids) return false;
        memcpy(kids, items + at, size * sizeof(*kids));
        for (size_t k = 0; k < size; ++k) sum += kids[k].bytes;
        if (!add_node(tree, (Node){.children = kids, .size = size, .height = height}, &index)) {
            free(kids);
            return false;
        }
        if (!push(out, (Entry){MEMORY_BIT | index, sum})) return false;
        at += size;
    }
    return true;
}

/* Only nodes created by this edit can be underfull; stored siblings are left as they are. */
static bool underfull(const Tree *tree, Entry entry, unsigned height) {
    if (!(entry.ref & MEMORY_BIT)) return false;
    return height ? tree->nodes[entry.ref & LOCATION].size < FAN_MIN : entry.bytes < LEAF_MIN;
}

static bool replace(List *list, size_t at, size_t removed, const List *with) {
    size_t count = list->count - removed + with->count;
    if (count > list->capacity) {
        Entry *items = realloc(list->items, count * sizeof(*items));
        if (!items) return false;
        list->items = items;
        list->capacity = count;
    }
    memmove(list->items + at + with->count, list->items + at + removed,
            (list->count - at - removed) * sizeof(*list->items));
    memcpy(list->items + at, with->items, with->count * sizeof(*list->items));
    list->count = count;
    return true;
}

static bool merge(Tree *tree, Entry a, Entry b, unsigned height, List *out);

/* Merge each new underfull node with a neighbour. A merge removes a node or
 * yields two nodes of at least half capacity, so the loop terminates. */
static bool repair(Tree *tree, List *all, unsigned height) {
    bool ok = true;
    for (size_t i = 0; ok && all->count > 1 && i < all->count;) {
        if (!underfull(tree, all->items[i], height)) { ++i; continue; }
        size_t at = i + 1 < all->count ? i : i - 1;
        List merged = {0};
        ok = merge(tree, all->items[at], all->items[at + 1], height, &merged) &&
             replace(all, at, 2, &merged);
        free(merged.items);
        i = at;
    }
    return ok;
}

static bool merge(Tree *tree, Entry a, Entry b, unsigned height, List *out) {
    if (!height) {
        const unsigned char *x = leaf_bytes(tree, a), *y = leaf_bytes(tree, b);
        if (!x || !y) return false;
        Source source = {.part = {{x, (size_t)a.bytes}, {y, (size_t)b.bytes}}};
        return chunk_bytes(tree, &source, a.bytes + b.bytes, out);
    }
    Entry kids[FAN_MAX];
    List all = {0};
    size_t count = children(tree, a, height, kids);
    bool ok = count != 0;
    for (size_t i = 0; ok && i < count; ++i) ok = push(&all, kids[i]);
    count = ok ? children(tree, b, height, kids) : 0;
    ok = count != 0;
    for (size_t i = 0; ok && i < count; ++i) ok = push(&all, kids[i]);
    /* A small child carried in from a one-child parent is repaired here too. */
    ok = ok && repair(tree, &all, height - 1) && chunk_entries(tree, all.items, all.count, height, out);
    free(all.items);
    return ok;
}

/* Replace [offset, offset + remove) of entry with insert. Appends the
 * replacement nodes, all at entry's height, to out (possibly none). */
static bool edit(Tree *tree, Entry entry, unsigned height, uint64_t offset, uint64_t remove,
                 PureSpan insert, List *out) {
    if (offset > entry.bytes || remove > entry.bytes - offset) return false;
    if (!height) {
        const unsigned char *bytes = leaf_bytes(tree, entry);
        if (!bytes) return false;
        Source source = {.part = {{bytes, (size_t)offset}, insert,
            {bytes + offset + remove, (size_t)(entry.bytes - offset - remove)}}};
        return chunk_bytes(tree, &source, entry.bytes - remove + insert.size, out);
    }
    Entry kids[FAN_MAX];
    size_t count = children(tree, entry, height, kids), first = count, final = count;
    if (!count) return false;
    uint64_t last = remove ? offset + remove - 1 : offset, start = 0, first_start = 0, final_start = 0;
    for (size_t i = 0; i < count; ++i) {
        uint64_t end = start + kids[i].bytes;
        bool tail = i + 1 == count;
        if (first == count && (offset < end || tail)) { first = i; first_start = start; }
        if (first != count && (last < end || tail)) { final = i; final_start = start; break; }
        start = end;
    }
    List middle = {0}, all = {0};
    bool ok = first == final ?
        edit(tree, kids[first], height - 1, offset - first_start, remove, insert, &middle) :
        edit(tree, kids[first], height - 1, offset - first_start,
             first_start + kids[first].bytes - offset, insert, &middle) &&
        edit(tree, kids[final], height - 1, 0, offset + remove - final_start, (PureSpan){0}, &middle);
    for (size_t i = 0; ok && i < first; ++i) ok = push(&all, kids[i]);
    for (size_t i = 0; ok && i < middle.count; ++i) ok = push(&all, middle.items[i]);
    for (size_t i = final + 1; ok && i < count; ++i) ok = push(&all, kids[i]);
    free(middle.items);
    ok = ok && repair(tree, &all, height - 1) && chunk_entries(tree, all.items, all.count, height, out);
    free(all.items);
    return ok;
}

/* The new root of root with [offset, offset + remove) replaced by insert. */
static bool rebuild(Tree *tree, Entry root, uint64_t offset, uint64_t remove,
                    PureSpan insert, Entry *out) {
    unsigned height;
    List list = {0};
    bool ok = root_height(tree, root, &height);
    if (ok && !root.bytes) {
        Source source = {.part = {insert}};
        ok = !offset && !remove && chunk_bytes(tree, &source, insert.size, &list);
    } else if (ok) ok = edit(tree, root, height, offset, remove, insert, &list);
    while (ok && list.count > 1) {
        List up = {0};
        ok = height < HEIGHT_MAX && chunk_entries(tree, list.items, list.count, ++height, &up);
        free(list.items);
        list = up;
    }
    Entry result = ok && list.count ? list.items[0] : (Entry){0};
    free(list.items);
    while (ok && height && result.bytes) {
        Entry kids[FAN_MAX];
        size_t count = children(tree, result, height, kids);
        if (count != 1) { ok = count > 1; break; }
        result = kids[0];
        --height;
    }
    *out = result;
    return ok;
}

static bool flush(Writer *writer) {
    if (writer->used && !writer->failed &&
        !write_at(writer->fd, writer->buffer, writer->used, writer->at - writer->used))
        writer->failed = true;
    writer->used = 0;
    return !writer->failed;
}

static void put_bytes(Writer *writer, const unsigned char *bytes, size_t length) {
    while (length && !writer->failed) {
        if (writer->used == BUFFER && !flush(writer)) return;
        size_t n = BUFFER - writer->used < length ? BUFFER - writer->used : length;
        memcpy(writer->buffer + writer->used, bytes, n);
        writer->used += n;
        writer->at += n;
        bytes += n;
        length -= n;
    }
}

static uint64_t put_branch(Writer *writer, const Entry *kids, size_t count, unsigned height) {
    static const unsigned char zero[8];
    put_bytes(writer, zero, (size_t)((8 - writer->at % 8) % 8));
    uint64_t at = writer->at;
    unsigned char header[8];
    memcpy(header, branch_magic, sizeof(branch_magic));
    header[4] = (unsigned char)count;
    header[5] = (unsigned char)(count >> 8);
    header[6] = (unsigned char)height;
    header[7] = (unsigned char)(height >> 8);
    put_bytes(writer, header, sizeof(header));
    for (size_t i = 0; i < count; ++i) {
        unsigned char pair[16];
        encode64(pair, kids[i].ref);
        encode64(pair + 8, kids[i].bytes);
        put_bytes(writer, pair, sizeof(pair));
    }
    return at;
}

/* Append a subtree's arena nodes, children first, and set entry to the
 * stored reference. The arena is not changed, so a failed commit leaves a
 * draft intact. With relocate, stored nodes are copied too (compaction, or a
 * draft whose base file was replaced by compaction). */
static bool write_subtree(const Tree *tree, Writer *writer, Entry *entry, unsigned height,
                          bool relocate) {
    if (!entry->bytes || (!(entry->ref & MEMORY_BIT) && !relocate)) return true;
    if (!height) {
        const unsigned char *bytes = leaf_bytes(tree, *entry);
        if (!bytes) return false;
        entry->ref = LEAF_BIT | writer->at;
        put_bytes(writer, bytes, (size_t)entry->bytes);
        return !writer->failed;
    }
    Entry kids[FAN_MAX];
    size_t count = children(tree, *entry, height, kids);
    if (!count) return false;
    for (size_t i = 0; i < count; ++i)
        if (!write_subtree(tree, writer, &kids[i], height - 1, relocate)) return false;
    entry->ref = put_branch(writer, kids, count, height);
    return !writer->failed;
}

static bool write_root(const Tree *tree, Writer *writer, Entry *root, bool relocate) {
    unsigned height;
    return root_height(tree, *root, &height) && write_subtree(tree, writer, root, height, relocate);
}

static bool read_record(const Tree *tree, Entry catalog, uint64_t index,
                        unsigned char record[RECORD], Entry *document, int64_t *revision) {
    unsigned height;
    if (!root_height(tree, catalog, &height) ||
        !copy_range(tree, catalog, height, index * RECORD, RECORD, record)) return false;
    *document = (Entry){decode64(record + KEY), decode64(record + KEY + 8)};
    uint64_t stored = decode64(record + KEY + 16);
    *revision = (int64_t)stored;
    return stored && stored <= (uint64_t)INT64_MAX && !document->ref == !document->bytes &&
           document->bytes <= PURE_OBJECT_LIMIT;
}

/* Binary search over records ordered by their zero-padded key bytes. */
static bool find(const Tree *tree, Entry catalog, const char *key, uint64_t *index,
                 bool *found, Entry *document, int64_t *revision) {
    unsigned char wanted[KEY] = {0}, record[RECORD];
    memcpy(wanted, key, strlen(key));
    uint64_t low = 0, high = catalog.bytes / RECORD;
    *found = false;
    *document = (Entry){0};
    *revision = 0;
    while (low < high) {
        uint64_t middle = low + (high - low) / 2;
        if (!read_record(tree, catalog, middle, record, document, revision)) return false;
        int order = memcmp(wanted, record, KEY);
        if (!order) { *index = middle; *found = true; return true; }
        if (order < 0) high = middle; else low = middle + 1;
    }
    *document = (Entry){0};
    *revision = 0;
    *index = low;
    return true;
}

static int create_temporary(int directory, char name[96]) {
    for (unsigned attempt = 0; attempt < 64; ++attempt) {
        if (temporary_sequence == UINT64_MAX) break;
        snprintf(name, 96, ".pending-%ld-%" PRIu64, (long)getpid(), ++temporary_sequence);
        int fd = openat(directory, name, O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd >= 0) return fd;
        if (errno != EEXIST) break;
    }
    name[0] = 0; /* Never remove an existing file on a name collision. */
    return -1;
}

/* Publish a fresh file with both slots pointing at head. */
static bool install(int directory, char temporary[96], int fd, const Head *head) {
    unsigned char slots[2 * SLOT];
    encode_head(slots, head);
    memcpy(slots + SLOT, slots, SLOT);
    bool ok = write_at(fd, slots, sizeof(slots), 0) && sync_fd(fd);
    if (close(fd)) ok = false;
    ok = ok && !renameat(directory, temporary, directory, tree_name);
    if (ok) temporary[0] = 0;
    return ok && sync_fd(directory);
}

/* Rewrite only reachable nodes. Open readers keep their mapping of the
 * previous file. A failure leaves the published file untouched. */
static void compact(int directory, int source, const Head *head) {
    Tree tree;
    if (!map_tree(source, head, &tree)) return;
    char temporary[96];
    int fd = create_temporary(directory, temporary);
    Writer *writer = fd >= 0 ? malloc(sizeof(*writer)) : NULL;
    uint64_t count = head->catalog_bytes / RECORD;
    unsigned char *records = count ? malloc((size_t)(count * RECORD)) : NULL;
    Entry catalog = {head->catalog, head->catalog_bytes};
    unsigned height;
    bool ok = writer && (!count || records) && root_height(&tree, catalog, &height) &&
              (!count || copy_range(&tree, catalog, height, 0, count * RECORD, records));
    if (writer) *writer = (Writer){.fd = fd, .at = DATA};
    for (uint64_t i = 0; ok && i < count; ++i) {
        unsigned char *record = records + i * RECORD;
        Entry document = {decode64(record + KEY), decode64(record + KEY + 8)};
        unsigned document_height;
        ok = root_height(&tree, document, &document_height) &&
             write_subtree(&tree, writer, &document, document_height, true);
        encode64(record + KEY, document.ref);
    }
    Entry fresh = {0};
    ok = ok && rebuild(&tree, (Entry){0}, 0, 0, (PureSpan){records, (size_t)(count * RECORD)}, &fresh) &&
         write_root(&tree, writer, &fresh, false) && flush(writer);
    if (ok) {
        Head compacted = {head->seq + 1, writer->at, fresh.ref, fresh.bytes, writer->at - DATA};
        ok = install(directory, temporary, fd, &compacted);
        fd = -1;
    }
    if (fd >= 0) close(fd);
    if (*temporary) unlinkat(directory, temporary, 0);
    free(records);
    free(writer);
    release_tree(&tree);
}

static bool lock_writer(int lock, bool wait) {
    struct flock hold = {.l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 0, .l_len = 0};
    int rc;
    do { rc = fcntl(lock, wait ? F_SETLKW : F_SETLK, &hold); } while (rc && wait && errno == EINTR);
    return rc == 0;
}

PureResult pure_store_init(const char *path) {
    if (!path || !*path) return PURE_STORE_ERROR;
    bool created = mkdir(path, 0700) == 0;
    if (!created && errno != EEXIST) return PURE_STORE_ERROR;
    int directory = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (directory < 0) return PURE_STORE_ERROR; /* Never reinterpret an old DB file. */
    pthread_mutex_lock(&writers);
    int lock = openat(directory, ".writer.lock", O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    struct stat info;
    bool ok = lock >= 0 && fstat(lock, &info) == 0 && S_ISREG(info.st_mode) && lock_writer(lock, true);
    if (ok) {
        Head head;
        int fd = openat(directory, tree_name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        if (fd >= 0) {
            ok = read_head(fd, &head); /* An unreadable file is never replaced. */
            close(fd);
        } else if (errno != ENOENT) ok = false;
        else {
            char temporary[96];
            int created_fd = create_temporary(directory, temporary);
            head = (Head){.seq = 1, .end = DATA};
            ok = created_fd >= 0 && install(directory, temporary, created_fd, &head);
            if (*temporary) unlinkat(directory, temporary, 0);
        }
    }
    ok = ok && sync_fd(lock) && sync_fd(directory);
    if (lock >= 0) close(lock);
    pthread_mutex_unlock(&writers);
    close(directory);
    if (ok && created) {
        char *parent = strdup(path);
        if (!parent) return PURE_STORE_ERROR;
        size_t length = strlen(parent);
        while (length > 1 && parent[length - 1] == '/') parent[--length] = 0;
        char *slash = strrchr(parent, '/');
        if (!slash) strcpy(parent, ".");
        else if (slash == parent) parent[1] = 0;
        else *slash = 0;
        int fd = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        free(parent);
        ok = fd >= 0 && sync_fd(fd);
        if (fd >= 0) close(fd);
    }
    return ok ? PURE_OK : PURE_STORE_ERROR;
}

void pure_store_release(PureStoredView *view) {
    if (view->mapped_size) munmap(view->mapping, view->mapped_size);
    *view = (PureStoredView){0};
}

PureResult pure_store_select(const char *path, const char *key, size_t offset,
                             size_t length, bool whole, size_t minimum,
                             size_t available, PureStoredView *out) {
    *out = (PureStoredView){0};
    if (!valid_key(key)) return PURE_STORE_ERROR;
    int directory = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (directory < 0) return PURE_STORE_ERROR;
    int fd = openat(directory, tree_name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    close(directory);
    if (fd < 0) return PURE_STORE_ERROR;
    Tree tree = {0};
    Head head;
    Entry document;
    uint64_t index;
    int64_t revision;
    bool found;
    unsigned height;
    PureResult result = PURE_STORE_ERROR;
    if (!read_head(fd, &head) || !map_tree(fd, &head, &tree) ||
        !find(&tree, (Entry){head.catalog, head.catalog_bytes}, key, &index, &found,
              &document, &revision)) goto finish;
    if (!found) { result = PURE_NOT_FOUND; goto finish; }
    size_t size = (size_t)document.bytes;
    if (size < minimum || offset > size) { result = PURE_LIMIT; goto finish; }
    if (whole) length = size - offset;
    if (length > size - offset) { result = PURE_LIMIT; goto finish; }
    if (length > available) { result = PURE_BUDGET; goto finish; }
    if (length) {
        if (!root_height(&tree, document, &height)) goto finish;
        const unsigned char *direct = single_leaf(&tree, document, height, offset, length);
        void *mapping;
        size_t mapped_size, prefix = 0;
        if (direct) {
            /* One extent: map it in place, as the flat store does. */
            long page = sysconf(_SC_PAGESIZE);
            if (page <= 0) goto finish;
            size_t start = (size_t)(direct - tree.map), aligned = start - start % (size_t)page;
            prefix = start - aligned;
            mapped_size = prefix + length;
            mapping = mmap(NULL, mapped_size, PROT_READ, MAP_PRIVATE, fd, (off_t)aligned);
            if (mapping == MAP_FAILED) goto finish;
        } else {
            /* A contiguous view of several extents is assembled for this API. */
            mapped_size = length;
            mapping = mmap(NULL, mapped_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (mapping == MAP_FAILED) goto finish;
            if (!copy_range(&tree, document, height, offset, length, mapping) ||
                mprotect(mapping, mapped_size, PROT_READ)) {
                munmap(mapping, mapped_size);
                goto finish;
            }
        }
        out->mapping = mapping;
        out->mapped_size = mapped_size;
        out->body = (PureSpan){(const unsigned char *)mapping + prefix, length};
    }
    out->revision = revision;
    out->object_size = size;
    result = PURE_OK;
finish:
    release_tree(&tree);
    close(fd);
    return result;
}

PureResult pure_store_get(const char *path, const char *key, size_t available, PureStoredView *out) {
    return pure_store_select(path, key, 0, 0, true, 0, available, out);
}

PureResult pure_store_get_range(const char *path, const char *key, size_t offset,
                                size_t length, size_t available, PureStoredView *out) {
    return pure_store_select(path, key, offset, length, false, 0, available, out);
}

/* A locked writer over the current published file. */
typedef struct {
    int directory, lock, fd;
    Head head;
    Tree tree;
    struct stat file;
    bool turn; /* holds this process's writer turn */
} Session;

static void close_session(Session *session) {
    release_tree(&session->tree);
    if (session->fd >= 0) close(session->fd);
    if (session->lock >= 0) close(session->lock);
    if (session->directory >= 0) close(session->directory);
    if (session->turn) pthread_mutex_unlock(&writers);
    session->turn = false;
}

static PureResult open_session(const char *path, Session *session) {
    *session = (Session){.directory = -1, .lock = -1, .fd = -1};
    pthread_mutex_lock(&writers); /* another thread of this process waits, not fails */
    session->turn = true;
    session->directory = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (session->directory < 0) return PURE_STORE_ERROR;
    session->lock = openat(session->directory, ".writer.lock", O_RDWR | O_NOFOLLOW | O_CLOEXEC);
    struct stat info;
    if (session->lock < 0 || fstat(session->lock, &info) || !S_ISREG(info.st_mode)) return PURE_STORE_ERROR;
    if (!lock_writer(session->lock, false))
        return errno == EACCES || errno == EAGAIN ? PURE_BUSY : PURE_STORE_ERROR;
    session->fd = openat(session->directory, tree_name, O_RDWR | O_NOFOLLOW | O_CLOEXEC);
    if (session->fd < 0 || !read_head(session->fd, &session->head) ||
        !map_tree(session->fd, &session->head, &session->tree) || fstat(session->fd, &session->file))
        return PURE_STORE_ERROR;
    /* Bytes after the published head belong to a commit nobody acknowledged. */
    if ((uint64_t)session->file.st_size > session->head.end &&
        ftruncate(session->fd, (off_t)session->head.end)) return PURE_STORE_ERROR;
    return PURE_OK;
}

/* The documents' new nodes are already appended at writer. Record each in
 * the catalog (revisions come in as the base and leave as the new one),
 * then make the commit durable before its head becomes visible: every
 * document becomes visible at once, or none does. */
#ifdef PURE_STORE_TEST
static unsigned long publications;
unsigned long pure_tree_test_publications(void) { return publications; }
#endif

static PureResult publish(Session *session, Writer *writer, size_t count, const char *const *keys,
                          const Entry *documents, int64_t *revisions) {
#ifdef PURE_STORE_TEST
    ++publications;
#endif
    Entry catalog = {session->head.catalog, session->head.catalog_bytes};
    bool written = true;
    for (size_t i = 0; written && i < count; ++i) {
        unsigned char record[RECORD] = {0};
        uint64_t index;
        bool found;
        Entry current, updated;
        int64_t stored;
        memcpy(record, keys[i], strlen(keys[i]));
        encode64(record + KEY, documents[i].ref);
        encode64(record + KEY + 8, documents[i].bytes);
        encode64(record + KEY + 16, (uint64_t)revisions[i] + 1);
        written = find(&session->tree, catalog, keys[i], &index, &found, &current, &stored) &&
                  rebuild(&session->tree, catalog, index * RECORD, found ? RECORD : 0,
                          (PureSpan){record, RECORD}, &updated);
        catalog = updated;
    }
    written = written && write_root(&session->tree, writer, &catalog, false) && flush(writer);
    if (!written || fault(PURE_AFTER_HEADER) || fault(PURE_BEFORE_FILE_SYNC) ||
        !sync_fd(session->fd) || fault(PURE_BEFORE_RENAME)) {
        if (ftruncate(session->fd, (off_t)session->head.end) == 0) sync_fd(session->fd);
        return PURE_STORE_ERROR;
    }
    Head published = {session->head.seq + 1, writer->at, catalog.ref, catalog.bytes, session->head.live};
    unsigned char slot[SLOT];
    encode_head(slot, &published);
    for (size_t i = 0; i < count; ++i) ++revisions[i]; /* Past this point the head may already be visible. */
    if (!write_at(session->fd, slot, SLOT, (published.seq % 2) * SLOT) ||
        fault(PURE_BEFORE_DIR_SYNC) || !sync_fd(session->fd)) return PURE_UNCERTAIN;
    if (published.end - DATA > 4 * published.live + COMPACT_SLACK)
        compact(session->directory, session->fd, &published);
    return PURE_OK;
}

static PureResult mutate(const char *path, const char *key, int64_t expected,
                         PureSpan body, bool splice, size_t offset, size_t remove,
                         size_t max_size, int64_t *revision) {
    *revision = 0;
    if (!valid_key(key) || (body.size && !body.data)) return PURE_STORE_ERROR;
    if (body.size > PURE_OBJECT_LIMIT || max_size > PURE_OBJECT_LIMIT || expected < 0) return PURE_LIMIT;
    Session session;
    Writer *writer = NULL;
    Entry document, next;
    uint64_t index;
    bool found;
    PureResult result = open_session(path, &session);
    if (result != PURE_OK) goto finish;
    result = PURE_STORE_ERROR;
    if (!find(&session.tree, (Entry){session.head.catalog, session.head.catalog_bytes}, key,
              &index, &found, &document, revision)) goto finish;
    if (*revision != expected) { result = PURE_CONFLICT; goto finish; }
    if (*revision == INT64_MAX) { result = PURE_LIMIT; goto finish; }
    uint64_t old_size = document.bytes;
    if (splice && (offset > old_size || remove > old_size - offset ||
                   old_size - remove > max_size ||
                   body.size > max_size - (old_size - remove))) { result = PURE_LIMIT; goto finish; }
    writer = malloc(sizeof(*writer));
    if (!writer || !rebuild(&session.tree, splice ? document : (Entry){0}, splice ? offset : 0,
                            splice ? remove : 0, body, &next)) goto finish;
    *writer = (Writer){.fd = session.fd, .at = session.head.end};
    if (!write_root(&session.tree, writer, &next, false)) {
        if (ftruncate(session.fd, (off_t)session.head.end) == 0) sync_fd(session.fd);
        goto finish;
    }
    result = publish(&session, writer, 1, &key, &next, revision);
finish:
    free(writer);
    close_session(&session);
    return result;
}

/* Keys come from the catalog in ascending byte order. Runtime values are not
 * documents and are left out. */
PureResult pure_store_keys(const char *path, bool (*visit)(void *, const char *key),
                           void *context) {
    int directory = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (directory < 0) return PURE_STORE_ERROR;
    int fd = openat(directory, tree_name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    close(directory);
    if (fd < 0) return PURE_STORE_ERROR;
    Tree tree = {0};
    Head head;
    PureResult result = PURE_STORE_ERROR;
    if (read_head(fd, &head) && map_tree(fd, &head, &tree)) {
        Entry catalog = {head.catalog, head.catalog_bytes}, document;
        unsigned char record[RECORD];
        char key[KEY + 1] = {0};
        int64_t revision;
        result = PURE_OK;
        for (uint64_t i = 0; i < catalog.bytes / RECORD; ++i) {
            if (!read_record(&tree, catalog, i, record, &document, &revision)) { result = PURE_STORE_ERROR; break; }
            memcpy(key, record, KEY);
            if (runtime_key(key)) continue;
            if (!valid_key(key)) { result = PURE_STORE_ERROR; break; }
            if (!visit(context, key)) break;
        }
    }
    release_tree(&tree);
    close(fd);
    return result;
}

/* A store kept open by one owner thread: its current file and a mapping of
 * its newest version, shared by the drafts opened through it. */
struct PureStore {
    char *path;
    int directory, fd;
    dev_t device;
    ino_t inode;
    Head head;
    Mapping *mapping;
    bool pinned; /* drafts open at the version of the last check */
};

struct PureDraft {
    PureStore *store; /* the handle it was opened through, or NULL */
    char *path;       /* owned unless opened through a store handle */
    char key[KEY + 1];
    Tree tree;        /* the base file's mapping plus this draft's node arena */
    dev_t device;     /* identity of the base file; compaction replaces it */
    ino_t inode;
    Entry root;
    int64_t revision;
    size_t live;      /* arena bytes reachable at the last collection */
    bool held;        /* a transaction still reads earlier versions */
};

/* Rebase the draft on the committed version of its key. */
static PureResult load_draft(PureDraft *draft) {
    int directory = open(draft->path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (directory < 0) return PURE_STORE_ERROR;
    int fd = openat(directory, tree_name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    close(directory);
    if (fd < 0) return PURE_STORE_ERROR;
    Tree tree = {0};
    Head head;
    struct stat info;
    Entry document;
    uint64_t index;
    int64_t revision;
    bool found;
    bool ok = !fstat(fd, &info) && read_head(fd, &head) && map_tree(fd, &head, &tree) &&
              find(&tree, (Entry){head.catalog, head.catalog_bytes}, draft->key, &index, &found,
                   &document, &revision);
    close(fd);
    if (!ok) { release_tree(&tree); return PURE_STORE_ERROR; }
    release_tree(&draft->tree);
    draft->tree = tree;
    draft->device = info.st_dev;
    draft->inode = info.st_ino;
    draft->root = document;
    draft->revision = revision;
    draft->live = 0;
    return PURE_OK;
}

PureResult pure_draft_open(const char *path, const char *key, PureDraft **out) {
    *out = NULL;
    if (!path || !(valid_key(key) || runtime_key(key))) return PURE_INVALID;
    PureDraft *draft = calloc(1, sizeof(*draft));
    if (!draft || !(draft->path = strdup(path))) { free(draft); return PURE_ALLOCATION; }
    memcpy(draft->key, key, strlen(key) + 1);
    PureResult result = load_draft(draft);
    if (result != PURE_OK) { pure_draft_close(draft); return result; }
    *out = draft;
    return PURE_OK;
}

PureResult pure_state_open(const char *path, const char *name, PureDraft **out) {
    char key[KEY + 2];
    *out = NULL;
    if (!valid_key(name) || strlen(name) >= KEY) return PURE_INVALID;
    snprintf(key, sizeof(key), ".%s", name);
    return pure_draft_open(path, key, out);
}

PureDraft *pure_draft_new(void) {
    return calloc(1, sizeof(PureDraft)); /* no path: nothing mapped, nothing to commit */
}

void pure_draft_close(PureDraft *draft) {
    if (!draft) return;
    release_tree(&draft->tree);
    if (!draft->store) free(draft->path);
    free(draft);
}

/* Bring the handle to the newest published version: the same file and head
 * cost two system calls; a new commit maps again, a compaction reopens. */
static PureResult refresh(PureStore *store) {
    struct stat info;
    if (fstatat(store->directory, tree_name, &info, AT_SYMLINK_NOFOLLOW) || !S_ISREG(info.st_mode))
        return PURE_STORE_ERROR;
    if (store->fd < 0 || info.st_dev != store->device || info.st_ino != store->inode) {
        int fd = openat(store->directory, tree_name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        if (fd < 0 || fstat(fd, &info)) { if (fd >= 0) close(fd); return PURE_STORE_ERROR; }
        if (store->fd >= 0) close(store->fd);
        store->fd = fd;
        store->device = info.st_dev;
        store->inode = info.st_ino;
        store->head = (Head){0};
    }
    Head head;
    if (!read_head(store->fd, &head)) return PURE_STORE_ERROR;
    if (store->mapping && head.seq == store->head.seq && head.end == store->head.end) return PURE_OK;
    Tree tree;
    Mapping *mapping = malloc(sizeof(*mapping));
    if (!mapping) return PURE_ALLOCATION;
    if (!map_tree(store->fd, &head, &tree)) { free(mapping); return PURE_STORE_ERROR; }
    *mapping = (Mapping){tree.map, tree.end, 1};
    release_mapping(store->mapping);
    store->mapping = mapping;
    store->head = head;
    return PURE_OK;
}

/* Base a draft on the handle's newest version of its key, sharing the mapping.
 * On failure the draft is unchanged. */
static PureResult rebase(PureDraft *draft) {
    PureStore *store = draft->store;
    Mapping *mapping = store->mapping;
    Tree tree = {.map = mapping->map, .end = mapping->end, .shared = mapping};
    Entry document;
    uint64_t index;
    int64_t revision;
    bool found;
    if (!find(&tree, (Entry){store->head.catalog, store->head.catalog_bytes}, draft->key, &index, &found, &document,
              &revision)) return PURE_STORE_ERROR;
    ++mapping->refs;
    release_tree(&draft->tree);
    draft->tree = tree;
    draft->device = store->device;
    draft->inode = store->inode;
    draft->root = document;
    draft->revision = revision;
    draft->live = 0;
    return PURE_OK;
}

PureResult pure_store_open(const char *path, PureStore **out) {
    *out = NULL;
    PureStore *store = calloc(1, sizeof(*store));
    if (!store || !path || !(store->path = strdup(path))) { free(store); return PURE_ALLOCATION; }
    store->fd = -1;
    store->directory = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    PureResult result = store->directory < 0 ? PURE_STORE_ERROR : refresh(store);
    if (result != PURE_OK) { pure_store_close(store); return result; }
    *out = store;
    return PURE_OK;
}

void pure_store_close(PureStore *store) {
    if (!store) return;
    release_mapping(store->mapping);
    if (store->fd >= 0) close(store->fd);
    if (store->directory >= 0) close(store->directory);
    free(store->path);
    free(store);
}

PureMapping *pure_draft_mapping(const PureDraft *draft) {
    Mapping *mapping = draft->tree.shared;
    if (mapping) ++mapping->refs;
    return mapping;
}

bool pure_mapping_holds(const PureMapping *mapping, PureSpan span) {
    uintptr_t base = (uintptr_t)mapping->map, at = (uintptr_t)span.data;
    return !span.size || (span.data && at >= base && at - base <= mapping->end && span.size <= mapping->end - (at - base));
}

void pure_mapping_release(PureMapping *mapping) { release_mapping(mapping); }

PureResult pure_store_pin(PureStore *store, bool pinned) {
    store->pinned = false;
    PureResult result = pinned ? refresh(store) : PURE_OK;
    store->pinned = pinned && result == PURE_OK;
    return result;
}

static PureResult open_through(PureStore *store, const char *key, PureDraft **out) {
    *out = NULL;
    PureResult result = store->pinned ? PURE_OK : refresh(store);
    if (result != PURE_OK) return result;
    PureDraft *draft = calloc(1, sizeof(*draft));
    if (!draft) return PURE_ALLOCATION;
    *draft = (PureDraft){.store = store, .path = store->path};
    memcpy(draft->key, key, strlen(key) + 1);
    result = rebase(draft);
    if (result != PURE_OK) { free(draft); return result; }
    *out = draft;
    return PURE_OK;
}

PureResult pure_store_draft(PureStore *store, const char *key, PureDraft **out) {
    *out = NULL;
    return store && valid_key(key) ? open_through(store, key, out) : PURE_INVALID;
}

PureResult pure_store_state(PureStore *store, const char *name, PureDraft **out) {
    char key[KEY + 2];
    *out = NULL;
    if (!store || !valid_key(name) || strlen(name) >= KEY) return PURE_INVALID;
    snprintf(key, sizeof(key), ".%s", name);
    return open_through(store, key, out);
}

size_t pure_draft_size(const PureDraft *draft) { return (size_t)draft->root.bytes; }

int64_t pure_draft_revision(const PureDraft *draft) { return draft->revision; }

static size_t mark(const Tree *tree, Entry entry, unsigned char *marks) {
    if (!(entry.ref & MEMORY_BIT)) return 0;
    size_t at = (size_t)(entry.ref & LOCATION);
    if (marks[at]) return 0;
    marks[at] = 1;
    const Node *node = &tree->nodes[at];
    size_t bytes = node_bytes(node);
    if (!node->bytes)
        for (size_t i = 0; i < node->size; ++i) bytes += mark(tree, node->children[i], marks);
    return bytes;
}

/* Every edit copies a leaf and path, so reclaim arena nodes the root no
 * longer reaches once they outweigh the reachable ones (and 32 KiB). */
static void collect(PureDraft *draft) {
    Tree *tree = &draft->tree;
    if (draft->held) return;
    size_t allowance = draft->live > 32768 ? draft->live : 32768;
    if (tree->bytes <= draft->live || tree->bytes - draft->live <= allowance) return;
    unsigned char *marks = calloc(tree->count, 1);
    if (!marks) return;
    draft->live = mark(tree, draft->root, marks);
    for (size_t i = 0; i < tree->count; ++i) {
        Node *node = &tree->nodes[i];
        if (marks[i] || node->height == FREE) continue;
        tree->bytes -= node_bytes(node);
        free(node->bytes);
        free(node->children);
        *node = (Node){.size = tree->free_head, .height = FREE};
        tree->free_head = i + 1;
        ++tree->free_count;
    }
    free(marks);
}

PureResult pure_draft_splice(PureDraft *draft, size_t offset, size_t remove, PureSpan insert) {
    uint64_t size = draft->root.bytes;
    if (insert.size && !insert.data) return PURE_INVALID;
    if (offset > size || remove > size - offset || insert.size > PURE_OBJECT_LIMIT - (size - remove))
        return PURE_LIMIT;
    if (!remove && !insert.size) return PURE_OK;
    Entry next;
    bool ok = rebuild(&draft->tree, draft->root, offset, remove, insert, &next);
    if (ok) draft->root = next;
    collect(draft);
    return ok ? PURE_OK : PURE_STORE_ERROR;
}

PureVersion pure_draft_version(const PureDraft *draft) {
    return (PureVersion){draft->root.ref, draft->root.bytes};
}

PureResult pure_draft_read_version(const PureDraft *draft, PureVersion version, size_t offset,
                                   size_t length, bool (*visit)(void *, PureSpan), void *context) {
    Entry root = {version.ref, version.bytes};
    unsigned height;
    if (offset > root.bytes || length > root.bytes - offset) return PURE_LIMIT;
    if (!length) return PURE_OK;
    if (!root_height(&draft->tree, root, &height)) return PURE_STORE_ERROR;
    return visit_range(&draft->tree, root, height, offset, length, visit, context) ?
           PURE_OK : PURE_STORE_ERROR;
}

PureResult pure_draft_read(const PureDraft *draft, size_t offset, size_t length,
                           bool (*visit)(void *, PureSpan), void *context) {
    return pure_draft_read_version(draft, pure_draft_version(draft), offset, length, visit, context);
}

void pure_draft_restore(PureDraft *draft, PureVersion version) {
    draft->root = (Entry){version.ref, version.bytes};
}

void pure_draft_hold(PureDraft *draft, bool held) {
    draft->held = held;
    if (!held) collect(draft);
}

/* ---- Group commits ------------------------------------------------------
 * Threads that commit at the same time share one commit: they queue, the
 * first becomes the leader and publishes every queued request of the same
 * store with one head (one pair of syncs); the others wait for its result.
 * Each request stays all or nothing on its own: a request with a stale draft
 * is left out, and so is one whose key an earlier request of the same group
 * already changes. The leader touches the waiters' drafts and handles only
 * while they wait. */
typedef struct Waiter {
    PureDraft *const *drafts;
    size_t count;
    int64_t *revisions;
    PureResult result;
    bool accepted, done;
    struct Waiter *next;
} Waiter;

static struct {
    pthread_mutex_t lock;
    pthread_cond_t finished, joined;
    Waiter *head, *tail;
    size_t queued, expected; /* expected: how many requests groups have held lately */
    bool leading;
    unsigned long groups, requests;
} group = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, PTHREAD_COND_INITIALIZER, NULL, NULL, 0, 1, false, 0, 0};

void pure_store_group_counts(unsigned long *groups, unsigned long *requests) {
    pthread_mutex_lock(&group.lock);
    *groups = group.groups;
    *requests = group.requests;
    pthread_mutex_unlock(&group.lock);
}

enum { GATHER_NS = 2000000 };

/* Publish the queued requests of the store at path in one session. */
static void commit_group(const char *path, Waiter *first) {
    size_t total = 0;
    for (Waiter *w = first; w; w = w->next)
        if (!strcmp(w->drafts[0]->path, path)) { total += w->count; w->accepted = false; }
    Session session;
    Writer *writer = NULL;
    Entry *roots = malloc(total * sizeof(*roots));
    const char **keys = malloc(total * sizeof(*keys));
    int64_t *revisions = malloc(total * sizeof(*revisions));
    PureDraft **drafts = malloc(total * sizeof(*drafts));
    bool allocated = roots && keys && revisions && drafts, checked = false;
    PureResult result = allocated ? open_session(path, &session) : PURE_ALLOCATION;
    size_t count = 0;
    if (result == PURE_OK) {
        /* A request holds when each of its keys is at its draft's base
         * revision and no earlier request of this group changes it. */
        for (Waiter *w = first; w; w = w->next) {
            if (strcmp(w->drafts[0]->path, path)) continue;
            w->result = PURE_OK;
            for (size_t i = 0; i < w->count && w->result == PURE_OK; ++i) {
                PureDraft *d = w->drafts[i];
                Entry current;
                uint64_t index;
                int64_t stored;
                bool found;
                if (!find(&session.tree, (Entry){session.head.catalog, session.head.catalog_bytes}, d->key, &index,
                          &found, &current, &stored)) { w->result = PURE_STORE_ERROR; break; }
                for (size_t k = 0; k < count; ++k)
                    if (!strcmp(keys[k], d->key)) { ++stored; break; } /* it moves with the earlier request */
                if (stored != d->revision) { w->revisions[i] = stored; w->result = PURE_CONFLICT; }
                else if (stored == INT64_MAX) w->result = PURE_LIMIT;
            }
            if (w->result != PURE_OK) continue;
            w->accepted = true;
            for (size_t i = 0; i < w->count; ++i) {
                drafts[count] = w->drafts[i];
                roots[count] = w->drafts[i]->root;
                keys[count] = w->drafts[i]->key;
                revisions[count++] = w->drafts[i]->revision;
            }
        }
        checked = true;
        if (count) {
            result = PURE_ALLOCATION;
            if ((writer = malloc(sizeof(*writer)))) {
                *writer = (Writer){.fd = session.fd, .at = session.head.end};
                bool written = true;
                for (size_t i = 0; i < count && written; ++i) {
                    /* After compaction a draft's stored references name the old file. */
                    bool relocate = session.file.st_dev != drafts[i]->device || session.file.st_ino != drafts[i]->inode;
                    written = write_root(&drafts[i]->tree, writer, &roots[i], relocate);
                }
                if (written) result = publish(&session, writer, count, keys, roots, revisions);
                else {
                    if (ftruncate(session.fd, (off_t)session.head.end) == 0) sync_fd(session.fd);
                    result = PURE_STORE_ERROR;
                }
            }
        }
        /* Rebase while still holding the writer lock; the arenas are no longer
         * needed. Drafts of a store handle share one new mapping. If remapping
         * fails a draft keeps equal content at its new revision. */
        PureStore *refreshed[8] = {NULL};
        size_t fresh = 0;
        for (size_t i = 0; result == PURE_OK && i < count; ++i) {
            PureStore *store = drafts[i]->store;
            bool rebased;
            if (store) {
                bool seen = false;
                for (size_t k = 0; k < fresh && !seen; ++k) seen = refreshed[k] == store;
                if (!seen && refresh(store) == PURE_OK) {
                    seen = true;
                    if (fresh < 8) refreshed[fresh++] = store;
                }
                rebased = seen && rebase(drafts[i]) == PURE_OK;
            } else rebased = load_draft(drafts[i]) == PURE_OK;
            if (!rebased) drafts[i]->revision = revisions[i];
        }
    }
    /* Before the checks every request shares the failure; after them an
     * accepted request takes the publication's result and new revisions, and
     * a refused one keeps its own result. */
    size_t at = 0;
    for (Waiter *w = first; w; w = w->next) {
        if (strcmp(w->drafts[0]->path, path)) continue;
        if (!checked) { w->result = result; continue; }
        if (!w->accepted) continue;
        for (size_t i = 0; i < w->count; ++i) w->revisions[i] = revisions[at++];
        w->result = result;
    }
    free(writer);
    free(roots);
    free((void *)keys);
    free(revisions);
    free(drafts);
    if (allocated) close_session(&session);
}

PureResult pure_draft_commit_all(PureDraft *const *drafts, size_t count, int64_t *revisions) {
    for (size_t i = 0; i < count; ++i) {
        revisions[i] = drafts[i]->revision;
        if (!drafts[i]->path || strcmp(drafts[i]->path, drafts[0]->path)) return PURE_INVALID;
        for (size_t k = 0; k < i; ++k) if (!strcmp(drafts[k]->key, drafts[i]->key)) return PURE_INVALID;
    }
    if (!count) return PURE_OK;
    Waiter me = {.drafts = drafts, .count = count, .revisions = revisions};
    pthread_mutex_lock(&group.lock);
    if (group.tail) group.tail->next = &me; else group.head = &me;
    group.tail = &me;
    ++group.queued;
    pthread_cond_signal(&group.joined);
    while (!me.done) {
        if (group.leading) { pthread_cond_wait(&group.finished, &group.lock); continue; }
        group.leading = true;
        /* When several threads have been committing together, give the others
         * a moment (at most 2 ms) to join, or they would miss this group and
         * wait for the next one: a sync costs far more. One thread never waits. */
        if (group.expected > 1 && group.queued < group.expected) {
            struct timespec deadline;
            clock_gettime(CLOCK_REALTIME, &deadline);
            deadline.tv_nsec += GATHER_NS;
            if (deadline.tv_nsec >= 1000000000) { ++deadline.tv_sec; deadline.tv_nsec -= 1000000000; }
            while (group.queued < group.expected &&
                   pthread_cond_timedwait(&group.joined, &group.lock, &deadline) == 0) {}
        }
        Waiter *batch = group.head;
        size_t size = group.queued;
        group.head = group.tail = NULL;
        group.queued = 0;
        ++group.groups;
        group.requests += size;
        /* Remember the largest recent group, forgetting one member per group. */
        group.expected = size > group.expected - 1 ? size : group.expected - 1;
        if (!group.expected) group.expected = 1;
        pthread_mutex_unlock(&group.lock);
        /* One session per store among the queued requests. */
        for (Waiter *w = batch; w; w = w->next) {
            bool earlier = false;
            for (Waiter *v = batch; v != w && !earlier; v = v->next) earlier = !strcmp(v->drafts[0]->path, w->drafts[0]->path);
            if (!earlier) commit_group(w->drafts[0]->path, batch);
        }
        pthread_mutex_lock(&group.lock);
        for (Waiter *w = batch, *next; w; w = next) { next = w->next; w->done = true; }
        group.leading = false;
        pthread_cond_broadcast(&group.finished);
    }
    pthread_mutex_unlock(&group.lock);
    return me.result;
}

PureResult pure_draft_commit(PureDraft *draft, int64_t *revision) {
    return pure_draft_commit_all(&draft, 1, revision);
}

PureResult pure_store_put(const char *path, const char *key, int64_t expected,
                          PureSpan body, int64_t *revision) {
    return mutate(path, key, expected, body, false, 0, 0, PURE_OBJECT_LIMIT, revision);
}

PureResult pure_store_splice(const char *path, const char *key, int64_t expected,
                             size_t offset, size_t remove, PureSpan insert,
                             size_t max_size, int64_t *revision) {
    return mutate(path, key, expected, insert, true, offset, remove, max_size, revision);
}

#ifdef PURE_STORE_TEST
static bool shape(const Tree *tree, Entry entry, unsigned height, bool root,
                  uint64_t *leaves, uint64_t *small) {
    if (!height) {
        ++*leaves;
        if (!root && entry.bytes < LEAF_MIN) ++*small;
        return leaf_bytes(tree, entry) != NULL;
    }
    Entry kids[FAN_MAX];
    size_t count = children(tree, entry, height, kids);
    if (!count || (root && count < 2)) return false;
    if (!root && count < FAN_MIN) ++*small;
    for (size_t i = 0; i < count; ++i)
        if (!shape(tree, kids[i], height - 1, false, leaves, small)) return false;
    return true;
}

/* Test-only: live arena nodes and their payload bytes. */
void pure_draft_test_arena(const PureDraft *draft, size_t *nodes, size_t *bytes) {
    *nodes = *bytes = 0;
    for (size_t i = 0; i < draft->tree.count; ++i) {
        const Node *node = &draft->tree.nodes[i];
        if (node->height == FREE) continue;
        ++*nodes;
        *bytes += node->bytes ? node->size : node->size * sizeof(Entry);
    }
}

/* Test-only: the stored height, leaf count and number of underfull nodes. */
bool pure_tree_test_shape(const char *path, const char *key, unsigned *height,
                          uint64_t *leaves, uint64_t *small, uint64_t *file_end) {
    int directory = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    int fd = directory >= 0 ? openat(directory, tree_name, O_RDONLY | O_CLOEXEC) : -1;
    if (directory >= 0) close(directory);
    Tree tree = {0};
    Head head;
    Entry document;
    uint64_t index;
    int64_t revision;
    bool found = false;
    *leaves = *small = 0;
    bool ok = fd >= 0 && read_head(fd, &head) && map_tree(fd, &head, &tree) &&
              find(&tree, (Entry){head.catalog, head.catalog_bytes}, key, &index, &found,
                   &document, &revision) && found && root_height(&tree, document, height) &&
              (!document.bytes || shape(&tree, document, *height, true, leaves, small));
    if (ok) *file_end = head.end;
    release_tree(&tree);
    if (fd >= 0) close(fd);
    return ok;
}
#endif
