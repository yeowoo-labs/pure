/* Research 045, the next experiment: UAX #14's range() (aup's examples/linebreak.aup: the last range whose start is at
 * or before a code point, by halving), 36% of the app's machine instructions in the executor, written by hand as an
 * emitter would write it, in ways that keep or drop what the verifier's proof could license:
 *
 *   exact    Pure's meaning as the executor runs it: numbers are doubles and each sum and quotient is checked finite;
 *            // is floor_divide (fmod) and checks its divisor; ranges[middle] checks a whole number and the bounds
 *            (INDEX_AT), .start checks the bounds again and reads the stored list through pure_list_get (ITEM_GET);
 *            each round of the loop charges the work budget (iterate), and so does the call. Locals in registers.
 *   integer  exact, but low, high and middle are integers (from 0, ranges.count and //): no fmod, no finite checks,
 *            no whole-number check; the bounds checks and pure_list_get kept.
 *   flat     exact, but .start read from the list's start column held flat (the list does not change in the event).
 *   both     integer and flat: integers read from a flat column, the bounds check and the budget kept.
 *   c        the same halving in plain C on the flat column: no checks, no budget.
 *
 * Each answers every code point of the lines; the answers must equal the executor's (an app answering each point's
 * range: variants/range-out.patch), read from its view first. Then machine instructions and cycles (perf_event_open,
 * user mode) a code point and a round of halving, ROUNDS passes each, the ways interleaved.
 *
 *   cc -O2 -std=c17 -I../../../core range.c ../../../build/libpure.a -lm -pthread -o range
 *   ./range range-out.pmod RANGES LINES ROUNDS     one JSON line a way */
#define _GNU_SOURCE
#include "module.h"
#include "module-list.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>

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

/* What the emitted code is given: the stored list, its start column held flat, and the work budget (as PureApp's). */
typedef struct { const PureList *ranges; const double *starts; size_t count; uint64_t work, budget; } Context;
enum { ROUND = 24, CALL = 8 }; /* what a round of the loop and a call charge (their instructions as written) */

static inline bool charge(Context *x, uint64_t instructions) {
    if (instructions > x->budget - x->work) return false;
    x->work += instructions;
    return true;
}

/* core/module.c's, as it is. */
static double floor_divide(double a, double b) {
    double mod = fmod(a, b), quotient = (a - mod) / b;
    if (mod != 0 && (b < 0) != (mod < 0)) quotient -= 1.0;
    if (quotient == 0) return copysign(0.0, a / b);
    double whole = floor(quotient);
    return quotient - whole > 0.5 ? whole + 1.0 : whole;
}

/* -1: a fault (the executor's would stop the event). */
static __attribute__((noinline)) double range_exact(Context *x, double c) {
    if (!charge(x, CALL)) return -1;
    double low = 0, high = (double)pure_list_count(x->ranges);
    for (;;) {
        double span = high - low;
        if (!isfinite(span)) return -1;
        if (!(span > 1)) break;
        if (!charge(x, ROUND)) return -1;
        double sum = low + high;
        if (!isfinite(sum)) return -1;
        double middle = floor_divide(sum, 2);
        if (!isfinite(middle)) return -1;
        if (middle != floor(middle) || middle < 0 || middle >= (double)pure_list_count(x->ranges)) return -1; /* INDEX_AT */
        size_t at = (size_t)middle;
        if (at >= pure_list_count(x->ranges)) return -1; /* ITEM_GET */
        double start = pure_list_get(x->ranges, at, 0).num;
        if (start <= c) low = middle;
        else high = middle;
    }
    return low;
}

static __attribute__((noinline)) double range_integer(Context *x, double c) {
    if (!charge(x, CALL)) return -1;
    int64_t low = 0, high = (int64_t)pure_list_count(x->ranges);
    while (high - low > 1) {
        if (!charge(x, ROUND)) return -1;
        int64_t middle = (low + high) / 2; /* both whole and at or above 0: // is / */
        if ((uint64_t)middle >= pure_list_count(x->ranges)) return -1;
        if (pure_list_get(x->ranges, (size_t)middle, 0).num <= c) low = middle;
        else high = middle;
    }
    return (double)low;
}

static __attribute__((noinline)) double range_flat(Context *x, double c) {
    if (!charge(x, CALL)) return -1;
    double low = 0, high = (double)x->count;
    for (;;) {
        double span = high - low;
        if (!isfinite(span)) return -1;
        if (!(span > 1)) break;
        if (!charge(x, ROUND)) return -1;
        double sum = low + high;
        if (!isfinite(sum)) return -1;
        double middle = floor_divide(sum, 2);
        if (!isfinite(middle)) return -1;
        if (middle != floor(middle) || middle < 0 || middle >= (double)x->count) return -1;
        size_t at = (size_t)middle;
        if (at >= x->count) return -1;
        if (x->starts[at] <= c) low = middle;
        else high = middle;
    }
    return low;
}

static __attribute__((noinline)) double range_both(Context *x, double c) {
    if (!charge(x, CALL)) return -1;
    int64_t low = 0, high = (int64_t)x->count;
    while (high - low > 1) {
        if (!charge(x, ROUND)) return -1;
        int64_t middle = (low + high) / 2;
        if ((uint64_t)middle >= x->count) return -1;
        if (x->starts[middle] <= c) low = middle;
        else high = middle;
    }
    return (double)low;
}

static __attribute__((noinline)) double range_c(Context *x, double c) {
    size_t low = 0, high = x->count;
    while (high - low > 1) {
        size_t middle = (low + high) / 2;
        if (x->starts[middle] <= c) low = middle;
        else high = middle;
    }
    return (double)low;
}

static double rounds_of(const Context *x, double c) { /* the rounds of halving a code point takes */
    size_t low = 0, high = x->count, n = 0;
    while (high - low > 1) {
        size_t middle = (low + high) / 2;
        ++n;
        if (x->starts[middle] <= c) low = middle;
        else high = middle;
    }
    return (double)n;
}

static int counter(uint64_t config) {
    struct perf_event_attr a;
    memset(&a, 0, sizeof(a));
    a.size = sizeof(a), a.type = PERF_TYPE_HARDWARE, a.config = config, a.disabled = 1, a.exclude_kernel = 1, a.exclude_hv = 1;
    return (int)syscall(SYS_perf_event_open, &a, 0, -1, -1, 0);
}

static uint64_t read_counter(int fd) {
    uint64_t v = 0;
    if (read(fd, &v, sizeof(v)) != sizeof(v)) return 0;
    return v;
}

/* The executor's answers: each point's range as two bytes, low first. */
typedef struct { unsigned char *bytes; size_t at, room; } Answers;
static bool take(void *context, PureSpan span) {
    Answers *a = context;
    for (size_t i = 0; i < span.size && a->at < a->room; ++i) a->bytes[a->at++] = span.data[i];
    return true;
}

typedef double (*Way)(Context *, double);

int main(int argc, char **argv) {
    if (argc != 5) { fprintf(stderr, "usage: %s range-out.pmod RANGES LINES ROUNDS\n", argv[0]); return 2; }
    size_t ranges_size, lines_size;
    unsigned char *ranges = whole(argv[2], &ranges_size), *bytes = whole(argv[3], &lines_size);
    int rounds = atoi(argv[4]);
    if (!ranges || !bytes) return 1;
    PureModule module = {0};
    char error[160];
    if (pure_module_read(argv[1], &module, error, sizeof(error)) != PURE_OK) { fprintf(stderr, "%s\n", error); return 1; }
    PureApp app;
    if (pure_app_start(&app, &module, (PureData){0}) != PURE_OK) return 1;
    int load = pure_module_input(&module, "load"), line = pure_module_input(&module, "line");
    int state = pure_module_state(&module, "ranges"), view = pure_module_view(&module, "boundaries");
    if (load < 0 || line < 0 || state < 0 || view < 0) return 1;
    if (pure_app_message(&app, (uint32_t)load, (PureSpan){ranges, ranges_size}) != PURE_OK) return 1;
    Context x = {.ranges = app.lists[state], .budget = UINT64_MAX};
    x.count = pure_list_count(x.ranges);
    double *starts = malloc(x.count * sizeof(double));
    for (size_t i = 0; i < x.count; ++i) starts[i] = pure_list_get(x.ranges, i, 0).num;
    x.starts = starts;
    /* The code points, and the executor's answer for each (a line at a time, as the app takes them). */
    size_t points = 0, wrong = 0;
    double *codes = NULL, *want = NULL, iterations = 0;
    for (size_t at = 0; at + 4 <= lines_size;) {
        uint32_t count;
        memcpy(&count, bytes + at, 4);
        const uint32_t *line_codes = (const uint32_t *)(bytes + at + 4);
        at += 4 + 4 * (size_t)count + count + 1;
        codes = realloc(codes, (points + count) * sizeof(double)), want = realloc(want, (points + count) * sizeof(double));
        unsigned char *payload = malloc(8 * (size_t)count + 1), answer[2 * 4096];
        for (uint32_t i = 0; i < count; ++i) {
            double c = line_codes[i];
            memcpy(payload + 8 * i, &c, 8);
            codes[points + i] = c;
        }
        Answers a = {answer, 0, sizeof(answer)};
        if (pure_app_message(&app, (uint32_t)line, (PureSpan){payload, 8 * (size_t)count}) != PURE_OK ||
            pure_app_view_text(&app, (uint32_t)view, take, &a) != PURE_OK || a.at != 2 * (size_t)count)
            return 1;
        for (uint32_t i = 0; i < count; ++i) want[points + i] = answer[2 * i] + 256.0 * answer[2 * i + 1];
        free(payload);
        points += count;
    }
    for (size_t i = 0; i < points; ++i) iterations += rounds_of(&x, codes[i]);
    const char *names[] = {"exact", "integer", "flat", "both", "c"};
    Way ways[] = {range_exact, range_integer, range_flat, range_both, range_c};
    enum { WAYS = 5 };
    for (int w = 0; w < WAYS; ++w)
        for (size_t i = 0; i < points; ++i)
            if (ways[w](&x, codes[i]) != want[i]) {
                if (wrong++ < 5) fprintf(stderr, "%s: point %zu (%.0f): %.0f, the executor %.0f\n", names[w], i, codes[i],
                                         ways[w](&x, codes[i]), want[i]);
            }
    if (wrong) { fprintf(stderr, "%zu answers differ\n", wrong); return 1; }
    int ins = counter(PERF_COUNT_HW_INSTRUCTIONS), cyc = counter(PERF_COUNT_HW_CPU_CYCLES);
    if (ins < 0 || cyc < 0) { perror("perf_event_open"); return 1; }
    uint64_t total_ins[WAYS] = {0}, total_cyc[WAYS] = {0};
    volatile double sink = 0;
    for (int r = 0; r < rounds; ++r)
        for (int w = 0; w < WAYS; ++w) {
            x.work = 0;
            ioctl(ins, PERF_EVENT_IOC_RESET, 0), ioctl(cyc, PERF_EVENT_IOC_RESET, 0);
            ioctl(ins, PERF_EVENT_IOC_ENABLE, 0), ioctl(cyc, PERF_EVENT_IOC_ENABLE, 0);
            double s = 0;
            for (size_t i = 0; i < points; ++i) s += ways[w](&x, codes[i]);
            ioctl(ins, PERF_EVENT_IOC_DISABLE, 0), ioctl(cyc, PERF_EVENT_IOC_DISABLE, 0);
            sink += s;
            total_ins[w] += read_counter(ins), total_cyc[w] += read_counter(cyc);
        }
    double n = (double)points * rounds;
    for (int w = 0; w < WAYS; ++w)
        printf("{\"way\":\"%s\",\"code_points\":%zu,\"rounds_of_halving_per_point\":%.2f,\"rounds\":%d,\"instructions_per_point\":%.1f,"
               "\"cycles_per_point\":%.1f,\"instructions_per_round\":%.2f,\"cycles_per_round\":%.2f,\"answers\":\"the executor's\"}\n",
               names[w], points, iterations / points, rounds, total_ins[w] / n, total_cyc[w] / n,
               total_ins[w] / (iterations * rounds), total_cyc[w] / (iterations * rounds));
    pure_app_stop(&app);
    return 0;
}
