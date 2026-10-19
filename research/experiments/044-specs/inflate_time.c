/* Research 140: inflate (RFC 1951) in aup (aup's examples/inflate.aup, run by Pure) against the same algorithm in C
 * (codes read a bit at a time from canonical counts, as zlib's puff.c) and against zlib's inflate (tables), on the
 * same raw deflate stream. Each output must be the original's bytes; then each is timed (the median of seven runs of
 * rounds inflations).
 *
 *   cc -O2 -std=c17 -I../../../core inflate_time.c ../../../build/libpure.a -lz -lm -pthread -o inflate_time
 *   ./inflate_time inflate.pmod STREAM ORIGINAL ROUNDS     (PURE_JIT and PURE_BATCH choose Pure's path)
 *
 * One JSON line: the sizes, the nanoseconds per output byte of each, Pure's ratios to both and the instructions Pure
 * counted for one stream. */
#define _POSIX_C_SOURCE 200809L
#include "module.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <zlib.h>

/* ---- inflate in C, bit by bit (puff's algorithm, no tables) */
typedef struct {
    const unsigned char *in;
    size_t size, at; /* at in bits */
    unsigned char *out;
    size_t used, room;
} Stream;
typedef struct { short counts[16], symbols[288]; } Code;

static int take(Stream *s, int n) {
    int v = 0;
    for (int k = 0; k < n; ++k, ++s->at) {
        if (s->at >> 3 >= s->size) return -1;
        v |= (s->in[s->at >> 3] >> (s->at & 7) & 1) << k;
    }
    return v;
}

static void code(Code *c, const short *lengths, int n) {
    memset(c->counts, 0, sizeof(c->counts));
    for (int s = 0; s < n; ++s) c->counts[lengths[s]]++;
    short offsets[16] = {0};
    for (int l = 1; l < 15; ++l) offsets[l + 1] = (short)(offsets[l] + c->counts[l]);
    for (int s = 0; s < n; ++s)
        if (lengths[s]) c->symbols[offsets[lengths[s]]++] = (short)s;
}

static int decode(Stream *s, const Code *c) {
    int value = 0, first = 0, index = 0;
    for (int n = 1; n < 16; ++n) {
        int b = take(s, 1);
        if (b < 0) return -1;
        value |= b;
        if (value - first < c->counts[n]) return c->symbols[index + value - first];
        index += c->counts[n];
        first = (first + c->counts[n]) << 1;
        value <<= 1;
    }
    return -1;
}

static const short length_base[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83,
                                       99, 115, 131, 163, 195, 227, 258};
static const short length_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const short distance_base[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769,
                                        1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const short distance_extra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11,
                                         12, 12, 13, 13};

static int put(Stream *s, unsigned char b) {
    if (s->used == s->room) return -1;
    s->out[s->used++] = b;
    return 0;
}

/* The bytes of a raw deflate stream; -1 if it is none (or the room is short). */
static long puff(const unsigned char *in, size_t size, unsigned char *out, size_t room) {
    Stream s = {in, size, 0, out, 0, room};
    static const short order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    int last;
    do {
        last = take(&s, 1);
        int kind = take(&s, 2);
        if (last < 0 || kind < 0 || kind == 3) return -1;
        if (kind == 0) {
            size_t b = (s.at + 7) >> 3;
            if (b + 4 > size) return -1;
            size_t n = in[b] | in[b + 1] << 8;
            if (n != ((in[b + 2] | in[b + 3] << 8) ^ 0xffffu) || b + 4 + n > size || s.used + n > room) return -1;
            memcpy(out + s.used, in + b + 4, n);
            s.used += n;
            s.at = (b + 4 + n) * 8;
            continue;
        }
        Code lc, dc;
        short lengths[320];
        if (kind == 1) {
            for (int i = 0; i < 288; ++i) lengths[i] = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
            code(&lc, lengths, 288);
            for (int i = 0; i < 30; ++i) lengths[i] = 5;
            code(&dc, lengths, 30);
        } else {
            int literals = take(&s, 5) + 257, count = take(&s, 5) + 1, n = take(&s, 4) + 4;
            short small[19] = {0};
            for (int k = 0; k < n; ++k) small[order[k]] = (short)take(&s, 3);
            Code c;
            code(&c, small, 19);
            int got = 0;
            while (got < literals + count) {
                int symbol = decode(&s, &c);
                if (symbol < 0) return -1;
                if (symbol < 16) { lengths[got++] = (short)symbol; continue; }
                short repeat = 0;
                int times;
                if (symbol == 16) {
                    if (!got) return -1;
                    repeat = lengths[got - 1], times = 3 + take(&s, 2);
                } else times = symbol == 17 ? 3 + take(&s, 3) : 11 + take(&s, 7);
                if (got + times > literals + count) return -1;
                while (times--) lengths[got++] = repeat;
            }
            code(&lc, lengths, literals);
            code(&dc, lengths + literals, count);
        }
        for (;;) {
            int symbol = decode(&s, &lc);
            if (symbol < 0) return -1;
            if (symbol == 256) break;
            if (symbol < 256) { if (put(&s, (unsigned char)symbol)) return -1; continue; }
            symbol -= 257;
            if (symbol >= 29) return -1;
            int length = length_base[symbol] + take(&s, length_extra[symbol]);
            symbol = decode(&s, &dc);
            if (symbol < 0 || symbol >= 30) return -1;
            size_t distance = (size_t)(distance_base[symbol] + take(&s, distance_extra[symbol]));
            if (distance > s.used) return -1;
            while (length--) if (put(&s, s.out[s.used - distance])) return -1;
        }
    } while (!last);
    return (long)s.used;
}

static long zlib_inflate(const unsigned char *in, size_t size, unsigned char *out, size_t room) {
    z_stream z = {0};
    if (inflateInit2(&z, -15) != Z_OK) return -1;
    z.next_in = (unsigned char *)in, z.avail_in = (uInt)size, z.next_out = out, z.avail_out = (uInt)room;
    int r = inflate(&z, Z_FINISH);
    long used = (long)z.total_out;
    inflateEnd(&z);
    return r == Z_STREAM_END ? used : -1;
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

int main(int argc, char **argv) {
    if (argc != 5) { fprintf(stderr, "usage: %s MODULE STREAM ORIGINAL ROUNDS\n", argv[0]); return 2; }
    size_t size, original_size;
    unsigned char *stream = whole(argv[2], &size), *original = whole(argv[3], &original_size);
    int rounds = atoi(argv[4]);
    if (!stream || !original) { fprintf(stderr, "cannot read the stream or the original\n"); return 1; }
    unsigned char *out = malloc(original_size + 1);
    if (puff(stream, size, out, original_size) != (long)original_size || memcmp(out, original, original_size)) {
        fprintf(stderr, "C's bytes differ\n");
        return 1;
    }
    if (zlib_inflate(stream, size, out, original_size + 1) != (long)original_size || memcmp(out, original, original_size)) {
        fprintf(stderr, "zlib's bytes differ\n");
        return 1;
    }

    PureModule module = {0};
    char error[160];
    if (pure_module_read(argv[1], &module, error, sizeof(error)) != PURE_OK) { fprintf(stderr, "%s\n", error); return 1; }
    PureApp app;
    if (pure_app_start(&app, &module, (PureData){0}) != PURE_OK) return 1;
    int input = pure_module_input(&module, "inflate"), state = pure_module_state(&module, "inflated");
    uint64_t before = app.work;
    if (pure_app_message(&app, (uint32_t)input, (PureSpan){stream, size}) != PURE_OK) {
        fprintf(stderr, "event failed\n");
        return 1;
    }
    uint64_t work = app.work - before;
    const PureList *list = app.lists[state];
    PureCell cell = pure_list_count(list) == 1 ? pure_list_get(list, 0, 0) : (PureCell){0};
    if (cell.size != original_size || memcmp(cell.text, original, original_size)) { fprintf(stderr, "Pure's bytes differ\n"); return 1; }

    double c_runs[7], zlib_runs[7], pure_runs[7];
    volatile unsigned char sink = 0;
    for (int r = 0; r < 7; ++r) {
        double start = now_ns();
        for (int i = 0; i < rounds; ++i) { puff(stream, size, out, original_size); sink ^= out[i % (original_size + 1)]; }
        c_runs[r] = (now_ns() - start) / rounds;
        start = now_ns();
        for (int i = 0; i < rounds; ++i) { zlib_inflate(stream, size, out, original_size + 1); sink ^= out[i % (original_size + 1)]; }
        zlib_runs[r] = (now_ns() - start) / rounds;
        start = now_ns();
        for (int i = 0; i < rounds; ++i)
            if (pure_app_message(&app, (uint32_t)input, (PureSpan){stream, size}) != PURE_OK) return 1;
        pure_runs[r] = (now_ns() - start) / rounds;
    }
    qsort(c_runs, 7, sizeof(double), by_value);
    qsort(zlib_runs, 7, sizeof(double), by_value);
    qsort(pure_runs, 7, sizeof(double), by_value);
    double n = (double)original_size;
    const char *jit = getenv("PURE_JIT"), *batch = getenv("PURE_BATCH");
    printf("{\"size\":%zu,\"stream\":%zu,\"jit\":\"%s\",\"batch\":\"%s\",\"c_ns_per_byte\":%.3f,\"zlib_ns_per_byte\":%.3f,"
           "\"pure_ns_per_byte\":%.3f,\"to_c\":%.1f,\"to_zlib\":%.1f,\"instructions\":%llu,\"instructions_per_byte\":%.1f}\n",
           original_size, size, jit ? jit : "", batch ? batch : "", c_runs[3] / n, zlib_runs[3] / n, pure_runs[3] / n,
           pure_runs[3] / c_runs[3], pure_runs[3] / zlib_runs[3], (unsigned long long)work, (double)work / n);
    pure_app_stop(&app);
    return 0;
}
