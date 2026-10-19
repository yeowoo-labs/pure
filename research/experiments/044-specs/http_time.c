/* Research 140: HTTP/1.1 requests read a packet at a time in aup (aup's examples/http.aup, run by Pure) against the
 * same algorithm in C, on the same streams (each whole, one packet). No C library for it is on the machine, so the
 * world's reference is h11 in Python (http_check.py's oracle). C's and Pure's requests must be as many as the streams
 * hold, none left over; then each is timed (the median of seven runs of rounds passes).
 *
 *   cc -O2 -std=c17 -I../../../core http_time.c ../../../build/libpure.a -lm -pthread -o http_time
 *   ./http_time http.pmod STREAMS ROUNDS     (STREAMS: each a u32 big-endian length, then its bytes, all whole)
 *
 * One JSON line: the streams, their bytes and requests, the nanoseconds per byte of each, Pure's ratio and the
 * instructions Pure counted for one pass. */
#define _POSIX_C_SOURCE 200809L
#include "module.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

/* ---- the same algorithm in C */
typedef struct { size_t name, name_size, value, value_size; } Field;
typedef struct {
    size_t method, method_size, target, target_size;
    int minor;
    Field fields[64], trailers[16];
    int count, trailer_count;
    unsigned char lowered[2048]; /* the names lowercased */
    size_t lowered_used;
    unsigned char body[1 << 16];
    size_t body_size;
} Request;

static bool token(unsigned char b) {
    return (b >= 'a' && b <= 'z') || (b >= 'A' && b <= 'Z') || (b >= '0' && b <= '9') || (b && strchr("!#$%&'*+-.^_`|~", b));
}
static bool hex(unsigned char b) { return (b >= '0' && b <= '9') || (b >= 'a' && b <= 'f') || (b >= 'A' && b <= 'F'); }
static bool space(unsigned char b) { return !b || (b >= 9 && b <= 13) || b == ' '; }
static bool blank(unsigned char b) { return b == ' ' || b == '\t'; }

static long crlf(const unsigned char *d, size_t size, size_t i) {
    for (size_t j = i; j + 1 < size; ++j)
        if (d[j] == '\r' && d[j + 1] == '\n') return (long)j;
    return -1;
}

/* The field line d[i .. e] into f (its name lowercased into r's room); false if it is none. */
static bool field(const unsigned char *d, size_t i, size_t e, Request *r, Field *f) {
    size_t c = i;
    while (c < e && d[c] != ':') ++c;
    if (c == i || c == e || r->lowered_used + (c - i) > sizeof(r->lowered)) return false;
    f->name = r->lowered_used, f->name_size = c - i;
    for (size_t k = i; k < c; ++k) {
        if (!token(d[k])) return false;
        r->lowered[r->lowered_used++] = d[k] >= 'A' && d[k] <= 'Z' ? (unsigned char)(d[k] + 32) : d[k];
    }
    size_t a = c + 1, b = e;
    while (a < b && blank(d[a])) ++a;
    while (b > a && blank(d[b - 1])) --b;
    for (size_t k = a; k < b; ++k)
        if (space(d[k]) && !blank(d[k])) return false;
    f->value = a, f->value_size = b - a;
    return true;
}

/* The field lines from i into fields; where they end, -1 if they have not all come, -2 if they are none. */
static long fields(const unsigned char *d, size_t size, size_t i, Request *r, Field *into, int room, int *count) {
    *count = 0;
    for (;;) {
        long e = crlf(d, size, i);
        if (e < 0) return -1;
        if ((size_t)e == i) return e + 2;
        if (*count == room || !field(d, i, (size_t)e, r, &into[(*count)++])) return -2;
        i = (size_t)e + 2;
    }
}

static bool named(const Request *r, const Field *f, const char *name) {
    return f->name_size == strlen(name) && !memcmp(r->lowered + f->name, name, f->name_size);
}

/* The request at the start of d and its length: -1 if d holds only part of one, -2 if it is none. */
static long request(const unsigned char *d, size_t size, Request *r) {
    long e = crlf(d, size, 0);
    if (e < 0) return -1;
    size_t s1 = 0, s2;
    while ((long)s1 < e && d[s1] != ' ') ++s1;
    for (s2 = s1 + 1; (long)s2 < e && d[s2] != ' '; ++s2) {}
    if (!s1 || s2 <= s1 + 1 || (long)s2 + 9 != e || memcmp(d + s2 + 1, "HTTP/1.", 7) || (d[e - 1] != '0' && d[e - 1] != '1'))
        return -2;
    for (size_t k = 0; k < s1; ++k) if (!token(d[k])) return -2;
    for (size_t k = s1 + 1; k < s2; ++k) if (d[k] < 0x21 || d[k] > 0x7e) return -2;
    r->method = 0, r->method_size = s1, r->target = s1 + 1, r->target_size = s2 - s1 - 1, r->minor = d[e - 1] - '0';
    r->lowered_used = 0, r->body_size = 0, r->trailer_count = 0;
    long i = fields(d, size, (size_t)e + 2, r, r->fields, 64, &r->count);
    if (i < 0) return i;
    int hosts = 0, coded = 0, lengths = 0;
    bool chunked = false;
    double length = 0;
    for (int k = 0; k < r->count; ++k) {
        const Field *f = &r->fields[k];
        if (named(r, f, "host")) ++hosts;
        if (named(r, f, "transfer-encoding")) {
            ++coded;
            chunked = f->value_size == 7 && !strncasecmp((const char *)d + f->value, "chunked", 7);
        }
        if (named(r, f, "content-length")) {
            ++lengths;
            if (!f->value_size) return -2;
            for (size_t b = 0; b < f->value_size; ++b) {
                if (d[f->value + b] < '0' || d[f->value + b] > '9') return -2;
                length = length * 10 + (d[f->value + b] - '0');
            }
        }
    }
    if (hosts > 1 || (!hosts && r->minor == 1)) return -2;
    size_t j = (size_t)i;
    if (coded) {
        if (lengths || coded > 1 || !chunked) return -2;
        for (;;) {
            long c = crlf(d, size, j);
            if (c < 0) return -1;
            size_t semi = j, z;
            while ((long)semi < c && d[semi] != ';') ++semi;
            z = semi;
            if ((long)semi == c) while (z > j && blank(d[z - 1])) --z;
            if (z == j || z - j > 20) return -2;
            double n = 0;
            for (size_t k = j; k < z; ++k) {
                if (!hex(d[k])) return -2;
                n = n * 16 + (d[k] <= '9' ? d[k] - '0' : d[k] >= 'a' ? d[k] - 'a' + 10 : d[k] - 'A' + 10);
            }
            for (size_t k = semi; (long)k < c; ++k) if (d[k] == '\n') return -2;
            if (n > 2147483648.0) return -2;
            if (n == 0) {
                long after = fields(d, size, (size_t)c + 2, r, r->trailers, 16, &r->trailer_count);
                if (after < 0) return after;
                return after;
            }
            size_t m = (size_t)n;
            if (size < (size_t)c + 4 + m) return -1;
            if (d[c + 2 + m] != '\r' || d[c + 3 + m] != '\n' || r->body_size + m > sizeof(r->body)) return -2;
            memcpy(r->body + r->body_size, d + c + 2, m);
            r->body_size += m;
            j = (size_t)c + 4 + m;
        }
    }
    if (lengths) {
        if (lengths > 1 || length > 2147483648.0) return -2;
        if (size < j + (size_t)length) return -1;
        if ((size_t)length > sizeof(r->body)) return -2;
        memcpy(r->body, d + j, (size_t)length);
        r->body_size = (size_t)length;
        j += (size_t)length;
    }
    return (long)j;
}

static unsigned char pending[1 << 20];
static size_t pending_size;
static Request current;

/* The requests a packet completes (counted), the rest kept; -1 if the stream is no HTTP. */
static long feed(const unsigned char *packet, size_t size) {
    if (pending_size + size > sizeof(pending)) return -1;
    memcpy(pending + pending_size, packet, size);
    pending_size += size;
    long count = 0, n;
    size_t at = 0;
    while (at < pending_size && (n = request(pending + at, pending_size - at, &current)) >= 0) at += (size_t)n, ++count;
    if (at < pending_size && n == -2) return -1;
    memmove(pending, pending + at, pending_size - at);
    pending_size -= at;
    return count;
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
    if (argc != 4) { fprintf(stderr, "usage: %s MODULE STREAMS ROUNDS\n", argv[0]); return 2; }
    FILE *f = fopen(argv[2], "rb");
    if (!f) { fprintf(stderr, "cannot read %s\n", argv[2]); return 1; }
    static unsigned char all[1 << 24];
    size_t size = fread(all, 1, sizeof(all), f), bytes = 0;
    fclose(f);
    int rounds = atoi(argv[3]), count = 0;
    static size_t starts[65536], sizes[65536];
    for (size_t at = 0; at + 4 <= size && count < 65536; at += 4 + sizes[count++]) {
        starts[count] = at + 4;
        sizes[count] = (size_t)all[at] << 24 | (size_t)all[at + 1] << 16 | (size_t)all[at + 2] << 8 | all[at + 3];
        bytes += sizes[count];
    }

    PureModule module = {0};
    char error[160];
    if (pure_module_read(argv[1], &module, error, sizeof(error)) != PURE_OK) { fprintf(stderr, "%s\n", error); return 1; }
    PureApp app;
    if (pure_app_start(&app, &module, (PureData){0}) != PURE_OK) return 1;
    int input = pure_module_input(&module, "packet"), requests = pure_module_state(&module, "requests"),
        left = pure_module_state(&module, "pending");
    long c_requests = 0;
    uint64_t work = 0;
    for (int s = 0; s < count; ++s) {
        long got = feed(all + starts[s], sizes[s]);
        if (got < 0 || pending_size) { fprintf(stderr, "stream %d: C read no whole requests\n", s); return 1; }
        c_requests += got;
        if (pure_app_message(&app, (uint32_t)input, (PureSpan){all + starts[s], sizes[s]}) != PURE_OK) {
            fprintf(stderr, "stream %d: Pure's event failed\n", s);
            return 1;
        }
        work += app.work;
    }
    if ((long)pure_list_count(app.lists[requests]) != c_requests || pure_list_count(app.lists[left])) {
        fprintf(stderr, "Pure read %zu requests, C %ld\n", pure_list_count(app.lists[requests]), c_requests);
        return 1;
    }

    double c_runs[7], pure_runs[7];
    for (int r = 0; r < 7; ++r) {
        double start = now_ns();
        for (int i = 0; i < rounds; ++i)
            for (int s = 0; s < count; ++s) feed(all + starts[s], sizes[s]);
        c_runs[r] = (now_ns() - start) / rounds;
        start = now_ns();
        for (int i = 0; i < rounds; ++i)
            for (int s = 0; s < count; ++s)
                if (pure_app_message(&app, (uint32_t)input, (PureSpan){all + starts[s], sizes[s]}) != PURE_OK) return 1;
        pure_runs[r] = (now_ns() - start) / rounds;
    }
    qsort(c_runs, 7, sizeof(double), by_value);
    qsort(pure_runs, 7, sizeof(double), by_value);
    double n = (double)bytes;
    const char *jit = getenv("PURE_JIT"), *batch = getenv("PURE_BATCH");
    printf("{\"streams\":%d,\"bytes\":%zu,\"requests\":%ld,\"jit\":\"%s\",\"batch\":\"%s\",\"c_ns_per_byte\":%.3f,"
           "\"pure_ns_per_byte\":%.3f,\"to_c\":%.1f,\"instructions\":%llu,\"instructions_per_byte\":%.1f}\n",
           count, bytes, c_requests, jit ? jit : "", batch ? batch : "", c_runs[3] / n, pure_runs[3] / n,
           pure_runs[3] / c_runs[3], (unsigned long long)work, (double)work / n);
    pure_app_stop(&app);
    return 0;
}
