/* Research 124's suite: kinds of work in one event each, Pure against C in
 * this process on the same machine, the same inputs (each program draws them
 * by MINSTD, x * 48271 mod 2^31 - 1, from seed 1, as its C does), the same
 * answers (checked every round). The C is what one would write for the task:
 * the same algorithm, its natural types (whole numbers as integers), qsort,
 * a hash table with linear probing, snprintf.
 *
 * suite-run DIR ROUNDS [NAME...]   a JSON line per round of each workload
 * (Pure's ms, C's ms), then per workload the medians and Pure / C. DIR holds
 * the workloads' modules (NAME.pmod; bfs: 033's graph.aup as graph.pmod;
 * bfs_ahead: the same search against bfs.c read ahead, for context).
 * Without NAMEs, all of them. PURE_JIT and PURE_AHEAD as the core reads them. */
#define _POSIX_C_SOURCE 200809L
#include "module.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e3 + (double)t.tv_nsec / 1e6;
}

static int compare_doubles(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

#include "natural.h"

/* The graph into the app (size, then the edges as rows) and into C's adjacency. */
static bool load_graph(PureApp *app, const PureModule *m, uint32_t nodes, size_t edges) {
    uint64_t *keys;
    size_t unique = graph_build(nodes, edges, &keys);
    unsigned char four[4];
    memcpy(four, &nodes, 4);
    if (pure_app_message(app, (uint32_t)pure_module_input(m, "size"), NULL, -1, (PureSpan){four, 4}) != PURE_OK) return false;
    enum { CHUNK = 65536 };
    double *rows = malloc(CHUNK * 2 * sizeof(*rows));
    for (size_t at = 0; at < unique; at += CHUNK) {
        size_t n = unique - at < CHUNK ? unique - at : CHUNK;
        for (size_t k = 0; k < n; ++k) rows[2 * k] = (double)keys[at + k], rows[2 * k + 1] = (double)(keys[at + k] & 0xFFFFF);
        if (pure_app_message(app, (uint32_t)pure_module_input(m, "load"), NULL, -1, (PureSpan){(unsigned char *)rows, n * 16}) !=
            PURE_OK)
            return false;
    }
    free(rows), free(keys);
    return true;
}

/* ---- The workloads */

typedef struct {
    const char *name, *input;
    uint32_t args[2];
    unsigned argc;
    const char *answers[4];
    void (*c)(const uint32_t *in, double *out);
} Work;

static const Work works[] = {
    {"roots", "go", {100000}, 1, {"total", "steps"}, c_roots},
    {"knapsack", "go", {100, 50000}, 2, {"best"}, c_knapsack},
    {"sort", "go", {200000}, 1, {"first", "middle", "last", "disorder"}, c_sort},
    {"join", "go", {100000, 1000000}, 2, {"top", "sum"}, c_join},
    {"words", "go", {300000, 50000}, 2, {"distinct", "most"}, c_words},
    {"matmul", "go", {200}, 1, {"sum", "corner"}, c_matmul},
    {"bfs", "route", {0, 0}, 2, {"hops", "reached"}, c_bfs},
    {"bfs_ahead", "route", {0, 0}, 2, {"hops", "reached"}, c_bfs_ahead},
};

static int run(const char *dir, const Work *w, unsigned rounds) {
    char path[512];
    bool bfs = !strncmp(w->name, "bfs", 3);
    snprintf(path, sizeof(path), "%s/%s.pmod", dir, bfs ? "graph" : w->name);
    static PureModule module;
    static PureApp app;
    char error[160];
    if (pure_module_read(path, &module, error, sizeof(error)) != PURE_OK) { fprintf(stderr, "%s: %s\n", path, error); return 1; }
    if (pure_app_start(&app, &module, (PureData){0}) != PURE_OK) return 1;
    int input = pure_module_input(&module, w->input), states[4];
    unsigned answers = 0;
    while (answers < 4 && w->answers[answers]) states[answers] = pure_module_state(&module, w->answers[answers]), ++answers;
    if (bfs && !load_graph(&app, &module, 100000, 1000000)) { fputs("bfs: load failed\n", stderr); return 1; }
    double *pure_ms = malloc(rounds * sizeof(double)), *c_ms = malloc(rounds * sizeof(double));
    bool agree = true;
    for (unsigned r = 0; r < rounds; ++r) {
        uint32_t args[2] = {w->args[0], w->args[1]};
        if (bfs) args[0] = (uint32_t)(graph_next() % 100000), args[1] = (uint32_t)(graph_next() % 100000);
        unsigned char bytes[8];
        memcpy(bytes, args, 4 * w->argc);
        double a = now_ms();
        PureResult result = pure_app_message(&app, (uint32_t)input, NULL, -1, (PureSpan){bytes, 4 * w->argc});
        double b = now_ms();
        if (result != PURE_OK) { fprintf(stderr, "%s failed: %s\n", w->name, pure_fault_name(app.fault)); return 1; }
        double out[4] = {0};
        w->c(args, out);
        double c = now_ms();
        pure_ms[r] = b - a, c_ms[r] = c - b;
        bool same = true;
        for (unsigned i = 0; i < answers; ++i) same = same && app.values[states[i]].num == out[i];
        agree = agree && same;
        printf("{\"work\":\"%s\",\"round\":%u,\"pure_ms\":%.3f,\"c_ms\":%.3f,\"same\":%s,\"answers\":[", w->name, r, pure_ms[r],
               c_ms[r], same ? "true" : "false");
        for (unsigned i = 0; i < answers; ++i) printf("%s%.17g", i ? "," : "", app.values[states[i]].num);
        printf("],\"c\":[");
        for (unsigned i = 0; i < answers; ++i) printf("%s%.17g", i ? "," : "", out[i]);
        printf("]}\n");
        fflush(stdout);
    }
    unsigned skip = bfs ? 1 : 0; /* bfs: the first search packs the edges (022) */
    qsort(pure_ms + skip, rounds - skip, sizeof(double), compare_doubles);
    qsort(c_ms, rounds, sizeof(double), compare_doubles);
    double p = pure_ms[skip + (rounds - skip) / 2], cm = c_ms[rounds / 2];
    printf("{\"work\":\"%s\",\"pure_median_ms\":%.3f,\"c_median_ms\":%.3f,\"ratio\":%.3f,\"agree\":%s}\n", w->name, p, cm, p / cm,
           agree ? "true" : "false");
    pure_app_stop(&app);
    free(pure_ms), free(c_ms);
    return agree ? 0 : 1;
}

int main(int argc, char **argv) {
    if (argc < 3) { fputs("suite-run DIR ROUNDS [NAME...]\n", stderr); return 2; }
    unsigned rounds = (unsigned)atoi(argv[2]);
    int failed = 0;
    for (size_t i = 0; i < sizeof(works) / sizeof(*works); ++i) {
        bool wanted = argc == 3;
        for (int a = 3; a < argc; ++a) wanted = wanted || !strcmp(argv[a], works[i].name);
        if (wanted) failed |= run(argv[1], &works[i], rounds);
    }
    return failed;
}
