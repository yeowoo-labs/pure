/* join as a batch executor could run it, on Pure's own forms: all (FLAT, ids as
 * u32 cells), byid (HASHED: key bits in a table of slots 4/5 full at most, the
 * region at its key's slot), totals (DENSE, 16 sums). Both loops a batch of
 * rounds at a time: the MINSTD recurrence in order (in C, or interpreted:
 * the executor's part), then each step of the round for every lane (the
 * setup: the appends, the adds in order; the orders: ids gathered, slots
 * probed, regions gathered, the sums added in order). Against natural.h's
 * c_join (its own hash table), both timed from nothing to the answer, the
 * sizes read at run time. */
#define _POSIX_C_SOURCE 200809L
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "natural.h"
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static const uint64_t VACANT = 0x7FF8000000000001ull;
static uint64_t key_bits(double k) { if (k == 0) k = 0; uint64_t b; memcpy(&b, &k, 8); return b; }
static size_t slot_of(uint64_t bits, size_t mask) { bits ^= bits >> 33; bits *= 0xff51afd7ed558ccdull; bits ^= bits >> 33; return bits & mask; }
enum { B = 256 };
/* A section that cannot run as a batch (each round needs the last's x) run
 * round by round by an interpreter of the same operations (computed goto,
 * Pure's checks): x = x * 48271 - floor(x * 48271 / 2147483647) * 2147483647,
 * each round's x into its lane. */
enum { S_MULK, S_FLOORDIVK, S_SUB, S_PUT, S_END };
typedef struct { uint8_t op, d, a, b; int64_t k; uint64_t magic; unsigned shift; } SOp; /* magic, shift: k's reciprocal, made with the program */
static SOp minstd_ops[] = {
    {S_MULK, 1, 0, 0, 48271},          /* t = x * 48271 */
    {S_FLOORDIVK, 2, 1, 0, 2147483647}, /* q = floor(t / 2147483647) */
    {S_MULK, 2, 2, 0, 2147483647},      /* q * 2147483647 */
    {S_SUB, 0, 1, 2, 0},                /* x = t - that */
    {S_PUT, 0, 0, 0, 0},                /* the lane's x */
    {S_END, 0, 0, 0, 0},
};
static void rounds_prepare(SOp *prog, bool magic) { /* a divisor's reciprocal, k not a power of two: q = (x m) >> (64 + s), 2^s < k, exact for x below 2^53 */
    for (; prog->op != S_END; ++prog)
        if (prog->op == S_FLOORDIVK && prog->k > 2 && (prog->k & (prog->k - 1)) && magic) {
            unsigned s = 0;
            while ((2ull << s) < (uint64_t)prog->k) ++s;
            prog->magic = (uint64_t)((((unsigned __int128)1 << (64 + s)) / (uint64_t)prog->k) + 1), prog->shift = s;
        } else prog->magic = 0;
}
__attribute__((noinline)) static bool rounds_run(const SOp *prog, int64_t *r, uint64_t *lanes, unsigned m) {
    static void *const at[] = {&&mulk, &&floordivk, &&sub, &&put, &&end};
    int64_t over = 0;
    for (unsigned l = 0; l < m; ++l) {
        const SOp *o = prog;
        goto *at[o->op];
    mulk: { int64_t x = r[o->a] * o->k; over |= (x >> 53) ^ (x >> 63); r[o->d] = x; ++o; goto *at[o->op]; }
    floordivk: {
        int64_t x = r[o->a], q;
        if (x >= 0 && o->magic) q = (int64_t)(((unsigned __int128)(uint64_t)x * o->magic) >> 64 >> o->shift); /* below 2^53 */
        else q = x / o->k, q -= (x % o->k != 0) & ((x < 0) != (o->k < 0));
        r[o->d] = q; ++o; goto *at[o->op];
    }
    sub: { int64_t x = r[o->a] - r[o->b]; over |= (x >> 53) ^ (x >> 63); r[o->d] = x; ++o; goto *at[o->op]; }
    put: lanes[l] = (uint64_t)r[o->a]; ++o; goto *at[o->op];
    end: ;
    }
    return over != 0;
}
static void pure_join(uint32_t customers, uint32_t orders, int interpreted, double *out) {
    rounds_prepare(minstd_ops, interpreted == 2);
    uint32_t *ids = malloc(4 * (size_t)customers);
    size_t cap = 16;
    while (5 * (size_t)customers > 4 * cap) cap *= 2;
    uint64_t *members = malloc(8 * cap);
    uint32_t *region = malloc(4 * cap);
    for (size_t i = 0; i < cap; ++i) members[i] = VACANT;
    uint64_t x = 1;
    int64_t totals[16] = {0};
    static uint64_t X[B], C[B], BITS[B], SLOT[B];
    static uint32_t R[B];
    bool bad = false;
    for (uint32_t i0 = 0; i0 < customers; i0 += B) { /* the setup: the recurrence, then the batch's appends, then its adds in order */
        unsigned m = customers - i0 < B ? customers - i0 : B;
        if (interpreted) {
            int64_t r[3] = {(int64_t)x};
            bad |= rounds_run(minstd_ops, r, X, m);
            x = (uint64_t)r[0];
        } else
            for (unsigned l = 0; l < m; ++l) x = x * 48271 % 2147483647, X[l] = x;
        for (unsigned l = 0; l < m; ++l) ids[i0 + l] = (uint32_t)X[l];
        for (unsigned l = 0; l < m; ++l) {
            uint64_t bits = key_bits((double)X[l]);
            size_t at = slot_of(bits, cap - 1);
            while (members[at] != VACANT) at = (at + 1) & (cap - 1);
            members[at] = bits, region[at] = (i0 + l) % 16;
        }
    }
    for (uint32_t o0 = 0; o0 < orders; o0 += B) {
        unsigned m = orders - o0 < B ? orders - o0 : B;
        if (interpreted) {                                                                 /* the recurrence, in order */
            int64_t r[3] = {(int64_t)x};
            bad |= rounds_run(minstd_ops, r, X, m);
            x = (uint64_t)r[0];
        } else
            for (unsigned l = 0; l < m; ++l) x = x * 48271 % 2147483647, X[l] = x;
        for (unsigned l = 0; l < m; ++l) C[l] = X[l] % customers;                          /* first + (x - floor(x / n) n) */
        for (unsigned l = 0; l < m; ++l) BITS[l] = key_bits((double)ids[C[l]]);           /* c.id, gathered */
        for (unsigned l = 0; l < m; ++l) {                                                 /* byid[c.id]: each lane's slot */
            size_t at = slot_of(BITS[l], cap - 1);
            while (members[at] != BITS[l]) {
                if (members[at] == VACANT) { bad = true; break; }
                at = (at + 1) & (cap - 1);
            }
            SLOT[l] = at;
        }
        for (unsigned l = 0; l < m; ++l) R[l] = region[SLOT[l]];                           /* .region, gathered */
        for (unsigned l = 0; l < m; ++l) totals[R[l]] += (int64_t)(X[l] % 1000);           /* the sums, in order */
    }
    int64_t most = 0, sum = 0;
    for (int r = 0; r < 16; ++r) most = totals[r] > most ? totals[r] : most, sum += totals[r];
    out[0] = bad ? -1 : (double)most, out[1] = (double)sum;
    free(ids), free(members), free(region);
}
static int cmpd(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return (x > y) - (x < y); }
static volatile uint32_t sizes[2] = {100000, 1000000}; /* read at run time */
static void (*volatile natural)(const uint32_t *, double *) = c_join;
int main(void) {
    double tc[11], tp[11], ti[11], tm[11], oc[2], op[2], oi[2], om[2];
    uint32_t in[2] = {sizes[0], sizes[1]};
    for (int r = 0; r < 11; ++r) {
        double t = now(); natural(in, oc); tc[r] = now() - t;
        t = now(); pure_join(in[0], in[1], 0, op); tp[r] = now() - t;
        t = now(); pure_join(in[0], in[1], 1, oi); ti[r] = now() - t;
        t = now(); pure_join(in[0], in[1], 2, om); tm[r] = now() - t;
    }
    qsort(tc, 11, 8, cmpd), qsort(tp, 11, 8, cmpd), qsort(ti, 11, 8, cmpd), qsort(tm, 11, 8, cmpd);
    printf("{\"work\":\"join\",\"c_ms\":%.2f,\"batch_ms\":%.2f,\"interpreted_ms\":%.2f,\"reciprocal_ms\":%.2f,\"same\":%s}\n", tc[5], tp[5], ti[5], tm[5],
           oc[0] == op[0] && oc[1] == op[1] && oc[0] == oi[0] && oc[1] == oi[1] && oc[0] == om[0] && oc[1] == om[1] ? "true" : "false");
}
