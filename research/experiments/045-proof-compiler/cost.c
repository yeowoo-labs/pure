/* Research 045 (029's principle 1, its breaking experiment): what UAX #14 costs in machine instructions and cycles a
 * code point, Vega's C (text/linebreak.c) and Pure's executor (aup's examples/linebreak.aup), on the lines
 * 044's linebreak_check.py writes; user-mode counters (perf_event_open) around each, ROUNDS passes after one checked.
 *
 *   cc -O2 -std=c17 -I../../../core -I../../../../vega/text cost.c ../../../../vega/text/linebreak.c \
 *      ../../../../vega/text/unicode-data.c ../../../build/libpure.a -lm -pthread -o cost
 *   ./cost linebreak.pmod RANGES LINES ROUNDS
 *
 * One JSON line. */
#define _GNU_SOURCE
#include "module.h"
#include "text.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>

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

int main(int argc, char **argv) {
    if (argc != 5) { fprintf(stderr, "usage: %s MODULE RANGES LINES ROUNDS\n", argv[0]); return 2; }
    size_t ranges_size, lines_size;
    unsigned char *ranges = whole(argv[2], &ranges_size), *bytes = whole(argv[3], &lines_size);
    int rounds = atoi(argv[4]);
    if (!ranges || !bytes) return 1;
    static Line lines[1 << 16];
    size_t count = 0, points = 0;
    for (size_t at = 0; at + 4 <= lines_size && count < sizeof(lines) / sizeof(*lines); ++count) {
        Line *l = &lines[count];
        memcpy(&l->count, bytes + at, 4);
        l->codes = (const uint32_t *)(bytes + at + 4);
        l->want = bytes + at + 4 + 4 * (size_t)l->count;
        at += 4 + 4 * (size_t)l->count + l->count + 1;
        l->payload = malloc(8 * (size_t)l->count + 1);
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
    int load = pure_module_input(&module, "load"), line = pure_module_input(&module, "line");
    if (pure_app_message(&app, (uint32_t)load, (PureSpan){ranges, ranges_size}) != PURE_OK) return 1;
    static uint8_t got[4096];
    size_t right = 0;
    for (size_t k = 0; k < count; ++k) { /* checked first: both give the test's boundaries */
        bool same = pure_app_message(&app, (uint32_t)line, (PureSpan){lines[k].payload, 8 * (size_t)lines[k].count}) == PURE_OK &&
                    lines[k].count < sizeof(got) && vega_line_breaks(lines[k].codes, lines[k].count, got);
        for (uint32_t i = 0; same && i <= lines[k].count; ++i) same = (got[i] != VEGA_BREAK_NONE) == lines[k].want[i];
        right += same;
    }
    if (right != count) { fprintf(stderr, "right %zu of %zu\n", right, count); return 1; }
    int ins = counter(PERF_COUNT_HW_INSTRUCTIONS), cyc = counter(PERF_COUNT_HW_CPU_CYCLES);
    if (ins < 0 || cyc < 0) { perror("perf_event_open"); return 1; }
    volatile unsigned sink = 0;
    ioctl(ins, PERF_EVENT_IOC_RESET, 0), ioctl(cyc, PERF_EVENT_IOC_RESET, 0);
    ioctl(ins, PERF_EVENT_IOC_ENABLE, 0), ioctl(cyc, PERF_EVENT_IOC_ENABLE, 0);
    for (int r = 0; r < rounds; ++r)
        for (size_t k = 0; k < count; ++k) {
            vega_line_breaks(lines[k].codes, lines[k].count, got);
            sink += got[0];
        }
    ioctl(ins, PERF_EVENT_IOC_DISABLE, 0), ioctl(cyc, PERF_EVENT_IOC_DISABLE, 0);
    uint64_t c_ins = read_counter(ins), c_cyc = read_counter(cyc);
    ioctl(ins, PERF_EVENT_IOC_RESET, 0), ioctl(cyc, PERF_EVENT_IOC_RESET, 0);
    ioctl(ins, PERF_EVENT_IOC_ENABLE, 0), ioctl(cyc, PERF_EVENT_IOC_ENABLE, 0);
    for (int r = 0; r < rounds; ++r)
        for (size_t k = 0; k < count; ++k)
            if (pure_app_message(&app, (uint32_t)line, (PureSpan){lines[k].payload, 8 * (size_t)lines[k].count}) != PURE_OK) return 1;
    ioctl(ins, PERF_EVENT_IOC_DISABLE, 0), ioctl(cyc, PERF_EVENT_IOC_DISABLE, 0);
    uint64_t p_ins = read_counter(ins), p_cyc = read_counter(cyc);
    double n = (double)points * rounds;
    const char *jit = getenv("PURE_JIT");
    printf("{\"lines\":%zu,\"code_points\":%zu,\"rounds\":%d,\"jit\":\"%s\",\"c_instructions_per_point\":%.1f,\"c_cycles_per_point\":%.1f,"
           "\"pure_instructions_per_point\":%.1f,\"pure_cycles_per_point\":%.1f}\n",
           count, points, rounds, jit ? jit : "", c_ins / n, c_cyc / n, p_ins / n, p_cyc / n);
    pure_app_stop(&app);
    return 0;
}
