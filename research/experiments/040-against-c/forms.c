/* matmul's inner loop (200 x 200 cells of u32) in the forms the tier may
 * make it, as AArch64 assembly, against natural.h's C on the same Pi
 * (research 124, docs/design/023 section 5):
 *   index    the cell at column + (i * n + k) and column + (k * n + j), each
 *            found by a multiply and an add; the product and the sum each
 *            checked below 2^53; a full list's bit tested before each load
 *            (the tier's code at e4b4694)
 *   plain    index, no checks, no bit tested, multiply-add into the sum
 *   pointer  a pointer to each column stepped by its stride, multiply-add
 *   apart    pointer, the multiply and the add apart (the sum's chain an add)
 *   checks   apart, the product and the sum checked below 2^53
 *   bits     apart, a full list's bit tested before each load
 * Each sums every product (the answers must be equal); the median of 11. */
#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
enum { N = 200 };
static uint32_t A[N * N], B[N * N];
static double DA[N * N], DB[N * N];
static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}
#define LOOP(name, body, ...)                                                                                          \
    static uint64_t name(void) {                                                                                       \
        uint64_t total = 0, flag = 1;                                                                                  \
        for (uint64_t i = 0; i < N; ++i)                                                                               \
            for (uint64_t j = 0; j < N; ++j) {                                                                         \
                uint64_t s = 0, k = 0, base = i * N;                                                                   \
                const uint32_t *pa = A + i * N, *pb = B + j;                                                           \
                __asm__ volatile(body : [s] "+r"(s), [k] "+r"(k), [pa] "+r"(pa), [pb] "+r"(pb)                         \
                                 : [base] "r"(base), [a] "r"(A), [b] "r"(B), [n] "r"((uint64_t)N), [j] "r"(j),           \
                                   [flag] "r"(flag)                                                                    \
                                 : "x10", "x20", "x24", "x27", "cc", "memory");                                        \
                total += s;                                                                                            \
            }                                                                                                          \
        return total;                                                                                                  \
    }
LOOP(index, "1: add x20, %[base], %[k]\n tbz %w[flag], #0, 9f\n ldr w24, [%[a], x20, lsl #2]\n mul x20, %[k], %[n]\n"
            " add x20, x20, %[j]\n tbz %w[flag], #0, 9f\n ldr w27, [%[b], x20, lsl #2]\n mul x20, x24, x27\n"
            " lsr x10, x20, #53\n cbnz x10, 9f\n add %[s], %[s], x20\n lsr x10, %[s], #53\n cbnz x10, 9f\n"
            " add %[k], %[k], #1\n cmp %[k], %[n]\n b.lo 1b\n 9:\n")
LOOP(plain, "1: add x20, %[base], %[k]\n ldr w24, [%[a], x20, lsl #2]\n mul x20, %[k], %[n]\n add x20, x20, %[j]\n"
            " ldr w27, [%[b], x20, lsl #2]\n madd %[s], x24, x27, %[s]\n add %[k], %[k], #1\n cmp %[k], %[n]\n b.lo 1b\n")
LOOP(pointer, "1: ldr w24, [%[pa]], #4\n ldr w27, [%[pb]]\n add %[pb], %[pb], #800\n madd %[s], x24, x27, %[s]\n"
              " add %[k], %[k], #1\n cmp %[k], %[n]\n b.lo 1b\n")
LOOP(apart, "1: ldr w24, [%[pa]], #4\n ldr w27, [%[pb]]\n add %[pb], %[pb], #800\n mul x20, x24, x27\n add %[s], %[s], x20\n"
            " add %[k], %[k], #1\n cmp %[k], %[n]\n b.lo 1b\n")
LOOP(checks, "1: ldr w24, [%[pa]], #4\n ldr w27, [%[pb]]\n add %[pb], %[pb], #800\n mul x20, x24, x27\n lsr x10, x20, #53\n"
             " cbnz x10, 9f\n add %[s], %[s], x20\n lsr x10, %[s], #53\n cbnz x10, 9f\n add %[k], %[k], #1\n"
             " cmp %[k], %[n]\n b.lo 1b\n 9:\n")
LOOP(bits, "1: tbz %w[flag], #0, 9f\n ldr w24, [%[pa]], #4\n tbz %w[flag], #0, 9f\n ldr w27, [%[pb]]\n add %[pb], %[pb], #800\n"
           " mul x20, x24, x27\n add %[s], %[s], x20\n add %[k], %[k], #1\n cmp %[k], %[n]\n b.lo 1b\n 9:\n")
static uint64_t c(void) { /* natural.h's c_matmul, its sum */
    double total = 0;
    for (uint32_t i = 0; i < N; ++i)
        for (uint32_t j = 0; j < N; ++j) {
            double s = 0;
            for (uint32_t k = 0; k < N; ++k) s += DA[i * N + k] * DB[k * N + j];
            total += s;
        }
    return (uint64_t)total;
}
static int by(const void *a, const void *b) { return (*(const double *)a > *(const double *)b) - (*(const double *)a < *(const double *)b); }
int main(void) {
    uint64_t x = 1;
    for (int i = 0; i < N * N; ++i) {
        x = x * 48271 % 2147483647, A[i] = (uint32_t)(x % 100), DA[i] = A[i];
        x = x * 48271 % 2147483647, B[i] = (uint32_t)(x % 100), DB[i] = B[i];
    }
    static uint64_t (*const loops[])(void) = {index, plain, pointer, apart, checks, bits, c};
    static const char *const names[] = {"index", "plain", "pointer", "apart", "checks", "bits", "c"};
    double ms[7][11];
    uint64_t answer[7];
    for (int r = 0; r < 11; ++r)
        for (int l = 0; l < 7; ++l) {
            double t = now();
            answer[l] = loops[l]();
            ms[l][r] = now() - t;
        }
    printf("{");
    for (int l = 0; l < 7; ++l) {
        qsort(ms[l], 11, sizeof(double), by);
        printf("%s\"%s\":%.2f", l ? "," : "", names[l], ms[l][5]);
    }
    int same = 1;
    for (int l = 1; l < 7; ++l) same = same && answer[l] == answer[0];
    printf(",\"same\":%s}\n", same ? "true" : "false");
}
