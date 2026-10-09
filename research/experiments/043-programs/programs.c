/* Research 139's deciding experiment: a batch loop's body as the program a
 * load would make of it (not built yet: docs/research/139), interpreted a
 * step at a time by one switch. Scalar steps make the uniform and affine
 * values as the executor's rules do (a number's arithmetic finite, an affine
 * value's parts whole within 2^52, a DENSE key's row whole and in range);
 * kernel steps run over four-byte lanes in registers fixed at load (no
 * buffer sought at run time); a list's cells are read once a batch; a row
 * range is checked at its ends; sums and products past 2^32 fail the batch;
 * a set waits for the batch's end, after its reads' distance is checked.
 * The bodies are the suite's as their bytecode has them:
 *   matmul   the j loop a batch (200 lanes), k's rounds together:
 *            s = s + a[i * n + k].v * b[k * n + j].v
 *   knapsack each item's rounds a batch (1024 lanes): c = capacity - k,
 *            with = table[c - w].best + v, if with > table[c].best { set }
 * Each also with the same kernels called straight from C, the scalars made
 * in C (direct: no interpretation), against natural.h's C.
 *   cc -O3 -falign-loops=64 -std=c17 -I../040-against-c programs.c -lm -o programs
 *   ./programs [matmul|knapsack] [program|direct|c|all] [rounds]  (one JSON line each) */
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

enum { LANES = 1024, SREGS = 16, LREGS = 8 };

/* ---- Kernels (as module-batch.c's words_op, cells_at, put_words), each its own function: inlined in run()'s switch,
 * GCC made them scalar (their lanes' pointers from one struct) */

__attribute__((noinline)) static bool k_mulk(uint32_t *restrict out, const uint32_t *restrict xs, uint32_t k, unsigned n) {
    uint32_t over = 0;
    for (unsigned l = 0; l < n; ++l) { uint64_t p = (uint64_t)xs[l] * k; out[l] = (uint32_t)p, over |= (uint32_t)(p >> 32); }
    return !over;
}
__attribute__((noinline)) static bool k_addk(uint32_t *restrict out, const uint32_t *restrict xs, uint32_t k, unsigned n) {
    uint32_t over = 0;
    for (unsigned l = 0; l < n; ++l) { uint32_t s = xs[l] + k; out[l] = s, over |= s < k; }
    return !over;
}
__attribute__((noinline)) static bool k_add(uint32_t *restrict out, const uint32_t *restrict xs, const uint32_t *restrict ys, unsigned n) {
    uint32_t over = 0;
    for (unsigned l = 0; l < n; ++l) { uint32_t s = xs[l] + ys[l]; out[l] = s, over |= s < xs[l]; }
    return !over;
}
__attribute__((noinline)) static unsigned k_gt(uint32_t *restrict out, const uint32_t *restrict xs, const uint32_t *restrict ys, unsigned n) {
    uint32_t count = 0;
    for (unsigned l = 0; l < n; ++l) { uint32_t t = xs[l] > ys[l]; out[l] = t, count += t; }
    return count;
}
/* a one-byte column's cells at rows first + lane: four-byte lanes */
__attribute__((noinline)) static void k_widen(uint32_t *restrict out, const uint8_t *restrict from, unsigned n) {
    for (unsigned l = 0; l < n; ++l) out[l] = from[l];
}
__attribute__((noinline)) static void k_put_masked(uint32_t *restrict to, const uint32_t *restrict from, const uint32_t *restrict mask, unsigned n) {
    for (unsigned l = 0; l < n; ++l) to[l] = mask[l] ? from[l] : to[l];
}

/* ---- Lists as a batch reads them: a DENSE list's column (each key its row) */

typedef struct { size_t count; void *column; uint8_t width; } Cells;

/* ---- The program */

enum {
    S_FIELD,    /* s[d] = the message's field arg */
    S_LOCAL,    /* s[d] = local arg (a local the block does not set) */
    S_ROUND,    /* s[d] = the nested loop's round (uniform) */
    S_LANE,     /* s[d] = the batch loop's value (affine: first, lane_step) */
    S_MUL,      /* s[d] = s[a] * s[b] (uniform by uniform, or uniform by affine) */
    S_ADD,      /* s[d] = s[a] + s[b] */
    S_SUB,      /* s[d] = s[a] - s[b] */
    S_GET,      /* s[d] = list arg's field at the row of key s[a] (uniform; DENSE: the key its row) */
    K_READ,     /* L[d] = list arg's field at affine rows s[a] (four bytes: the column itself; one byte: made four) */
    K_MULK,     /* L[d] = L[a] * s[b] */
    K_ADDK,     /* L[d] = L[a] + s[b] */
    K_ADD,      /* L[d] = L[a] + L[b] */
    K_GT,       /* L[d] = L[a] > L[b], its 1s counted (a branch's lanes) */
    K_IF,       /* none of L[a]'s lanes: on past the next b steps */
    K_SET,      /* list arg's field at affine rows s[a] = L[b] in L[d]'s lanes, at the batch's end */
};
typedef struct { uint8_t op, d, a, b; uint32_t arg; } Step;
typedef struct { double value, step; bool affine; } Scalar;
typedef struct { uint32_t list; Scalar rows; const uint32_t *value, *mask; } Set;

typedef struct {
    Scalar s[SREGS];
    const uint32_t *lane[LREGS];  /* each lane register's lanes: its own, or a column's */
    uint32_t own[LREGS][2][LANES + 24]; /* each two (laid out at load, not 4 KB apart as the executor's buffers): a result never its operand's */
    unsigned ones[LREGS];
    const uint32_t *field;        /* the message */
    const double *local;
    double round, first, lane_step;
    unsigned n;
    Cells cells[4];               /* each list's, once a batch */
    Set sets[2];
    unsigned set_count;
    bool failed;
} Machine;

static bool whole(double x) { return x == floor(x) && fabs(x) <= 0x1p52 && !(x == 0 && signbit(x)); }
/* value + step × lane, as affine_of: whole parts at both ends (else the executor makes lanes; here the batch fails) */
static bool affine(Machine *M, double value, double step, Scalar *out) {
    if (!whole(value) || !whole(step) || !whole(value + step * (M->n - 1))) return false;
    *out = (Scalar){value, step, step != 0};
    return true;
}
/* a op b of scalars as the executor's rules: uniforms as numbers (finite), affine values as affine_op */
static bool scalar_op(Machine *M, uint8_t op, const Scalar *a, const Scalar *b, Scalar *out) {
    if (!a->affine && !b->affine) {
        double v = op == S_MUL ? a->value * b->value : op == S_ADD ? a->value + b->value : a->value - b->value;
        if (!isfinite(v)) return false;
        *out = (Scalar){v, 0, false};
        return true;
    }
    if ((!a->affine && !whole(a->value)) || (!b->affine && !whole(b->value))) return false;
    double as = a->affine ? a->step : 0, bs = b->affine ? b->step : 0;
    switch (op) {
    case S_ADD: return affine(M, a->value + b->value, as + bs, out);
    case S_SUB: return affine(M, a->value - b->value, as - bs, out);
    default:
        if (!a->affine && a->value > 0) return affine(M, a->value * b->value, a->value * bs, out);
        if (!b->affine && b->value > 0) return affine(M, a->value * b->value, as * b->value, out);
        return false;
    }
}

/* Where register d's result goes: of its two buffers the one neither operand holds (s = s + e: the other). */
static uint32_t *target(Machine *M, unsigned d, unsigned a, unsigned b) {
    uint32_t *out = M->own[d][0];
    return out == M->lane[a] || out == M->lane[b] ? M->own[d][1] : out;
}

/* The program's steps once for the batch's lanes (a nested loop's round once). false: the batch fails (the executor's rounds). */
static bool run(const Step *prog, unsigned count, Machine *M) {
    unsigned n = M->n;
    for (unsigned i = 0; i < count; ++i) {
        const Step *p = &prog[i];
        switch (p->op) {
        case S_FIELD: M->s[p->d] = (Scalar){M->field[p->arg], 0, false}; break;
        case S_LOCAL: M->s[p->d] = (Scalar){M->local[p->arg], 0, false}; break;
        case S_ROUND: M->s[p->d] = (Scalar){M->round, 0, false}; break;
        case S_LANE:
            if (!affine(M, M->first, M->lane_step, &M->s[p->d])) return false;
            break;
        case S_MUL: case S_ADD: case S_SUB:
            if (!scalar_op(M, p->op, &M->s[p->a], &M->s[p->b], &M->s[p->d])) return false;
            break;
        case S_GET: {
            const Cells *c = &M->cells[p->arg];
            double k = M->s[p->a].value;
            if (M->s[p->a].affine || !(k >= 0 && k < (double)c->count && k == floor(k))) return false;
            size_t row = (size_t)k;
            M->s[p->d] = (Scalar){c->width == 1 ? ((const uint8_t *)c->column)[row] : ((const uint32_t *)c->column)[row], 0, false};
            break;
        }
        case K_READ: {
            const Cells *c = &M->cells[p->arg];
            const Scalar *r = &M->s[p->a];
            double end = r->value + r->step * (n - 1);
            if (!r->affine || r->step != 1 || !(r->value >= 0 && end < (double)c->count)) return false; /* (the executor's other ways) */
            size_t first = (size_t)r->value;
            if (c->width == 4) M->lane[p->d] = (const uint32_t *)c->column + first; /* a view */
            else k_widen(M->own[p->d][0], (const uint8_t *)c->column + first, n), M->lane[p->d] = M->own[p->d][0];
            break;
        }
        case K_MULK: {
            uint32_t *out = target(M, p->d, p->a, p->a);
            if (M->s[p->b].affine || !(M->s[p->b].value >= 0 && M->s[p->b].value < 4294967296.0) ||
                !k_mulk(out, M->lane[p->a], (uint32_t)M->s[p->b].value, n))
                return false;
            M->lane[p->d] = out;
            break;
        }
        case K_ADDK: {
            uint32_t *out = target(M, p->d, p->a, p->a);
            if (M->s[p->b].affine || !(M->s[p->b].value >= 0 && M->s[p->b].value < 4294967296.0) ||
                !k_addk(out, M->lane[p->a], (uint32_t)M->s[p->b].value, n))
                return false;
            M->lane[p->d] = out;
            break;
        }
        case K_ADD: {
            uint32_t *out = target(M, p->d, p->a, p->b);
            if (!k_add(out, M->lane[p->a], M->lane[p->b], n)) return false;
            M->lane[p->d] = out;
            break;
        }
        case K_GT: {
            uint32_t *out = target(M, p->d, p->a, p->b);
            M->ones[p->d] = k_gt(out, M->lane[p->a], M->lane[p->b], n);
            M->lane[p->d] = out;
            break;
        }
        case K_IF:
            if (!M->ones[p->a]) i += p->b;
            break;
        case K_SET:
            if (M->set_count == 2) return false;
            M->sets[M->set_count++] = (Set){p->arg, M->s[p->a], M->lane[p->b], M->ones[p->d] == n ? NULL : M->lane[p->d]};
            break;
        }
    }
    return true;
}

/* ---- matmul */

static uint64_t minstd_x;
static uint32_t minstd_next(void) { return (uint32_t)(minstd_x = minstd_x * 48271 % 2147483647); }

/* The j loop a batch (as the executor runs it: the lanes j, k's rounds together), s in lane register 0. */
static double matmul_program(uint32_t n, const uint8_t *a, const uint8_t *b, double *last, uint64_t *work) {
    static Machine M;
    enum { R_I, R_N, R_IN, R_K, R_KEYA, R_AV, R_KN, R_J, R_ROWS };
    const Step body[] = {
        {S_LOCAL, R_I, 0, 0, 0}, {S_FIELD, R_N, 0, 0, 0}, {S_MUL, R_IN, R_I, R_N, 0}, {S_ROUND, R_K, 0, 0, 0},
        {S_ADD, R_KEYA, R_IN, R_K, 0}, {S_GET, R_AV, R_KEYA, 0, 0},                 /* a[i * n + k].v */
        {S_MUL, R_KN, R_K, R_N, 0}, {S_LANE, R_J, 0, 0, 0}, {S_ADD, R_ROWS, R_KN, R_J, 0},
        {K_READ, 1, R_ROWS, 0, 1},                                                   /* b[k * n + j].v */
        {K_MULK, 2, 1, R_AV, 0}, {K_ADD, 0, 0, 2, 0},                               /* s = s + ... */
    };
    double local[1], total = 0;
    uint32_t field[1] = {n};
    M.field = field, M.local = local, M.n = n, M.first = 0, M.lane_step = 1;
    for (uint32_t i = 0; i < n; ++i) {
        local[0] = i;
        M.cells[0] = (Cells){(size_t)n * n, (void *)a, 1}, M.cells[1] = (Cells){(size_t)n * n, (void *)b, 1}; /* once a batch */
        memset(M.own[0][0], 0, n * sizeof(uint32_t)), M.lane[0] = M.own[0][0];                            /* let s = 0 */
        for (uint32_t k = 0; k < n; ++k) {
            M.round = k;
            *work += n * (uint64_t)(sizeof(body) / sizeof(body[0]) + 1);
            if (!run(body, sizeof(body) / sizeof(body[0]), &M)) return NAN;
        }
        uint64_t sum = 0;
        for (uint32_t l = 0; l < n; ++l) sum += M.lane[0][l];
        total += (double)sum, *last = M.lane[0][n - 1];
    }
    return total;
}

/* The same kernels called straight, the scalars made in C. */
static double matmul_direct(uint32_t n, const uint8_t *a, const uint8_t *b, double *last) {
    static uint32_t s[2][LANES + 24], x[LANES + 24], y[LANES + 24]; /* s two (a result never its operand), as the program's */
    double total = 0;
    for (uint32_t i = 0; i < n; ++i) {
        unsigned t = 0;
        memset(s[0], 0, n * sizeof(uint32_t));
        for (uint32_t k = 0; k < n; ++k, t ^= 1) {
            k_widen(x, b + (size_t)k * n, n);
            if (!k_mulk(y, x, a[(size_t)i * n + k], n) || !k_add(s[t ^ 1], s[t], y, n)) return NAN;
        }
        uint64_t sum = 0;
        for (uint32_t l = 0; l < n; ++l) sum += s[t][l];
        total += (double)sum, *last = s[t][n - 1];
    }
    return total;
}

/* ---- knapsack */

/* Each item's rounds a batch, lanes in memory's order (the executor numbers them so after its first batch). */
static double knapsack_program(uint32_t items, uint32_t capacity, const uint32_t *weight, const uint32_t *worth, uint32_t *best, uint64_t *work) {
    static Machine M;
    enum { R_CAP, R_K, R_C, R_W, R_CW, R_V };
    const Step body[] = {
        {S_FIELD, R_CAP, 0, 0, 0}, {S_LANE, R_K, 0, 0, 0}, {S_SUB, R_C, R_CAP, R_K, 0},   /* c = capacity - k (lanes: -k) */
        {S_LOCAL, R_W, 0, 0, 0}, {S_SUB, R_CW, R_C, R_W, 0},                              /* c - it.weight */
        {K_READ, 1, R_CW, 0, 0}, {S_LOCAL, R_V, 0, 0, 1}, {K_ADDK, 2, 1, R_V, 0},         /* with = table[c - w].best + it.worth */
        {K_READ, 3, R_C, 0, 0}, {K_GT, 4, 2, 3, 0},                                       /* with > table[c].best */
        {K_IF, 0, 4, 1, 0}, {K_SET, 4, R_C, 2, 0},                                        /* table[c].best = with */
    };
    double local[2];
    uint32_t field[1] = {capacity};
    M.field = field, M.local = local;
    for (uint32_t i = 0; i < items; ++i) {
        local[0] = weight[i], local[1] = worth[i];
        uint32_t rounds = capacity + 1 - weight[i];
        for (uint32_t k0 = 0; k0 < rounds; k0 += LANES) {
            unsigned n = rounds - k0 < LANES ? rounds - k0 : LANES;
            /* lanes in memory's order: lane l is round k0 + n - 1 - l (c = capacity - k ascending in l) */
            M.n = n, M.first = (double)(k0 + n - 1), M.lane_step = -1, M.set_count = 0;
            field[0] = capacity;
            M.cells[0] = (Cells){(size_t)capacity + 1, best, 4}; /* once a batch */
            *work += n * (uint64_t)(sizeof(body) / sizeof(body[0]) + 1);
            if (!run(body, sizeof(body) / sizeof(body[0]), &M)) return NAN;
            for (unsigned t = 0; t < M.set_count; ++t) { /* the end: the set's rows against the reads' (distance w >= 0: none of these rounds reads what an earlier one sets), then set */
                const Set *s = &M.sets[t];
                double d = M.s[R_CW].value - s->rows.value; /* the read c - w against the set c, both step 1 */
                if (!(d <= 0 && d == floor(d))) return NAN;
                uint32_t *to = best + (size_t)s->rows.value;
                if (s->mask) k_put_masked(to, s->value, s->mask, n);
                else memcpy(to, s->value, n * sizeof(uint32_t));
            }
        }
    }
    return best[capacity];
}

static double knapsack_direct(uint32_t items, uint32_t capacity, const uint32_t *weight, const uint32_t *worth, uint32_t *best) {
    static uint32_t with[LANES], gt[LANES];
    for (uint32_t i = 0; i < items; ++i) {
        uint32_t w = weight[i], v = worth[i], rounds = capacity + 1 - w;
        for (uint32_t k0 = 0; k0 < rounds; k0 += LANES) {
            unsigned n = rounds - k0 < LANES ? rounds - k0 : LANES;
            size_t low = capacity - (k0 + n - 1);
            if (!k_addk(with, best + low - w, v, n)) return NAN;
            unsigned ones = k_gt(gt, with, best + low, n);
            if (!ones) continue;
            if (ones == n) memcpy(best + low, with, n * sizeof(uint32_t));
            else k_put_masked(best + low, with, gt, n);
        }
    }
    return best[capacity];
}

/* ---- */

static int cmp(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return (x > y) - (x < y); }

int main(int argc, char **argv) {
    const char *body = argc > 1 ? argv[1] : "all", *way = argc > 2 ? argv[2] : "all";
    int rounds = argc > 3 ? atoi(argv[3]) : 11;
    bool all = !strcmp(way, "all");
    double *ms = malloc(rounds * sizeof(double)), out[4];
    if (!strcmp(body, "all") || !strcmp(body, "matmul")) {
        uint32_t n = 200, in[2] = {n, 0};
        uint8_t *a = malloc((size_t)n * n), *b = malloc((size_t)n * n);
        minstd_x = 1;
        for (uint32_t i = 0; i < n * n; ++i) a[i] = (uint8_t)(minstd_next() % 100), b[i] = (uint8_t)(minstd_next() % 100);
        c_matmul(in, out);
        double want = out[0];
        for (int w = 0; w < 3; ++w) {
            const char *name = w == 0 ? "program" : w == 1 ? "direct" : "c";
            if (!all && strcmp(way, name)) continue;
            double got = 0, last = 0;
            uint64_t work = 0;
            for (int r = 0; r < rounds; ++r) {
                double t = now();
                if (w == 0) got = matmul_program(n, a, b, &last, &work);
                else if (w == 1) got = matmul_direct(n, a, b, &last);
                else c_matmul(in, out), got = out[0];
                ms[r] = now() - t;
            }
            qsort(ms, rounds, sizeof(double), cmp);
            printf("{\"body\":\"matmul\",\"way\":\"%s\",\"ms\":%.3f,\"same\":%s}\n", name, ms[rounds / 2], got == want ? "true" : "false");
        }
    }
    if (!strcmp(body, "all") || !strcmp(body, "knapsack")) {
        uint32_t items = 100, capacity = 50000, in[2] = {items, capacity};
        uint32_t *weight = malloc(4 * items), *worth = malloc(4 * items), *best = malloc(4 * (capacity + 1));
        minstd_x = 1;
        for (uint32_t i = 0; i < items; ++i) weight[i] = minstd_next() % 1000 + 1, worth[i] = minstd_next() % 1000;
        c_knapsack(in, out);
        double want = out[0];
        for (int w = 0; w < 3; ++w) {
            const char *name = w == 0 ? "program" : w == 1 ? "direct" : "c";
            if (!all && strcmp(way, name)) continue;
            double got = 0;
            uint64_t work = 0;
            for (int r = 0; r < rounds; ++r) {
                memset(best, 0, 4 * (capacity + 1));
                double t = now();
                if (w == 0) got = knapsack_program(items, capacity, weight, worth, best, &work);
                else if (w == 1) got = knapsack_direct(items, capacity, weight, worth, best);
                else c_knapsack(in, out), got = out[0];
                ms[r] = now() - t;
            }
            qsort(ms, rounds, sizeof(double), cmp);
            printf("{\"body\":\"knapsack\",\"way\":\"%s\",\"ms\":%.3f,\"same\":%s}\n", name, ms[rounds / 2], got == want ? "true" : "false");
        }
    }
    return 0;
}
