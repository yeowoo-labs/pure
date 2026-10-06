/* Loops a batch of rounds at a time (docs/design/024). A count's loop
 * (LOOP) whose block the load planned PLAN_BATCH runs, when the executor
 * runs it, up to BATCH rounds as one pass over its block: each instruction
 * once for all of them, a value one per round (a lane). A value is the same
 * for every round (uniform), a whole number moving by a whole step from
 * round to round (affine: its first value and its step), or one per round
 * (lanes: numbers, or whole numbers below 2^32 kept in four bytes each). A
 * branch on lanes runs both ways, each for its own lanes (the lanes running
 * an instruction are its mask).
 *
 * The pass computes what the rounds would, and changes nothing while it
 * does: its ITEM_SETs wait for its end, and happen then only when no round
 * reads a cell an earlier round of the batch sets. A round that would fault,
 * a value or a list it has no form for, or such a read, and the batch does
 * not happen: nothing changed, and the executor runs its rounds one by one
 * (faulting where it would), then batches resume past them, until FAILURES
 * batches failed in the loop. A fault in a lane a branch left out is no
 * fault (that round does not run the instruction).
 *
 * Work counts as the executor counts it: a round's block and its LOOP, for
 * each round, and a batch takes no more rounds than the work left pays for.
 * A local the block sets is left as the last round to set it left it, and
 * the loop's slot as its last round left it. The lanes and masks are the
 * thread's (a block a batch runs runs no other block). */
#include "module-run.h"
#include <stdlib.h>

enum { BATCH = 1024, BUFFERS = 24, MASKS = 32, SPLITS = 8, READS = 16, STORES = 8, APPENDS = 8, REDUCES = 8, RECURS = 4, UPDATES = 8,
       RECORDS = 8, STEPS = 96, SUMS = 4, LOCALS = 256, LEAST = 16, FAILURES = 3 };

enum { UNIFORM, AFFINE, LANES, REDUCE }; /* REDUCE: x in x = x op e (value: its reduction) */
typedef struct {
    uint8_t kind;
    bool u32;       /* lanes: whole numbers below 2^32 in four bytes each (else numbers) */
    bool temp;      /* lanes the stack alone holds: an instruction may leave its result in them */
    uint16_t owner; /* a position's list (a slot's bytes.owner) */
    double value, step;
    void *lane;
} Val;

/* Lanes running: a 1 or 0 per lane (made when first needed: its parent's
 * but its sibling's), the lanes a branch split it from (NULL: every lane)
 * and the other part of that split, and how many run. */
typedef struct Mask { const uint32_t *lanes; const struct Mask *parent; struct Mask *sibling; unsigned count; } Mask;
/* An item's field read at rows (known: else any row), and one set: what it sets and in which lanes (NULL: all). */
typedef struct { uint32_t list, field; bool known; Val rows; } Read;
typedef struct { uint32_t list, field; Val rows, value; Mask *mask; } Store;
/* A recurrence's x = e ahead of the batch, round by round: e's instructions
 * as steps (a number, x's value, the loop's value, or an operation on the
 * numbers before it), the statement from to its set, the recurrence's index;
 * and where the block reads a recurrence, its value there in each lane. */
enum { S_NUMBER = 250, S_X, S_SLOT };
typedef struct { uint8_t op, x; double k; } Step;
typedef struct { uint32_t from, set; unsigned x, first, count; } Update;
typedef struct { uint32_t pc; unsigned x; double *lanes; } Record;
/* A sum into a cell, L[k].f = L[k].f + e: its statement's second k from
 * again to resume (past its find and read, skipped), its ADD and its set;
 * and as the pass leaves it, its rows and e in the lanes of mask, added at
 * the end round by round. */
typedef struct { uint32_t again, resume, add, set; } Sum;
typedef struct { uint32_t list, field, add, set; Val rows, e; Mask *mask; } Scatter;
/* An item a LIST_APPEND at pc appends in the lanes of mask (NULL: all): its fields. */
typedef struct { uint32_t pc; Mask *mask; unsigned count; Val cells[PURE_RECORD_FIELDS]; } Append;
/* Lanes a branch sent ahead (NULL: every lane): they run again at pc. */
typedef struct { uint32_t pc; Mask *mask; } Split;
/* A reduction (x = x op e, x read no other way): each lane's op of its rounds'
 * e (from op's identity), how many it had, whether one was not whole or a
 * lane's past 2^53 (then a lane's two may not sum as the rounds would). */
typedef struct { unsigned local; uint8_t op; double *partial; uint32_t *count; bool loose; double result; } Reduce;
/* A loop the batch's block runs, its rounds together: the lanes that came
 * to it, how many rounds each has, from what, and those that broke out. */
typedef struct { Mask *entry; uint32_t *count, *broken; Val low; } Nest;

typedef struct {
    Frame *frame;
    const PureModule *m;
    PureApp *app;
    Event *ev;
    uint32_t slot, end;         /* the loop's slot (LOCALS: a walk's, none), and where its block ends */
    int walk;                   /* a walk's list (-1: a count's loop) */
    unsigned n;
    double first;               /* the loop's value in the batch's first round */
    bool reverse;               /* lane l runs round n - 1 - l (else round l) */
    int backward;               /* the list accesses whose rows go down from lane to lane, less those going up */
    bool failed;
    Val stack[PURE_MODULE_STACK];
    unsigned sp;
    uint64_t sets[LOCALS / 64]; /* the locals the block sets */
    uint8_t state[LOCALS];      /* each this pass: 0 not set yet, 1 set in every lane, 2 in those of written */
    Mask *written[LOCALS];
    Val local[LOCALS];
    Mask *active;               /* the lanes running here (NULL: every lane) */
    Mask mask[MASKS];
    Mask *mask_free[MASKS];     /* masks no lane set holds any more */
    unsigned mask_free_count;
    Reduce reduce[REDUCES];
    unsigned reduce_count;
    Nest nest;                  /* the nested loop running (nest.count NULL: none) */
    uint64_t work;              /* the nested loops' rounds' work */
    double *var;                /* the loop's value in each lane, or NULL (not made yet) */
    double var_start, var_step; /* the loop's value as an affine value */
    bool breaks;                /* a lane broke out of the nested loop running */
    bool items;                 /* the block sets items: its reads are kept to check against them */
    bool none;                  /* no lane runs here */
    Split splits[SPLITS];
    unsigned split_count, buffers, masks, read_count, store_count, spare_count;
    void *spare[BUFFERS];       /* lanes no value holds any more */
    Read reads[READS];
    Store stores[STORES];
    Append appends[APPENDS];
    unsigned append_count;
    unsigned recur_count, update_count, record_count, step_count;
    unsigned recur_local[RECURS];
    double recur_value[RECURS]; /* each recurrence's after the rounds so far */
    Update updates[UPDATES];
    Record records[RECORDS];
    Step steps[STEPS];
    unsigned sum_count, scatter_count;
    Sum sums[SUMS];
    Scatter scatters[SUMS];
} Batch;

static _Thread_local Batch batch;

/* A lane's round in the batch (and a round's lane). */
static inline unsigned round_of(const Batch *b, unsigned l) { return b->reverse ? b->n - 1 - l : l; }
static _Thread_local double buffer[BUFFERS][BATCH + 24]; /* not 2^k apart: lanes of different buffers in different sets of the cache */
static const uint32_t no_lanes[BATCH]; /* a mask's when there were no lanes left to make it (the batch failed) */

/* ---- Values */

static Val uniform(double value, uint16_t owner) { return (Val){.kind = UNIFORM, .value = value, .owner = owner}; }

/* Whole, within 2^52 and not -0: what an affine value's parts are (each lane's value then exact, and as the rounds compute it). */
static bool whole(double x) { return x == floor(x) && fabs(x) <= 0x1p52 && !(x == 0 && signbit(x)); }
static bool whole32(double x) { return x >= 0 && x < 4294967296.0 && x == floor(x) && !(x == 0 && signbit(x)); }

static bool affine_of(Batch *b, double value, double step, Val *out) {
    if (!whole(value) || !whole(step) || !whole(value + step * (b->n - 1))) return false;
    *out = step ? (Val){.kind = AFFINE, .value = value, .step = step} : uniform(value, 0);
    return true;
}

static void *fresh(Batch *b) {
    if (b->spare_count) return b->spare[--b->spare_count];
    if (b->buffers == BUFFERS) {
        b->failed = true;
        return NULL;
    }
    return buffer[b->buffers++];
}

/* A value the stack held alone, gone: its lanes for others. */
static void release(Batch *b, const Val *v) {
    if (v->kind == LANES && v->temp) b->spare[b->spare_count++] = v->lane;
}

static Mask *new_mask(Batch *b, const uint32_t *lanes, const Mask *parent, unsigned count) {
    if (!b->mask_free_count && b->masks == MASKS) {
        b->failed = true;
        return NULL;
    }
    Mask *m = b->mask_free_count ? b->mask_free[--b->mask_free_count] : &b->mask[b->masks++];
    *m = (Mask){lanes, parent, NULL, count};
    return m;
}

/* A mask's lanes (NULL: every lane). */
static const uint32_t *mask_lanes(Batch *b, Mask *m) {
    if (!m || m->lanes) return m ? m->lanes : NULL;
    const uint32_t *parent = mask_lanes(b, (Mask *)m->parent), *other = mask_lanes(b, m->sibling);
    uint32_t *out = fresh(b);
    if (!out) return no_lanes;
    if (parent) for (unsigned l = 0; l < b->n; ++l) out[l] = parent[l] & (other[l] ^ 1);
    else for (unsigned l = 0; l < b->n; ++l) out[l] = other[l] ^ 1;
    return m->lanes = out;
}

static const uint32_t *running(Batch *b) { return mask_lanes(b, b->active); }

/* Lanes of either (two parts of a split: what it split). */
static Mask *union_of(Batch *b, Mask *x, Mask *y) {
    if (!x || !y) return NULL;
    if (x->sibling == y) return (Mask *)x->parent;
    const uint32_t *xs = mask_lanes(b, x), *ys = mask_lanes(b, y);
    uint32_t *out = fresh(b);
    if (!out) return NULL;
    unsigned count = 0;
    for (unsigned l = 0; l < b->n; ++l) out[l] = xs[l] | ys[l], count += out[l];
    return new_mask(b, out, NULL, count);
}

static inline double lane_of(const Val *v, unsigned l) {
    return v->kind == UNIFORM ? v->value : v->kind == AFFINE ? v->value + v->step * l
         : v->u32 ? ((const uint32_t *)v->lane)[l] : ((const double *)v->lane)[l];
}

/* v as numbers, one per lane (lanes of its own, or new ones); NULL when no lanes are left. */
static const double *numbers(Batch *b, const Val *v) {
    if (v->kind == LANES && !v->u32) return v->lane;
    if (v->kind == AFFINE && v->value == b->var_start && v->step == b->var_step) { /* the loop's value: made once a batch */
        if (!b->var && (b->var = fresh(b)))
            for (unsigned l = 0; l < b->n; ++l) b->var[l] = v->value + v->step * l;
        return b->var;
    }
    double *out = fresh(b);
    if (!out) return NULL;
    if (v->kind == LANES) {
        const uint32_t *from = v->lane;
        for (unsigned l = 0; l < b->n; ++l) out[l] = from[l];
    } else if (v->kind == AFFINE) {
        double value = v->value, step = v->step;
        for (unsigned l = 0; l < b->n; ++l) out[l] = value + step * l;
    } else
        for (unsigned l = 0; l < b->n; ++l) out[l] = v->value;
    return out;
}

/* v as whole numbers below 2^32 in four bytes each, or NULL: not all are (or a uniform: its value is). */
static const uint32_t *words(Batch *b, const Val *v) {
    if (v->kind == LANES) return v->u32 ? v->lane : NULL;
    if (v->kind == UNIFORM || !whole32(v->value) || !whole32(v->value + v->step * (b->n - 1))) return NULL;
    uint32_t *out = fresh(b);
    if (out)
        for (unsigned l = 0; l < b->n; ++l) out[l] = (uint32_t)(v->value + v->step * l);
    return out;
}

static Val lanes_val(void *lane, bool u32) { return (Val){.kind = LANES, .u32 = u32, .temp = true, .lane = lane}; }

/* Whether a running lane has a bad one (bad: a byte per lane). */
static bool any_running(Batch *b, const uint8_t *bad) {
    const uint32_t *run = running(b);
    uint32_t any = 0;
    if (!run)
        for (unsigned l = 0; l < b->n; ++l) any |= bad[l];
    else
        for (unsigned l = 0; l < b->n; ++l) any |= bad[l] & run[l];
    return any != 0;
}

/* Whether every running lane's number is finite (as the executor checks
 * each result): its exponent's bits not all ones, in integers (vector code). */
static bool finite_running(Batch *b, const double *x) {
    const uint32_t *run = running(b);
    const uint64_t *bits = (const uint64_t *)(const void *)x;
    uint64_t bad = 0;
    if (!run)
        for (unsigned l = 0; l < b->n; ++l) bad |= (~bits[l] & UINT64_C(0x7FF0000000000000)) == 0;
    else
        for (unsigned l = 0; l < b->n; ++l) bad |= ((~bits[l] & UINT64_C(0x7FF0000000000000)) == 0) & run[l];
    return !bad;
}

/* ---- Arithmetic */

/* x op y as an affine value: sums and differences of affine and uniform whole numbers, and products by a positive one. */
static bool affine_op(Batch *b, uint8_t op, Val *x, const Val *y) {
    if (x->kind == LANES || y->kind == LANES || (x->kind == UNIFORM && !whole(x->value)) || (y->kind == UNIFORM && !whole(y->value)))
        return false;
    double xs = x->kind == AFFINE ? x->step : 0, ys = y->kind == AFFINE ? y->step : 0;
    switch (op) {
    case PURE_OP_ADD: return affine_of(b, x->value + y->value, xs + ys, x);
    case PURE_OP_SUB: return affine_of(b, x->value - y->value, xs - ys, x);
    case PURE_OP_MUL:
        if (x->kind == UNIFORM && x->value > 0) return affine_of(b, x->value * y->value, x->value * ys, x);
        if (y->kind == UNIFORM && y->value > 0) return affine_of(b, x->value * y->value, xs * y->value, x);
        return false;
    default: return false;
    }
}

/* Each form of operands its own loop (a uniform's value, or lanes), over
 * lanes apart from the result's: the compiler makes vector code of each. */
#define EACH(type, expr)                                                                               \
    do {                                                                                               \
        if (ux) for (unsigned l = 0; l < n; ++l) { type a = xk, c = ys[l]; out[l] = (expr); }          \
        else if (uy) for (unsigned l = 0; l < n; ++l) { type a = xs[l], c = yk; out[l] = (expr); }     \
        else for (unsigned l = 0; l < n; ++l) { type a = xs[l], c = ys[l]; out[l] = (expr); }          \
    } while (0)

/* x op y over whole numbers below 2^32 in four bytes (xs or the uniform xk, ys or yk): false past them (as numbers then). */
static bool words_op(uint8_t op, uint32_t *restrict out, const uint32_t *restrict xs, uint32_t xk, bool ux,
                     const uint32_t *restrict ys, uint32_t yk, bool uy, unsigned n) {
    uint32_t over = 0;
    switch (op) {
    case PURE_OP_ADD: /* past 2^32: the sum below an operand */
        if (ux) for (unsigned l = 0; l < n; ++l) { uint32_t sum = xk + ys[l]; out[l] = sum, over |= sum < xk; }
        else if (uy) for (unsigned l = 0; l < n; ++l) { uint32_t sum = xs[l] + yk; out[l] = sum, over |= sum < yk; }
        else for (unsigned l = 0; l < n; ++l) { uint32_t sum = xs[l] + ys[l]; out[l] = sum, over |= sum < xs[l]; }
        break;
    case PURE_OP_SUB:
        EACH(uint32_t, a - c);
        if (ux) for (unsigned l = 0; l < n; ++l) over |= xk < ys[l];
        else if (uy) for (unsigned l = 0; l < n; ++l) over |= xs[l] < yk;
        else for (unsigned l = 0; l < n; ++l) over |= xs[l] < ys[l];
        break;
    case PURE_OP_MIN: EACH(uint32_t, a < c ? a : c); break;
    case PURE_OP_MAX: EACH(uint32_t, a > c ? a : c); break;
    case PURE_OP_LT: EACH(uint32_t, a < c); break;
    case PURE_OP_LE: EACH(uint32_t, a <= c); break;
    case PURE_OP_GT: EACH(uint32_t, a > c); break;
    case PURE_OP_GE: EACH(uint32_t, a >= c); break;
    case PURE_OP_EQ: EACH(uint32_t, a == c); break;
    case PURE_OP_NE: EACH(uint32_t, a != c); break;
    case PURE_OP_AND: EACH(uint32_t, a != 0 && c != 0); break;
    default: EACH(uint32_t, a != 0 || c != 0); break;
    }
    return !over;
}

/* A number's exponent bits all ones: not finite (in integers, vector code). */
static inline uint64_t not_finite(double v) {
    uint64_t bits;
    memcpy(&bits, &v, sizeof(bits));
    return (~bits & UINT64_C(0x7FF0000000000000)) == 0;
}

/* Each form of operands its own loop as EACH, each result checked finite in
 * the running lanes (run: NULL every lane) as it is made. */
#define CHECKED(expr)                                                                                             \
    do {                                                                                                          \
        uint64_t bad = 0;                                                                                         \
        if (!run) {                                                                                               \
            if (ux) for (unsigned l = 0; l < n; ++l) { double a = xk, c = ys[l], v = (expr); out[l] = v, bad |= not_finite(v); } \
            else if (uy) for (unsigned l = 0; l < n; ++l) { double a = xs[l], c = yk, v = (expr); out[l] = v, bad |= not_finite(v); } \
            else for (unsigned l = 0; l < n; ++l) { double a = xs[l], c = ys[l], v = (expr); out[l] = v, bad |= not_finite(v); } \
        } else {                                                                                                  \
            if (ux) for (unsigned l = 0; l < n; ++l) { double a = xk, c = ys[l], v = (expr); out[l] = v, bad |= not_finite(v) & run[l]; } \
            else if (uy) for (unsigned l = 0; l < n; ++l) { double a = xs[l], c = yk, v = (expr); out[l] = v, bad |= not_finite(v) & run[l]; } \
            else for (unsigned l = 0; l < n; ++l) { double a = xs[l], c = ys[l], v = (expr); out[l] = v, bad |= not_finite(v) & run[l]; } \
        }                                                                                                         \
        return !bad;                                                                                              \
    } while (0)

/* x op y as numbers, as the executor computes each: false when a running lane's is not finite (the executor's fault). */
static bool numbers_op(uint8_t op, double *restrict out, const double *restrict xs, double xk, bool ux,
                       const double *restrict ys, double yk, bool uy, unsigned n, const uint32_t *restrict run) {
    switch (op) {
    case PURE_OP_ADD: CHECKED(a + c);
    case PURE_OP_SUB: CHECKED(a - c);
    case PURE_OP_MUL: CHECKED(a * c);
    case PURE_OP_DIV: CHECKED(a / c); /* by 0: not finite, as the executor's fault */
    case PURE_OP_MOD:
        for (unsigned l = 0; l < n; ++l) {
            double a = ux ? xk : xs[l], c = uy ? yk : ys[l], value = fmod(a, c);
            if (value != 0 && (value < 0) != (c < 0)) value += c;
            else if (value == 0) value = copysign(0.0, c);
            out[l] = value;
        }
        return true; /* checked by the caller */
    case PURE_OP_MIN: EACH(double, minimum(a, c)); return true;
    default: EACH(double, maximum(a, c)); return true;
    }
}
#undef CHECKED

/* x op y for a test of numbers (a comparison, AND, OR): 1 or 0 in four bytes, a branch's mask as it is. */
static void numbers_test(uint8_t op, uint32_t *restrict out, const double *restrict xs, double xk, bool ux,
                         const double *restrict ys, double yk, bool uy, unsigned n) {
    switch (op) {
    case PURE_OP_LT: EACH(double, a < c); break;
    case PURE_OP_LE: EACH(double, a <= c); break;
    case PURE_OP_GT: EACH(double, a > c); break;
    case PURE_OP_GE: EACH(double, a >= c); break;
    case PURE_OP_EQ: EACH(double, a == c); break;
    case PURE_OP_NE: EACH(double, a != c); break;
    case PURE_OP_AND: EACH(double, a != 0 && c != 0); break;
    default: EACH(double, a != 0 || c != 0); break;
    }
}
#undef EACH

static bool word_form(uint8_t op) {
    switch (op) {
    case PURE_OP_ADD: case PURE_OP_SUB: case PURE_OP_MIN: case PURE_OP_MAX: case PURE_OP_LT: case PURE_OP_LE:
    case PURE_OP_GT: case PURE_OP_GE: case PURE_OP_EQ: case PURE_OP_NE: case PURE_OP_AND: case PURE_OP_OR: return true;
    default: return false;
    }
}

/* x op y over whole numbers below 2^32, when both are such: false when one is not, or the result would not be. */
static bool word_op(Batch *b, uint8_t op, Val *x, const Val *y) {
    if (!word_form(op) || (x->kind == UNIFORM && !whole32(x->value)) || (y->kind == UNIFORM && !whole32(y->value)) ||
        (x->kind == LANES && !x->u32) || (y->kind == LANES && !y->u32))
        return false;
    unsigned spares = b->spare_count, buffers = b->buffers;
    bool ux = x->kind == UNIFORM, uy = y->kind == UNIFORM;
    const uint32_t *xs = ux ? NULL : words(b, x), *ys = uy ? NULL : words(b, y);
    uint32_t *out = (ux || xs) && (uy || ys) ? fresh(b) : NULL;
    if (!out || !words_op(op, out, xs, ux ? (uint32_t)x->value : 0, ux, ys, uy ? (uint32_t)y->value : 0, uy, b->n)) {
        b->spare_count = spares, b->buffers = buffers; /* lanes it took back: past 2^32 or below 0 in some lane, as numbers */
        b->failed = false;
        return false;
    }
    if (xs && xs != x->lane) b->spare[b->spare_count++] = (void *)xs; /* an affine value's lanes, made for this */
    if (ys && ys != y->lane) b->spare[b->spare_count++] = (void *)ys;
    release(b, x), release(b, y);
    *x = lanes_val(out, true);
    return true;
}

/* x op y as numbers. */
static void number_lanes(Batch *b, uint8_t op, Val *x, const Val *y) {
    bool ux = x->kind == UNIFORM, uy = y->kind == UNIFORM;
    const double *xs = ux ? NULL : numbers(b, x), *ys = uy ? NULL : numbers(b, y);
    void *out = (ux || xs) && (uy || ys) ? fresh(b) : NULL;
    bool test = op >= PURE_OP_LT && op <= PURE_OP_OR && op != PURE_OP_NOT;
    if (!out) return;
    if (test) numbers_test(op, out, xs, x->value, ux, ys, y->value, uy, b->n);
    else if (!numbers_op(op, out, xs, x->value, ux, ys, y->value, uy, b->n, running(b)) || (op == PURE_OP_MOD && !finite_running(b, out)))
        b->failed = true;
    if (xs && xs != x->lane && xs != b->var) b->spare[b->spare_count++] = (void *)xs; /* numbers made for this */
    if (ys && ys != y->lane && ys != b->var) b->spare[b->spare_count++] = (void *)ys;
    release(b, x), release(b, y);
    *x = lanes_val(out, test);
}

static void reduce_step(Batch *b, Reduce *r, const Val *e);

static void binary(Batch *b, uint8_t op) {
    Val y = b->stack[--b->sp], *x = &b->stack[b->sp - 1];
    if (x->kind == REDUCE) { /* x = x op e: e into each running lane's */
        reduce_step(b, &b->reduce[(unsigned)x->value], &y);
        release(b, &y);
        return;
    }
    if (x->kind == UNIFORM && y.kind == UNIFORM) {
        double value = 0;
        if (!number_op(op, x->value, y.value, &value)) b->failed = true; /* every running lane faults */
        *x = uniform(value, 0);
        return;
    }
    if (affine_op(b, op, x, &y) || word_op(b, op, x, &y)) return;
    number_lanes(b, op, x, &y);
}

/* NEG, NOT, FLOOR of the top. */
static void unary(Batch *b, uint8_t op) {
    Val *x = &b->stack[b->sp - 1];
    if (x->kind == UNIFORM) {
        *x = uniform(op == PURE_OP_NEG ? -x->value : op == PURE_OP_NOT ? x->value == 0 : floor(x->value), 0);
        return;
    }
    if (op == PURE_OP_FLOOR && (x->kind == AFFINE || x->u32)) { /* whole already */
        x->owner = 0;
        return;
    }
    if (op == PURE_OP_NOT && x->u32) {
        uint32_t *out = fresh(b);
        if (!out) return;
        for (unsigned l = 0; l < b->n; ++l) out[l] = ((const uint32_t *)x->lane)[l] == 0;
        release(b, x);
        *x = lanes_val(out, true);
        return;
    }
    const double *xs = numbers(b, x);
    double *out = xs ? fresh(b) : NULL;
    if (!out) return;
    if (op == PURE_OP_NEG) for (unsigned l = 0; l < b->n; ++l) out[l] = -xs[l];
    else if (op == PURE_OP_NOT) for (unsigned l = 0; l < b->n; ++l) out[l] = xs[l] == 0;
    else for (unsigned l = 0; l < b->n; ++l) out[l] = floor(xs[l]);
    if (xs != x->lane && xs != b->var) b->spare[b->spare_count++] = (void *)xs;
    release(b, x);
    *x = lanes_val(out, false);
}

/* CLAMP and INDEX_MOVE: lane by lane as the executor computes them. */
static void clamp(Batch *b) {
    Val high = b->stack[--b->sp], low = b->stack[--b->sp], *x = &b->stack[b->sp - 1];
    double *out = fresh(b);
    if (!out) return;
    uint8_t bad[BATCH];
    for (unsigned l = 0; l < b->n; ++l) {
        double lo = lane_of(&low, l), hi = lane_of(&high, l);
        bad[l] = lo > hi;
        out[l] = maximum(lo, minimum(lane_of(x, l), hi));
    }
    if (any_running(b, bad)) b->failed = true;
    *x = lanes_val(out, false);
}

static void index_move(Batch *b) {
    Val by = b->stack[--b->sp], *x = &b->stack[b->sp - 1];
    size_t count = pure_list_count(b->app->lists[x->owner]);
    double last = count ? (double)(count - 1) : 0, *out = fresh(b);
    if (!out) return;
    uint8_t bad[BATCH];
    for (unsigned l = 0; l < b->n; ++l) {
        double step = lane_of(&by, l);
        bad[l] = step != floor(step);
        out[l] = maximum(0, minimum(lane_of(x, l) + step, last));
    }
    if (any_running(b, bad)) b->failed = true;
    uint16_t owner = x->owner;
    *x = lanes_val(out, false);
    x->owner = owner;
}

/* ---- Lists */

/* Rows of positions in [0, count) for every running lane (whole: positions are). */
static bool rows_in(Batch *b, const Val *rows, size_t count) {
    if (rows->kind == UNIFORM) return rows->value >= 0 && rows->value < (double)count;
    if (rows->kind == AFFINE) { /* in range at both ends: in every lane */
        double end = rows->value + rows->step * (b->n - 1);
        if (rows->value >= 0 && rows->value < (double)count && end >= 0 && end < (double)count) return true;
        if (!b->active) return false;
    }
    uint8_t bad[BATCH];
    for (unsigned l = 0; l < b->n; ++l) {
        double at = lane_of(rows, l);
        bad[l] = !(at >= 0 && at < (double)count);
    }
    return !any_running(b, bad);
}

/* A row a lane reads: its own, or 0 for a lane that does not run (its row may be any). */
static inline size_t row_of(const Val *rows, unsigned l, size_t count) {
    double at = lane_of(rows, l);
    return at >= 0 && at < (double)count ? (size_t)at : 0;
}

/* Field of cells at rows (all in range for running lanes) as a value. */
static Val cells_at(Batch *b, const PureCells *cells, unsigned field, const Val *rows) {
    uint8_t width = cells->width[field];
    const void *column = cells->columns[field];
    if (!width) return *rows; /* a DENSE list's key: its row */
    unsigned n = b->n;
    double end = rows->value + rows->step * (n - 1);
    if (rows->kind == AFFINE && rows->value >= 0 && rows->value < (double)cells->count && end >= 0 && end < (double)cells->count) {
        /* in range in every lane */
        ptrdiff_t first = (ptrdiff_t)rows->value, step = (ptrdiff_t)rows->step;
        b->backward += step < 0 ? 1 : -1;
        if (width == 8) {
            double *out = fresh(b);
            if (!out) return *rows;
            const double *from = (const double *)column + first;
            if (step == 1) memcpy(out, from, n * sizeof(*out));
            else for (unsigned l = 0; l < n; ++l) out[l] = from[(ptrdiff_t)l * step];
            return lanes_val(out, false);
        }
        uint32_t *out = fresh(b);
        if (!out) return *rows;
        if (width == 4) {
            const uint32_t *from = (const uint32_t *)column + first;
            if (step == 1) memcpy(out, from, n * sizeof(*out));
            else if (step == -1) for (unsigned l = 0; l < n; ++l) out[l] = from[-(ptrdiff_t)l];
            else for (unsigned l = 0; l < n; ++l) out[l] = from[(ptrdiff_t)l * step];
        } else {
            const uint8_t *from = (const uint8_t *)column + first;
            if (step == 1) for (unsigned l = 0; l < n; ++l) out[l] = from[l];
            else if (step == -1) for (unsigned l = 0; l < n; ++l) out[l] = from[-(ptrdiff_t)l];
            else for (unsigned l = 0; l < n; ++l) out[l] = from[(ptrdiff_t)l * step];
        }
        return lanes_val(out, true);
    }
    void *out = fresh(b);
    if (!out) return *rows;
    for (unsigned l = 0; l < n; ++l) {
        size_t at = row_of(rows, l, cells->count);
        if (width == 8) memcpy((double *)out + l, (const double *)column + at, sizeof(double));
        else ((uint32_t *)out)[l] = width == 4 ? ((const uint32_t *)column)[at] : ((const uint8_t *)column)[at];
    }
    return lanes_val(out, width != 8);
}

/* Whether the block set field of list before here (a read of it then would not see that). */
static bool stored(const Batch *b, uint32_t list, uint32_t field) {
    for (unsigned s = 0; s < b->store_count; ++s)
        if (b->stores[s].list == list && b->stores[s].field == field) return true;
    return false;
}

static void read_of(Batch *b, uint32_t list, uint32_t field, bool known, const Val *rows) {
    if (!b->items) return; /* nothing set: no read to check */
    if (b->read_count == READS) {
        b->failed = true;
        return;
    }
    Read *r = &b->reads[b->read_count++];
    r->list = list, r->field = field, r->known = known, r->rows = *rows;
    if (rows->kind == LANES) b->reads[b->read_count - 1].rows.temp = false;
}

/* ITEM_GET: field of the items at the top's positions. */
static void item_get(Batch *b, uint32_t list, uint32_t field) {
    Val *x = &b->stack[b->sp - 1];
    PureList *items = b->app->lists[list];
    size_t count = pure_list_count(items);
    if (stored(b, list, field) || !rows_in(b, x, count)) {
        b->failed = true; /* past the items: the executor's fault */
        return;
    }
    if (x->kind == LANES) x->temp = false; /* the read keeps its rows */
    read_of(b, list, field, true, x);
    if (x->kind == UNIFORM) {
        *x = uniform(pure_list_get(items, (size_t)x->value, field).num, 0);
        return;
    }
    PureCells cells;
    if (pure_list_cells(items, &cells) && cells.width[field] != 255) {
        *x = cells_at(b, &cells, field, x);
        x->owner = 0;
        return;
    }
    double *out = fresh(b); /* a tree, or HASHED: the item at each lane's position */
    if (!out) return;
    for (unsigned l = 0; l < b->n; ++l) out[l] = pure_list_get(items, row_of(x, l, count), field).num;
    *x = lanes_val(out, false);
}

/* LIST_FIND and LIST_HAS at pc, as the executor finds (a FIND joined to its ITEM_GET: the field too); false: it faulted. */
static bool find(Batch *b, const PureOp *op, uint32_t pc) {
    const PureModule *m = b->m;
    uint32_t list = op->arg, key = m->state_key[list] - 1u, field = 0;
    PureList *keyed = b->app->lists[list];
    bool has = op->op == PURE_OP_LIST_HAS, joined = !has && op->plan & PLAN_JOINED, found;
    if (joined) field = item_field(m->code[pc + 1].arg);
    if (joined && stored(b, list, field)) return false;
    Val *x = &b->stack[b->sp - 1];
    PureCells cells;
    bool dense = pure_list_cells(keyed, &cells) && cells.width[key] == 0; /* DENSE with every key below its count: a key its row */
    if (x->kind == UNIFORM) {
        PureCell cell = {.num = x->value};
        if (joined) {
            PureCell value;
            if (!pure_list_lookup(keyed, key, cell, field, &value)) return false;
            Val rows = uniform(x->value, 0);
            read_of(b, list, field, dense, &rows);
            *x = uniform(value.num, 0);
            return true;
        }
        if (has && pure_list_contains(keyed, cell.num, &found)) {
            *x = uniform(found, 0);
            return true;
        }
        size_t at = op->plan & PLAN_ROW ? pure_list_find_row(keyed, key, cell, &found) : pure_list_seek(keyed, key, cell, &found);
        if (has) {
            *x = uniform(found, 0);
            return true;
        }
        if (!found) return false;
        *x = uniform((double)at, (uint16_t)list);
        return true;
    }
    if (dense) { /* a key its row, if whole and below the count */
        if (has) {
            uint32_t *out = fresh(b);
            if (!out) return true;
            for (unsigned l = 0; l < b->n; ++l) {
                double k = lane_of(x, l);
                out[l] = k >= 0 && k < (double)cells.count && k == floor(k);
            }
            *x = lanes_val(out, true);
            return true;
        }
        if (x->kind == LANES) {
            uint8_t bad[BATCH];
            for (unsigned l = 0; l < b->n; ++l) {
                double k = lane_of(x, l);
                bad[l] = !(k >= 0 && k < (double)cells.count && k == floor(k));
            }
            if (any_running(b, bad)) return false;
            if (!x->u32) { /* the rows: -0 is the key 0's */
                double *rows = fresh(b);
                if (!rows) return true;
                for (unsigned l = 0; l < b->n; ++l) rows[l] = ((const double *)x->lane)[l] + 0.0;
                *x = lanes_val(rows, false);
            }
        } else if (!rows_in(b, x, cells.count))
            return false;
        x->owner = (uint16_t)list;
        if (joined) {
            read_of(b, list, field, true, x);
            *x = cells_at(b, &cells, field, x);
            x->owner = 0;
        }
        return true;
    }
    double *out = fresh(b); /* any other: each lane's key found as the executor finds it */
    const uint32_t *run = running(b);
    if (!out) return true;
    uint8_t bad[BATCH];
    for (unsigned l = 0; l < b->n; ++l) {
        PureCell cell = {.num = lane_of(x, l)}, value = {0};
        size_t at = 0;
        found = false;
        if (!run || run[l]) {
            if (joined) found = pure_list_lookup(keyed, key, cell, field, &value);
            else if (!(has && pure_list_contains(keyed, cell.num, &found)))
                at = op->plan & PLAN_ROW ? pure_list_find_row(keyed, key, cell, &found) : pure_list_seek(keyed, key, cell, &found);
        }
        bad[l] = !has && !found;
        out[l] = joined ? value.num : has ? found : (double)at;
    }
    if (!has && any_running(b, bad)) return false;
    if (joined) {
        Val rows = {.kind = UNIFORM};
        read_of(b, list, field, false, &rows);
    }
    *x = lanes_val(out, false);
    if (!has && !joined) x->owner = (uint16_t)list;
    return true;
}

/* ITEM_SET: kept for the end (the value's lanes kept with it). */
static void item_set(Batch *b, uint32_t list, uint32_t field) {
    Val value = b->stack[--b->sp], rows = b->stack[--b->sp];
    if (b->store_count == STORES || (int)field == b->m->state_key[list] - 1) {
        b->failed = true;
        return;
    }
    if (value.kind == LANES) value.temp = false;
    if (rows.kind == LANES) rows.temp = false;
    b->stores[b->store_count++] = (Store){list, field, rows, value, b->active};
}

/* ---- Locals and branches */

static void local_store(Batch *b, uint32_t arg) {
    unsigned at = arg & 0xFF;
    Val v = b->stack[--b->sp];
    if (v.kind == REDUCE) return; /* x = x op e: in its lanes already */
    if (at == b->slot) {
        b->failed = true;
        return;
    }
    if (v.kind == LANES) v.temp = false;
    if (!b->active) {
        b->local[at] = v, b->state[at] = 1;
        return;
    }
    if (!b->state[at]) { /* its first set this pass: in the running lanes */
        b->local[at] = v, b->state[at] = 2, b->written[at] = b->active;
        return;
    }
    const uint32_t *run = running(b);
    double *out = fresh(b); /* the running lanes' new, the others' as they were */
    if (!out) return;
    for (unsigned l = 0; l < b->n; ++l) out[l] = run[l] ? lane_of(&v, l) : lane_of(&b->local[at], l);
    b->local[at] = lanes_val(out, false);
    b->local[at].temp = false;
    if (b->state[at] == 2 && !(b->written[at] = union_of(b, b->written[at], b->active))) b->state[at] = 1;
}

static Reduce *reduction(Batch *b, unsigned local, uint8_t op);

static void local_load(Batch *b, uint32_t arg, uint32_t pc) {
    unsigned at = arg & 0xFF;
    Val v;
    uint32_t by;
    if (b->sets[at / 64] >> (at % 64) & 1 && !b->state[at] && at != b->slot && pure_batch_reduces(b->m, pc, b->end, &by)) {
        Reduce *r = reduction(b, at, b->m->code[by].op); /* not set yet: an earlier round's, as x = x op e reads it */
        if (r) b->stack[b->sp++] = (Val){.kind = REDUCE, .value = (double)(r - b->reduce)};
        return;
    }
    if (at == b->slot) {
        double start = b->reverse ? b->first + (b->n - 1) : b->first, step = b->reverse ? -1 : 1;
        if (!affine_of(b, start, step, &v)) {
            double *out = fresh(b);
            if (!out) return;
            for (unsigned l = 0; l < b->n; ++l) out[l] = start + step * l;
            v = lanes_val(out, false);
        }
    } else if (!(b->sets[at / 64] >> (at % 64) & 1)) v = uniform(b->ev->locals[at].num, (uint16_t)(arg >> 16));
    else if (b->state[at] == 1) v = b->local[at];
    else { /* set this pass in some lanes: every running lane must have (else its value is an earlier round's) */
        const uint32_t *run = running(b), *set = b->state[at] == 2 ? mask_lanes(b, b->written[at]) : NULL;
        uint32_t missing = !set || !run;
        for (unsigned l = 0; !missing && l < b->n; ++l) missing |= run[l] & (set[l] ^ 1);
        if (missing) {
            b->failed = true;
            return;
        }
        v = b->local[at];
    }
    v.owner = (uint16_t)(arg >> 16);
    b->stack[b->sp++] = v;
}

/* Lanes (mask: NULL every lane) sent to pc. */
static void send(Batch *b, uint32_t pc, Mask *mask) {
    if (b->split_count == SPLITS) {
        b->failed = true;
        return;
    }
    b->splits[b->split_count++] = (Split){pc, mask};
}

/* The lanes sent to pc join those running into it. */
static void arrive(Batch *b, uint32_t pc) {
    for (unsigned s = 0; s < b->split_count;) {
        if (b->splits[s].pc != pc) {
            ++s;
            continue;
        }
        Mask *mask = b->splits[s].mask;
        b->splits[s] = b->splits[--b->split_count];
        if (b->none) b->active = mask, b->none = false;
        else b->active = union_of(b, b->active, mask);
    }
    if (b->active && b->active->count == b->n) b->active = NULL; /* every lane again */
}

/* JZ to pc: the lanes whose top is 0 go there. */
static void branch(Batch *b, uint32_t pc) {
    Val c = b->stack[--b->sp];
    if (c.kind == UNIFORM) {
        if (c.value == 0) send(b, pc, b->active), b->none = true;
        return;
    }
    const uint32_t *parent = running(b), *on;
    if (c.kind == LANES && c.u32 && !parent) on = c.lane; /* a comparison's 1s and 0s: the lanes going on */
    else {
        uint32_t *out = fresh(b);
        if (!out) return;
        if (c.kind == LANES && c.u32) { /* a comparison's 1s and 0s, in the running lanes */
            const uint32_t *yes = c.lane;
            for (unsigned l = 0; l < b->n; ++l) out[l] = parent[l] & yes[l];
        } else
            for (unsigned l = 0; l < b->n; ++l) out[l] = (parent ? parent[l] : 1) & (lane_of(&c, l) != 0);
        release(b, &c);
        on = out;
    }
    unsigned count = 0;
    for (unsigned l = 0; l < b->n; ++l) count += on[l];
    Mask *yes = new_mask(b, on, b->active, count), *no = new_mask(b, NULL, b->active, (b->active ? b->active->count : b->n) - count);
    if (!yes || !no) return;
    yes->sibling = no, no->sibling = yes;
    if (no->count) send(b, pc, no);
    if (count) b->active = yes;
    else b->none = true;
}

/* ---- Reductions */

bool pure_batch_reduces(const PureModule *m, uint32_t pc, uint32_t end, uint32_t *at) {
    unsigned h = m->height[pc], x = m->code[pc].arg & 0xFF;
    for (uint32_t i = pc + 1; i + 1 < end; ++i) {
        const PureOp *op = &m->code[i];
        if (op->op == PURE_OP_JZ || op->op == PURE_OP_JMP || op->op == PURE_OP_LOOP || op->op == PURE_OP_BREAK || m->height[i] < h + 1)
            return false;
        if (m->height[i + 1] == h + 1) { /* x taken: by op, with e */
            *at = i;
            return m->height[i] == h + 2 && (op->op == PURE_OP_ADD || op->op == PURE_OP_MIN || op->op == PURE_OP_MAX) &&
                   m->code[i + 1].op == PURE_OP_LOCAL_STORE && (m->code[i + 1].arg & 0xFF) == x;
        }
    }
    return false;
}

/* Local's reduction by op, begun from op's identity in every lane (-0 a sum's: -0 + e is e). */
static Reduce *reduction(Batch *b, unsigned local, uint8_t op) {
    for (unsigned r = 0; r < b->reduce_count; ++r)
        if (b->reduce[r].local == local) {
            if (b->reduce[r].op == op) return &b->reduce[r];
            b->failed = true; /* x = x + e here, x = max(x, e) there: not one reduction */
            return NULL;
        }
    double *partial = b->reduce_count < REDUCES ? fresh(b) : NULL;
    uint32_t *count = partial ? fresh(b) : NULL;
    if (!count) {
        b->failed = true;
        return NULL;
    }
    double identity = op == PURE_OP_ADD ? -0.0 : op == PURE_OP_MIN ? INFINITY : -INFINITY;
    for (unsigned l = 0; l < b->n; ++l) partial[l] = identity, count[l] = 0;
    Reduce *r = &b->reduce[b->reduce_count++];
    *r = (Reduce){local, op, partial, count, false, 0};
    return r;
}

/* x = x op e in each running lane: e into its partial. */
static void reduce_step(Batch *b, Reduce *r, const Val *e) {
    const uint32_t *run = running(b);
    double *p = r->partial;
    uint32_t *c = r->count;
    unsigned n = b->n;
    if (e->kind == UNIFORM && r->op == PURE_OP_ADD) { /* a count's +1, say */
        double v = e->value;
        if (!run) for (unsigned l = 0; l < n; ++l) p[l] += v, c[l] += 1;
        else for (unsigned l = 0; l < n; ++l) p[l] = run[l] ? p[l] + v : p[l], c[l] += run[l];
        r->loose |= v != floor(v);
        if (v == floor(v) && fabs(v) <= 0x1p20) return; /* a lane's at most 2^31 of them: below 2^53 */
    } else {
        const double *es = numbers(b, e);
        if (!es) return;
        uint8_t fraction = 0;
        for (unsigned l = 0; l < n; ++l) {
            if (run && !run[l]) continue;
            double v = es[l];
            p[l] = r->op == PURE_OP_ADD ? p[l] + v : r->op == PURE_OP_MIN ? minimum(p[l], v) : maximum(p[l], v);
            ++c[l], fraction |= v != floor(v);
        }
        if (es != e->lane && es != b->var) b->spare[b->spare_count++] = (void *)es;
        r->loose |= r->op == PURE_OP_ADD && fraction;
    }
    if (r->op == PURE_OP_ADD) { /* past 2^53, a lane's sum of whole numbers may round */
        uint8_t far = 0;
        for (unsigned l = 0; l < n; ++l) far |= !(fabs(p[l]) <= 0x1p53);
        r->loose |= far;
    }
}

/* ---- Recurrences */

bool pure_batch_recurs(const PureModule *m, uint32_t s, uint32_t start, const uint64_t *set, unsigned slot, uint32_t *from) {
    unsigned x = m->code[s].arg & 0xFF;
    uint32_t p = s;
    while (p > start && m->height[p]) --p; /* its statement's start: nothing on the stack */
    if (m->height[p]) return false;
    for (uint32_t at = p; at < s; ++at) {
        const PureOp *op = &m->code[at];
        unsigned local = op->arg & 0xFF;
        switch (op->op) {
        case PURE_OP_CONST: if (op->arg == 2) return false; break; /* an i64 */
        case PURE_OP_FIELD: case PURE_OP_LOAD: break;
        case PURE_OP_LOCAL_LOAD:
            if (local != x && local != slot && set[local / 64] >> (local % 64) & 1) return false;
            break;
        case PURE_OP_ADD: case PURE_OP_SUB: case PURE_OP_MUL: case PURE_OP_DIV: case PURE_OP_MOD: case PURE_OP_MIN: case PURE_OP_MAX:
        case PURE_OP_NEG: case PURE_OP_NOT: case PURE_OP_FLOOR: case PURE_OP_LT: case PURE_OP_LE: case PURE_OP_GT: case PURE_OP_GE:
        case PURE_OP_EQ: case PURE_OP_NE: case PURE_OP_AND: case PURE_OP_OR: break;
        default: return false;
        }
    }
    *from = p;
    return true;
}

/* A field the message (or the arguments) gives, as FIELD reads it. */
static double field_value(const Batch *b, uint32_t arg) {
    const PureField *field = &b->m->field[b->ev->shape->field_start + arg];
    const unsigned char *given = (b->ev->arguments ? b->app->arguments.data : b->app->payload.data) + field->offset;
    return field->type == PURE_FIELD_U32 ? read32(given) : field->type == PURE_FIELD_BOOL ? given[0] : read_double(given);
}

/* The recurrences of the batch's block (their sets planned PLAN_RECUR): each
 * x = e as steps, and each other read of x a record. false: more than it holds. */
static bool recurrences(Batch *b, uint32_t start, uint32_t end) {
    const PureModule *m = b->m;
    for (uint32_t pc = start; pc < end; ++pc) {
        const PureOp *op = &m->code[pc];
        if (op->op == PURE_OP_LOOP) pc += op->arg >> 16;
        if (op->op != PURE_OP_LOCAL_STORE || !(op->plan & PLAN_RECUR)) continue;
        unsigned local = op->arg & 0xFF, x = 0;
        while (x < b->recur_count && b->recur_local[x] != local) ++x;
        if (x == b->recur_count) {
            if (x == RECURS) return false;
            b->recur_local[x] = local, b->recur_value[x] = b->ev->locals[local].num, ++b->recur_count;
        }
        uint32_t from;
        if (b->update_count == UPDATES || !pure_batch_recurs(m, pc, start, b->sets, b->slot, &from)) return false;
        Update *u = &b->updates[b->update_count++];
        *u = (Update){from, pc, x, b->step_count, 0};
        for (uint32_t at = from; at < pc; ++at) {
            const PureOp *e = &m->code[at];
            unsigned l = e->arg & 0xFF, r = 0;
            if (b->step_count == STEPS) return false;
            Step *step = &b->steps[b->step_count++];
            *step = (Step){e->op, 0, 0};
            if (e->op == PURE_OP_CONST) step->op = S_NUMBER, step->k = e->value;
            else if (e->op == PURE_OP_FIELD) step->op = S_NUMBER, step->k = field_value(b, e->arg);
            else if (e->op == PURE_OP_LOAD) step->op = S_NUMBER, step->k = b->frame->values[e->arg].num;
            else if (e->op == PURE_OP_LOCAL_LOAD && l == b->slot) step->op = S_SLOT;
            else if (e->op == PURE_OP_LOCAL_LOAD) {
                while (r < b->recur_count && b->recur_local[r] != l) ++r;
                if (r < b->recur_count) step->op = S_X, step->x = (uint8_t)r;
                else step->op = S_NUMBER, step->k = b->ev->locals[l].num; /* the block does not set it */
            }
        }
        u->count = b->step_count - u->first;
    }
    for (uint32_t pc = start; pc < end; ++pc) { /* the block's other reads of a recurrence */
        const PureOp *op = &m->code[pc];
        if (op->op == PURE_OP_LOOP) pc += op->arg >> 16;
        if (op->op != PURE_OP_LOCAL_LOAD) continue;
        unsigned x = 0, u = 0;
        while (x < b->recur_count && b->recur_local[x] != (op->arg & 0xFF)) ++x;
        while (u < b->update_count && !(pc >= b->updates[u].from && pc < b->updates[u].set)) ++u;
        if (x == b->recur_count || u < b->update_count) continue;
        if (b->record_count == RECORDS || !(b->records[b->record_count].lanes = fresh(b))) return false;
        b->records[b->record_count].pc = pc, b->records[b->record_count++].x = x;
    }
    return true;
}

/* The recurrences round by round, in the rounds' order (as the executor computes them): each record's lane. false: a round faults. */
static bool recur(Batch *b) {
    double stack[PURE_MODULE_STACK];
    if (!b->update_count) return true;
    for (unsigned r = 0; r < b->n; ++r) {
        unsigned lane = round_of(b, r), u = 0, k = 0;
        double slot = r ? b->first + (double)r : b->first;
        while (u < b->update_count || k < b->record_count) { /* in the block's order */
            if (k < b->record_count && b->records[k].pc < (u < b->update_count ? b->updates[u].from : UINT32_MAX)) {
                b->records[k].lanes[lane] = b->recur_value[b->records[k].x];
                ++k;
                continue;
            }
            const Update *up = &b->updates[u++];
            unsigned sp = 0;
            for (unsigned i = up->first; i < up->first + up->count; ++i) {
                const Step *step = &b->steps[i];
                double v;
                switch (step->op) { /* as the executor computes each (a fault: not finite, or 0 divided by) */
                case S_NUMBER: stack[sp++] = step->k; continue;
                case S_X: stack[sp++] = b->recur_value[step->x]; continue;
                case S_SLOT: stack[sp++] = slot; continue;
                case PURE_OP_NEG: stack[sp - 1] = -stack[sp - 1]; continue;
                case PURE_OP_NOT: stack[sp - 1] = stack[sp - 1] == 0; continue;
                case PURE_OP_FLOOR: stack[sp - 1] = floor(stack[sp - 1]); continue;
                case PURE_OP_ADD: --sp, v = stack[sp - 1] + stack[sp]; break;
                case PURE_OP_SUB: --sp, v = stack[sp - 1] - stack[sp]; break;
                case PURE_OP_MUL: --sp, v = stack[sp - 1] * stack[sp]; break;
                case PURE_OP_DIV:
                    if (stack[--sp] == 0) return false;
                    v = stack[sp - 1] / stack[sp];
                    break;
                default:
                    --sp;
                    if (!number_op(step->op, stack[sp - 1], stack[sp], &v)) return false;
                }
                if (not_finite(v)) return false;
                stack[sp - 1] = v;
            }
            b->recur_value[up->x] = stack[0];
        }
    }
    return true;
}

/* ---- Sums into cells */

/* Whether instructions [a, a + n) and [b, b + n) are the same (a key computed twice). */
static bool same_code(const PureModule *m, uint32_t a, uint32_t b, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i)
        if (m->code[a + i].op != m->code[b + i].op || m->code[a + i].arg != m->code[b + i].arg ||
            m->code[a + i].integer != m->code[b + i].integer)
            return false;
    return true;
}

/* Whether code [a, b) reads list (its cells or keys), branches or loops. */
static bool touches(const PureModule *m, uint32_t a, uint32_t b, uint32_t list) {
    for (uint32_t at = a; at < b; ++at) {
        const PureOp *op = &m->code[at];
        if (op->op == PURE_OP_JZ || op->op == PURE_OP_JMP || op->op == PURE_OP_LOOP || op->op == PURE_OP_BREAK) return true;
        if ((op->op == PURE_OP_ITEM_GET && list_of(op->arg) == list) ||
            ((op->op == PURE_OP_LIST_FIND || op->op == PURE_OP_LIST_HAS || op->op == PURE_OP_LIST_COUNT) && op->arg == list))
            return true;
    }
    return false;
}

/* The block's statements L[k].f = L[k].f + e (k computed twice the same,
 * reading no L; e reading no L; the set at k's row as its find found it). */
static bool sums(Batch *b, uint32_t start, uint32_t end) {
    const PureModule *m = b->m;
    for (uint32_t set = start; set < end; ++set) {
        const PureOp *op = &m->code[set];
        if (op->op == PURE_OP_LOOP) set += op->arg >> 16;
        if (op->op != PURE_OP_ITEM_SET || !(op->plan & PLAN_ROW) || set < start + 4 || m->code[set - 1].op != PURE_OP_ADD) continue;
        uint32_t list = list_of(op->arg), from = set;
        while (from > start && m->height[from]) --from; /* the statement's start */
        uint32_t find = from;
        while (find < set && !(m->code[find].op == PURE_OP_LIST_FIND && m->code[find].arg == list && m->height[find] == 1)) ++find;
        uint32_t n = find - from, again = find + 1, read = again + n;
        if (find == set || read + 2 > set - 1 || !(m->code[find].plan & PLAN_ROW) || m->height[from] || !same_code(m, from, again, n) ||
            m->code[read].op != PURE_OP_LIST_FIND || m->code[read].arg != list || !(m->code[read].plan & PLAN_JOINED) ||
            m->code[read + 1].op != PURE_OP_ITEM_GET || m->code[read + 1].arg != op->arg || touches(m, from, find, list) ||
            touches(m, read + 2, set - 1, list) || m->height[read + 2] != 2 || m->height[set - 1] != 3)
            continue;
        if (b->sum_count == SUMS) return false;
        b->sums[b->sum_count++] = (Sum){again, read + 2, set - 1, set};
    }
    return true;
}

/* ---- The pass */

static void nested(Batch *b, const PureOp *op, uint32_t pc);

/* The block from start to end once for the batch's lanes: false where it failed. */
static bool pass(Batch *b, uint32_t start, uint32_t end) {
    const PureModule *m = b->m;
    Event *ev = b->ev;
    PureApp *app = b->app;
    for (uint32_t pc = start; pc < end && !b->failed;) {
        arrive(b, pc);
        if (b->none) { /* no lane here: on to where one goes */
            uint32_t next = end;
            for (unsigned s = 0; s < b->split_count; ++s)
                if (b->splits[s].pc < next) next = b->splits[s].pc;
            pc = next;
            continue;
        }
        unsigned u = 0, k = 0;
        while (k < b->sum_count && b->sums[k].again != pc && b->sums[k].add != pc && b->sums[k].set != pc) ++k;
        if (k < b->sum_count) { /* a sum into a cell: its row and e, kept for the end */
            const Sum *sum = &b->sums[k];
            if (pc == sum->again) pc = sum->resume; /* the key again, its find and the cell's read: not needed */
            else if (pc == sum->add) pc = sum->set;
            else {
                const PureOp *set = &m->code[pc++];
                if (b->scatter_count == SUMS) {
                    b->failed = true;
                    break;
                }
                Scatter *sc = &b->scatters[b->scatter_count++];
                sc->list = list_of(set->arg), sc->field = item_field(set->arg), sc->add = sum->add, sc->set = sum->set;
                sc->e = b->stack[--b->sp], sc->rows = b->stack[--b->sp], sc->mask = b->active;
                sc->e.temp = sc->rows.temp = false;
            }
            continue;
        }
        while (u < b->update_count && b->updates[u].from != pc) ++u;
        if (u < b->update_count) { /* a recurrence's x = e: made ahead, round by round */
            pc = b->updates[u].set + 1;
            continue;
        }
        const PureOp *op = &m->code[pc];
        uint32_t arg = op->arg;
        ++pc;
        switch (op->op) {
        case PURE_OP_CONST: b->stack[b->sp++] = uniform(op->value, 0); break;
        case PURE_OP_LOAD: b->stack[b->sp++] = uniform(b->frame->values[arg].num, 0); break;
        case PURE_OP_FIELD: {
            const PureField *field = &m->field[ev->shape->field_start + arg];
            const unsigned char *given = (ev->arguments ? app->arguments.data : app->payload.data) + field->offset;
            b->stack[b->sp++] = uniform(field->type == PURE_FIELD_U32 ? read32(given) : field->type == PURE_FIELD_BOOL ? given[0]
                                                                                                                   : read_double(given), 0);
            break;
        }
        case PURE_OP_INDEX_LOAD: b->stack[b->sp++] = uniform(b->frame->values[arg].num, m->state_ref[arg]); break;
        case PURE_OP_ROW:
            if ((int)arg == b->walk) { /* the walk's item: each lane's place */
                Val v;
                if (!affine_of(b, b->reverse ? b->first + (b->n - 1) : b->first, b->reverse ? -1 : 1, &v)) {
                    b->failed = true;
                    break;
                }
                v.owner = (uint16_t)arg;
                b->stack[b->sp++] = v;
            } else
                b->stack[b->sp++] = uniform((double)ev->row, (uint16_t)arg);
            break;
        case PURE_OP_OUTER_ROW: b->stack[b->sp++] = uniform((double)ev->outer, (uint16_t)arg); break;
        case PURE_OP_LIST_COUNT: b->stack[b->sp++] = uniform((double)pure_list_count(app->lists[arg]), 0); break;
        case PURE_OP_INDEX_FIRST: case PURE_OP_INDEX_LAST: {
            size_t count = op->op == PURE_OP_INDEX_LAST ? pure_list_count(app->lists[arg]) : 0;
            b->stack[b->sp++] = uniform(count ? (double)(count - 1) : 0, (uint16_t)arg);
            break;
        }
        case PURE_OP_INDEX_NUM: break;
        case PURE_OP_PICK: {
            Val *v = &b->stack[b->sp - 1 - arg];
            v->temp = false;
            b->stack[b->sp++] = *v;
            break;
        }
        case PURE_OP_SLIDE:
            for (unsigned d = b->sp - 1 - arg; d + 1 < b->sp; ++d) release(b, &b->stack[d]);
            b->stack[b->sp - 1 - arg] = b->stack[b->sp - 1], b->sp -= arg;
            break;
        case PURE_OP_LOCAL_LOAD: {
            unsigned k = 0;
            while (k < b->record_count && b->records[k].pc != pc - 1) ++k;
            if (k == b->record_count) local_load(b, arg, pc - 1);
            else { /* a recurrence's value here, round by round */
                Val v = lanes_val(b->records[k].lanes, false);
                v.temp = false, v.owner = (uint16_t)(arg >> 16);
                b->stack[b->sp++] = v;
            }
            break;
        }
        case PURE_OP_LOCAL_STORE: local_store(b, arg); break;
        case PURE_OP_NEG: case PURE_OP_NOT: case PURE_OP_FLOOR: unary(b, op->op); break;
        case PURE_OP_CLAMP: clamp(b); break;
        case PURE_OP_INDEX_MOVE: index_move(b); break;
        case PURE_OP_ITEM_GET: item_get(b, list_of(arg), item_field(arg)); break;
        case PURE_OP_ITEM_SET: item_set(b, list_of(arg), item_field(arg)); break;
        case PURE_OP_LIST_APPEND: { /* kept for the end, made there round by round as the executor makes it */
            unsigned count = m->record[m->state_ref[arg]].count;
            if (b->append_count == APPENDS) {
                b->failed = true;
                break;
            }
            Append *a = &b->appends[b->append_count++];
            a->pc = pc - 1, a->mask = b->active, a->count = count;
            b->sp -= count;
            for (unsigned f = 0; f < count; ++f) {
                a->cells[f] = b->stack[b->sp + f];
                a->cells[f].temp = false;
            }
            break;
        }
        case PURE_OP_LIST_FIND: case PURE_OP_LIST_HAS:
            if (!find(b, op, pc - 1)) b->failed = true;
            else if (op->op == PURE_OP_LIST_FIND && op->plan & PLAN_JOINED) ++pc; /* its ITEM_GET with it */
            break;
        case PURE_OP_JZ: branch(b, arg); break;
        case PURE_OP_LOOP: nested(b, op, pc - 1), pc += arg >> 16; break;
        case PURE_OP_BREAK: { /* the running lanes leave the nested loop */
            const uint32_t *run = running(b);
            uint32_t *broken = b->nest.broken;
            if (!broken) {
                b->failed = true; /* none: no batch form (the load plans none such) */
                break;
            }
            if (!run) for (unsigned l = 0; l < b->n; ++l) broken[l] = 1;
            else for (unsigned l = 0; l < b->n; ++l) broken[l] |= run[l];
            b->none = b->breaks = true;
            break;
        }
        case PURE_OP_JMP: send(b, arg, b->active), b->none = true; break;
        case PURE_OP_ADD: case PURE_OP_SUB: case PURE_OP_MUL: case PURE_OP_DIV: case PURE_OP_MOD: case PURE_OP_MIN:
        case PURE_OP_MAX: case PURE_OP_LT: case PURE_OP_LE: case PURE_OP_GT: case PURE_OP_GE: case PURE_OP_EQ:
        case PURE_OP_NE: case PURE_OP_AND: case PURE_OP_OR: binary(b, op->op); break;
        default: b->failed = true; /* no batch form (the load plans none such) */
        }
    }
    return !b->failed;
}


/* ---- Nested loops: their rounds together */

static void mark_lanes(bool *kept, const void *lanes) {
    uintptr_t at = (uintptr_t)lanes, first = (uintptr_t)&buffer[0][0], past = (uintptr_t)&buffer[BUFFERS - 1][BATCH + 24];
    if (at >= first && at < past) kept[(at - first) / sizeof(buffer[0])] = true;
}

static void mark_mask(Batch *b, bool *kept, bool *kept_mask, const Mask *m) {
    for (; m && !kept_mask[m - b->mask]; m = m->parent) {
        kept_mask[m - b->mask] = true;
        mark_lanes(kept, m->lanes);
        if (m->sibling) mark_mask(b, kept, kept_mask, m->sibling);
    }
}

static void mark_val(bool *kept, const Val *v) {
    if (v->lane) mark_lanes(kept, v->lane);
}

/* Between a nested loop's rounds (the stack empty): the lanes and masks
 * nothing holds (no local, reduction, nest, read, set or running lanes) free again. */
static void sweep(Batch *b) {
    bool kept[BUFFERS] = {false}, kept_mask[MASKS] = {false};
    for (unsigned w = 0; w < LOCALS / 64; ++w)
        for (uint64_t bits = b->sets[w]; bits; bits &= bits - 1) {
            unsigned at = w * 64 + (unsigned)__builtin_ctzll(bits);
            if (b->state[at]) mark_val(kept, &b->local[at]);
            if (b->state[at] == 2) mark_mask(b, kept, kept_mask, b->written[at]);
        }
    mark_mask(b, kept, kept_mask, b->active);
    mark_mask(b, kept, kept_mask, b->nest.entry);
    mark_lanes(kept, b->var);
    mark_lanes(kept, b->nest.count), mark_lanes(kept, b->nest.broken), mark_val(kept, &b->nest.low);
    for (unsigned r = 0; r < b->reduce_count; ++r) mark_lanes(kept, b->reduce[r].partial), mark_lanes(kept, b->reduce[r].count);
    for (unsigned r = 0; r < b->read_count; ++r) mark_val(kept, &b->reads[r].rows);
    for (unsigned s = 0; s < b->store_count; ++s)
        mark_val(kept, &b->stores[s].rows), mark_val(kept, &b->stores[s].value), mark_mask(b, kept, kept_mask, b->stores[s].mask);
    for (unsigned d = 0; d < b->sp; ++d) mark_val(kept, &b->stack[d]);
    for (unsigned r = 0; r < b->record_count; ++r) mark_lanes(kept, b->records[r].lanes);
    for (unsigned i = 0; i < b->scatter_count; ++i)
        mark_val(kept, &b->scatters[i].rows), mark_val(kept, &b->scatters[i].e), mark_mask(b, kept, kept_mask, b->scatters[i].mask);
    for (unsigned a = 0; a < b->append_count; ++a) {
        mark_mask(b, kept, kept_mask, b->appends[a].mask);
        for (unsigned f = 0; f < b->appends[a].count; ++f) mark_val(kept, &b->appends[a].cells[f]);
    }
    b->spare_count = b->mask_free_count = 0;
    for (unsigned i = 0; i < b->buffers; ++i)
        if (!kept[i]) b->spare[b->spare_count++] = buffer[i];
    for (unsigned i = 0; i < b->masks; ++i)
        if (!kept_mask[i]) b->mask_free[b->mask_free_count++] = &b->mask[i];
}

/* A nested loop's slot in round t: low + t in the lanes running it (low
 * itself in round 0, as the LOOP sets it), the others' as it was. */
static void set_slot(Batch *b, unsigned slot, const Val *low, uint32_t t, const uint32_t *live, unsigned alive) {
    Val v;
    if (alive == b->n && low->kind == UNIFORM) v = uniform(t ? low->value + t : low->value, 0);
    else if (!(alive == b->n && low->kind == AFFINE && affine_of(b, low->value + t, low->step, &v))) {
        double *out = fresh(b);
        if (!out) return;
        for (unsigned l = 0; l < b->n; ++l)
            out[l] = !live || live[l] ? (t ? lane_of(low, l) + t : lane_of(low, l))
                   : b->state[slot] ? lane_of(&b->local[slot], l) : b->ev->locals[slot].num;
        v = lanes_val(out, false);
        v.temp = false;
    }
    b->local[slot] = v, b->state[slot] = 1;
}

/* The LOOP at pc in the batch's block: its rounds for every lane running
 * here together, round t for the lanes with more than t rounds that did not
 * break out, its work counted for each (the batch's end checks it). */
static void nested(Batch *b, const PureOp *op, uint32_t pc) {
    Val high = b->stack[--b->sp], low = b->stack[--b->sp];
    uint32_t start = pc + 1, end = start + (op->arg >> 16);
    unsigned slot = op->arg & 0xFF, n = b->n;
    uint32_t *count = fresh(b), *broken = fresh(b);
    if (!count || !broken) return;
    const uint32_t *entry = running(b);
    uint8_t bad[BATCH];
    if (low.kind == UNIFORM && high.kind == UNIFORM) { /* the same rounds in every lane */
        double lo = low.value, hi = high.value;
        if (!(fabs(lo) <= 0x1p53 && fabs(hi) <= 0x1p53 && lo == floor(lo) && hi == floor(hi)) || hi - lo > 0x1p31) {
            b->failed = true;
            return;
        }
        uint32_t rounds = hi > lo ? (uint32_t)(hi - lo) : 0;
        for (unsigned l = 0; l < n; ++l) count[l] = entry && !entry[l] ? 0 : rounds, broken[l] = 0;
        memset(bad, 0, n);
    } else
    for (unsigned l = 0; l < n; ++l) { /* as the LOOP opens: whole ends within 2^53 */
        double lo = lane_of(&low, l), hi = lane_of(&high, l);
        bad[l] = !(fabs(lo) <= 0x1p53 && fabs(hi) <= 0x1p53 && lo == floor(lo) && hi == floor(hi)) || hi - lo > 0x1p31;
        count[l] = bad[l] || (entry && !entry[l]) || hi <= lo ? 0 : (uint32_t)(hi - lo), broken[l] = 0;
    }
    release(b, &high);
    if (any_running(b, bad)) {
        b->failed = true;
        return;
    }
    if (low.kind == LANES) low.temp = false; /* the nest keeps it */
    Mask *outer = b->active;
    Nest saved = b->nest;
    b->nest = (Nest){outer, count, broken, low};
    uint64_t per = (uint64_t)(end - start) + 1;
    uint32_t least = UINT32_MAX; /* the fewest rounds a lane that came has */
    unsigned came = 0;
    for (unsigned l = 0; l < n; ++l)
        if (!entry || entry[l]) least = count[l] < least ? count[l] : least, ++came;
    b->breaks = false;
    for (uint32_t t = 0; !b->failed; ++t) {
        uint32_t *live;
        unsigned alive = 0;
        if (t < least && !b->breaks) /* every lane that came: no mask to make */
            live = (uint32_t *)entry, alive = came;
        else {
            if (!(live = fresh(b))) break;
            for (unsigned l = 0; l < n; ++l) live[l] = (t < count[l]) & (broken[l] ^ 1), alive += live[l];
        }
        if (!alive) break;
        b->work += alive * per; /* each lane's round: its block and its LOOP, as written */
        set_slot(b, slot, &low, t, live, alive);
        b->active = alive == n ? NULL : live == entry ? outer : new_mask(b, live, outer, alive);
        b->none = false;
        if (!pass(b, start, end)) break;
        arrive(b, end);
        sweep(b);
    }
    b->nest = saved;
    b->active = outer, b->none = false;
}

/* ---- The end: the sets, if no read of the batch would have seen one */


/* Whether a store at rows (in the lanes of mask: NULL all) sets in a round the row a later round's read reads. */
static bool reads_set(const Batch *b, const Read *r, const Store *s, const uint32_t *mask) {
    if (!r->known) return true;
    unsigned n = b->n;
    if (r->rows.kind == UNIFORM) { /* read once, before any set: set in a round before the last */
        for (unsigned l = 0; l < n; ++l)
            if (round_of(b, l) + 1 < n && (!mask || mask[l]) && lane_of(&s->rows, l) == r->rows.value) return true;
        return false;
    }
    if (s->rows.kind == UNIFORM) { /* one row: read in a round after the first to set it */
        unsigned first = n;
        for (unsigned l = 0; l < n; ++l)
            if ((!mask || mask[l]) && round_of(b, l) < first) first = round_of(b, l);
        for (unsigned l = 0; l < n; ++l)
            if (round_of(b, l) > first && lane_of(&r->rows, l) == s->rows.value) return true;
        return false;
    }
    if (r->rows.kind == AFFINE && s->rows.kind == AFFINE && r->rows.step == s->rows.step) { /* lane l reads what lane l + d sets */
        double d = (r->rows.value - s->rows.value) / s->rows.step;
        return d == floor(d) && fabs(d) < n && (b->reverse ? d > 0 : d < 0);
    }
    if (r->rows.kind == AFFINE && s->rows.kind == AFFINE) { /* lane l reads what lane (r0 - s0 + rs l) / ss sets */
        for (unsigned l = 0; l < n; ++l) {
            double w = (r->rows.value + r->rows.step * l - s->rows.value) / s->rows.step;
            if (w >= 0 && w < n && w == floor(w) && round_of(b, (unsigned)w) < round_of(b, l) && (!mask || mask[(unsigned)w]))
                return true;
        }
        return false;
    }
    return true; /* lanes of rows: not looked into yet */
}

/* Whether two stores of the same cells (a before b in the block, each in the
 * lanes of its mask) set a row in rounds the other way round: b's in an
 * earlier round than a's (the rounds leave a's, the batch b's). */
static bool sets_cross(const Batch *b, const Store *x, const uint32_t *xm, const Store *y, const uint32_t *ym) {
    unsigned n = b->n;
    if (x->rows.kind == AFFINE && y->rows.kind == AFFINE && x->rows.step == y->rows.step) { /* y's lane l sets x's lane l + d's row */
        double d = (y->rows.value - x->rows.value) / x->rows.step;
        return d == floor(d) && fabs(d) < n && (b->reverse ? d < 0 : d > 0);
    }
    if (x->rows.kind == LANES || y->rows.kind == LANES) return true; /* not looked into yet */
    unsigned last = 0; /* the last round x sets its row in (x uniform) */
    for (unsigned k = 0; x->rows.kind == UNIFORM && k < n; ++k)
        if ((!xm || xm[k]) && round_of(b, k) > last) last = round_of(b, k);
    for (unsigned l = 0; l < n; ++l) { /* y's row in each of its lanes: set by x in a later round? */
        if (ym && !ym[l]) continue;
        double row = lane_of(&y->rows, l);
        if (x->rows.kind == UNIFORM) {
            if (row == x->rows.value && last > round_of(b, l)) return true;
            continue;
        }
        double k = (row - x->rows.value) / x->rows.step;
        if (k >= 0 && k < n && k == floor(k) && (!xm || xm[(unsigned)k]) && round_of(b, (unsigned)k) > round_of(b, l)) return true;
    }
    return false;
}

/* Lanes' values set at rows to + step × lane, those of mask (NULL: all). */
static void put_words(uint32_t *to, ptrdiff_t step, const uint32_t *from, const uint32_t *mask, unsigned n) {
    if (!mask && step == 1) memcpy(to, from, n * sizeof(*to));
    else if (!mask && step == -1) for (unsigned l = 0; l < n; ++l) to[-(ptrdiff_t)l] = from[l];
    else if (!mask) for (unsigned l = 0; l < n; ++l) to[(ptrdiff_t)l * step] = from[l];
    else if (step == 1) for (unsigned l = 0; l < n; ++l) to[l] = mask[l] ? from[l] : to[l];
    else if (step == -1) for (unsigned l = 0; l < n; ++l) to[-(ptrdiff_t)l] = mask[l] ? from[l] : to[-(ptrdiff_t)l];
    else for (unsigned l = 0; l < n; ++l) if (mask[l]) to[(ptrdiff_t)l * step] = from[l];
}

static void put_numbers(double *to, ptrdiff_t step, const double *from, const uint32_t *mask, unsigned n) {
    if (!mask && step == 1) memcpy(to, from, n * sizeof(*to));
    else for (unsigned l = 0; l < n; ++l) if (!mask || mask[l]) to[(ptrdiff_t)l * step] = from[l];
}

/* Whether a number goes into a column of width bytes as it is (else: the width it needs). */
static uint8_t width_needed(double v) {
    if (!(v >= 0 && v == floor(v)) || (v == 0 && signbit(v))) return 8;
    return v < 256 ? 1 : v < 4294967296.0 ? 4 : 8;
}

/* Each reduction's value after the batch's rounds, as they would leave it
 * (false: they would fault, or a lane's sum may not round as theirs). */
static bool fold(Batch *b) {
    for (unsigned i = 0; i < b->reduce_count; ++i) {
        Reduce *r = &b->reduce[i];
        double c = b->ev->locals[r->local].num; /* as the batch began */
        bool several = false;
        for (unsigned l = 0; l < b->n; ++l) several |= r->count[l] > 1;
        if (r->op == PURE_OP_ADD && several && (r->loose || !(c == floor(c) && fabs(c) <= 0x1p53))) return false;
        for (unsigned k = 0; k < b->n; ++k) { /* in the rounds' order */
            unsigned l = round_of(b, k);
            if (!r->count[l]) continue;
            c = r->op == PURE_OP_ADD ? c + r->partial[l] : r->op == PURE_OP_MIN ? minimum(c, r->partial[l]) : maximum(c, r->partial[l]);
            if (!isfinite(c) || (several && fabs(c) > 0x1p53)) return false;
        }
        r->result = c;
    }
    return true;
}

static bool settle(Batch *b) {
    PureCells cells[STORES];
    const uint32_t *mask[STORES];
    if (!fold(b)) return false;
    for (unsigned i = 0; i < b->scatter_count; ++i) { /* a sum's cells: an event's list's, read and set by nothing else in the batch */
        const Scatter *sc = &b->scatters[i];
        PureCells c;
        if (!pure_list_cells(b->app->lists[sc->list], &c) || !c.owned || c.width[sc->field] == 255 || c.width[sc->field] == 0) return false;
        Mask *saved = b->active;
        b->active = sc->mask;
        bool in = rows_in(b, &sc->rows, c.count);
        b->active = saved;
        if (!in) return false;
        for (unsigned r = 0; r < b->read_count; ++r)
            if (b->reads[r].list == sc->list && b->reads[r].field == sc->field) return false;
        for (unsigned t = 0; t < b->store_count; ++t)
            if (b->stores[t].list == sc->list && b->stores[t].field == sc->field) return false;
        for (unsigned j = 0; j < i; ++j)
            if (b->scatters[j].list == sc->list && b->scatters[j].field == sc->field) return false;
    }
    for (unsigned i = 0; i < b->store_count; ++i) {
        Store *s = &b->stores[i];
        PureList *list = b->app->lists[s->list];
        if (!pure_list_cells(list, &cells[i]) || !cells[i].owned || cells[i].width[s->field] == 255 || cells[i].width[s->field] == 0)
            return false;
        Mask *saved = b->active;
        b->active = s->mask;
        bool in = rows_in(b, &s->rows, cells[i].count);
        b->active = saved;
        mask[i] = mask_lanes(b, s->mask);
        if (!in || b->failed) return false;
        for (unsigned r = 0; r < b->read_count; ++r)
            if (b->reads[r].list == s->list && b->reads[r].field == s->field && reads_set(b, &b->reads[r], s, mask[i])) return false;
        for (unsigned j = 0; j < i; ++j)
            if (b->stores[j].list == s->list && b->stores[j].field == s->field && sets_cross(b, &b->stores[j], mask[j], s, mask[i]))
                return false;
    }
    for (unsigned i = 0; i < b->store_count; ++i) { /* columns wide enough: wider changes no item */
        Store *s = &b->stores[i];
        uint8_t need = 1, width = cells[i].width[s->field];
        if (width == 8 || (s->value.u32 && width == 4)) continue;
        for (unsigned l = 0; l < b->n && need < 8; ++l)
            if (!mask[i] || mask[i][l]) {
                uint8_t lane = width_needed(lane_of(&s->value, l));
                need = lane > need ? lane : need;
            }
        if (need > width) {
            if (!pure_list_widen(b->app->lists[s->list], s->field, need)) return false;
            for (unsigned j = 0; j < b->store_count; ++j) /* its columns as they are now */
                if (b->stores[j].list == s->list) pure_list_cells(b->app->lists[s->list], &cells[j]);
        }
    }
    for (unsigned i = 0; i < b->store_count; ++i) { /* each lane's, in the rounds' order */
        Store *s = &b->stores[i];
        void *column = cells[i].columns[s->field];
        uint8_t width = cells[i].width[s->field];
        pure_list_touched(b->app->lists[s->list]);
        if (s->rows.kind == AFFINE && s->value.kind == LANES && (width == 8 ? !s->value.u32 : s->value.u32 && width == 4)) {
            ptrdiff_t first = (ptrdiff_t)s->rows.value, step = (ptrdiff_t)s->rows.step; /* distinct rows: set each as its lane runs */
            b->backward += step < 0 ? 1 : -1;
            if (width == 4) put_words((uint32_t *)column + first, step, s->value.lane, mask[i], b->n);
            else put_numbers((double *)column + first, step, s->value.lane, mask[i], b->n);
            continue;
        }
        for (unsigned r = 0; r < b->n; ++r) {
            unsigned l = round_of(b, r);
            if (mask[i] && !mask[i][l]) continue;
            size_t at = (size_t)lane_of(&s->rows, l);
            double v = lane_of(&s->value, l);
            if (width == 8) memcpy((double *)column + at, &v, sizeof(v));
            else if (width == 4) ((uint32_t *)column)[at] = (uint32_t)v;
            else ((uint8_t *)column)[at] = (uint8_t)v;
        }
    }
    return true;
}

int pure_batch_run(Frame *frame, Loop *loop, unsigned *done) {
    PureApp *app = frame->app;
    const PureModule *m = frame->m;
    bool walking = loop->walking; /* a walk: its rounds its places, their work counted as it opened */
    size_t items = walking ? pure_list_count(app->lists[loop->walk.list]) : 0, end = loop->walk.end < items ? loop->walk.end : items;
    uint64_t round = walking ? loop->walk.at : loop->k, per = walking ? 0 : (uint64_t)(loop->end - loop->start) + 1;
    uint64_t left = walking ? end - loop->walk.at : loop->count - loop->k;
    uint64_t budget = app->work_budget ? app->work_budget : PURE_WORK_DEFAULT, paid = per ? 1 + (budget - app->work) / per : left;
    *done = 0;
    if (left < LEAST || (!walking && loop->integer)) {
        loop->resume = UINT64_MAX;
        return STEP_NEXT;
    }
    unsigned n = (unsigned)(left < BATCH ? left : BATCH);
    if (paid < n) n = (unsigned)paid;
    double first = walking ? (double)loop->walk.at : loop->slot->num;
    if (n < 2 || (first == 0 && signbit(first))) { /* the work left pays for one round, or the loop counts from -0: one round by the executor */
        loop->resume = round + 1;
        return STEP_NEXT;
    }
    Batch *b = &batch;
    b->frame = frame, b->m = m, b->app = app, b->ev = frame->ev, b->n = n, b->first = first;
    b->reverse = loop->reverse, b->backward = 0;
    b->var_start = b->reverse ? first + (n - 1) : first, b->var_step = b->reverse ? -1 : 1;
    b->slot = walking ? LOCALS : (uint32_t)(loop->slot - frame->ev->locals), b->end = loop->end;
    b->walk = walking ? (int)loop->walk.list : -1;
    b->failed = b->none = b->items = false, b->active = NULL, b->nest = (Nest){0}, b->work = 0, b->var = NULL;
    b->sp = b->split_count = b->buffers = b->masks = b->read_count = b->store_count = b->spare_count = 0;
    b->mask_free_count = b->reduce_count = b->append_count = 0;
    b->recur_count = b->update_count = b->record_count = b->step_count = b->sum_count = b->scatter_count = 0;
    memset(b->sets, 0, sizeof(b->sets));
    for (uint32_t pc = loop->start; pc < loop->end; ++pc) { /* the locals it sets (a nested loop's slot too), and whether it sets items */
        const PureOp *op = &m->code[pc];
        b->items |= op->op == PURE_OP_ITEM_SET;
        if (op->op == PURE_OP_LOCAL_STORE || op->op == PURE_OP_LOOP) {
            unsigned at = op->arg & 0xFF;
            b->sets[at / 64] |= UINT64_C(1) << (at % 64);
            b->state[at] = 0;
        }
    }
    if (!recurrences(b, loop->start, loop->end) || !recur(b) || (b->items && !sums(b, loop->start, loop->end)))
        b->failed = true; /* the recurrences ahead, round by round; the sums into cells */
    if (!b->failed && pass(b, loop->start, loop->end)) {
        arrive(b, loop->end);
        for (unsigned w = 0; w < LOCALS / 64; ++w) /* the lanes each local was set in, made before anything changes */
            for (uint64_t bits = b->sets[w]; bits; bits &= bits - 1) {
                unsigned at = w * 64 + (unsigned)__builtin_ctzll(bits);
                if (b->state[at] == 2) mask_lanes(b, b->written[at]);
            }
        for (unsigned a = 0; a < b->append_count; ++a) mask_lanes(b, b->appends[a].mask);
        for (unsigned i = 0; i < b->scatter_count; ++i) mask_lanes(b, b->scatters[i].mask);
    }
    uint64_t work = (n - 1) * per + b->work; /* the rounds after the first, and every round of their nested loops */
    if (b->failed || work > budget - app->work || !settle(b)) {
        loop->resume = ++loop->failures >= FAILURES ? UINT64_MAX : round + n; /* its rounds one by one, then batches again */
        return STEP_NEXT;
    }
    if (b->scatter_count) { /* the sums into cells round by round, as the executor adds and sets (each its own cells) */
        const double *rows[SUMS], *es[SUMS];
        const uint32_t *masks[SUMS];
        PureCells cells[SUMS];
        for (unsigned i = 0; i < b->scatter_count; ++i) {
            const Scatter *sc = &b->scatters[i];
            rows[i] = numbers(b, &sc->rows), es[i] = numbers(b, &sc->e), masks[i] = mask_lanes(b, sc->mask);
            pure_list_cells(b->app->lists[sc->list], &cells[i]);
            if (!rows[i] || !es[i]) { /* no lanes left to make them: as the executor, round by round */
                loop->resume = ++loop->failures >= FAILURES ? UINT64_MAX : round + n;
                return STEP_NEXT;
            }
        }
        for (unsigned r = 0; r < n; ++r) {
            unsigned l = round_of(b, r);
            for (unsigned i = 0; i < b->scatter_count; ++i) {
                const Scatter *sc = &b->scatters[i];
                if (masks[i] && !masks[i][l]) continue;
                size_t row = (size_t)rows[i][l];
                uint8_t width = cells[i].width[sc->field];
                void *column = cells[i].columns[sc->field];
                double cell, v;
                if (width == 8) memcpy(&cell, (const double *)column + row, sizeof(cell));
                else cell = width == 4 ? ((const uint32_t *)column)[row] : ((const uint8_t *)column)[row];
                v = cell + es[i][l];
                if (!isfinite(v)) { /* its round's fault (the event undone) */
                    app->work += (uint64_t)r * per;
                    frame->kind = PURE_FAULT_NON_FINITE, frame->fault_pc = sc->add;
                    return STEP_FAULT;
                }
                bool fits = width == 8 || (v >= 0 && v < (width == 4 ? 4294967296.0 : 256.0) && v == (double)(uint32_t)v &&
                                           !(v == 0 && signbit(v)));
                uint8_t need = fits ? width : width_needed(v);
                if (need > width) { /* wider: the column as the executor's set widens it */
                    PureList *list = b->app->lists[sc->list];
                    if (!pure_list_widen(list, sc->field, need)) {
                        app->work += (uint64_t)r * per;
                        frame->status = PURE_ALLOCATION, frame->fault_pc = sc->set;
                        return STEP_REFUSED;
                    }
                    pure_list_cells(list, &cells[i]);
                    column = cells[i].columns[sc->field], width = need;
                }
                if (width == 8) memcpy((double *)column + row, &v, sizeof(v));
                else if (width == 4) ((uint32_t *)column)[row] = (uint32_t)v;
                else ((uint8_t *)column)[row] = (uint8_t)v;
            }
        }
        for (unsigned i = 0; i < b->scatter_count; ++i) pure_list_touched(b->app->lists[b->scatters[i].list]);
    }
    for (unsigned r = 0; r < n && b->append_count; ++r) { /* the appends round by round, each as the executor's step makes it */
        unsigned l = round_of(b, r);
        for (unsigned i = 0; i < b->append_count; ++i) {
            const Append *a = &b->appends[i];
            const uint32_t *mask = mask_lanes(b, a->mask);
            if (mask && !mask[l]) continue;
            for (unsigned f = 0; f < a->count; ++f) frame->stack[f].num = lane_of(&a->cells[f], l);
            frame->sp = a->count;
            int how = pure_run_step(frame, a->pc);
            if (how != STEP_NEXT) { /* its fault at its round: the event's undone, so only how and the work so far tell */
                app->work += (uint64_t)r * per;
                return how;
            }
        }
    }
    app->work += work;
    for (unsigned r = 0; r < b->reduce_count; ++r) frame->ev->locals[b->reduce[r].local].num = b->reduce[r].result;
    for (unsigned x = 0; x < b->recur_count; ++x) frame->ev->locals[b->recur_local[x]].num = b->recur_value[x];
    for (unsigned w = 0; w < LOCALS / 64; ++w) /* each local the block set: as the last round to set it left it */
        for (uint64_t bits = b->sets[w]; bits; bits &= bits - 1) {
            unsigned at = w * 64 + (unsigned)__builtin_ctzll(bits), last = n - 1;
            if (!b->state[at]) continue;
            const uint32_t *set = b->state[at] == 2 ? mask_lanes(b, b->written[at]) : NULL;
            while (set && last && !set[round_of(b, last)]) --last;
            if (!set || set[round_of(b, last)]) frame->ev->locals[at].num = lane_of(&b->local[at], round_of(b, last));
        }
    if (!walking) loop->slot->num = first + (double)(n - 1);
    else frame->ev->row = (size_t)first + n - 1; /* the walk's place as its last round left it */
    if (b->backward > 0) loop->reverse = !loop->reverse; /* rows going down from lane to lane: the next batch's lanes the other way */
    *done = n;
    return STEP_NEXT;
}
