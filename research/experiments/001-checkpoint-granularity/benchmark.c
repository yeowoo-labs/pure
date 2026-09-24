#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Experimental apparatus, not the Pure runtime. All arithmetic is modulo 2^64. */
typedef struct { uint32_t salt, control; } Op;
typedef struct {
    uint32_t chain, index;
    uint64_t mask;
} Change;
typedef struct {
    uint32_t chains, length, block, slots;
    Op *ops;
    uint64_t *inputs, *cache;
    uint32_t *first, *last;
} Engine;
typedef struct {
    uint32_t ticks, per_tick;
    Change *changes;
} Trace;

static void die(const char *s) { fprintf(stderr, "%s\n", s); exit(1); }
static void require(int condition, const char *s) { if (!condition) die(s); }
static void *allocate(size_t count, size_t size) {
    if (size && count > SIZE_MAX / size) die("allocation size overflow");
    void *p = calloc(count, size);
    if (!p) die("allocation failed");
    return p;
}
static uint64_t random64(uint64_t *s) {
    uint64_t z = (*s += UINT64_C(0x9e3779b97f4a7c15));
    z = (z ^ (z >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94d049bb133111eb);
    return z ^ (z >> 31);
}
static uint64_t rotate(uint64_t x, uint32_t n) {
    return (x << n) | (x >> (64 - n));
}
static uint64_t evaluate(Op op, uint64_t x) {
    switch (op.control & 3u) {
    case 0: return x + op.salt;
    case 1: return x ^ ((uint64_t)op.salt << 17);
    case 2: return x * ((uint64_t)op.salt | 1u);
    default: return rotate(x, 1u + ((op.control >> 2) % 63u)) ^ op.salt;
    }
}
static uint64_t now_ns(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) die("clock_gettime failed");
    return (uint64_t)t.tv_sec * UINT64_C(1000000000) + (uint64_t)t.tv_nsec;
}
static uint64_t output(const Engine *e, uint32_t c) {
    return e->cache[(size_t)c * e->slots + e->slots - 1];
}
static uint64_t reference_output(const Engine *e, uint32_t c) {
    uint64_t x = e->inputs[c];
    for (uint32_t n = 0; n < e->length; ++n)
        x = evaluate(e->ops[(size_t)c * e->length + n], x);
    return x;
}
static Engine create_engine(uint32_t chains, uint32_t length,
                            uint32_t block, uint64_t seed) {
    Engine e = { .chains = chains, .length = length, .block = block,
                 .slots = (length + block - 1) / block };
    e.ops = allocate((size_t)chains * length, sizeof(*e.ops));
    e.inputs = allocate(chains, sizeof(*e.inputs));
    e.cache = allocate((size_t)chains * e.slots, sizeof(*e.cache));
    e.first = allocate(chains, sizeof(*e.first));
    e.last = allocate(chains, sizeof(*e.last));
    for (uint32_t c = 0; c < chains; ++c) {
        e.inputs[c] = random64(&seed);
        uint64_t x = e.inputs[c];
        for (uint32_t n = 0; n < length; ++n) {
            Op *op = &e.ops[(size_t)c * length + n];
            op->salt = (uint32_t)random64(&seed);
            op->control = (uint32_t)random64(&seed);
            x = evaluate(*op, x);
            if ((n + 1) % block == 0 || n + 1 == length)
                e.cache[(size_t)c * e.slots + n / block] = x;
        }
        e.first[c] = length;
    }
    return e;
}
static void destroy_engine(Engine *e) {
    free(e->ops); free(e->inputs); free(e->cache);
    free(e->first); free(e->last);
}
static void apply(Engine *e, Change change) {
    uint32_t c = change.chain, n = change.index;
    require(c < e->chains && n <= e->length, "invalid change");
    if (!change.mask) return;
    if (n == e->length) {
        e->inputs[c] ^= change.mask;
        n = 0;
    } else {
        Op *op = &e->ops[(size_t)c * e->length + n];
        op->salt ^= (uint32_t)change.mask;
        op->control ^= (uint32_t)(change.mask >> 32);
    }
    if (n < e->first[c]) e->first[c] = n;
    if (n > e->last[c]) e->last[c] = n;
}
static uint64_t update(Engine *e) {
    uint64_t work = 0;
    for (uint32_t c = 0; c < e->chains; ++c) {
        uint32_t first = e->first[c], last = e->last[c];
        if (first == e->length) continue;
        const Op *ops = e->ops + (size_t)c * e->length;
        uint64_t *cache = e->cache + (size_t)c * e->slots;
        /* Full and fine have dedicated loops: no block-loop penalty for B=1. */
        if (e->block == e->length) {
            uint64_t x = e->inputs[c];
            for (uint32_t n = 0; n < e->length; ++n) x = evaluate(ops[n], x);
            cache[0] = x;
            work += e->length;
        } else if (e->block == 1) {
            uint64_t x = first ? cache[first - 1] : e->inputs[c];
            uint32_t n = first;
            while (n < e->length) {
                uint64_t old = cache[n];
                cache[n] = x = evaluate(ops[n], x);
                ++n;
                /* Do not skip a later edit in the same batch. */
                if (n > last && x == old) break;
            }
            work += n - first;
        } else {
            uint32_t start = first / e->block;
            uint64_t x = start ? cache[start - 1] : e->inputs[c];
            for (uint32_t b = start; b < e->slots; ++b) {
                uint32_t begin = b * e->block, end = begin + e->block;
                if (end > e->length) end = e->length;
                for (uint32_t n = begin; n < end; ++n) x = evaluate(ops[n], x);
                uint64_t old = cache[b];
                cache[b] = x;
                work += end - begin;
                if (end > last && x == old) break;
            }
        }
        e->first[c] = e->length;
        e->last[c] = 0;
    }
    return work;
}
static uint64_t checksum(const Engine *e) {
    uint64_t h = UINT64_C(0xcbf29ce484222325);
    for (uint32_t c = 0; c < e->chains; ++c)
        h = (h ^ output(e, c)) * UINT64_C(0x100000001b3);
    return h;
}
static Trace create_trace(const char *kind, uint32_t chains, uint32_t length,
                          uint32_t ticks, uint64_t seed) {
    uint32_t per_tick = 1;
    int roots = !strcmp(kind, "roots");
    int dense = !strcmp(kind, "dense");
    int uniform = !strcmp(kind, "uniform");
    int tail = !strcmp(kind, "tail");
    require(roots || dense || uniform || tail, "unknown workload");
    if (roots) per_tick = chains;
    if (dense) per_tick = chains * 8;
    Trace t = { .ticks = ticks, .per_tick = per_tick };
    t.changes = allocate((size_t)ticks * per_tick, sizeof(*t.changes));
    for (uint32_t i = 0; i < ticks; ++i) {
        for (uint32_t k = 0; k < per_tick; ++k) {
            Change *p = &t.changes[(size_t)i * per_tick + k];
            p->chain = roots ? k : dense ? k / 8 : (uint32_t)(random64(&seed) % chains);
            p->index = roots ? length : (uint32_t)(random64(&seed) % length);
            if (tail) {
                uint32_t width = length < 8 ? length : 8;
                p->index = length - width + p->index % width;
            }
            /* XOR changes keep every replay active; fixed assignments would not. */
            p->mask = random64(&seed) | 1u;
        }
    }
    return t;
}
static uint64_t replay(Engine *e, const Trace *t) {
    uint64_t work = 0;
    for (uint32_t i = 0; i < t->ticks; ++i) {
        for (uint32_t k = 0; k < t->per_tick; ++k)
            apply(e, t->changes[(size_t)i * t->per_tick + k]);
        work += update(e);
    }
    return work;
}
static size_t owned_bytes(const Engine *e) {
    return sizeof(*e) + (size_t)e->chains * e->length * sizeof(*e->ops)
        + (size_t)e->chains * (sizeof(*e->inputs) + sizeof(*e->first) + sizeof(*e->last))
        + (size_t)e->chains * e->slots * sizeof(*e->cache);
}
static void check_outputs(const Engine *e) {
    for (uint32_t c = 0; c < e->chains; ++c)
        require(output(e, c) == reference_output(e, c), "output disagrees with full reference");
}
static void self_test(void) {
    const uint32_t lengths[] = {1, 2, 7, 17, 65, 257};
    const uint32_t blocks[] = {1, 2, 4, 16, 64, 256};
    uint64_t checked_ticks = 0;
    for (uint64_t seed = 1; seed <= 8; ++seed) {
        for (size_t l = 0; l < sizeof(lengths) / sizeof(*lengths); ++l) {
            uint32_t length = lengths[l];
            for (size_t b = 0; b <= sizeof(blocks) / sizeof(*blocks); ++b) {
                uint32_t block = b == sizeof(blocks) / sizeof(*blocks) ? length : blocks[b];
                if (block > length) continue;
                Engine e = create_engine(3, length, block, seed);
                check_outputs(&e);
                uint64_t rng = seed;
                for (uint32_t tick = 0; tick < 500; ++tick) {
                    uint32_t count = (uint32_t)(random64(&rng) % 9);
                    for (uint32_t k = 0; k < count; ++k) {
                        Change change = {
                            .chain = (uint32_t)(random64(&rng) % e.chains),
                            .index = (uint32_t)(random64(&rng) % (length + 1)),
                            .mask = random64(&rng)
                        };
                        if (tick % 11 == 0) change.mask = 0;
                        apply(&e, change);
                        if (tick % 13 == 0) apply(&e, change); /* cancelling edits */
                    }
                    (void)update(&e);
                    check_outputs(&e);
                    require(update(&e) == 0, "clean update performed work");
                    ++checked_ticks;
                }
                destroy_engine(&e);
            }
        }
    }
    /* An unchanged early checkpoint must not suppress a later dirty operation. */
    for (uint32_t block = 1; block <= 4; block *= 2) {
        Engine e = create_engine(1, 8, block, 77);
        Change cancel = {0, 0, 123}, later = {0, 7, 987};
        apply(&e, cancel); apply(&e, cancel); apply(&e, later);
        (void)update(&e); check_outputs(&e); destroy_engine(&e);
    }
    printf("{\"checked_ticks\":%" PRIu64 ",\"status\":\"passed\"}\n", checked_ticks);
}
static uint32_t number(const char *s, uint32_t max) {
    char *end;
    errno = 0;
    unsigned long n = strtoul(s, &end, 10);
    if (errno || end == s || *end || !n || n > max) die("invalid positive integer");
    return (uint32_t)n;
}
int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--check")) { self_test(); return 0; }
    if (argc != 8) die("usage: benchmark CHAINS LENGTH BLOCK WORKLOAD TICKS REPLAYS SEED | --check");
    uint32_t chains = number(argv[1], 1024), length = number(argv[2], 65536);
    uint32_t block = number(argv[3], length), ticks = number(argv[5], 4096);
    uint32_t replays = number(argv[6], 4096), seed = number(argv[7], UINT32_MAX);
    require((uint64_t)chains * length <= UINT64_C(1048576), "model too large for this lab");
    Engine e = create_engine(chains, length, block, seed);
    Trace t = create_trace(argv[4], chains, length, ticks, seed + UINT64_C(1234567));
    (void)replay(&e, &t); (void)replay(&e, &t); /* two warm-up replays */
    uint64_t work = 0, start = now_ns();
    for (uint32_t r = 0; r < replays; ++r) work += replay(&e, &t);
    uint64_t elapsed = now_ns() - start;
    check_outputs(&e); /* correctness check is outside the measured interval */
    printf("{\"elapsed_ns\":%" PRIu64 ",\"ticks\":%" PRIu64
           ",\"node_evaluations\":%" PRIu64 ",\"core_owned_bytes\":%zu"
           ",\"model_bytes\":%zu,\"cache_bytes\":%zu,\"engine_bytes\":%zu"
           ",\"trace_bytes\":%zu,\"checksum\":\"%016" PRIx64 "\"}\n",
           elapsed, (uint64_t)ticks * replays, work, owned_bytes(&e),
           (size_t)chains * length * sizeof(Op),
           (size_t)chains * e.slots * sizeof(uint64_t), sizeof(Engine),
           sizeof(t) + (size_t)t.ticks * t.per_tick * sizeof(Change), checksum(&e));
    free(t.changes); destroy_engine(&e);
    return 0;
}
