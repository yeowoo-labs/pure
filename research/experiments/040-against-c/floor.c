/* Research 124's floors: each workload of the suite written by hand in C as
 * Pure could run it at best, its representation Pure's (an event's lists as
 * arrays: FLAT in order, DENSE at their keys with key bits, columns as
 * narrow as their values; a list kept by sparse keys with a hash index,
 * ordered when it is walked), its meaning Pure's, and what that meaning
 * costs, by MODE:
 *   0 bare     no check at all: what the representation costs
 *   1 hoisted  the checks a compiler could leave: once a loop where it can
 *              bound what the loop does (its rounds against the work budget,
 *              its appends against the memory budget, a full DENSE list's
 *              bits, a column's width), none where the values' ranges prove
 *              them (whole numbers well inside 2^53 are finite and never
 *              -0), else where they fall
 *   2 naive    numbers as numbers (binary64, no ranges known) and every
 *              check where the meaning puts it: each result finite, no
 *              division by 0, each key a whole number (not -0) below 2^53,
 *              in its bits and below their reach, each position below the
 *              count, each cell fitting its column, each round against the
 *              work budget, each append against the memory budget
 * In 0 and 1 whole numbers whose ranges are known are integers.
 * A check that fails ends the event; which check and where is the
 * executor's to say (a failed event is undone and may be run again by it),
 * so a hoisted check only has to fail when some check would.
 *
 * floor-MODE ROUNDS [NAME...]   per round the floor's ms and the natural
 * C's (natural.h), the same answers, then the medians. */
#define _POSIX_C_SOURCE 200809L
#include <math.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "natural.h"

#ifndef MODE
#define MODE 1
#endif

static jmp_buf failed;
static uint64_t work, budget = UINT64_C(1) << 28; /* executed instructions, as Pure counts them */
static uint64_t grown, room = UINT64_C(1) << 28;  /* bytes the event added */

#define FAIL() longjmp(failed, 1)
#define NAIVE(ok)                                                                                                    \
    do {                                                                                                             \
        if (MODE == 2 && __builtin_expect(!(ok), 0)) FAIL();                                                        \
    } while (0)
#define HOISTED(ok)                                                                                                  \
    do {                                                                                                             \
        if (MODE >= 1 && __builtin_expect(!(ok), 0)) FAIL();                                                        \
    } while (0)
#define FINITE(x) (fabs(x) < INFINITY)
/* A round of `cost` instructions: counted each round (naive), or the rounds a loop may make at most checked
 * as it opens (hoisted, ROUNDS_OPEN) and what it made counted as it ends (ROUNDS_DONE). */
#define ROUND(cost)                                                                                                  \
    do {                                                                                                             \
        if (MODE == 2) {                                                                                             \
            work += (cost);                                                                                          \
            if (__builtin_expect(work > budget, 0)) FAIL();                                                          \
        }                                                                                                            \
    } while (0)
#define ROUNDS_OPEN(most, cost) HOISTED(budget - work >= (uint64_t)(most) * (cost))
#define ROUNDS_DONE(made, cost)                                                                                      \
    do {                                                                                                             \
        if (MODE == 1) work += (uint64_t)(made) * (cost);                                                            \
    } while (0)
#define APPEND(bytes)                                                                                                \
    do {                                                                                                             \
        if (MODE == 2) {                                                                                             \
            grown += (bytes);                                                                                        \
            if (__builtin_expect(grown > room, 0)) FAIL();                                                           \
        }                                                                                                            \
    } while (0)
#define APPENDS_OPEN(most, bytes)                                                                                    \
    do {                                                                                                             \
        if (MODE == 1) {                                                                                             \
            grown += (uint64_t)(most) * (bytes);                                                                     \
            if (__builtin_expect(grown > room, 0)) FAIL();                                                           \
        }                                                                                                            \
    } while (0)

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e3 + (double)t.tv_nsec / 1e6;
}

static int compare_doubles(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

/* What a number is: binary64 in the naive code (its results checked finite, its keys whole), else an
 * integer where the values are whole (their ranges known). */
#if MODE == 2
typedef double num;
static double fin(double x) {
    NAIVE(FINITE(x));
    return x;
}
static uint64_t key_of(double x) {
    NAIVE(x >= 0 && x < 0x1p53 && x == floor(x) && !(x == 0 && signbit(x)));
    return (uint64_t)x;
}
#define F(x) fin(x)
#define KEY(x) key_of(x)
#else
typedef int64_t num;
#define F(x) (x)
#define KEY(x) ((uint64_t)(x))
#endif

/* A DENSE list's key bits: whether key is there. */
static bool bit(const uint64_t *bits, uint64_t key) { return bits[key >> 6] >> (key & 63) & 1; }

/* ---- roots: numbers; r >= 1 and i <= n bound every value, so the hoisted code checks none */

static void f_roots(const uint32_t *in, double *out) {
    uint32_t n = in[0];
    double sum = 0, count = 0;
    ROUNDS_OPEN(n, 12 * 64 + 10);
    for (uint32_t i = 1; i <= n; ++i) {
        double r = i;
        int s = 0;
        for (; s < 64; ++s) {
            NAIVE(r != 0);
            double q = i / r;
            NAIVE(FINITE(q));
            double t = r + q;
            NAIVE(FINITE(t));
            double next = t / 2;
            NAIVE(FINITE(next));
            ROUND(12);
            if (next >= r) break;
            r = next;
            count += 1;
            NAIVE(FINITE(count));
        }
        ROUNDS_DONE(s + 1, 12);
        sum += r;
        NAIVE(FINITE(sum));
        ROUND(10);
    }
    ROUNDS_DONE(n, 10);
    out[0] = sum, out[1] = count;
}

/* ---- knapsack: the items a FLAT list (weight, worth: four bytes), the table a DENSE list (best: four
 * bytes, the key its place); whole numbers below 2^31 as integers; the table full (every key there),
 * which the hoisted code checks once */

static void f_knapsack(const uint32_t *in, double *out) {
    uint32_t n = in[0], capacity = in[1];
    uint32_t *weight = malloc(4 * (size_t)n), *worth = malloc(4 * (size_t)n), *best = calloc((size_t)capacity + 1, 4);
    uint64_t *bits = calloc((size_t)capacity / 64 + 1, 8), x = 1, limit = ((size_t)capacity / 64 + 1) * 64;
    APPENDS_OPEN(n, 16);
    for (uint32_t i = 0; i < n; ++i) {
        x = minstd(x), weight[i] = (uint32_t)(x % 1000) + 1;
        x = minstd(x), worth[i] = (uint32_t)(x % 1000);
        APPEND(16);
        ROUND(30);
    }
    APPENDS_OPEN(capacity + 1, 8);
    for (uint32_t c = 0; c <= capacity; ++c) {
        NAIVE(!bit(bits, c));
        bits[c >> 6] |= UINT64_C(1) << (c & 63);
        APPEND(8);
        ROUND(8);
    }
    bool full = true; /* hoisted: every key from 0 to the capacity there (its count is its keys' reach) */
    HOISTED(full);
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t w = weight[i], v = worth[i];
        uint32_t rounds = capacity + 1 > w ? capacity + 1 - w : 0;
        ROUNDS_OPEN(rounds, 20);
        HOISTED((uint64_t)n * 1000 < (UINT64_C(1) << 32)); /* the best fits its column */
        for (uint32_t k = 0; k < rounds; ++k) {
            num c = F((num)capacity - (num)k);
            uint64_t below = KEY(F(c - (num)w)), at = KEY(c);
            NAIVE(below < limit && bit(bits, below));
            num with = F((num)best[below] + (num)v);
            NAIVE(at < limit && bit(bits, at));
            if (with > (num)best[at]) {
                NAIVE(with >= 0 && with < 0x1p32 && with == floor(with)); /* fits its column */
                best[at] = (uint32_t)with;
            }
            ROUND(20);
        }
        ROUNDS_DONE(rounds, 20);
    }
    NAIVE(bit(bits, capacity));
    out[0] = best[capacity];
    free(weight), free(worth), free(best), free(bits);
}

/* ---- sort: the list kept by its numbers appended in order and sorted when walked (the runtime's
 * sort: qsort as C's); a key twice is EXISTS: naive asks a set at each add, hoisted looks at
 * neighbours once sorted */

static uint32_t hash64(uint64_t x) { return (uint32_t)((x * UINT64_C(0x9E3779B97F4A7C15)) >> 32); }

/* Numbers in order (none NaN): a quicksort, the median of three its pivot, small runs by insertion. */
static void sort_numbers(double *v, size_t n) {
    while (n > 24) {
        double a = v[0], b = v[n / 2], c = v[n - 1];
        double pivot = a < b ? (b < c ? b : a < c ? c : a) : (a < c ? a : b < c ? c : b);
        size_t i = 0, j = n - 1;
        for (;;) {
            while (v[i] < pivot) ++i;
            while (v[j] > pivot) --j;
            if (i >= j) break;
            double t = v[i];
            v[i] = v[j], v[j] = t;
            ++i, --j;
        }
        if (j + 1 < n - j - 1) sort_numbers(v, j + 1), v += j + 1, n -= j + 1; /* the smaller side first, the larger in the loop */
        else sort_numbers(v + j + 1, n - j - 1), n = j + 1;
    }
    for (size_t i = 1; i < n; ++i) {
        double t = v[i];
        size_t j = i;
        while (j && v[j - 1] > t) v[j] = v[j - 1], --j;
        v[j] = t;
    }
}

static void f_sort(const uint32_t *in, double *out) {
    uint32_t n = in[0], size = 1;
    while (size < 2 * n) size *= 2;
    double *keys = malloc(8 * (size_t)n);
    uint64_t *set = MODE == 2 ? calloc(size, 8) : NULL, x = 1;
    APPENDS_OPEN(n, 8);
    for (uint32_t i = 0; i < n; ++i) {
        x = minstd(x);
        if (MODE == 2) { /* EXISTS where it falls */
            uint32_t at = hash64(x) & (size - 1);
            while (set[at] && set[at] != x) at = (at + 1) & (size - 1);
            if (set[at]) FAIL();
            set[at] = x;
        }
        keys[i] = (double)x;
        APPEND(8);
        ROUND(14);
    }
    sort_numbers(keys, n); /* the runtime's sort: its comparison inline */
    for (uint32_t i = 1; MODE == 1 && i < n; ++i) HOISTED(keys[i] != keys[i - 1]);
    double before = -1, middle = 0, wrong = 0;
    ROUNDS_OPEN(n, 16);
    for (uint32_t at = 0; at < n; ++at) {
        double k = keys[at];
        if (k < before) wrong += 1;
        if (at == n / 2) middle = k;
        before = k;
        ROUND(16);
    }
    ROUNDS_DONE(n, 16);
    out[0] = keys[0], out[1] = middle, out[2] = keys[n - 1], out[3] = wrong;
    free(keys), free(set);
}

/* ---- join: the customers a FLAT list (id: four bytes, its values below 2^32; region: one), byid a
 * list kept by the sparse ids with a hash index whose slots hold a key and its position (keys of four
 * bytes), totals a DENSE list of 16; a missing id is NOT_FOUND (the probe meets an empty slot). Whole
 * x and a whole divisor: x - floor(x / d) * d is the integers' remainder (below 2^53, d > 0). */

typedef struct { uint32_t key, at; } Slot; /* at: the position + 1, 0 when empty */

static void f_join(const uint32_t *in, double *out) {
    uint32_t customers = in[0], orders = in[1], size = 1;
    while (size < 2 * customers) size *= 2;
    uint32_t *ids = malloc(4 * (size_t)customers);
    uint8_t *region = malloc(customers);
    Slot *slots = calloc(size, sizeof(Slot));
    uint64_t x = 1, totals[16] = {0}, totals_bits = 0;
    APPENDS_OPEN(2 * (uint64_t)customers, 16);
    for (uint32_t i = 0; i < customers; ++i) {
        x = minstd(x), ids[i] = (uint32_t)x, region[i] = (uint8_t)(i % 16);
        APPEND(16);
        uint32_t at = hash32((uint32_t)x) & (size - 1);
        while (slots[at].at && slots[at].key != x) at = (at + 1) & (size - 1);
        NAIVE(!slots[at].at); /* EXISTS */
        slots[at] = (Slot){(uint32_t)x, i + 1};
        APPEND(16);
        ROUND(40);
    }
    for (int r = 0; r < 16; ++r) totals_bits |= UINT64_C(1) << r;
    HOISTED(totals_bits == 0xFFFF); /* totals full: a region below 16 is there */
    ROUNDS_OPEN(orders, 40);
    for (uint32_t o = 0; o < orders; ++o) {
        x = minstd(x);
#if MODE == 2
        num xn = (num)x, cn = F(xn - F((num)floor(F(xn / customers)) * (num)customers));
        num amount = F(xn - F((num)floor(F(xn / 1000)) * 1000));
#else
        num cn = (num)(x % customers), amount = (num)(x % 1000);
#endif
        uint64_t c = KEY(cn);
        NAIVE(c < customers);
        uint32_t id = ids[c], at = hash32(id) & (size - 1);
        while (slots[at].key != id || !slots[at].at) {
            if (!slots[at].at) FAIL(); /* NOT_FOUND: no id there (the probe needs it) */
            at = (at + 1) & (size - 1);
        }
        uint32_t r = region[slots[at].at - 1];
        HOISTED(r < 16);
        NAIVE(totals_bits >> r & 1);
        totals[r] = (uint64_t)F((num)totals[r] + amount);
        ROUND(40);
    }
    ROUNDS_DONE(orders, 40);
    uint64_t most = 0, sum = 0;
    for (int r = 0; r < 16; ++r) most = totals[r] > most ? totals[r] : most, sum += totals[r];
    out[0] = (double)most, out[1] = (double)sum;
    free(ids), free(region), free(slots);
}

/* ---- words: "w{n}" written as Pure writes a whole number (its digits), counts a list kept by text with
 * a hash index, ordered when walked (its words sorted, then the walk) */

static uint32_t text_hash(const char *s, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; ++i) h = (h ^ (unsigned char)s[i]) * 16777619u;
    return h;
}

static size_t digits(char *to, uint32_t v) {
    char tmp[12];
    size_t n = 0;
    do tmp[n++] = (char)('0' + v % 10), v /= 10;
    while (v);
    for (size_t i = 0; i < n; ++i) to[i] = tmp[n - 1 - i];
    return n;
}

typedef struct { char text[16]; uint32_t n; } Word;
static int compare_words(const void *a, const void *b) { return strcmp(((const Word *)a)->text, ((const Word *)b)->text); }

static void f_words(const uint32_t *in, double *out) {
    uint32_t n = in[0], vocabulary = in[1], size = 1, distinct = 0;
    while (size < 2 * vocabulary) size *= 2;
    Word *words = malloc(sizeof(Word) * (size_t)vocabulary);
    uint32_t *slots = calloc(size, 4); /* position + 1 */
    uint64_t x = 1;
    ROUNDS_OPEN(n, 30);
    APPENDS_OPEN(vocabulary, 32);
    for (uint32_t i = 0; i < n; ++i) {
        x = minstd(x);
        char w[16];
        w[0] = 'w';
        size_t len = 1 + digits(w + 1, (uint32_t)(x % vocabulary));
        w[len] = 0;
        uint32_t at = text_hash(w, len) & (size - 1);
        while (slots[at] && strcmp(words[slots[at] - 1].text, w)) at = (at + 1) & (size - 1);
        if (slots[at]) {
            Word *c = &words[slots[at] - 1];
            c->n += 1;
            NAIVE(c->n < (UINT64_C(1) << 32)); /* its column */
        } else {
            memcpy(words[distinct].text, w, len + 1), words[distinct].n = 1;
            slots[at] = ++distinct;
            APPEND(32);
        }
        ROUND(30);
    }
    ROUNDS_DONE(n, 30);
    qsort(words, distinct, sizeof(Word), compare_words); /* walked in the order of its keys */
    uint32_t most = 0;
    for (uint32_t i = 0; i < distinct; ++i) most = words[i].n > most ? words[i].n : most;
    out[0] = distinct, out[1] = most;
    free(words), free(slots);
}

/* ---- matmul: a and b DENSE lists, full, their cells one byte; whole numbers below 2^31 as integers (a
 * product's sum below 200 * 99 * 99): none of it can be -0 or past 2^53 */

static void f_matmul(const uint32_t *in, double *out) {
    uint32_t n = in[0], cells = n * n;
    uint8_t *a = malloc(cells), *b = malloc(cells);
    uint64_t *abits = calloc(cells / 64 + 1, 8), *bbits = calloc(cells / 64 + 1, 8), x = 1, limit = ((uint64_t)cells / 64 + 1) * 64;
    APPENDS_OPEN(2 * (uint64_t)cells, 8);
    for (uint32_t i = 0; i < cells; ++i) {
        x = minstd(x), a[i] = (uint8_t)(x % 100), abits[i >> 6] |= UINT64_C(1) << (i & 63);
        x = minstd(x), b[i] = (uint8_t)(x % 100), bbits[i >> 6] |= UINT64_C(1) << (i & 63);
        APPEND(16);
        ROUND(40);
    }
    HOISTED(true); /* both full: every key below n * n there */
    num total = 0, last = 0;
    ROUNDS_OPEN((uint64_t)n * n * n, 24);
    for (uint32_t i = 0; i < n; ++i)
        for (uint32_t j = 0; j < n; ++j) {
            num s = 0;
            for (uint32_t k = 0; k < n; ++k) {
                uint64_t ka = KEY(F(F((num)i * (num)n) + (num)k)), kb = KEY(F(F((num)k * (num)n) + (num)j));
                NAIVE(ka < limit && bit(abits, ka));
                NAIVE(kb < limit && bit(bbits, kb));
                s = F(s + F((num)a[ka] * (num)b[kb]));
                ROUND(24);
            }
            total = F(total + s), last = s;
        }
    ROUNDS_DONE((uint64_t)n * n * n, 24);
    out[0] = (double)total, out[1] = (double)last;
    free(a), free(b), free(abits), free(bbits);
}

/* ---- bfs: 022's narrow floor without reading ahead: edges a packed list (to: four bytes) with a bucket
 * index (a node's slice its bucket), seen a DENSE list (bits, depth one byte), queue a FLAT list */

static uint32_t *first; /* a node's first edge: the bucket index */
static uint64_t *seen_bits;
static uint8_t *seen_depth;
static uint32_t *queue_node;

static void f_bfs(const uint32_t *in, double *out) {
    const Graph *g = &graph;
    uint32_t nodes = g->nodes, limit = (nodes / 64 + 1) * 64;
    memset(seen_bits, 0, (nodes / 64 + 1) * 8);
    uint32_t count = 0, s = in[0];
    queue_node[count++] = s;
    seen_bits[s >> 6] |= UINT64_C(1) << (s & 63), seen_depth[s] = 0;
    uint32_t reached = 1;
    APPENDS_OPEN(2 * (uint64_t)nodes, 16);
    ROUNDS_OPEN(nodes, 30);
    uint32_t i = 0;
    for (; i < nodes; ++i) {
        if (i >= count) break;
        NAIVE(i < count);
        uint32_t q = queue_node[i];
        uint32_t lo = first[q], hi = first[q + 1];
        ROUNDS_OPEN(hi - lo, 17);
        for (uint32_t e = lo; e < hi; ++e) {
            uint32_t to = g->to[e];
            NAIVE(to < limit);
            if (!bit(seen_bits, to)) {
                NAIVE(bit(seen_bits, q));
                uint32_t d = seen_depth[q] + 1u;
                HOISTED(d < 256); /* depth's column one byte: widened past it */
                NAIVE(to < limit && !bit(seen_bits, to));
                seen_bits[to >> 6] |= UINT64_C(1) << (to & 63), seen_depth[to] = (uint8_t)d;
                queue_node[count++] = to;
                ++reached;
                APPEND(32);
            }
            ROUND(17);
        }
        ROUNDS_DONE(hi - lo, 17);
        ROUND(30);
    }
    ROUNDS_DONE(i, 30);
    uint32_t t = in[1];
    out[0] = bit(seen_bits, t) ? seen_depth[t] : -1, out[1] = reached;
}

typedef struct {
    const char *name;
    uint32_t args[2];
    unsigned answers;
    void (*floor)(const uint32_t *, double *), (*c)(const uint32_t *, double *);
} Work;

static const Work works[] = {
    {"roots", {100000}, 2, f_roots, c_roots},
    {"knapsack", {100, 50000}, 1, f_knapsack, c_knapsack},
    {"sort", {200000}, 4, f_sort, c_sort},
    {"join", {100000, 1000000}, 2, f_join, c_join},
    {"words", {300000, 50000}, 2, f_words, c_words},
    {"matmul", {200}, 2, f_matmul, c_matmul},
    {"bfs", {0, 0}, 2, f_bfs, c_bfs},
};

int main(int argc, char **argv) {
    if (argc < 2) { fputs("floor ROUNDS [NAME...]\n", stderr); return 2; }
    unsigned rounds = (unsigned)atoi(argv[1]);
    (void)c_bfs_ahead; /* suite-run's */
    uint64_t *keys;
    size_t edges = graph_build(100000, 1000000, &keys);
    first = calloc(100001 + 1, 4), seen_bits = calloc(100000 / 64 + 1, 8), seen_depth = malloc(100000), queue_node = malloc(4 * 100000);
    for (size_t i = 0; i < edges; ++i) ++first[(keys[i] >> 20) + 1];
    for (uint32_t v = 0; v < 100001; ++v) first[v + 1] += first[v];
    free(keys);
    int failed_any = 0;
    for (size_t w = 0; w < sizeof(works) / sizeof(*works); ++w) {
        bool wanted = argc == 2;
        for (int a = 2; a < argc; ++a) wanted = wanted || !strcmp(argv[a], works[w].name);
        if (!wanted) continue;
        double *f_ms = malloc(rounds * 8), *c_ms = malloc(rounds * 8);
        bool agree = true;
        for (unsigned r = 0; r < rounds; ++r) {
            uint32_t args[2] = {works[w].args[0], works[w].args[1]};
            if (!strcmp(works[w].name, "bfs")) args[0] = (uint32_t)(graph_next() % 100000), args[1] = (uint32_t)(graph_next() % 100000);
            double fo[4] = {0}, co[4] = {0};
            work = grown = 0;
            double a = now_ms();
            if (setjmp(failed)) {
                fprintf(stderr, "%s: failed\n", works[w].name);
                return 1;
            }
            works[w].floor(args, fo);
            double b = now_ms();
            works[w].c(args, co);
            double c = now_ms();
            f_ms[r] = b - a, c_ms[r] = c - b;
            bool same = true;
            for (unsigned i = 0; i < works[w].answers; ++i) same = same && fo[i] == co[i];
            agree = agree && same;
            printf("{\"mode\":%d,\"work\":\"%s\",\"round\":%u,\"floor_ms\":%.3f,\"c_ms\":%.3f,\"same\":%s}\n", MODE, works[w].name, r,
                   f_ms[r], c_ms[r], same ? "true" : "false");
        }
        qsort(f_ms, rounds, 8, compare_doubles), qsort(c_ms, rounds, 8, compare_doubles);
        printf("{\"mode\":%d,\"work\":\"%s\",\"floor_median_ms\":%.3f,\"c_median_ms\":%.3f,\"ratio\":%.3f,\"agree\":%s}\n", MODE,
               works[w].name, f_ms[rounds / 2], c_ms[rounds / 2], f_ms[rounds / 2] / c_ms[rounds / 2], agree ? "true" : "false");
        fflush(stdout);
        failed_any |= !agree;
        free(f_ms), free(c_ms);
    }
    return failed_any;
}
