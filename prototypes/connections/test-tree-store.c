#define _POSIX_C_SOURCE 200809L
#include "store.h"
#include "state.h"
#include <assert.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); abort(); } } while (0)

static int fail_stage = -1;
static int crash;
int pure_store_test_fault(PureStoreStage stage) {
    if ((int)stage != fail_stage) return 0;
    if (crash) _exit(77);
    return 1;
}
bool pure_tree_test_shape(const char *path, const char *key, unsigned *height,
                          uint64_t *leaves, uint64_t *small, uint64_t *file_end);

void pure_draft_test_arena(const PureDraft *draft, size_t *nodes, size_t *bytes);

enum { KEYS = 4, CAPACITY = 1 << 20 };
typedef struct { unsigned char *bytes; size_t size; int64_t revision; } Oracle;

static uint32_t random_value(uint32_t *state) {
    *state ^= *state << 13;
    *state ^= *state >> 17;
    *state ^= *state << 5;
    return *state;
}

static void expect(const char *path, const char *key, const Oracle *o) {
    PureStoredView view;
    CHECK(pure_store_get(path, key, PURE_OBJECT_LIMIT, &view) == PURE_OK);
    CHECK(view.revision == o->revision && view.body.size == o->size);
    CHECK(!o->size || !memcmp(view.body.data, o->bytes, o->size));
    pure_store_release(&view);
}

static uint64_t file_size(const char *path) {
    char name[512];
    snprintf(name, sizeof(name), "%s/pure.tree", path);
    struct stat info;
    CHECK(stat(name, &info) == 0);
    return (uint64_t)info.st_size;
}

static void fill(unsigned char *bytes, size_t size, uint32_t *state) {
    for (size_t i = 0; i < size; ++i) bytes[i] = (unsigned char)random_value(state);
}

/* Random edits against a flat oracle, including views held across later
 * commits and compactions. Every stored node must stay balanced. */
static void oracle(const char *path) {
    static const char *keys[KEYS] = {"alpha", "b", "gamma-long_key-0123456789", "z"};
    Oracle docs[KEYS] = {{0}};
    unsigned char *insert = malloc(65536);
    for (unsigned k = 0; k < KEYS; ++k) CHECK((docs[k].bytes = malloc(CAPACITY)));
    CHECK(insert);
    uint32_t state = 0x9e3779b9u;
    PureStoredView held = {0};
    unsigned char *held_copy = malloc(CAPACITY);
    size_t held_size = 0;
    unsigned max_height = 0;
    size_t max_size = 0, splices = 0;
    for (unsigned step = 0; step < 6000; ++step) {
        unsigned k = random_value(&state) % KEYS;
        Oracle *o = &docs[k];
        uint32_t kind = random_value(&state) % 100;
        size_t count = kind < 70 ? random_value(&state) % 24 :
                       kind < 95 ? random_value(&state) % 6000 : random_value(&state) % 65536;
        int64_t revision;
        fill(insert, count, &state);
        if (kind >= 98 || !o->revision) {
            CHECK(pure_store_put(path, keys[k], o->revision, (PureSpan){insert, count}, &revision) == PURE_OK);
            memcpy(o->bytes, insert, count);
            o->size = count;
        } else {
            size_t offset = o->size ? random_value(&state) % (o->size + 1) : 0;
            size_t remove = o->size - offset ? random_value(&state) % (o->size - offset + 1) : 0;
            if (random_value(&state) % 24) remove = remove % 64;
            if (o->size - remove + count > CAPACITY) { count = 0; remove = o->size - offset; }
            CHECK(pure_store_splice(path, keys[k], o->revision, offset, remove,
                                    (PureSpan){insert, count}, PURE_OBJECT_LIMIT, &revision) == PURE_OK);
            memmove(o->bytes + offset + count, o->bytes + offset + remove, o->size - offset - remove);
            memcpy(o->bytes + offset, insert, count);
            o->size = o->size - remove + count;
            ++splices;
        }
        if (o->size > max_size) max_size = o->size;
        CHECK(revision == o->revision + 1);
        o->revision = revision;
        expect(path, keys[k], o);
        unsigned height;
        uint64_t leaves, small, end;
        CHECK(pure_tree_test_shape(path, keys[k], &height, &leaves, &small, &end));
        CHECK(small == 0);
        if (height > max_height) max_height = height;
        if (step % 97 == 0) {
            if (held.mapped_size) {
                CHECK(held.body.size == held_size && !memcmp(held.body.data, held_copy, held_size));
                pure_store_release(&held);
            }
            CHECK(pure_store_get(path, keys[k], PURE_OBJECT_LIMIT, &held) == PURE_OK);
            held_size = held.body.size;
            if (held_size) memcpy(held_copy, held.body.data, held_size);
        }
    }
    if (held.mapped_size) {
        CHECK(held.body.size == held_size && !memcmp(held.body.data, held_copy, held_size));
        pure_store_release(&held);
    }
    for (unsigned k = 0; k < KEYS; ++k) { expect(path, keys[k], &docs[k]); free(docs[k].bytes); }
    CHECK(max_height >= 2 && max_height <= 4 && max_size > 262144);
    printf("{\"oracle_steps\":6000,\"splices\":%zu,\"max_document_bytes\":%zu,\"max_height\":%u}\n",
           splices, max_size, max_height);
    free(insert);
    free(held_copy);
}

/* A one-byte insert, delete or replacement in a 1 MiB document appends only
 * its changed leaf and path, not another copy of the document. */
static void amplification(const char *path) {
    size_t size = 1048576;
    unsigned char *bytes = malloc(size + 1);
    uint32_t state = 7;
    CHECK(bytes);
    fill(bytes, size, &state);
    int64_t revision;
    CHECK(pure_store_put(path, "large", 0, (PureSpan){bytes, size}, &revision) == PURE_OK);
    Oracle o = {bytes, size, revision};
    unsigned char byte = 'Q';
    const size_t edits[][3] = {{size / 2, 0, 1}, {size / 3, 1, 0}, {size - 1, 1, 1}, {0, 0, 1}};
    uint64_t worst = 0;
    for (size_t i = 0; i < sizeof(edits) / sizeof(*edits); ++i) {
        uint64_t before = file_size(path);
        size_t offset = edits[i][0], remove = edits[i][1], count = edits[i][2];
        CHECK(pure_store_splice(path, "large", o.revision, offset, remove, (PureSpan){&byte, count},
                                PURE_OBJECT_LIMIT, &revision) == PURE_OK);
        memmove(bytes + offset + count, bytes + offset + remove, o.size - offset - remove);
        memcpy(bytes + offset, &byte, count);
        o.size = o.size - remove + count;
        o.revision = revision;
        uint64_t after = file_size(path);
        if (after > before && after - before > worst) worst = after - before;
        expect(path, "large", &o);
    }
    /* Compaction may shrink the file; growth per edit is the claim. */
    CHECK(worst > 0 && worst < 16384);
    unsigned height;
    uint64_t leaves, small, end;
    CHECK(pure_tree_test_shape(path, "large", &height, &leaves, &small, &end));
    CHECK(small == 0 && height == 2 && leaves >= 256 && leaves <= 512);
    printf("{\"one_byte_edit_max_appended_bytes\":%llu,\"height\":%u,\"leaves\":%llu}\n",
           (unsigned long long)worst, height, (unsigned long long)leaves);
    free(bytes);
}

static void faults(const char *path) {
    unsigned char text[] = "fault-free";
    int64_t revision;
    CHECK(pure_store_put(path, "fault", 0, (PureSpan){text, 10}, &revision) == PURE_OK);
    Oracle o = {text, 10, revision};
    uint64_t size = file_size(path);
    for (fail_stage = PURE_AFTER_HEADER; fail_stage <= PURE_BEFORE_RENAME; ++fail_stage) {
        CHECK(pure_store_splice(path, "fault", o.revision, 1, 3, (PureSpan){text, 5},
                                PURE_OBJECT_LIMIT, &revision) == PURE_STORE_ERROR);
        expect(path, "fault", &o);
        CHECK(file_size(path) == size);
    }
    fail_stage = PURE_BEFORE_DIR_SYNC;
    CHECK(pure_store_splice(path, "fault", o.revision, 10, 0, (PureSpan){text, 2},
                            PURE_OBJECT_LIMIT, &revision) == PURE_UNCERTAIN);
    fail_stage = -1;
    unsigned char next[] = "fault-freefa";
    o = (Oracle){next, 12, o.revision + 1};
    CHECK(revision == o.revision);
    expect(path, "fault", &o);
}

/* A process that dies at each publication stage leaves either the previous
 * or the next complete version, and its unpublished bytes are discarded. */
static void crashes(const char *path) {
    unsigned char text[] = "crash-base";
    int64_t revision;
    CHECK(pure_store_put(path, "crash", 0, (PureSpan){text, 10}, &revision) == PURE_OK);
    Oracle o = {text, 10, revision};
    for (int stage = PURE_AFTER_HEADER; stage <= PURE_BEFORE_DIR_SYNC; ++stage) {
        unsigned height;
        uint64_t leaves, small, before, end;
        CHECK(pure_tree_test_shape(path, "crash", &height, &leaves, &small, &before));
        pid_t child = fork();
        CHECK(child >= 0);
        if (!child) {
            crash = 1;
            fail_stage = stage;
            pure_store_splice(path, "crash", o.revision, 0, 1, (PureSpan){(const unsigned char *)"C", 1},
                              PURE_OBJECT_LIMIT, &revision);
            _exit(0);
        }
        int status;
        CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 77);
        if (stage == PURE_BEFORE_DIR_SYNC) { text[0] = 'C'; ++o.revision; }
        expect(path, "crash", &o);
        uint64_t garbage = file_size(path);
        CHECK(pure_store_splice(path, "crash", o.revision, 10, 0, (PureSpan){(const unsigned char *)"", 0},
                                PURE_OBJECT_LIMIT, &revision) == PURE_OK);
        o.revision = revision;
        CHECK(pure_tree_test_shape(path, "crash", &height, &leaves, &small, &end));
        CHECK(end == file_size(path));
        /* Same-sized commit: equal growth only if the dead writer's tail was cut. */
        if (stage != PURE_BEFORE_DIR_SYNC) CHECK(garbage > before && end - before == garbage - before);
    }
}

/* A torn newest head slot exposes the previous commit, and the next writer
 * overwrites that slot and the unpublished tail. */
static void torn_head(const char *path) {
    unsigned char first[] = "first", second[] = "second";
    int64_t revision;
    CHECK(pure_store_put(path, "torn", 0, (PureSpan){first, 5}, &revision) == PURE_OK);
    Oracle o = {first, 5, revision};
    CHECK(pure_store_put(path, "torn", o.revision, (PureSpan){second, 6}, &revision) == PURE_OK);
    char name[512];
    snprintf(name, sizeof(name), "%s/pure.tree", path);
    int fd = open(name, O_RDWR);
    unsigned char slots[128];
    CHECK(fd >= 0 && pread(fd, slots, 128, 0) == 128);
    uint64_t a = 0, b = 0;
    for (unsigned i = 0; i < 8; ++i) { a |= (uint64_t)slots[8 + i] << (8 * i); b |= (uint64_t)slots[72 + i] << (8 * i); }
    off_t newest = a > b ? 0 : 64;
    unsigned char torn = slots[newest + 20] ^ 0x5a;
    CHECK(pwrite(fd, &torn, 1, newest + 20) == 1 && close(fd) == 0);
    expect(path, "torn", &o);
    CHECK(pure_store_put(path, "torn", o.revision + 1, (PureSpan){first, 1}, &revision) == PURE_CONFLICT);
    CHECK(pure_store_put(path, "torn", o.revision, (PureSpan){second, 3}, &revision) == PURE_OK);
    o = (Oracle){second, 3, revision};
    expect(path, "torn", &o);
}

/* A reader's view stays byte-identical while another process commits and
 * compacts; readers take no lock. */
static void isolation(const char *path) {
    size_t size = 300000;
    unsigned char *bytes = malloc(size);
    uint32_t state = 99;
    CHECK(bytes);
    fill(bytes, size, &state);
    int64_t revision;
    CHECK(pure_store_put(path, "shared", 0, (PureSpan){bytes, size}, &revision) == PURE_OK);
    PureStoredView view;
    CHECK(pure_store_get(path, "shared", size, &view) == PURE_OK);
    uint64_t before = file_size(path);
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        int64_t at = revision;
        unsigned char byte = 'x';
        for (unsigned i = 0; i < 400; ++i)
            if (pure_store_splice(path, "shared", at, (i * 7919u) % size, 1, (PureSpan){&byte, 1},
                                  PURE_OBJECT_LIMIT, &at) != PURE_OK) _exit(1);
        _exit(0);
    }
    int status;
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    CHECK(view.revision == revision && view.body.size == size && !memcmp(view.body.data, bytes, size));
    pure_store_release(&view);
    PureStoredView latest;
    CHECK(pure_store_get(path, "shared", size, &latest) == PURE_OK && latest.revision == revision + 400);
    pure_store_release(&latest);
    /* About 2 MB of path copies without compaction would exceed this. */
    CHECK(file_size(path) < before + 3 * 524288);
    free(bytes);
}

/* Damaged branch bytes are reported as storage errors, never trusted. */
static void corruption(const char *path) {
    unsigned char *bytes = malloc(200000);
    uint32_t state = 3;
    CHECK(bytes);
    fill(bytes, 200000, &state);
    int64_t revision;
    CHECK(pure_store_put(path, "damaged", 0, (PureSpan){bytes, 200000}, &revision) == PURE_OK);
    char name[512];
    snprintf(name, sizeof(name), "%s/pure.tree", path);
    uint64_t size = file_size(path);
    int fd = open(name, O_RDWR);
    CHECK(fd >= 0);
    unsigned errors = 0;
    for (unsigned i = 0; i < 3000; ++i) {
        off_t at = (off_t)(128 + random_value(&state) % (size - 128));
        unsigned char original, damaged;
        CHECK(pread(fd, &original, 1, at) == 1);
        damaged = original ^ (unsigned char)(1u << (random_value(&state) % 8));
        CHECK(pwrite(fd, &damaged, 1, at) == 1);
        PureStoredView view;
        PureResult result = pure_store_get(path, "damaged", PURE_OBJECT_LIMIT, &view);
        CHECK(result == PURE_OK || result == PURE_STORE_ERROR || result == PURE_NOT_FOUND);
        if (result == PURE_OK) pure_store_release(&view); else ++errors;
        CHECK(pwrite(fd, &original, 1, at) == 1);
    }
    CHECK(close(fd) == 0 && errors > 0);
    free(bytes);
}

static void many_keys(const char *path) {
    int64_t revision;
    char key[32];
    for (unsigned i = 0; i < 1024; ++i) {
        unsigned id = (i * 617u) % 1024u;
        snprintf(key, sizeof(key), "doc-%04u", id);
        CHECK(pure_store_put(path, key, 0, (PureSpan){(const unsigned char *)key, strlen(key)}, &revision) == PURE_OK);
    }
    for (unsigned id = 0; id < 1024; ++id) {
        snprintf(key, sizeof(key), "doc-%04u", id);
        Oracle o = {(unsigned char *)key, strlen(key), 1};
        expect(path, key, &o);
    }
    PureStoredView view;
    CHECK(pure_store_get(path, "doc-1024", 64, &view) == PURE_NOT_FOUND);
    CHECK(pure_store_get(path, "doc-0001", 3, &view) == PURE_BUDGET);
    CHECK(pure_store_splice(path, "doc-0002", 1, 9, 0, (PureSpan){0}, PURE_OBJECT_LIMIT, &revision) == PURE_LIMIT);
    CHECK(pure_store_put(path, "doc-0003", 0, (PureSpan){0}, &revision) == PURE_CONFLICT && revision == 1);
}

/* The newest catalog of a one-key store is one leaf; forge its revision. */
static void revision_limit(const char *path) {
    int64_t revision;
    CHECK(pure_store_put(path, "work", 0, (PureSpan){(const unsigned char *)"w", 1}, &revision) == PURE_OK);
    char name[512];
    snprintf(name, sizeof(name), "%s/pure.tree", path);
    int fd = open(name, O_RDWR);
    unsigned char slots[128];
    CHECK(fd >= 0 && pread(fd, slots, 128, 0) == 128);
    uint64_t seq[2] = {0, 0}, catalog = 0;
    for (unsigned s = 0; s < 2; ++s)
        for (unsigned i = 0; i < 8; ++i) seq[s] |= (uint64_t)slots[64 * s + 8 + i] << (8 * i);
    unsigned newest = seq[0] > seq[1] ? 0 : 1;
    for (unsigned i = 0; i < 8; ++i) catalog |= (uint64_t)slots[64 * newest + 24 + i] << (8 * i);
    CHECK(catalog >> 63);
    const unsigned char maximum[] = {255,255,255,255,255,255,255,127};
    off_t record = (off_t)(catalog & ((UINT64_C(1) << 62) - 1));
    CHECK(pwrite(fd, maximum, 8, record + 80) == 8 && close(fd) == 0);
    PureStoredView view;
    CHECK(pure_store_get(path, "work", 1, &view) == PURE_OK && view.revision == INT64_MAX);
    pure_store_release(&view);
    CHECK(pure_store_splice(path, "work", INT64_MAX, 0, 0, (PureSpan){0}, PURE_OBJECT_LIMIT, &revision) == PURE_LIMIT);
}

static bool count_key(void *context, const char *key) {
    (void)key;
    ++*(unsigned *)context;
    return true;
}

static void keys(const char *path) {
    unsigned count = 0;
    CHECK(pure_store_keys(path, count_key, &count) == PURE_OK && count == 0);
    int64_t revision;
    const char *names[] = {"m", "a", "zz", "a-b", "A"};
    for (unsigned i = 0; i < 5; ++i)
        CHECK(pure_store_put(path, names[i], 0, (PureSpan){0}, &revision) == PURE_OK);
    CHECK(pure_store_keys(path, count_key, &count) == PURE_OK && count == 5);
}

typedef struct { unsigned char *out; size_t spans, largest, limit; } Gather;

static bool gather(void *context, PureSpan span) {
    Gather *g = context;
    memcpy(g->out, span.data, span.size);
    g->out += span.size;
    if (span.size > g->largest) g->largest = span.size;
    return ++g->spans != g->limit;
}

static void expect_draft(const PureDraft *draft, const Oracle *o, unsigned char *scratch) {
    Gather g = {scratch, 0, 0, 0};
    CHECK(pure_draft_size(draft) == o->size && pure_draft_revision(draft) == o->revision);
    CHECK(pure_draft_read(draft, 0, o->size, gather, &g) == PURE_OK);
    CHECK((size_t)(g.out - scratch) == o->size && (!o->size || !memcmp(scratch, o->bytes, o->size)));
    CHECK(g.largest <= 4096);
}

static void apply(Oracle *o, size_t offset, size_t remove, const unsigned char *insert, size_t count) {
    memmove(o->bytes + offset + count, o->bytes + offset + remove, o->size - offset - remove);
    if (count) memcpy(o->bytes + offset, insert, count);
    o->size = o->size - remove + count;
}

/* Edits live only in the draft until commit; each commit continues from the
 * published version with an empty arena. */
static void drafts(const char *path) {
    Oracle o = {malloc(CAPACITY), 0, 0};
    unsigned char *scratch = malloc(CAPACITY), *insert = malloc(65536);
    CHECK(o.bytes && scratch && insert);
    PureDraft *draft;
    PureStoredView view;
    CHECK(pure_draft_open(path, "draft", &draft) == PURE_OK);
    expect_draft(draft, &o, scratch);
    uint32_t state = 0x1234567u;
    for (unsigned step = 0; step < 20000; ++step) {
        uint32_t kind = random_value(&state) % 100;
        size_t count = kind < 80 ? random_value(&state) % 8 :
                       kind < 98 ? random_value(&state) % 4000 : random_value(&state) % 65536;
        size_t offset = o.size ? random_value(&state) % (o.size + 1) : 0;
        size_t remove = o.size - offset ? random_value(&state) % (o.size - offset + 1) : 0;
        if (random_value(&state) % 64) remove %= 16;
        if (o.size - remove + count > CAPACITY) count = 0;
        fill(insert, count, &state);
        CHECK(pure_draft_splice(draft, offset, remove, (PureSpan){insert, count}) == PURE_OK);
        apply(&o, offset, remove, insert, count);
        if (step % 97 == 0) expect_draft(draft, &o, scratch);
        if (step % 1000 == 999) {
            if (o.revision) CHECK(pure_store_get(path, "draft", PURE_OBJECT_LIMIT, &view) == PURE_OK &&
                                  view.revision == o.revision);
            if (o.revision) pure_store_release(&view);
            int64_t revision;
            CHECK(pure_draft_commit(draft, &revision) == PURE_OK && revision == o.revision + 1);
            o.revision = revision;
            expect(path, "draft", &o);
            expect_draft(draft, &o, scratch);
            size_t nodes, bytes;
            pure_draft_test_arena(draft, &nodes, &bytes);
            CHECK(nodes == 0);
        }
    }
    CHECK(o.size > 100000);
    Gather g = {scratch, 0, 0, 3};
    CHECK(pure_draft_read(draft, 0, o.size, gather, &g) == PURE_OK && g.spans == 3);
    CHECK(pure_draft_splice(draft, o.size + 1, 0, (PureSpan){0}) == PURE_LIMIT);
    CHECK(pure_draft_splice(draft, 0, o.size + 1, (PureSpan){0}) == PURE_LIMIT);
    CHECK(pure_draft_read(draft, o.size, 1, gather, &g) == PURE_LIMIT);
    expect_draft(draft, &o, scratch);
    pure_draft_close(draft);
    printf("{\"draft_steps\":20000,\"draft_commits\":%lld,\"draft_bytes\":%zu}\n",
           (long long)o.revision, o.size);
    free(o.bytes);
    free(scratch);
    free(insert);
}

/* Long typing and deleting at one place keeps only the live path in memory,
 * and its commit appends the typed bytes and path, not the document. */
static void typing(const char *path) {
    size_t size = 1048576;
    Oracle o = {malloc(size + 40000), size, 0};
    unsigned char *scratch = malloc(size + 40000);
    uint32_t state = 5;
    CHECK(o.bytes && scratch);
    fill(o.bytes, size, &state);
    CHECK(pure_store_put(path, "typed", 0, (PureSpan){o.bytes, size}, &o.revision) == PURE_OK);
    PureDraft *draft;
    CHECK(pure_draft_open(path, "typed", &draft) == PURE_OK);
    size_t cursor = size / 2, nodes, bytes, peak = 0;
    for (unsigned i = 0; i < 30000; ++i) {
        unsigned char byte = (unsigned char)('a' + i % 26);
        if (i % 3 == 2) {
            CHECK(pure_draft_splice(draft, cursor - 1, 1, (PureSpan){0}) == PURE_OK);
            apply(&o, --cursor, 1, NULL, 0);
        } else {
            CHECK(pure_draft_splice(draft, cursor, 0, (PureSpan){&byte, 1}) == PURE_OK);
            apply(&o, cursor++, 0, &byte, 1);
        }
        pure_draft_test_arena(draft, &nodes, &bytes);
        if (bytes > peak) peak = bytes;
    }
    expect_draft(draft, &o, scratch);
    /* Reachable: 10,000 net typed bytes, the leaves they landed in and one
     * path. Unreachable copies are reclaimed past max(reachable, 32 KiB). */
    CHECK(peak < 2 * (10000 + 4 * 4096 + 2048) + 32768 + 8192);
    uint64_t before = file_size(path);
    int64_t revision;
    CHECK(pure_draft_commit(draft, &revision) == PURE_OK);
    o.revision = revision;
    CHECK(file_size(path) - before < 10000 + 16384);
    expect(path, "typed", &o);
    pure_draft_close(draft);
    printf("{\"typing_keystrokes\":30000,\"peak_arena_bytes\":%zu,\"commit_appended_bytes\":%llu}\n",
           peak, (unsigned long long)(file_size(path) - before));
    free(o.bytes);
    free(scratch);
}

/* Two drafts of one key: the second commit conflicts and keeps its edits. */
static void draft_conflict(const char *path) {
    unsigned char scratch[64];
    int64_t revision;
    CHECK(pure_store_put(path, "shared", 0, (PureSpan){(const unsigned char *)"base", 4}, &revision) == PURE_OK);
    PureDraft *first, *second;
    CHECK(pure_draft_open(path, "shared", &first) == PURE_OK && pure_draft_open(path, "shared", &second) == PURE_OK);
    CHECK(pure_draft_splice(first, 4, 0, (PureSpan){(const unsigned char *)"-one", 4}) == PURE_OK);
    CHECK(pure_draft_splice(second, 0, 0, (PureSpan){(const unsigned char *)"two-", 4}) == PURE_OK);
    CHECK(pure_draft_commit(first, &revision) == PURE_OK && revision == 2);
    CHECK(pure_draft_commit(second, &revision) == PURE_CONFLICT && revision == 2);
    Oracle kept = {(unsigned char *)"two-base", 8, 1};
    expect_draft(second, &kept, scratch);
    Oracle stored = {(unsigned char *)"base-one", 8, 2};
    expect(path, "shared", &stored);
    pure_draft_close(second);
    CHECK(pure_draft_open(path, "shared", &second) == PURE_OK);
    expect_draft(second, &stored, scratch);
    CHECK(pure_store_put(path, "shared", 2, (PureSpan){(const unsigned char *)"x", 1}, &revision) == PURE_OK);
    CHECK(pure_draft_commit(first, &revision) == PURE_CONFLICT && revision == 3);
    pure_draft_close(first);
    pure_draft_close(second);
}

/* Another key's writes compact the file under an open draft; the draft's
 * stored extents are copied from its old mapping on commit. */
static void draft_relocation(const char *path) {
    unsigned char *big = malloc(300000), scratch[20000];
    uint32_t state = 11;
    CHECK(big);
    fill(big, 300000, &state);
    Oracle o = {malloc(20000), 12000, 0};
    CHECK(o.bytes);
    fill(o.bytes, o.size, &state);
    CHECK(pure_store_put(path, "kept", 0, (PureSpan){o.bytes, o.size}, &o.revision) == PURE_OK);
    PureDraft *draft;
    CHECK(pure_draft_open(path, "kept", &draft) == PURE_OK);
    CHECK(pure_draft_splice(draft, 6000, 10, (PureSpan){(const unsigned char *)"relocated", 9}) == PURE_OK);
    apply(&o, 6000, 10, (const unsigned char *)"relocated", 9);
    char name[512];
    snprintf(name, sizeof(name), "%s/pure.tree", path);
    struct stat before, after;
    CHECK(stat(name, &before) == 0);
    int64_t revision = 0;
    for (unsigned i = 0; i < 8; ++i)
        CHECK(pure_store_put(path, "other", revision, (PureSpan){big, 300000 - i}, &revision) == PURE_OK);
    CHECK(stat(name, &after) == 0 && after.st_ino != before.st_ino);
    CHECK(pure_draft_commit(draft, &revision) == PURE_OK && revision == o.revision + 1);
    o.revision = revision;
    expect(path, "kept", &o);
    expect_draft(draft, &o, scratch);
    Oracle other = {big, 300000 - 7, 8};
    expect(path, "other", &other);
    pure_draft_close(draft);
    free(o.bytes);
    free(big);
}

/* A held draft keeps earlier versions readable across later edits. */
static void draft_versions(const char *path) {
    static unsigned char bytes[200000], scratch[200000];
    uint32_t state = 17;
    fill(bytes, sizeof(bytes), &state);
    int64_t revision;
    CHECK(pure_store_put(path, "versioned", 0, (PureSpan){bytes, sizeof(bytes)}, &revision) == PURE_OK);
    PureDraft *draft;
    CHECK(pure_draft_open(path, "versioned", &draft) == PURE_OK);
    pure_draft_hold(draft, true);
    PureVersion first = pure_draft_version(draft);
    for (unsigned i = 0; i < 400; ++i) {
        unsigned char byte = (unsigned char)i;
        CHECK(pure_draft_splice(draft, (i * 977u) % 150000, 3, (PureSpan){&byte, 1}) == PURE_OK);
    }
    Gather g = {scratch, 0, 0, 0};
    CHECK(pure_draft_read_version(draft, first, 0, sizeof(bytes), gather, &g) == PURE_OK);
    CHECK((size_t)(g.out - scratch) == sizeof(bytes) && !memcmp(scratch, bytes, sizeof(bytes)));
    CHECK(pure_draft_size(draft) == sizeof(bytes) - 800);
    size_t nodes, held_bytes, after;
    pure_draft_test_arena(draft, &nodes, &held_bytes);
    pure_draft_hold(draft, false);
    pure_draft_test_arena(draft, &nodes, &after);
    CHECK(after < held_bytes);
    pure_draft_close(draft);
}

/* A failed commit changes neither the store nor the draft. */
static void draft_faults(const char *path) {
    unsigned char scratch[64];
    int64_t revision;
    CHECK(pure_store_put(path, "faulty", 0, (PureSpan){(const unsigned char *)"stable", 6}, &revision) == PURE_OK);
    PureDraft *draft;
    CHECK(pure_draft_open(path, "faulty", &draft) == PURE_OK);
    CHECK(pure_draft_splice(draft, 6, 0, (PureSpan){(const unsigned char *)"+edit", 5}) == PURE_OK);
    Oracle stored = {(unsigned char *)"stable", 6, 1}, edited = {(unsigned char *)"stable+edit", 11, 1};
    uint64_t size = file_size(path);
    for (fail_stage = PURE_AFTER_HEADER; fail_stage <= PURE_BEFORE_RENAME; ++fail_stage) {
        CHECK(pure_draft_commit(draft, &revision) == PURE_STORE_ERROR);
        expect(path, "faulty", &stored);
        expect_draft(draft, &edited, scratch);
        CHECK(file_size(path) == size);
    }
    fail_stage = PURE_BEFORE_DIR_SYNC;
    CHECK(pure_draft_commit(draft, &revision) == PURE_UNCERTAIN && revision == 2);
    fail_stage = -1;
    expect_draft(draft, &edited, scratch);
    CHECK(pure_draft_commit(draft, &revision) == PURE_CONFLICT && revision == 2);
    pure_draft_close(draft);
    CHECK(pure_draft_open(path, "faulty", &draft) == PURE_OK);
    edited.revision = 2;
    expect_draft(draft, &edited, scratch);
    CHECK(pure_draft_splice(draft, 0, 6, (PureSpan){0}) == PURE_OK);
    CHECK(pure_draft_commit(draft, &revision) == PURE_OK && revision == 3);
    Oracle final = {(unsigned char *)"+edit", 5, 3};
    expect(path, "faulty", &final);
    pure_draft_close(draft);
    CHECK(pure_draft_open(path, "bad/key", &draft) == PURE_INVALID && !draft);
}

static void clean(const char *path) {
    DIR *entries = opendir(path);
    CHECK(entries);
    struct dirent *entry;
    while ((entry = readdir(entries))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        CHECK(strncmp(entry->d_name, ".pending-", 9));
        CHECK(unlinkat(dirfd(entries), entry->d_name, 0) == 0);
    }
    CHECK(closedir(entries) == 0 && rmdir(path) == 0);
}

int main(void) {
    void (*checks[])(const char *) = {oracle, amplification, faults, crashes, torn_head,
                                      isolation, corruption, many_keys, revision_limit, keys,
                                      drafts, typing, draft_conflict, draft_relocation, draft_faults,
                                      draft_versions};
    for (size_t i = 0; i < sizeof(checks) / sizeof(*checks); ++i) {
        char path[] = "/tmp/pure-tree-check-XXXXXX";
        CHECK(mkdtemp(path) && pure_store_init(path) == PURE_OK);
        checks[i](path);
        CHECK(pure_store_init(path) == PURE_OK);
        clean(path);
    }
    puts("{\"tree_store\":\"random-oracle-balance-held-views-one-byte-amplification-faults-"
         "process-crash-torn-head-lockless-isolation-compaction-corruption-1024-keys-revision-limit-key-listing-"
         "draft-oracle-typing-arena-conflict-relocation-commit-faults-held-versions\"}");
    return 0;
}
