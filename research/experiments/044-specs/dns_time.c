/* Research 140: DNS messages (RFC 1035) in aup (aup's examples/dns.aup, run by Pure) against the same algorithm in C
 * (each message read into its records, names uncompressed, then written back with the owners compressed) and against
 * glibc's resolver reading them (ns_initparse, ns_parserr and ns_name_unpack for the names in data: reading only, the
 * world's C has no writer of whole messages), on the same messages. Pure's and C's written bytes must be the same, and
 * the resolver must take every message; then each is timed (the median of seven runs of rounds passes).
 *
 *   cc -O2 -std=c17 -I../../../core dns_time.c ../../../build/libpure.a -lresolv -lm -pthread -o dns_time
 *   ./dns_time dns.pmod MESSAGES ROUNDS     (MESSAGES: each a u16 big-endian length, then its bytes, as over TCP)
 *
 * One JSON line: the messages and their bytes, the nanoseconds per message byte of each, Pure's ratios and the
 * instructions Pure counted for one pass (each event's and each view's). */
#define _DEFAULT_SOURCE
#include "module.h"
#include <arpa/nameser.h>
#include <resolv.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- the same algorithm in C */
typedef struct {
    int section, type, class;
    unsigned long ttl;
    unsigned char name[256], data[1024];
    size_t name_size, data_size;
} Record;

static int word(const unsigned char *d, size_t i) { return d[i] << 8 | d[i + 1]; }

/* The name at i uncompressed into out; where it ends in place, 0 if it is none. */
static size_t name(const unsigned char *d, size_t size, size_t i, unsigned char *out, size_t *used) {
    size_t j = i, end = 0, below = i, n = 0;
    for (;;) {
        if (j >= size) return 0;
        if (!d[j]) break;
        if (d[j] >= 0xc0) {
            if (j + 1 >= size) return 0;
            size_t to = (size_t)((d[j] & 0x3f) << 8 | d[j + 1]);
            if (to >= below) return 0;
            if (!end) end = j + 2;
            below = j = to;
        } else {
            if (d[j] >= 0x40 || j + d[j] + 1 > size || n + d[j] + 1 >= 255) return 0;
            memcpy(out + n, d + j, d[j] + 1u);
            n += d[j] + 1u;
            j += d[j] + 1u;
        }
    }
    out[n++] = 0;
    *used = n;
    return end ? end : j + 1;
}

static int data(const unsigned char *d, size_t size, size_t i, size_t length, int type, Record *r) {
    int skip = type == 15 ? 2 : type == 33 ? 6 : 0;
    size_t used, end;
    if (type == 2 || type == 5 || type == 12 || type == 15 || type == 33) {
        memcpy(r->data, d + i, (size_t)skip);
        if ((end = name(d, size, i + (size_t)skip, r->data + skip, &used)) != i + length) return -1;
        r->data_size = (size_t)skip + used;
    } else if (type == 6) {
        size_t a, b;
        if (!(end = name(d, size, i, r->data, &a)) || (end = name(d, size, end, r->data + a, &b)) + 20 != i + length) return -1;
        memcpy(r->data + a + b, d + end, 20);
        r->data_size = a + b + 20;
    } else {
        if (length > sizeof(r->data)) return -1;
        memcpy(r->data, d + i, length);
        r->data_size = length;
    }
    return 0;
}

/* The records of a message (at most room); their count, -1 if it is none. */
static int read_message(const unsigned char *d, size_t size, Record *records, int room, int *id, int *flags) {
    if (size < 12) return -1;
    int n = 0;
    size_t at = 12;
    for (int section = 0; section < 4; ++section)
        for (int k = word(d, 4 + 2 * (size_t)section); k > 0; --k) {
            if (n == room) return -1;
            Record *r = &records[n++];
            size_t next = name(d, size, at, r->name, &r->name_size);
            if (!next || next + 4 > size) return -1;
            r->section = section, r->type = word(d, next), r->class = word(d, next + 2), r->ttl = 0, r->data_size = 0;
            at = next + 4;
            if (section) {
                if (at + 6 > size) return -1;
                size_t length = (size_t)word(d, at + 4);
                if (at + 6 + length > size) return -1;
                r->ttl = (unsigned long)word(d, at) << 16 | (unsigned long)word(d, at + 2);
                if (data(d, size, at + 6, length, r->type, r)) return -1;
                at += 6 + length;
            }
        }
    if (at != size) return -1;
    *id = word(d, 0), *flags = word(d, 2);
    return n;
}

static void two(unsigned char *out, size_t *n, unsigned v) { out[(*n)++] = (unsigned char)(v >> 8), out[(*n)++] = (unsigned char)v; }

/* The message's bytes, each owner from its first label whose rest was written before, as a pointer to it. */
static size_t write_message(const Record *records, int count, int id, int flags, unsigned char *out) {
    static struct { const unsigned char *name; size_t size, at; } seen[4096];
    size_t n = 0, kept = 0;
    two(out, &n, (unsigned)id), two(out, &n, (unsigned)flags);
    for (int section = 0; section < 4; ++section) {
        unsigned c = 0;
        for (int i = 0; i < count; ++i) c += records[i].section == section;
        two(out, &n, c);
    }
    for (int i = 0; i < count; ++i) {
        const Record *r = &records[i];
        size_t k = 0;
        bool done = false;
        while (!done && r->name[k]) {
            for (size_t s = 0; s < kept && !done; ++s)
                if (seen[s].size == r->name_size - k && !memcmp(seen[s].name, r->name + k, seen[s].size)) {
                    two(out, &n, 0xc000u | (unsigned)seen[s].at);
                    done = true;
                }
            if (!done) {
                if (n < 0x4000 && kept < 4096) seen[kept].name = r->name + k, seen[kept].size = r->name_size - k, seen[kept++].at = n;
                memcpy(out + n, r->name + k, r->name[k] + 1u);
                n += r->name[k] + 1u;
                k += r->name[k] + 1u;
            }
        }
        if (!done) out[n++] = 0;
        two(out, &n, (unsigned)r->type), two(out, &n, (unsigned)r->class);
        if (r->section) {
            two(out, &n, (unsigned)(r->ttl >> 16)), two(out, &n, (unsigned)r->ttl), two(out, &n, (unsigned)r->data_size);
            memcpy(out + n, r->data, r->data_size);
            n += r->data_size;
        }
    }
    return n;
}

/* ---- glibc's resolver reading a message: every record, and the names in NS, CNAME, PTR, MX, SOA and SRV data */
static int resolver_read(const unsigned char *d, size_t size) {
    ns_msg msg;
    if (ns_initparse(d, (int)size, &msg)) return -1;
    int n = 0;
    for (int section = 0; section < 4; ++section)
        for (int k = 0; k < ns_msg_count(msg, (ns_sect)section); ++k) {
            ns_rr rr;
            if (ns_parserr(&msg, (ns_sect)section, k, &rr)) return -1;
            ++n;
            int type = ns_rr_type(rr), skip = type == 15 ? 2 : type == 33 ? 6 : 0;
            unsigned char name[NS_MAXCDNAME];
            if (section && (type == 2 || type == 5 || type == 12 || type == 15 || type == 33 || type == 6)) {
                const unsigned char *at = ns_rr_rdata(rr) + skip;
                int used = ns_name_unpack(d, d + size, at, name, sizeof(name));
                if (used < 0 || (type == 6 && ns_name_unpack(d, d + size, at + used, name, sizeof(name)) < 0)) return -1;
            }
        }
    return n;
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

typedef struct { unsigned char *bytes; size_t used, room; } Gathered;
static bool gather(void *context, PureSpan span) {
    Gathered *g = context;
    if (g->used + span.size > g->room) return false;
    memcpy(g->bytes + g->used, span.data, span.size);
    g->used += span.size;
    return true;
}

int main(int argc, char **argv) {
    if (argc != 4) { fprintf(stderr, "usage: %s MODULE MESSAGES ROUNDS\n", argv[0]); return 2; }
    FILE *f = fopen(argv[2], "rb");
    if (!f) { fprintf(stderr, "cannot read %s\n", argv[2]); return 1; }
    static unsigned char all[1 << 24];
    size_t size = fread(all, 1, sizeof(all), f);
    fclose(f);
    int rounds = atoi(argv[3]), count = 0;
    static size_t starts[65536], sizes[65536];
    size_t bytes = 0;
    for (size_t at = 0; at + 2 <= size && count < 65536; at += 2 + sizes[count++])
        starts[count] = at + 2, sizes[count] = (size_t)word(all, at), bytes += sizes[count];

    PureModule module = {0};
    char error[160];
    if (pure_module_read(argv[1], &module, error, sizeof(error)) != PURE_OK) { fprintf(stderr, "%s\n", error); return 1; }
    PureApp app;
    if (pure_app_start(&app, &module, (PureData){0}) != PURE_OK) return 1;
    int input = pure_module_input(&module, "read"), view = pure_module_view(&module, "wire");
    static Record records[4096];
    static unsigned char written[1 << 17], viewed[1 << 17];
    uint64_t work = 0; /* what each event and each view evaluation used (app.work counts the unit running) */
    for (int m = 0; m < count; ++m) {
        int id, flags, n = read_message(all + starts[m], sizes[m], records, 4096, &id, &flags);
        if (n < 0 || resolver_read(all + starts[m], sizes[m]) != n) { fprintf(stderr, "message %d not read\n", m); return 1; }
        size_t length = write_message(records, n, id, flags, written);
        Gathered g = {viewed, 0, sizeof(viewed)};
        bool read = pure_app_message(&app, (uint32_t)input, (PureSpan){all + starts[m], sizes[m]}) == PURE_OK;
        work += app.work;
        if (!read || pure_app_view_text(&app, (uint32_t)view, gather, &g) != PURE_OK || g.used != length ||
            memcmp(viewed, written, length)) {
            fprintf(stderr, "message %d: Pure's bytes differ\n", m);
            return 1;
        }
        work += app.work;
    }

    double c_runs[7], resolver_runs[7], pure_runs[7];
    volatile size_t sink = 0;
    for (int r = 0; r < 7; ++r) {
        double start = now_ns();
        for (int i = 0; i < rounds; ++i)
            for (int m = 0; m < count; ++m) {
                int id, flags, n = read_message(all + starts[m], sizes[m], records, 4096, &id, &flags);
                sink += write_message(records, n, id, flags, written);
            }
        c_runs[r] = (now_ns() - start) / rounds;
        start = now_ns();
        for (int i = 0; i < rounds; ++i)
            for (int m = 0; m < count; ++m) sink += (size_t)resolver_read(all + starts[m], sizes[m]);
        resolver_runs[r] = (now_ns() - start) / rounds;
        start = now_ns();
        for (int i = 0; i < rounds; ++i)
            for (int m = 0; m < count; ++m) {
                Gathered g = {viewed, 0, sizeof(viewed)};
                if (pure_app_message(&app, (uint32_t)input, (PureSpan){all + starts[m], sizes[m]}) != PURE_OK ||
                    pure_app_view_text(&app, (uint32_t)view, gather, &g) != PURE_OK) return 1;
                sink += g.used;
            }
        pure_runs[r] = (now_ns() - start) / rounds;
    }
    qsort(c_runs, 7, sizeof(double), by_value);
    qsort(resolver_runs, 7, sizeof(double), by_value);
    qsort(pure_runs, 7, sizeof(double), by_value);
    double n = (double)bytes;
    const char *jit = getenv("PURE_JIT"), *batch = getenv("PURE_BATCH");
    printf("{\"messages\":%d,\"bytes\":%zu,\"jit\":\"%s\",\"batch\":\"%s\",\"c_ns_per_byte\":%.3f,\"resolver_read_ns_per_byte\":%.3f,"
           "\"pure_ns_per_byte\":%.3f,\"to_c\":%.1f,\"to_resolver_read\":%.1f,\"instructions\":%llu,\"instructions_per_byte\":%.1f}\n",
           count, bytes, jit ? jit : "", batch ? batch : "", c_runs[3] / n, resolver_runs[3] / n, pure_runs[3] / n,
           pure_runs[3] / c_runs[3], pure_runs[3] / resolver_runs[3], (unsigned long long)work, (double)work / n);
    pure_app_stop(&app);
    return 0;
}
