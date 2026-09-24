#ifndef PURE_STORE_H
#define PURE_STORE_H
#include "flow.h"
#include "result.h"
#include <stdint.h>

#define PURE_OBJECT_LIMIT ((2u * 6u + 32u) * 32768u + 8192u)
typedef struct {
    void *mapping;
    size_t mapped_size, object_size;
    PureSpan body;
    int64_t revision;
} PureStoredView;

PureResult pure_store_init(const char *path);
PureResult pure_store_get(const char *path, const char *key, size_t available,
                          PureStoredView *out);
PureResult pure_store_get_range(const char *path, const char *key, size_t offset,
                                size_t length, size_t available, PureStoredView *out);
/* Inspect once: guards and mapping refer to the same immutable file version.
 * minimum preserves bounds checks from eliminated intermediate views.
 * whole selects the suffix starting at offset; otherwise length is exact. */
PureResult pure_store_select(const char *path, const char *key, size_t offset,
                            size_t length, bool whole, size_t minimum,
                            size_t available, PureStoredView *out);
void pure_store_release(PureStoredView *);
PureResult pure_store_put(const char *path, const char *key, int64_t expected,
                          PureSpan body, int64_t *revision);

/* Fault hooks exist only in the separately built unit-test binary. */
typedef enum {
    PURE_AFTER_HEADER, PURE_BEFORE_FILE_SYNC, PURE_BEFORE_RENAME, PURE_BEFORE_DIR_SYNC
} PureStoreStage;
#ifdef PURE_STORE_TEST
int pure_store_test_fault(PureStoreStage stage);
#endif
#endif
