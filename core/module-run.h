#ifndef PURE_MODULE_RUN_H
#define PURE_MODULE_RUN_H
/* How a block runs, shared by the executor (module.c) and the compiled code
 * (module-jit.c): their frame, and the steps the compiled code calls. Not a
 * public interface. */
#include "module.h"
#include <math.h>
#include <string.h>

/* Read from a message's bytes (little-endian). */
static inline uint32_t read32(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static inline double read_double(const unsigned char *p) {
    uint64_t bits = read32(p) | (uint64_t)read32(p + 4) << 32;
    double value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static inline double minimum(double a, double b) { return a < b || (a == b && signbit(a)) ? a : b; }
static inline double maximum(double a, double b) { return a > b || (a == b && !signbit(a)) ? a : b; }

/* a op b as run computes it; false where run faults. */
static inline bool number_op(uint8_t op, double a, double b, double *out) {
    double value;
    switch (op) {
    case PURE_OP_ADD: value = a + b; break;
    case PURE_OP_SUB: value = a - b; break;
    case PURE_OP_MUL: value = a * b; break;
    case PURE_OP_DIV:
        if (b == 0) return false;
        value = a / b;
        break;
    case PURE_OP_LT: value = a < b; break;
    case PURE_OP_LE: value = a <= b; break;
    case PURE_OP_GT: value = a > b; break;
    case PURE_OP_GE: value = a >= b; break;
    case PURE_OP_EQ: value = a == b; break;
    case PURE_OP_NE: value = a != b; break;
    case PURE_OP_AND: value = a != 0 && b != 0; break;
    case PURE_OP_OR: value = a != 0 || b != 0; break;
    case PURE_OP_MIN: value = minimum(a, b); break;
    case PURE_OP_MOD:
        if (b == 0) return false;
        value = fmod(a, b);
        if (value != 0 && (value < 0) != (b < 0)) value += b;
        else if (value == 0) value = copysign(0.0, b);
        break;
    default: value = maximum(a, b); break;
    }
    *out = value;
    return isfinite(value);
}

/* An item's or a key's op->plan: its cell a text; a FIND joined to the ITEM_GET after it; a FIND whose row an ITEM_SET
 * takes; a LOOP whose block a batch may run (module-batch.c); a LOCAL_STORE ending x = e of a recurrence in such a block;
 * such a LOOP whose block begins if slot >= L.count { break } (L only grows in it: a batch runs to L's count as it opens);
 * an EACH in a batch's block whose items' rounds may run apart (spread: no break, its locals its items' own or reductions);
 * a HAS in a batch's block beginning if !L.has(k) { ... } whose code only adds to L (a filter: L only grows, so a batch asks
 * each round as it opens and again, in the rounds' order, at its end). */
enum { PLAN_TEXT = 1, PLAN_JOINED = 2, PLAN_ROW = 4, PLAN_BATCH = 8, PLAN_RECUR = 16, PLAN_GUARD = 32, PLAN_SPREAD = 64, PLAN_FILTER = 128 };

/* An item instruction's argument: its list and its field. */
static inline uint32_t list_of(uint32_t arg) { return arg & 0xFFFF; }
static inline uint32_t item_field(uint32_t arg) { return arg >> 16; }

typedef struct { union { double num; int64_t i64; }; PureBytes bytes; } Slot;

/* A walk (EACH): the list, the item it runs for, where it ends (items the
 * walk removes move it), and whether that item left. */
typedef struct { uint32_t list; size_t at, end; bool removed; } Walk;
/* A loop that runs in its block's run (LOOP, EACH): its block and where
 * the code around it ends, the arena as it began, a walk's walk and the
 * event's walk and row around it, a count's values (low + k of count) and
 * slot, and the round from which batches may run its rounds (UINT64_MAX:
 * none), how many failed and which way their lanes run. */
typedef struct {
    uint32_t start, end, outer;
    size_t mark;
    bool walking, integer;
    Walk walk, *around;
    size_t row;
    uint64_t count, k;
    PureValue low, *slot;
    uint64_t resume;
    unsigned failures;
    bool reverse; /* a batch's lanes run its rounds backwards */
} Loop;
typedef struct {
    PureApp *app;
    const PureInputShape *shape; /* message, or NULL */
    const char *key;             /* a keyed message's key */
    int64_t base;                /* the revision a message's edit was made against, or -1 */
    PureArena *arena;
    double dt;
    size_t row;   /* the row a column is evaluated for (in a fold, the fold's item) */
    size_t outer; /* in a fold, the row of the column it runs in */
    Walk *walk;   /* the walk the code runs in, or NULL */
    PureValue *locals; /* the event's locals (a handler's, and what runs in it), or NULL */
    bool arguments; /* FIELD reads the arguments given for a view, not the message */
    bool modified, exists;
    uint32_t (*spans)[2]; /* a message's variable fields: start and size in the payload */
} Event;

/* ---- Running a block (docs/design/021). A frame holds what a block reads
 * and changes as it runs: its operand stack and the loops open in it. step
 * runs one instruction but a branch or a loop's; run takes those itself, as
 * the compiled code does (module-jit.c), so both run the same steps. */
typedef struct {
    const PureModule *m;
    PureValue *values;
    Event *ev;
    PureApp *app;
    PureArena *arena;
    Slot *stack;
    size_t sp;
    uint32_t end;      /* where the innermost loop's block ends, or the block's */
    unsigned depth;    /* the loops open */
    PureFault kind;    /* a fault's */
    PureResult status; /* what refused or stopped it */
    uint32_t fault_pc; /* the instruction that faulted or refused */
    Loop *loops;       /* PURE_MODULE_NESTING of them, apart: a walk points into its loop */
    /* The run of a tree compiled code read an item from last (pure_run_item):
     * the list as it was (its version), its positions and each field's cells. */
    struct { const PureList *list; uint64_t version; size_t start, count; const void *columns[16]; } run;
} Frame;

/* How a step went: on to the next instruction; a fault (kind at fault_pc);
 * refused (status: past a limit or nothing found is a fault there, else it
 * is returned); stopped (status returned as it is). */
enum { STEP_NEXT, STEP_FAULT, STEP_REFUSED, STEP_RETURN };

/* The instruction after op at pc: past a fold's block, or the ITEM_GET a FIND was joined to. */
static inline uint32_t after(const PureOp *op, uint32_t pc) {
    return op->op == PURE_OP_FOLD ? pc + 1 + (op->arg >> 16) : pc + 1 + (op->op == PURE_OP_LIST_FIND && op->plan & PLAN_JOINED);
}

/* The steps compiled code takes from the executor (module.c): an
 * instruction but a branch or a loop's (how; the next is after()); a walk or
 * a loop opened at pc (STEP_NEXT: its block next, STEP_SKIP: none, past its
 * block); BREAK; a loop's block ran once (STEP_NEXT: again, STEP_DONE: the
 * loop ended). STEP_BATCH: step leaves a LOOP planned PLAN_BATCH to its
 * caller (module.c). */
enum { STEP_SKIP = 4, STEP_DONE = 5, STEP_BATCH = 6 };
int pure_run_step(Frame *, uint32_t pc);
int pure_run_each(Frame *, uint32_t pc);
int pure_run_loop(Frame *, uint32_t pc);
void pure_run_break(Frame *);
int pure_run_iterate(Frame *);
/* ITEM_GET at pc whose list is a tree its kept run does not hold: the
 * item's run kept, then the step. */
int pure_run_item(Frame *, uint32_t pc);
/* The rest of the innermost loop's round from pc, as the executor runs it
 * (the compiled code assumed what no longer holds, docs/design/022): the
 * frame as the code left it (the stack in its slots, the walks' places
 * written back). STEP_NEXT at the round's end (the loop's iteration next),
 * STEP_SKIP when it left the loop (BREAK: past its block), else how it
 * stopped. */
int pure_run_rest(Frame *, uint32_t pc);

/* Loops a batch of rounds at a time (module-batch.c, docs/design/024): the
 * innermost loop's rounds from loop->k (set up and counted; its LOOP
 * planned PLAN_BATCH) run as one batch: *done how many (0: none ran, its
 * block runs round by round next, at least until round loop->resume), the
 * frame left as the last of them leaves it, their work counted. STEP_NEXT,
 * or how an item it appends stopped at its round (as the executor's step:
 * the event is undone, the work so far counted). */
int pure_batch_run(Frame *, Loop *, unsigned *done);
/* Whether the LOCAL_LOAD at pc, in a block ending at end, begins x = x op e
 * (x its local, op ADD, MIN or MAX, e computed above x with no branch or
 * loop): a reduction across a batch's rounds. *at: op's pc. */
bool pure_batch_reduces(const PureModule *, uint32_t pc, uint32_t end, uint32_t *at);
/* Whether the LOCAL_STORE at s, in a block from start setting the locals
 * of set (slot: its loop's), ends x = e with e made of x, numbers, fields,
 * states, the loop's value and locals the block does not set: a recurrence
 * run round by round ahead of a batch. *from: where its statement begins. */
bool pure_batch_recurs(const PureModule *, uint32_t s, uint32_t start, const uint64_t *set, unsigned slot, uint32_t *from);

/* Where a list keeps what compiled code reads of it (module-list.c owns the
 * layout and says where): its item count, its key set (the words, how many,
 * whether they are bits), its form (pure_list_form), its arrays and the rows
 * they hold, its version, how many keys its set holds, its stored bytes,
 * its pack (a packed list's, else NULL) and its columns' cell widths (u8
 * each: 0 a DENSE list's key, 1, 4 or 8 a number's, more a text's). */
enum {
    LIST_COUNT_AT, LIST_MEMBERS_AT, LIST_MEMBER_CAPACITY_AT, LIST_BITMAP_AT, LIST_FORM_AT, LIST_COLUMNS_AT, LIST_VERSION_AT,
    LIST_ROOM_AT, LIST_MEMBER_COUNT_AT, LIST_BYTES_AT, LIST_PACK_AT, LIST_WIDTH_AT, LIST_FULL_AT
};
extern const size_t pure_list_layout[];
/* A packed list's index (docs/design/022), where its keys are whole: the
 * first position of each bucket (u32), whether the keys are whole, the
 * buckets' width as a shift, the first bucket's start, the first key and
 * the last (i64), and how many buckets (size_t). */
enum { PACK_FIRST_AT, PACK_WHOLE_AT, PACK_SHIFT_AT, PACK_ORIGIN_AT, PACK_LOW_AT, PACK_HIGH_AT, PACK_BUCKETS_AT };
extern const size_t pure_pack_layout[];

/* A module's code compiled (AArch64 Linux; elsewhere, or with PURE_JIT=0 in
 * the environment, none): a block's code runs a frame as run does, giving how
 * it went (a STEP_). */
typedef int (*PureCompiled)(Frame *);
struct PureJit *pure_jit_make(const PureModule *);
void pure_jit_free(struct PureJit *);
PureCompiled pure_jit_code(const struct PureJit *, PureBlock);
#endif
