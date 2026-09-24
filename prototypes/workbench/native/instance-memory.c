#define _POSIX_C_SOURCE 200809L
/* Linux-only evidence harness: 1000 real initialized scalar instances of the
 * same prepared app. It measures their process RSS delta, not a full service. */
#include "plan.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const PureScalarPlan pure_app_plan;

static long rss_kib(void) {
    FILE *file = fopen("/proc/self/status", "r");
    if (!file) return -1;
    char line[256];
    long result = -1;
    while (fgets(line, sizeof(line), file)) {
        if (strncmp(line, "VmRSS:", 6) == 0) {
            if (sscanf(line + 6, "%ld", &result) != 1) result = -1;
            break;
        }
    }
    fclose(file);
    return result;
}

int main(void) {
    enum { COUNT = 1000 };
    PureScalar **instances = calloc(COUNT, sizeof(*instances));
    if (!instances) return 2;
    long before = rss_kib();
    size_t bytes = pure_scalar_bytes();
    uint64_t checksum = 0;
    for (size_t i = 0; i < COUNT; ++i) {
        void *memory = malloc(bytes);
        if (!memory) return 2;
        instances[i] = pure_scalar_initialize(memory, bytes);
        if (!instances[i] || pure_scalar_load(instances[i], &pure_app_plan, NULL, NULL)) return 2;
        checksum += pure_scalar_info(instances[i], 4);
    }
    long after = rss_kib();
    if (before < 0 || after < before || checksum != COUNT) return 2;
    printf("{\"instances\":%u,\"instance_bytes\":%zu,\"rss_before_kib\":%ld,"
           "\"rss_after_kib\":%ld,\"rss_delta_kib\":%ld,\"checksum\":%" PRIu64 "}\n",
           COUNT, bytes, before, after, after - before, checksum);
    for (size_t i = 0; i < COUNT; ++i) free(instances[i]);
    free(instances);
    return 0;
}
