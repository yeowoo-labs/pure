#define _POSIX_C_SOURCE 200809L
#include "store.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "line %d: %s\n", __LINE__, #expr); exit(1); } } while (0)
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
    CHECK(!first.db && !first.statement);
    static const unsigned char original[] = {0, 1, 2, 255, 17};
    static const unsigned char updated[] = {9, 8, 7};
    int64_t revision;
    CHECK(pure_store_put(path, "work", 0, (PureSpan){original, sizeof(original)}, &revision) == PURE_OK);
    CHECK(revision == 1);
    CHECK(pure_store_get(path, "work", sizeof(original) - 1, &first) == PURE_BUDGET);
    CHECK(!first.db && !first.statement);
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

    sqlite3 *other = NULL;
    CHECK(sqlite3_open(path, &other) == SQLITE_OK);
    CHECK(sqlite3_exec(other, "BEGIN IMMEDIATE", NULL, NULL, NULL) == SQLITE_OK);
    CHECK(pure_store_put(path, "work", 2, (PureSpan){original, sizeof(original)}, &revision) == PURE_BUSY);
    CHECK(sqlite3_exec(other, "ROLLBACK", NULL, NULL, NULL) == SQLITE_OK);
    CHECK(sqlite3_exec(other, "CREATE TRIGGER deny BEFORE UPDATE ON pure_objects "
        "BEGIN SELECT RAISE(ABORT,'injected failure'); END", NULL, NULL, NULL) == SQLITE_OK);
    CHECK(pure_store_put(path, "work", 2, (PureSpan){original, sizeof(original)}, &revision) == PURE_STORE_ERROR);
    CHECK(pure_store_get(path, "work", 100, &first) == PURE_OK);
    CHECK(first.revision == 2 && !memcmp(first.body.data, updated, sizeof(updated)));
    pure_store_release(&first);
    CHECK(sqlite3_exec(other, "DROP TRIGGER deny; UPDATE pure_objects SET revision=9223372036854775807", NULL, NULL, NULL) == SQLITE_OK);
    CHECK(pure_store_put(path, "work", INT64_MAX, (PureSpan){original, sizeof(original)}, &revision) == PURE_LIMIT);
    CHECK(sqlite3_close(other) == SQLITE_OK);
    CHECK(pure_store_put(path, "empty", 0, (PureSpan){0}, &revision) == PURE_OK);
    CHECK(pure_store_get(path, "empty", 0, &first) == PURE_OK && first.body.size == 0);
    pure_store_release(&first);
}

int main(void) {
    transfers();
    char directory[] = "/tmp/pure-connection-unit-XXXXXX";
    CHECK(mkdtemp(directory));
    char path[256], sidecar[280];
    snprintf(path, sizeof(path), "%s/store.db", directory);
    storage(path);
    CHECK(unlink(path) == 0);
    snprintf(sidecar, sizeof(sidecar), "%s-wal", path); unlink(sidecar);
    snprintf(sidecar, sizeof(sidecar), "%s-shm", path); unlink(sidecar);
    CHECK(rmdir(directory) == 0);
    puts("{\"transfer\":\"partial-consumption-and-single-release\","
         "\"storage\":\"snapshot-budget-conflict-busy-rollback-overflow-empty\"}");
    return 0;
}
