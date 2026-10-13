/* Research 140, design 027 stage 2a: aup's examples/json.aup (run by Pure)
 * against the same validator in C, on the same document: both must say it is
 * JSON, then each is timed (the median of seven runs of rounds checks).
 *
 *   cc -O2 -std=c17 -I../../../core json_time.c ../../../build/libpure.a -lm -pthread -o json_time
 *   ./json_time json.pmod ROUNDS     (PURE_JIT and PURE_BATCH choose Pure's path)
 *
 * The document is json_check.py's: 2,000 objects of a key and an array of a
 * number, true, null and a string with an escape. One JSON line. */
#define _POSIX_C_SOURCE 200809L
#include "module.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int space(int b) { return b == 0x20 || b == 0x09 || b == 0x0a || b == 0x0d; }
static int digit(int b) { return b >= 0x30 && b <= 0x39; }
static int hex(int b) { return digit(b) || (b >= 0x61 && b <= 0x66) || (b >= 0x41 && b <= 0x46); }

/* json.aup's design in C (its UTF-8 check left out: the document is ASCII). */
static int valid(const unsigned char *d, size_t n, unsigned char *open) {
    size_t i = 0, depth = 0;
    int mode = 0, ok = 1;
    while (ok) {
        while (i < n && space(d[i])) ++i;
        if (i == n) return mode == 1 && !depth;
        int b = d[i];
        if (mode == 1) {
            if (!depth) ok = 0;
            else if (b == 0x2c) mode = open[depth - 1] == 0x5b ? 0 : 2;
            else if (b == open[depth - 1] + 2) --depth;
            else ok = 0;
            ++i;
        } else if ((mode == 3 && b == 0x7d) || (mode == 4 && b == 0x5d)) {
            --depth, mode = 1, ++i;
        } else if (mode == 2 || mode == 3) {
            ok = b == 0x22, mode = 5;
        } else if (b == 0x5b || b == 0x7b) {
            open[depth++] = (unsigned char)b, mode = b == 0x5b ? 4 : 3, ++i;
        } else if (b == 0x22 || mode == 5) {
            ++i;
            while (i < n && d[i] != 0x22 && ok) {
                if (d[i] < 0x20) ok = 0;
                else if (d[i] == 0x5c && i + 1 < n && d[i + 1] == 0x75) {
                    ok = i + 5 < n && hex(d[i + 2]) && hex(d[i + 3]) && hex(d[i + 4]) && hex(d[i + 5]);
                    i += 6;
                } else if (d[i] == 0x5c) {
                    ok = i + 1 < n && strchr("\"\\/bfnrt", d[i + 1]) && d[i + 1];
                    i += 2;
                } else ++i;
            }
            ok = ok && i < n, ++i;
            if (mode == 5) {
                while (i < n && space(d[i])) ++i;
                ok = ok && i < n && d[i] == 0x3a, ++i, mode = 0;
            } else mode = 1;
        } else if (b == 0x2d || digit(b)) {
            if (b == 0x2d) ++i;
            ok = i < n && digit(d[i]);
            if (ok && d[i] == 0x30) ++i;
            else while (i < n && digit(d[i])) ++i;
            if (ok && i < n && d[i] == 0x2e) {
                ++i, ok = i < n && digit(d[i]);
                while (i < n && digit(d[i])) ++i;
            }
            if (ok && i < n && (d[i] == 0x65 || d[i] == 0x45)) {
                ++i;
                if (i < n && (d[i] == 0x2b || d[i] == 0x2d)) ++i;
                ok = i < n && digit(d[i]);
                while (i < n && digit(d[i])) ++i;
            }
            mode = 1;
        } else {
            const char *word = b == 0x74 ? "true" : b == 0x66 ? "false" : "null";
            size_t length = strlen(word);
            ok = i + length <= n && !memcmp(d + i, word, length), i += length, mode = 1;
        }
    }
    return 0;
}

static double now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e9 + (double)t.tv_nsec;
}

static int by_value(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

int main(int argc, char **argv) {
    if (argc != 3) { fprintf(stderr, "usage: %s MODULE ROUNDS\n", argv[0]); return 2; }
    int rounds = atoi(argv[2]);
    static unsigned char doc[1 << 18], open[1 << 18];
    size_t n = 0;
    doc[n++] = '[';
    for (int k = 0; k < 2000; ++k)
        n += (size_t)sprintf((char *)doc + n, "%s{\"key%d\": [1.5e3, true, null, \"text \\u00e9 %d\"]}", k ? "," : "", k, k);
    doc[n++] = ']';
    PureModule module;
    char error[160];
    if (pure_module_read(argv[1], &module, error, sizeof(error)) != PURE_OK) { fprintf(stderr, "%s\n", error); return 1; }
    PureApp app;
    if (pure_app_start(&app, &module, (PureData){0}) != PURE_OK) return 1;
    int check = pure_module_input(&module, "check");
    if (pure_app_message(&app, (uint32_t)check, NULL, -1, (PureSpan){doc, n}) != PURE_OK) { fprintf(stderr, "event failed\n"); return 1; }
    uint64_t work = app.work;
    if (app.values[0].num != 1 || !valid(doc, n, open)) { fprintf(stderr, "not both valid\n"); return 1; }
    double c_runs[7], pure_runs[7];
    volatile int sink = 0;
    for (int r = 0; r < 7; ++r) {
        double start = now_ns();
        for (int i = 0; i < rounds; ++i) sink += valid(doc, n, open);
        c_runs[r] = (now_ns() - start) / rounds;
        start = now_ns();
        for (int i = 0; i < rounds; ++i)
            if (pure_app_message(&app, (uint32_t)check, NULL, -1, (PureSpan){doc, n}) != PURE_OK) return 1;
        pure_runs[r] = (now_ns() - start) / rounds;
    }
    qsort(c_runs, 7, sizeof(double), by_value);
    qsort(pure_runs, 7, sizeof(double), by_value);
    const char *jit = getenv("PURE_JIT"), *batch = getenv("PURE_BATCH");
    printf("{\"size\":%zu,\"jit\":\"%s\",\"batch\":\"%s\",\"c_ns_per_byte\":%.3f,\"pure_ns_per_byte\":%.3f,\"ratio\":%.1f,"
           "\"instructions\":%llu,\"instructions_per_byte\":%.1f}\n",
           n, jit ? jit : "", batch ? batch : "", c_runs[3] / (double)n, pure_runs[3] / (double)n, pure_runs[3] / c_runs[3],
           (unsigned long long)work, (double)work / (double)n);
    pure_app_stop(&app);
    return 0;
}
