/* Research 045: where Pure's machine instructions go on UAX #14, by mechanism. The app (aup's examples/linebreak.aup)
 * on the lines 044's linebreak_check.py writes, its answers checked first; then ROUNDS passes with a user-mode
 * instruction counter sampling its address every PERIOD instructions (perf_event_open, PERF_SAMPLE_IP, read from its
 * ring after each line). Built without PIE, so where.py can give the addresses to addr2line as they are.
 *
 *   cc -O2 -g -no-pie -std=c17 -I../../../core -I../../../../vega/text where.c ../../../../vega/text/linebreak.c \
 *      ../../../../vega/text/unicode-data.c ../../../build/libpure.a -lm -pthread -o where
 *   ./where MODULE RANGES LINES ROUNDS PERIOD > samples.txt     "instructions N points N" then "ADDRESS COUNT" lines,
 *                                                                     the maps on stderr
 * WHERE_VARIANT=1: a variant (events.c's), whose answers are not the test's. */
#define _GNU_SOURCE
#include "module.h"
#include "text.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
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

typedef struct { uint32_t count; const uint32_t *codes; const unsigned char *want; unsigned char *payload; } Line;

/* Addresses sampled, counted in an open table. */
enum { SLOTS = 1 << 20, PAGES = 64 };
static uint64_t address[SLOTS], hits[SLOTS];

static void count_address(uint64_t ip) {
    for (uint64_t k = (ip * 0x9E3779B97F4A7C15u) >> 44;; k = (k + 1) & (SLOTS - 1)) {
        if (address[k] == ip) { ++hits[k]; return; }
        if (!address[k]) { address[k] = ip, hits[k] = 1; return; }
    }
}

/* The samples the ring holds: a header and the address each (PERF_SAMPLE_IP); lost records skipped. */
static void drain(struct perf_event_mmap_page *head, size_t data_size) {
    uint64_t end = __atomic_load_n(&head->data_head, __ATOMIC_ACQUIRE), at = head->data_tail;
    const unsigned char *data = (const unsigned char *)head + head->data_offset;
    while (at < end) {
        struct perf_event_header h;
        for (size_t i = 0; i < sizeof(h); ++i) ((unsigned char *)&h)[i] = data[(at + i) % data_size];
        if (h.type == PERF_RECORD_SAMPLE) {
            uint64_t ip;
            for (size_t i = 0; i < sizeof(ip); ++i) ((unsigned char *)&ip)[i] = data[(at + sizeof(h) + i) % data_size];
            count_address(ip);
        }
        at += h.size;
    }
    __atomic_store_n(&head->data_tail, at, __ATOMIC_RELEASE);
}

int main(int argc, char **argv) {
    if (argc != 6) { fprintf(stderr, "usage: %s MODULE RANGES LINES ROUNDS PERIOD\n", argv[0]); return 2; }
    size_t ranges_size, lines_size;
    unsigned char *ranges = whole(argv[2], &ranges_size), *bytes = whole(argv[3], &lines_size);
    int rounds = atoi(argv[4]);
    uint64_t period = strtoull(argv[5], NULL, 10);
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
    for (size_t k = 0; k < count; ++k) { /* checked first: Pure gives the test's boundaries */
        bool same = pure_app_message(&app, (uint32_t)line, (PureSpan){lines[k].payload, 8 * (size_t)lines[k].count}) == PURE_OK &&
                    lines[k].count < sizeof(got) && vega_line_breaks(lines[k].codes, lines[k].count, got);
        for (uint32_t i = 0; same && i <= lines[k].count; ++i) same = (got[i] != VEGA_BREAK_NONE) == lines[k].want[i];
        right += same;
    }
    if (right != count && !getenv("WHERE_VARIANT")) { fprintf(stderr, "right %zu of %zu\n", right, count); return 1; }
    struct perf_event_attr a;
    memset(&a, 0, sizeof(a));
    a.size = sizeof(a), a.type = PERF_TYPE_HARDWARE, a.config = PERF_COUNT_HW_INSTRUCTIONS, a.disabled = 1,
    a.exclude_kernel = 1, a.exclude_hv = 1, a.sample_period = period, a.sample_type = PERF_SAMPLE_IP;
    int fd = (int)syscall(SYS_perf_event_open, &a, 0, -1, -1, 0);
    if (fd < 0) { perror("perf_event_open"); return 1; }
    size_t page = (size_t)sysconf(_SC_PAGESIZE), data_size = PAGES * page;
    struct perf_event_mmap_page *head = mmap(NULL, page + data_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (head == MAP_FAILED) { perror("mmap"); return 1; }
    ioctl(fd, PERF_EVENT_IOC_RESET, 0);
    for (int r = 0; r < rounds; ++r)
        for (size_t k = 0; k < count; ++k) {
            ioctl(fd, PERF_EVENT_IOC_ENABLE, 0);
            PureResult did = pure_app_message(&app, (uint32_t)line, (PureSpan){lines[k].payload, 8 * (size_t)lines[k].count});
            ioctl(fd, PERF_EVENT_IOC_DISABLE, 0);
            if (did != PURE_OK) return 1;
            drain(head, data_size);
        }
    uint64_t total = 0;
    if (read(fd, &total, sizeof(total)) != sizeof(total)) return 1;
    printf("instructions %llu points %llu\n", (unsigned long long)total, (unsigned long long)points * (unsigned long long)rounds);
    for (size_t k = 0; k < SLOTS; ++k)
        if (address[k]) printf("%llx %llu\n", (unsigned long long)address[k], (unsigned long long)hits[k]);
    FILE *maps = fopen("/proc/self/maps", "r"); /* to stderr: where the shared libraries were (libc's malloc, memcpy) */
    for (int c; maps && (c = fgetc(maps)) != EOF;) fputc(c, stderr);
    if (maps) fclose(maps);
    pure_app_stop(&app);
    return 0;
}
