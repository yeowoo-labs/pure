/* The suite's BFS (natural.h's graph: 100,000 nodes, 1,000,000 edges) run
 * as a frontier batch would (research 129): rounds i .. min(i + B, the
 * queue's count at the batch's open), each edge's has asked as the batch
 * opened; candidates the edges it said no to (each asked again in order:
 * added, or added already by an earlier round of the batch); lock-step
 * lane-rounds what walking every round's edges together would cost (the
 * most edges of a batch's rounds times its rounds). cc -O2 frontier.c;
 * ./a.out [B]. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
static uint64_t st = 121;
static uint64_t nx(void) { st ^= st << 13, st ^= st >> 7, st ^= st << 17; return st; }
static int cmp(const void *a, const void *b) { uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b; return (x > y) - (x < y); }
int main(int argc, char **argv) {
    unsigned B = argc > 1 ? atoi(argv[1]) : 1024, nodes = 100000; size_t edges = 1000000;
    uint64_t *keys = malloc(edges * 8); size_t c = 0;
    while (c < edges) { uint64_t a = nx() % nodes, b = nx() % nodes; if (a != b) keys[c++] = a << 20 | b; }
    qsort(keys, c, 8, cmp);
    size_t u = 0; for (size_t i = 0; i < c; ++i) if (!u || keys[i] != keys[u - 1]) keys[u++] = keys[i];
    uint32_t *start = calloc(nodes + 1, 4), *to = malloc(4 * u);
    for (size_t i = 0; i < u; ++i) ++start[(keys[i] >> 20) + 1], to[i] = keys[i] & 0xFFFFF;
    for (unsigned v = 0; v < nodes; ++v) start[v + 1] += start[v];
    unsigned char *seen = malloc(nodes), *open = malloc(nodes); uint32_t *queue = malloc(4 * nodes);
    for (int s = 0; s < 6; ++s) {
        uint32_t from = nx() % nodes, dest = nx() % nodes; (void)dest;
        memset(seen, 0, nodes);
        size_t steps = 0, count = 1, cand = 0, adds = 0, edges_seen = 0, batches = 0, small = 0, lanes = 0;
        queue[0] = from, seen[from] = 1;
        for (size_t i = 0; i < count;) {
            size_t end = count < i + B ? count : i + B;
            memcpy(open, seen, nodes); /* has at the batch's open */
            ++batches, lanes += end - i, small += end - i < 16;
            { uint32_t most = 0; for (size_t r = i; r < end; ++r) { uint32_t l = start[queue[r] + 1] - start[queue[r]]; if (l > most) most = l; } steps += (size_t)most * (end - i); }
            for (size_t r = i; r < end; ++r)
                for (uint32_t e = start[queue[r]]; e < start[queue[r] + 1]; ++e) {
                    ++edges_seen;
                    if (open[to[e]]) continue;
                    ++cand;
                    if (!seen[to[e]]) seen[to[e]] = 1, queue[count++] = to[e], ++adds;
                }
            i = end;
        }
        printf("B %u search %d: edges %zu candidates %zu adds %zu batches %zu (under 16 lanes %zu) lanes/batch %.0f lockstep lane-rounds %zu (%.2fx the edges)\n", B, s, edges_seen, cand, adds, batches, small, (double)lanes / batches, steps, (double)steps / edges_seen);
    }
}
