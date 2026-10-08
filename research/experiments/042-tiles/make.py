#!/usr/bin/env python3
"""Research 132's deciding experiment: a register-tile interpreter (tiles.c).

A body's instructions run over a tile of T lanes held in vector registers
(GCC vector types), on a stack of DEPTH vector slots whose depth is known
per instruction (each operation has one label a depth: the C compiler sees
fixed variables, never an indexed array), dispatched as threaded code
(computed goto). No code is made at run time. Three bodies: knapsack's
inner loop, BFS's frontier (an edge a lane), roots' Newton steps with lanes
leaving. Against the same bodies as one pass per operation over 1024
lanes (the batch executor's shape) and as plain C.

    python3 make.py > tiles.c
    cc -O2 -DT=8 -o tiles-8 tiles.c -lm   (T 4, 8, 16)
    ./tiles-8 BODY ROUNDS                 (knapsack | bfs | roots | all)

The bar (132): cycles an element within 1.3x the compiled code's on all
three, measured with 041's icount over ROUNDS 1 and 11.
"""
DEPTH = 6
ops = []  # (name, pops, pushes, body as a format with {d}, {a}, {b}, {c} slot names)


def op(name, pops, pushes, body, needs=None):
    ops.append((name, pops, pushes, body, needs))


# stack: s0 .. s{DEPTH-1}; at depth d the top is s{d-1}
op('PUSHK', 0, 1, 's{t} = (vd){{}} + ip->k;')
op('PUSH_AFFINE', 0, 1, 's{t} = (vd){{}} + ip->k + (lanes_v + base) * ip->k2;')  # k + step * (tile base + lane)
op('PUSH_ALL', 0, 1, 's{t} = (vd)all_v;')
op('PUSH_ROWS', 0, 1, 'for (int i = 0; i < T; ++i) s{t}[i] = (double)((const uint32_t *)ip->p)[tile * T + i];')
op('PICK', 0, 1, 's{t} = s{pick};', needs='pick')      # push a copy of slot (d-1-a)
op('SET', 1, 0, 's{set} = s{top};', needs='set')        # pop the top into slot (d-2-a)
op('POP', 1, 0, '')
op('ADD', 2, 1, 's{b} = s{b} + s{top};')
op('SUB', 2, 1, 's{b} = s{b} - s{top};')
op('MUL', 2, 1, 's{b} = s{b} * s{top};')
op('DIV', 2, 1, 's{b} = s{b} / s{top};')
op('ADDK', 1, 1, 's{top} = s{top} + ip->k;')
op('SUBK', 1, 1, 's{top} = s{top} - ip->k;')
op('MULK', 1, 1, 's{top} = s{top} * ip->k;')
op('DIVK', 1, 1, 's{top} = s{top} / ip->k;')
op('LT', 2, 1, 's{b} = (vd)(s{b} < s{top});')
op('GT', 2, 1, 's{b} = (vd)(s{b} > s{top});')
op('AND', 2, 1, 's{b} = (vd)((vm)s{b} & (vm)s{top});')
op('NOT', 1, 1, 's{top} = (vd)(~(vm)s{top});')
op('GATHER4', 1, 1, '{{ const uint32_t *col = ip->p; for (int i = 0; i < T; ++i) s{top}[i] = col[(size_t)s{top}[i]]; }}')
op('GATHER8', 1, 1, '{{ const double *col = ip->p; for (int i = 0; i < T; ++i) s{top}[i] = col[(size_t)s{top}[i]]; }}')
op('BITS', 1, 1, '{{ const uint64_t *bits = ip->p; vm m; for (int i = 0; i < T; ++i) {{ size_t k = (size_t)s{top}[i]; m[i] = -(int64_t)(bits[k >> 6] >> (k & 63) & 1); }} s{top} = (vd)m; }}')
op('STOREM4', 3, 0, '{{ uint32_t *col = ip->p; vm m = (vm)s{top} & valid; for (int i = 0; i < T; ++i) if (m[i]) col[(size_t)s{c}[i]] = (uint32_t)s{b}[i]; }}')
op('SEL', 3, 1, 's{c} = (vd)(((vm)s{b} & (vm)s{top}) | ((vm)s{c} & ~(vm)s{top}));')  # [a, b, m] -> [m ? b : a]
op('ACCM', 1, 1, 'acc[ip->a] += (vd)((vm)s{top} & valid & (vm)one_v);')                    # acc += top mask ? 1 : 0 (peek)
op('ACC_ADD', 1, 0, 'acc[ip->a] += (vd)((vm)s{top} & valid);')
op('COMPACT', 1, 0, '{{ vm m = (vm)s{top} & valid; for (int i = 0; i < T; ++i) if (m[i]) ctx->out[ctx->count++] = (uint32_t)(tile * T + i); }}')
op('JANYS', 0, 0, '{{ vm m = (vm)s{pick} & valid; int64_t any = 0; for (int i = 0; i < T; ++i) any |= m[i]; if (any && ++steps < 64) {{ ip = prog + ip->a; goto *ip->label; }} steps = 0; }}', needs='pickj')
op('TILE', 0, 0, 'if (++tile == ctx->tiles) goto halt; base += T; for (int i = 0; i < T; ++i) valid[i] = -(int64_t)(tile * T + i < ctx->count_lanes); ip = prog; goto *ip->label;')

print('/* Made by make.py (research 132): a register-tile interpreter. See make.py. */')
print('#define _POSIX_C_SOURCE 200809L')
print('#include <stdint.h>\n#include <stdbool.h>\n#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n#include <math.h>\n#include <time.h>')
print('#ifndef T\n#define T 8\n#endif')
print(f'enum {{ DEPTH = {DEPTH} }};')
print('typedef double vd __attribute__((vector_size(8 * T)));')
print('typedef int64_t vm __attribute__((vector_size(8 * T)));')
print('typedef struct { const void *label; int op, a; double k, k2; void *p; } Insn;')
print('typedef struct { size_t tiles, count_lanes; uint32_t *out; size_t count; vd acc[2]; } Ctx;')
print('static vd lanes_v, one_v; static vm all_v;')
print('static void init_vectors(void) { for (int i = 0; i < T; ++i) lanes_v[i] = i, one_v[i] = 1, all_v[i] = -1; }')
# op numbering: (name, depth)
names = {}
labels = []
for name, pops, pushes, body, needs in ops:
    for d in range(DEPTH + 1):
        if d < pops or d - pops + pushes > DEPTH:
            continue
        if needs == 'pick' or needs == 'pickj':
            for a in range(d):
                labels.append((name, d, a))
        elif needs == 'set':
            for a in range(d - 1):
                labels.append((name, d, a))
        else:
            labels.append((name, d, -1))
print('enum {')
for i, (name, d, a) in enumerate(labels):
    print(f'    OP_{name}_{d}' + (f'_{a}' if a >= 0 else '') + f' = {i},')
print('    OPS\n};')
print('static const void *table[OPS];')
print('''
/* The program run tile by tile from ctx (tiles, the valid lanes of the last), until TILE finds none left. */
static __attribute__((noinline)) void run(const Insn *prog, Ctx *ctx) {
    if (!prog) {''')
for i, (name, d, a) in enumerate(labels):
    print(f'        table[{i}] = &&L_{name}_{d}' + (f'_{a}' if a >= 0 else '') + ';')
print('''        return;
    }
    register const Insn *ip = prog;
    vd ''' + ', '.join(f's{i} = {{0}}' for i in range(DEPTH)) + ''';
    vd acc[2] = {ctx->acc[0], ctx->acc[1]};
    vm valid;
    double base = 0;
    size_t tile = 0;
    unsigned steps = 0;
    for (int i = 0; i < T; ++i) valid[i] = -(int64_t)((size_t)i < ctx->count_lanes);
    goto *ip->label;''')
for name, d, a in labels:
    spec = next(o for o in ops if o[0] == name)
    _, pops, pushes, body, needs = spec
    top = f'{d-1}'
    b = f'{d-2}'
    c = f'{d-3}'
    t = f'{d}'
    pick = f'{d-1-a}' if a >= 0 and needs in ('pick', 'pickj') else ''
    sett = f'{d-2-a}' if a >= 0 and needs == 'set' else ''
    code = body.format(t=t, top=top, b=b, c=c, pick=pick, set=sett)
    suffix = f'_{a}' if a >= 0 else ''
    print(f'    L_{name}_{d}{suffix}: {code} ++ip; goto *ip->label;')
print('''    halt:
    ctx->acc[0] = acc[0], ctx->acc[1] = acc[1];
}

static Insn *at;
static Insn *emit(int opc, int a, double k, double k2, void *p) {
    Insn *i = at++;
    i->label = table[opc], i->op = opc, i->a = a, i->k = k, i->k2 = k2, i->p = p;
    return i;
}
#define I(name, d) emit(OP_##name##_##d, 0, 0, 0, NULL)
#define IK(name, d, k) emit(OP_##name##_##d, 0, k, 0, NULL)
#define IP(name, d, p) emit(OP_##name##_##d, 0, 0, 0, p)
#define IA(name, d, a) emit(OP_##name##_##d##_##a, a, 0, 0, NULL)

static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static uint64_t minstd(uint64_t x) { return x * 48271 % 2147483647; }

/* ---- knapsack: 100 items, capacity 50,000 (the suite's), the table in u32; the inner loop the body */
static double knapsack_tiles(unsigned n, unsigned capacity, const uint32_t *weight, const uint32_t *worth, uint32_t *best, double *ms) {
    Insn prog[16];
    double a = now_ms();
    for (unsigned i = 0; i < n; ++i) {
        uint32_t w = weight[i];
        if (w > capacity) continue;
        at = prog;
        emit(OP_PUSH_AFFINE_0, 0, (double)capacity, -1, NULL); /* c = capacity - k */
        IA(PICK, 1, 0); IK(SUBK, 2, (double)w); IP(GATHER4, 2, best); IK(ADDK, 2, (double)worth[i]); /* [c, with] */
        IA(PICK, 2, 0); IA(PICK, 3, 2); IP(GATHER4, 4, best); I(GT, 4); IP(STOREM4, 3, best); I(TILE, 0); /* [c, with, with > best[c]] */
        size_t count = capacity + 1 - w;
        Ctx ctx = {.tiles = (count + T - 1) / T, .count_lanes = count};
        run(prog, &ctx);
    }
    *ms = now_ms() - a;
    return best[capacity];
}

static double knapsack_passes(unsigned n, unsigned capacity, const uint32_t *weight, const uint32_t *worth, uint32_t *best, double *ms) {
    enum { LANES = 1024 };
    static double c_[LANES], with[LANES], old[LANES];
    static uint32_t mask[LANES];
    double a = now_ms();
    for (unsigned i = 0; i < n; ++i) {
        uint32_t w = weight[i];
        if (w > capacity) continue;
        size_t count = capacity + 1 - w;
        for (size_t k0 = 0; k0 < count; k0 += LANES) {
            size_t m = count - k0 < LANES ? count - k0 : LANES;
            for (size_t l = 0; l < m; ++l) c_[l] = (double)capacity - (double)(k0 + l);
            for (size_t l = 0; l < m; ++l) with[l] = best[(size_t)(c_[l] - w)];
            for (size_t l = 0; l < m; ++l) with[l] += worth[i];
            for (size_t l = 0; l < m; ++l) old[l] = best[(size_t)c_[l]];
            for (size_t l = 0; l < m; ++l) mask[l] = with[l] > old[l];
            for (size_t l = 0; l < m; ++l) if (mask[l]) best[(size_t)c_[l]] = (uint32_t)with[l];
        }
    }
    *ms = now_ms() - a;
    return best[capacity];
}

static double knapsack_c(unsigned n, unsigned capacity, const uint32_t *weight, const uint32_t *worth, uint32_t *best, double *ms) {
    double a = now_ms();
    for (uint32_t i = 0; i < n; ++i)
        for (uint32_t c = capacity; c >= weight[i] && c <= capacity; --c) {
            uint32_t with = best[c - weight[i]] + worth[i];
            if (with > best[c]) best[c] = with;
        }
    *ms = now_ms() - a;
    return best[capacity];
}

/* ---- roots: the suite's 100,000 Newton square roots, a tile of lanes stepping together, each leaving as it converges */
static void roots_tiles(unsigned n, double *sum, double *steps, double *ms) {
    Insn prog[32];
    at = prog;
    emit(OP_PUSH_AFFINE_0, 0, 1, 1, NULL);  /* [i] */
    I(PUSH_ALL, 1);                           /* [i, live] */
    IA(PICK, 2, 1);                           /* [i, live, r] */
    Insn *loop = at;
    IA(PICK, 3, 2); IA(PICK, 4, 1); I(DIV, 5); IA(PICK, 4, 1); I(ADD, 5); IK(DIVK, 4, 2); /* [i, live, r, next] */
    IA(PICK, 4, 0); IA(PICK, 5, 2); I(LT, 6); IA(PICK, 5, 3); I(AND, 6);                 /* [i, live, r, next, live'] */
    IA(SET, 5, 2);                                                                           /* [i, live', r, next] */
    IA(PICK, 4, 2); emit(OP_ACCM_5, 1, 0, 0, NULL); I(SEL, 5);                              /* count += live'; [i, live', r'] */
    emit(OP_JANYS_3_1, 1, 0, 0, NULL)->a = (int)(loop - prog);
    emit(OP_ACC_ADD_3, 0, 0, 0, NULL);        /* sum += r */
    I(POP, 2); I(POP, 1); I(TILE, 0);
    double a = now_ms();
    Ctx ctx = {.tiles = (n + T - 1) / T, .count_lanes = n};
    run(prog, &ctx);
    *ms = now_ms() - a;
    *sum = *steps = 0;
    for (int i = 0; i < T; ++i) *sum += ctx.acc[0][i], *steps += ctx.acc[1][i];
}

static void roots_c(unsigned n, double *sum, double *steps, double *ms) {
    double a = now_ms(), s = 0, c = 0;
    for (uint32_t i = 1; i <= n; ++i) {
        double r = i;
        for (int k = 0; k < 64; ++k) {
            double next = (r + i / r) / 2;
            if (next >= r) break;
            r = next;
            ++c;
        }
        s += r;
    }
    *sum = s, *steps = c, *ms = now_ms() - a;
}

/* ---- bfs: the suite's graph (100,000 nodes, 1,000,000 edges drawn by xorshift from 121), a search from a node; the
 * frontier's edges spread a lane each: to gathered, seen's bits asked, the candidates compacted; then the tail in C. */
static uint64_t gstate = 121;
static uint64_t gnext(void) { gstate ^= gstate << 13, gstate ^= gstate >> 7, gstate ^= gstate << 17; return gstate; }
static int cmp_u64(const void *a, const void *b) { uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b; return (x > y) - (x < y); }
typedef struct { uint32_t nodes; uint32_t *start, *to; } Graph;
static Graph graph_build(uint32_t nodes, size_t edges) {
    uint64_t *keys = malloc(edges * 8);
    size_t c = 0;
    gstate = 121;
    while (c < edges) { uint64_t a = gnext() % nodes, b = gnext() % nodes; if (a != b) keys[c++] = a << 20 | b; }
    qsort(keys, c, 8, cmp_u64);
    size_t u = 0;
    for (size_t i = 0; i < c; ++i) if (!u || keys[i] != keys[u - 1]) keys[u++] = keys[i];
    Graph g = {nodes, calloc(nodes + 1, 4), malloc(4 * u)};
    for (size_t i = 0; i < u; ++i) ++g.start[(keys[i] >> 20) + 1], g.to[i] = keys[i] & 0xFFFFF;
    for (uint32_t v = 0; v < nodes; ++v) g.start[v + 1] += g.start[v];
    free(keys);
    return g;
}

enum { BATCH = 1024, LANES = 65536 };
static uint32_t rows[LANES], owner[LANES], cand[LANES];

/* One search: the queue walked a batch of nodes at a time, the batch's edges spread into lanes (owner: the node). tiles: the
 * frontier by the tile program; else by one pass an operation over the lanes (the batch executor's shape). */
static uint32_t bfs_search(const Graph *g, uint32_t from, bool tiles, uint64_t *seen, uint32_t *depth, uint32_t *queue, double *ms) {
    Insn prog[8];
    at = prog;
    IP(PUSH_ROWS, 0, rows); IP(GATHER4, 1, g->to); IA(PICK, 1, 0); IP(BITS, 2, seen); I(NOT, 2); I(COMPACT, 2); I(TILE, 0);
    double a = now_ms();
    memset(seen, 0, (g->nodes + 63) / 64 * 8);
    size_t count = 1, i = 0;
    queue[0] = from, seen[from >> 6] |= UINT64_C(1) << (from & 63), depth[from] = 0;
    while (i < count) {
        size_t end = count < i + BATCH ? count : i + BATCH, n = 0;
        for (size_t r = i; r < end; ++r) { /* spread: an edge a lane (a batch's edges fit: 1024 nodes x 10 on average) */
            uint32_t v = queue[r];
            for (uint32_t e = g->start[v]; e < g->start[v + 1]; ++e) {
                if (n == LANES) { fputs("too many edges in a batch\\n", stderr); exit(1); }
                rows[n] = e, owner[n++] = v;
            }
        }
        Ctx ctx = {.tiles = (n + T - 1) / T, .count_lanes = n, .out = cand, .count = 0};
        if (tiles) run(prog, &ctx);
        else {
            static uint32_t to_[LANES], has[LANES];
            for (size_t l = 0; l < n; ++l) to_[l] = g->to[rows[l]];
            for (size_t l = 0; l < n; ++l) has[l] = seen[to_[l] >> 6] >> (to_[l] & 63) & 1;
            for (size_t l = 0; l < n; ++l) if (!has[l]) cand[ctx.count++] = (uint32_t)l;
        }
        for (size_t k = 0; k < ctx.count; ++k) { /* the tail: each candidate asked again in order, added, appended */
            uint32_t l = cand[k], t = g->to[rows[l]];
            if (seen[t >> 6] >> (t & 63) & 1) continue;
            seen[t >> 6] |= UINT64_C(1) << (t & 63);
            depth[t] = depth[owner[l]] + 1;
            queue[count++] = t;
        }
        i = end;
    }
    *ms = now_ms() - a;
    return (uint32_t)count;
}

static uint32_t bfs_c(const Graph *g, uint32_t from, uint64_t *seen, uint32_t *depth, uint32_t *queue, double *ms) {
    double a = now_ms();
    memset(seen, 0, (g->nodes + 63) / 64 * 8);
    size_t count = 1;
    queue[0] = from, seen[from >> 6] |= UINT64_C(1) << (from & 63), depth[from] = 0;
    for (size_t i = 0; i < count; ++i) {
        uint32_t v = queue[i];
        for (uint32_t e = g->start[v]; e < g->start[v + 1]; ++e) {
            uint32_t t = g->to[e];
            if (seen[t >> 6] >> (t & 63) & 1) continue;
            seen[t >> 6] |= UINT64_C(1) << (t & 63);
            depth[t] = depth[v] + 1;
            queue[count++] = t;
        }
    }
    *ms = now_ms() - a;
    return (uint32_t)count;
}

int main(int argc, char **argv) {
    const char *body = argc > 1 ? argv[1] : "all";
    int rounds = argc > 2 ? atoi(argv[2]) : 1;
    init_vectors();
    run(NULL, NULL);
    bool all = !strcmp(body, "all");
    if (all || !strcmp(body, "knapsack")) {
        unsigned n = 100, capacity = 50000;
        uint32_t *weight = malloc(4 * n), *worth = malloc(4 * n), *best = calloc(capacity + 1, 4);
        uint64_t x = 1;
        for (unsigned i = 0; i < n; ++i) x = minstd(x), weight[i] = (uint32_t)(x % 1000) + 1, x = minstd(x), worth[i] = (uint32_t)(x % 1000);
        for (int r = 0; r < rounds; ++r) {
            double ms[3], answers[3];
            memset(best, 0, 4 * (capacity + 1)), answers[0] = knapsack_tiles(n, capacity, weight, worth, best, &ms[0]);
            memset(best, 0, 4 * (capacity + 1)), answers[1] = knapsack_passes(n, capacity, weight, worth, best, &ms[1]);
            memset(best, 0, 4 * (capacity + 1)), answers[2] = knapsack_c(n, capacity, weight, worth, best, &ms[2]);
            printf("{\\"body\\":\\"knapsack\\",\\"T\\":%d,\\"tiles_ms\\":%.3f,\\"passes_ms\\":%.3f,\\"c_ms\\":%.3f,\\"same\\":%s,\\"answer\\":%.17g}\\n", T, ms[0], ms[1], ms[2],
                   answers[0] == answers[2] && answers[1] == answers[2] ? "true" : "false", answers[0]);
        }
    }
    if (all || !strcmp(body, "roots")) {
        for (int r = 0; r < rounds; ++r) {
            double sum[2], steps[2], ms[2];
            roots_tiles(100000, &sum[0], &steps[0], &ms[0]);
            roots_c(100000, &sum[1], &steps[1], &ms[1]);
            printf("{\\"body\\":\\"roots\\",\\"T\\":%d,\\"tiles_ms\\":%.3f,\\"c_ms\\":%.3f,\\"same\\":%s,\\"sum\\":%.17g,\\"steps\\":%.0f}\\n", T, ms[0], ms[1],
                   sum[0] == sum[1] && steps[0] == steps[1] ? "true" : "false", sum[0], steps[0]);
        }
    }
    if (all || !strcmp(body, "bfs")) {
        Graph g = graph_build(100000, 1000000);
        uint64_t *seen = calloc((g.nodes + 63) / 64, 8);
        uint32_t *depth = calloc(g.nodes, 4), *queue = malloc(4 * g.nodes);
        for (int r = 0; r < rounds; ++r) {
            uint32_t from = (uint32_t)(gnext() % g.nodes), reached[3];
            double ms[3];
            reached[0] = bfs_search(&g, from, true, seen, depth, queue, &ms[0]);
            reached[1] = bfs_search(&g, from, false, seen, depth, queue, &ms[1]);
            reached[2] = bfs_c(&g, from, seen, depth, queue, &ms[2]);
            printf("{\\"body\\":\\"bfs\\",\\"T\\":%d,\\"tiles_ms\\":%.3f,\\"passes_ms\\":%.3f,\\"c_ms\\":%.3f,\\"same\\":%s,\\"reached\\":%u}\\n", T, ms[0], ms[1], ms[2],
                   reached[0] == reached[2] && reached[1] == reached[2] ? "true" : "false", reached[0]);
        }
    }
    return 0;
}''')
