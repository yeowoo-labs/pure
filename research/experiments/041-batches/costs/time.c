/* What a batch costs apart from its lanes (research 128): time MODULE N
 * ROUNDS gives the median ms of message go {n: N} over ROUNDS, and the work
 * and out of the last; run each module three ways (PURE_JIT=0 with and
 * without PURE_BATCH=0, and PURE_JIT=1). Built cc -O2 -std=c17 -Icore
 * time.c build/libpure.a -lm -lpthread from the repository's root.
 *   ops     a loop of 1,024,000 rounds, six affine lets and a max (in a
 *           batch, mostly the max's reduction)
 *   nest    200 x 200 x 200, the inner loop's rounds together: 200 lanes
 *   adds    50,000 keys added in order to a DENSE list: an append's call
 *   probes  100,000 keys added to a HASHED list, 1,000,000 found
 *   recur   1,000 x N rounds of MINSTD: a recurrence ahead of a batch
 *   recur4  recur and four reads of x into a sum (records of x in the lanes)
 *   ops0, ops12, sum0, sum6, sum12: ops with one or twelve lets, and with
 *           total = total + 1 for the max (n 51200, batches only): what
 *           an instruction costs a batch apart from the reduction */
#define _POSIX_C_SOURCE 200809L
#include "module.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static int cmp(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return (x > y) - (x < y); }
int main(int argc, char **argv) {
    static PureModule module;
    static PureApp app;
    char error[160];
    if (argc < 4 || pure_module_read(argv[1], &module, error, sizeof(error)) != PURE_OK) { fprintf(stderr, "usage or %s\n", error); return 1; }
    if (pure_app_start(&app, &module, (PureData){0}) != PURE_OK) return 1;
    uint32_t n = (uint32_t)atoi(argv[2]);
    int rounds = atoi(argv[3]), input = pure_module_input(&module, "go"), out = pure_module_state(&module, "out");
    double *ms = malloc(rounds * sizeof(double));
    for (int r = 0; r < rounds; ++r) {
        unsigned char bytes[4];
        memcpy(bytes, &n, 4);
        double a = now_ms();
        PureResult result = pure_app_message(&app, (uint32_t)input, NULL, -1, (PureSpan){bytes, 4});
        ms[r] = now_ms() - a;
        if (result != PURE_OK) { fprintf(stderr, "failed: %s\n", pure_fault_name(app.fault)); return 1; }
    }
    qsort(ms, rounds, sizeof(double), cmp);
    printf("{\"module\":\"%s\",\"n\":%u,\"ms\":%.3f,\"work\":%llu,\"out\":%.17g}\n", argv[1], n, ms[rounds / 2], (unsigned long long)app.work, app.values[out].num);
}
