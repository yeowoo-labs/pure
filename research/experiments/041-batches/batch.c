/* What a batch executor could do (docs/design/024): a loop's body as a short
 * program of operations over a batch of rounds (a value one per round, a
 * lane), each operation a C loop the compiler makes at build time (no code
 * made at run time), one operation interpreted per batch; Pure's checks
 * (whole numbers below 2^53, finite numbers, cells fitting their columns)
 * done over the batch, a flag for it (a failed batch runs again round by
 * round in the executor). Against natural.h's C on the same inputs, called
 * where its sizes are not constants (c_natural: it makes its cells too), and
 * against C given what the batches are given, the cells made before (c: the
 * natural loop; c_ikj: the loops over j and k swapped; u32: Pure's four-byte
 * cells; c_ikj_vec, c_ikj_dvec: those, and the numbers' eight bytes, at -O3,
 * made vector; c_vec: the natural knapsack at -O3 with ivdep, which gcc
 * leaves scalar; c_block: knapsack written as its batch runs it).
 *   matmul   batch_k: the inner loop's rounds a batch (strided reads of b);
 *            batch_j: the loops over j and k swapped (each sum still in k's
 *            order: the same numbers), a row of b a batch; axpy: that with
 *            the multiply and the add as one operation
 *   knapsack each item's rounds a batch, lanes of 8 bytes; u32: lanes of
 *            the column's 4 bytes, the add, max and store one operation
 *   roots    a batch of i, each lane's Newton rounds while it goes on
 * Built cc -O2 -std=c17 -I../040-against-c batch.c -lm (the batch operations
 * at -O3: their loops made vector), as join.c and bfs.c; their runs in
 * 2026-10-05-batches.jsonl, three of each (each the median of 11 turns). */
#define _POSIX_C_SOURCE 200809L
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "natural.h"

static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

enum { B = 256, REGS = 16 };
/* The operations: d = a op b over n lanes; ints in i[], numbers in f[]. */
enum {
    O_IOTA,    /* i[d] = k + lane * k2 */
    O_ADDK,    /* i[d] = i[a] + k */
    O_MULK,    /* i[d] = i[a] * k (checked below 2^53) */
    O_SUBK_R,  /* i[d] = k - i[a] */
    O_ADD,     /* i[d] = i[a] + i[b] (checked) */
    O_MUL,     /* i[d] = i[a] * i[b] (checked) */
    O_MAX,     /* i[d] = max(i[a], i[b]) */
    O_LOAD32,  /* i[d] = col[i[a]] (u32 cells) */
    O_STORE32, /* col[i[a]] = i[b] (fits 32 bits, checked) */
    O_SUM,     /* acc += sum of i[a] (checked) */
    O_FROMI,   /* f[d] = i[a] */
    O_FDIV,    /* f[d] = f[a] / f[b] (b not 0, finite) */
    O_FADD,    /* f[d] = f[a] + f[b] (finite) */
    O_FMULK,   /* f[d] = f[a] * k (finite) */
};
typedef struct { uint8_t op, d, a, b; int64_t k, k2; uint32_t *col; } Op;
typedef struct {
    int64_t i[REGS][B];
    double f[REGS][B];
    int64_t acc;
    bool bad; /* a check failed in the batch: the executor runs it again round by round */
} Lanes;

/* Each operation over n lanes; over collects bits whose any set means a check failed (a whole number past 2^53). */
__attribute__((optimize("O3"))) static void op_add(int64_t *restrict d, const int64_t *restrict a, const int64_t *restrict b, unsigned n, int64_t *over) {
    int64_t o = 0;
    for (unsigned l = 0; l < n; ++l) { int64_t x = a[l] + b[l]; d[l] = x; o |= (x >> 53) ^ (x >> 63); }
    *over |= o;
}
__attribute__((optimize("O3"))) static void op_addto(int64_t *restrict d, const int64_t *restrict b, unsigned n, int64_t *over) {
    int64_t o = 0;
    for (unsigned l = 0; l < n; ++l) { int64_t x = d[l] + b[l]; d[l] = x; o |= (x >> 53) ^ (x >> 63); }
    *over |= o;
}
__attribute__((optimize("O3"))) static void op_mul(int64_t *restrict d, const int64_t *restrict a, const int64_t *restrict b, unsigned n, int64_t *over) {
    int64_t o = 0;
    for (unsigned l = 0; l < n; ++l) { int64_t x = a[l] * b[l]; d[l] = x; o |= (x >> 53) ^ (x >> 63); }
    *over |= o;
}
__attribute__((optimize("O3"))) static void op_mulk(int64_t *restrict d, const int64_t *restrict a, int64_t k, unsigned n, int64_t *over) {
    int64_t o = 0;
    for (unsigned l = 0; l < n; ++l) { int64_t x = a[l] * k; d[l] = x; o |= (x >> 53) ^ (x >> 63); }
    *over |= o;
}
__attribute__((optimize("O3"))) static void op_addk(int64_t *restrict d, const int64_t *restrict a, int64_t k, unsigned n) { for (unsigned l = 0; l < n; ++l) d[l] = a[l] + k; }
__attribute__((optimize("O3"))) static void op_max(int64_t *restrict d, const int64_t *restrict a, const int64_t *restrict b, unsigned n) {
    for (unsigned l = 0; l < n; ++l) d[l] = a[l] > b[l] ? a[l] : b[l];
}
/* A column's cells at an affine position base + lane * stride (as a key affine in the loop's round is): strided reads */
__attribute__((optimize("O3"))) static void op_loads(int64_t *restrict d, const uint32_t *restrict col, int64_t base, int64_t stride, unsigned n) {
    if (stride == 1) for (unsigned l = 0; l < n; ++l) d[l] = col[base + l];
    else if (stride == -1) for (unsigned l = 0; l < n; ++l) d[l] = col[base - (int64_t)l];
    else for (unsigned l = 0; l < n; ++l) d[l] = col[base + (int64_t)l * stride];
}
__attribute__((optimize("O3"))) static void op_stores(uint32_t *restrict col, int64_t base, int64_t stride, const int64_t *restrict v, unsigned n, int64_t *over) {
    int64_t o = 0;
    for (unsigned l = 0; l < n; ++l) o |= v[l] >> 32;
    *over |= o;
    if (o) return;
    if (stride == -1) for (unsigned l = 0; l < n; ++l) col[base - (int64_t)l] = (uint32_t)v[l];
    else for (unsigned l = 0; l < n; ++l) col[base + (int64_t)l * stride] = (uint32_t)v[l];
}
__attribute__((optimize("O3"))) static int64_t op_sum(const int64_t *restrict a, unsigned n) { int64_t s = 0; for (unsigned l = 0; l < n; ++l) s += a[l]; return s; }

enum { P_LOADS, P_ADDK, P_MUL, P_MULK, P_ADD, P_MAX, P_STORES, P_SUM };
typedef struct { uint8_t op, d, a, b; int64_t k, base, stride; uint32_t *col; } POp;
static void run(const POp *prog, unsigned ops, Lanes *L, unsigned n) {
    int64_t over = 0;
    for (unsigned p = 0; p < ops; ++p) {
        const POp *o = &prog[p];
        switch (o->op) {
        case P_LOADS: op_loads(L->i[o->d], o->col, o->base, o->stride, n); break;
        case P_ADDK: op_addk(L->i[o->d], L->i[o->a], o->k, n); break;
        case P_MUL: op_mul(L->i[o->d], L->i[o->a], L->i[o->b], n, &over); break;
        case P_MULK: op_mulk(L->i[o->d], L->i[o->a], o->k, n, &over); break;
        case P_ADD: op_add(L->i[o->d], L->i[o->a], L->i[o->b], n, &over); break;
        case P_MAX: op_max(L->i[o->d], L->i[o->a], L->i[o->b], n); break;
        case P_STORES: op_stores(o->col, o->base, o->stride, L->i[o->a], n, &over); break;
        case P_SUM: L->acc += op_sum(L->i[o->a], n); over |= (L->acc >> 53) ^ (L->acc >> 63); break;
        }
    }
    L->bad |= over != 0;
}

static uint64_t minstd_x;
static uint32_t minstd_next(void) { return (uint32_t)(minstd_x = minstd_x * 48271 % 2147483647); }

/* matmul, its inner loop (over k) a batch: s = sum of a[i n + k] * b[k n + j] (affine reads) */
static double matmul_k(uint32_t n, uint32_t *a, uint32_t *b, double *last) {
    static Lanes L;
    double total = 0;
    POp prog[] = {
        {P_LOADS, 2, 0, 0, 0, 0, 1, a},
        {P_LOADS, 4, 0, 0, 0, 0, 0, b},
        {P_MUL, 5, 2, 4, 0, 0, 0, NULL},
        {P_SUM, 0, 5, 0, 0, 0, 0, NULL},
    };
    for (uint32_t i = 0; i < n; ++i)
        for (uint32_t j = 0; j < n; ++j) {
            L.acc = 0, L.bad = false;
            for (uint32_t k = 0; k < n; k += B) {
                unsigned m = n - k < B ? n - k : B;
                prog[0].base = (int64_t)i * n + k, prog[1].base = (int64_t)k * n + j, prog[1].stride = n;
                run(prog, 4, &L, m);
            }
            total += (double)L.acc, *last = (double)L.acc;
        }
    return total;
}

/* matmul with the loops over j and k swapped (each s[j] still summed in k's order: the same numbers): a row of b a batch */
static double matmul_j(uint32_t n, uint32_t *a, uint32_t *b, double *last) {
    static Lanes L;
    static int64_t s[1024];
    double total = 0;
    for (uint32_t i = 0; i < n; ++i) {
        memset(s, 0, n * sizeof(*s));
        int64_t over = 0;
        for (uint32_t k = 0; k < n; ++k)
            for (uint32_t j = 0; j < n; j += B) {
                unsigned m = n - j < B ? n - j : B;
                op_loads(L.i[1], b, (int64_t)k * n + j, 1, m);
                op_mulk(L.i[2], L.i[1], a[i * n + k], m, &over);
                op_addto(s + j, L.i[2], m, &over);
            }
        for (uint32_t j = 0; j < n; ++j) total += (double)s[j], *last = (double)s[j];
    }
    return total;
}

/* knapsack: each item's rounds a batch: best[c] = max(best[c], best[c - w] + v), c = capacity - k */
static double knapsack(uint32_t n, uint32_t capacity, const uint32_t *weight, const uint32_t *worth, uint32_t *best) {
    static Lanes L;
    POp prog[] = {
        {P_LOADS, 2, 0, 0, 0, 0, -1, best},   /* best[c - w] */
        {P_ADDK, 2, 2, 0, 0, 0, 0, NULL},     /* + worth */
        {P_LOADS, 3, 0, 0, 0, 0, -1, best},   /* best[c] */
        {P_MAX, 4, 2, 3, 0, 0, 0, NULL},
        {P_STORES, 0, 4, 0, 0, 0, -1, best},
    };
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t rounds = capacity + 1 - weight[i];
        for (uint32_t k = 0; k < rounds; k += B) {
            unsigned m = rounds - k < B ? rounds - k : B;
            int64_t c = (int64_t)capacity - k;
            prog[0].base = c - weight[i], prog[1].k = worth[i], prog[2].base = c, prog[4].base = c;
            run(prog, 5, &L, m);
        }
    }
    return best[capacity];
}

/* roots: a batch of i, each lane's Newton rounds while it goes on (lanes out as they stop) */
static double roots(uint32_t n, double *steps) {
    static double r[B], x[B], next[B];
    static uint32_t live[B];
    double sum = 0, count = 0;
    for (uint32_t i0 = 1; i0 <= n; i0 += B) {
        unsigned m = n + 1 - i0 < B ? n + 1 - i0 : B, alive = m;
        for (unsigned l = 0; l < m; ++l) r[l] = x[l] = i0 + l, live[l] = l;
        bool bad = false;
        for (unsigned s = 0; s < 64 && alive; ++s) {
            /* the body as three operations over the live lanes (gathered dense), then who goes on */
            for (unsigned q = 0; q < alive; ++q) { unsigned l = live[q]; bad |= r[l] == 0; next[q] = x[l] / r[l]; bad |= !isfinite(next[q]); }
            for (unsigned q = 0; q < alive; ++q) { unsigned l = live[q]; next[q] = r[l] + next[q]; bad |= !isfinite(next[q]); }
            for (unsigned q = 0; q < alive; ++q) { next[q] = next[q] * 0.5; bad |= !isfinite(next[q]); }
            unsigned kept = 0;
            for (unsigned q = 0; q < alive; ++q) {
                unsigned l = live[q];
                if (next[q] >= r[l]) continue;
                r[l] = next[q], ++count, live[kept++] = l;
            }
            alive = kept;
        }
        (void)bad;
        for (unsigned l = 0; l < m; ++l) sum += r[l]; /* in i's order, as run adds them */
    }
    *steps = count;
    return sum;
}


/* Narrow lanes: cells of four bytes as they are (u32), a sum past 32 bits a failed check (the column's width) */
__attribute__((optimize("O3"))) static void u32_addk_max_store(uint32_t *restrict col, int64_t dst, int64_t src, uint32_t k, unsigned n, uint32_t *over) {
    /* best[c - l] = max(best[c - l], best[c - w - l] + k), lanes descending (reads below every write of the batch) */
    uint32_t o = 0;
    const uint32_t *from = col + src - (n - 1);
    uint32_t *to = col + dst - (n - 1);
    uint32_t tmp[256];
    for (unsigned l = 0; l < n; ++l) { uint32_t x = from[l] + k; o |= x < k; uint32_t y = to[l]; tmp[l] = x > y ? x : y; }
    *over |= o;
    if (!o) for (unsigned l = 0; l < n; ++l) to[l] = tmp[l];
}
static double knapsack_u32(uint32_t n, uint32_t capacity, const uint32_t *weight, const uint32_t *worth, uint32_t *best) {
    uint32_t over = 0;
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t rounds = capacity + 1 - weight[i];
        for (uint32_t k = 0; k < rounds; k += B) {
            unsigned m = rounds - k < B ? rounds - k : B;
            int64_t c = (int64_t)capacity - k;
            u32_addk_max_store(best, c, c - weight[i], worth[i], m, &over);
        }
    }
    return over ? -1 : best[capacity];
}
/* s[j..] += k * b[k n + j..]: one operation (a multiply and an add of a kept column), s as whole numbers below 2^53 */
__attribute__((optimize("O3"))) static void axpy(int64_t *restrict s, const uint32_t *restrict b, uint32_t k, unsigned n, int64_t *over) {
    int64_t o = 0;
    for (unsigned l = 0; l < n; ++l) { int64_t x = s[l] + (int64_t)((uint64_t)k * b[l]); s[l] = x; o |= (x >> 53) ^ (x >> 63); }
    *over |= o;
}
static double matmul_axpy(uint32_t n, uint32_t *a, uint32_t *b, double *last) {
    static int64_t s[1024];
    double total = 0;
    int64_t over = 0;
    for (uint32_t i = 0; i < n; ++i) {
        memset(s, 0, n * sizeof(*s));
        for (uint32_t k = 0; k < n; ++k)
            for (uint32_t j = 0; j < n; j += B) {
                unsigned m = n - j < B ? n - j : B;
                axpy(s + j, b + (size_t)k * n + j, a[i * n + k], m, &over);
            }
        for (uint32_t j = 0; j < n; ++j) total += (double)s[j], *last = (double)s[j];
    }
    return over ? -1 : total;
}

/* The C given what the batches were given, the cells made before as the batches' are:
 * the loops over j and k swapped, Pure's four-byte cells, -O3 with ivdep (vector). */
__attribute__((noinline)) static double c_ijk(uint32_t n, const double *a, const double *b, double *last) {
    double total = 0;
    for (uint32_t i = 0; i < n; ++i)
        for (uint32_t j = 0; j < n; ++j) {
            double s = 0;
            for (uint32_t k = 0; k < n; ++k) s += a[i * n + k] * b[k * n + j];
            total += s, *last = s;
        }
    return total;
}
__attribute__((noinline)) static double c_ikj(uint32_t n, const double *a, const double *b, double *last) {
    static double s[1024];
    double total = 0;
    for (uint32_t i = 0; i < n; ++i) {
        memset(s, 0, n * sizeof(*s));
        for (uint32_t k = 0; k < n; ++k)
            for (uint32_t j = 0; j < n; ++j) s[j] += a[i * n + k] * b[k * n + j];
        for (uint32_t j = 0; j < n; ++j) total += s[j], *last = s[j];
    }
    return total;
}
__attribute__((noinline)) static double c_ikj_u32(uint32_t n, const uint32_t *a, const uint32_t *b, double *last) {
    static int64_t s[1024];
    double total = 0;
    for (uint32_t i = 0; i < n; ++i) {
        memset(s, 0, n * sizeof(*s));
        for (uint32_t k = 0; k < n; ++k)
            for (uint32_t j = 0; j < n; ++j) s[j] += (int64_t)a[i * n + k] * b[k * n + j];
        for (uint32_t j = 0; j < n; ++j) total += (double)s[j], *last = (double)s[j];
    }
    return total;
}
__attribute__((noinline, optimize("O3"))) static double c_ikj_vec(uint32_t n, const uint32_t *restrict a, const uint32_t *restrict b, double *last) {
    static int64_t s[1024];
    double total = 0;
    for (uint32_t i = 0; i < n; ++i) {
        memset(s, 0, n * sizeof(*s));
        for (uint32_t k = 0; k < n; ++k)
            for (uint32_t j = 0; j < n; ++j) s[j] += (int64_t)a[i * n + k] * b[k * n + j];
        for (uint32_t j = 0; j < n; ++j) total += (double)s[j], *last = (double)s[j];
    }
    return total;
}
__attribute__((noinline, optimize("O3"))) static double c_ikj_dvec(uint32_t n, const double *restrict a, const double *restrict b, double *last) {
    static double s[1024];
    double total = 0;
    for (uint32_t i = 0; i < n; ++i) {
        memset(s, 0, n * sizeof(*s));
        for (uint32_t k = 0; k < n; ++k)
            for (uint32_t j = 0; j < n; ++j) s[j] += a[i * n + k] * b[k * n + j];
        for (uint32_t j = 0; j < n; ++j) total += s[j], *last = s[j];
    }
    return total;
}
__attribute__((noinline)) static double c_knap_i64(uint32_t n, uint32_t capacity, const uint32_t *weight, const uint32_t *worth, int64_t *best) {
    for (uint32_t i = 0; i < n; ++i)
        for (uint32_t c = capacity; c >= weight[i] && c <= capacity; --c) {
            int64_t with = best[c - weight[i]] + worth[i];
            if (with > best[c]) best[c] = with;
        }
    return (double)best[capacity];
}
__attribute__((noinline)) static double c_knap_u32(uint32_t n, uint32_t capacity, const uint32_t *weight, const uint32_t *worth, uint32_t *best) {
    for (uint32_t i = 0; i < n; ++i)
        for (uint32_t c = capacity; c >= weight[i] && c <= capacity; --c) {
            uint32_t with = best[c - weight[i]] + worth[i];
            if (with > best[c]) best[c] = with;
        }
    return best[capacity];
}
__attribute__((noinline, optimize("O3", "vect-cost-model=unlimited"))) static double c_knap_vec(uint32_t n, uint32_t capacity, const uint32_t *weight, const uint32_t *worth, uint32_t *best) {
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t w = weight[i], v = worth[i], rounds = capacity + 1 - w;
        /* best[c] for c descending reads best[c - w], written later in that order: no round reads another's write */
#pragma GCC ivdep
        for (uint32_t k = 0; k < rounds; ++k) {
            uint32_t c = capacity - k, with = best[c - w] + v;
            best[c] = with > best[c] ? with : best[c];
        }
    }
    return best[capacity];
}

/* knapsack in C as the batch runs it: 256 rounds at a time, read, then written (ascending within the block) */
__attribute__((noinline, optimize("O3"))) static double c_knap_block(uint32_t n, uint32_t capacity, const uint32_t *weight, const uint32_t *worth, uint32_t *restrict best) {
    uint32_t tmp[256];
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t w = weight[i], v = worth[i], rounds = capacity + 1 - w;
        for (uint32_t k = 0; k < rounds; k += 256) {
            uint32_t m = rounds - k < 256 ? rounds - k : 256, low = capacity - k - (m - 1);
            const uint32_t *from = best + low - w;
            uint32_t *to = best + low;
            for (uint32_t l = 0; l < m; ++l) { uint32_t with = from[l] + v, was = to[l]; tmp[l] = with > was ? with : was; }
            for (uint32_t l = 0; l < m; ++l) to[l] = tmp[l];
        }
    }
    return best[capacity];
}

static int cmpd(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return (x > y) - (x < y); }
#define MEDIAN(arr) (qsort(arr, 11, sizeof(double), cmpd), arr[5])
#define TIME(slot, call) do { double t_ = now(); call; (slot) = now() - t_; } while (0)
static volatile uint32_t sizes[5] = {200, 100, 50000, 100000, 0}; /* read at run time: no C sees them as constants */
static void (*volatile natural[3])(const uint32_t *, double *) = {c_matmul, c_knapsack, c_roots};

int main(void) {
    /* matmul, 200 x 200, cells as natural.h makes them */
    uint32_t n = sizes[0];
    uint32_t *a = malloc(4 * n * n), *b = malloc(4 * n * n);
    double *da = malloc(8 * n * n), *db = malloc(8 * n * n);
    minstd_x = 1;
    for (uint32_t i = 0; i < n * n; ++i) a[i] = minstd_next() % 100, b[i] = minstd_next() % 100, da[i] = a[i], db[i] = b[i];
    double tc[11], tn[11], ti[11], tu[11], tv[11], td[11], tk[11], tj[11], tx[11], out[4], last;
    uint32_t in[2] = {n, 0};
    double want = 0, got[8];
    for (int r = 0; r < 11; ++r) {
        TIME(tc[r], natural[0](in, out));
        want = out[0];
        TIME(tn[r], got[0] = c_ijk(n, da, db, &last));
        TIME(ti[r], got[1] = c_ikj(n, da, db, &last));
        TIME(tu[r], got[2] = c_ikj_u32(n, a, b, &last));
        TIME(tv[r], got[6] = c_ikj_vec(n, a, b, &last));
        TIME(td[r], got[7] = c_ikj_dvec(n, da, db, &last));
        TIME(tk[r], got[3] = matmul_k(n, a, b, &last));
        TIME(tj[r], got[4] = matmul_j(n, a, b, &last));
        TIME(tx[r], got[5] = matmul_axpy(n, a, b, &last));
    }
    bool same = true;
    for (int g = 0; g < 8; ++g) same &= got[g] == want;
    printf("{\"work\":\"matmul\",\"c_natural_ms\":%.2f,\"c_ms\":%.2f,\"c_ikj_ms\":%.2f,\"c_ikj_u32_ms\":%.2f,\"c_ikj_vec_ms\":%.2f,\"c_ikj_dvec_ms\":%.2f,\"batch_k_ms\":%.2f,\"batch_j_ms\":%.2f,\"axpy_ms\":%.2f,\"same\":%s}\n",
           MEDIAN(tc), MEDIAN(tn), MEDIAN(ti), MEDIAN(tu), MEDIAN(tv), MEDIAN(td), MEDIAN(tk), MEDIAN(tj), MEDIAN(tx), same ? "true" : "false");
    /* knapsack, 100 items, capacity 50,000 */
    uint32_t items = sizes[1], capacity = sizes[2], *weight = malloc(4 * items), *worth = malloc(4 * items), *best = malloc(4 * (capacity + 1));
    int64_t *best64 = malloc(8 * (capacity + 1));
    minstd_x = 1;
    for (uint32_t i = 0; i < items; ++i) {
        weight[i] = minstd_next() % 1000 + 1;
        worth[i] = minstd_next() % 1000;
    }
    double kc[11], k6[11], k3[11], kv[11], kk[11], kb[11], ku[11], kgot[6];
    uint32_t kin[2] = {items, capacity};
    for (int r = 0; r < 11; ++r) {
        TIME(kc[r], natural[1](kin, out));
        want = out[0];
        memset(best64, 0, 8 * (capacity + 1));
        TIME(k6[r], kgot[0] = c_knap_i64(items, capacity, weight, worth, best64));
        memset(best, 0, 4 * (capacity + 1));
        TIME(k3[r], kgot[1] = c_knap_u32(items, capacity, weight, worth, best));
        memset(best, 0, 4 * (capacity + 1));
        TIME(kv[r], kgot[2] = c_knap_vec(items, capacity, weight, worth, best));
        memset(best, 0, 4 * (capacity + 1));
        memset(best, 0, 4 * (capacity + 1));
        TIME(kk[r], kgot[5] = c_knap_block(items, capacity, weight, worth, best));
        memset(best, 0, 4 * (capacity + 1));
        TIME(kb[r], kgot[3] = knapsack(items, capacity, weight, worth, best));
        memset(best, 0, 4 * (capacity + 1));
        TIME(ku[r], kgot[4] = knapsack_u32(items, capacity, weight, worth, best));
    }
    same = true;
    for (int g = 0; g < 6; ++g) same &= kgot[g] == want;
    printf("{\"work\":\"knapsack\",\"c_natural_ms\":%.2f,\"c_ms\":%.2f,\"c_u32_ms\":%.2f,\"c_vec_ms\":%.2f,\"c_block_ms\":%.2f,\"batch_ms\":%.2f,\"u32_ms\":%.2f,\"same\":%s}\n",
           MEDIAN(kc), MEDIAN(k6), MEDIAN(k3), MEDIAN(kv), MEDIAN(kk), MEDIAN(kb), MEDIAN(ku), same ? "true" : "false");
    /* roots, 100,000 */
    double rc[11], rb[11], rgot = 0, steps;
    uint32_t rin[1] = {sizes[3]};
    for (int r = 0; r < 11; ++r) {
        TIME(rc[r], natural[2](rin, out));
        want = out[0];
        TIME(rb[r], rgot = roots(rin[0], &steps));
    }
    printf("{\"work\":\"roots\",\"c_ms\":%.2f,\"batch_ms\":%.2f,\"same\":%s}\n", MEDIAN(rc), MEDIAN(rb), rgot == want && steps == out[1] ? "true" : "false");
}
