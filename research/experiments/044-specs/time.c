/* Research 140, design 027 stage 1: SHA-256 in aup (aup's examples/sha256.aup, run by Pure)
 * against the same algorithm in C, on the same bytes. The digests must be the
 * same; then each is timed (the median of seven runs of rounds digests).
 *
 *   cc -O2 -std=c17 -I../../../core time.c ../../../build/libpure.a -lm -pthread -o time
 *   ./time sha256.pmod SIZE ROUNDS     (PURE_JIT and PURE_BATCH choose Pure's path)
 *
 * One JSON line: the size, the C and Pure nanoseconds per byte, their ratio and
 * the instructions Pure counted for one digest. */
#define _POSIX_C_SOURCE 200809L
#include "module.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- SHA-256 in C, as FIPS 180-4 writes it (no SHA instructions: the same algorithm) */
static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

static uint32_t rotr(uint32_t x, unsigned n) { return x >> n | x << (32 - n); }

static void block(uint32_t h[8], const unsigned char *p) {
    uint32_t w[64];
    for (int t = 0; t < 16; ++t) w[t] = (uint32_t)p[4 * t] << 24 | (uint32_t)p[4 * t + 1] << 16 | (uint32_t)p[4 * t + 2] << 8 | p[4 * t + 3];
    for (int t = 16; t < 64; ++t) {
        uint32_t s0 = rotr(w[t - 15], 7) ^ rotr(w[t - 15], 18) ^ w[t - 15] >> 3;
        uint32_t s1 = rotr(w[t - 2], 17) ^ rotr(w[t - 2], 19) ^ w[t - 2] >> 10;
        w[t] = w[t - 16] + s0 + w[t - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int t = 0; t < 64; ++t) {
        uint32_t t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + K[t] + w[t];
        uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        hh = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
    }
    h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e, h[5] += f, h[6] += g, h[7] += hh;
}

static void sha256(const unsigned char *data, size_t size, char hex[65]) {
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    size_t whole = size / 64 * 64;
    for (size_t at = 0; at < whole; at += 64) block(h, data + at);
    unsigned char last[128] = {0};
    size_t left = size - whole, tail = left + 9 <= 64 ? 64 : 128;
    memcpy(last, data + whole, left);
    last[left] = 0x80;
    for (int i = 0; i < 8; ++i) last[tail - 1 - i] = (unsigned char)((uint64_t)size * 8 >> (8 * i));
    for (size_t at = 0; at < tail; at += 64) block(h, last + at);
    for (int i = 0; i < 8; ++i) snprintf(hex + 8 * i, 9, "%08x", h[i]);
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

typedef struct { char text[65]; size_t used; } Hex;
static bool gather(void *context, PureSpan span) {
    Hex *h = context;
    for (size_t i = 0; i < span.size && h->used < 64; ++i) h->text[h->used++] = (char)span.data[i];
    return true;
}

int main(int argc, char **argv) {
    if (argc != 4) { fprintf(stderr, "usage: %s MODULE SIZE ROUNDS\n", argv[0]); return 2; }
    size_t size = strtoul(argv[2], NULL, 10);
    int rounds = atoi(argv[3]);
    unsigned char *data = malloc(size ? size : 1);
    uint32_t seed = 140;
    for (size_t i = 0; i < size; ++i) seed = seed * 1664525u + 1013904223u, data[i] = (unsigned char)(seed >> 24);

    PureModule module = {0};
    char error[160];
    if (pure_module_read(argv[1], &module, error, sizeof(error)) != PURE_OK) { fprintf(stderr, "%s\n", error); return 1; }
    PureApp app;
    if (pure_app_start(&app, &module, (PureData){0}) != PURE_OK) return 1;
    int hash = pure_module_input(&module, "hash");
    char want[65];
    sha256(data, size, want);
    if (pure_app_message(&app, (uint32_t)hash, NULL, -1, (PureSpan){data, size}) != PURE_OK) { fprintf(stderr, "event failed\n"); return 1; }
    uint64_t work = app.work;
    Hex got = {{0}, 0};
    pure_draft_read(app.texts[0], 0, pure_draft_size(app.texts[0]), gather, &got);
    if (strcmp(got.text, want)) { fprintf(stderr, "digests differ: %s %s\n", got.text, want); return 1; }

    double c_runs[7], pure_runs[7];
    volatile char sink = 0;
    for (int r = 0; r < 7; ++r) {
        double start = now_ns();
        for (int i = 0; i < rounds; ++i) { char hex[65]; sha256(data, size, hex); sink ^= hex[i % 64]; }
        c_runs[r] = (now_ns() - start) / rounds;
        start = now_ns();
        for (int i = 0; i < rounds; ++i)
            if (pure_app_message(&app, (uint32_t)hash, NULL, -1, (PureSpan){data, size}) != PURE_OK) return 1;
        pure_runs[r] = (now_ns() - start) / rounds;
    }
    qsort(c_runs, 7, sizeof(double), by_value);
    qsort(pure_runs, 7, sizeof(double), by_value);
    const char *jit = getenv("PURE_JIT"), *batch = getenv("PURE_BATCH");
    printf("{\"size\":%zu,\"jit\":\"%s\",\"batch\":\"%s\",\"c_ns_per_byte\":%.3f,\"pure_ns_per_byte\":%.3f,\"ratio\":%.1f,"
           "\"instructions\":%llu,\"instructions_per_block\":%.0f}\n",
           size, jit ? jit : "", batch ? batch : "", c_runs[3] / (double)size, pure_runs[3] / (double)size,
           pure_runs[3] / c_runs[3], (unsigned long long)work, (double)work / (double)(size / 64 + 1));
    pure_app_stop(&app);
    return 0;
}
