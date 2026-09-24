#define _POSIX_C_SOURCE 200809L
/* Linux-only whole-app evidence: many independent native app instances, each
 * exercising the execution core, scalar program and durable state path. */
#include "app.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern const PureScalarPlan pure_app_plan;

static long rss_kib(void) {
    FILE *file = fopen("/proc/self/status", "r");
    if (!file) return -1;
    char line[256];
    long result = -1;
    while (fgets(line, sizeof(line), file))
        if (strncmp(line, "VmRSS:", 6) == 0) {
            if (sscanf(line + 6, "%ld", &result) != 1) result = -1;
            break;
        }
    fclose(file);
    return result;
}

int main(void) {
    enum { COUNT = 1000 };
    char directory[] = "/tmp/pure-app-memory-XXXXXX";
    assert(mkdtemp(directory));
    long before = rss_kib();
    assert(before >= 0);
    PureNativeApp *apps = calloc(COUNT, sizeof(*apps));
    void **scalar_memory = calloc(COUNT, sizeof(*scalar_memory));
    assert(apps && scalar_memory);
    size_t scalar_bytes = pure_scalar_bytes();
    size_t completed_actions = 0, completed_transfers = 0;
    for (size_t i = 0; i < COUNT; ++i) {
        scalar_memory[i] = malloc(scalar_bytes);
        assert(scalar_memory[i]);
        char key[32];
        snprintf(key, sizeof(key), "doc%04zu", i);
        assert(pure_native_app_init(&apps[i], &pure_app_plan, scalar_memory[i],
                                    scalar_bytes, directory, key, 4096) == PURE_OK);
        assert(pure_native_app_action(&apps[i], (PureAction){.phase = PURE_RESET}).status == PURE_OK);
        ++completed_actions;
#ifndef PURE_APP_MEMORY_PERSISTENT_WORK
        assert(!apps[i].execution.work && !apps[i].execution.capacity);
#endif
    }
    long initialized = rss_kib();
    assert(initialized >= before);
    for (size_t i = 0; i < COUNT; ++i) {
        assert(pure_native_app_save(&apps[i]).status == PURE_OK);
        ++completed_transfers;
        assert(apps[i].stored_revision == 1);
        assert(!apps[i].execution.active && !apps[i].execution.retained);
#ifndef PURE_APP_MEMORY_PERSISTENT_WORK
        assert(!apps[i].execution.work && !apps[i].execution.capacity);
#endif
    }
    long saved = rss_kib();
    assert(saved >= before);
    for (size_t i = 0; i < COUNT; i += 249) {
        assert(pure_native_app_restore(&apps[i]).status == PURE_OK);
        ++completed_transfers;
    }
    for (size_t i = 0; i < COUNT; ++i) {
        assert(!apps[i].execution.active && !apps[i].execution.retained);
        char name[128];
        snprintf(name, sizeof(name), "%s/obj-%s", directory, apps[i].document);
        assert(unlink(name) == 0);
        free(scalar_memory[i]);
    }
    char lock[128];
    snprintf(lock, sizeof(lock), "%s/.writer.lock", directory);
    assert(unlink(lock) == 0 && rmdir(directory) == 0);
    printf("{\"apps\":%u,\"app_controller_bytes\":%zu,\"scalar_bytes\":%zu,"
           "\"rss_before_kib\":%ld,\"rss_initialized_kib\":%ld,\"rss_saved_kib\":%ld,"
           "\"rss_initialized_delta_kib\":%ld,\"rss_saved_delta_kib\":%ld,"
           "\"completed_actions\":%zu,\"completed_transfers\":%zu}\n",
           COUNT, sizeof(*apps), scalar_bytes, before, initialized, saved,
           initialized - before, saved - before, completed_actions, completed_transfers);
    free(scalar_memory);
    free(apps);
    return 0;
}
