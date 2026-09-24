#define _POSIX_C_SOURCE 200809L
#include "store.h"
#include <assert.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

enum { PAGE = 16384, SIZE = 3 * PAGE + 23 };
static int fail_stage = -1;
static int crash;
int pure_store_test_fault(PureStoreStage stage) {
    if ((int)stage != fail_stage) return 0;
    if (crash) _exit(77);
    return 1;
}

static void expect(const char *path, int64_t revision, const unsigned char *bytes) {
    PureStoredView view;
    assert(pure_store_get(path, "large", SIZE, &view) == PURE_OK);
    assert(view.revision == revision && view.body.size == SIZE &&
           !memcmp(view.body.data, bytes, SIZE));
    pure_store_release(&view);
}

static uint32_t random_value(uint32_t *state) {
    *state ^= *state << 13;
    *state ^= *state >> 17;
    *state ^= *state << 5;
    return *state;
}

static void expect_root(const char *path, const char *key,
                        const char magic[8], off_t size) {
    char filename[320];
    snprintf(filename, sizeof(filename), "%s/obj-%s", path, key);
    struct stat info;
    assert(stat(filename, &info) == 0 && info.st_size == size);
    int fd = open(filename, O_RDONLY);
    assert(fd >= 0);
    char actual[8];
    assert(read(fd, actual, sizeof(actual)) == sizeof(actual));
    assert(!memcmp(actual, magic, sizeof(actual)));
    assert(close(fd) == 0);
}

static void format_transitions(const char *path) {
    unsigned char bytes[PAGE + 1];
    memset(bytes, 'A', sizeof(bytes));
    int64_t revision;
    assert(pure_store_put(path, "boundary", 0, (PureSpan){bytes, PAGE},
                          &revision) == PURE_OK);
    expect_root(path, "boundary", "PUREB001", 24 + PAGE);
    PureStoredView old;
    assert(pure_store_get(path, "boundary", PAGE + 1, &old) == PURE_OK);
    assert(pure_store_splice(path, "boundary", revision, PAGE, 0,
                             (PureSpan){bytes, 1}, PURE_OBJECT_LIMIT, &revision) == PURE_OK);
    expect_root(path, "boundary", "PUREP001", 48 + 2 * 8);
    assert(old.body.size == PAGE && old.body.data[PAGE - 1] == 'A');
    pure_store_release(&old);
    PureStoredView large;
    assert(pure_store_get(path, "boundary", PAGE + 1, &large) == PURE_OK);
    assert(large.body.size == PAGE + 1 && large.body.data[PAGE] == 'A');
    assert(pure_store_splice(path, "boundary", revision, PAGE, 1,
                             (PureSpan){NULL, 0}, PURE_OBJECT_LIMIT, &revision) == PURE_OK);
    expect_root(path, "boundary", "PUREB001", 24 + PAGE);
    assert(large.body.size == PAGE + 1 && large.body.data[PAGE] == 'A');
    pure_store_release(&large);
    PureStoredView current;
    assert(pure_store_get(path, "boundary", PAGE, &current) == PURE_OK);
    assert(current.revision == 3 && current.body.size == PAGE &&
           !memcmp(current.body.data, bytes, PAGE));
    pure_store_release(&current);

    const unsigned char one = 'X', two = 'Y';
    assert(pure_store_put(path, "tiny", 0, (PureSpan){&one, 1}, &revision) == PURE_OK);
    expect_root(path, "tiny", "PUREB001", 25);
    assert(pure_store_splice(path, "tiny", revision, 1, 0,
                             (PureSpan){&two, 1}, 1, &revision) == PURE_LIMIT);
    assert(revision == 1);
    PureStoredView policy_view;
    assert(pure_store_get(path, "tiny", 1, &policy_view) == PURE_OK);
    assert(policy_view.revision == 1 && policy_view.body.size == 1 && policy_view.body.data[0] == one);
    pure_store_release(&policy_view);
    fail_stage = PURE_BEFORE_RENAME;
    assert(pure_store_splice(path, "tiny", revision, 0, 1,
                             (PureSpan){&two, 1}, PURE_OBJECT_LIMIT, &revision) == PURE_STORE_ERROR);
    fail_stage = -1;
    assert(pure_store_get(path, "tiny", 1, &current) == PURE_OK);
    assert(current.revision == 1 && current.body.data[0] == one);
    pure_store_release(&current);
    fail_stage = PURE_BEFORE_DIR_SYNC;
    assert(pure_store_splice(path, "tiny", revision, 0, 1,
                             (PureSpan){&two, 1}, PURE_OBJECT_LIMIT, &revision) == PURE_UNCERTAIN);
    fail_stage = -1;
    assert(pure_store_get(path, "tiny", 1, &current) == PURE_OK);
    assert(current.revision == 2 && current.body.data[0] == two);
    pure_store_release(&current);

    /* An existing flat-store object has no data segment. Its first large
     * edit must read the old inline body before publishing a page root. */
    char legacy_file[320];
    snprintf(legacy_file, sizeof(legacy_file), "%s/obj-legacy", path);
    int fd = open(legacy_file, O_WRONLY | O_CREAT | O_EXCL, 0600);
    assert(fd >= 0);
    unsigned char header[24] = "PUREB001";
    header[8] = 1;
    header[16] = 1;
    header[17] = 0x40; /* 0x4001 = 16 KiB + 1 */
    assert(write(fd, header, sizeof(header)) == sizeof(header));
    assert(write(fd, bytes, sizeof(bytes)) == sizeof(bytes));
    assert(close(fd) == 0);
    assert(pure_store_get(path, "legacy", PAGE + 1, &current) == PURE_OK);
    assert(current.revision == 1 && current.body.size == PAGE + 1);
    pure_store_release(&current);
    assert(pure_store_splice(path, "legacy", 1, PAGE, 1,
                             (PureSpan){&two, 1}, PURE_OBJECT_LIMIT, &revision) == PURE_OK);
    expect_root(path, "legacy", "PUREP001", 48 + 2 * 8);
    assert(pure_store_get(path, "legacy", PAGE + 1, &current) == PURE_OK);
    assert(current.revision == 2 && current.body.size == PAGE + 1 &&
           current.body.data[PAGE] == two);
    pure_store_release(&current);
}

static void mixed_edits(const char *path) {
    enum { CAPACITY = 6 * PAGE };
    unsigned char *oracle = malloc(CAPACITY), *before = malloc(CAPACITY);
    unsigned char insert[4096];
    assert(oracle && before);
    size_t length = 2 * PAGE + 17;
    uint32_t state = 0xA17E532Du;
    for (size_t i = 0; i < length; ++i) oracle[i] = (unsigned char)random_value(&state);
    int64_t revision;
    assert(pure_store_put(path, "mixed", 0, (PureSpan){oracle, length}, &revision) == PURE_OK);
    for (unsigned step = 0; step < 128; ++step) {
        size_t offset = random_value(&state) % (length + 1);
        size_t remove = random_value(&state) % 4097;
        if (remove > length - offset) remove = length - offset;
        size_t count = step % 3 ? random_value(&state) % 4097 : remove;
        if (count > CAPACITY - (length - remove)) count = CAPACITY - (length - remove);
        for (size_t i = 0; i < count; ++i) insert[i] = (unsigned char)random_value(&state);
        PureStoredView old;
        assert(pure_store_get(path, "mixed", CAPACITY, &old) == PURE_OK);
        if (length) memcpy(before, old.body.data, length);
        assert(pure_store_splice(path, "mixed", revision, offset, remove,
                                 (PureSpan){insert, count}, PURE_OBJECT_LIMIT, &revision) == PURE_OK);
        assert(old.body.size == length &&
               (!length || !memcmp(old.body.data, before, length)));
        pure_store_release(&old);
        memmove(oracle + offset + count, oracle + offset + remove, length - offset - remove);
        memcpy(oracle + offset, insert, count);
        length = length - remove + count;
        PureStoredView current;
        assert(pure_store_get(path, "mixed", CAPACITY, &current) == PURE_OK);
        assert(current.revision == (int64_t)step + 2 && current.body.size == length &&
               (!length || !memcmp(current.body.data, oracle, length)));
        pure_store_release(&current);
    }
    free(before);
    free(oracle);
}

int main(void) {
    long host_page = sysconf(_SC_PAGESIZE);
    assert(host_page > 0 && PAGE % host_page == 0);
    char base[] = "/tmp/pure-page-store-XXXXXX";
    assert(mkdtemp(base));
    char path[256];
    snprintf(path, sizeof(path), "%s/store", base);
    assert(pure_store_init(path) == PURE_OK);
    unsigned char expected[SIZE];
    for (size_t i = 0; i < SIZE; ++i) expected[i] = (unsigned char)(i * 17u + i / PAGE);
    int64_t revision;
    assert(pure_store_put(path, "large", 0, (PureSpan){expected, SIZE}, &revision) == PURE_OK);
    assert(revision == 1);
    PureStoredView old;
    assert(pure_store_get(path, "large", SIZE, &old) == PURE_OK);
    unsigned char original = old.body.data[PAGE - 1];
    const unsigned char replacement = 0xC7;
    assert(pure_store_splice(path, "large", revision, PAGE - 1, 1,
                             (PureSpan){&replacement, 1}, PURE_OBJECT_LIMIT, &revision) == PURE_OK);
    expected[PAGE - 1] = replacement;
    assert(revision == 2 && old.body.data[PAGE - 1] == original);
    expect(path, revision, expected);
    PureStoredView range;
    assert(pure_store_get_range(path, "large", PAGE - 3, 9, 9, &range) == PURE_OK);
    assert(!memcmp(range.body.data, expected + PAGE - 3, 9));
    pure_store_release(&range);
    char file[320];
    struct stat info;
    snprintf(file, sizeof(file), "%s/dat-large-%016x", path, 1);
    assert(stat(file, &info) == 0 && info.st_size == (off_t)(5 * PAGE));

    /* The bounded append log compacts into a new generation. The old view
     * remains valid even after the old generation has been unlinked. */
    for (unsigned i = 0; i < 3; ++i) {
        unsigned char value = (unsigned char)(0xA0 + i);
        assert(pure_store_splice(path, "large", revision, SIZE - 1, 1,
                                 (PureSpan){&value, 1}, PURE_OBJECT_LIMIT, &revision) == PURE_OK);
        expected[SIZE - 1] = value;
        expect(path, revision, expected);
    }
    assert(access(file, F_OK) != 0 && old.body.data[PAGE - 1] == original);
    pure_store_release(&old);

    /* Unpublished crashes may leave an orphan generation. They never change
     * the visible root; the next writer must skip the collided generation. */
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        fail_stage = PURE_BEFORE_RENAME;
        crash = 1;
        pure_store_put(path, "large", revision, (PureSpan){expected, SIZE}, &revision);
        _exit(78);
    }
    int status;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 77);
    assert(pure_store_init(path) == PURE_OK);
    expect(path, revision, expected);
    unsigned char value = 0x5A;
    assert(pure_store_splice(path, "large", revision, 0, 1,
                             (PureSpan){&value, 1}, PURE_OBJECT_LIMIT, &revision) == PURE_OK);
    expected[0] = value;
    expect(path, revision, expected);

    fail_stage = PURE_BEFORE_DIR_SYNC;
    value = 0x36;
    assert(pure_store_splice(path, "large", revision, PAGE + 4, 1,
                             (PureSpan){&value, 1}, PURE_OBJECT_LIMIT, &revision) == PURE_UNCERTAIN);
    fail_stage = -1;
    expected[PAGE + 4] = value;
    expect(path, revision, expected);

    /* A whole replacement must skip the generation orphaned by the crash. */
    assert(pure_store_put(path, "large", revision,
                          (PureSpan){expected, SIZE}, &revision) == PURE_OK);
    expect(path, revision, expected);
    DIR *segments = opendir(path);
    assert(segments);
    unsigned generations = 0;
    struct dirent *segment;
    while ((segment = readdir(segments))) {
        assert(strncmp(segment->d_name, ".pending-", 9));
        if (!strncmp(segment->d_name, "dat-large-", 10)) ++generations;
    }
    assert(closedir(segments) == 0 && generations == 1);
    const unsigned char initial[] = "abcde", inserted[] = "XYZ";
    assert(pure_store_put(path, "variable", 0,
                          (PureSpan){initial, 5}, &revision) == PURE_OK);
    assert(pure_store_splice(path, "variable", revision, 1, 2,
                             (PureSpan){inserted, 3}, PURE_OBJECT_LIMIT, &revision) == PURE_OK);
    PureStoredView variable;
    assert(pure_store_get(path, "variable", 6, &variable) == PURE_OK);
    assert(variable.body.size == 6 && !memcmp(variable.body.data, "aXYZde", 6));
    pure_store_release(&variable);
    format_transitions(path);
    mixed_edits(path);
    DIR *directory = opendir(path);
    assert(directory);
    struct dirent *entry;
    while ((entry = readdir(directory))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        assert(unlinkat(dirfd(directory), entry->d_name, 0) == 0);
    }
    assert(closedir(directory) == 0 && rmdir(path) == 0 && rmdir(base) == 0);
    puts("{\"hybrid_storage\":\"snapshot-patch-range-compaction-crash-conflict-uncertain-inline-transitions\",\"mixed_edits\":128}");
    return 0;
}
