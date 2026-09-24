#ifndef PURE_STORE_H
#define PURE_STORE_H
#include "flow.h"
#include <sqlite3.h>
#include <stdint.h>

#define PURE_OBJECT_LIMIT ((2u * 6u + 32u) * 32768u + 8192u)
typedef enum {
    PURE_OK, PURE_NOT_FOUND, PURE_CONFLICT, PURE_BUSY, PURE_BUDGET,
    PURE_LIMIT, PURE_STORE_ERROR
} PureResult;
typedef struct {
    sqlite3 *db;
    sqlite3_stmt *statement;
    PureSpan body;
    int64_t revision;
} PureStoredView;

PureResult pure_store_init(const char *path);
PureResult pure_store_get(const char *path, const char *key, size_t available,
                          PureStoredView *out);
void pure_store_release(PureStoredView *);
PureResult pure_store_put(const char *path, const char *key, int64_t expected,
                          PureSpan body, int64_t *revision);
#endif
