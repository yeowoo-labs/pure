/* The floor before building more (research 123): the whole breadth first
 * search of 033's graph.aup as Pure could run it at best, written by hand in
 * C, against 033's bfs.c on the same graph (bfs-run's) in the same run.
 *
 *  c        bfs.c's loop: offsets (u32), to (u32), depth (int32), queue (u32)
 *  c_bits   bfs.c with what it has seen as bits, depth written only when seen
 *           (021's context: where Pure's share would come from)
 *  packed   Pure's representations, every check kept: edges a stored keyed
 *           list packed as columns (key, to: doubles) with a bucket index
 *           (the first position at or past each bucket's start; buckets a
 *           power of two of keys wide, starts aligned); seen a dense event
 *           list (key bits, node and depth columns by key, count); queue a
 *           flat event list (node column, count); the work charged as the
 *           executor does (EACH: the block's length times its items; LOOP:
 *           each round)
 *  ahead    packed, and while taking node i, the bucket of node i + FAR and
 *           the first edge of node i + NEAR are read ahead (prefetch)
 *  narrow   packed with columns as narrow as their values: to and the queue's
 *           node u32, the bucket index u32, seen's depth u8 and no node
 *           column (a dense list's key column is its keys)
 *  narrow_ahead  narrow, read ahead as ahead
 *  c_ahead  bfs.c read ahead as ahead (context: what C would gain)
 *
 * bfsfloor NODES EDGES SEARCHES [FAR NEAR]: a JSON line per search and form
 * (the forms in turns; the answers must agree), with this thread's counters
 * (user space), then the medians. */
#define _GNU_SOURCE
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <linux/perf_event.h>
#include <sys/syscall.h>
#include <unistd.h>

/* Counters of this thread, user space only: instructions, cycles, branch misses, L1D read misses, LLC read misses. */
enum { COUNTERS = 5 };
static int counter[COUNTERS];
static void counters_open(void) {
    static const struct { uint32_t type; uint64_t config; } what[COUNTERS] = {
        {PERF_TYPE_HARDWARE, PERF_COUNT_HW_INSTRUCTIONS}, {PERF_TYPE_HARDWARE, PERF_COUNT_HW_CPU_CYCLES},
        {PERF_TYPE_HARDWARE, PERF_COUNT_HW_BRANCH_MISSES},
        {PERF_TYPE_HW_CACHE, PERF_COUNT_HW_CACHE_L1D | PERF_COUNT_HW_CACHE_OP_READ << 8 | PERF_COUNT_HW_CACHE_RESULT_MISS << 16},
        {PERF_TYPE_HW_CACHE, PERF_COUNT_HW_CACHE_LL | PERF_COUNT_HW_CACHE_OP_READ << 8 | PERF_COUNT_HW_CACHE_RESULT_MISS << 16}};
    for (int i = 0; i < COUNTERS; ++i) {
        struct perf_event_attr a = {.size = sizeof(a), .type = what[i].type, .config = what[i].config, .exclude_kernel = 1,
                                    .exclude_hv = 1};
        counter[i] = (int)syscall(SYS_perf_event_open, &a, 0, -1, -1, 0);
    }
}
static void counters_read(uint64_t *v) {
    for (int i = 0; i < COUNTERS; ++i) if (counter[i] < 0 || read(counter[i], &v[i], 8) != 8) v[i] = 0;
}

static uint64_t state = 121;
static uint64_t next(void) { state ^= state << 13; state ^= state >> 7; state ^= state << 17; return state; }
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static int by_key(const void *a, const void *b) { uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b; return (x > y) - (x < y); }
static int by_num(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return (x > y) - (x < y); }

typedef struct { int hops; uint32_t reached; uint64_t work; } Answer;

/* ---- bfs.c */
static uint32_t n, m, *start, *to32, *queue32;
static int32_t *depth32;

static Answer search_c(uint32_t s, uint32_t t) {
    for (uint32_t v = 0; v < n; ++v) depth32[v] = -1;
    uint32_t head = 0, tail = 0;
    queue32[tail++] = s;
    depth32[s] = 0;
    while (head < tail) {
        uint32_t v = queue32[head++];
        for (uint32_t k = start[v]; k < start[v + 1]; ++k)
            if (depth32[to32[k]] < 0) { depth32[to32[k]] = depth32[v] + 1; queue32[tail++] = to32[k]; }
    }
    return (Answer){depth32[t], tail, 0};
}

static uint64_t *seen_bits;

static Answer search_c_bits(uint32_t s, uint32_t t) {
    memset(seen_bits, 0, (n + 63) / 64 * 8);
    uint32_t head = 0, tail = 0;
    queue32[tail++] = s;
    seen_bits[s >> 6] |= 1ull << (s & 63), depth32[s] = 0;
    while (head < tail) {
        uint32_t v = queue32[head++];
        int32_t d = depth32[v] + 1;
        for (uint32_t k = start[v]; k < start[v + 1]; ++k) {
            uint32_t w = to32[k];
            if (!(seen_bits[w >> 6] >> (w & 63) & 1)) { seen_bits[w >> 6] |= 1ull << (w & 63), depth32[w] = d; queue32[tail++] = w; }
        }
    }
    return (Answer){seen_bits[t >> 6] >> (t & 63) & 1 ? depth32[t] : -1, tail, 0};
}

/* ---- Pure's representations */
typedef struct { size_t count; double *key, *to; double low, scale; size_t buckets; uint32_t *first; } Packed;
typedef struct { size_t count, keys; uint64_t *bits; double *node, *depth; } Dense;
typedef struct { size_t count, room; double *node; } Flat;
static Packed edges;
static Dense seen;
static Flat queue;
enum { EACH_LENGTH = 23, LOOP_LENGTH = 30 }; /* the blocks' lengths, as graph.aup compiles (the charge is what matters, not the value) */

/* The first position whose key is not below key: the bucket's first, then on while below. */
static inline __attribute__((always_inline)) size_t seek(const Packed *p, double key) {
    double at = (key - p->low) * p->scale;
    if (!(at >= 0)) at = 0;
    if (at >= (double)p->buckets) return p->count;
    size_t b = (size_t)at, pos = p->first[b];
    if ((double)b == at) return pos; /* a bucket's start: its first is the answer */
    while (pos < p->count && p->key[pos] < key) ++pos;
    return pos;
}

static inline __attribute__((always_inline)) bool small(double key, size_t keys, size_t *k) {
    if (!(key >= 0 && key < (double)keys)) return false;
    *k = (size_t)key;
    return (double)*k == key;
}

static void fault(const char *what) { fprintf(stderr, "fault: %s\n", what); exit(1); }

static inline __attribute__((always_inline)) Answer search_packed(uint32_t s, uint32_t t, int far, int near) {
    uint64_t work = 0;
    memset(seen.bits, 0, (seen.keys + 63) / 64 * 8), seen.count = 0, queue.count = 0; /* the event's lists, cleared */
    queue.node[queue.count++] = s;
    seen.bits[s >> 6] |= 1ull << (s & 63), seen.node[s] = s, seen.depth[s] = 0, ++seen.count;
    double nodes = n;
    for (double i = 0; i < nodes; ++i) {
        work += LOOP_LENGTH;
        if (i >= (double)queue.count) break;
        size_t at = (size_t)i;
        if (far) {
            size_t ahead = at + (size_t)far, closer = at + (size_t)near;
            if (ahead < queue.count) {
                double a = (queue.node[ahead] * 1048576 - edges.low) * edges.scale;
                if (a >= 0 && a < (double)edges.buckets) __builtin_prefetch(&edges.first[(size_t)a]);
            }
            if (closer < queue.count) {
                double a = (queue.node[closer] * 1048576 - edges.low) * edges.scale;
                if (a >= 0 && a < (double)edges.buckets) __builtin_prefetch(&edges.to[edges.first[(size_t)a]]);
            }
        }
        double node = queue.node[at];
        double lo = node * 1048576, hi = node * 1048576 + 1048576;
        size_t from = seek(&edges, lo), until = seek(&edges, hi);
        if (until < from) until = from;
        work += (uint64_t)EACH_LENGTH * (until - from);
        size_t q;
        if (!small(node, seen.keys, &q) || !(seen.bits[q >> 6] >> (q & 63) & 1)) fault("absent");
        double d = seen.depth[q] + 1; /* hoisted: nothing in the loop changes seen[q.node] before it is read */
        for (size_t e = from; e < until; ++e) {
            double key = edges.to[e];
            size_t k;
            if (!small(key, seen.keys, &k)) fault("key"); /* the runtime: another form, not a fault */
            if (seen.bits[k >> 6] >> (k & 63) & 1) continue;
            seen.bits[k >> 6] |= 1ull << (k & 63), seen.node[k] = key, seen.depth[k] = d, ++seen.count;
            if (queue.count == queue.room) fault("room");
            queue.node[queue.count++] = key;
        }
    }
    int hops = (seen.bits[t >> 6] >> (t & 63) & 1) ? (int)seen.depth[t] : -1;
    return (Answer){hops, (uint32_t)seen.count, work};
}

/* ---- narrow columns */
typedef struct { size_t count; double *key; uint32_t *to; double low, scale; size_t buckets; uint32_t *first; } Narrow;
typedef struct { size_t count, keys; uint64_t *bits; uint8_t *depth; } NarrowDense;
typedef struct { size_t count, room; uint32_t *node; } NarrowFlat;
static Narrow nedges;
static NarrowDense nseen;
static NarrowFlat nqueue;

static inline __attribute__((always_inline)) size_t nseek(const Narrow *p, double key) {
    double at = (key - p->low) * p->scale;
    if (!(at >= 0)) at = 0;
    if (at >= (double)p->buckets) return p->count;
    size_t b = (size_t)at, pos = p->first[b];
    if ((double)b == at) return pos;
    while (pos < p->count && p->key[pos] < key) ++pos;
    return pos;
}

static inline __attribute__((always_inline)) Answer search_narrow(uint32_t s, uint32_t t, int far, int near) {
    uint64_t work = 0;
    memset(nseen.bits, 0, (nseen.keys + 63) / 64 * 8), nseen.count = 0, nqueue.count = 0;
    nqueue.node[nqueue.count++] = s;
    nseen.bits[s >> 6] |= 1ull << (s & 63), nseen.depth[s] = 0, ++nseen.count;
    double nodes = n;
    for (double i = 0; i < nodes; ++i) {
        work += LOOP_LENGTH;
        if (i >= (double)nqueue.count) break;
        size_t at = (size_t)i;
        if (far) {
            size_t ahead = at + (size_t)far, closer = at + (size_t)near;
            if (ahead < nqueue.count) {
                double a = ((double)nqueue.node[ahead] * 1048576 - nedges.low) * nedges.scale;
                if (a >= 0 && a < (double)nedges.buckets) __builtin_prefetch(&nedges.first[(size_t)a]);
            }
            if (closer < nqueue.count) {
                double a = ((double)nqueue.node[closer] * 1048576 - nedges.low) * nedges.scale;
                if (a >= 0 && a < (double)nedges.buckets) __builtin_prefetch(&nedges.to[nedges.first[(size_t)a]]);
            }
        }
        uint32_t q = nqueue.node[at];
        double node = q;
        double lo = node * 1048576, hi = node * 1048576 + 1048576;
        size_t from = nseek(&nedges, lo), until = nseek(&nedges, hi);
        if (until < from) until = from;
        work += (uint64_t)EACH_LENGTH * (until - from);
        if (q >= nseen.keys || !(nseen.bits[q >> 6] >> (q & 63) & 1)) fault("absent");
        unsigned d = nseen.depth[q] + 1u;
        if (d > 255) fault("wider"); /* the runtime: the column made wider */
        for (size_t e = from; e < until; ++e) {
            uint32_t k = nedges.to[e];
            if (k >= nseen.keys) fault("key");
            if (nseen.bits[k >> 6] >> (k & 63) & 1) continue;
            nseen.bits[k >> 6] |= 1ull << (k & 63), nseen.depth[k] = (uint8_t)d, ++nseen.count;
            if (nqueue.count == nqueue.room) fault("room");
            nqueue.node[nqueue.count++] = k;
        }
    }
    int hops = (nseen.bits[t >> 6] >> (t & 63) & 1) ? (int)nseen.depth[t] : -1;
    return (Answer){hops, (uint32_t)nseen.count, work};
}
static Answer narrow_plain(uint32_t s, uint32_t t) { return search_narrow(s, t, 0, 0); }
static Answer narrow_ahead(uint32_t s, uint32_t t);

static Answer search_c_ahead(uint32_t s, uint32_t t);

static Answer packed_plain(uint32_t s, uint32_t t) { return search_packed(s, t, 0, 0); }
static int far_, near_;
static Answer packed_ahead(uint32_t s, uint32_t t) { return search_packed(s, t, far_, near_); }
static Answer narrow_ahead(uint32_t s, uint32_t t) { return search_narrow(s, t, far_, near_); }

static Answer search_c_ahead(uint32_t s, uint32_t t) {
    for (uint32_t v = 0; v < n; ++v) depth32[v] = -1;
    uint32_t head = 0, tail = 0;
    queue32[tail++] = s;
    depth32[s] = 0;
    while (head < tail) {
        if (head + (uint32_t)far_ < tail) __builtin_prefetch(&start[queue32[head + (uint32_t)far_]]);
        if (head + (uint32_t)near_ < tail) __builtin_prefetch(&to32[start[queue32[head + (uint32_t)near_]]]);
        uint32_t v = queue32[head++];
        for (uint32_t k = start[v]; k < start[v + 1]; ++k)
            if (depth32[to32[k]] < 0) { depth32[to32[k]] = depth32[v] + 1; queue32[tail++] = to32[k]; }
    }
    return (Answer){depth32[t], tail, 0};
}

int main(int argc, char **argv) {
    if (argc < 4) { fputs("bfsfloor NODES EDGES SEARCHES [FAR NEAR]\n", stderr); return 2; }
    n = (uint32_t)atol(argv[1]);
    size_t want = (size_t)atol(argv[2]), searches = (size_t)atol(argv[3]);
    far_ = argc > 4 ? atoi(argv[4]) : 16, near_ = argc > 5 ? atoi(argv[5]) : 8;
    /* bfs-run's graph: from and to uniform, no loops, each pair once, sorted */
    uint64_t *keys = malloc(want * sizeof(*keys));
    size_t count = 0;
    while (count < want) {
        uint64_t a = next() % n, b = next() % n;
        if (a != b) keys[count++] = a << 20 | b;
    }
    qsort(keys, count, sizeof(*keys), by_key);
    m = 0;
    for (size_t i = 0; i < count; ++i)
        if (!m || keys[i] != keys[m - 1]) keys[m++] = keys[i];
    seen_bits = calloc((n + 63) / 64, 8), start = calloc(n + 1, 4), to32 = malloc(4ull * m), depth32 = malloc(4ull * n), queue32 = malloc(4ull * n);
    for (uint32_t k = 0; k < m; ++k) ++start[(keys[k] >> 20) + 1], to32[k] = (uint32_t)(keys[k] & 0xFFFFF);
    for (uint32_t v = 0; v < n; ++v) start[v + 1] += start[v];
    /* packed: about eight items a bucket, a power of two wide, starts at multiples of the width */
    edges.count = m, edges.key = malloc(8ull * m), edges.to = malloc(8ull * m);
    for (size_t k = 0; k < m; ++k) edges.key[k] = (double)keys[k], edges.to[k] = (double)(keys[k] & 0xFFFFF);
    double span = edges.key[m - 1] - edges.key[0];
    int exponent;
    frexp(span / (double)(m / 8 + 1), &exponent);
    edges.scale = ldexp(1.0, -exponent);
    edges.low = floor(edges.key[0] * edges.scale) / edges.scale;
    edges.buckets = (size_t)floor((edges.key[m - 1] - edges.low) * edges.scale) + 2;
    edges.first = malloc(4 * edges.buckets);
    size_t filled = 0;
    for (size_t k = 0; k < m; ++k) {
        size_t b = (size_t)floor((edges.key[k] - edges.low) * edges.scale);
        while (filled <= b) edges.first[filled++] = (uint32_t)k;
    }
    while (filled < edges.buckets) edges.first[filled++] = (uint32_t)m;
    seen.keys = n, seen.bits = calloc((n + 63) / 64, 8), seen.node = malloc(8ull * n), seen.depth = malloc(8ull * n);
    queue.room = n, queue.node = malloc(8ull * n);
    nedges = (Narrow){edges.count, edges.key, malloc(4ull * m), edges.low, edges.scale, edges.buckets, edges.first};
    for (size_t k = 0; k < m; ++k) nedges.to[k] = (uint32_t)edges.to[k];
    nseen.keys = n, nseen.bits = calloc((n + 63) / 64, 8), nseen.depth = malloc(n);
    nqueue.room = n, nqueue.node = malloc(4ull * n);
    fprintf(stderr, "edges %u, buckets %zu (%zu KB, %g keys wide)\n", m, edges.buckets, edges.buckets * 4 / 1024,
            1 / edges.scale);
    enum { FORMS = 7 };
    counters_open();
    const char *names[] = {"c", "c_bits", "packed", "ahead", "narrow", "narrow_ahead", "c_ahead"};
    Answer (*forms[])(uint32_t, uint32_t) = {search_c, search_c_bits, packed_plain, packed_ahead, narrow_plain, narrow_ahead,
                                             search_c_ahead};
    double *took[FORMS];
    for (int f = 0; f < FORMS; ++f) took[f] = malloc(searches * sizeof(double));
    for (size_t r = 0; r < searches; ++r) {
        uint32_t s = (uint32_t)(next() % n), t = (uint32_t)(next() % n);
        Answer first = {0};
        for (int j = 0; j < FORMS; ++j) {
            int f = (int)((j + r) % FORMS); /* each form first in turn */
            uint64_t c0[COUNTERS], c1[COUNTERS];
            counters_read(c0);
            double a = now();
            Answer got = forms[f](s, t);
            took[f][r] = now() - a;
            counters_read(c1);
            if (!j) first = got;
            else if (got.hops != first.hops || got.reached != first.reached) { fprintf(stderr, "answers differ\n"); return 1; }
            printf("{\"form\":\"%s\",\"from\":%u,\"to\":%u,\"hops\":%d,\"reached\":%u,\"ms\":%.3f,\"instructions\":%llu,"
                   "\"cycles\":%llu,\"branch_misses\":%llu,\"l1d_misses\":%llu,\"llc_misses\":%llu,\"work\":%llu}\n", names[f], s, t,
                   got.hops, got.reached, took[f][r], (unsigned long long)(c1[0] - c0[0]), (unsigned long long)(c1[1] - c0[1]),
                   (unsigned long long)(c1[2] - c0[2]), (unsigned long long)(c1[3] - c0[3]), (unsigned long long)(c1[4] - c0[4]),
                   (unsigned long long)got.work);
        }
    }
    printf("{\"nodes\":%u,\"edges\":%u,\"far\":%d,\"near\":%d", n, m, far_, near_);
    for (int f = 0; f < FORMS; ++f) {
        qsort(took[f], searches, sizeof(double), by_num);
        printf(",\"%s_ms\":%.3f", names[f], took[f][searches / 2]);
    }
    puts("}");
    return 0;
}
