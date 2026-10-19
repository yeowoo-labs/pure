/* Research 140: box layout (a subset of CSS Flexbox 1) in aup (aup's examples/layout.aup, run by Pure), timed on the
 * trees layout_check.py writes (each a u32 count, its nodes' rows of 14 binary64s in preorder, then their boxes, four
 * binary64s each, as layout.py works them out). Each tree is a message; its boxes must be the file's, bit for bit; then
 * the median of seven runs of ROUNDS passes. Taffy's time is layout_taffy --time's.
 *
 *   cc -O2 -std=c17 -I../../../core layout_time.c ../../../build/libpure.a -lm -pthread -o layout_time
 *   ./layout_time layout.pmod TREES ROUNDS
 *
 * One JSON line: the trees and nodes, the nanoseconds per node, and the instructions Pure counted for one pass. */
#define _POSIX_C_SOURCE 200809L
#include "module.h"
#include "module-list.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e9 + (double)t.tv_nsec;
}

static int by_value(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static unsigned char *whole(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    *size = (size_t)ftell(f);
    rewind(f);
    unsigned char *bytes = malloc(*size ? *size : 1);
    if (fread(bytes, 1, *size, f) != *size) { free(bytes); bytes = NULL; }
    fclose(f);
    return bytes;
}

typedef struct { uint32_t count; const unsigned char *rows; const unsigned char *boxes; } Tree;

int main(int argc, char **argv) {
    if (argc != 4) { fprintf(stderr, "usage: %s MODULE TREES ROUNDS\n", argv[0]); return 2; }
    size_t size;
    unsigned char *bytes = whole(argv[2], &size);
    int rounds = atoi(argv[3]);
    if (!bytes) { fprintf(stderr, "cannot read the trees\n"); return 1; }
    static Tree trees[1 << 16];
    size_t count = 0, nodes = 0;
    for (size_t at = 0; at + 4 <= size && count < sizeof(trees) / sizeof(*trees); ++count) {
        Tree *t = &trees[count];
        memcpy(&t->count, bytes + at, 4);
        t->rows = bytes + at + 4;
        t->boxes = t->rows + 112 * (size_t)t->count;
        at += 4 + 144 * (size_t)t->count;
        nodes += t->count;
    }

    PureModule module = {0};
    char error[160];
    if (pure_module_read(argv[1], &module, error, sizeof(error)) != PURE_OK) { fprintf(stderr, "%s\n", error); return 1; }
    PureApp app;
    if (pure_app_start(&app, &module, (PureData){0}) != PURE_OK) return 1;
    int input = pure_module_input(&module, "tree"), boxes = pure_module_state(&module, "boxes");
    size_t right = 0;
    uint64_t work = 0;
    for (size_t k = 0; k < count; ++k) {
        const Tree *t = &trees[k];
        bool same = pure_app_message(&app, (uint32_t)input, (PureSpan){t->rows, 112 * (size_t)t->count}) == PURE_OK &&
                    pure_list_count(app.lists[boxes]) == t->count;
        work += app.work;
        for (uint32_t i = 0; same && i < t->count; ++i)
            for (unsigned f = 0; same && f < 4; ++f) {
                double want;
                memcpy(&want, t->boxes + 32 * (size_t)i + 8 * f, 8);
                same = pure_list_get(app.lists[boxes], i, f).num == want;
            }
        right += same;
    }
    if (right != count) { fprintf(stderr, "trees %zu: Pure right %zu\n", count, right); return 1; }
    double runs[7];
    for (int r = 0; r < 7; ++r) {
        double start = now_ns();
        for (int i = 0; i < rounds; ++i)
            for (size_t k = 0; k < count; ++k)
                if (pure_app_message(&app, (uint32_t)input, (PureSpan){trees[k].rows, 112 * (size_t)trees[k].count}) != PURE_OK)
                    return 1;
        runs[r] = (now_ns() - start) / rounds;
    }
    qsort(runs, 7, sizeof(double), by_value);
    const char *jit = getenv("PURE_JIT"), *batch = getenv("PURE_BATCH");
    printf("{\"trees\":%zu,\"nodes\":%zu,\"right\":%zu,\"jit\":\"%s\",\"batch\":\"%s\",\"pure_ns_per_node\":%.1f,"
           "\"instructions\":%llu,\"instructions_per_node\":%.1f}\n",
           count, nodes, right, jit ? jit : "", batch ? batch : "", runs[3] / (double)nodes, (unsigned long long)work,
           (double)work / (double)nodes);
    pure_app_stop(&app);
    return 0;
}
