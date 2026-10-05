/* The floor a stored keyed list sets for a search (021): per node, the two
 * seeks of its slice (as slice_of does), in a random order of nodes, against
 * C's offsets: start[v], start[v+1] and the first to[] of the node. */
#define _POSIX_C_SOURCE 200809L
#include "module-list.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
static uint64_t state = 121;
static uint64_t next(void) { state ^= state << 13; state ^= state >> 7; state ^= state << 17; return state; }
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e9 + t.tv_nsec; }
static int cmp64(const void *a, const void *b) { uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b; return (x > y) - (x < y); }
int main(void) {
    enum { N = 100000, M = 1000000 };
    uint64_t *e = malloc(M * sizeof(*e));
    for (size_t k = 0; k < M; ++k) { uint64_t a = next() % N, b = next() % N; e[k] = a << 20 | b; }
    qsort(e, M, sizeof(*e), cmp64);
    size_t m = 0;
    for (size_t k = 0; k < M; ++k) if (!m || e[k] != e[m - 1]) e[m++] = e[k];
    static const uint8_t kinds[3] = {PURE_CELL_NUMBER, PURE_CELL_NUMBER, PURE_CELL_NUMBER};
    PureList *list = pure_list_new(3, kinds);
    pure_list_keyed(list, 0);
    for (size_t k = 0; k < m; ++k) {
        PureCell cells[3] = {{.num = (double)e[k]}, {.num = (double)(e[k] & 0xFFFFF)}, {.num = 1}};
        pure_list_insert(list, k, cells, NULL);
    }
    uint32_t *start = calloc(N + 1, 4), *to = malloc(m * 4);
    for (size_t k = 0; k < m; ++k) { ++start[(e[k] >> 20) + 1]; to[k] = (uint32_t)(e[k] & 0xFFFFF); }
    for (size_t v = 0; v < N; ++v) start[v + 1] += start[v];
    uint32_t *order = malloc(N * 4);
    for (uint32_t v = 0; v < N; ++v) order[v] = v;
    for (uint32_t v = N - 1; v > 0; --v) { uint32_t w = (uint32_t)(next() % (v + 1)), t = order[v]; order[v] = order[w]; order[w] = t; }
    for (int round = 0; round < 5; ++round) {
        double t0 = now();
        size_t sum = 0;
        for (size_t i = 0; i < N; ++i) {
            double low = (double)((uint64_t)order[i] << 20), high = (double)(((uint64_t)order[i] + 1) << 20);
            bool found;
            size_t a = pure_list_seek(list, 0, (PureCell){.num = low}, &found), b = pure_list_seek(list, 0, (PureCell){.num = high}, &found);
            sum += b - a;
            if (a < b) sum += (size_t)pure_list_get(list, a, 1).num; /* the slice's first item read, as the walk does */
        }
        double t1 = now();
        size_t csum = 0;
        for (size_t i = 0; i < N; ++i) {
            uint32_t v = order[i], s = start[v], f = start[v + 1];
            csum += f - s;
            if (s < f) csum += to[s];
        }
        double t2 = now();
        printf("{\"round\":%d,\"pure_ns_per_node\":%.1f,\"c_ns_per_node\":%.1f,\"same\":%d}\n", round, (t1 - t0) / N, (t2 - t1) / N, sum == csum);
    }
    return 0;
}
