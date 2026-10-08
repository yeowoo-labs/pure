/* research 130: the suite's graph (natural.h) loaded as suite-run.c loads it, then one handler run ROUNDS times.
 * Built from the repository's root: cc -O2 -std=c17 -Icore -Iresearch/experiments/040-* (natural.h) FILE build/libpure.a -lm.
 * frontier-run DIR ROUNDS: frontier.aup's frontier (each node's edges' to summed by a walk in a guarded loop);
 * search-run DIR ROUNDS: graph.pmod's route (the suite's BFS), its best ms. Counted with 041's icount (ROUNDS 1 and 11). */
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


/* front-run DIR ROUNDS: the frontier walk (front.pmod) ROUNDS times after the graph's load (the first packs the edges): its ms and sum. */
int main(int argc, char **argv) {
    static PureModule module;
    static PureApp app;
    char error[200], path[512];
    snprintf(path, sizeof(path), "%s/frontier.pmod", argv[1]);
    if (pure_module_read(path, &module, error, sizeof(error)) != PURE_OK) { fprintf(stderr, "%s\n", error); return 1; }
    if (pure_app_start(&app, &module, (PureData){0}) != PURE_OK) return 1;
    if (!load_graph(&app, &module, 100000, 1000000)) return 1;
    int rounds = atoi(argv[2]), input = pure_module_input(&module, "frontier"), sum = pure_module_state(&module, "sum");
    for (int r = 0; r <= rounds; ++r) {
        uint32_t args[2] = {0, 0};
        double a = now_ms();
        PureResult result = pure_app_message(&app, (uint32_t)input, NULL, -1, (PureSpan){(unsigned char *)args, 8});
        double ms = now_ms() - a;
        if (result != PURE_OK) { fprintf(stderr, "failed %s\n", pure_fault_name(app.fault)); return 1; }
        printf("{\"round\":%d,\"ms\":%.3f,\"sum\":%.17g,\"work\":%llu}\n", r, ms, app.values[sum].num, (unsigned long long)app.work);
    }
}
