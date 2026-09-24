#include "store-port.h"
#include "store.h"
#include <stdlib.h>
#include <sys/mman.h>

static void free_memory(void *data, size_t size) { (void)size; free(data); }
static void unmap_memory(void *data, size_t size) { if (size) munmap(data, size); }

static bool allocate(void *context, size_t size, PureResource *out) {
    (void)context;
    void *data = malloc(size);
    if (!data) return false;
    *out = (PureResource){.body = {data, size}, .allocation = data,
                         .allocation_size = size, .release = free_memory};
    return true;
}

static PureResult read_value(void *context, const char *key, PureRegion region,
                              size_t minimum, size_t available, PureResource *out, int64_t *revision) {
    PureStoredView view = {0};
    PureResult result = pure_store_select(context, key, region.offset, region.length,
                                         region.to_end, minimum, available, &view);
    if (result == PURE_OK) {
        *out = (PureResource){.body = view.body, .allocation = view.mapping,
            .allocation_size = view.mapped_size, .release = view.mapped_size ? unmap_memory : NULL};
        *revision = view.revision;
    }
    return result;
}

static PureResult write_value(void *context, const char *key, int64_t expected,
                               PureSpan body, int64_t *revision) {
    return pure_store_put(context, key, expected, body, revision);
}

PureHost pure_store_host(const char *path) {
    return (PureHost){.context = (void *)path, .allocate = allocate,
                      .read = read_value, .write = write_value};
}
