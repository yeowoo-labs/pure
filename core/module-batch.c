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

#ifndef PURE_BATCH_LANES
#define PURE_BATCH_LANES 1024 /* (another to measure a batch's fixed cost: its time against 1024's, docs/design/025) */
#endif
enum { BATCH = PURE_BATCH_LANES, BUFFERS = 24, MASKS = 32, SPLITS = 8, READS = 16, STORES = 8, APPENDS = 8, REDUCES = 8, RECURS = 4, UPDATES = 8,
       RECORDS = 8, STEPS = 96, SUMS = 4, LOCALS = 256, LEAST = 16, FAILURES = 3 };

enum { UNIFORM, AFFINE, LANES, REDUCE }; /* REDUCE: x in x = x op e (value: its reduction) */
typedef struct {
    uint8_t kind;
    bool u32;       /* lanes: whole numbers below 2^32 in four bytes each (else numbers) */
    bool temp;      /* lanes the stack alone holds: an instruction may leave its result in them */
    bool view;      /* lanes a list's column itself (cells_at): read only, and only while the pass runs (own) */
    uint16_t ones;  /* lanes of 1s and 0s a comparison made: how many 1s, + 1 (0: not counted) */
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
/* A recurrence's x = e ahead of the batch, round by round: e's operations
 * as steps, each of two of the numbers a round keeps (V_ ones: a step's
 * result at its index, a recurrence's value, the loop's value, a number
 * that stays), an operation computed twice in e once; the statement from to
 * its set, the recurrence's index and where e's value is; and where the
 * block reads a recurrence, its value there in each lane. */
enum { V_X = STEPS, V_SLOT = V_X + RECURS, V_NUMBER, VALUES = 256 };
typedef struct { uint8_t op, a, c; } Step;
typedef struct { uint32_t from, set; unsigned x, first, count; uint8_t value; } Update;
typedef struct { uint32_t pc; unsigned x, of; bool shared; } Record; /* shared: record of's lanes, x not set between */
/* A sum into a cell, L[k].f = L[k].f + e: its statement's second k from
 * again to resume (past its find and read, skipped), its ADD and its set;
 * and as the pass leaves it, its rows and e in the lanes of mask, added at
 * the end round by round. */
typedef struct { uint32_t again, resume, add, set; } Sum;
typedef struct { uint32_t list, field, add, set; Val rows, e; Mask *mask; } Scatter;
/* A round of the recurrences, in the block's order: a step (op, its two
 * numbers, where its result goes), a record of x (a: x, c: the record) or x
 * set (a: its value, to: x). */
enum { A_RECORD = 254, A_SET };
typedef struct { uint8_t op, a, c, to; } Action;
/* A batch loop's recurrences as the load reads them (docs/design/025): the
 * locals, the updates and their steps, the records, the numbers that stay
 * (each from where a batch reads it: a constant, a field, a LOAD, a local
 * the block does not set; the same source one number) and a round's
 * actions. A batch fills the numbers and the recurrences' values in. */
enum { S_CONST, S_FIELD, S_LOAD, S_LOCAL };
typedef struct { uint8_t kind; uint32_t arg; double value; } Source;
typedef struct {
    unsigned recur_count, update_count, record_count, step_count, source_count, action_count;
    unsigned recur_local[RECURS];
    Update updates[UPDATES];
    Record records[RECORDS];
    Step steps[STEPS];
    Source sources[VALUES - V_NUMBER];
    Action actions[STEPS + UPDATES + RECORDS];
} Recurring;
/* A batch loop's block as the load reads it, once (docs/design/025): the
 * locals it sets (a nested loop's slot too), whether it sets items, its sums
 * into cells, and at each of its instructions what a pass does there besides
 * the instruction: at a sum's again, add or set, at a recurrence's update
 * (its from) or record, that (role, the sum's, update's or record's index),
 * and at a LOCAL_LOAD beginning x = x op e how far on op is (by; 0: none).
 * A nested loop's instructions have no role (sums and recurrences are the
 * loop's own block's) but may begin x = x op e. Its recurrences (NULL:
 * none). failed: more sums or recurrences than a batch holds. A segment
 * (role AT_SEGMENT at its first instruction, docs/design/025's stage 2):
 * instructions to its end (to) that make one uniform or affine value from
 * uniform and affine values alone, then its LOCAL_STORE if store, run at
 * once in scalars when the batch runs plainly (the pass's own way else). */
enum { AT_PLAIN, AT_AGAIN, AT_ADD, AT_SET, AT_UPDATE, AT_RECORD, AT_SEGMENT };
typedef struct { uint8_t role, index; uint16_t by; } PlanAt;
typedef struct { uint32_t to; bool store; } Segment;
enum { SEGMENTS = 255 };
typedef struct PureBatchPlan {
    uint32_t loop, start, end;
    uint64_t sets[LOCALS / 64];
    bool items, failed;
    unsigned sum_count, segment_count;
    Sum sums[SUMS];
    Recurring *recur;
    Segment *segments;
    PlanAt at[];
} PureBatchPlan;
/* An item a LIST_APPEND at pc appends in the lanes of mask (NULL: all): its fields. */
typedef struct { uint32_t pc; Mask *mask; unsigned count; Val cells[PURE_RECORD_FIELDS]; const uint32_t *sel; unsigned sel_count; } Append;
/* A branch's arm run in its lanes alone when they are few (a selection,
 * docs/design/024): sel[j] the lane (of the space around it) of its lane
 * j, the lanes around (n, their mask), where the arm ends (a JMP there: an
 * else follows), and the locals the arm set with what they were. */
typedef struct {
    const uint32_t *sel;
    unsigned n;
    Mask *active;
    uint32_t end;
    bool jmp;
    uint64_t set[LOCALS / 64];
    Val old[LOCALS];
    uint8_t old_state[LOCALS];
    Mask *old_written[LOCALS];
} Selection;
/* A filter, if !L.has(k) { its code } (PLAN_FILTER: L only grows in the
 * batch's block; its code a line, appending): its HAS, its code's end, L; k
 * and the lanes its code runs in this pass; its code's appends (their
 * instructions, lists and cells' counts). The rounds its code ran in are
 * kept (filtered: a round's number, k, each append's cells), asked again in
 * the rounds' order at the end, their appends made for those still not there. */
enum { FILTERS = 2, FILTER_APPENDS = 4, FILTERED = 1 << 16, CELLS = 4 };
enum { KNOWN = 8 };
typedef struct { uint32_t pc; Val v; const uint32_t *sel, *parent; } Known; /* sel, parent: the space it was kept in */
typedef struct {
    uint32_t has, end, list;
    Val key;
    Mask *mask;
    const uint32_t *sel; /* its code's lanes a selection's (sel_count of them), else mask's */
    unsigned sel_count;
    bool none, spread;
    unsigned first, appends, made, stride; /* made: its appends, as its kept rounds hold their cells */
    uint32_t pc[FILTER_APPENDS], to[FILTER_APPENDS];
    unsigned cells[FILTER_APPENDS];
    size_t kept;
} Filter;
/* Lanes a branch sent ahead (NULL: every lane): they run again at pc. */
typedef struct { uint32_t pc; Mask *mask; } Split;
/* A reduction (x = x op e, x read no other way): each lane's op of its rounds'
 * e (from op's identity), how many it had, whether one was not whole or a
 * lane's past 2^53 (then a lane's two may not sum as the rounds would). While
 * every step has been the same e in every lane, no lanes: every lane's the
 * same, uniform after uniform_count of them (docs/design/025, stage 3). */
typedef struct {
    unsigned local;
    uint8_t op;
    double *partial;
    uint32_t *count;
    bool loose;
    double result, uniform;
    uint32_t uniform_count;
} Reduce;
/* A loop the batch's block runs, its rounds together: the lanes that came
 * to it, how many rounds each has, from what, and those that broke out. */
typedef struct { Mask *entry; uint32_t *count, *broken; Val low; uint32_t *rows, t; } Nest; /* rows: a walk's, each lane's first (round t: + t) */

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
    const struct PureBatchPlan *plan; /* the loop's, made at load */
    const uint64_t *sets;       /* the locals the block sets (the plan's) */
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
    const uint32_t *parent;     /* a walk's items spread into the lanes: each lane's outer lane (NULL: none spread) */
    const uint32_t *item_rows;  /* and its item's row */
    unsigned outer_n;           /* the outer lanes then, their order and their loop's first value */
    bool outer_reverse;
    double outer_first;
    uint64_t own[LOCALS / 64];  /* the locals the spread items set this time (theirs: fresh at each chunk) */
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
    double recur_value[RECURS]; /* each recurrence's after the rounds so far */
    double values[VALUES];      /* what a round of the recurrences keeps (V_): numbers from V_NUMBER on */
    double *record_lanes[RECORDS]; /* each record's value in each lane */
    unsigned scatter_count;
    Scatter scatters[SUMS];
    Filter filters[FILTERS];
    unsigned filter_count;
    Known known[KNOWN]; /* values kept for later copies of their code (the module's reuse_from) */
    unsigned known_count;
    PureCells cells[CELLS];     /* lists' cells as the pass reads them (cells_of), */
    uint32_t cells_list[CELLS]; /* whose (UINT32_MAX: neither form) */
    bool cells_form[CELLS];
    unsigned cells_count;
    const uint32_t *sel;        /* a selection running (NULL: none): its lanes */
    Selection selection;
} Batch;

static _Thread_local Batch batch;
static _Thread_local double *filtered[FILTERS]; /* each filter's kept rounds (Filter), grown as needed */
static _Thread_local size_t filtered_room[FILTERS];

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

/* Numbers numbers made for a value and done with: their lanes free again (unless a value's own, or the loop's). */
static void spare_numbers(Batch *b, const double *made) {
    if (made == b->var) return;
    for (unsigned k = 0; k < b->known_count; ++k) /* (a kept value's lanes: its) */
        if (b->known[k].v.lane == made) return;
    uintptr_t at = (uintptr_t)made, first = (uintptr_t)&buffer[0][0], past = (uintptr_t)&buffer[BUFFERS - 1][BATCH + 24];
    if (at < first || at >= past || (at - first) % sizeof(buffer[0])) return; /* (not a buffer's start: a lane's elsewhere) */
    for (unsigned s = 0; s < b->spare_count; ++s)
        if (b->spare[s] == (void *)made) return;
    b->spare[b->spare_count++] = (void *)made;
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

/* A view's lanes (a column read in place) as lanes of its own: what outlives
 * the pass (a local, a set, an append, a sum into a cell, a filter's key, a
 * read's rows) must not see its column set or widened at the batch's end. */
static void own(Batch *b, Val *v) {
    if (v->kind != LANES || !v->view) return;
    void *out = fresh(b);
    if (!out) return;
    memcpy(out, v->lane, b->n * (v->u32 ? sizeof(uint32_t) : sizeof(double)));
    uint16_t owner = v->owner;
    *v = lanes_val(out, v->u32), v->owner = owner;
}
static Val gather(Batch *b, const Val *v);
static Val expand(Batch *b, const Val *v);
static Val expand_rows(Batch *b, const Val *v);

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

/* A comparison's 1s and 0s, counted in the same loop (the branch after it needs not count them). */
#define TEST(expr)                                                                                                     \
    do {                                                                                                               \
        if (ux) for (unsigned l = 0; l < n; ++l) { uint32_t a = xk, c = ys[l], t = (expr); out[l] = t, count += t; }    \
        else if (uy) for (unsigned l = 0; l < n; ++l) { uint32_t a = xs[l], c = yk, t = (expr); out[l] = t, count += t; } \
        else for (unsigned l = 0; l < n; ++l) { uint32_t a = xs[l], c = ys[l], t = (expr); out[l] = t, count += t; }    \
    } while (0)

/* x op y over whole numbers below 2^32 in four bytes (xs or the uniform xk, ys or yk): false past them (as numbers then).
 * A comparison's 1s: *ones (else 0). */
static bool words_op(uint8_t op, uint32_t *restrict out, const uint32_t *restrict xs, uint32_t xk, bool ux,
                     const uint32_t *restrict ys, uint32_t yk, bool uy, unsigned n, unsigned *ones) {
    uint32_t over = 0, count = 0;
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
    case PURE_OP_MUL: /* past 2^32: the product's high half */
        if (ux) for (unsigned l = 0; l < n; ++l) { uint64_t p = (uint64_t)xk * ys[l]; out[l] = (uint32_t)p, over |= (uint32_t)(p >> 32); }
        else if (uy) for (unsigned l = 0; l < n; ++l) { uint64_t p = (uint64_t)xs[l] * yk; out[l] = (uint32_t)p, over |= (uint32_t)(p >> 32); }
        else for (unsigned l = 0; l < n; ++l) { uint64_t p = (uint64_t)xs[l] * ys[l]; out[l] = (uint32_t)p, over |= (uint32_t)(p >> 32); }
        break;
    case PURE_OP_MIN: EACH(uint32_t, a < c ? a : c); break;
    case PURE_OP_MAX: EACH(uint32_t, a > c ? a : c); break;
    case PURE_OP_LT: TEST(a < c); break;
    case PURE_OP_LE: TEST(a <= c); break;
    case PURE_OP_GT: TEST(a > c); break;
    case PURE_OP_GE: TEST(a >= c); break;
    case PURE_OP_EQ: TEST(a == c); break;
    case PURE_OP_NE: TEST(a != c); break;
    case PURE_OP_AND: TEST(a != 0 && c != 0); break;
    default: TEST(a != 0 || c != 0); break;
    }
    *ones = count;
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
    case PURE_OP_ADD: case PURE_OP_SUB: case PURE_OP_MUL: case PURE_OP_MIN: case PURE_OP_MAX: case PURE_OP_LT: case PURE_OP_LE:
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
    unsigned ones;
    if (!out || !words_op(op, out, xs, ux ? (uint32_t)x->value : 0, ux, ys, uy ? (uint32_t)y->value : 0, uy, b->n, &ones)) {
        b->spare_count = spares, b->buffers = buffers; /* lanes it took back: past 2^32 or below 0 in some lane, as numbers */
        b->failed = false;
        return false;
    }
    if (xs && xs != x->lane) b->spare[b->spare_count++] = (void *)xs; /* an affine value's lanes, made for this */
    if (ys && ys != y->lane) b->spare[b->spare_count++] = (void *)ys;
    release(b, x), release(b, y);
    *x = lanes_val(out, true);
    if (op >= PURE_OP_LT && op <= PURE_OP_OR) x->ones = (uint16_t)(ones + 1);
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
    double last = count ? (double)(count - 1) : 0;
    if (x->kind != LANES && by.kind != LANES) { /* uniform or affine both: affine too, while every lane's place is within the list */
        double v0 = x->value + by.value, v1 = v0, step = (x->kind == AFFINE ? x->step : 0) + (by.kind == AFFINE ? by.step : 0);
        v1 = v0 + step * (b->n - 1);
        Val v;
        if (by.value == floor(by.value) && (by.kind != AFFINE || by.step == floor(by.step)) && v0 >= 0 && v0 <= last && v1 >= 0 &&
            v1 <= last && (step ? affine_of(b, v0, step, &v) : (v = uniform(v0, 0), true))) {
            v.owner = x->owner;
            *x = v;
            return;
        }
    }
    double *out = fresh(b);
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
    if (rows->kind == LANES && rows->lane == b->item_rows && b->item_rows) return true; /* spread items' rows: made so */
    if (rows->kind == AFFINE) { /* in range at both ends: in every lane */
        double end = rows->value + rows->step * (b->n - 1);
        if (rows->value >= 0 && rows->value < (double)count && end >= 0 && end < (double)count) return true;
        if (!b->active) return false;
    }
    uint8_t bad[BATCH];
    if (rows->kind == LANES && rows->u32) { /* rows in four bytes: whole, not negative */
        const uint32_t *at = rows->lane;
        for (unsigned l = 0, n = b->n; l < n; ++l) bad[l] = at[l] >= count;
    } else
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
        if (step == 1 && (width == 8 || width == 4)) { /* the column itself, in memory's order: a view, no copy */
            Val v = lanes_val((char *)column + first * width, width == 4);
            v.temp = false, v.view = true;
            return v;
        }
        if (width == 8) {
            double *out = fresh(b);
            if (!out) return *rows;
            const double *from = (const double *)column + first;
            for (unsigned l = 0; l < n; ++l) out[l] = from[(ptrdiff_t)l * step];
            return lanes_val(out, false);
        }
        uint32_t *out = fresh(b);
        if (!out) return *rows;
        if (width == 4) {
            const uint32_t *from = (const uint32_t *)column + first;
            if (step == -1) for (unsigned l = 0; l < n; ++l) out[l] = from[-(ptrdiff_t)l];
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
    if (rows->kind == LANES && rows->u32) { /* rows in four bytes: a lane that does not run may have any (row 0 then) */
        const uint32_t *at = rows->lane;
        size_t count = cells->count;
        if (width == 8)
            for (unsigned l = 0; l < n; ++l) memcpy((double *)out + l, (const double *)column + (at[l] < count ? at[l] : 0), sizeof(double));
        else if (width == 4)
            for (unsigned l = 0; l < n; ++l) ((uint32_t *)out)[l] = ((const uint32_t *)column)[at[l] < count ? at[l] : 0];
        else
            for (unsigned l = 0; l < n; ++l) ((uint32_t *)out)[l] = ((const uint8_t *)column)[at[l] < count ? at[l] : 0];
        return lanes_val(out, width != 8);
    }
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
    r->list = list, r->field = field, r->known = known, r->rows = b->sel ? expand_rows(b, rows) : *rows;
    own(b, &r->rows);
    if (r->rows.kind == LANES) r->rows.temp = false;
}

/* ITEM_GET: field of the items at the top's positions. */
/* A list's cells as the pass reads them (pure_list_cells): no list changes
 * while a batch's pass runs (its sets, appends and filters wait for its end,
 * which reads its own), so once a batch for the first CELLS lists it reads.
 * NULL: neither form. */
static const PureCells *cells_of(Batch *b, uint32_t list) {
    for (unsigned i = 0; i < b->cells_count; ++i)
        if (b->cells_list[i] == list) return b->cells_form[i] ? &b->cells[i] : NULL;
    unsigned i = b->cells_count < CELLS ? b->cells_count++ : CELLS - 1; /* (more: the last one again and again) */
    b->cells_list[i] = list, b->cells_form[i] = pure_list_cells(b->app->lists[list], &b->cells[i]);
    return b->cells_form[i] ? &b->cells[i] : NULL;
}

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
    const PureCells *cells = cells_of(b, list);
    if (cells && cells->width[field] != 255) {
        *x = cells_at(b, cells, field, x);
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
    const PureCells *cells = cells_of(b, list);
    bool dense = cells && cells->width[key] == 0; /* DENSE with every key below its count: a key its row */
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
                out[l] = k >= 0 && k < (double)cells->count && k == floor(k);
            }
            *x = lanes_val(out, true);
            return true;
        }
        if (x->kind == LANES) {
            uint8_t bad[BATCH];
            for (unsigned l = 0; l < b->n; ++l) {
                double k = lane_of(x, l);
                bad[l] = !(k >= 0 && k < (double)cells->count && k == floor(k));
            }
            if (any_running(b, bad)) return false;
            if (!x->u32) { /* the rows: -0 is the key 0's */
                double *rows = fresh(b);
                if (!rows) return true;
                for (unsigned l = 0; l < b->n; ++l) rows[l] = ((const double *)x->lane)[l] + 0.0;
                *x = lanes_val(rows, false);
            }
        } else if (!rows_in(b, x, cells->count))
            return false;
        x->owner = (uint16_t)list;
        if (joined) {
            read_of(b, list, field, true, x);
            *x = cells_at(b, cells, field, x);
            x->owner = 0;
        }
        return true;
    }
    const uint64_t *bits;
    size_t words;
    PureCells at_keys;
    if ((has || (joined && pure_list_dense(keyed, &at_keys) && at_keys.width[field] != 255)) && pure_list_bits(keyed, &bits, &words)) {
        /* keys as bits: each lane's asked at once (a key whole, not below 0, below words * 64), a field read at its key */
        const uint32_t *run = running(b);
        void *out = fresh(b);
        if (!out) return true;
        uint8_t bad[BATCH];
        size_t top = words * 64;
        if (has && x->kind == LANES && x->u32) { /* keys in four bytes: whole, not below 0 */
            const uint32_t *keys = x->lane;
            for (unsigned l = 0, n = b->n; l < n; ++l) {
                size_t at = keys[l] < top ? keys[l] : 0;
                ((uint32_t *)out)[l] = keys[l] < top && bits[at >> 6] >> (at & 63) & 1;
            }
            *x = lanes_val(out, true);
            return true;
        }
        for (unsigned l = 0; l < b->n; ++l) {
            double k = lane_of(x, l);
            size_t at = k >= 0 && k < (double)top && k == floor(k) ? (size_t)k : top;
            bool there = at < top && bits[at >> 6] >> (at & 63) & 1;
            if (has) ((uint32_t *)out)[l] = there;
            else {
                bad[l] = !there;
                at = there ? at : 0;
                uint8_t width = at_keys.width[field];
                const void *column = at_keys.columns[field];
                double v = width == 8 ? ((const double *)column)[at] : width == 4 ? ((const uint32_t *)column)[at]
                         : width == 1 ? ((const uint8_t *)column)[at] : (double)at; /* (width 0: the key itself) */
                ((double *)out)[l] = v;
            }
        }
        (void)run;
        if (!has && any_running(b, bad)) return false;
        if (joined) {
            Val rows = {.kind = UNIFORM};
            read_of(b, list, field, false, &rows);
        }
        *x = lanes_val(out, has);
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
    if (b->sel) rows = expand_rows(b, &rows), value = expand(b, &value); /* in the lanes around: the selection's mask tells which */
    own(b, &rows), own(b, &value);
    if (value.kind == LANES) value.temp = false;
    if (rows.kind == LANES) rows.temp = false;
    b->stores[b->store_count++] = (Store){list, field, rows, value, b->sel ? b->selection.active : b->active};
}

/* ---- Locals and branches */

static void local_store(Batch *b, uint32_t arg) {
    unsigned at = arg & 0xFF;
    Val v = b->stack[--b->sp];
    if (v.kind == REDUCE) return; /* x = x op e: in its lanes already */
    if (b->parent && !(b->own[at / 64] >> (at % 64) & 1)) /* a spread item's own: fresh in these lanes */
        b->own[at / 64] |= UINT64_C(1) << (at % 64), b->state[at] = 0;
    if (at == b->slot) {
        b->failed = true;
        return;
    }
    if (b->sel && !(b->selection.set[at / 64] >> (at % 64) & 1)) { /* its first set in the selection: what it was, kept for the end */
        Selection *se = &b->selection;
        se->set[at / 64] |= UINT64_C(1) << (at % 64);
        se->old[at] = b->local[at], se->old_state[at] = b->state[at], se->old_written[at] = b->written[at];
        b->state[at] = 0;
    }
    own(b, &v);
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

/* An outer lane's value v in each spread item's lane (its outer lane's). */
static Val outer_lanes(Batch *b, const Val *v) {
    if (v->kind == UNIFORM) return *v;
    const uint32_t *parent = b->parent;
    void *out = fresh(b);
    if (!out) return *v;
    Val r;
    double last = v->value + v->step * (b->outer_n - 1);
    if (v->kind == AFFINE && whole32(v->value) && whole32(last)) { /* whole below 2^32 in every outer lane: four bytes */
        for (unsigned j = 0; j < b->n; ++j) ((uint32_t *)out)[j] = (uint32_t)(v->value + v->step * parent[j]);
        r = lanes_val(out, true);
    } else if (v->kind == AFFINE) {
        for (unsigned j = 0; j < b->n; ++j) ((double *)out)[j] = v->value + v->step * parent[j];
        r = lanes_val(out, false);
    } else if (v->u32) {
        for (unsigned j = 0; j < b->n; ++j) ((uint32_t *)out)[j] = ((const uint32_t *)v->lane)[parent[j]];
        r = lanes_val(out, true);
    } else {
        for (unsigned j = 0; j < b->n; ++j) ((double *)out)[j] = ((const double *)v->lane)[parent[j]];
        r = lanes_val(out, false);
    }
    r.owner = v->owner;
    return r;
}

/* An outer lane's value v in each lane of a selection in a spread walk (its item's outer lane's). */
static Val outer_through(Batch *b, const Val *v) {
    if (v->kind == UNIFORM) return *v;
    const uint32_t *sel = b->sel, *parent = b->parent;
    unsigned n = b->n;
    void *out = fresh(b);
    if (!out) return *v;
    Val r;
    double last = v->value + v->step * (b->outer_n - 1);
    if (v->kind == AFFINE && whole32(v->value) && whole32(last)) {
        for (unsigned j = 0; j < n; ++j) ((uint32_t *)out)[j] = (uint32_t)(v->value + v->step * parent[sel[j]]);
        r = lanes_val(out, true);
    } else if (v->kind == AFFINE) {
        for (unsigned j = 0; j < n; ++j) ((double *)out)[j] = v->value + v->step * parent[sel[j]];
        r = lanes_val(out, false);
    } else if (v->u32) {
        for (unsigned j = 0; j < n; ++j) ((uint32_t *)out)[j] = ((const uint32_t *)v->lane)[parent[sel[j]]];
        r = lanes_val(out, true);
    } else {
        for (unsigned j = 0; j < n; ++j) ((double *)out)[j] = ((const double *)v->lane)[parent[sel[j]]];
        r = lanes_val(out, false);
    }
    r.owner = v->owner;
    return r;
}

/* v (the space around the selection's) in the selection's lanes. */
static Val gather(Batch *b, const Val *v) {
    if (v->kind == UNIFORM || v->kind == REDUCE) return *v;
    const uint32_t *sel = b->sel;
    unsigned n = b->n;
    void *out = fresh(b);
    if (!out) return *v;
    Val r;
    if (v->kind == AFFINE) {
        double last = v->value + v->step * (b->selection.n - 1);
        if (whole32(v->value) && whole32(last)) {
            for (unsigned j = 0; j < n; ++j) ((uint32_t *)out)[j] = (uint32_t)(v->value + v->step * sel[j]);
            r = lanes_val(out, true);
        } else {
            for (unsigned j = 0; j < n; ++j) ((double *)out)[j] = v->value + v->step * sel[j];
            r = lanes_val(out, false);
        }
    } else if (v->u32) {
        for (unsigned j = 0; j < n; ++j) ((uint32_t *)out)[j] = ((const uint32_t *)v->lane)[sel[j]];
        r = lanes_val(out, true);
    } else {
        for (unsigned j = 0; j < n; ++j) ((double *)out)[j] = ((const double *)v->lane)[sel[j]];
        r = lanes_val(out, false);
    }
    r.owner = v->owner;
    return r;
}

/* Rows of the selection's lanes in the lanes around: affine over them when
 * they are (a loop's value gathered: the end's checks then know them as
 * such), else expanded. */
static Val expand_rows(Batch *b, const Val *v) {
    if (v->kind != LANES) return expand(b, v);
    const uint32_t *sel = b->sel;
    unsigned n = b->n;
    double r0 = lane_of(v, 0);
    if (n == 1) return uniform(r0, v->owner);
    double step = (lane_of(v, 1) - r0) / ((double)sel[1] - sel[0]), start = r0 - step * sel[0];
    bool affine = step == floor(step) && whole(start) && whole(start + step * (b->selection.n - 1));
    for (unsigned j = 0; affine && j < n; ++j) affine = lane_of(v, j) == start + step * sel[j];
    if (!affine) return expand(b, v);
    Val a = {.kind = AFFINE, .value = start, .step = step, .owner = v->owner};
    return a;
}

/* v (the selection's) in the lanes around it: its lanes' values, 0 elsewhere. */
static Val expand(Batch *b, const Val *v) {
    if (v->kind == UNIFORM) return *v;
    const uint32_t *sel = b->sel;
    unsigned n = b->n, outer = b->selection.n;
    void *out = fresh(b);
    if (!out) return *v;
    bool u32 = v->kind == LANES && v->u32;
    memset(out, 0, outer * (u32 ? sizeof(uint32_t) : sizeof(double)));
    if (u32) for (unsigned j = 0; j < n; ++j) ((uint32_t *)out)[sel[j]] = ((const uint32_t *)v->lane)[j];
    else for (unsigned j = 0; j < n; ++j) ((double *)out)[sel[j]] = lane_of(v, j);
    Val r = lanes_val(out, u32);
    r.owner = v->owner;
    return r;
}

static inline __attribute__((always_inline)) void local_load(Batch *b, uint32_t arg, uint32_t pc) {
    unsigned at = arg & 0xFF;
    Val v;
    uint32_t by;
    if (b->sets[at / 64] >> (at % 64) & 1 && !b->state[at] && at != b->slot && b->plan->at[pc - b->plan->start].by) {
        by = pc + b->plan->at[pc - b->plan->start].by; /* x = x op e's op */
        Reduce *r = reduction(b, at, b->m->code[by].op); /* not set yet: an earlier round's, as x = x op e reads it */
        if (r) b->stack[b->sp++] = (Val){.kind = REDUCE, .value = (double)(r - b->reduce)};
        return;
    }
    if (b->sel && !(b->selection.set[at / 64] >> (at % 64) & 1)) { /* the space around the selection's, in its lanes */
        if (at == b->slot && !b->parent) {
            v = (Val){.kind = AFFINE, .value = b->reverse ? b->first + (b->selection.n - 1) : b->first, .step = b->reverse ? -1 : 1};
        } else if (at == b->slot || (b->parent && b->sets[at / 64] >> (at % 64) & 1 && !(b->own[at / 64] >> (at % 64) & 1))) {
            if (at == b->slot)
                v = (Val){.kind = AFFINE, .value = b->outer_reverse ? b->outer_first + (b->outer_n - 1) : b->outer_first,
                          .step = b->outer_reverse ? -1 : 1};
            else if (b->state[at] == 1) v = b->local[at];
            else {
                b->failed = true;
                return;
            }
            v = outer_through(b, &v); /* an outer lane's value in each of these lanes, by its item's outer lane */
            v.owner = (uint16_t)(arg >> 16);
            b->stack[b->sp++] = v;
            return;
        } else if (!(b->sets[at / 64] >> (at % 64) & 1)) v = uniform(b->ev->locals[at].num, (uint16_t)(arg >> 16));
        else if (b->state[at] == 1) v = b->local[at];
        else if (b->state[at] == 2) { /* set this pass in some lanes around: every one of these must be among them */
            const uint32_t *set = mask_lanes(b, b->written[at]);
            uint32_t missing = !set;
            for (unsigned j = 0; !missing && j < b->n; ++j) missing |= set[b->sel[j]] ^ 1;
            if (missing) {
                b->failed = true;
                return;
            }
            v = b->local[at];
        } else {
            b->failed = true; /* set in the block but not yet: an earlier round's */
            return;
        }
        v = gather(b, &v);
        v.owner = (uint16_t)(arg >> 16);
        b->stack[b->sp++] = v;
        return;
    }
    if (b->parent && (at == b->slot || (b->sets[at / 64] >> (at % 64) & 1 && !(b->own[at / 64] >> (at % 64) & 1)))) {
        if (at == b->slot) /* the outer loop's value, by each item's outer lane */
            v = (Val){.kind = AFFINE, .value = b->outer_reverse ? b->outer_first + (b->outer_n - 1) : b->outer_first,
                      .step = b->outer_reverse ? -1 : 1};
        else if (b->state[at] == 1) /* set in every outer lane before the walk */
            v = b->local[at];
        else {
            b->failed = true; /* in some outer lanes, or none yet: not looked into */
            return;
        }
        v = outer_lanes(b, &v);
    } else if (at == b->slot) {
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
/* JZ to pc on the top (invert: on its NOT, as if !c { ... }): the lanes it holds go on, the others to pc. */
static bool selects(Batch *b, unsigned count, uint32_t jz);
static void select_arm(Batch *b, const uint32_t *on, unsigned count, uint32_t jz);

static void branch(Batch *b, uint32_t pc, bool invert, uint32_t jz) {
    Val c = b->stack[--b->sp];
    if (c.kind == UNIFORM) {
        if ((c.value == 0) != invert) send(b, pc, b->active), b->none = true;
        return;
    }
    const uint32_t *parent = running(b), *on;
    unsigned count = 0, n = b->n;
    if (c.kind == LANES && c.u32 && !parent && !invert) { /* a comparison's 1s and 0s: the lanes going on */
        on = c.lane;
        if (c.ones) count = c.ones - 1u; /* counted as they were made */
        else for (unsigned l = 0; l < n; ++l) count += on[l];
    } else {
        uint32_t *out = fresh(b);
        if (!out) return;
        if (c.kind == LANES && c.u32) { /* a comparison's 1s and 0s, in the running lanes */
            const uint32_t *yes = c.lane;
            if (!parent) for (unsigned l = 0; l < n; ++l) out[l] = yes[l] == 0, count += out[l];
            else if (invert) for (unsigned l = 0; l < n; ++l) out[l] = parent[l] & (yes[l] == 0), count += out[l];
            else for (unsigned l = 0; l < n; ++l) out[l] = parent[l] & yes[l], count += out[l];
        } else
            for (unsigned l = 0; l < n; ++l) out[l] = (parent ? parent[l] : 1) & ((lane_of(&c, l) != 0) != invert), count += out[l];
        release(b, &c);
        on = out;
    }
    Mask *yes = new_mask(b, on, b->active, count), *no = new_mask(b, NULL, b->active, (b->active ? b->active->count : b->n) - count);
    if (!yes || !no) return;
    yes->sibling = no, no->sibling = yes;
    if (no->count) send(b, pc, no);
    if (count) b->active = yes;
    else b->none = true;
    if (count && selects(b, count, jz)) select_arm(b, on, count, jz);
}

/* Whether the arm of the JZ at jz (its block to its target, or to a JMP
 * ending it: an else) runs in its count lanes alone: a line of code (no
 * branch, loop or break in it) of a few instructions at least, the lanes
 * few (PURE_SELECT in the environment: 0 none, all every arm; else a
 * quarter of the lanes at most), no selection running, no lock-step
 * nested loop's round running (not yet). */
static bool selects(Batch *b, unsigned count, uint32_t jz) {
    static int mode = -1;
    if (mode < 0) {
        const char *given = getenv("PURE_SELECT");
        mode = !given ? 1 : !strcmp(given, "0") ? 0 : !strcmp(given, "all") ? 2 : 1;
    }
    const PureModule *m = b->m;
    uint32_t end = m->code[jz].arg;
    if (!mode || b->sel || (b->nest.count && !b->parent) || (mode == 1 && count > b->n / 4) || end < jz + 4) return false;
    for (uint32_t at = jz + 1; at < end; ++at)
        switch (m->code[at].op) {
        case PURE_OP_JZ: case PURE_OP_LOOP: case PURE_OP_EACH: case PURE_OP_BREAK: return false;
        case PURE_OP_JMP: return at == end - 1 && at > jz + 3;
        default:;
        }
    return true;
}

/* The arm of the JZ at jz run in the count lanes on alone (its lanes
 * j: their lanes around sel[j]), every lane running, until it ends. */
static void select_arm(Batch *b, const uint32_t *on, unsigned count, uint32_t jz) {
    uint32_t *sel = fresh(b);
    if (!sel) return;
    for (unsigned l = 0, j = 0; l < b->n; ++l)
        if (on[l]) sel[j++] = l;
    Selection *se = &b->selection;
    uint32_t end = b->m->code[jz].arg;
    se->sel = sel, se->n = b->n, se->active = b->active, se->jmp = b->m->code[end - 1].op == PURE_OP_JMP;
    se->end = se->jmp ? end - 1 : end;
    memset(se->set, 0, sizeof(se->set));
    b->sel = sel, b->n = count, b->active = NULL;
}

/* The selection running ends: its lanes the lanes around again, each local
 * it set as the arm's lanes left it (the others' as they were). */
static void deselect_arm(Batch *b) {
    Selection *se = &b->selection;
    unsigned count = b->n, outer = se->n;
    const uint32_t *sel = b->sel;
    b->sel = NULL, b->n = outer, b->active = se->active;
    for (unsigned w = 0; w < LOCALS / 64; ++w)
        for (uint64_t bits = se->set[w]; bits; bits &= bits - 1) {
            unsigned at = w * 64 + (unsigned)__builtin_ctzll(bits);
            Val now = b->local[at], was = se->old[at];
            uint8_t state = b->state[at], was_state = se->old_state[at];
            Mask *written = b->written[at], *was_written = se->old_written[at];
            b->local[at] = was, b->state[at] = was_state, b->written[at] = was_written;
            if (!state) continue; /* (set in no lane of the arm after all) */
            const uint32_t *in = state == 2 ? mask_lanes(b, written) : NULL; /* the arm's lanes it was set in (of the selection's) */
            double *out = fresh(b);
            if (!out) return;
            for (unsigned l = 0; l < outer; ++l) out[l] = was_state ? lane_of(&was, l) : b->ev->locals[at].num;
            for (unsigned j = 0; j < count; ++j)
                if (!in || in[j]) out[sel[j]] = lane_of(&now, j);
            b->local[at] = lanes_val(out, false), b->local[at].temp = false;
            if (was_state == 1 && !in) { /* set in every lane before and in all of these: still every lane */
                b->state[at] = 1;
                continue;
            }
            Mask *mask = se->active;
            if (in) { /* some of the selection's lanes: those, among the lanes around */
                uint32_t *lanes = fresh(b);
                if (!lanes) return;
                unsigned k = 0;
                memset(lanes, 0, outer * sizeof(*lanes));
                for (unsigned j = 0; j < count; ++j) lanes[sel[j]] = in[j], k += in[j];
                if (!(mask = new_mask(b, lanes, se->active, k))) return;
            }
            if (was_state == 2) {
                if (!(mask = union_of(b, was_written, mask))) return;
            } else if (was_state == 1) {
                b->state[at] = 1;
                continue;
            }
            b->state[at] = 2, b->written[at] = mask;
        }
}

/* ---- Reductions */

bool pure_batch_reduces(const PureModule *m, uint32_t pc, uint32_t end, uint32_t *at) {
    unsigned h = m->height[pc], x = m->code[pc].arg & 0xFF;
    for (uint32_t i = pc + 1; i + 1 < end; ++i) {
        const PureOp *op = &m->code[i];
        if (op->op == PURE_OP_JZ || op->op == PURE_OP_JMP || op->op == PURE_OP_LOOP || op->op == PURE_OP_EACH || op->op == PURE_OP_BREAK ||
            m->height[i] < h + 1)
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
    if (b->reduce_count == REDUCES) {
        b->failed = true;
        return NULL;
    }
    Reduce *r = &b->reduce[b->reduce_count++];
    *r = (Reduce){local, op, NULL, NULL, false, 0, op == PURE_OP_ADD ? -0.0 : op == PURE_OP_MIN ? INFINITY : -INFINITY, 0};
    return r;
}

/* The reduction's lanes (the lanes around's), each as every lane's so far. false: none left. */
static bool reduce_lanes(Batch *b, Reduce *r) {
    double *partial = fresh(b);
    uint32_t *count = partial ? fresh(b) : NULL;
    if (!count) {
        b->failed = true;
        return false;
    }
    for (unsigned l = 0, n = b->parent ? b->outer_n : b->sel ? b->selection.n : b->n; l < n; ++l) partial[l] = r->uniform, count[l] = r->uniform_count;
    r->partial = partial, r->count = count;
    return true;
}

/* Each lane's partial the least (most: the most) of it and its number, as
 * minimum and maximum: each number as an integer in their order (-0 below
 * +0), one loop of vector code. */
static void least_most(double *restrict p, const double *restrict es, uint32_t *restrict c, unsigned n, bool most) {
    for (unsigned l = 0; l < n; ++l) {
        double x = p[l], v = es[l];
        int64_t u, w;
        memcpy(&u, &x, sizeof(u)), memcpy(&w, &v, sizeof(w));
        u ^= (int64_t)((uint64_t)(u >> 63) >> 1), w ^= (int64_t)((uint64_t)(w >> 63) >> 1);
        p[l] = (most ? u >= w : u <= w) ? x : v;
        ++c[l];
    }
}

/* The least (most: the most) of n numbers, as minimum and maximum. */
static double least_most_of(const double *p, unsigned n, bool most) {
    int64_t m = most ? INT64_MIN : INT64_MAX;
    for (unsigned l = 0; l < n; ++l) {
        int64_t u;
        memcpy(&u, &p[l], sizeof(u));
        u ^= (int64_t)((uint64_t)(u >> 63) >> 1);
        m = most ? (u > m ? u : m) : (u < m ? u : m);
    }
    m ^= (int64_t)((uint64_t)(m >> 63) >> 1); /* back to its bits */
    double v;
    memcpy(&v, &m, sizeof(v));
    return v;
}

/* x = x op e in each running lane: e into its partial. */
static void reduce_step(Batch *b, Reduce *r, const Val *e) {
    const uint32_t *run = running(b);
    if (!r->partial) {
        if (e->kind == UNIFORM && !run && !b->sel && !b->parent) { /* the same e in every lane: as each lane would, once for all */
            double v = e->value;
            if (r->op == PURE_OP_ADD) {
                r->uniform += v, r->loose |= v != floor(v);
                if (!(v == floor(v) && fabs(v) <= 0x1p20)) r->loose |= !(fabs(r->uniform) <= 0x1p53);
            } else
                r->uniform = r->op == PURE_OP_MIN ? minimum(r->uniform, v) : maximum(r->uniform, v);
            ++r->uniform_count;
            return;
        }
        if (!reduce_lanes(b, r)) return;
    }
    double *p = r->partial;
    uint32_t *c = r->count;
    unsigned n = b->n;
    if (b->sel) { /* a selection's lanes: each into its lane around (an item's outer lane in a spread walk) */
        const uint32_t *sel = b->sel, *parent = b->parent;
        uint8_t fraction = 0, far = 0;
        for (unsigned j = 0; j < n; ++j) {
            if (run && !run[j]) continue;
            double v = lane_of(e, j);
            unsigned o = parent ? parent[sel[j]] : sel[j];
            p[o] = r->op == PURE_OP_ADD ? p[o] + v : r->op == PURE_OP_MIN ? minimum(p[o], v) : maximum(p[o], v);
            ++c[o], fraction |= v != floor(v), far |= !(fabs(p[o]) <= 0x1p53);
        }
        if (r->op == PURE_OP_ADD) r->loose |= fraction | far;
        return;
    }
    if (b->parent) { /* spread items: each into its outer lane's, in their order (an outer lane's items in turn) */
        const uint32_t *parent = b->parent;
        if (e->kind == LANES && e->u32 && r->op == PURE_OP_ADD) { /* whole numbers below 2^32: no fraction, the sums only growing */
            const uint32_t *es = e->lane;
            uint8_t far = 0;
            for (unsigned j = 0; j < n; ++j) {
                if (run && !run[j]) continue;
                unsigned o = parent[j];
                p[o] += es[j], ++c[o];
                far |= !(p[o] <= 0x1p53);
            }
            r->loose |= far;
            return;
        }
        const double *es = numbers(b, e);
        if (!es) return;
        if (r->op == PURE_OP_ADD) {
            uint8_t fraction = 0, far = 0;
            for (unsigned j = 0; j < n; ++j) fraction |= es[j] != floor(es[j]);
            for (unsigned j = 0; j < n; ++j) {
                if (run && !run[j]) continue;
                unsigned o = parent[j];
                p[o] += es[j], ++c[o];
                far |= !(fabs(p[o]) <= 0x1p53); /* past 2^53, a lane's sum of whole numbers may round */
            }
            r->loose |= fraction | far;
        } else
            for (unsigned j = 0; j < n; ++j) {
                if (run && !run[j]) continue;
                unsigned o = parent[j];
                p[o] = r->op == PURE_OP_MIN ? minimum(p[o], es[j]) : maximum(p[o], es[j]), ++c[o];
            }
        if (es != e->lane && es != b->var) b->spare[b->spare_count++] = (void *)es;
        return;
    }
    if (e->kind == UNIFORM && r->op == PURE_OP_ADD) { /* a count's +1, say */
        double v = e->value;
        if (!run) for (unsigned l = 0; l < n; ++l) p[l] += v, c[l] += 1;
        else for (unsigned l = 0; l < n; ++l) p[l] = run[l] ? p[l] + v : p[l], c[l] += run[l];
        r->loose |= v != floor(v);
        if (v == floor(v) && fabs(v) <= 0x1p20) return; /* a lane's at most 2^31 of them: below 2^53 */
    } else if (r->op != PURE_OP_ADD && !run) { /* the least or the most: each number as an integer in their order (-0 below +0) */
        const double *es = numbers(b, e);
        if (!es) return;
        least_most(p, es, c, n, r->op == PURE_OP_MAX);
        if (es != e->lane && es != b->var) b->spare[b->spare_count++] = (void *)es;
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

/* Where the recurrences' rounds keep a number that stays (one source's
 * there already, the same constant's or the same field's, LOAD's or local's),
 * or -1 when they hold no more. */
static int number(Recurring *rc, uint8_t kind, uint32_t arg, double value) {
    for (unsigned i = 0; i < rc->source_count; ++i)
        if (rc->sources[i].kind == kind && (kind == S_CONST ? !memcmp(&rc->sources[i].value, &value, sizeof(value)) : rc->sources[i].arg == arg))
            return (int)(V_NUMBER + i);
    if (V_NUMBER + rc->source_count == VALUES) return -1;
    rc->sources[rc->source_count] = (Source){kind, arg, value};
    return (int)(V_NUMBER + rc->source_count++);
}

/* The recurrences of the plan's block (their sets planned PLAN_RECUR): each
 * x = e as steps, each other read of x a record, and a round's actions (a
 * slot: the loop's local, LOCALS a walk's). false: more than a batch holds
 * (counted by the numbers' sources: two of the same value are two). */
static bool plan_recurrences(const PureModule *m, PureBatchPlan *p, unsigned slot) {
    uint32_t start = p->start, end = p->end;
    Recurring *rc = NULL;
    for (uint32_t pc = start; pc < end; ++pc) {
        const PureOp *op = &m->code[pc];
        if (op->op == PURE_OP_LOOP || op->op == PURE_OP_EACH) pc += op->arg >> 16;
        if (op->op != PURE_OP_LOCAL_STORE || !(op->plan & PLAN_RECUR)) continue;
        if (!rc && !(rc = p->recur = calloc(1, sizeof(*rc)))) return false;
        unsigned local = op->arg & 0xFF, x = 0;
        while (x < rc->recur_count && rc->recur_local[x] != local) ++x;
        if (x == rc->recur_count) {
            if (x == RECURS) return false;
            rc->recur_local[rc->recur_count++] = local;
        }
        uint32_t from;
        if (rc->update_count == UPDATES || !pure_batch_recurs(m, pc, start, p->sets, slot, &from)) return false;
        Update *u = &rc->updates[rc->update_count++];
        *u = (Update){from, pc, x, rc->step_count, 0, 0};
        uint8_t operands[PURE_MODULE_STACK];
        unsigned depth = 0;
        for (uint32_t at = from; at < pc; ++at) {
            const PureOp *e = &m->code[at];
            unsigned l = e->arg & 0xFF, r = 0;
            int v = -1;
            switch (e->op) {
            case PURE_OP_CONST: v = number(rc, S_CONST, 0, e->value); break;
            case PURE_OP_FIELD: v = number(rc, S_FIELD, e->arg, 0); break;
            case PURE_OP_LOAD: v = number(rc, S_LOAD, e->arg, 0); break;
            case PURE_OP_LOCAL_LOAD:
                while (r < rc->recur_count && rc->recur_local[r] != l) ++r;
                v = l == slot ? V_SLOT : r < rc->recur_count ? (int)(V_X + r) : number(rc, S_LOCAL, l, 0); /* the block does not set it */
                break;
            default: { /* an operation of one number (NEG, NOT, FLOOR) or two */
                bool one = e->op == PURE_OP_NEG || e->op == PURE_OP_NOT || e->op == PURE_OP_FLOOR;
                uint8_t c = operands[--depth], a = one ? c : operands[--depth];
                for (unsigned i = u->first; i < rc->step_count && v < 0; ++i) /* computed before in e (x the same until e is done) */
                    if (rc->steps[i].op == e->op && rc->steps[i].a == a && rc->steps[i].c == c) v = (int)i;
                if (v < 0 && rc->step_count < STEPS) rc->steps[rc->step_count] = (Step){e->op, a, c}, v = (int)rc->step_count++;
            }
            }
            if (v < 0) return false; /* more steps or numbers than it holds */
            operands[depth++] = (uint8_t)v;
        }
        u->count = rc->step_count - u->first, u->value = operands[0];
        p->at[from - start].role = AT_UPDATE, p->at[from - start].index = (uint8_t)(rc->update_count - 1);
    }
    if (!rc) return true;
    for (uint32_t pc = start; pc < end; ++pc) { /* the block's other reads of a recurrence */
        const PureOp *op = &m->code[pc];
        if (op->op == PURE_OP_LOOP || op->op == PURE_OP_EACH) pc += op->arg >> 16;
        if (op->op != PURE_OP_LOCAL_LOAD) continue;
        unsigned x = 0, u = 0;
        while (x < rc->recur_count && rc->recur_local[x] != (op->arg & 0xFF)) ++x;
        while (u < rc->update_count && !(pc >= rc->updates[u].from && pc < rc->updates[u].set)) ++u;
        if (x == rc->recur_count || u < rc->update_count) continue;
        if (rc->record_count == RECORDS) return false;
        Record *record = &rc->records[rc->record_count++];
        *record = (Record){pc, x, 0, false};
        for (unsigned j = 0; j + 1 < rc->record_count && !record->shared; ++j) { /* x read before where it was not set since: those lanes */
            bool set = rc->records[j].x != x;
            for (unsigned v = 0; v < rc->update_count && !set; ++v)
                set = rc->updates[v].x == x && rc->updates[v].set > rc->records[j].pc && rc->updates[v].set < pc;
            if (!set) record->of = rc->records[j].shared ? rc->records[j].of : j, record->shared = true;
        }
        p->at[pc - start].role = AT_RECORD, p->at[pc - start].index = (uint8_t)(rc->record_count - 1);
    }
    unsigned u = 0, k = 0; /* a round's actions, in the block's order */
    while (u < rc->update_count || k < rc->record_count) {
        if (k < rc->record_count && rc->records[k].pc < (u < rc->update_count ? rc->updates[u].from : UINT32_MAX)) {
            if (!rc->records[k].shared) rc->actions[rc->action_count++] = (Action){A_RECORD, (uint8_t)(V_X + rc->records[k].x), (uint8_t)k, 0};
            ++k;
            continue;
        }
        const Update *up = &rc->updates[u++];
        for (unsigned i = up->first; i < up->first + up->count; ++i)
            rc->actions[rc->action_count++] = (Action){rc->steps[i].op, rc->steps[i].a, rc->steps[i].c, (uint8_t)i};
        if (up->count && up->value == up->first + up->count - 1) /* its last step's: x itself (nothing after it reads that step) */
            rc->actions[rc->action_count - 1].to = (uint8_t)(V_X + up->x);
        else
            rc->actions[rc->action_count++] = (Action){A_SET, up->value, up->value, (uint8_t)(V_X + up->x)};
    }
    return true;
}

/* The batch's recurrences: their values as the rounds before left them, the
 * numbers that stay, and lanes for the records. false: no lanes left. */
static bool recurrences(Batch *b) {
    const Recurring *rc = b->plan->recur;
    if (!rc) return true;
    for (unsigned x = 0; x < rc->recur_count; ++x) b->recur_value[x] = b->ev->locals[rc->recur_local[x]].num;
    for (unsigned i = 0; i < rc->source_count; ++i) {
        const Source *source = &rc->sources[i];
        b->values[V_NUMBER + i] = source->kind == S_CONST ? source->value
                                  : source->kind == S_FIELD ? field_value(b, source->arg)
                                  : source->kind == S_LOAD  ? b->frame->values[source->arg].num
                                                            : b->ev->locals[source->arg].num;
    }
    for (unsigned k = 0; k < rc->record_count; ++k)
        if (!(b->record_lanes[k] = rc->records[k].shared ? b->record_lanes[rc->records[k].of] : fresh(b))) return false;
    return true;
}

/* The recurrences round by round, in the rounds' order (as the executor
 * computes them): each record's lane. Their steps, sets of x and records
 * laid out once in the block's order (the plan's actions), each a number
 * from two others (an update's last step setting x itself), so a round is
 * one loop. false: a round faults. */
static bool recur(Batch *b) {
    const Recurring *rc = b->plan->recur;
    if (!rc || !rc->update_count) return true;
    const Action *actions = rc->actions;
    double *const *lanes = b->record_lanes;
    unsigned count = rc->action_count;
    double *restrict values = b->values;
    for (unsigned x = 0; x < rc->recur_count; ++x) values[V_X + x] = b->recur_value[x];
    for (unsigned r = 0, n = b->n; r < n; ++r) {
        unsigned lane = round_of(b, r);
        values[V_SLOT] = r ? b->first + (double)r : b->first;
        for (unsigned j = 0; j < count; ++j) {
            const Action *t = &actions[j];
            double a = values[t->a], c = values[t->c], v;
            switch (t->op) { /* as the executor computes each (a fault: not finite, or 0 divided by) */
            case A_RECORD: lanes[t->c][lane] = a; continue;
            case A_SET: values[t->to] = a; continue;
            case PURE_OP_NEG: v = -a; break;
            case PURE_OP_NOT: v = a == 0; break;
            case PURE_OP_FLOOR: v = floor(a); break;
            case PURE_OP_ADD: v = a + c; break;
            case PURE_OP_SUB: v = a - c; break;
            case PURE_OP_MUL: v = a * c; break;
            case PURE_OP_DIV:
                if (c == 0) return false;
                v = a / c;
                break;
            default:
                if (!number_op(t->op, a, c, &v)) return false;
            }
            if (not_finite(v)) return false;
            values[t->to] = v;
        }
    }
    for (unsigned x = 0; x < rc->recur_count; ++x) b->recur_value[x] = values[V_X + x];
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
        if (op->op == PURE_OP_JZ || op->op == PURE_OP_JMP || op->op == PURE_OP_LOOP || op->op == PURE_OP_EACH || op->op == PURE_OP_BREAK)
            return true;
        if ((op->op == PURE_OP_ITEM_GET && list_of(op->arg) == list) ||
            ((op->op == PURE_OP_LIST_FIND || op->op == PURE_OP_LIST_HAS || op->op == PURE_OP_LIST_COUNT) && op->arg == list))
            return true;
    }
    return false;
}

/* The block's statements L[k].f = L[k].f + e (k computed twice the same,
 * reading no L; e reading no L; the set at k's row as its find found it),
 * into the plan. false: more than a batch holds. */
static bool plan_sums(const PureModule *m, PureBatchPlan *p) {
    uint32_t start = p->start, end = p->end;
    for (uint32_t set = start; set < end; ++set) {
        const PureOp *op = &m->code[set];
        if (op->op == PURE_OP_LOOP || op->op == PURE_OP_EACH) set += op->arg >> 16;
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
        if (p->sum_count == SUMS) return false;
        p->sums[p->sum_count++] = (Sum){again, read + 2, set - 1, set};
    }
    return true;
}

/* What the load can tell of a value a segment makes: uniform, affine (or
 * uniform, as its parts come out), or neither (no segment holds it). */
enum { K_NONE, K_UNIFORM, K_AFFINE };

/* The plan's segments (docs/design/025, stage 2): each run of instructions
 * with no other part in the plan (no role, no x = x op e, no value the same
 * code makes again or keeps), no jump landing inside, two or more, that
 * makes one value from values it makes itself, each uniform or affine as
 * far as the load can tell: a constant, LOAD, field, INDEX_LOAD, a walk's
 * item (not the loop's own walk's, nor one walked in the block), a local
 * the block does not set, the loop's value, a nested count loop's value in
 * its block, a local set once in the block by a segment before here; an
 * item's field at a uniform row (not a FIND's joined read); NEG, NOT (not
 * before a JZ), FLOOR, arithmetic and comparisons by the pass's rules. Its
 * LOCAL_STORE too when the statement ends there. Each is checked again as
 * it runs (segment). slot: the loop's local (LOCALS: a walk's); walk: the
 * list a walk walks (-1: a count's loop). */
static void plan_segments(const PureModule *m, PureBatchPlan *p, unsigned slot, int walk) {
    uint32_t start = p->start, end = p->end;
    uint8_t *landed = calloc(end - start + 1, 1), stores[LOCALS] = {0}, local[LOCALS] = {0}, kinds[PURE_MODULE_STACK + 1];
    Segment found[SEGMENTS];
    unsigned count = 0;
    if (!landed) return;
    for (uint32_t pc = start; pc < end; ++pc) { /* where jumps land, where nested blocks begin and end; how often each local is set */
        const PureOp *op = &m->code[pc];
        if ((op->op == PURE_OP_JZ || op->op == PURE_OP_JMP) && op->arg >= start && op->arg <= end) landed[op->arg - start] = 1;
        if (op->op == PURE_OP_LOOP || op->op == PURE_OP_EACH) landed[pc + 1 - start] = landed[pc + 1 + (op->arg >> 16) - start] = 1;
        if ((op->op == PURE_OP_LOCAL_STORE || op->op == PURE_OP_LOOP) && stores[op->arg & 0xFF] < 2) ++stores[op->arg & 0xFF];
    }
    typedef struct { uint32_t end; unsigned slot; bool each; } Around;
    Around nest[8];
    unsigned depth = 0, walks = 0; /* walks: the nested walks around pc */
    for (uint32_t pc = start; pc < end;) {
        while (depth && pc >= nest[depth - 1].end) {
            --depth;
            if (nest[depth].each) --walks;
            else local[nest[depth].slot] = K_NONE; /* its last value, whatever it was */
        }
        const PureOp *op = &m->code[pc];
        if (op->op == PURE_OP_LOOP || op->op == PURE_OP_EACH) {
            bool each = op->op == PURE_OP_EACH;
            if (depth == 8) break; /* (deeper: no more segments) */
            nest[depth++] = (Around){pc + 1 + (op->arg >> 16), op->arg & 0xFF, each};
            if (each) ++walks;
            else local[op->arg & 0xFF] = K_UNIFORM; /* the same round in every lane, as a rule */
            ++pc;
            continue;
        }
        uint32_t to = 0;
        unsigned h0 = m->height[pc];
        for (uint32_t q = pc; q < end && (q == pc || !landed[q - start]); ++q) {
            const PureOp *o = &m->code[q];
            const PlanAt *at = &p->at[q - start];
            if (at->role || at->by || m->reuse_from[q] || m->reuse_length[q]) break;
            unsigned h = m->height[q], pops = 0, k = K_NONE, l = o->arg & 0xFF;
            switch (o->op) {
            case PURE_OP_CONST: case PURE_OP_LOAD: case PURE_OP_FIELD: case PURE_OP_INDEX_LOAD: k = K_UNIFORM; break;
            case PURE_OP_ROW: k = walks || (int)o->arg == walk ? K_NONE : K_UNIFORM; break;
            case PURE_OP_LOCAL_LOAD: k = l == slot ? K_AFFINE : !(p->sets[l / 64] >> (l % 64) & 1) ? K_UNIFORM : local[l]; break;
            case PURE_OP_ITEM_GET:
                pops = 1;
                if (!(q > start && m->code[q - 1].op == PURE_OP_LIST_FIND && m->code[q - 1].plan & PLAN_JOINED) && h >= h0 + 1 &&
                    kinds[h - 1] == K_UNIFORM)
                    k = K_UNIFORM;
                break;
            case PURE_OP_NOT:
                if (q + 1 < end && m->code[q + 1].op == PURE_OP_JZ) break; /* a branch */
                /* fall through */
            case PURE_OP_NEG: case PURE_OP_FLOOR:
                pops = 1;
                if (h >= h0 + 1) k = kinds[h - 1] == K_UNIFORM ? K_UNIFORM : o->op == PURE_OP_FLOOR ? kinds[h - 1] : K_NONE;
                break;
            case PURE_OP_ADD: case PURE_OP_SUB: case PURE_OP_MUL: case PURE_OP_DIV: case PURE_OP_MOD: case PURE_OP_MIN: case PURE_OP_MAX:
            case PURE_OP_LT: case PURE_OP_LE: case PURE_OP_GT: case PURE_OP_GE: case PURE_OP_EQ: case PURE_OP_NE: case PURE_OP_AND: case PURE_OP_OR:
                pops = 2;
                if (h >= h0 + 2 && kinds[h - 2] && kinds[h - 1]) {
                    unsigned x = kinds[h - 2], y = kinds[h - 1];
                    if (x == K_UNIFORM && y == K_UNIFORM) k = K_UNIFORM;
                    else if (o->op == PURE_OP_ADD || o->op == PURE_OP_SUB) k = K_AFFINE;
                    else if (o->op == PURE_OP_MUL && (x == K_UNIFORM || y == K_UNIFORM)) k = K_AFFINE;
                }
                break;
            default: break;
            }
            if (k == K_NONE || h < h0 + pops) break; /* not one, or its operands made before the run */
            kinds[h - pops] = (uint8_t)k;
            if (m->height[q + 1] == h0 + 1) to = q + 1; /* one value made so far */
        }
        if (to && to - pc >= 2 && count < SEGMENTS) {
            const PureOp *set = &m->code[to];
            bool store = to < end && !landed[to - start] && set->op == PURE_OP_LOCAL_STORE && m->height[to + 1] == h0 && !p->at[to - start].role &&
                         !m->reuse_from[to] && !m->reuse_length[to];
            if (store && stores[set->arg & 0xFF] == 1) local[set->arg & 0xFF] = kinds[h0];
            else if (store) local[set->arg & 0xFF] = K_NONE;
            p->at[pc - start].role = AT_SEGMENT, p->at[pc - start].index = (uint8_t)count;
            found[count++] = (Segment){to, store};
            pc = store ? to + 1 : to;
            continue;
        }
        if (op->op == PURE_OP_LOCAL_STORE) local[op->arg & 0xFF] = K_NONE; /* set otherwise: not known here on */
        ++pc;
    }
    free(landed);
    if (count && (p->segments = malloc(count * sizeof(*p->segments)))) memcpy(p->segments, found, count * sizeof(*p->segments)), p->segment_count = count;
    else /* (no memory: the pass's way throughout) */
        for (uint32_t pc = start; pc < end; ++pc)
            if (p->at[pc - start].role == AT_SEGMENT) p->at[pc - start].role = AT_PLAIN;
}

/* The plan of the batch loop at pc, or NULL (no memory). */
static PureBatchPlan *plan_loop(const PureModule *m, uint32_t pc) {
    uint32_t start = pc + 1, end = start + (m->code[pc].arg >> 16);
    PureBatchPlan *p = calloc(1, sizeof(*p) + (size_t)(end - start) * sizeof(PlanAt));
    if (!p) return NULL;
    p->loop = pc, p->start = start, p->end = end;
    for (uint32_t at = start; at < end; ++at) {
        const PureOp *op = &m->code[at];
        uint32_t by;
        p->items |= op->op == PURE_OP_ITEM_SET;
        if (op->op == PURE_OP_LOCAL_STORE || op->op == PURE_OP_LOOP) p->sets[(op->arg & 0xFF) / 64] |= UINT64_C(1) << (op->arg & 63);
        if (op->op == PURE_OP_LOCAL_LOAD && pure_batch_reduces(m, at, end, &by)) {
            if (by - at > UINT16_MAX) p->failed = true;
            else p->at[at - start].by = (uint16_t)(by - at);
        }
    }
    if (!plan_recurrences(m, p, m->code[pc].op == PURE_OP_EACH ? LOCALS : m->code[pc].arg & 0xFF)) p->failed = true;
    if (p->items && !plan_sums(m, p)) p->failed = true;
    for (unsigned k = 0; k < p->sum_count; ++k) { /* (over a record's: the pass skips the sum's key again) */
        const Sum *sum = &p->sums[k];
        p->at[sum->again - start].role = AT_AGAIN, p->at[sum->add - start].role = AT_ADD, p->at[sum->set - start].role = AT_SET;
        p->at[sum->again - start].index = p->at[sum->add - start].index = p->at[sum->set - start].index = (uint8_t)k;
    }
    if (!p->failed) plan_segments(m, p, m->code[pc].op == PURE_OP_EACH ? LOCALS : m->code[pc].arg & 0xFF,
                                  m->code[pc].op == PURE_OP_EACH ? (int)(m->code[pc].arg >> 8 & 0xFF) : -1);
    return p;
}

bool pure_batch_plans(PureModule *m) {
    for (uint32_t pc = 0; pc < m->code_count; ++pc) {
        const PureOp *op = &m->code[pc];
        if ((op->op != PURE_OP_LOOP && op->op != PURE_OP_EACH) || !(op->plan & PLAN_BATCH)) continue;
        if (!(m->batch_plan_count & (m->batch_plan_count - 1))) { /* room doubled at each power of two */
            PureBatchPlan **more = realloc(m->batch_plans, (m->batch_plan_count ? 2 * m->batch_plan_count : 1) * sizeof(*more));
            if (!more) return false;
            m->batch_plans = more;
        }
        if (!(m->batch_plans[m->batch_plan_count] = plan_loop(m, pc))) return false;
        ++m->batch_plan_count;
    }
    return true;
}

void pure_batch_plans_free(PureModule *m) {
    for (uint32_t i = 0; i < m->batch_plan_count; ++i) free(m->batch_plans[i]->recur), free(m->batch_plans[i]->segments), free(m->batch_plans[i]);
    free(m->batch_plans);
    m->batch_plans = NULL, m->batch_plan_count = 0;
}

/* The plan of the loop whose LOOP (or EACH) is at pc (the plans in their loops' order), or NULL. */
static const PureBatchPlan *plan_of(const PureModule *m, uint32_t pc) {
    uint32_t low = 0, high = m->batch_plan_count;
    while (low < high) {
        uint32_t mid = low + (high - low) / 2;
        if (m->batch_plans[mid]->loop < pc) low = mid + 1;
        else high = mid;
    }
    return low < m->batch_plan_count && m->batch_plans[low]->loop == pc ? m->batch_plans[low] : NULL;
}

/* The segment at from (docs/design/025, stage 2) as the pass would run
 * it, each value a scalar (uniform, or affine: its first lane's and its
 * step) by the pass's own rules: its value pushed (and its LOCAL_STORE
 * run). false: a value is not as the load planned (lanes, not whole, a
 * fault, past a list's end ...): nothing done, the pass's way then. */
static bool segment(Batch *b, uint32_t from, const Segment *sg) {
    const PureModule *m = b->m;
    Val r[PURE_MODULE_STACK], reads[4];
    uint32_t read_list[4], read_field[4];
    unsigned sp = 0, read_count = 0;
    for (uint32_t pc = from; pc < sg->to; ++pc) {
        const PureOp *op = &m->code[pc];
        uint32_t arg = op->arg;
        switch (op->op) {
        case PURE_OP_CONST: r[sp++] = uniform(op->value, 0); break;
        case PURE_OP_LOAD: r[sp++] = uniform(b->frame->values[arg].num, 0); break;
        case PURE_OP_FIELD: r[sp++] = uniform(field_value(b, arg), 0); break;
        case PURE_OP_INDEX_LOAD: r[sp++] = uniform(b->frame->values[arg].num, m->state_ref[arg]); break;
        case PURE_OP_ROW:
            if (b->nest.rows || (int)arg == b->walk) return false;
            r[sp++] = uniform((double)b->ev->row, (uint16_t)arg);
            break;
        case PURE_OP_LOCAL_LOAD: { /* as local_load, in plain lanes */
            unsigned at = arg & 0xFF;
            Val *v = &r[sp++];
            if (at == b->slot) {
                if (!affine_of(b, b->reverse ? b->first + (b->n - 1) : b->first, b->reverse ? -1 : 1, v)) return false;
            } else if (!(b->sets[at / 64] >> (at % 64) & 1)) *v = uniform(b->ev->locals[at].num, 0);
            else if (b->state[at] == 1 && (b->local[at].kind == UNIFORM || b->local[at].kind == AFFINE)) *v = b->local[at];
            else return false;
            v->owner = (uint16_t)(arg >> 16);
            break;
        }
        case PURE_OP_ITEM_GET: { /* as item_get, at a uniform row */
            Val *x = &r[sp - 1];
            uint32_t list = list_of(arg), field = item_field(arg);
            PureList *items = b->app->lists[list];
            if (x->kind != UNIFORM || stored(b, list, field) || !(x->value >= 0 && x->value < (double)pure_list_count(items)) ||
                (b->items && read_count == 4))
                return false;
            if (b->items) reads[read_count] = *x, read_list[read_count] = list, read_field[read_count++] = field;
            *x = uniform(pure_list_get(items, (size_t)x->value, field).num, 0);
            break;
        }
        case PURE_OP_NEG: case PURE_OP_NOT: case PURE_OP_FLOOR: { /* as unary */
            Val *x = &r[sp - 1];
            if (x->kind == UNIFORM) *x = uniform(op->op == PURE_OP_NEG ? -x->value : op->op == PURE_OP_NOT ? x->value == 0 : floor(x->value), 0);
            else if (op->op == PURE_OP_FLOOR && x->kind == AFFINE) x->owner = 0;
            else return false;
            break;
        }
        default: { /* an operation of two, as binary: uniform, or affine (affine_op) */
            Val y = r[--sp], *x = &r[sp - 1];
            if (x->kind == UNIFORM && y.kind == UNIFORM) {
                double value;
                if (!number_op(op->op, x->value, y.value, &value)) return false;
                *x = uniform(value, 0);
            } else if (!affine_op(b, op->op, x, &y))
                return false;
        }
        }
    }
    for (unsigned i = 0; i < read_count; ++i) read_of(b, read_list[i], read_field[i], true, &reads[i]);
    b->stack[b->sp++] = r[0];
    if (sg->store) local_store(b, m->code[sg->to].arg);
    return true;
}

/* ---- The pass */

static void nested(Batch *b, const PureOp *op, uint32_t pc);
static void walk_nested(Batch *b, const PureOp *op, uint32_t pc);

/* After the JZ at jz: a filter's (its HAS two before: HAS, NOT, JZ), the lanes its code runs in. */
static void branched(Batch *b, uint32_t jz) {
    const PureModule *m = b->m;
    if (jz < 2 || m->code[jz - 2].op != PURE_OP_LIST_HAS || !(m->code[jz - 2].plan & PLAN_FILTER)) return;
    for (unsigned f = 0; f < b->filter_count; ++f)
        if (b->filters[f].has == jz - 2)
            b->filters[f].mask = b->sel ? b->selection.active : b->active, b->filters[f].none = b->none,
            b->filters[f].sel = b->sel, b->filters[f].sel_count = b->sel ? b->n : 0;
}

/* The value at the top, computed at pc, kept for later copies of its code (never a spare while the batch runs). */
static void keep_known(Batch *b, uint32_t pc) {
    Val *v = &b->stack[b->sp - 1];
    if (v->kind == REDUCE) return;
    unsigned k = 0;
    while (k < b->known_count && b->known[k].pc != pc) ++k;
    if (k == b->known_count) {
        if (k == KNOWN) return;
        ++b->known_count;
    }
    v->temp = false;
    b->known[k] = (Known){pc, *v, b->sel, b->parent};
}

/* The filter whose HAS is at pc, as this pass reaches it: k (the top) kept, its appends from here. false: more than it holds. */
static bool open_filter(Batch *b, const PureOp *op, uint32_t pc) {
    unsigned f = 0;
    while (f < b->filter_count && b->filters[f].has != pc) ++f;
    if (f == b->filter_count) {
        if (f == FILTERS) return false;
        b->filters[b->filter_count++] = (Filter){.has = pc, .end = b->m->code[pc + 2].arg, .list = op->arg, .key = uniform(0, 0)};
    }
    Filter *fl = &b->filters[f];
    Val *k = &b->stack[b->sp - 1];
    own(b, k);
    k->temp = false; /* the filter keeps it (HAS gives a new value) */
    fl->key = *k, fl->mask = NULL, fl->sel = NULL, fl->none = false, fl->spread = b->parent != NULL, fl->first = b->append_count, fl->appends = 0;
    return true;
}

/* Each filter's rounds whose code ran this pass (spread: a spread walk's
 * filters', this chunk's; else the batch's own), in the rounds' order: its
 * round, k and its appends' cells kept; its appends then let go. false: more
 * rounds than it keeps, or no memory. */
static bool keep_filtered(Batch *b) {
    unsigned least = b->append_count;
    for (unsigned f = 0; f < b->filter_count; ++f) {
        Filter *fl = &b->filters[f];
        if (!fl->appends || fl->spread != (b->parent != NULL)) continue;
        least = fl->first < least ? fl->first : least;
        if (fl->none) continue;
        unsigned stride = 2;
        for (unsigned a = 0; a < fl->appends; ++a) stride += fl->cells[a];
        fl->stride = stride, fl->made = fl->appends;
        const uint32_t *mask = fl->mask ? mask_lanes(b, fl->mask) : NULL;
        unsigned rounds = fl->sel ? fl->sel_count : b->n, n = b->n, kept = fl->kept;
        if ((kept + rounds) * stride > filtered_room[f]) { /* room for them all (kept rounds at most FILTERED) */
            size_t room = filtered_room[f] ? filtered_room[f] : 4096;
            while (room < (kept + rounds) * stride) room *= 2;
            double *grown = realloc(filtered[f], room * sizeof(double));
            if (!grown) return false;
            filtered[f] = grown, filtered_room[f] = room;
        }
        const double *cells[FILTER_APPENDS * PURE_RECORD_FIELDS], *keys; /* the key (the lanes around's) and the cells as numbers once */
        unsigned columns = 0;
        if (fl->sel) b->n = b->selection.n ? b->selection.n : n; /* (the key was made before the selection: its lanes) */
        keys = numbers(b, &fl->key);
        b->n = n;
        for (unsigned a = 0; a < fl->appends && keys; ++a) {
            const Append *ap = &b->appends[fl->first + a];
            for (unsigned c = 0; c < ap->count && keys; ++c)
                if (!(cells[columns++] = numbers(b, &ap->cells[c]))) keys = NULL;
        }
        if (!keys) return false;
        if (kept + rounds > FILTERED) return false;
        double *r = filtered[f] + kept * stride;
        for (unsigned k = 0; k < rounds; ++k) { /* in the rounds' order: a selection's lanes are in their lanes' order */
            unsigned l, j = k;
            if (fl->sel) j = b->parent || !b->reverse ? k : rounds - 1 - k, l = fl->sel[j];
            else l = b->parent ? k : round_of(b, k);
            if (!fl->sel && mask && !mask[l]) continue;
            unsigned o = b->parent ? b->parent[l] : 0, from = fl->sel ? j : l;
            r[0] = b->parent ? (double)(b->outer_reverse ? b->outer_n - 1 - o : o) : (double)k;
            r[1] = keys[l];
            for (unsigned c = 0; c < columns; ++c) r[2 + c] = cells[c][from];
            r += stride, ++kept;
        }
        fl->kept = kept;
        for (unsigned c = 0; c < columns; ++c) spare_numbers(b, cells[c]);
        spare_numbers(b, keys);
        fl->appends = 0, fl->key = uniform(0, 0), fl->mask = NULL, fl->sel = NULL;
    }
    b->append_count = least;
    return true;
}

/* The block from start to end once for the batch's lanes: false where it failed. */

static bool pass(Batch *b, uint32_t start, uint32_t end) {
    const PureModule *m = b->m;
    Event *ev = b->ev;
    PureApp *app = b->app;
    for (uint32_t pc = start; pc < end && !b->failed;) {
        if (b->sel && pc == b->selection.end) deselect_arm(b);
        arrive(b, pc);
        if (b->none) { /* no lane here: on to where one goes */
            uint32_t next = end;
            for (unsigned s = 0; s < b->split_count; ++s)
                if (b->splits[s].pc < next) next = b->splits[s].pc;
            pc = next;
            continue;
        }
        const PlanAt *at_pc = &b->plan->at[pc - b->plan->start];
        if (at_pc->role == AT_SEGMENT && !b->sel && !b->parent) { /* uniform and affine values at once, in scalars */
            const Segment *sg = &b->plan->segments[at_pc->index];
            if (segment(b, pc, sg)) {
                pc = sg->store ? sg->to + 1 : sg->to;
                continue;
            }
        }
        if (at_pc->role == AT_UPDATE) { /* a recurrence's x = e: made ahead, round by round */
            pc = b->plan->recur->updates[at_pc->index].set + 1;
            continue;
        }
        if (at_pc->role >= AT_AGAIN && at_pc->role <= AT_SET) { /* a sum into a cell: its row and e, kept for the end */
            const Sum *sum = &b->plan->sums[at_pc->index];
            if (at_pc->role == AT_AGAIN) pc = sum->resume; /* the key again, its find and the cell's read: not needed */
            else if (at_pc->role == AT_ADD) pc = sum->set;
            else {
                const PureOp *set = &m->code[pc++];
                if (b->scatter_count == SUMS) {
                    b->failed = true;
                    break;
                }
                Scatter *sc = &b->scatters[b->scatter_count++];
                sc->list = list_of(set->arg), sc->field = item_field(set->arg), sc->add = sum->add, sc->set = sum->set;
                sc->e = b->stack[--b->sp], sc->rows = b->stack[--b->sp], sc->mask = b->sel ? b->selection.active : b->active;
                if (b->sel) sc->e = expand(b, &sc->e), sc->rows = expand_rows(b, &sc->rows);
                own(b, &sc->e), own(b, &sc->rows);
                sc->e.temp = sc->rows.temp = false;
            }
            continue;
        }
        if (m->reuses && m->reuse_length[pc]) { /* the same code as an earlier value's, nothing changed between: that value, the code skipped */
            uint32_t root = m->reuse_from[pc] - 1;
            unsigned k = 0;
            while (k < b->known_count && b->known[k].pc != root) ++k;
            if (k < b->known_count && b->known[k].parent == b->parent && (b->known[k].sel == b->sel || !b->known[k].sel)) {
                b->stack[b->sp++] = b->known[k].sel == b->sel ? b->known[k].v : gather(b, &b->known[k].v); /* kept around: in these lanes */
                pc += m->reuse_length[pc];
                continue;
            }
        }
        const PureOp *op = &m->code[pc];
        uint32_t arg = op->arg, at = pc;
        unsigned was = b->sp;
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
            if (b->sel) { /* a selection's lanes: the rows around, in them */
                Val v;
                if (b->parent) v = lanes_val((void *)b->item_rows, true), v.temp = false;
                else if (b->nest.rows) {
                    uint32_t *rows = fresh(b);
                    if (!rows) break;
                    for (unsigned j = 0, t = b->nest.t; j < b->n; ++j) rows[j] = b->nest.rows[b->sel[j]] + t;
                    v = lanes_val(rows, true), v.owner = (uint16_t)arg;
                    b->stack[b->sp++] = v;
                    break;
                } else if ((int)arg == b->walk)
                    v = (Val){.kind = AFFINE, .value = b->reverse ? b->first + (b->selection.n - 1) : b->first, .step = b->reverse ? -1 : 1};
                else {
                    b->stack[b->sp++] = uniform((double)ev->row, (uint16_t)arg);
                    break;
                }
                v = gather(b, &v);
                v.owner = (uint16_t)arg;
                b->stack[b->sp++] = v;
            } else if (b->parent) { /* spread items: each lane's */
                Val v = lanes_val((void *)b->item_rows, true);
                v.temp = false, v.owner = (uint16_t)arg;
                b->stack[b->sp++] = v;
            } else if (b->nest.rows) { /* in a nested walk: each lane's item in round t (ROW reads the innermost walk's) */
                uint32_t *rows = fresh(b);
                if (!rows) break;
                for (unsigned l = 0, t = b->nest.t; l < b->n; ++l) rows[l] = b->nest.rows[l] + t;
                Val v = lanes_val(rows, true);
                v.owner = (uint16_t)arg;
                b->stack[b->sp++] = v;
            } else if ((int)arg == b->walk) { /* the walk's item: each lane's place */
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
            if (at_pc->role != AT_RECORD) local_load(b, arg, pc - 1);
            else { /* a recurrence's value here, round by round */
                Val v = lanes_val(b->record_lanes[at_pc->index], false);
                v.temp = false;
                if (b->sel) v = gather(b, &v);
                v.owner = (uint16_t)(arg >> 16);
                b->stack[b->sp++] = v;
            }
            break;
        }
        case PURE_OP_LOCAL_STORE: local_store(b, arg); break;
        case PURE_OP_NOT:
            if (pc < end && m->code[pc].op == PURE_OP_JZ) { /* if !c { ... }: the branch the other way, no NOT */
                uint32_t jz = pc++;
                branch(b, m->code[jz].arg, true, jz), branched(b, jz);
                break;
            }
            unary(b, op->op);
            break;
        case PURE_OP_NEG: case PURE_OP_FLOOR: unary(b, op->op); break;
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
            a->pc = pc - 1, a->mask = b->sel ? b->selection.active : b->active, a->count = count;
            a->sel = b->sel, a->sel_count = b->sel ? b->n : 0;
            for (unsigned f = 0; f < b->filter_count; ++f) /* a filter's code's */
                if (pc - 1 > b->filters[f].has && pc - 1 < b->filters[f].end) {
                    Filter *fl = &b->filters[f];
                    if (fl->appends == FILTER_APPENDS) b->failed = true;
                    else fl->pc[fl->appends] = pc - 1, fl->to[fl->appends] = arg, fl->cells[fl->appends] = count, ++fl->appends;
                }
            b->sp -= count;
            for (unsigned f = 0; f < count; ++f) {
                a->cells[f] = b->stack[b->sp + f];
                own(b, &a->cells[f]);
                a->cells[f].temp = false;
            }
            break;
        }
        case PURE_OP_LIST_FIND: case PURE_OP_LIST_HAS:
            if (op->plan & PLAN_FILTER && !open_filter(b, op, pc - 1)) {
                b->failed = true;
                break;
            }
            if (!find(b, op, pc - 1)) b->failed = true;
            else if (op->op == PURE_OP_LIST_FIND && op->plan & PLAN_JOINED) ++pc; /* its ITEM_GET with it */
            break;
        case PURE_OP_JZ: branch(b, arg, false, pc - 1), branched(b, pc - 1); break;
        case PURE_OP_LOOP: nested(b, op, pc - 1), pc += arg >> 16; break;
        case PURE_OP_EACH: walk_nested(b, op, pc - 1), pc += arg >> 16; break;
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
        if (op->op == PURE_OP_LIST_FIND && op->plan & PLAN_JOINED) ++at; /* its value its ITEM_GET's */
        if (m->reuses && m->reuse_from[at] == PURE_REUSE_KEPT && !b->failed && b->sp > was) keep_known(b, at);
    }
    if (b->sel) { /* (failed within a selection, or its end the block's) */
        if (b->failed) b->sel = NULL, b->n = b->selection.n, b->active = b->selection.active;
        else deselect_arm(b);
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
    mark_lanes(kept, b->nest.count), mark_lanes(kept, b->nest.broken), mark_val(kept, &b->nest.low), mark_lanes(kept, b->nest.rows);
    mark_lanes(kept, b->parent), mark_lanes(kept, b->item_rows);
    for (unsigned f = 0; f < b->filter_count; ++f) mark_val(kept, &b->filters[f].key), mark_mask(b, kept, kept_mask, b->filters[f].mask);
    for (unsigned k = 0; k < b->known_count; ++k) mark_val(kept, &b->known[k].v);
    for (unsigned f = 0; f < b->filter_count; ++f) mark_lanes(kept, b->filters[f].sel);
    for (unsigned a = 0; a < b->append_count; ++a) mark_lanes(kept, b->appends[a].sel);
    for (unsigned r = 0; r < b->reduce_count; ++r) mark_lanes(kept, b->reduce[r].partial), mark_lanes(kept, b->reduce[r].count);
    for (unsigned r = 0; r < b->read_count; ++r) mark_val(kept, &b->reads[r].rows);
    for (unsigned s = 0; s < b->store_count; ++s)
        mark_val(kept, &b->stores[s].rows), mark_val(kept, &b->stores[s].value), mark_mask(b, kept, kept_mask, b->stores[s].mask);
    for (unsigned d = 0; d < b->sp; ++d) mark_val(kept, &b->stack[d]);
    for (unsigned r = 0; b->plan->recur && r < b->plan->recur->record_count; ++r) mark_lanes(kept, b->record_lanes[r]);
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
    b->nest = (Nest){outer, count, broken, low, NULL, 0};
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

/* A walk planned PLAN_SPREAD in the batch's block (its block from start to
 * end): its items, each lane's count of them from its row from on, spread
 * into lanes in the rounds' order (an outer lane's items in turn), up to
 * BATCH a chunk; the block once a chunk. An outer lane's value read is that
 * lane's; a reduction adds into its outer lane's in the items' order; the
 * items' own locals (read nowhere else in the loop's block) leave the last
 * item's that set them. */
static void spread(Batch *b, uint32_t start, uint32_t end, const uint32_t *count, const uint32_t *from) {
    unsigned n = b->n;
    uint32_t *parent = fresh(b), *rows = fresh(b);
    if (!parent || !rows) return;
    Mask *outer = b->active;
    Nest saved = b->nest;
    b->nest = (Nest){outer, (uint32_t *)count, NULL, uniform(0, 0), (uint32_t *)from, 0}; /* kept by the sweeps */
    bool reverse = b->reverse;
    double var_start = b->var_start, last[LOCALS];
    uint64_t wrote[LOCALS / 64] = {0};
    b->outer_n = n, b->outer_reverse = reverse, b->outer_first = b->first;
    b->parent = parent, b->item_rows = rows, b->reverse = false, b->var_start = NAN; /* (the outer loop's value is not these lanes') */
    for (unsigned r = 0, k = 0; !b->failed;) {
        unsigned m = 0;
        while (r < n && m < BATCH) {
            unsigned l = reverse ? n - 1 - r : r, take = count[l] - k < BATCH - m ? count[l] - k : BATCH - m;
            for (unsigned i = 0; i < take; ++i) parent[m + i] = l, rows[m + i] = from[l] + k + i;
            m += take, k += take;
            if (k == count[l]) ++r, k = 0;
        }
        if (!m) break;
        b->n = m, b->active = NULL, b->none = false;
        memset(b->own, 0, sizeof(b->own));
        if (!pass(b, start, end)) break;
        arrive(b, end);
        if (b->filter_count && !keep_filtered(b)) {
            b->failed = true;
            break;
        }
        for (unsigned w = 0; w < LOCALS / 64; ++w) /* each own local: the last item's that set it */
            for (uint64_t bits = b->own[w]; bits; bits &= bits - 1) {
                unsigned at = w * 64 + (unsigned)__builtin_ctzll(bits), j = m;
                const uint32_t *set = b->state[at] == 2 ? mask_lanes(b, b->written[at]) : NULL;
                if (!b->state[at]) continue;
                while (j && set && !set[j - 1]) --j;
                if (j) last[at] = lane_of(&b->local[at], j - 1), wrote[w] |= UINT64_C(1) << (at % 64);
                b->state[at] = 0;
            }
        sweep(b);
    }
    b->n = n, b->reverse = reverse, b->var_start = var_start, b->parent = b->item_rows = NULL;
    b->nest = saved;
    b->active = outer, b->none = false;
    for (unsigned w = 0; w < LOCALS / 64; ++w)
        for (uint64_t bits = wrote[w]; bits; bits &= bits - 1) {
            unsigned at = w * 64 + (unsigned)__builtin_ctzll(bits);
            b->local[at] = uniform(last[at], 0), b->state[at] = 1; /* read only after the loop: as the last round left it */
        }
}

/* The EACH at pc in the batch's block: each running lane's walk of its
 * list (of its slice's items, when sliced) together, round t for the lanes
 * with more than t items that did not break out, ROW each lane's item; its
 * work counted for each as it opens (the batch's end checks it). The list
 * changes in no round (the load plans no set or append with it). */
static void walk_nested(Batch *b, const PureOp *op, uint32_t pc) {
    uint32_t arg = op->arg, start = pc + 1, end = start + (arg >> 16), list = arg >> 8 & 0xFF;
    unsigned n = b->n, key = b->m->state_key[list] - 1u;
    const PureList *walked = b->app->lists[list];
    size_t items = pure_list_count(walked), from = 0, to = items;
    Val high = uniform(0, 0), low = uniform(0, 0);
    bool sliced = arg & PURE_EACH_SLICED;
    if (sliced) high = b->stack[--b->sp], low = b->stack[--b->sp];
    uint32_t *count = fresh(b), *broken = fresh(b), *rows = fresh(b);
    if (!count || !broken || !rows) return;
    const uint32_t *entry = running(b);
    if (sliced && low.kind == UNIFORM && high.kind == UNIFORM) { /* one slice for every lane */
        from = pure_list_bound(walked, key, (PureCell){.num = low.value}), to = pure_list_bound(walked, key, (PureCell){.num = high.value});
        sliced = false;
    }
    if (to < from) to = from;
    uint64_t length = arg >> 16;
    for (unsigned l = 0; l < n; ++l) {
        broken[l] = 0;
        if (entry && !entry[l]) {
            count[l] = rows[l] = 0;
            continue;
        }
        if (sliced) { /* as slice_of finds them */
            pure_list_slice(walked, key, lane_of(&low, l), lane_of(&high, l), &from, &to);
        }
        size_t last = to < items ? to : items;
        rows[l] = (uint32_t)from, count[l] = last > from ? (uint32_t)(last - from) : 0;
        b->work += length * count[l]; /* as the walk opens: its block for each of its items */
    }
    release(b, &high), release(b, &low);
    if (op->plan & PLAN_SPREAD) {
        spread(b, start, end, count, rows);
        return;
    }
    Mask *outer = b->active;
    Nest saved = b->nest;
    b->nest = (Nest){outer, count, broken, uniform(0, 0), rows, 0};
    uint32_t least = UINT32_MAX;
    unsigned came = 0;
    for (unsigned l = 0; l < n; ++l)
        if (!entry || entry[l]) least = count[l] < least ? count[l] : least, ++came;
    b->breaks = false;
    for (uint32_t t = 0; !b->failed; ++t) {
        uint32_t *live;
        unsigned alive = 0;
        if (t < least && !b->breaks)
            live = (uint32_t *)entry, alive = came;
        else {
            if (!(live = fresh(b))) break;
            for (unsigned l = 0; l < n; ++l) live[l] = (t < count[l]) & (broken[l] ^ 1), alive += live[l];
        }
        if (!alive) break;
        b->nest.t = t;
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
    unsigned n = b->n;
    for (unsigned i = 0; i < b->reduce_count; ++i) {
        Reduce *r = &b->reduce[i];
        const double *p = r->partial;
        double c = b->ev->locals[r->local].num; /* as the batch began */
        if (!p) { /* every lane's the same: uniform, uniform_count of them */
            double u = r->uniform;
            bool several = r->uniform_count > 1;
            if (r->op != PURE_OP_ADD) r->result = r->op == PURE_OP_MAX ? maximum(c, u) : minimum(c, u);
            else if (!r->loose && c == floor(c) && fabs(c) < 0x1p53 && n * fabs(u) + fabs(c) < 0x1p53)
                r->result = c + n * u; /* whole numbers, exact (as the lanes' sums would be) */
            else if (several && (r->loose || !(c == floor(c) && fabs(c) <= 0x1p53)))
                return false;
            else {
                for (unsigned k = 0; k < n && r->uniform_count; ++k) { /* in the rounds' order */
                    c = c + u;
                    if (!isfinite(c) || (several && fabs(c) > 0x1p53)) return false;
                }
                r->result = c;
            }
            continue;
        }
        if (r->op != PURE_OP_ADD) { /* the least or the most: the same in any order (-0 below +0), lanes that ran none at the identity */
            double m = least_most_of(p, n, r->op == PURE_OP_MAX);
            r->result = r->op == PURE_OP_MAX ? maximum(c, m) : minimum(c, m);
            continue;
        }
        if (!r->loose && c == floor(c) && fabs(c) < 0x1p53) { /* whole numbers whose magnitudes together stay below 2^53: exact in any order */
            double sum[4] = {-0.0, -0.0, -0.0, -0.0}, size[4] = {0, 0, 0, 0};
            unsigned l = 0;
            for (; l + 4 <= n; l += 4)
                for (unsigned j = 0; j < 4; ++j) sum[j] += p[l + j], size[j] += fabs(p[l + j]);
            for (; l < n; ++l) sum[0] += p[l], size[0] += fabs(p[l]);
            if ((size[0] + size[1]) + (size[2] + size[3]) + fabs(c) < 0x1p53) {
                r->result = c + ((sum[0] + sum[1]) + (sum[2] + sum[3]));
                continue;
            }
        }
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

enum { CHUNK = 128 }; /* items a call of pure_list_add_many takes */

static _Thread_local uint64_t *added_keys;  /* decide's keys added to a filter's list by its kept rounds (a hash set) */
static _Thread_local uint64_t *added_bits;  /* or, for a list whose keys are bits, a bit each (cleared after) */
static _Thread_local double *scratch;       /* and the keys its rounds add, for pure_list_takes */
static _Thread_local size_t added_room, added_bits_room, scratch_room;
static const uint64_t NO_KEY = UINT64_C(0x7FF8000000000001); /* a NaN: never a key */

static uint64_t key_of(double k) {
    if (k == 0) k = 0; /* -0 and +0 are one key */
    uint64_t bits;
    memcpy(&bits, &k, sizeof(bits));
    return bits;
}

/* Whether a list keyed by a number has key k now, as HAS asks it. */
static bool list_has(const PureList *list, unsigned key, double k) {
    const uint64_t *bits;
    size_t words;
    if (pure_list_bits(list, &bits, &words)) {
        size_t top = words * 64, at = k >= 0 && k < (double)top && k == floor(k) ? (size_t)k : top;
        return at < top && bits[at >> 6] >> (at & 63) & 1;
    }
    bool found;
    if (!pure_list_contains(list, k, &found)) pure_list_seek(list, key, (PureCell){.num = k}, &found);
    return found;
}

/* Whether k is in (add: put in) decide's keys, mask + 1 slots. */
static bool added(uint64_t bits, size_t mask, bool add) {
    size_t at = (size_t)((bits ^ bits >> 33) * UINT64_C(0xff51afd7ed558ccd) >> 20) & mask;
    for (; added_keys[at] != NO_KEY; at = (at + 1) & mask)
        if (added_keys[at] == bits) return true;
    if (add) added_keys[at] = bits;
    return false;
}

/* Each filter's kept rounds asked again in the rounds' order, its list as it
 * is and what the kept rounds before add to it: a round whose k is there by
 * now is left out (its number -1, as the executor's code skips it then);
 * then whether each list takes the rest at once (pure_list_takes) and the
 * memory budget all of them. false: the batch gives its rounds back (a key
 * an append adds there already, a list that does not take them so, the
 * memory: the executor's rounds tell where). */
static bool decide(Batch *b) {
    const PureModule *m = b->m;
    PureApp *app = b->app;
    size_t stored = 0, extra = 0;
    for (unsigned f = 0; f < b->filter_count; ++f) {
        Filter *fl = &b->filters[f];
        if (!fl->kept) continue;
        const PureList *list = app->lists[fl->list];
        unsigned key = m->state_key[fl->list] - 1u;
        const uint64_t *bits;
        size_t words, slots = 64;
        bool by_bits = pure_list_bits(list, &bits, &words);
        if (fl->kept > scratch_room) {
            double *grown = realloc(scratch, fl->kept * sizeof(*grown));
            if (!grown) return false;
            scratch = grown, scratch_room = fl->kept;
        }
        if (by_bits) { /* the keys bits: a kept round's add marked in bits of our own (a key a bit: whole, below words * 64) */
            if (words > added_bits_room) { /* clear (and cleared again after each use) */
                uint64_t *grown = realloc(added_bits, words * sizeof(*grown));
                if (!grown) return false;
                memset(grown + added_bits_room, 0, (words - added_bits_room) * sizeof(*grown));
                added_bits = grown, added_bits_room = words;
            }
            size_t top = words * 64;
            bool twice = false; /* a key marked already: the batch gives its rounds back (EXISTS at that round) */
            for (size_t i = 0; i < fl->kept && !twice; ++i) {
                double *r = filtered[f] + i * fl->stride, k = r[1];
                size_t at = k >= 0 && k < (double)top && (double)(uint32_t)k == k ? (size_t)k : top; /* top: no bit, so not there */
                if (at < top && (bits[at >> 6] >> (at & 63) & 1 || added_bits[at >> 6] >> (at & 63) & 1)) {
                    r[0] = -1;
                    continue;
                }
                for (unsigned a = 0, c = 2; a < fl->made; c += fl->cells[a++]) /* its adds to the list: keys not there yet */
                    if (fl->to[a] == fl->list) {
                        double k2 = r[c + key];
                        size_t at2 = k2 >= 0 && k2 < (double)top && (double)(uint32_t)k2 == k2 ? (size_t)k2 : top;
                        if (at2 == top || bits[at2 >> 6] >> (at2 & 63) & 1 || added_bits[at2 >> 6] >> (at2 & 63) & 1)
                            twice = true; /* (no bit for it: the list would not take it so either) */
                        else added_bits[at2 >> 6] |= UINT64_C(1) << (at2 & 63);
                    }
            }
            for (size_t i = 0; i < fl->kept; ++i) { /* the bits of our own clear again */
                const double *r = filtered[f] + i * fl->stride;
                if (r[0] < 0) continue;
                for (unsigned a = 0, c = 2; a < fl->made; c += fl->cells[a++])
                    if (fl->to[a] == fl->list) {
                        double k2 = r[c + key];
                        size_t at2 = k2 >= 0 && k2 < (double)top && (double)(uint32_t)k2 == k2 ? (size_t)k2 : top;
                        if (at2 < top) added_bits[at2 >> 6] &= ~(UINT64_C(1) << (at2 & 63));
                    }
            }
            if (twice) return false;
        } else {
            while (slots < 2 * fl->kept * (fl->made + 1)) slots *= 2;
            if (slots > added_room) {
                uint64_t *grown = realloc(added_keys, slots * sizeof(*grown));
                if (!grown) return false;
                added_keys = grown, added_room = slots;
            }
            for (size_t i = 0; i < slots; ++i) added_keys[i] = NO_KEY;
            for (size_t i = 0; i < fl->kept; ++i) {
                double *r = filtered[f] + i * fl->stride;
                if (list_has(list, key, r[1]) || added(key_of(r[1]), slots - 1, false)) {
                    r[0] = -1;
                    continue;
                }
                for (unsigned a = 0, at = 2; a < fl->made; at += fl->cells[a++]) /* its adds to the list: keys not there yet */
                    if (fl->to[a] == fl->list) {
                        double k = r[at + key];
                        if (list_has(list, key, k) || added(key_of(k), slots - 1, true)) return false;
                    }
            }
        }
        for (unsigned a = 0, at = 2; a < fl->made; at += fl->cells[a++]) {
            uint32_t to = fl->to[a];
            int keyed = m->state_key[to] ? (int)m->state_key[to] - 1 : -1;
            size_t count = 0, s, e;
            for (size_t i = 0; i < fl->kept; ++i) {
                const double *r = filtered[f] + i * fl->stride;
                if (r[0] < 0) continue;
                if (keyed >= 0) scratch[count] = r[at + (unsigned)keyed];
                ++count;
            }
            if (!(m->state_flags[to] & PURE_LIST_ROWS >> 8) || m->state_indexed[to] ||
                !pure_list_takes(app->lists[to], keyed, count, scratch, &s, &e))
                return false;
            stored += s, extra += e;
        }
    }
    return !((app->memory && app->grown + (int64_t)stored > (int64_t)app->memory) || app->spent + (int64_t)(stored + extra) > (int64_t)app->room);
}

/* The filters' appends their kept rounds still make (decide), each list's
 * at once in the rounds' order. STEP_NEXT, or STEP_REFUSED without memory. */
static int apply_filters(Batch *b, Frame *frame, uint64_t per) {
    PureApp *app = b->app;
    for (unsigned f = 0; f < b->filter_count; ++f) {
        Filter *fl = &b->filters[f];
        for (unsigned a = 0, at = 2; fl->kept && a < fl->made; at += fl->cells[a++]) {
            PureList *list = app->lists[fl->to[a]];
            size_t was = pure_list_size(list), had = pure_list_extra(list);
            double cells[PURE_RECORD_FIELDS][CHUNK];
            const double *fields[PURE_RECORD_FIELDS];
            for (unsigned c = 0; c < fl->cells[a]; ++c) fields[c] = cells[c];
            for (size_t i = 0; i < fl->kept;) {
                size_t count = 0, first = i;
                for (; i < fl->kept && count < CHUNK; ++i) {
                    const double *r = filtered[f] + i * fl->stride;
                    if (r[0] < 0) continue;
                    for (unsigned c = 0; c < fl->cells[a]; ++c) cells[c][count] = r[at + c];
                    ++count;
                }
                if (count && pure_list_add_many(list, count, fields) != count) { /* no memory (decide left no key there) */
                    app->work += (uint64_t)filtered[f][first * fl->stride] * per;
                    frame->status = PURE_ALLOCATION, frame->fault_pc = fl->pc[a], frame->sp = 0;
                    return STEP_REFUSED;
                }
            }
            app->grown += (int64_t)pure_list_size(list) - (int64_t)was; /* as grows counts them, item by item */
            app->spent += (int64_t)(pure_list_size(list) + pure_list_extra(list)) - (int64_t)(was + had);
        }
    }
    return STEP_NEXT;
}

/* v in the k rounds from r on, in their order (each round's lane's). */
static void rounds_of(const Batch *b, const Val *v, unsigned r, unsigned k, double *out) {
    if (v->kind == UNIFORM)
        for (unsigned i = 0; i < k; ++i) out[i] = v->value;
    else if (!b->reverse) {
        if (v->kind == AFFINE)
            for (unsigned i = 0; i < k; ++i) out[i] = v->value + v->step * (r + i);
        else if (v->u32)
            for (unsigned i = 0; i < k; ++i) out[i] = ((const uint32_t *)v->lane)[r + i];
        else
            memcpy(out, (const double *)v->lane + r, k * sizeof(double));
    } else {
        unsigned top = b->n - 1 - r; /* round r's lane, the lanes going down */
        if (v->kind == AFFINE)
            for (unsigned i = 0; i < k; ++i) out[i] = v->value + v->step * (top - i);
        else if (v->u32)
            for (unsigned i = 0; i < k; ++i) out[i] = ((const uint32_t *)v->lane)[top - i];
        else
            for (unsigned i = 0; i < k; ++i) out[i] = ((const double *)v->lane)[top - i];
    }
}

/* The rounds from *r on that mask runs (NULL: every one), at most CHUNK of
 * them, v's in each into out (rounds: which, when mask is not NULL); *r
 * past the last. How many. */
static unsigned chunk_of(const Batch *b, const Val *v, const uint32_t *mask, unsigned *r, unsigned n, double *out, unsigned *rounds) {
    if (!mask) {
        unsigned k = n - *r < CHUNK ? n - *r : CHUNK;
        rounds_of(b, v, *r, k, out);
        *r += k;
        return k;
    }
    unsigned k = 0;
    for (; *r < n && k < CHUNK; ++*r) {
        unsigned l = round_of(b, *r);
        if (mask[l]) rounds[k] = *r, out[k++] = lane_of(v, l);
    }
    return k;
}

/* The batch's appends from round from on at once, each list's items by
 * one call a chunk (pure_list_add_many), when every list takes its items
 * so and the memory budget takes them all (each item only grows what the
 * budget counts, so no round in between runs out): as the executor's steps
 * make them round by round, the first key there already (an append's at
 * its round, the earliest) its fault. false, nothing changed: make them
 * round by round. */
static bool at_once(Batch *b, Frame *frame, unsigned from, unsigned n, uint64_t per, int *how) {
    PureApp *app = b->app;
    const PureModule *m = b->m;
    double keys[BATCH];
    size_t stored = 0, extra = 0;
    for (unsigned i = 0; i < b->append_count; ++i) {
        const Append *a = &b->appends[i];
        uint32_t arg = m->code[a->pc].arg;
        int key = m->state_key[arg] ? (int)m->state_key[arg] - 1 : -1;
        if (!(m->state_flags[arg] & PURE_LIST_ROWS >> 8) || m->state_indexed[arg]) return false; /* an event's list, no index into it */
        for (unsigned j = 0; j < i; ++j)
            if (m->code[b->appends[j].pc].arg == arg) return false; /* two appends to one list: their items in turn */
        const uint32_t *mask = mask_lanes(b, a->mask);
        size_t count = 0, s, e;
        if (a->sel) { /* a selection's: its lanes, in their rounds' order */
            for (unsigned k = 0; k < a->sel_count; ++k) {
                unsigned j = b->reverse ? a->sel_count - 1 - k : k;
                if (round_of(b, a->sel[j]) < from) continue;
                if (key >= 0) keys[count] = lane_of(&a->cells[key], j);
                ++count;
            }
        } else if (!mask) {
            count = n - from;
            if (key >= 0) rounds_of(b, &a->cells[key], from, n - from, keys);
        } else
            for (unsigned r = from; r < n; ++r) {
                unsigned l = round_of(b, r);
                if (!mask[l]) continue;
                if (key >= 0) keys[count] = lane_of(&a->cells[key], l);
                ++count;
            }
        if (!pure_list_takes(app->lists[arg], key, count, keys, &s, &e)) return false;
        stored += s, extra += e;
    }
    if ((app->memory && app->grown + (int64_t)stored > (int64_t)app->memory) || app->spent + (int64_t)(stored + extra) > (int64_t)app->room)
        return false; /* it runs out at some round: there, round by round */
    unsigned fault = n, faulting = 0;
    for (unsigned i = 0; i < b->append_count; ++i) {
        const Append *a = &b->appends[i];
        PureList *list = app->lists[m->code[a->pc].arg];
        const uint32_t *mask = mask_lanes(b, a->mask);
        size_t was = pure_list_size(list), had = pure_list_extra(list);
        double cells[PURE_RECORD_FIELDS][CHUNK];
        const double *fields[PURE_RECORD_FIELDS];
        unsigned rounds[CHUNK];
        for (unsigned f = 0; f < a->count; ++f) fields[f] = cells[f];
        for (unsigned r = from, k = 0; r < n && r < fault;) {
            unsigned at = r, count = 0;
            if (a->sel) { /* its lanes in their rounds' order, a chunk at a time */
                for (; k < a->sel_count && count < CHUNK; ++k) {
                    unsigned j = b->reverse ? a->sel_count - 1 - k : k, round = round_of(b, a->sel[j]);
                    if (round < from) continue;
                    for (unsigned f = 0; f < a->count; ++f) cells[f][count] = lane_of(&a->cells[f], j);
                    rounds[count++] = round;
                }
                at = k < a->sel_count ? r : n;
                if (!count) break;
            } else {
                for (unsigned f = 0; f < a->count; ++f) {
                    at = r;
                    count = chunk_of(b, &a->cells[f], mask, &at, n, cells[f], rounds);
                }
                if (!mask)
                    for (unsigned k = 0; k < count; ++k) rounds[k] = r + k;
            }
            r = at;
            size_t added = count ? pure_list_add_many(list, count, fields) : 0;
            if (added == SIZE_MAX) { /* no memory: the event stops there, as the executor's step would */
                app->work += (uint64_t)rounds[0] * per;
                frame->status = PURE_ALLOCATION, frame->fault_pc = a->pc, frame->sp = 0;
                *how = STEP_REFUSED;
                return true;
            }
            if (added < count) { /* its key there already: the executor's fault at that round, unless one comes before it */
                if (rounds[added] < fault) fault = rounds[added], faulting = i;
                break;
            }
        }
        app->grown += (int64_t)pure_list_size(list) - (int64_t)was; /* as grows counts them, item by item */
        app->spent += (int64_t)(pure_list_size(list) + pure_list_extra(list)) - (int64_t)(was + had);
    }
    *how = STEP_NEXT;
    if (fault < n) { /* the event's undone: only the fault and the work so far tell */
        app->work += (uint64_t)fault * per;
        frame->kind = PURE_FAULT_EXISTS, frame->fault_pc = b->appends[faulting].pc, frame->sp = 0;
        *how = STEP_FAULT;
    }
    return true;
}

/* The batch's appends round by round, as the executor's steps make them:
 * at once where they may (at_once), tried again after a round that changed
 * a list's form (a keyed list's first items make it DENSE or HASHED). */
static int append_items(Batch *b, Frame *frame, unsigned n, uint64_t per) {
    PureApp *app = b->app;
    unsigned forms[APPENDS];
    bool again = true;
    for (unsigned r = 0; r < n; ++r) {
        if (again) {
            int how;
            if (at_once(b, frame, r, n, per, &how)) return how;
            for (unsigned i = 0; i < b->append_count; ++i) forms[i] = pure_list_form(app->lists[b->m->code[b->appends[i].pc].arg]);
            again = false;
        }
        unsigned l = round_of(b, r);
        for (unsigned i = 0; i < b->append_count; ++i) {
            const Append *a = &b->appends[i];
            const uint32_t *mask = mask_lanes(b, a->mask);
            if (mask && !mask[l]) continue;
            unsigned j = l;
            if (a->sel) { /* a selection's: its lane of l (the mask holds l: among them) */
                for (j = 0; j < a->sel_count && a->sel[j] != l; ++j);
                if (j == a->sel_count) continue;
            }
            for (unsigned f = 0; f < a->count; ++f) frame->stack[f].num = lane_of(&a->cells[f], j);
            frame->sp = a->count;
            int how = pure_run_step(frame, a->pc);
            if (how != STEP_NEXT) { /* its fault at its round: the event's undone, so only how and the work so far tell */
                app->work += (uint64_t)r * per;
                return how;
            }
            again |= pure_list_form(app->lists[b->m->code[a->pc].arg]) != forms[i];
        }
    }
    return STEP_NEXT;
}

static int batch_run(Frame *frame, Loop *loop, unsigned *done);

/* A batch (batch_run), the filters' rounds and keys let go after it (held only while one runs: a thread keeps none). */
int pure_batch_run(Frame *frame, Loop *loop, unsigned *done) {
    int how = batch_run(frame, loop, done);
    if (added_room || scratch_room || added_bits_room) {
        free(added_keys), free(scratch), free(added_bits);
        added_keys = NULL, scratch = NULL, added_bits = NULL, added_room = scratch_room = added_bits_room = 0;
    }
    for (unsigned f = 0; f < FILTERS; ++f)
        if (filtered_room[f]) free(filtered[f]), filtered[f] = NULL, filtered_room[f] = 0;
    return how;
}

static int batch_run(Frame *frame, Loop *loop, unsigned *done) {
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
    bool guard = !walking && m->code[loop->start - 1].plan & PLAN_GUARD;
    if (guard) { /* if slot >= L.count { break }: the rounds below L's count as it is now (only growing meanwhile) */
        double room = (double)pure_list_count(app->lists[m->code[loop->start + 1].arg]) - first;
        if (room < LEAST) { /* none (the executor's round breaks) or few: the executor's rounds, then batches again */
            loop->resume = round + (room >= 1 ? (uint64_t)room : 1);
            return STEP_NEXT;
        }
        if (room < n) n = (unsigned)room;
    }
    if (n < 2 || (first == 0 && signbit(first))) { /* the work left pays for one round, or the loop counts from -0: one round by the executor */
        loop->resume = round + 1;
        return STEP_NEXT;
    }
    const PureBatchPlan *plan = plan_of(m, loop->start - 1);
    if (!plan) { /* none made: its rounds the executor's */
        loop->resume = UINT64_MAX;
        return STEP_NEXT;
    }
    Batch *b = &batch;
    b->frame = frame, b->m = m, b->app = app, b->ev = frame->ev, b->n = n, b->first = first;
    b->reverse = loop->reverse, b->backward = 0;
    b->var_start = b->reverse ? first + (n - 1) : first, b->var_step = b->reverse ? -1 : 1;
    b->slot = walking ? LOCALS : (uint32_t)(loop->slot - frame->ev->locals), b->end = loop->end;
    b->walk = walking ? (int)loop->walk.list : -1;
    b->failed = b->none = false, b->active = NULL, b->nest = (Nest){0}, b->work = 0, b->var = NULL;
    b->plan = plan, b->sets = plan->sets, b->items = plan->items;
    b->sp = b->split_count = b->buffers = b->masks = b->read_count = b->store_count = b->spare_count = 0;
    b->mask_free_count = b->reduce_count = b->append_count = b->filter_count = b->known_count = b->cells_count = 0;
    b->sel = NULL;
    b->scatter_count = 0;
    for (unsigned w = 0; w < LOCALS / 64; ++w) /* the locals it sets: not set yet */
        for (uint64_t bits = plan->sets[w]; bits; bits &= bits - 1) b->state[w * 64 + (unsigned)__builtin_ctzll(bits)] = 0;
    if (plan->failed || !recurrences(b) || !recur(b))
        b->failed = true; /* more sums than a batch holds; the recurrences ahead, round by round */
    if (!b->failed && pass(b, guard ? loop->start + 5 : loop->start, loop->end)) {
        arrive(b, loop->end);
        if (b->filter_count && !keep_filtered(b)) b->failed = true; /* the filters' rounds in the batch's own lanes */
        for (unsigned w = 0; w < LOCALS / 64; ++w) /* the lanes each local was set in, made before anything changes */
            for (uint64_t bits = b->sets[w]; bits; bits &= bits - 1) {
                unsigned at = w * 64 + (unsigned)__builtin_ctzll(bits);
                if (b->state[at] == 2) mask_lanes(b, b->written[at]);
            }
        for (unsigned a = 0; a < b->append_count; ++a) mask_lanes(b, b->appends[a].mask);
        for (unsigned i = 0; i < b->scatter_count; ++i) mask_lanes(b, b->scatters[i].mask);
    }
    uint64_t work = (n - 1) * per + b->work; /* the rounds after the first, and every round of their nested loops */
    if (b->failed || work > budget - app->work || (b->filter_count && !decide(b)) || !settle(b)) {
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
    if (b->append_count) {
        int how = append_items(b, frame, n, per);
        if (how != STEP_NEXT) return how;
    }
    if (b->filter_count) {
        int how = apply_filters(b, frame, per);
        if (how != STEP_NEXT) return how;
    }
    app->work += work;
    for (unsigned r = 0; r < b->reduce_count; ++r) frame->ev->locals[b->reduce[r].local].num = b->reduce[r].result;
    for (unsigned x = 0; plan->recur && x < plan->recur->recur_count; ++x) frame->ev->locals[plan->recur->recur_local[x]].num = b->recur_value[x];
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
