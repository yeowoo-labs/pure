/* The C baseline of research 118: a breadth first search over the graph
 * bfs.py writes (u32le nodes, u32le edges, then u32le from and to of each,
 * sorted), adjacency as offsets into one array, from each start given.
 * bfs GRAPH FROM TO [FROM TO ...]: per search a JSON line, hops, reached
 * and the median of ROUNDS runs in microseconds. */
#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static double now_us(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e6 + t.tv_nsec / 1e3;
}

static int compare(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

int main(int argc, char **argv) {
    FILE *f = fopen(argv[1], "rb");
    uint32_t n, m;
    if (!f || fread(&n, 4, 1, f) != 1 || fread(&m, 4, 1, f) != 1) return 1;
    uint32_t *from = malloc(4ull * m), *to = malloc(4ull * m), *start = calloc(n + 1, 4);
    for (uint32_t k = 0; k < m; ++k)
        if (fread(&from[k], 4, 1, f) != 1 || fread(&to[k], 4, 1, f) != 1) return 1;
    fclose(f);
    for (uint32_t k = 0; k < m; ++k) ++start[from[k] + 1];
    for (uint32_t v = 0; v < n; ++v) start[v + 1] += start[v];
    int32_t *depth = malloc(4ull * n);
    uint32_t *queue = malloc(4ull * n);
    for (int a = 2; a + 1 < argc; a += 2) {
        uint32_t s = (uint32_t)atoi(argv[a]), t = (uint32_t)atoi(argv[a + 1]);
        double runs[21];
        uint32_t reached = 0;
        for (int r = 0; r < 21; ++r) {
            double began = now_us();
            for (uint32_t v = 0; v < n; ++v) depth[v] = -1;
            uint32_t head = 0, tail = 0;
            queue[tail++] = s;
            depth[s] = 0;
            while (head < tail) {
                uint32_t v = queue[head++];
                for (uint32_t k = start[v]; k < start[v + 1]; ++k)
                    if (depth[to[k]] < 0) { depth[to[k]] = depth[v] + 1; queue[tail++] = to[k]; }
            }
            runs[r] = now_us() - began;
            reached = tail;
        }
        qsort(runs, 21, sizeof(*runs), compare);
        printf("{\"system\":\"c\",\"from\":%u,\"to\":%u,\"hops\":%d,\"reached\":%u,\"us\":%.1f}\n", s, t, depth[t], reached, runs[10]);
    }
    return 0;
}
