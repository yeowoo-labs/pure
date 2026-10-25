/* Research 049: what SHA-256 costs a block in machine instructions and cycles, Pure's path as built (the executor, or
 * a block emitted by emit.py when built with it) and the same algorithm in C (044's time.c, FIPS 180-4), user-mode
 * counters (perf_event_open) around each. First the answers: digests of messages of many sizes against C's, then
 * events cut short by a small work budget (the fault, where, and the digest left as it was), printed for the builds
 * to be compared.
 *
 *   cc -O2 -std=c17 -I../../../core sha-cost.c ../../../build/libpure.a -lm -pthread -o sha-cost
 *   ./sha-cost sha256.pmod SIZE ROUNDS
 * One JSON line. */
#define _GNU_SOURCE
#include "module.h"
#include <linux/perf_event.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>

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

typedef struct { char text[65]; size_t used; } Hex;
static bool gather(void *context, PureSpan span) {
    Hex *h = context;
    for (size_t i = 0; i < span.size && h->used < 64; ++i) h->text[h->used++] = (char)span.data[i];
    return true;
}
static void digest_of(PureApp *app, Hex *got) {
    memset(got, 0, sizeof(*got));
    pure_draft_read(app->texts[0], 0, pure_draft_size(app->texts[0]), gather, got);
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

static unsigned char *bytes_of(size_t size, uint32_t seed) {
    unsigned char *data = malloc(size ? size : 1);
    for (size_t i = 0; i < size; ++i) seed = seed * 1664525u + 1013904223u, data[i] = (unsigned char)(seed >> 24);
    return data;
}

int main(int argc, char **argv) {
    if (argc != 4) { fprintf(stderr, "usage: %s MODULE SIZE ROUNDS\n", argv[0]); return 2; }
    size_t size = strtoul(argv[2], NULL, 10);
    int rounds = atoi(argv[3]);
    PureModule module = {0};
    char error[160];
    if (pure_module_read(argv[1], &module, error, sizeof(error)) != PURE_OK) { fprintf(stderr, "%s\n", error); return 1; }
    PureApp app;
    if (pure_app_start(&app, &module, (PureData){0}) != PURE_OK) return 1;
    uint32_t hash = (uint32_t)pure_module_input(&module, "hash");
    /* The answers: every size up to 300, then some larger ones. */
    static const size_t larger[] = {511, 512, 1000, 4096, 65535, 65536};
    unsigned checked = 0;
    for (size_t k = 0; k < 301 + sizeof(larger) / sizeof(*larger); ++k) {
        size_t n = k < 301 ? k : larger[k - 301];
        unsigned char *data = bytes_of(n, (uint32_t)(49 + n));
        char want[65];
        sha256(data, n, want);
        Hex got;
        if (pure_app_message(&app, hash, (PureSpan){data, n}) != PURE_OK) { fprintf(stderr, "event failed at %zu\n", n); return 1; }
        digest_of(&app, &got);
        if (strcmp(got.text, want)) { fprintf(stderr, "digests differ at %zu: %s %s\n", n, got.text, want); return 1; }
        free(data);
        ++checked;
    }
    /* Cut short: the outcome of an event a small work budget stops, and the digest after (as before it). */
    printf("{\"checked\":%u,\"cut\":[", checked);
    unsigned char *some = bytes_of(4096, 7);
    static const uint64_t budgets[] = {100, 1000, 5000, 20000, 60000, 200000};
    for (size_t b = 0; b < sizeof(budgets) / sizeof(*budgets); ++b) {
        pure_app_work(&app, budgets[b]);
        PureResult r = pure_app_message(&app, hash, (PureSpan){some, 4096});
        Hex got;
        digest_of(&app, &got);
        printf("%s[%llu,%d,%d,%u,\"%.8s\"]", b ? "," : "", (unsigned long long)budgets[b], (int)r, (int)app.fault, app.fault_pc, got.text);
    }
    pure_app_work(&app, 0);
    free(some);
    /* The cost: ROUNDS digests of SIZE bytes, Pure's and C's, counted. */
    unsigned char *data = bytes_of(size, 140);
    int ins = counter(PERF_COUNT_HW_INSTRUCTIONS), cyc = counter(PERF_COUNT_HW_CPU_CYCLES);
    if (ins < 0 || cyc < 0) { fprintf(stderr, "no counters\n"); return 1; }
    double blocks = (double)(size / 64 + (size % 64 + 9 <= 64 ? 1 : 2)) * rounds;
    ioctl(ins, PERF_EVENT_IOC_RESET, 0), ioctl(cyc, PERF_EVENT_IOC_RESET, 0);
    ioctl(ins, PERF_EVENT_IOC_ENABLE, 0), ioctl(cyc, PERF_EVENT_IOC_ENABLE, 0);
    for (int i = 0; i < rounds; ++i)
        if (pure_app_message(&app, hash, (PureSpan){data, size}) != PURE_OK) return 1;
    ioctl(ins, PERF_EVENT_IOC_DISABLE, 0), ioctl(cyc, PERF_EVENT_IOC_DISABLE, 0);
    uint64_t pure_ins = read_counter(ins), pure_cyc = read_counter(cyc);
    volatile char sink = 0;
    ioctl(ins, PERF_EVENT_IOC_RESET, 0), ioctl(cyc, PERF_EVENT_IOC_RESET, 0);
    ioctl(ins, PERF_EVENT_IOC_ENABLE, 0), ioctl(cyc, PERF_EVENT_IOC_ENABLE, 0);
    for (int i = 0; i < rounds; ++i) { char hex[65]; sha256(data, size, hex); sink ^= hex[i % 64]; }
    ioctl(ins, PERF_EVENT_IOC_DISABLE, 0), ioctl(cyc, PERF_EVENT_IOC_DISABLE, 0);
    uint64_t c_ins = read_counter(ins), c_cyc = read_counter(cyc);
    printf("],\"size\":%zu,\"rounds\":%d,\"pure_instructions_per_block\":%.1f,\"pure_cycles_per_block\":%.1f,"
           "\"c_instructions_per_block\":%.1f,\"c_cycles_per_block\":%.1f}\n",
           size, rounds, pure_ins / blocks, pure_cyc / blocks, c_ins / blocks, c_cyc / blocks);
    pure_app_stop(&app);
    return 0;
}
