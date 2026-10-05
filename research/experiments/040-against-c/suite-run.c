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

static uint64_t minstd(uint64_t x) { return x * 48271 % 2147483647; }

/* ---- The C of each workload: its answers in out[] */

static void c_roots(const uint32_t *in, double *out) {
    double sum = 0, count = 0;
    for (uint32_t i = 1; i <= in[0]; ++i) {
        double r = i;
        for (int s = 0; s < 64; ++s) {
            double next = (r + i / r) / 2;
            if (next >= r) break;
            r = next;
            ++count;
        }
        sum += r;
    }
    out[0] = sum, out[1] = count;
}

static void c_knapsack(const uint32_t *in, double *out) {
    uint32_t n = in[0], capacity = in[1];
    uint32_t *weight = malloc(4 * (size_t)n), *worth = malloc(4 * (size_t)n);
    int64_t *best = calloc((size_t)capacity + 1, 8);
    uint64_t x = 1;
    for (uint32_t i = 0; i < n; ++i) {
        x = minstd(x), weight[i] = (uint32_t)(x % 1000) + 1;
        x = minstd(x), worth[i] = (uint32_t)(x % 1000);
    }
    for (uint32_t i = 0; i < n; ++i)
        for (uint32_t c = capacity; c >= weight[i] && c <= capacity; --c) {
            int64_t with = best[c - weight[i]] + worth[i];
            if (with > best[c]) best[c] = with;
        }
    out[0] = (double)best[capacity];
    free(weight), free(worth), free(best);
}

static int compare_u32(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}

static void c_sort(const uint32_t *in, double *out) {
    uint32_t n = in[0];
    uint32_t *keys = malloc(4 * (size_t)n);
    uint64_t x = 1;
    for (uint32_t i = 0; i < n; ++i) x = minstd(x), keys[i] = (uint32_t)x;
    qsort(keys, n, 4, compare_u32);
    uint32_t wrong = 0;
    for (uint32_t i = 1; i < n; ++i) wrong += keys[i] < keys[i - 1];
    out[0] = keys[0], out[1] = keys[n / 2], out[2] = keys[n - 1], out[3] = wrong;
    free(keys);
}

static uint32_t hash32(uint32_t x) { return x * 2654435761u; }

static void c_join(const uint32_t *in, double *out) {
    uint32_t customers = in[0], orders = in[1], size = 1;
    while (size < 2 * customers) size *= 2;
    uint32_t *ids = malloc(4 * (size_t)customers), *keys = calloc(size, 4), *regions = malloc(4 * (size_t)size);
    uint64_t x = 1, totals[16] = {0};
    for (uint32_t i = 0; i < customers; ++i) {
        x = minstd(x), ids[i] = (uint32_t)x;
        uint32_t at = hash32(ids[i]) & (size - 1);
        while (keys[at]) at = (at + 1) & (size - 1);
        keys[at] = ids[i], regions[at] = i % 16;
    }
    for (uint32_t o = 0; o < orders; ++o) {
        x = minstd(x);
        uint32_t id = ids[x % customers], at = hash32(id) & (size - 1);
        while (keys[at] != id) at = (at + 1) & (size - 1);
        totals[regions[at]] += x % 1000;
    }
    uint64_t most = 0, sum = 0;
    for (int r = 0; r < 16; ++r) most = totals[r] > most ? totals[r] : most, sum += totals[r];
    out[0] = (double)most, out[1] = (double)sum;
    free(ids), free(keys), free(regions);
}

static uint32_t fnv(const char *s) {
    uint32_t h = 2166136261u;
    for (; *s; ++s) h = (h ^ (unsigned char)*s) * 16777619u;
    return h;
}

static void c_words(const uint32_t *in, double *out) {
    uint32_t n = in[0], vocabulary = in[1], size = 1;
    while (size < 2 * vocabulary) size *= 2;
    char (*words)[16] = calloc(size, 16);
    uint32_t *counts = calloc(size, 4), distinct = 0, most = 0;
    uint64_t x = 1;
    for (uint32_t i = 0; i < n; ++i) {
        x = minstd(x);
        char w[16];
        snprintf(w, sizeof(w), "w%u", (unsigned)(x % vocabulary));
        uint32_t at = fnv(w) & (size - 1);
        while (counts[at] && strcmp(words[at], w)) at = (at + 1) & (size - 1);
        if (!counts[at]) memcpy(words[at], w, sizeof(w)), ++distinct;
        if (++counts[at] > most) most = counts[at];
    }
    out[0] = distinct, out[1] = most;
    free(words), free(counts);
}

static void c_matmul(const uint32_t *in, double *out) {
    uint32_t n = in[0];
    double *a = malloc(8 * (size_t)n * n), *b = malloc(8 * (size_t)n * n), total = 0, last = 0;
    uint64_t x = 1;
    for (uint32_t i = 0; i < n * n; ++i) {
        x = minstd(x), a[i] = (double)(x % 100);
        x = minstd(x), b[i] = (double)(x % 100);
    }
    for (uint32_t i = 0; i < n; ++i)
        for (uint32_t j = 0; j < n; ++j) {
            double s = 0;
            for (uint32_t k = 0; k < n; ++k) s += a[i * n + k] * b[k * n + j];
            total += s, last = s;
        }
    out[0] = total, out[1] = last;
    free(a), free(b);
}

/* ---- BFS: 033's graph (bfs-run's generator, seed 121), bfs.c's loop */

typedef struct { uint32_t nodes, *start, *to, *queue; int32_t *depth; } Graph;
static Graph graph;
static uint64_t graph_state = 121;
static uint64_t graph_next(void) {
    graph_state ^= graph_state << 13, graph_state ^= graph_state >> 7, graph_state ^= graph_state << 17;
    return graph_state;
}
static int compare_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

static void c_bfs(const uint32_t *in, double *out) {
    const Graph *g = &graph;
    for (uint32_t v = 0; v < g->nodes; ++v) g->depth[v] = -1;
    uint32_t head = 0, tail = 0;
    g->queue[tail++] = in[0];
    g->depth[in[0]] = 0;
    while (head < tail) {
        uint32_t v = g->queue[head++];
        for (uint32_t k = g->start[v]; k < g->start[v + 1]; ++k)
            if (g->depth[g->to[k]] < 0) g->depth[g->to[k]] = g->depth[v] + 1, g->queue[tail++] = g->to[k];
    }
    out[0] = g->depth[in[1]], out[1] = tail;
}

/* bfs.c read ahead as 022's floor reads Pure's (bfsfloor's c_ahead): the
 * offsets of the node 16 on in the queue, the first edge of the one 8 on
 * (context for Pure's reading ahead, not the C it is held to). */
static void c_bfs_ahead(const uint32_t *in, double *out) {
    const Graph *g = &graph;
    for (uint32_t v = 0; v < g->nodes; ++v) g->depth[v] = -1;
    uint32_t head = 0, tail = 0;
    g->queue[tail++] = in[0];
    g->depth[in[0]] = 0;
    while (head < tail) {
        if (head + 16 < tail) __builtin_prefetch(&g->start[g->queue[head + 16]]);
        if (head + 8 < tail) __builtin_prefetch(&g->to[g->start[g->queue[head + 8]]]);
        uint32_t v = g->queue[head++];
        for (uint32_t k = g->start[v]; k < g->start[v + 1]; ++k)
            if (g->depth[g->to[k]] < 0) g->depth[g->to[k]] = g->depth[v] + 1, g->queue[tail++] = g->to[k];
    }
    out[0] = g->depth[in[1]], out[1] = tail;
}

/* The graph into the app (size, then the edges as rows) and into C's adjacency. */
static bool load_graph(PureApp *app, const PureModule *m, uint32_t nodes, size_t edges) {
    graph_state = 121; /* the same graph each time */
    free(graph.start), free(graph.to), free(graph.queue), free(graph.depth);
    uint64_t *keys = malloc(edges * sizeof(*keys));
    size_t count = 0;
    while (count < edges) {
        uint64_t a = graph_next() % nodes, b = graph_next() % nodes;
        if (a != b) keys[count++] = a << 20 | b;
    }
    qsort(keys, count, sizeof(*keys), compare_u64);
    size_t unique = 0;
    for (size_t i = 0; i < count; ++i)
        if (!unique || keys[i] != keys[unique - 1]) keys[unique++] = keys[i];
    graph.nodes = nodes;
    graph.start = calloc((size_t)nodes + 1, 4), graph.to = malloc(4 * unique), graph.queue = malloc(4ull * nodes);
    graph.depth = malloc(4ull * nodes);
    for (size_t i = 0; i < unique; ++i) ++graph.start[(keys[i] >> 20) + 1], graph.to[i] = (uint32_t)(keys[i] & 0xFFFFF);
    for (uint32_t v = 0; v < nodes; ++v) graph.start[v + 1] += graph.start[v];
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
