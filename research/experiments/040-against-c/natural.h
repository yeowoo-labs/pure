/* The C each workload of research 124's suite is held to (suite-run.c,
 * floor.c): what one would write for the task, its answers in out[]. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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


/* 033's graph of nodes and edges (bfs-run's generator, seed 121): C's
 * adjacency in graph, and the edges as from * 2^20 + to, sorted, each once,
 * in *keys (the caller's to free); their count. */
static size_t graph_build(uint32_t nodes, size_t edges, uint64_t **keys_out) {
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
    *keys_out = keys;
    return unique;
}
