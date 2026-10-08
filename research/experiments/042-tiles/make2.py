#!/usr/bin/env python3
"""Research 132's deciding experiment, second form: a register-tile
interpreter as threaded tail calls (tail.c).

make.py's form (computed goto, one function) spilled every slot to the
stack: GCC keeps nothing in registers across an indirect branch. Here each
operation is a function whose arguments are the slots (homogeneous vector
aggregates: AAPCS64 passes them in v0-v7) and which tail-calls the next
(clang's musttail; GCC's sibling calls), so the slots stay in registers:
an operation is its vector instructions, a load of the next address and a
branch. K registers a slot (a tile of 2K doubles), S slots (K * S <= 8).
Operations name their slots (three-address), so each (op, slots) is a
function; only the ones the three programs use are made.

    python3 make2.py K S > tail-K-S.c
    clang -O2 -o tail-K-S tail-K-S.c -lm     (or gcc)
    ./tail-K-S BODY ROUNDS                   (knapsack | bfs | roots | all)
"""
import sys

K = int(sys.argv[1]) if len(sys.argv) > 1 else 2
S = int(sys.argv[2]) if len(sys.argv) > 2 else 4
T = 2 * K
assert K * S <= 8
F = 'abcd'[:K]  # a slot's fields, a register each

# ---- programs: (op, dst, a, b, k, k2, p, target); slots by number; None where not used
programs = {}
# knapsack: slots 0 c, 1 with, 2 best then the mask
programs['knapsack'] = [
    ('AFFINE', 0, None, None, 'capacity', -1.0, None, None),
    ('SUBK', 1, 0, None, 'w', None, None, None),
    ('GATHER4', 1, 1, None, None, None, 'best', None),
    ('ADDK', 1, 1, None, 'worth', None, None, None),
    ('GATHER4', 2, 0, None, None, None, 'best', None),
    ('GT', 2, 1, 2, None, None, None, None),
    ('STOREM4', None, 0, 1, None, None, 'best', 2),  # rows a, value b, mask target
    ('TILE', None, None, None, None, None, None, None),
]
# bfs: slots 0 row then to, 1 has then its not
programs['bfs'] = [
    ('ROWS', 0, None, None, None, None, 'rows', None),
    ('GATHER4', 0, 0, None, None, None, 'to', None),
    ('BITS', 1, 0, None, None, None, 'seen', None),
    ('NOT', 1, 1, None, None, None, None, None),
    ('COMPACT', None, 1, None, None, None, 'cand', None),
    ('TILE', None, None, None, None, None, None, None),
]
if S >= 5:  # roots: 0 i, 1 live, 2 r, 3 next, 4 a mask
    programs['roots'] = [
        ('AFFINE', 0, None, None, 1.0, 1.0, None, None),
        ('ALL', 1, None, None, None, None, None, None),
        ('MOV', 2, 0, None, None, None, None, None),
        ('DIV', 3, 0, 2, None, None, None, None),       # 3: loop
        ('ADD', 3, 3, 2, None, None, None, None),
        ('DIVK', 3, 3, None, 2.0, None, None, None),
        ('LT', 4, 3, 2, None, None, None, None),
        ('AND', 1, 1, 4, None, None, None, None),
        ('ACCM', None, 1, None, None, None, None, 1),   # acc[1] += live
        ('SEL', 2, 3, 1, None, None, None, None),       # r = live ? next : r
        ('JANY', None, 1, None, None, None, None, 3),
        ('ACC_ADD', None, 2, None, None, None, None, 0),
        ('TILE', None, None, None, None, None, None, None),
    ]
else:  # roots in four slots: the compare and the and as one (CMPLT_AND: dst &= a < b), the only fused operation here
    programs['roots'] = [
        ('AFFINE', 0, None, None, 1.0, 1.0, None, None),
        ('ALL', 1, None, None, None, None, None, None),
        ('MOV', 2, 0, None, None, None, None, None),
        ('DIV', 3, 0, 2, None, None, None, None),       # 3: loop
        ('ADD', 3, 3, 2, None, None, None, None),
        ('DIVK', 3, 3, None, 2.0, None, None, None),
        ('CMPLT_AND', 1, 3, 2, None, None, None, None),
        ('ACCM', None, 1, None, None, None, None, 1),
        ('SEL', 2, 3, 1, None, None, None, None),
        ('JANY', None, 1, None, None, None, None, 3),
        ('ACC_ADD', None, 2, None, None, None, None, 0),
        ('TILE', None, None, None, None, None, None, None),
    ]

# ---- operations: C for the fields, with {d} {a} {b} slot names and {f} the field
def per_field(fmt):
    return ' '.join(fmt.replace('{f}', f) for f in F)

bodies = {
    'AFFINE': lambda d, a, b: per_field('s{d}.{f} = vfmaq_f64(vdupq_n_f64(ip->k), vaddq_f64(lanes_{f}, vdupq_n_f64(c->base)), vdupq_n_f64(ip->k2));'.replace('{d}', str(d))),
    'ALL': lambda d, a, b: per_field('s{d}.{f} = all_f64;'.replace('{d}', str(d))),
    'MOV': lambda d, a, b: per_field('s{d}.{f} = s{a}.{f};'.replace('{d}', str(d)).replace('{a}', str(a))),
    'ADD': lambda d, a, b: per_field('s{d}.{f} = vaddq_f64(s{a}.{f}, s{b}.{f});'.replace('{d}', str(d)).replace('{a}', str(a)).replace('{b}', str(b))),
    'SUB': lambda d, a, b: per_field('s{d}.{f} = vsubq_f64(s{a}.{f}, s{b}.{f});'.replace('{d}', str(d)).replace('{a}', str(a)).replace('{b}', str(b))),
    'MUL': lambda d, a, b: per_field('s{d}.{f} = vmulq_f64(s{a}.{f}, s{b}.{f});'.replace('{d}', str(d)).replace('{a}', str(a)).replace('{b}', str(b))),
    'DIV': lambda d, a, b: per_field('s{d}.{f} = vdivq_f64(s{a}.{f}, s{b}.{f});'.replace('{d}', str(d)).replace('{a}', str(a)).replace('{b}', str(b))),
    'ADDK': lambda d, a, b: per_field('s{d}.{f} = vaddq_f64(s{a}.{f}, vdupq_n_f64(ip->k));'.replace('{d}', str(d)).replace('{a}', str(a))),
    'SUBK': lambda d, a, b: per_field('s{d}.{f} = vsubq_f64(s{a}.{f}, vdupq_n_f64(ip->k));'.replace('{d}', str(d)).replace('{a}', str(a))),
    'DIVK': lambda d, a, b: per_field('s{d}.{f} = vdivq_f64(s{a}.{f}, vdupq_n_f64(ip->k));'.replace('{d}', str(d)).replace('{a}', str(a))),
    'LT': lambda d, a, b: per_field('s{d}.{f} = vreinterpretq_f64_u64(vcltq_f64(s{a}.{f}, s{b}.{f}));'.replace('{d}', str(d)).replace('{a}', str(a)).replace('{b}', str(b))),
    'GT': lambda d, a, b: per_field('s{d}.{f} = vreinterpretq_f64_u64(vcgtq_f64(s{a}.{f}, s{b}.{f}));'.replace('{d}', str(d)).replace('{a}', str(a)).replace('{b}', str(b))),
    'CMPLT_AND': lambda d, a, b: per_field('s{d}.{f} = vreinterpretq_f64_u64(vandq_u64(vreinterpretq_u64_f64(s{d}.{f}), vcltq_f64(s{a}.{f}, s{b}.{f})));'.replace('{d}', str(d)).replace('{a}', str(a)).replace('{b}', str(b))),
    'AND': lambda d, a, b: per_field('s{d}.{f} = vreinterpretq_f64_u64(vandq_u64(vreinterpretq_u64_f64(s{a}.{f}), vreinterpretq_u64_f64(s{b}.{f})));'.replace('{d}', str(d)).replace('{a}', str(a)).replace('{b}', str(b))),
    'NOT': lambda d, a, b: per_field('s{d}.{f} = vreinterpretq_f64_u64(veorq_u64(vreinterpretq_u64_f64(s{a}.{f}), all_u64));'.replace('{d}', str(d)).replace('{a}', str(a))),
    'SEL': lambda d, a, b: per_field('s{d}.{f} = vbslq_f64(vreinterpretq_u64_f64(s{b}.{f}), s{a}.{f}, s{d}.{f});'.replace('{d}', str(d)).replace('{a}', str(a)).replace('{b}', str(b))),
    'GATHER4': lambda d, a, b: '{ const uint32_t *col = ip->p; ' + per_field('s{d}.{f} = vsetq_lane_f64(col[(size_t)vgetq_lane_f64(s{a}.{f}, 0)], s{d}.{f}, 0); s{d}.{f} = vsetq_lane_f64(col[(size_t)vgetq_lane_f64(s{a}.{f}, 1)], s{d}.{f}, 1);'.replace('{d}', str(d)).replace('{a}', str(a))) + ' }',
    'BITS': lambda d, a, b: '{ const uint64_t *bits = ip->p; uint64x2_t m = vdupq_n_u64(0); size_t k; ' + per_field('k = (size_t)vgetq_lane_f64(s{a}.{f}, 0); m = vsetq_lane_u64(-(bits[k >> 6] >> (k & 63) & 1), m, 0); k = (size_t)vgetq_lane_f64(s{a}.{f}, 1); m = vsetq_lane_u64(-(bits[k >> 6] >> (k & 63) & 1), m, 1); s{d}.{f} = vreinterpretq_f64_u64(m);'.replace('{d}', str(d)).replace('{a}', str(a))) + ' }',
    'ROWS': lambda d, a, b: '{ const uint32_t *rows = (const uint32_t *)ip->p + c->tile * T; ' + ' '.join(f's{d}.{f} = vcvtq_f64_u64(vmovl_u32(vld1_u32(rows + {2 * i})));' for i, f in enumerate(F)) + ' }',
    'STOREM4': lambda d, a, b: '{ uint32_t *col = ip->p; uint64x2_t m; ' + ' '.join(f'm = vandq_u64(vreinterpretq_u64_f64(s{{m}}.{f}), c->valid.{f}); if (vgetq_lane_u64(m, 0)) col[(size_t)vgetq_lane_f64(s{a}.{f}, 0)] = (uint32_t)vgetq_lane_f64(s{b}.{f}, 0); if (vgetq_lane_u64(m, 1)) col[(size_t)vgetq_lane_f64(s{a}.{f}, 1)] = (uint32_t)vgetq_lane_f64(s{b}.{f}, 1);' for f in F) + ' }',
    'COMPACT': lambda d, a, b: '{ uint32_t *out = ip->p; uint64x2_t m; ' + ' '.join(f'm = vandq_u64(vreinterpretq_u64_f64(s{a}.{f}), c->valid.{f}); if (vgetq_lane_u64(m, 0)) out[c->count++] = (uint32_t)(c->tile * T + {2 * i}); if (vgetq_lane_u64(m, 1)) out[c->count++] = (uint32_t)(c->tile * T + {2 * i + 1});' for i, f in enumerate(F)) + ' }',
    'ACCM': lambda d, a, b: per_field('c->acc[{n}].{f} = vaddq_f64(c->acc[{n}].{f}, vbslq_f64(vandq_u64(vreinterpretq_u64_f64(s{a}.{f}), c->valid.{f}), one_f64, zero_f64));'.replace('{a}', str(a)).replace('{n}', '{n}')),
    'ACC_ADD': lambda d, a, b: per_field('c->acc[{n}].{f} = vaddq_f64(c->acc[{n}].{f}, vbslq_f64(c->valid.{f}, s{a}.{f}, zero_f64));'.replace('{a}', str(a)).replace('{n}', '{n}')),
    'JANY': lambda d, a, b: '{ uint64x2_t m = ' + ' | '.join(f'vandq_u64(vreinterpretq_u64_f64(s{a}.{f}), c->valid.{f})' for f in F) + '; if ((vgetq_lane_u64(m, 0) | vgetq_lane_u64(m, 1)) && ++c->steps < 64) { ip = c->prog + ip->a - 1; } else c->steps = 0; }',
    'TILE': lambda d, a, b: '{ if (++c->tile == c->tiles) return; c->base += T; ' + ' '.join(f'c->valid.{f} = vsetq_lane_u64(-(uint64_t)(c->tile * T + {2 * i} < c->count_lanes), c->valid.{f}, 0); c->valid.{f} = vsetq_lane_u64(-(uint64_t)(c->tile * T + {2 * i + 1} < c->count_lanes), c->valid.{f}, 1);' for i, f in enumerate(F)) + ' ip = c->prog - 1; }',
}

used = {}
for name, prog in programs.items():
    for op, d, a, b, k, k2, p, target in prog:
        n = target if op in ('ACCM', 'ACC_ADD', 'STOREM4') else None
        used[(op, d, a, b, n)] = True

def fname(op, d, a, b, n):
    return 'op_' + op.lower() + ''.join('_' + str(x) for x in (d, a, b, n) if x is not None)

args = ', '.join(f'slot s{i}' for i in range(S))
pass_args = ', '.join(f's{i}' for i in range(S))
print(f'/* Made by make2.py {K} {S} (research 132): a register-tile interpreter as threaded tail calls. See make2.py. */')
print('''#define _POSIX_C_SOURCE 200809L
#include <arm_neon.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#ifdef __clang__
#define MUSTTAIL __attribute__((musttail))
#else
#define MUSTTAIL
#endif''')
print(f'enum {{ K = {K}, S = {S}, T = {T} }};')
print('typedef struct { ' + ' '.join(f'float64x2_t {f};' for f in F) + ' } slot;')
print('typedef struct { ' + ' '.join(f'uint64x2_t {f};' for f in F) + ' } vmask;')
print('typedef struct insn insn;')
print('typedef struct { double base; size_t tile, tiles, count_lanes, count; unsigned steps; vmask valid; slot acc[2]; const insn *prog; uint32_t *out; } Ctx;')
print(f'typedef void op_f(const insn *ip, Ctx *c, {args});')
print('struct insn { op_f *f; double k, k2; void *p; int a; };')
print('static float64x2_t ' + ', '.join(f'lanes_{f}' for f in F) + ', all_f64, one_f64, zero_f64; static uint64x2_t all_u64;')
print('static void init_vectors(void) { ' + ' '.join(f'lanes_{f} = (float64x2_t){{{2 * i}, {2 * i + 1}}};' for i, f in enumerate(F)) + ' all_u64 = vdupq_n_u64(~UINT64_C(0)); all_f64 = vreinterpretq_f64_u64(all_u64); one_f64 = vdupq_n_f64(1); zero_f64 = vdupq_n_f64(0); }')
print(f'#define NEXT do {{ const insn *n = ip + 1; MUSTTAIL return n->f(n, c, {pass_args}); }} while (0)')
for (op, d, a, b, n) in used:
    body = bodies[op](d, a, b)
    if n is not None:
        body = body.replace('{n}', str(n)).replace('{m}', str(n))
    print(f'static void {fname(op, d, a, b, n)}(const insn *ip, Ctx *c, {args}) {{ {body} NEXT; }}')
print('''
static insn *at;
static insn *emit(op_f *f, double k, double k2, void *p, int a) {
    insn *i = at++;
    i->f = f, i->k = k, i->k2 = k2, i->p = p, i->a = a;
    return i;
}
static void start(const insn *prog, Ctx *c) {
    slot z = {''' + ', '.join('vdupq_n_f64(0)' for _ in F) + '''};
    c->prog = prog, c->tile = 0, c->base = 0, c->steps = 0;
    ''' + ' '.join(f'c->valid.{f} = (uint64x2_t){{-(uint64_t)({2 * i} < c->count_lanes), -(uint64_t)({2 * i + 1} < c->count_lanes)}};' for i, f in enumerate(F)) + '''
    prog->f(prog, c, ''' + ', '.join('z' for _ in range(S)) + ''');
}
static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static uint64_t minstd(uint64_t x) { return x * 48271 % 2147483647; }
''')

def emit_program(name, prog, vars):
    lines = []
    for op, d, a, b, k, k2, p, target in prog:
        n = target if op in ('ACCM', 'ACC_ADD', 'STOREM4') else None
        kk = vars.get(k, k) if isinstance(k, str) else (k if k is not None else 0)
        kk2 = k2 if k2 is not None else 0
        pp = vars.get(p, 'NULL') if p else 'NULL'
        aa = target if op in ('JANY', 'STOREM4') else 0
        lines.append(f'    emit({fname(op, d, a, b, n)}, {kk}, {kk2}, {pp}, {aa});')
    return '\n'.join(lines)

print('''/* ---- knapsack: 100 items, capacity 50,000 (the suite's), the table in u32; the inner loop the body */
static double knapsack_tiles(unsigned n, unsigned capacity, const uint32_t *weight, const uint32_t *worth, uint32_t *best, double *ms) {
    insn prog[16];
    double a = now_ms();
    for (unsigned i = 0; i < n; ++i) {
        uint32_t w = weight[i];
        if (w > capacity) continue;
        at = prog;
        double capacity_ = capacity, w_ = w, worth_ = worth[i];
''' + emit_program('knapsack', programs['knapsack'], {'capacity': 'capacity_', 'w': 'w_', 'worth': 'worth_', 'best': 'best'}) + '''
        size_t count = capacity + 1 - w;
        Ctx ctx = {.tiles = (count + T - 1) / T, .count_lanes = count};
        start(prog, &ctx);
    }
    *ms = now_ms() - a;
    return best[capacity];
}

/* The same body as the batch executor's shape done tightly: four-byte lanes, affine rows read as strides, two passes a batch
 * of 1024 rounds (with = best[c - w] + worth; best[c] = max(best[c], with), the store where it is greater as a select). */
static double knapsack_passes(unsigned n, unsigned capacity, const uint32_t *weight, const uint32_t *worth, uint32_t *best, double *ms) {
    enum { LANES = 1024 };
    static uint32_t with[LANES];
    double a = now_ms();
    for (unsigned i = 0; i < n; ++i) {
        uint32_t w = weight[i], v = worth[i];
        if (w > capacity) continue;
        size_t count = capacity + 1 - w;
        for (size_t k0 = 0; k0 < count; k0 += LANES) {
            size_t m = count - k0 < LANES ? count - k0 : LANES;
            const uint32_t *from = best + (capacity - k0 - w);
            uint32_t *to = best + (capacity - k0);
            for (size_t l = 0; l < m; ++l) with[l] = from[-(ptrdiff_t)l] + v;
            for (size_t l = 0; l < m; ++l) { uint32_t cur = to[-(ptrdiff_t)l]; to[-(ptrdiff_t)l] = with[l] > cur ? with[l] : cur; }
        }
    }
    *ms = now_ms() - a;
    return best[capacity];
}

/* The same two passes with the lanes numbered the other way (the batch executor's reverse): memory's order, ascending. */
static double knapsack_ascending(unsigned n, unsigned capacity, const uint32_t *weight, const uint32_t *worth, uint32_t *best, double *ms) {
    enum { LANES = 1024 };
    static uint32_t with[LANES];
    double a = now_ms();
    for (unsigned i = 0; i < n; ++i) {
        uint32_t w = weight[i], v = worth[i];
        if (w > capacity) continue;
        size_t count = capacity + 1 - w;
        for (size_t k0 = 0; k0 < count; k0 += LANES) {
            size_t m = count - k0 < LANES ? count - k0 : LANES;
            size_t low = capacity - k0 - (m - 1); /* the batch's lowest c */
            const uint32_t *restrict from = best + low - w;
            uint32_t *restrict to = best + low;
            for (size_t l = 0; l < m; ++l) with[l] = from[l] + v;
            for (size_t l = 0; l < m; ++l) to[l] = with[l] > to[l] ? with[l] : to[l];
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

/* ---- roots: the suite's 100,000 Newton square roots, a tile stepping together, each lane leaving as it converges */
static void roots_tiles(unsigned n, double *sum, double *steps, double *ms) {
    insn prog[16];
    at = prog;
''' + emit_program('roots', programs['roots'], {}) + '''
    Ctx ctx = {.tiles = (n + T - 1) / T, .count_lanes = n};
    double a = now_ms();
    start(prog, &ctx);
    *ms = now_ms() - a;
    *sum = *steps = 0;
    ''' + ' '.join(f'*sum += vgetq_lane_f64(ctx.acc[0].{f}, 0) + vgetq_lane_f64(ctx.acc[0].{f}, 1); *steps += vgetq_lane_f64(ctx.acc[1].{f}, 0) + vgetq_lane_f64(ctx.acc[1].{f}, 1);' for f in F) + '''
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

/* ---- bfs: the suite's graph, a search; the frontier's edges a lane each: to gathered, seen's bits asked, candidates compacted; the tail in C */
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
static uint32_t rows[LANES + 16], owner[LANES], cand[LANES];

static uint32_t bfs_search(const Graph *g, uint32_t from, bool tiles, uint64_t *seen, uint32_t *depth, uint32_t *queue, double *ms) {
    insn prog[8];
    at = prog;
    uint32_t *to = g->to;
''' + emit_program('bfs', programs['bfs'], {'rows': 'rows', 'to': 'to', 'seen': 'seen', 'cand': 'cand'}) + '''
    double a = now_ms();
    memset(seen, 0, (g->nodes + 63) / 64 * 8);
    size_t count = 1, i = 0;
    queue[0] = from, seen[from >> 6] |= UINT64_C(1) << (from & 63), depth[from] = 0;
    while (i < count) {
        size_t end = count < i + BATCH ? count : i + BATCH, n = 0;
        for (size_t r = i; r < end; ++r) {
            uint32_t v = queue[r];
            for (uint32_t e = g->start[v]; e < g->start[v + 1]; ++e) {
                if (n == LANES) { fputs("too many edges in a batch\\n", stderr); exit(1); }
                rows[n] = e, owner[n++] = v;
            }
        }
        Ctx ctx = {.tiles = (n + T - 1) / T, .count_lanes = n, .out = cand, .count = 0};
        if (tiles) start(prog, &ctx);
        else {
            static uint32_t to_[LANES], has[LANES];
            for (size_t l = 0; l < n; ++l) to_[l] = g->to[rows[l]];
            for (size_t l = 0; l < n; ++l) has[l] = seen[to_[l] >> 6] >> (to_[l] & 63) & 1;
            for (size_t l = 0; l < n; ++l) if (!has[l]) cand[ctx.count++] = (uint32_t)l;
        }
        for (size_t k = 0; k < ctx.count; ++k) {
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
    bool all = !strcmp(body, "all");
    if (all || !strcmp(body, "knapsack")) {
        unsigned n = 100, capacity = 50000;
        uint32_t *weight = malloc(4 * n), *worth = malloc(4 * n), *best = calloc(capacity + 1, 4);
        uint64_t x = 1;
        for (unsigned i = 0; i < n; ++i) x = minstd(x), weight[i] = (uint32_t)(x % 1000) + 1, x = minstd(x), worth[i] = (uint32_t)(x % 1000);
        for (int r = 0; r < rounds; ++r) {
            double ms[4], answers[4];
            memset(best, 0, 4 * (capacity + 1)), answers[0] = knapsack_tiles(n, capacity, weight, worth, best, &ms[0]);
            memset(best, 0, 4 * (capacity + 1)), answers[1] = knapsack_passes(n, capacity, weight, worth, best, &ms[1]);
            memset(best, 0, 4 * (capacity + 1)), answers[2] = knapsack_c(n, capacity, weight, worth, best, &ms[2]);
            memset(best, 0, 4 * (capacity + 1)), answers[3] = knapsack_ascending(n, capacity, weight, worth, best, &ms[3]);
            printf("{\\"body\\":\\"knapsack\\",\\"K\\":%d,\\"S\\":%d,\\"T\\":%d,\\"tiles_ms\\":%.3f,\\"passes_ms\\":%.3f,\\"ascending_ms\\":%.3f,\\"c_ms\\":%.3f,\\"same\\":%s,\\"answer\\":%.17g}\\n", K, S, T, ms[0], ms[1], ms[3], ms[2],
                   answers[0] == answers[2] && answers[1] == answers[2] && answers[3] == answers[2] ? "true" : "false", answers[0]);
        }
    }
    if (all || !strcmp(body, "roots")) {
        for (int r = 0; r < rounds; ++r) {
            double sum[2], steps[2], ms[2];
            roots_tiles(100000, &sum[0], &steps[0], &ms[0]);
            roots_c(100000, &sum[1], &steps[1], &ms[1]);
            printf("{\\"body\\":\\"roots\\",\\"K\\":%d,\\"S\\":%d,\\"T\\":%d,\\"tiles_ms\\":%.3f,\\"c_ms\\":%.3f,\\"same\\":%s,\\"sum\\":%.17g,\\"steps\\":%.0f}\\n", K, S, T, ms[0], ms[1],
                   steps[0] == steps[1] && fabs(sum[0] - sum[1]) < 1e-6 ? "true" : "false", sum[0], steps[0]);
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
            printf("{\\"body\\":\\"bfs\\",\\"K\\":%d,\\"S\\":%d,\\"T\\":%d,\\"tiles_ms\\":%.3f,\\"passes_ms\\":%.3f,\\"c_ms\\":%.3f,\\"same\\":%s,\\"reached\\":%u}\\n", K, S, T, ms[0], ms[1], ms[2],
                   reached[0] == reached[2] && reached[1] == reached[2] ? "true" : "false", reached[0]);
        }
    }
    return 0;
}''')
