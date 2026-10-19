/* Research 140: line breaking (UAX #14, Unicode 16.0) in aup (aup's examples/linebreak.aup, run by Pure) against the
 * same algorithm in C (Vega's text/linebreak.c, with its own tables), on the lines linebreak_check.py writes
 * (LineBreakTest.txt's and its random ones). The app's properties come in as a load message (RANGES: rows of six
 * binary64s, start, class, pi, pf, east, pictographic). Pure's and Vega's boundaries must be each line's; then each is
 * timed over all the lines (the median of seven runs of rounds passes).
 *
 *   cc -O2 -std=c17 -I../../../core -I../../../../vega/text linebreak_time.c ../../../../vega/text/linebreak.c \
 *      ../../../../vega/text/unicode-data.c ../../../build/libpure.a -lm -pthread -o linebreak_time
 *   ./linebreak_time linebreak.pmod RANGES LINES ROUNDS     (LINES: each a u32 count, its u32 code points, count + 1
 *                                                             bytes: 1 where a line may break)
 *
 * One JSON line: the lines and code points, the nanoseconds per code point of each, Pure's ratio and the instructions
 * Pure counted for one pass. */
#define _POSIX_C_SOURCE 200809L
#include "module.h"
#include "text.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e9 + (double)t.tv_nsec;
}

static int by_value(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static unsigned char *whole(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    *size = (size_t)ftell(f);
    rewind(f);
    unsigned char *bytes = malloc(*size ? *size : 1);
    if (fread(bytes, 1, *size, f) != *size) { free(bytes); bytes = NULL; }
    fclose(f);
    return bytes;
}

typedef struct { uint32_t count; const uint32_t *codes; const unsigned char *want; unsigned char *payload; } Line;

/* A line's boundaries from Pure: the bytes state "breaks", compared as breaks or not with want. */
typedef struct { const unsigned char *want; size_t at; bool same; } Compare;
static bool compare(void *context, PureSpan span) {
    Compare *c = context;
    for (size_t i = 0; i < span.size; ++i, ++c->at) c->same = c->same && (span.data[i] != 0) == c->want[c->at];
    return true;
}

int main(int argc, char **argv) {
    if (argc != 5) { fprintf(stderr, "usage: %s MODULE RANGES LINES ROUNDS\n", argv[0]); return 2; }
    size_t ranges_size, lines_size;
    unsigned char *ranges = whole(argv[2], &ranges_size), *bytes = whole(argv[3], &lines_size);
    int rounds = atoi(argv[4]);
    if (!ranges || !bytes) { fprintf(stderr, "cannot read the ranges or the lines\n"); return 1; }
    static Line lines[1 << 16];
    size_t count = 0, points = 0;
    for (size_t at = 0; at + 4 <= lines_size && count < sizeof(lines) / sizeof(*lines); ++count) {
        Line *l = &lines[count];
        memcpy(&l->count, bytes + at, 4);
        l->codes = (const uint32_t *)(bytes + at + 4);
        l->want = bytes + at + 4 + 4 * (size_t)l->count;
        at += 4 + 4 * (size_t)l->count + l->count + 1;
        l->payload = malloc(8 * (size_t)l->count + 1); /* the message's rows: each code point a binary64 */
        for (uint32_t i = 0; i < l->count; ++i) {
            double c = l->codes[i];
            memcpy(l->payload + 8 * i, &c, 8);
        }
        points += l->count;
    }

    PureModule module = {0};
    char error[160];
    if (pure_module_read(argv[1], &module, error, sizeof(error)) != PURE_OK) { fprintf(stderr, "%s\n", error); return 1; }
    PureApp app;
    if (pure_app_start(&app, &module, (PureData){0}) != PURE_OK) return 1;
    int load = pure_module_input(&module, "load"), line = pure_module_input(&module, "line"),
        breaks = pure_module_state(&module, "breaks");
    if (pure_app_message(&app, (uint32_t)load, (PureSpan){ranges, ranges_size}) != PURE_OK) { fprintf(stderr, "load failed\n"); return 1; }
    size_t pure_right = 0, vega_right = 0;
    uint64_t work = 0;
    static uint8_t got[4096];
    for (size_t k = 0; k < count; ++k) {
        const Line *l = &lines[k];
        bool done = pure_app_message(&app, (uint32_t)line, (PureSpan){l->payload, 8 * (size_t)l->count}) == PURE_OK;
        work += app.work;
        Compare c = {l->want, 0, true};
        if (done && pure_draft_read(app.texts[breaks], 0, pure_draft_size(app.texts[breaks]), compare, &c) == PURE_OK &&
            c.same && c.at == l->count + 1)
            ++pure_right;
        bool same = l->count < sizeof(got) && vega_line_breaks(l->codes, l->count, got);
        for (uint32_t i = 0; same && i <= l->count; ++i) same = (got[i] != VEGA_BREAK_NONE) == l->want[i];
        vega_right += same;
    }
    if (pure_right != count || vega_right != count) {
        fprintf(stderr, "lines %zu: Pure right %zu, Vega right %zu\n", count, pure_right, vega_right);
        return 1;
    }
    double vega_runs[7], pure_runs[7];
    volatile unsigned sink = 0;
    for (int r = 0; r < 7 && rounds; ++r) {
        double start = now_ns();
        for (int i = 0; i < rounds; ++i)
            for (size_t k = 0; k < count; ++k) {
                vega_line_breaks(lines[k].codes, lines[k].count, got);
                sink += got[0];
            }
        vega_runs[r] = (now_ns() - start) / rounds;
        start = now_ns();
        for (int i = 0; i < rounds; ++i)
            for (size_t k = 0; k < count; ++k)
                if (pure_app_message(&app, (uint32_t)line, (PureSpan){lines[k].payload, 8 * (size_t)lines[k].count}) != PURE_OK)
                    return 1;
        pure_runs[r] = (now_ns() - start) / rounds;
    }
    if (rounds) {
        qsort(vega_runs, 7, sizeof(double), by_value);
        qsort(pure_runs, 7, sizeof(double), by_value);
    }
    double n = (double)points;
    const char *jit = getenv("PURE_JIT"), *batch = getenv("PURE_BATCH");
    printf("{\"lines\":%zu,\"code_points\":%zu,\"right\":%zu,\"jit\":\"%s\",\"batch\":\"%s\",\"c_ns_per_point\":%.1f,"
           "\"pure_ns_per_point\":%.1f,\"to_c\":%.1f,\"instructions\":%llu,\"instructions_per_point\":%.1f}\n",
           count, points, pure_right, jit ? jit : "", batch ? batch : "", rounds ? vega_runs[3] / n : 0,
           rounds ? pure_runs[3] / n : 0, rounds ? pure_runs[3] / vega_runs[3] : 0, (unsigned long long)work, (double)work / n);
    pure_app_stop(&app);
    return 0;
}
