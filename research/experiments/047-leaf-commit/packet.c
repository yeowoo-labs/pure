/* Research 047: a message's rows as leaves (feedback 009: packet bytes = memory leaf), against today's reading.
 * The rows of UAX #14's load message (6 fields of 8 bytes, 2,787 rows: tests/specs/linebreak/ranges.bin):
 *   today    pure_list_read into an event's list, as message() does (the list made a tree, each row decoded and
 *            inserted)
 *   leaves   the same rows sent as leaves: chunks of 32 rows, each field's 32 cells together (the memory leaf's
 *            columns, core/module-list.c), checked as decode_cell checks them (each number finite, each bool 0 or 1)
 *            and taken where they are: a chunk's address kept, nothing copied
 * Machine instructions and cycles a row (perf_event_open, user mode), ROUNDS passes each.
 *
 *   cc -O2 -std=c17 -I../../../core packet.c ../../../build/libpure.a -lm -pthread -o packet
 *   ./packet ranges.bin ROUNDS     one JSON line */
#define _GNU_SOURCE
#include "module-list.h"
#include <linux/perf_event.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>

enum { FIELDS = 6, RUN = 32 };
static const uint8_t kinds[FIELDS] = {PURE_CELL_NUMBER, PURE_CELL_NUMBER, PURE_CELL_BOOL, PURE_CELL_BOOL, PURE_CELL_BOOL, PURE_CELL_BOOL};

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

/* A leaf as memory keeps one: a 40-byte header (unused here but laid out), then each field's 32 cells. */
typedef struct { unsigned char header[40]; double cells[FIELDS][RUN]; } Leaf;

/* The leaves taken: each checked, then its address kept. */
static __attribute__((noinline)) size_t take_leaves(const Leaf *leaves, size_t chunks, size_t rows, const Leaf **kept) {
    for (size_t c = 0; c < chunks; ++c) {
        const Leaf *leaf = &leaves[c];
        size_t n = c + 1 < chunks ? RUN : rows - c * RUN;
        for (unsigned f = 0; f < FIELDS; ++f) {
            const double *column = leaf->cells[f];
            if (kinds[f] == PURE_CELL_BOOL) {
                for (size_t i = 0; i < n; ++i) if (!(column[i] == 0 || column[i] == 1)) return SIZE_MAX;
            } else
                for (size_t i = 0; i < n; ++i) if (!isfinite(column[i])) return SIZE_MAX;
        }
        kept[c] = leaf;
    }
    return rows;
}

int main(int argc, char **argv) {
    if (argc != 3) { fprintf(stderr, "usage: %s ranges.bin ROUNDS\n", argv[0]); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 1;
    static unsigned char bytes[1 << 20];
    size_t size = fread(bytes, 1, sizeof(bytes), f);
    fclose(f);
    int rounds = atoi(argv[2]);
    size_t rows = size / (8 * FIELDS), chunks = (rows + RUN - 1) / RUN;
    /* The same rows laid out as leaves (what a sender would put on the wire). */
    Leaf *leaves = calloc(chunks, sizeof(Leaf));
    for (size_t r = 0; r < rows; ++r)
        for (unsigned g = 0; g < FIELDS; ++g) memcpy(&leaves[r / RUN].cells[g][r % RUN], bytes + 8 * (FIELDS * r + g), 8);
    const Leaf **kept = calloc(chunks, sizeof(*kept));
    PureList *list = pure_list_new(FIELDS, kinds);
    if (!list || !pure_list_arrays(list)) return 1;
    /* Both read the same: each row's fields. */
    if (pure_list_read(list, bytes, size) != PURE_OK || pure_list_count(list) != rows ||
        take_leaves(leaves, chunks, rows, kept) != rows)
        return 1;
    for (size_t r = 0; r < rows; ++r)
        for (unsigned g = 0; g < FIELDS; ++g)
            if (pure_list_get(list, r, g).num != kept[r / RUN]->cells[g][r % RUN]) { fprintf(stderr, "row %zu differs\n", r); return 1; }
    int ins = counter(PERF_COUNT_HW_INSTRUCTIONS), cyc = counter(PERF_COUNT_HW_CPU_CYCLES);
    if (ins < 0 || cyc < 0) return 1;
    uint64_t today_ins = 0, today_cyc = 0, leaves_ins = 0, leaves_cyc = 0;
    for (int k = 0; k < rounds; ++k) {
        pure_list_clear(list);
        ioctl(ins, PERF_EVENT_IOC_RESET, 0), ioctl(cyc, PERF_EVENT_IOC_RESET, 0);
        ioctl(ins, PERF_EVENT_IOC_ENABLE, 0), ioctl(cyc, PERF_EVENT_IOC_ENABLE, 0);
        PureResult read = pure_list_read(list, bytes, size);
        ioctl(ins, PERF_EVENT_IOC_DISABLE, 0), ioctl(cyc, PERF_EVENT_IOC_DISABLE, 0);
        if (read != PURE_OK) return 1;
        today_ins += read_counter(ins), today_cyc += read_counter(cyc);
        ioctl(ins, PERF_EVENT_IOC_RESET, 0), ioctl(cyc, PERF_EVENT_IOC_RESET, 0);
        ioctl(ins, PERF_EVENT_IOC_ENABLE, 0), ioctl(cyc, PERF_EVENT_IOC_ENABLE, 0);
        size_t taken = take_leaves(leaves, chunks, rows, kept);
        ioctl(ins, PERF_EVENT_IOC_DISABLE, 0), ioctl(cyc, PERF_EVENT_IOC_DISABLE, 0);
        if (taken != rows) return 1;
        leaves_ins += read_counter(ins), leaves_cyc += read_counter(cyc);
    }
    double n = (double)rows * rounds;
    printf("{\"rows\":%zu,\"fields\":%d,\"chunks\":%zu,\"rounds\":%d,\"today_instructions_per_row\":%.1f,\"today_cycles_per_row\":%.1f,"
           "\"leaves_instructions_per_row\":%.1f,\"leaves_cycles_per_row\":%.1f,\"answers\":\"every cell the same both ways\"}\n",
           rows, FIELDS, chunks, rounds, today_ins / n, today_cyc / n, leaves_ins / n, leaves_cyc / n);
    return 0;
}
