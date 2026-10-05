/* 033's breadth first search (graph.aup's route) as a batch executor could
 * run it, on the forms Pure gives its lists here (docs/design/021, 022): edges
 * packed (a column per field in key order, from * 2^20 + to; an index of
 * buckets of 2^20 keys: a node's edges from index[node] to index[node + 1]),
 * seen DENSE (keyed by node: a bit per key, the depth at the key, four
 * bytes), queue FLAT (appended to).
 *   forms  round by round, as the executor runs the loop
 *   batch  the rounds whose test (i below queue.count) holds when the batch
 *          opens (count only grows): each step for every lane (queue read,
 *          the depth read, the edges walked into lanes of their own, seen's
 *          bit read for each), then the adds and appends in order, each
 *          lane's bit read again first (an earlier lane may have added it)
 * Against natural.h's c_bfs (offsets per node: CSR) and c_bfs_ahead (that
 * read ahead), on the suite's graph and routes (100,000 nodes, 1,000,000
 * edges drawn, seed 121). Built cc -O2. */
#define _POSIX_C_SOURCE 200809L
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "natural.h"

static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

enum { B = 256, LANES = 8192 };

/* seen: list of Seen by node, DENSE */
typedef struct { uint64_t *bits; uint32_t *depth; size_t count; } Seen;
static void seen_start(Seen *s, uint32_t nodes) {
    s->bits = calloc((nodes + 63) / 64, 8), s->depth = malloc(4ull * nodes), s->count = 0;
}
static bool seen_has(const Seen *s, uint32_t node) { return s->bits[node / 64] >> (node % 64) & 1; }
static void seen_add(Seen *s, uint32_t node, uint32_t depth) {
    s->bits[node / 64] |= UINT64_C(1) << (node % 64), s->depth[node] = depth, ++s->count;
}

/* edges: list of Edge by link, packed */
static uint32_t *to_col, *index_col;

/* round by round, as the executor runs the loop */
static void forms_bfs(uint32_t from, uint32_t target, uint32_t nodes, double *out) {
    Seen seen;
    seen_start(&seen, nodes);
    uint32_t *queue = malloc(4ull * nodes), count = 0;
    queue[count++] = from;
    seen_add(&seen, from, 0);
    for (uint32_t i = 0; i < nodes; ++i) {
        if (i >= count) break;
        uint32_t node = queue[i];
        for (uint32_t k = index_col[node]; k < index_col[node + 1]; ++k) {
            uint32_t to = to_col[k];
            if (!seen_has(&seen, to)) seen_add(&seen, to, seen.depth[node] + 1), queue[count++] = to;
        }
    }
    out[0] = seen_has(&seen, target) ? seen.depth[target] : -1, out[1] = (double)seen.count;
    free(seen.bits), free(seen.depth), free(queue);
}

/* a batch of rounds at a time */
static void batch_bfs(uint32_t from, uint32_t target, uint32_t nodes, double *out) {
    static uint32_t node[B], depth[B], lo[B], hi[B], edge_to[LANES], edge_round[LANES];
    static bool has[LANES];
    Seen seen;
    seen_start(&seen, nodes);
    uint32_t *queue = malloc(4ull * nodes), count = 0;
    queue[count++] = from;
    seen_add(&seen, from, 0);
    for (uint32_t i = 0; i < nodes && i < count;) {
        unsigned m = count - i < B ? count - i : B; /* the rounds whose test holds now (count only grows) */
        if (m > nodes - i) m = nodes - i;
        for (unsigned l = 0; l < m; ++l) node[l] = queue[i + l];
        for (unsigned l = 0; l < m; ++l) lo[l] = index_col[node[l]], hi[l] = index_col[node[l] + 1];
        for (unsigned l = 0; l < m; ++l) depth[l] = seen.depth[node[l]] + 1; /* seen[q.node].depth + 1 */
        unsigned e = 0, rounds = 0; /* the edges into lanes, as many rounds as fit */
        for (; rounds < m && e + (hi[rounds] - lo[rounds]) <= LANES; ++rounds)
            for (uint32_t k = lo[rounds]; k < hi[rounds]; ++k) edge_to[e] = to_col[k], edge_round[e] = rounds, ++e;
        for (unsigned q = 0; q < e; ++q) has[q] = seen_has(&seen, edge_to[q]); /* seen.has(e.to), each lane */
        for (unsigned q = 0; q < e; ++q) /* the adds and appends, in order */
            if (!has[q] && !seen_has(&seen, edge_to[q])) seen_add(&seen, edge_to[q], depth[edge_round[q]]), queue[count++] = edge_to[q];
        i += rounds;
    }
    out[0] = seen_has(&seen, target) ? seen.depth[target] : -1, out[1] = (double)seen.count;
    free(seen.bits), free(seen.depth), free(queue);
}

static int cmpd(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return (x > y) - (x < y); }
static void (*volatile natural[2])(const uint32_t *, double *) = {c_bfs, c_bfs_ahead};
int main(void) {
    uint64_t *keys;
    size_t edge_count = graph_build(100000, 1000000, &keys);
    to_col = malloc(4 * edge_count), index_col = calloc(100001, 4);
    for (size_t k = 0; k < edge_count; ++k) to_col[k] = (uint32_t)(keys[k] & 0xFFFFF), ++index_col[(keys[k] >> 20) + 1];
    for (uint32_t v = 0; v < 100000; ++v) index_col[v + 1] += index_col[v];
    double tc[11], ta[11], tf[11], tb[11], oc[2], oa[2], of[2], ob[2];
    bool same = true;
    for (int r = 0; r < 11; ++r) {
        uint32_t in[2] = {(uint32_t)(graph_next() % 100000), (uint32_t)(graph_next() % 100000)};
        double t = now(); natural[0](in, oc); tc[r] = now() - t;
        t = now(); natural[1](in, oa); ta[r] = now() - t;
        t = now(); forms_bfs(in[0], in[1], 100000, of); tf[r] = now() - t;
        t = now(); batch_bfs(in[0], in[1], 100000, ob); tb[r] = now() - t;
        for (int k = 0; k < 2; ++k) same &= oc[k] == oa[k] && oc[k] == of[k] && oc[k] == ob[k];
    }
    qsort(tc, 11, 8, cmpd), qsort(ta, 11, 8, cmpd), qsort(tf, 11, 8, cmpd), qsort(tb, 11, 8, cmpd);
    printf("{\"work\":\"bfs\",\"c_ms\":%.2f,\"c_ahead_ms\":%.2f,\"forms_ms\":%.2f,\"batch_ms\":%.2f,\"reached\":%.0f,\"same\":%s}\n", tc[5], ta[5], tf[5], tb[5], ob[1],
           same ? "true" : "false");
}
