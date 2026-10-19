/* Research 045: UAX #14's whole app emitted by hand (app-way.h), a rung at a time, each dropping one more thing the
 * runtime holds that a fact of the proof could let go. Cumulative:
 *
 *   exact         Pure's meaning and the runtime's forms: numbers doubles, each sum and difference checked finite, //
 *                 floor_divide and % Pure's (fmod); every index checked whole and in bounds (INDEX_AT) and again
 *                 (ITEM_GET); the event's lists PureLists in FLAT arrays as the runtime keeps them (021: columns of
 *                 the narrowest width that holds what they hold; cleared each line, appended with pure_list_insert and
 *                 the memory a change holds counted as list_held and grows count it), the points the message's list,
 *                 ranges the stored tree (pure_list_get); a Text's lists copied (pure_list_copied, LIST_VALUE) and let
 *                 go at the line's end; the work budget charged each round and call. Locals in registers, plain calls.
 *   integer       + whole numbers as integers (positions, counts, ranges, start, nonspace, ri, at): one bounds check.
 *   stored_flat   + ranges' columns flat (unchanged in the event): copied out of the tree first. Kept to compare: it is
 *                 the copy feedback 009 forbids, and the rungs after it do not use it.
 *   leaves        integer, + ranges read where its tree keeps it: a leaf's column at the item's place, no copy (the
 *                 leaf found by the list's own pure_list_run when the item is not in the one kept).
 *   leaves_direct integer, + the same, the tree read as its layout is (the descent by the guides' counts, no run).
 *   local_flat    leaves_direct, + the event's lists, the points and out as flat buffers (a memory charge an append kept).
 *   shared        + a Text's lists the buffers themselves (no copy).
 *   unbounded     - the bounds checks (the budget and memory still charged).
 *   plain         - every charge too: the same algorithm in C.
 *   vega          Vega's C (text/linebreak.c; its classes from a two-stage table, no halving).
 *
 * Every rung's boundaries are compared first with the test's for all lines (and exactly with exact's); built with
 * -DBREAK_ONE_RULE (LB18 made wrong) every rung must be refused. Then machine instructions and cycles a code point
 * (perf_event_open, user mode), ROUNDS passes a rung, the rungs interleaved.
 *
 *   cc -O2 -std=gnu17 -I../../../core -I../../../../vega/text app.c ../../../../vega/text/linebreak.c \
 *      ../../../../vega/text/unicode-data.c ../../../build/libpure.a -lm -pthread -o app
 * Then a message end to end (BORDER below): the runtime's pure_app_message (the executor; an empty handler, the event
 * alone), the border written by hand around shared and plain, and Vega's C given the same payload.
 *
 *   ./app linebreak.pmod RANGES LINES ROUNDS v0-empty.pmod     one JSON line a rung, then one an end */
#define _GNU_SOURCE
#include "module.h"
#include "module-list.h"
#include "text.h"
/* The list's own source, so a rung can read its tree as an emitter that knows the tree's layout would (leaves_direct);
 * built with this file, it stands in for libpure.a's module-list.o, which the link then does not take. */
#include "../../../core/module-list.c"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>

enum {
    BK = 0, CR = 1, LF = 2, NL = 3, SP = 4, ZW = 5, ZWJ = 6, CM = 7, WJ = 8, GL = 9, BA = 10, BB = 11, B2 = 12, HY = 13, CB = 14,
    CL = 15, CP = 16, EX = 17, IN = 18, NS = 19, OP = 20, QU = 21, IS = 22, NU = 23, PO = 24, PR = 25, SY = 26, AK = 27, AL = 28,
    AP = 29, AS = 30, EB = 31, EM = 32, H2 = 33, H3 = 34, HL = 35, ID = 36, JL = 37, JV = 38, JT = 39, RI = 40, VF = 41, VI = 42
};
enum { NONE = 0, ALLOWED = 1, MANDATORY = 2 };
enum { ROUND_COST = 24, CALL_COST = 8 }; /* what a round and a call charge (constants: what is charged, not how much) */
enum { START = 0, CLASS = 1, PI_FIELD = 2, PF_FIELD = 3, EAST_FIELD = 4, PICT_FIELD = 5 }; /* Range's fields */

typedef struct { double *v; size_t n, cap; } BufN;
typedef struct { int64_t *v; size_t n, cap; } BufI;
typedef struct { unsigned char *v; size_t n, cap; } BufB;
typedef struct { size_t stored, kept; } Held;
typedef PureList *ListP; /* a list's handle as one name, so a declaration of several declares each a handle */
typedef BufN *BufNP;
typedef BufI *BufIP;
typedef BufB *BufBP;

typedef struct {
    bool fault;
    int fault_line; /* where the last fault was (a macro's use) */
    uint64_t work, budget;
    int64_t grown, spent, room; /* the memory the event's changes hold (module.c's grows) */
    PureUndo undo;              /* empty: the event's lists keep none (undo_for), list_held still asks its size */
    const PureList *ranges;
    size_t ranges_count;
    const double *r_start, *r_class;
    const unsigned char *r_pi, *r_pf, *r_east, *r_pict;
    PureRun run; /* the leaf of ranges last read, its columns where the tree keeps them (pure_list_run): begun each message */
    const Node *leaf; /* leaves_direct's: the leaf of ranges last read, its first item's place and its items */
    size_t leaf_start, leaf_count;
    PureList *codes, *cls, *eff, *start, *nonspace, *ri, *at, *joined, *numeric, *out; /* the event's lists */
    PureList *copies[9];
    unsigned copied;
    BufN b_codes, b_cls, b_eff, k_codes, k_cls, k_eff; /* the event's lists as buffers, and a Text's copies of them */
    BufI b_start, b_nonspace, b_ri, b_at, b_out, k_start, k_nonspace, k_ri, k_at;
    BufB b_joined, b_numeric, k_joined, k_numeric;
} Context;
typedef Context CONTEXT;

typedef struct { const PureList *list; const double *codes; size_t count; } Points;
typedef Points POINTS;

/* core/module.c's, as they are. */
static double floor_divide(double a, double b) {
    double mod = fmod(a, b), quotient = (a - mod) / b;
    if (mod != 0 && (b < 0) != (mod < 0)) quotient -= 1.0;
    if (quotient == 0) return copysign(0.0, a / b);
    double whole = floor(quotient);
    return quotient - whole > 0.5 ? whole + 1.0 : whole;
}
static double pure_mod(double a, double b) {
    double value = fmod(a, b);
    if (value != 0 && (value < 0) != (b < 0)) value += b;
    else if (value == 0) value = copysign(0.0, b);
    return value;
}
static inline Held held(const Context *x, const PureList *l) {
    return (Held){pure_list_size(l), pure_list_changes(l).size + pure_undo_size(&x->undo) + pure_list_extra(l)};
}
static inline bool grows(Context *x, Held before, Held after) {
    x->grown += (int64_t)after.stored - (int64_t)before.stored;
    x->spent += (int64_t)(after.stored + after.kept) - (int64_t)(before.stored + before.kept);
    return x->spent <= x->room;
}
static __attribute__((noinline)) bool grow(void **v, size_t *cap, size_t size) {
    size_t more = *cap ? *cap * 2 : 64;
    void *p = realloc(*v, more * size);
    if (!p) return false;
    *v = p, *cap = more;
    return true;
}

#define CHARGES_ON(n) do { if ((uint64_t)(n) > x->budget - x->work) FAIL; x->work += (uint64_t)(n); } while (0)

/* ======== exact ======== */
#define WAY exact
#define IDX double
#define ADD(a, b) ({ double v_ = (double)(a) + (double)(b); if (!isfinite(v_)) FAIL; v_; })
#define SUB(a, b) ({ double v_ = (double)(a) - (double)(b); if (!isfinite(v_)) FAIL; v_; })
#define GT(a, b) ((a) > (b))
#define DIV2(a) ({ double q_ = floor_divide((a), 2); if (!isfinite(q_)) FAIL; q_; })
#define MOD2(a) ({ double m_ = pure_mod((a), 2); if (!isfinite(m_)) FAIL; m_; })
#define CHECK_AT(count, i) do { size_t c_ = (count); double i_ = (i); /* INDEX_AT, then ITEM_GET */ \
        if (i_ != floor(i_) || i_ < 0 || i_ >= (double)c_) FAIL; if ((size_t)i_ >= c_) FAIL; } while (0)
#define CHARGE(n) CHARGES_ON(n)
#define LIST_N ListP
#define LIST_I ListP
#define LIST_B ListP
#define L(name) (x->name)
#define NCOUNT(l) ((IDX)pure_list_count(l))
#define GET_CELL(l, i) ({ const PureList *l_ = (l); IDX at_ = (i); CHECK_AT(pure_list_count(l_), at_); pure_list_get(l_, (size_t)at_, 0); })
#define GETN(l, i) (GET_CELL(l, i).num)
#define GETI(l, i) ((IDX)GET_CELL(l, i).num)
#define GETB(l, i) (GET_CELL(l, i).num != 0)
#define APP_LIST(l, cell) do { PureList *l_ = (l); Held b_ = held(x, l_); PureCell c_ = cell; \
        if (pure_list_insert(l_, pure_list_count(l_), &c_, NULL) != PURE_OK) FAIL; if (!grows(x, b_, held(x, l_))) FAIL; } while (0)
#define APPN(l, v) APP_LIST(l, ((PureCell){.num = (v)}))
#define APPI(l, v) APP_LIST(l, ((PureCell){.num = (double)(v)}))
#define APPB(l, v) APP_LIST(l, ((PureCell){.num = (v) ? 1.0 : 0.0}))
#define R_COUNT ((IDX)pure_list_count(x->ranges))
#define R_CELL(f, k) ({ IDX k_ = (k); CHECK_AT(pure_list_count(x->ranges), k_); pure_list_get(x->ranges, (size_t)k_, f); })
#define R_START(k) (R_CELL(START, k).num)
#define R_CLASS(k) (R_CELL(CLASS, k).num)
#define R_PI(k) (R_CELL(PI_FIELD, k).num != 0)
#define R_PF(k) (R_CELL(PF_FIELD, k).num != 0)
#define R_EAST(k) (R_CELL(EAST_FIELD, k).num != 0)
#define R_PICT(k) (R_CELL(PICT_FIELD, k).num != 0)
#define P_COUNT(p) ((IDX)pure_list_count((p)->list))
#define P_CODE(p, k) ({ IDX k_ = (k); CHECK_AT(pure_list_count((p)->list), k_); pure_list_get((p)->list, (size_t)k_, 0).num; })
#define CAPTURE(dst, name) do { PureList *s_ = L(name); /* LIST_VALUE: nest's copy, an empty list none */ \
        if (pure_list_count(s_)) { PureList *c_ = pure_list_copied(s_); if (!c_) FAIL; \
            if (!grows(x, (Held){0, 0}, (Held){0, pure_list_size(c_)})) FAIL; x->copies[x->copied++] = c_; s_ = c_; } \
        dst = s_; } while (0)
#define OUT_APPEND(v) APP_LIST(x->out, ((PureCell){.i64 = (int64_t)(v)}))
#define OUT_EMIT(got) do { size_t n_ = pure_list_count(x->out); for (size_t k_ = 0; k_ < n_; ++k_) { \
        int64_t v_ = pure_list_get(x->out, k_, 0).i64; if ((uint64_t)v_ > 255) FAIL; (got)[k_] = (uint8_t)v_; } } while (0)
#define LINE_BEGIN do { PureList *all_[] = {x->codes, x->cls, x->eff, x->start, x->nonspace, x->ri, x->at, x->joined, \
        x->numeric, x->out}; for (unsigned l_ = 0; l_ < 10; ++l_) pure_list_clear(all_[l_]); x->copied = 0; } while (0)
#define LINE_END do { for (unsigned c_ = 0; c_ < x->copied; ++c_) pure_list_free(x->copies[c_]); x->copied = 0; } while (0)
#include "app-way.h"
#undef WAY
#undef IDX
#undef ADD
#undef SUB
#undef DIV2
#undef MOD2
#undef CHECK_AT
#undef GETI

/* ======== integer: whole numbers as integers ======== */
#define WAY integer
#define IDX int64_t
#define ADD(a, b) ((a) + (b))
#define SUB(a, b) ((a) - (b))
#define DIV2(a) ((a) / 2)   /* whole and at or above 0 */
#define MOD2(a) ((a) % 2)   /* ri: whole and at or above 0 */
#define CHECK_AT(count, i) do { if ((uint64_t)(i) >= (uint64_t)(count)) FAIL; } while (0)
#define GETI(l, i) ((IDX)GET_CELL(l, i).num)
#include "app-way.h"
#undef WAY

/* ======== stored_flat: ranges' columns flat ======== */
#define WAY stored_flat
#undef R_COUNT
#undef R_START
#undef R_CLASS
#undef R_PI
#undef R_PF
#undef R_EAST
#undef R_PICT
#define R_COUNT ((IDX)x->ranges_count)
#define R_AT(column, k) ({ IDX k_ = (k); CHECK_AT(x->ranges_count, k_); (column)[k_]; })
#define R_START(k) R_AT(x->r_start, k)
#define R_CLASS(k) R_AT(x->r_class, k)
#define R_PI(k) (R_AT(x->r_pi, k) != 0)
#define R_PF(k) (R_AT(x->r_pf, k) != 0)
#define R_EAST(k) (R_AT(x->r_east, k) != 0)
#define R_PICT(k) (R_AT(x->r_pict, k) != 0)
#include "app-way.h"
#undef WAY

/* ======== leaves: ranges read where its tree keeps it, no copy (feedback 009) ========
 * An item's field is its leaf's column at its place in the leaf: the leaf last read is kept (a run), and a position
 * outside it finds its own (pure_list_run: the descent, the columns' addresses; nothing copied). Valid while the list
 * does not change: within the event (N2). Begun anew each message. */
#define WAY leaves
#undef R_COUNT
#undef R_AT
#define R_COUNT ((IDX)x->ranges_count)
#define R_LEAF(f, k) ({ IDX k_ = (k); CHECK_AT(x->ranges_count, k_); size_t at_ = (size_t)k_; \
        if (at_ - x->run.start >= x->run.count && !pure_list_run(x->ranges, at_, &x->run)) FAIL; \
        x->run.column[f].numbers[at_ - x->run.start]; })
#undef R_START
#undef R_CLASS
#undef R_PI
#undef R_PF
#undef R_EAST
#undef R_PICT
#define R_START(k) R_LEAF(START, k)
#define R_CLASS(k) R_LEAF(CLASS, k)
#define R_PI(k) (R_LEAF(PI_FIELD, k) != 0)
#define R_PF(k) (R_LEAF(PF_FIELD, k) != 0)
#define R_EAST(k) (R_LEAF(EAST_FIELD, k) != 0)
#define R_PICT(k) (R_LEAF(PICT_FIELD, k) != 0)
#undef LINE_BEGIN
#define LINE_BEGIN do { PureList *all_[] = {x->codes, x->cls, x->eff, x->start, x->nonspace, x->ri, x->at, x->joined, \
        x->numeric, x->out}; for (unsigned l_ = 0; l_ < 10; ++l_) pure_list_clear(all_[l_]); x->copied = 0; \
        x->run.count = 0; } while (0)
#include "app-way.h"
#undef WAY

/* ======== leaves_direct: ranges' tree read as its layout is (no pure_list_run): the descent by the guides' counts
 * (child_at), the leaf's column at the item's place, the last leaf kept ======== */
#define WAY leaves_direct
static inline __attribute__((always_inline)) double leaf_number(Context *x, size_t at, unsigned f) {
    if (at - x->leaf_start >= x->leaf_count) {
        const Node *node = x->ranges->root;
        size_t start = 0;
        while (!node->leaf) {
            unsigned i = child_at(node, at - start);
            start += guide(node)->before[i];
            node = node->child[i];
        }
        x->leaf = node, x->leaf_start = start, x->leaf_count = node->count;
    }
    return numbers(&x->ranges->layout, x->leaf, f)[at - x->leaf_start];
}
#undef R_LEAF
#define R_LEAF(f, k) ({ IDX k_ = (k); CHECK_AT(x->ranges_count, k_); leaf_number(x, (size_t)k_, f); })
#undef LINE_BEGIN
#define LINE_BEGIN do { PureList *all_[] = {x->codes, x->cls, x->eff, x->start, x->nonspace, x->ri, x->at, x->joined, \
        x->numeric, x->out}; for (unsigned l_ = 0; l_ < 10; ++l_) pure_list_clear(all_[l_]); x->copied = 0; \
        x->leaf_count = 0; } while (0)
#include "app-way.h"
#undef WAY

/* ======== leaves_seek: leaves_direct, and range() done by the tree's own order: each inner node's children chosen by
 * their first items' starts (the leaf each names, read where it is), then halving within the leaf. Another algorithm
 * than the app's (positions halved): it needs ranges ordered by start, which the data is and a list declared keyed by
 * start would be (its seek, pure_list_seek's); the budget charged for the call alone ======== */
#define WAY leaves_seek
static inline __attribute__((always_inline)) size_t seek_start(Context *x, double c) {
    const Layout *l = &x->ranges->layout;
    const Node *node = x->ranges->root;
    size_t start = 0;
    while (!node->leaf) {
        unsigned low = 0, high = node->count;
        while (high - low > 1) {
            unsigned middle = (low + high) / 2;
            if (numbers(l, node->child[middle]->first, START)[0] <= c) low = middle;
            else high = middle;
        }
        start += guide(node)->before[low];
        node = node->child[low];
    }
    const double *starts = numbers(l, node, START);
    unsigned low = 0, high = node->count;
    while (high - low > 1) {
        unsigned middle = (low + high) / 2;
        if (starts[middle] <= c) low = middle;
        else high = middle;
    }
    x->leaf = node, x->leaf_start = start, x->leaf_count = node->count; /* the class read next is in it */
    return start + low;
}
#define RANGE_SEEK(c) seek_start(x, (c))
#include "app-way.h"
#undef WAY
#undef RANGE_SEEK

/* ======== local_flat: the event's lists, the points and out as buffers ======== */
#define WAY local_flat
#undef LIST_N
#undef LIST_I
#undef LIST_B
#undef L
#undef NCOUNT
#undef GETN
#undef GETI
#undef GETB
#undef APPN
#undef APPI
#undef APPB
#undef P_COUNT
#undef P_CODE
#undef CAPTURE
#undef OUT_APPEND
#undef OUT_EMIT
#undef LINE_BEGIN
#undef LINE_END
#define LIST_N BufNP
#define LIST_I BufIP
#define LIST_B BufBP
#define L(name) (&x->b_##name)
#define NCOUNT(l) ((IDX)(l)->n)
#define GET_BUF(l, i) ({ __typeof__(l) l_ = (l); IDX at_ = (i); CHECK_AT(l_->n, at_); l_->v[at_]; })
#define GETN(l, i) GET_BUF(l, i)
#define GETI(l, i) GET_BUF(l, i)
#define GETB(l, i) (GET_BUF(l, i) != 0)
#define MEMORY(bytes) do { if ((x->spent += (bytes)) > x->room) FAIL; } while (0)
#define APP_BUF(l, value) do { __typeof__(l) l_ = (l); \
        if (l_->n == l_->cap && !grow((void **)&l_->v, &l_->cap, sizeof(*l_->v))) FAIL; \
        MEMORY(sizeof(*l_->v)); l_->v[l_->n++] = (value); } while (0)
#define APPN(l, v) APP_BUF(l, (v))
#define APPI(l, v) APP_BUF(l, (v))
#define APPB(l, v) APP_BUF(l, (unsigned char)((v) ? 1 : 0))
#define P_COUNT(p) ((IDX)(p)->count)
#define P_CODE(p, k) ({ IDX k_ = (k); CHECK_AT((p)->count, k_); (p)->codes[k_]; })
#define CAPTURE(dst, name) do { __typeof__(L(name)) s_ = L(name), c_ = &x->k_##name; /* LIST_VALUE: the items copied */ \
        while (c_->cap < s_->n) if (!grow((void **)&c_->v, &c_->cap, sizeof(*c_->v))) FAIL; \
        MEMORY(s_->n * sizeof(*s_->v)); memcpy(c_->v, s_->v, s_->n * sizeof(*s_->v)); c_->n = s_->n; dst = c_; } while (0)
#define OUT_APPEND(v) APP_BUF(&x->b_out, (int64_t)(v))
#define OUT_EMIT(got) do { for (size_t k_ = 0; k_ < x->b_out.n; ++k_) { int64_t v_ = x->b_out.v[k_]; \
        if ((uint64_t)v_ > 255) FAIL; (got)[k_] = (uint8_t)v_; } } while (0)
#define LINE_BEGIN do { x->b_codes.n = x->b_cls.n = x->b_eff.n = x->b_start.n = x->b_nonspace.n = x->b_ri.n = x->b_at.n = \
        x->b_joined.n = x->b_numeric.n = x->b_out.n = 0; x->leaf_count = 0; } while (0)
#define LINE_END do { } while (0)
#include "app-way.h"
#undef WAY
#define WAY local_flat_seek /* the same, range() by the tree's order (leaves_seek) */
#define RANGE_SEEK(c) seek_start(x, (c))
#include "app-way.h"
#undef WAY
#undef RANGE_SEEK

/* ======== shared: a Text's lists the buffers ======== */
#define WAY shared
#undef CAPTURE
#define CAPTURE(dst, name) ((dst) = L(name))
#include "app-way.h"
#undef WAY
#define WAY shared_seek /* the same, range() by the tree's order (leaves_seek) */
#define RANGE_SEEK(c) seek_start(x, (c))
#include "app-way.h"
#undef WAY
#undef RANGE_SEEK

/* ======== unbounded: shared, no bounds checks (the budget and memory charged) ======== */
#define WAY unbounded
#undef CHECK_AT
#define CHECK_AT(count, i) do { (void)(count); } while (0)
#include "app-way.h"
#undef WAY
#define WAY unbounded_seek /* the same, range() by the tree's order (leaves_seek) */
#define RANGE_SEEK(c) seek_start(x, (c))
#include "app-way.h"
#undef WAY
#undef RANGE_SEEK

/* ======== plain: no check, no charge ======== */
#define WAY plain
#undef CHARGE
#undef MEMORY
#define CHARGE(n) do { (void)x; } while (0)
#define MEMORY(bytes) do { } while (0)
#undef OUT_EMIT
#define OUT_EMIT(got) do { for (size_t k_ = 0; k_ < x->b_out.n; ++k_) (got)[k_] = (uint8_t)x->b_out.v[k_]; } while (0)
#include "app-way.h"
#undef WAY

static bool on_line_vega(Context *x, const Points *p, const uint32_t *codes, uint8_t *got) {
    (void)x, (void)p;
    return vega_line_breaks(codes, p->count, got);
}

/* ---- A message end to end: the event's border written by hand around a rung's handler. What it keeps of the
 * runtime's meaning: the payload's rows whole and each number finite (place_fields, decode_cell); the work budget
 * begun again and charged the handler's length (dispatch); the memory counts begun again; the one state the handler
 * changes (breaks, its change set: N2) written apart and made the state's only when the handler ends without a fault
 * (the pending copy and undo, for that state alone), into the buffer readers do not hold (a snapshot holding it: a new
 * one); the events and changes counted. The rows are read where the payload has them (the message cannot change them),
 * and breaks is a buffer (the module only ever sets it whole). Without a store (as the runtime here): no save. */
typedef struct { uint8_t *buffer[2]; size_t room[2], size; unsigned now; bool held[2]; uint64_t events, changes; } BytesState;
enum { HANDLER_LENGTH = 180 }; /* what the handler charges as it begins (its instructions as written: a constant) */

#define BORDER(WAYNAME, CHECKED)                                                                                       \
    static bool message_##WAYNAME(Context *x, BytesState *breaks, const unsigned char *payload, size_t size) {        \
        if (CHECKED && size % 8) return false; /* rows not whole: LIMIT */                                             \
        size_t n = size / 8;                                                                                           \
        const double *codes = (const double *)payload;                                                                 \
        if (CHECKED)                                                                                                   \
            for (size_t i = 0; i < n; ++i)                                                                             \
                if (!isfinite(codes[i])) return false; /* a number that does not fit */                                \
        x->fault = false, x->work = 0, x->spent = x->grown = 0;                                                        \
        if (CHECKED && HANDLER_LENGTH > x->budget) return false;                                                       \
        x->work = HANDLER_LENGTH;                                                                                      \
        unsigned next = breaks->now ^ 1;                                                                               \
        if (breaks->held[next] || breaks->room[next] < n + 1) { /* a snapshot reads it, or it is too small: another */ \
            size_t room = n + 1 > 2 * breaks->room[next] ? n + 1 : 2 * breaks->room[next];                             \
            uint8_t *b = malloc(room);                                                                                 \
            if (!b) return false;                                                                                      \
            if (!breaks->held[next]) free(breaks->buffer[next]);                                                       \
            breaks->buffer[next] = b, breaks->room[next] = room, breaks->held[next] = false;                           \
        }                                                                                                              \
        Points p = {NULL, codes, n};                                                                                   \
        if (!on_line_##WAYNAME(x, &p, breaks->buffer[next]) || x->fault) return false; /* nothing changed */          \
        breaks->now = next, breaks->size = n + 1;                                                                      \
        ++breaks->events, ++breaks->changes;                                                                           \
        return true;                                                                                                   \
    }
BORDER(shared, true)
BORDER(plain, false)
BORDER(shared_seek, true)

/* The same message to Vega's C: its numbers as code points, the boundaries into the state's buffer. */
static bool message_vega(BytesState *breaks, const unsigned char *payload, size_t size, uint32_t *scratch) {
    size_t n = size / 8;
    const double *codes = (const double *)payload;
    for (size_t i = 0; i < n; ++i) scratch[i] = (uint32_t)codes[i];
    unsigned next = breaks->now ^ 1;
    if (breaks->room[next] < n + 1) {
        free(breaks->buffer[next]);
        if (!(breaks->buffer[next] = malloc(n + 1))) return false;
        breaks->room[next] = n + 1;
    }
    if (!vega_line_breaks(scratch, n, breaks->buffer[next])) return false;
    breaks->now = next, breaks->size = n + 1;
    ++breaks->events, ++breaks->changes;
    return true;
}

/* Pure's own boundaries, from its view: compared with the test's. */
typedef struct { const unsigned char *want; size_t at; bool same; } Compare;
static bool compare(void *context, PureSpan span) {
    Compare *c = context;
    for (size_t i = 0; i < span.size; ++i, ++c->at) c->same = c->same && (span.data[i] != 0) == c->want[c->at];
    return true;
}

/* ---- the harness ---- */
static unsigned char *whole(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    *size = (size_t)ftell(f);
    rewind(f);
    unsigned char *bytes = malloc(*size ? *size : 1);
    if (fread(bytes, 1, *size, f) != *size) { free(bytes); bytes = NULL; }
    fclose(f);
    return bytes;
}

static int counter(uint64_t config) {
    struct perf_event_attr a;
    memset(&a, 0, sizeof(a));
    a.size = sizeof(a), a.type = PERF_TYPE_HARDWARE, a.config = config, a.disabled = 1, a.exclude_kernel = 1, a.exclude_hv = 1;
    return (int)syscall(SYS_perf_event_open, &a, 0, -1, -1, 0);
}

static uint64_t read_counter(int fd) {
    uint64_t v = 0;
    if (read(fd, &v, sizeof(v)) != sizeof(v)) return 0;
    return v;
}

typedef struct { Points points; const uint32_t *codes; const unsigned char *want; } Line;
enum { RUNGS = 14 };
static const char *const names[RUNGS] = {"exact", "integer", "stored_flat", "leaves", "leaves_direct", "local_flat", "shared",
                                         "unbounded", "plain", "leaves_seek", "local_flat_seek", "shared_seek",
                                         "unbounded_seek", "vega"};

static bool run(int rung, Context *x, const Line *l, uint8_t *got) {
    switch (rung) {
    case 0: return on_line_exact(x, &l->points, got);
    case 1: return on_line_integer(x, &l->points, got);
    case 2: return on_line_stored_flat(x, &l->points, got);
    case 3: return on_line_leaves(x, &l->points, got);
    case 4: return on_line_leaves_direct(x, &l->points, got);
    case 5: return on_line_local_flat(x, &l->points, got);
    case 6: return on_line_shared(x, &l->points, got);
    case 7: return on_line_unbounded(x, &l->points, got);
    case 8: return on_line_plain(x, &l->points, got);
    case 9: return on_line_leaves_seek(x, &l->points, got);
    case 10: return on_line_local_flat_seek(x, &l->points, got);
    case 11: return on_line_shared_seek(x, &l->points, got);
    case 12: return on_line_unbounded_seek(x, &l->points, got);
    default: return on_line_vega(x, &l->points, l->codes, got);
    }
}

int main(int argc, char **argv) {
    if (argc != 6) { fprintf(stderr, "usage: %s linebreak.pmod RANGES LINES ROUNDS v0-empty.pmod\n", argv[0]); return 2; }
    size_t ranges_size, lines_size;
    unsigned char *ranges = whole(argv[2], &ranges_size), *bytes = whole(argv[3], &lines_size);
    int rounds = atoi(argv[4]);
    if (!ranges || !bytes) return 1;
    PureModule module = {0};
    char error[160];
    if (pure_module_read(argv[1], &module, error, sizeof(error)) != PURE_OK) { fprintf(stderr, "%s\n", error); return 1; }
    PureApp app;
    if (pure_app_start(&app, &module, (PureData){0}) != PURE_OK) return 1;
    int load = pure_module_input(&module, "load"), state = pure_module_state(&module, "ranges");
    if (load < 0 || state < 0 || pure_app_message(&app, (uint32_t)load, (PureSpan){ranges, ranges_size}) != PURE_OK) return 1;
    static Context x;
    x.budget = UINT64_MAX, x.room = INT64_MAX;
    x.ranges = app.lists[state], x.ranges_count = pure_list_count(x.ranges);
    double *r_start = malloc(x.ranges_count * sizeof(double)), *r_class = malloc(x.ranges_count * sizeof(double));
    unsigned char *flags = malloc(4 * x.ranges_count);
    for (size_t i = 0; i < x.ranges_count; ++i) {
        r_start[i] = pure_list_get(x.ranges, i, START).num, r_class[i] = pure_list_get(x.ranges, i, CLASS).num;
        for (unsigned f = 0; f < 4; ++f) flags[f * x.ranges_count + i] = pure_list_get(x.ranges, i, PI_FIELD + f).num != 0;
    }
    x.r_start = r_start, x.r_class = r_class;
    x.r_pi = flags, x.r_pf = flags + x.ranges_count, x.r_east = flags + 2 * x.ranges_count, x.r_pict = flags + 3 * x.ranges_count;
    const uint8_t number[] = {PURE_CELL_NUMBER}, boolean[] = {PURE_CELL_BOOL}, integer[] = {PURE_CELL_INTEGER};
    PureList **numbers[] = {&x.codes, &x.cls, &x.eff, &x.start, &x.nonspace, &x.ri, &x.at};
    for (unsigned k = 0; k < 7; ++k) *numbers[k] = pure_list_new(1, number);
    x.joined = pure_list_new(1, boolean), x.numeric = pure_list_new(1, boolean), x.out = pure_list_new(1, integer);
    PureList *events[] = {x.codes, x.cls, x.eff, x.start, x.nonspace, x.ri, x.at, x.joined, x.numeric, x.out};
    for (unsigned k = 0; k < 10; ++k) /* an event's list in FLAT arrays, as the runtime starts one (021) */
        if (!pure_list_arrays(events[k])) return 1;
    /* The lines: each its points as the message's list (the event's form) and as the message's numbers. */
    static Line lines[1 << 16];
    size_t count = 0, points = 0;
    for (size_t at = 0; at + 4 <= lines_size && count < sizeof(lines) / sizeof(*lines); ++count) {
        Line *l = &lines[count];
        uint32_t n;
        memcpy(&n, bytes + at, 4);
        l->codes = (const uint32_t *)(bytes + at + 4);
        l->want = bytes + at + 4 + 4 * (size_t)n;
        at += 4 + 4 * (size_t)n + n + 1;
        double *codes = malloc(((size_t)n + 1) * sizeof(double));
        PureList *list = pure_list_new(1, number);
        if (!pure_list_arrays(list)) return 1;
        for (uint32_t i = 0; i < n; ++i) {
            codes[i] = l->codes[i];
            PureCell cell = {.num = codes[i]};
            if (pure_list_insert(list, i, &cell, NULL) != PURE_OK) return 1;
        }
        l->points = (Points){list, codes, n};
        points += n;
    }
    /* Checked first: each rung's boundaries are the test's, and exactly exact's. */
    static uint8_t got[4096], first[4096];
    size_t wrong[RUNGS] = {0};
    for (size_t k = 0; k < count; ++k) {
        const Line *l = &lines[k];
        if (l->points.count + 1 > sizeof(got)) return 1;
        for (int r = 0; r < RUNGS; ++r) {
            x.fault = false;
            bool same = run(r, &x, l, got) && !x.fault;
            for (size_t i = 0; same && i <= l->points.count; ++i) same = (got[i] != NONE) == l->want[i];
            if (r == 0) memcpy(first, got, l->points.count + 1);
            else if (r < RUNGS - 1) same = same && !memcmp(first, got, l->points.count + 1);
            if (!same && getenv("APP_DEBUG") && !wrong[r]) {
                fprintf(stderr, "%s line %zu fault %d at %d:", names[r], k, x.fault, x.fault_line);
                for (size_t i = 0; i <= l->points.count; ++i) fprintf(stderr, " %u/%u", got[i], l->want[i]);
                fprintf(stderr, "\n");
            }
            wrong[r] += !same;
        }
    }
    bool refused = false;
    for (int r = 0; r < RUNGS; ++r)
        if (wrong[r]) fprintf(stderr, "%s: %zu of %zu lines wrong\n", names[r], wrong[r], count), refused = true;
#ifndef BREAK_ONE_RULE /* broken, on to the ends: they must be refused too */
    if (refused) return 1;
#endif
    int ins = counter(PERF_COUNT_HW_INSTRUCTIONS), cyc = counter(PERF_COUNT_HW_CPU_CYCLES);
    if (ins < 0 || cyc < 0) { perror("perf_event_open"); return 1; }
    uint64_t total_ins[RUNGS] = {0}, total_cyc[RUNGS] = {0};
    for (int round = 0; round < rounds; ++round)
        for (int r = 0; r < RUNGS; ++r) {
            x.work = 0, x.spent = 0, x.grown = 0;
            ioctl(ins, PERF_EVENT_IOC_RESET, 0), ioctl(cyc, PERF_EVENT_IOC_RESET, 0);
            ioctl(ins, PERF_EVENT_IOC_ENABLE, 0), ioctl(cyc, PERF_EVENT_IOC_ENABLE, 0);
            for (size_t k = 0; k < count; ++k) run(r, &x, &lines[k], got);
            ioctl(ins, PERF_EVENT_IOC_DISABLE, 0), ioctl(cyc, PERF_EVENT_IOC_DISABLE, 0);
            total_ins[r] += read_counter(ins), total_cyc[r] += read_counter(cyc);
            x.spent = 0;
        }
    double n = (double)points * rounds;
    for (int r = 0; r < RUNGS; ++r)
        printf("{\"rung\":\"%s\",\"lines\":%zu,\"code_points\":%zu,\"rounds\":%d,\"instructions_per_point\":%.1f,"
               "\"cycles_per_point\":%.1f,\"answers\":\"the test's, and exact's\"}\n",
               names[r], count, points, rounds, total_ins[r] / n, total_cyc[r] / n);
    /* A message end to end: the runtime (the executor, and an empty handler: the event alone), the border written by
     * hand around shared and plain, and Vega's C on the same payload. */
    PureModule empty_module = {0};
    PureApp empty;
    if (pure_module_read(argv[5], &empty_module, error, sizeof(error)) != PURE_OK) { fprintf(stderr, "%s\n", error); return 1; }
    if (pure_app_start(&empty, &empty_module, (PureData){0}) != PURE_OK ||
        pure_app_message(&empty, (uint32_t)pure_module_input(&empty_module, "load"), (PureSpan){ranges, ranges_size}) != PURE_OK)
        return 1;
    int line = pure_module_input(&module, "line"), empty_line = pure_module_input(&empty_module, "line");
    int boundaries = pure_module_view(&module, "boundaries");
    if (line < 0 || empty_line < 0 || boundaries < 0) return 1;
    unsigned char **payloads = malloc(count * sizeof(*payloads));
    for (size_t k = 0; k < count; ++k) {
        size_t n = lines[k].points.count;
        payloads[k] = malloc(8 * n + 8);
        memcpy(payloads[k], lines[k].points.codes, 8 * n); /* the message: each point's code, a number (8 bytes) */
    }
    static BytesState breaks[4];
    static uint32_t scratch[4096];
    enum { ENDS = 6 };
    const char *ends[ENDS] = {"runtime", "runtime_empty_handler", "end_to_end_shared", "end_to_end_plain", "end_to_end_vega",
                              "end_to_end_shared_seek"};
    size_t wrong_ends[ENDS] = {0};
    for (size_t k = 0; k < count; ++k) { /* checked first: each end's boundaries the test's */
        const Line *l = &lines[k];
        size_t size = 8 * l->points.count;
        Compare c = {l->want, 0, true};
        c.same = pure_app_message(&app, (uint32_t)line, (PureSpan){payloads[k], size}) == PURE_OK &&
                 pure_app_view_text(&app, (uint32_t)boundaries, compare, &c) == PURE_OK && c.at == l->points.count + 1;
        wrong_ends[0] += !c.same;
        wrong_ends[1] += pure_app_message(&empty, (uint32_t)empty_line, (PureSpan){payloads[k], size}) != PURE_OK;
        for (int e = 2; e < ENDS; ++e) {
            BytesState *b = &breaks[e - 2];
            bool done = e == 2 ? message_shared(&x, b, payloads[k], size) : e == 3 ? message_plain(&x, b, payloads[k], size)
                        : e == 4 ? message_vega(b, payloads[k], size, scratch) : message_shared_seek(&x, b, payloads[k], size);
            bool same = done && b->size == l->points.count + 1;
            for (size_t i = 0; same && i < b->size; ++i) same = (b->buffer[b->now][i] != NONE) == l->want[i];
            wrong_ends[e] += !same;
        }
    }
    for (int e = 0; e < ENDS; ++e)
        if (wrong_ends[e]) fprintf(stderr, "%s: %zu of %zu lines wrong\n", ends[e], wrong_ends[e], count), refused = true;
    if (refused) return 1;
    uint64_t end_ins[ENDS] = {0}, end_cyc[ENDS] = {0};
    for (int round = 0; round < rounds; ++round)
        for (int e = 0; e < ENDS; ++e) {
            ioctl(ins, PERF_EVENT_IOC_RESET, 0), ioctl(cyc, PERF_EVENT_IOC_RESET, 0);
            ioctl(ins, PERF_EVENT_IOC_ENABLE, 0), ioctl(cyc, PERF_EVENT_IOC_ENABLE, 0);
            for (size_t k = 0; k < count; ++k) {
                size_t size = 8 * lines[k].points.count;
                switch (e) {
                case 0: pure_app_message(&app, (uint32_t)line, (PureSpan){payloads[k], size}); break;
                case 1: pure_app_message(&empty, (uint32_t)empty_line, (PureSpan){payloads[k], size}); break;
                case 2: message_shared(&x, &breaks[0], payloads[k], size); break;
                case 3: message_plain(&x, &breaks[1], payloads[k], size); break;
                case 4: message_vega(&breaks[2], payloads[k], size, scratch); break;
                default: message_shared_seek(&x, &breaks[3], payloads[k], size);
                }
            }
            ioctl(ins, PERF_EVENT_IOC_DISABLE, 0), ioctl(cyc, PERF_EVENT_IOC_DISABLE, 0);
            end_ins[e] += read_counter(ins), end_cyc[e] += read_counter(cyc);
        }
    for (int e = 0; e < ENDS; ++e)
        printf("{\"end\":\"%s\",\"lines\":%zu,\"code_points\":%zu,\"rounds\":%d,\"instructions_per_point\":%.1f,"
               "\"cycles_per_point\":%.1f,\"instructions_per_line\":%.1f,\"answers\":\"the test's\"}\n",
               ends[e], count, points, rounds, end_ins[e] / n, end_cyc[e] / n, end_ins[e] / ((double)count * rounds));
    pure_app_stop(&empty);
    pure_app_stop(&app);
    return 0;
}
