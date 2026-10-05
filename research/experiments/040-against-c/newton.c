/* roots' Newton loop (natural.h's c_roots, i to 100,000) in the forms the
 * tier may make it, as AArch64 assembly (research 124, docs/design/023):
 *   moved    next apart from r, moved to r on the way back (the tier's)
 *   kept     next in r's register, the old r kept apart for the compare (gcc's)
 *   checked  moved, next checked finite as numbers (fabs, fcmp with infinity)
 *   int      moved, next checked finite in integers (its exponent's bits)
 *   sub      moved, next - next checked 0 (not a NaN)
 *   after    moved, the check after the compare
 * Each sums r and the steps (the answers must be equal), three rounds. */
#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdio.h>
#include <time.h>
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
/* Newton for each i: r = i; repeat next = (r + i/r) * 0.5; if next >= r break; r = next */
static double moved(uint32_t n) { /* r and next apart: fmov on the way back */
    double sum = 0; uint64_t steps = 0;
    for (uint32_t i = 1; i <= n; ++i) {
        double r = i, x = i, half = 0.5;
        uint64_t c = 0;
        __asm__ volatile(
            "1: fdiv d1, %d[x], %d[r]\n fadd d1, %d[r], d1\n fmul d2, d1, %d[half]\n fcmp d2, %d[r]\n b.ge 2f\n"
            "add %[c], %[c], #1\n fmov %d[r], d2\n cmp %[c], #64\n b.lo 1b\n 2:\n"
            : [r] "+&w"(r), [c] "+r"(c) : [x] "w"(x), [half] "w"(half) : "d1", "d2", "cc");
        sum += r, steps += c;
    }
    return sum + steps;
}
static double kept(uint32_t n) { /* next in r's register, the old r kept apart for the compare (as gcc) */
    double sum = 0; uint64_t steps = 0;
    for (uint32_t i = 1; i <= n; ++i) {
        double r = i, x = i, half = 0.5;
        uint64_t c = 0;
        __asm__ volatile(
            "1: fmov d3, %d[r]\n fdiv d1, %d[x], %d[r]\n fadd d1, %d[r], d1\n fmul %d[r], d1, %d[half]\n fcmp %d[r], d3\n b.ge 2f\n"
            "add %[c], %[c], #1\n cmp %[c], #64\n b.lo 1b\n 2: fmov %d[r], d3\n"
            : [r] "+&w"(r), [c] "+r"(c) : [x] "w"(x), [half] "w"(half) : "d1", "d2", "d3", "cc");
        sum += r, steps += c;
    }
    return sum + steps;
}
static double checked(uint32_t n) { /* moved, plus a finite check (fabs, fcmp, b.hs) on next */
    double sum = 0; uint64_t steps = 0;
    for (uint32_t i = 1; i <= n; ++i) {
        double r = i, x = i, half = 0.5, inf = 1.0 / 0.0;
        uint64_t c = 0;
        __asm__ volatile(
            "1: fdiv d1, %d[x], %d[r]\n fadd d1, %d[r], d1\n fmul d2, d1, %d[half]\n fabs d4, d2\n fcmp d4, %d[inf]\n b.hs 2f\n fcmp d2, %d[r]\n b.ge 2f\n"
            "add %[c], %[c], #1\n fmov %d[r], d2\n cmp %[c], #64\n b.lo 1b\n 2:\n"
            : [r] "+&w"(r), [c] "+r"(c) : [x] "w"(x), [half] "w"(half), [inf] "w"(inf) : "d1", "d2", "d4", "cc");
        sum += r, steps += c;
    }
    return sum + steps;
}

static double checked_int(uint32_t n) { /* the finite check in integers: the exponent's bits all ones */
    double sum = 0; uint64_t steps = 0;
    for (uint32_t i = 1; i <= n; ++i) {
        double r = i, x = i, half = 0.5;
        uint64_t c = 0;
        __asm__ volatile(
            "1: fdiv d1, %d[x], %d[r]\n fadd d1, %d[r], d1\n fmul d2, d1, %d[half]\n fmov x10, d2\n ubfx x10, x10, #52, #11\n cmp x10, #2047\n b.eq 2f\n fcmp d2, %d[r]\n b.ge 2f\n"
            "add %[c], %[c], #1\n fmov %d[r], d2\n cmp %[c], #64\n b.lo 1b\n 2:\n"
            : [r] "+&w"(r), [c] "+r"(c) : [x] "w"(x), [half] "w"(half) : "d1", "d2", "x10", "cc");
        sum += r, steps += c;
    }
    return sum + steps;
}
static double checked_sub(uint32_t n) { /* the finite check as next - next being 0 (not a NaN) */
    double sum = 0; uint64_t steps = 0;
    for (uint32_t i = 1; i <= n; ++i) {
        double r = i, x = i, half = 0.5;
        uint64_t c = 0;
        __asm__ volatile(
            "1: fdiv d1, %d[x], %d[r]\n fadd d1, %d[r], d1\n fmul d2, d1, %d[half]\n fsub d4, d2, d2\n fcmp d4, #0.0\n b.ne 2f\n fcmp d2, %d[r]\n b.ge 2f\n"
            "add %[c], %[c], #1\n fmov %d[r], d2\n cmp %[c], #64\n b.lo 1b\n 2:\n"
            : [r] "+&w"(r), [c] "+r"(c) : [x] "w"(x), [half] "w"(half) : "d1", "d2", "d4", "cc");
        sum += r, steps += c;
    }
    return sum + steps;
}
static double checked_after(uint32_t n) { /* the finite check after the compare (off the way to the next round) */
    double sum = 0; uint64_t steps = 0;
    for (uint32_t i = 1; i <= n; ++i) {
        double r = i, x = i, half = 0.5, inf = 1.0 / 0.0;
        uint64_t c = 0;
        __asm__ volatile(
            "1: fdiv d1, %d[x], %d[r]\n fadd d1, %d[r], d1\n fmul d2, d1, %d[half]\n fcmp d2, %d[r]\n b.ge 3f\n"
            "add %[c], %[c], #1\n fmov %d[r], d2\n fabs d4, d2\n fcmp d4, %d[inf]\n b.hs 2f\n cmp %[c], #64\n b.lo 1b\n b 2f\n 3: fabs d4, d2\n fcmp d4, %d[inf]\n 2:\n"
            : [r] "+&w"(r), [c] "+r"(c) : [x] "w"(x), [half] "w"(half), [inf] "w"(inf) : "d1", "d2", "d4", "cc");
        sum += r, steps += c;
    }
    return sum + steps;
}
int main(void) {
    for (int k = 0; k < 3; ++k) {
        double t0 = now(), a = moved(100000), t1 = now(), b = kept(100000), t2 = now(), c = checked(100000), t3 = now();
        double d = checked_int(100000), t4 = now(), e = checked_sub(100000), t5 = now(), f = checked_after(100000), t6 = now();
        printf("{\"moved\":%.2f,\"kept\":%.2f,\"checked\":%.2f,\"int\":%.2f,\"sub\":%.2f,\"after\":%.2f,\"same\":%d}\n", t1 - t0, t2 - t1, t3 - t2, t4 - t3, t5 - t4, t6 - t5, a == b && b == c && c == d && d == e && e == f); if (0) printf("moved %.2f kept %.2f checked %.2f int %.2f sub %.2f after %.2f ms (%d)\n", t1 - t0, t2 - t1, t3 - t2, t4 - t3, t5 - t4, t6 - t5, a == b && b == c && c == d && d == e && e == f);
    }
}
