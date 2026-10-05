/* A breadth first search in one event, on the core alone (research 121):
 * 033's graph.aup, a random graph of NODES nodes and EDGES edges (from and
 * to uniform, no loops, each pair once; its own generator, seeded), loaded
 * as rows, then ROUNDS searches from fixed starts, each timed. No host, no
 * store: what the interpreter costs.
 *
 * bfs-run MODULE NODES EDGES ROUNDS   a JSON line per search, then the median */
#define _POSIX_C_SOURCE 200809L
#include "module.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint64_t state = 121;
static uint64_t next(void) { state ^= state << 13; state ^= state >> 7; state ^= state << 17; return state; }

static int compare(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static int edge_order(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

int main(int argc, char **argv) {
    if (argc != 5) { fputs("bfs-run MODULE NODES EDGES ROUNDS\n", stderr); return 2; }
    uint32_t nodes = (uint32_t)atol(argv[2]);
    size_t edges = (size_t)atol(argv[3]), rounds = (size_t)atol(argv[4]);
    static PureModule module;
    char error[160];
    if (pure_module_read(argv[1], &module, error, sizeof(error)) != PURE_OK) { fprintf(stderr, "%s\n", error); return 1; }
    int size = pure_module_input(&module, "size"), load = pure_module_input(&module, "load"),
        route = pure_module_input(&module, "route"), hops = pure_module_state(&module, "hops"),
        reached = pure_module_state(&module, "reached");
    static PureApp app;
    if (pure_app_start(&app, &module, (PureData){0}) != PURE_OK) return 1;
    /* Edges as from * 2^20 + to, sorted, each once (the few drawn twice once). */
    uint64_t *keys = malloc(edges * sizeof(*keys));
    size_t count = 0;
    while (count < edges) {
        uint64_t a = next() % nodes, b = next() % nodes;
        if (a != b) keys[count++] = a << 20 | b;
    }
    qsort(keys, count, sizeof(*keys), edge_order);
    size_t unique = 0;
    for (size_t i = 0; i < count; ++i)
        if (!unique || keys[i] != keys[unique - 1]) keys[unique++] = keys[i];
    unsigned char four[4] = {(unsigned char)nodes, (unsigned char)(nodes >> 8), (unsigned char)(nodes >> 16),
                             (unsigned char)(nodes >> 24)};
    if (pure_app_message(&app, (uint32_t)size, NULL, -1, (PureSpan){four, 4}) != PURE_OK) return 1;
    enum { CHUNK = 65536 };
    double *rows = malloc(CHUNK * 2 * sizeof(*rows));
    for (size_t at = 0; at < unique; at += CHUNK) {
        size_t n = unique - at < CHUNK ? unique - at : CHUNK;
        for (size_t k = 0; k < n; ++k) rows[2 * k] = (double)keys[at + k], rows[2 * k + 1] = (double)(keys[at + k] & 0xFFFFF);
        if (pure_app_message(&app, (uint32_t)load, NULL, -1, (PureSpan){(unsigned char *)rows, n * 16}) != PURE_OK) {
            fprintf(stderr, "load failed: %s\n", pure_fault_name(app.fault));
            return 1;
        }
    }
    if (getenv("BFS_WAIT")) { /* loaded: a counter attaches, then a line on stdin starts the searches */
        fprintf(stderr, "loaded\n");
        fflush(stderr);
        (void)getchar();
    }
    double *took = malloc(rounds * sizeof(*took));
    for (size_t r = 0; r < rounds; ++r) {
        uint32_t from = (uint32_t)(next() % nodes), to = (uint32_t)(next() % nodes);
        if (r) next(), next(); /* each search its own pair, whatever came before */
        unsigned char eight[8];
        for (int i = 0; i < 4; ++i) eight[i] = (unsigned char)(from >> (8 * i)), eight[4 + i] = (unsigned char)(to >> (8 * i));
        struct timespec a, b;
        clock_gettime(CLOCK_MONOTONIC, &a);
        PureResult result = pure_app_message(&app, (uint32_t)route, NULL, -1, (PureSpan){eight, 8});
        clock_gettime(CLOCK_MONOTONIC, &b);
        if (result != PURE_OK) { fprintf(stderr, "route failed: %s\n", pure_fault_name(app.fault)); return 1; }
        took[r] = (double)(b.tv_sec - a.tv_sec) * 1e3 + (double)(b.tv_nsec - a.tv_nsec) / 1e6;
        printf("{\"from\":%u,\"to\":%u,\"hops\":%g,\"reached\":%g,\"ms\":%.2f,\"work\":%llu}\n", from, to,
               app.values[hops].num, app.values[reached].num, took[r], (unsigned long long)app.work);
    }
    qsort(took, rounds, sizeof(*took), compare);
    printf("{\"nodes\":%u,\"edges\":%zu,\"median_ms\":%.2f}\n", nodes, unique, took[rounds / 2]);
    pure_app_stop(&app);
    return 0;
}
