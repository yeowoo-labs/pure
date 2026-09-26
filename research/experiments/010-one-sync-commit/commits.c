#define _POSIX_C_SOURCE 200809L
/* Commit latency of the tree store: COMMITS sequential 16-byte inserts at
 * the start of one of 64 documents, as in the HTTP put benchmark.
 * commits STORE [COMMITS] -> one JSON object. */
#include "store.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}
static int order(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}
int main(int argc, char **argv) {
    if (argc < 2) return 2;
    const char *path = argv[1];
    int commits = argc > 2 ? atoi(argv[2]) : 200;
    if (commits < 1 || pure_store_init(path) != PURE_OK) return 1;
    int64_t revision[64] = {0};
    char key[16];
    const unsigned char body[16] = "0123456789abcdef";
    for (int k = 0; k < 64; ++k) {
        snprintf(key, sizeof(key), "k%d", k);
        if (pure_store_put(path, key, 0, (PureSpan){body, 16}, &revision[k]) != PURE_OK) return 1;
    }
    double *t = malloc(sizeof(double) * (size_t)commits), start = now_ms();
    if (!t) return 1;
    for (int i = 0; i < commits; ++i) {
        int k = i % 64;
        snprintf(key, sizeof(key), "k%d", k);
        double begun = now_ms();
        if (pure_store_splice(path, key, revision[k], 0, 0, (PureSpan){body, 16}, PURE_OBJECT_LIMIT,
                              &revision[k]) != PURE_OK) return 1;
        t[i] = now_ms() - begun;
    }
    double total = now_ms() - start;
    qsort(t, (size_t)commits, sizeof(double), order);
    printf("{\"commits_per_second\":%.0f,\"p50_ms\":%.2f,\"p90_ms\":%.2f,\"p99_ms\":%.2f}\n", commits / total * 1e3,
           t[commits / 2], t[commits * 9 / 10], t[commits * 99 / 100]);
    free(t);
    return 0;
}
