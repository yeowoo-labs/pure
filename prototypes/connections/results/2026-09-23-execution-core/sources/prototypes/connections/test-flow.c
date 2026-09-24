#define _POSIX_C_SOURCE 200809L
#include "store.h"
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "line %d: %s\n", __LINE__, #expr); exit(1); } } while (0)
static int failed_stage = -1;
static bool crash_at_stage;
int pure_store_test_fault(PureStoreStage stage) {
    if ((int)stage != failed_stage) return 0;
    if (crash_at_stage) _exit(77);
    return 1;
}
typedef struct { unsigned char *bytes; unsigned releases; } Owner;
static void release(void *raw) {
    Owner *owner = raw;
    free(owner->bytes);
    owner->bytes = NULL;
    ++owner->releases;
}

static void transfers(void) {
    Owner owner = {.bytes = malloc(5)};
    CHECK(owner.bytes);
    memcpy(owner.bytes, "a\0bcd", 5);
    PureTransfer transfer = {0};
    pure_transfer_init(&transfer, (PureSpan){(const unsigned char *)"head", 4},
                       (PureSpan){owner.bytes, 5}, &owner, release);
    CHECK(!pure_transfer_consume(&transfer, 5));
    CHECK(transfer.active && !owner.releases);
    CHECK(pure_transfer_consume(&transfer, 0));
    unsigned char received[9];
    size_t used = 0;
    while (transfer.active) {
        PureSpan span = pure_transfer_next(&transfer, 2);
        CHECK(span.size && span.size <= 2);
        memcpy(received + used, span.data, span.size);
        used += span.size;
        CHECK(pure_transfer_consume(&transfer, span.size));
        CHECK((used == 9) == (owner.releases == 1));
    }
    CHECK(used == 9 && !memcmp(received, "heada\0bcd", 9));
    pure_transfer_abort(&transfer);
    CHECK(owner.releases == 1 && !pure_transfer_consume(&transfer, 0));
    owner.bytes = malloc(3);
    CHECK(owner.bytes);
    pure_transfer_init(&transfer, (PureSpan){0}, (PureSpan){owner.bytes, 3}, &owner, release);
    CHECK(pure_transfer_consume(&transfer, 1));
    pure_transfer_abort(&transfer);
    pure_transfer_abort(&transfer);
    CHECK(owner.releases == 2 && !transfer.active);
    pure_transfer_init(&transfer, (PureSpan){0}, (PureSpan){0}, &owner, release);
    CHECK(owner.releases == 3 && !transfer.active);
}

static void storage(const char *path) {
    CHECK(pure_store_init(path) == PURE_OK);
    PureStoredView first, second;
    CHECK(pure_store_get(path, "missing", 100, &first) == PURE_NOT_FOUND);
    CHECK(!first.mapping && !first.mapped_size);
    static const unsigned char original[] = {0, 1, 2, 255, 17};
    static const unsigned char updated[] = {9, 8, 7};
    int64_t revision;
    CHECK(pure_store_put(path, "work", 0, (PureSpan){original, sizeof(original)}, &revision) == PURE_OK);
    CHECK(revision == 1);
    CHECK(pure_store_get(path, "work", sizeof(original) - 1, &first) == PURE_BUDGET);
    CHECK(!first.mapping && !first.mapped_size);
    CHECK(pure_store_get_range(path, "work", 1, 3, 3, &first) == PURE_OK);
    CHECK(first.object_size == sizeof(original) && first.body.size == 3 && !memcmp(first.body.data, original + 1, 3));
    pure_store_release(&first);
    CHECK(pure_store_get_range(path, "work", SIZE_MAX, 1, 10, &first) == PURE_LIMIT);
    CHECK(pure_store_get_range(path, "work", 1, SIZE_MAX, 10, &first) == PURE_LIMIT);
    CHECK(pure_store_get_range(path, "work", 1, 3, 2, &first) == PURE_BUDGET);
    CHECK(pure_store_get_range(path, "work", sizeof(original), 0, 0, &first) == PURE_OK);
    CHECK(!first.mapped_size && !first.body.size);
    pure_store_release(&first);
    CHECK(pure_store_get(path, "work", sizeof(original), &first) == PURE_OK);
    CHECK(first.body.size == sizeof(original) && first.revision == 1);
    CHECK(pure_store_put(path, "work", 1, (PureSpan){updated, sizeof(updated)}, &revision) == PURE_OK);
    CHECK(revision == 2);
    CHECK(!memcmp(first.body.data, original, sizeof(original)));
    CHECK(pure_store_get(path, "work", 100, &second) == PURE_OK);
    CHECK(second.revision == 2 && second.body.size == sizeof(updated));
    CHECK(!memcmp(second.body.data, updated, sizeof(updated)));
    pure_store_release(&first);
    pure_store_release(&first);
    pure_store_release(&second);
    CHECK(pure_store_put(path, "work", 1, (PureSpan){original, sizeof(original)}, &revision) == PURE_CONFLICT);
    CHECK(revision == 2);

    for (failed_stage = PURE_AFTER_HEADER; failed_stage <= PURE_BEFORE_RENAME; ++failed_stage) {
        CHECK(pure_store_put(path, "work", 2, (PureSpan){original, sizeof(original)}, &revision) == PURE_STORE_ERROR);
        CHECK(pure_store_get(path, "work", 100, &first) == PURE_OK);
        CHECK(first.revision == 2 && !memcmp(first.body.data, updated, sizeof(updated)));
        pure_store_release(&first);
    }
    failed_stage = PURE_BEFORE_DIR_SYNC;
    CHECK(pure_store_put(path, "work", 2, (PureSpan){original, sizeof(original)}, &revision) == PURE_UNCERTAIN);
    CHECK(revision == 3);
    failed_stage = -1;
    CHECK(pure_store_get(path, "work", 100, &first) == PURE_OK);
    CHECK(first.revision == 3 && !memcmp(first.body.data, original, sizeof(original)));
    pure_store_release(&first);
    CHECK(pure_store_put(path, "work", 2, (PureSpan){updated, sizeof(updated)}, &revision) == PURE_CONFLICT);

    /* A crashed unpublished writer leaves only a private file. A new publisher
     * must still use the previous committed version, never an incomplete body. */
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        failed_stage = PURE_AFTER_HEADER;
        crash_at_stage = true;
        pure_store_put(path, "work", 3, (PureSpan){updated, sizeof(updated)}, &revision);
        _exit(78);
    }
    int status;
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 77);
    CHECK(pure_store_init(path) == PURE_OK);
    CHECK(pure_store_get(path, "work", 100, &first) == PURE_OK);
    CHECK(first.revision == 3 && !memcmp(first.body.data, original, sizeof(original)));
    pure_store_release(&first);
    CHECK(pure_store_put(path, "work", 3, (PureSpan){updated, sizeof(updated)}, &revision) == PURE_OK);
    CHECK(revision == 4);

    char file[320];
    snprintf(file, sizeof(file), "%s/obj-work", path);
    int fd = open(file, O_WRONLY);
    CHECK(fd >= 0);
    const unsigned char maximum[] = {255,255,255,255,255,255,255,127};
    CHECK(pwrite(fd, maximum, 8, 8) == 8 && close(fd) == 0);
    CHECK(pure_store_put(path, "work", INT64_MAX, (PureSpan){original, sizeof(original)}, &revision) == PURE_LIMIT);
    CHECK(pure_store_put(path, "empty", 0, (PureSpan){0}, &revision) == PURE_OK);
    CHECK(pure_store_get(path, "empty", 0, &first) == PURE_OK && first.body.size == 0);
    pure_store_release(&first);

    /* Range mapping crosses page boundaries without copying a whole object. */
    long page = sysconf(_SC_PAGESIZE);
    CHECK(page >= 128 && (size_t)page <= PURE_OBJECT_LIMIT / 3);
    size_t size = (size_t)page * 3, offset = (size_t)page + 13, length = (size_t)page + 105;
    unsigned char *bytes = malloc(size);
    CHECK(bytes);
    for (size_t i = 0; i < size; ++i) bytes[i] = (unsigned char)((i * 17u) ^ (i >> 8) ^ (i >> 16));
    CHECK(pure_store_put(path, "range", 0, (PureSpan){bytes, size}, &revision) == PURE_OK);
    CHECK(pure_store_get_range(path, "range", offset, length, length, &first) == PURE_OK);
    CHECK(first.body.size == length && !memcmp(first.body.data, bytes + offset, length));
    pure_store_release(&first);
    free(bytes);
    snprintf(file, sizeof(file), "%s/obj-broken", path);
    fd = open(file, O_CREAT | O_WRONLY | O_EXCL, 0600);
    CHECK(fd >= 0 && write(fd, "incomplete", 10) == 10 && close(fd) == 0);
    CHECK(pure_store_get(path, "broken", 100, &first) == PURE_STORE_ERROR && !first.mapped_size);
    CHECK(pure_store_put(path, "broken", 0, (PureSpan){updated, sizeof(updated)}, &revision) == PURE_STORE_ERROR);
    CHECK(pure_store_init(file) == PURE_STORE_ERROR);
    struct stat info;
    CHECK(stat(file, &info) == 0 && info.st_size == 10);
}

int main(void) {
    transfers();
    char directory[] = "/tmp/pure-connection-unit-XXXXXX";
    CHECK(mkdtemp(directory));
    char path[256];
    snprintf(path, sizeof(path), "%s/store", directory);
    storage(path);
    DIR *entries = opendir(path);
    CHECK(entries);
    struct dirent *entry;
    while ((entry = readdir(entries))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        CHECK(unlinkat(dirfd(entries), entry->d_name, 0) == 0);
    }
    CHECK(closedir(entries) == 0 && rmdir(path) == 0);
    CHECK(rmdir(directory) == 0);
    puts("{\"transfer\":\"partial-consumption-and-single-release\","
         "\"storage\":\"mapped-ranges-snapshot-budget-conflict-failure-uncertain-crash-overflow-empty\"}");
    return 0;
}
