#include "module-run.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Compiled code (docs/design/021, 022)
 * A module's blocks with a loop (its handlers' and views' code, and the
 * folds' blocks in them) as AArch64 code, made when the module loads. A
 * block's code runs a frame as run does: branches and loops are branches,
 * the hot instructions are written here, and every other one calls the
 * executor's step (module.c).
 *
 * The operand stack is the frame's, but the compiler follows what each of
 * its slots holds (a slot's height at each instruction is known when the
 * module loads): a constant, a number or a whole number in a register, a
 * comparison in the flags. A value goes to its slot only when something
 * needs it there: a call, a branch, a place two ways join.
 *
 * Each loop keeps in registers what its instructions read of the lists (a
 * column's cells, a key set's bits and how far they reach, a count) and its
 * own state (where a walk is and where it stops, a count's round). They are
 * read again after a call, which may change them; an instruction whose fast
 * way does not hold (another form, no room, no bits) calls its step out of
 * the way, then goes on as the fast way would have. */

#if defined(__aarch64__) && defined(__linux__)
#include <math.h>
#include <stddef.h>
#include <sys/mman.h>

struct PureJit {
    unsigned char *memory; /* the code, executable */
    size_t size;
    struct Entry { uint32_t length; PureCompiled code; } *by_start; /* a block's code by its start */
    uint32_t code_count;
};

/* ---- Emitting instructions */

typedef struct { size_t at; int label; unsigned kind; } Fixup; /* kind: 0 B, 1 B.cond and CBZ/CBNZ, 2 TBZ/TBNZ */
typedef struct {
    uint32_t *words;
    size_t count, room;
    long *labels; /* each label's word, or -1 */
    size_t label_count, label_room;
    Fixup *fixups;
    size_t fixup_count, fixup_room;
    bool failed;
} Emit;

static bool grow_array(void **array, size_t *room, size_t need, size_t size) {
    if (need <= *room) return true;
    size_t next = *room ? *room : 256;
    while (next < need) next *= 2;
    void *grown = realloc(*array, next * size);
    if (!grown) return false;
    *array = grown;
    *room = next;
    return true;
}

static void put(Emit *e, uint32_t word) {
    if (!grow_array((void **)&e->words, &e->room, e->count + 1, sizeof(*e->words))) {
        e->failed = true;
        return;
    }
    e->words[e->count++] = word;
}

static int new_label(Emit *e) {
    if (!grow_array((void **)&e->labels, &e->label_room, e->label_count + 1, sizeof(*e->labels))) {
        e->failed = true;
        return 0;
    }
    e->labels[e->label_count] = -1;
    return (int)e->label_count++;
}

static void bind(Emit *e, int label) {
    if (!e->failed) e->labels[label] = (long)e->count;
}

static void branch_to(Emit *e, uint32_t word, int label, unsigned kind) {
    if (!grow_array((void **)&e->fixups, &e->fixup_room, e->fixup_count + 1, sizeof(*e->fixups))) {
        e->failed = true;
        return;
    }
    e->fixups[e->fixup_count++] = (Fixup){e->count, label, kind};
    put(e, word);
}

/* Branches given their offsets, once every label is bound. */
static bool resolve(Emit *e) {
    for (size_t i = 0; i < e->fixup_count && !e->failed; ++i) {
        Fixup f = e->fixups[i];
        long target = e->labels[f.label];
        if (target < 0) return false;
        long offset = target - (long)f.at;
        if (f.kind == 0) {
            if (offset < -(1L << 25) || offset >= (1L << 25)) return false;
            e->words[f.at] |= (uint32_t)offset & 0x3FFFFFF;
        } else if (f.kind == 2) {
            if (offset < -(1L << 13) || offset >= (1L << 13)) return false;
            e->words[f.at] |= ((uint32_t)offset & 0x3FFF) << 5;
        } else {
            if (offset < -(1L << 18) || offset >= (1L << 18)) return false;
            e->words[f.at] |= ((uint32_t)offset & 0x7FFFF) << 5;
        }
    }
    return !e->failed;
}

/* x0, x1, x9, x10, x16, x17 and d0-d7 are scratch within an instruction's
 * code; x11-x13 and d16-d31 hold the stack's values; x2-x8, x14, x15, x25
 * and x26 the loops' kept values; x19 the frame, x20 its stack, x21 the values, x22 the
 * event, x23 the app, x24 its lists, x27 the frame's loops, x28 the event's
 * locals; d8 infinity. */
enum { X0 = 0, X1 = 1, X2 = 2, X9 = 9, X10 = 10, X11 = 11, X12 = 12, X13 = 13, X16 = 16, X17 = 17, X19 = 19, X20 = 20, X21 = 21, X22 = 22,
       X23 = 23, X24 = 24, X25 = 25, X26 = 26, X27 = 27, X28 = 28, X29 = 29, X30 = 30, SP = 31, XZR = 31 };
enum { D0 = 0, D1 = 1, D2 = 2, D3 = 3, D4 = 4, D8 = 8, D9 = 9 };
enum { EQ = 0, NE = 1, HS = 2, LO = 3, MI = 4, PL = 5, VS = 6, VC = 7, HI = 8, LS = 9, GE = 10, LT = 11, GT = 12, LE = 13 };

/* A 64-bit constant into register d. */
static void mov_imm(Emit *e, unsigned d, uint64_t value) {
    put(e, 0xD2800000 | (uint32_t)(value & 0xFFFF) << 5 | d); /* MOVZ */
    for (unsigned k = 1; k < 4; ++k)
        if (value >> (16 * k) & 0xFFFF) put(e, 0xF2800000 | k << 21 | (uint32_t)(value >> (16 * k) & 0xFFFF) << 5 | d); /* MOVK */
}

/* LDR/STR of 64 bits at base + offset (a multiple of 8 below 32 KiB), or through x16 past it. */
static void load64(Emit *e, unsigned t, unsigned base, size_t offset) {
    if (offset % 8 == 0 && offset < 32768) {
        put(e, 0xF9400000 | (uint32_t)(offset / 8) << 10 | base << 5 | t);
        return;
    }
    mov_imm(e, X16, offset);
    put(e, 0xF8606800 | X16 << 16 | base << 5 | t); /* LDR Xt, [Xbase, X16] */
}

static void store64(Emit *e, unsigned t, unsigned base, size_t offset) {
    if (offset % 8 == 0 && offset < 32768) {
        put(e, 0xF9000000 | (uint32_t)(offset / 8) << 10 | base << 5 | t);
        return;
    }
    mov_imm(e, X16, offset);
    put(e, 0xF8206800 | X16 << 16 | base << 5 | t); /* STR Xt, [Xbase, X16] */
}

static void load_d(Emit *e, unsigned t, unsigned base, size_t offset) { put(e, 0xFD400000 | (uint32_t)(offset / 8) << 10 | base << 5 | t); }
static void store_d(Emit *e, unsigned t, unsigned base, size_t offset) { put(e, 0xFD000000 | (uint32_t)(offset / 8) << 10 | base << 5 | t); }
static void store_h(Emit *e, unsigned t, unsigned base, size_t offset) { put(e, 0x79000000 | (uint32_t)(offset / 2) << 10 | base << 5 | t); }
static void store_w(Emit *e, unsigned t, unsigned base, size_t offset) { put(e, 0xB9000000 | (uint32_t)(offset / 4) << 10 | base << 5 | t); }
static void mov_reg(Emit *e, unsigned d, unsigned n) { put(e, 0xAA0003E0 | n << 16 | d); }
static void load8(Emit *e, unsigned t, unsigned base, size_t offset) { put(e, 0x39400000 | (uint32_t)offset << 10 | base << 5 | t); }
static void store8(Emit *e, unsigned t, unsigned base, size_t offset) { put(e, 0x39000000 | (uint32_t)offset << 10 | base << 5 | t); }
static void load16(Emit *e, unsigned t, unsigned base, size_t offset) { put(e, 0x79400000 | (uint32_t)(offset / 2) << 10 | base << 5 | t); }
static void load32(Emit *e, unsigned t, unsigned base, size_t offset) { put(e, 0xB9400000 | (uint32_t)(offset / 4) << 10 | base << 5 | t); }
/* [base, index, LSL #3] */
static void load64_index(Emit *e, unsigned t, unsigned base, unsigned index) { put(e, 0xF8607800 | index << 16 | base << 5 | t); }
static void store64_index(Emit *e, unsigned t, unsigned base, unsigned index) { put(e, 0xF8207800 | index << 16 | base << 5 | t); }
static void load_d_index(Emit *e, unsigned t, unsigned base, unsigned index) { put(e, 0xFC607800 | index << 16 | base << 5 | t); }
static void add_imm(Emit *e, unsigned d, unsigned n, uint32_t imm) { put(e, 0x91000000 | imm << 10 | n << 5 | d); } /* imm < 4096 */
static void sub_imm(Emit *e, unsigned d, unsigned n, uint32_t imm) { put(e, 0xD1000000 | imm << 10 | n << 5 | d); }
static void sub_w_imm(Emit *e, unsigned d, unsigned n, uint32_t imm) { put(e, 0x51000000 | imm << 10 | n << 5 | d); }
static void add_reg(Emit *e, unsigned d, unsigned n, unsigned m) { put(e, 0x8B000000 | m << 16 | n << 5 | d); }
static void sub_reg(Emit *e, unsigned d, unsigned n, unsigned m) { put(e, 0xCB000000 | m << 16 | n << 5 | d); }
static void cmp_reg(Emit *e, unsigned n, unsigned m) { put(e, 0xEB00001F | m << 16 | n << 5); }
static void cmp_imm(Emit *e, unsigned n, uint32_t imm) { put(e, 0xF100001F | imm << 10 | n << 5); }
static void csel(Emit *e, unsigned d, unsigned n, unsigned m, unsigned cond) { put(e, 0x9A800000 | m << 16 | cond << 12 | n << 5 | d); }
static void lsr_imm(Emit *e, unsigned d, unsigned n, unsigned shift) { put(e, 0xD340FC00 | shift << 16 | n << 5 | d); }
static void lsl_imm(Emit *e, unsigned d, unsigned n, unsigned shift) { put(e, 0xD3400000 | ((64 - shift) & 63) << 16 | (63 - shift) << 10 | n << 5 | d); }
static void lsr_reg(Emit *e, unsigned d, unsigned n, unsigned m) { put(e, 0x9AC02400 | m << 16 | n << 5 | d); }
static void lsl_reg(Emit *e, unsigned d, unsigned n, unsigned m) { put(e, 0x9AC02000 | m << 16 | n << 5 | d); }
static void orr_reg(Emit *e, unsigned d, unsigned n, unsigned m) { put(e, 0xAA000000 | m << 16 | n << 5 | d); }
static void cset(Emit *e, unsigned d, unsigned cond) { put(e, 0x1A9F07E0 | (cond ^ 1) << 12 | d); }
static void and_w(Emit *e, unsigned d, unsigned n, unsigned m) { put(e, 0x0A000000 | m << 16 | n << 5 | d); }
static void orr_w(Emit *e, unsigned d, unsigned n, unsigned m) { put(e, 0x2A000000 | m << 16 | n << 5 | d); }
static void and_w_one(Emit *e, unsigned d, unsigned n) { put(e, 0x12000000 | n << 5 | d); } /* AND Wd, Wn, #1 */
static void eor_w_one(Emit *e, unsigned d, unsigned n) { put(e, 0x52000000 | n << 5 | d); } /* EOR Wd, Wn, #1 */
static void fp3(Emit *e, uint32_t op, unsigned d, unsigned n, unsigned m) { put(e, op | m << 16 | n << 5 | d); }
static void fp2(Emit *e, uint32_t op, unsigned d, unsigned n) { put(e, op | n << 5 | d); }
static const uint32_t FADD = 0x1E602800;
static const uint32_t FSUB = 0x1E603800;
static const uint32_t FMUL = 0x1E600800;
static const uint32_t FDIV = 0x1E601800;
static const uint32_t FMIN = 0x1E605800;
static const uint32_t FMAX = 0x1E604800;
static const uint32_t FABS = 0x1E60C000;
static const uint32_t FNEG = 0x1E614000;
static const uint32_t FRINTM = 0x1E654000;
static const uint32_t UCVTF_X = 0x9E630000;
static const uint32_t UCVTF_W = 0x1E630000;
static const uint32_t FCVTZU_X = 0x9E790000;
static const uint32_t FMOV_DX = 0x9E670000; /* FMOV Dd, Xn */
static const uint32_t FMOV_XD = 0x9E660000; /* FMOV Xd, Dn */
static const uint32_t FCVTZS_X = 0x9E780000;
static const uint32_t SCVTF_X = 0x9E620000;
static void fcmp(Emit *e, unsigned n, unsigned m) { put(e, 0x1E602000 | m << 16 | n << 5); }
static void fcmp_zero(Emit *e, unsigned n) { put(e, 0x1E602008 | n << 5); }
/* STP/LDP of two 64-bit registers at sp + offset (pre-indexed down, or plain). */
static void stp(Emit *e, unsigned a, unsigned b2, int offset, bool pre) {
    put(e, (pre ? 0xA9800000 : 0xA9000000) | ((uint32_t)(offset / 8) & 0x7F) << 15 | b2 << 10 | SP << 5 | a);
}
static void ldp(Emit *e, unsigned a, unsigned b2, int offset, bool post) {
    put(e, (post ? 0xA8C00000 : 0xA9400000) | ((uint32_t)(offset / 8) & 0x7F) << 15 | b2 << 10 | SP << 5 | a);
}
static void mov_w(Emit *e, unsigned d, uint32_t value) { /* MOVZ (and MOVK) Wd */
    put(e, 0x52800000 | (value & 0xFFFF) << 5 | d);
    if (value >> 16) put(e, 0x72A00000 | (value >> 16) << 5 | d);
}

/* A call of a C function at address (in x16). */
static void call(Emit *e, uintptr_t address) {
    mov_imm(e, X16, address);
    put(e, 0xD63F0000 | X16 << 5); /* BLR x16 */
}

static void b(Emit *e, int label) { branch_to(e, 0x14000000, label, 0); }
static void b_cond(Emit *e, unsigned cond, int label) { branch_to(e, 0x54000000 | cond, label, 1); }
static void cbz_w(Emit *e, unsigned t, int label) { branch_to(e, 0x34000000 | t, label, 1); }
static void cbnz_w(Emit *e, unsigned t, int label) { branch_to(e, 0x35000000 | t, label, 1); }
static void cbz_x(Emit *e, unsigned t, int label) { branch_to(e, 0xB4000000 | t, label, 1); }
static void cbnz_x(Emit *e, unsigned t, int label) { branch_to(e, 0xB5000000 | t, label, 1); }
static void tbz(Emit *e, unsigned t, unsigned bit, int label) { branch_to(e, 0x36000000 | (bit >> 5) << 31 | (bit & 31) << 19 | t, label, 2); }
static void tbnz(Emit *e, unsigned t, unsigned bit, int label) { branch_to(e, 0x37000000 | (bit >> 5) << 31 | (bit & 31) << 19 | t, label, 2); }
static void cmp_w_imm(Emit *e, unsigned n, uint32_t imm) { put(e, 0x7100001F | imm << 10 | n << 5); }
static void mul_reg(Emit *e, unsigned d, unsigned n, unsigned m) { put(e, 0x9B007C00 | m << 16 | n << 5 | d); }
static void load32_index(Emit *e, unsigned t, unsigned base, unsigned index) { put(e, 0xB8607800 | index << 16 | base << 5 | t); } /* [base, index, LSL #2] */
static void store32_index(Emit *e, unsigned t, unsigned base, unsigned index) { put(e, 0xB8207800 | index << 16 | base << 5 | t); }
static void load8_index(Emit *e, unsigned t, unsigned base, unsigned index) { put(e, 0x38606800 | index << 16 | base << 5 | t); } /* [base, index] */
static void store8_index(Emit *e, unsigned t, unsigned base, unsigned index) { put(e, 0x38206800 | index << 16 | base << 5 | t); }
static void orr_lsl(Emit *e, unsigned d, unsigned n, unsigned m, unsigned shift) { put(e, 0xAA000000 | m << 16 | shift << 10 | n << 5 | d); }
static void add_lsl(Emit *e, unsigned d, unsigned n, unsigned m, unsigned shift) { put(e, 0x8B000000 | m << 16 | shift << 10 | n << 5 | d); }
static void prefetch(Emit *e, unsigned base) { put(e, 0xF9800000 | base << 5); } /* PRFM PLDL1KEEP, [base] */

/* ---- What the compiler knows */

enum { SLOT = sizeof(Slot) };
#define AT(h) ((size_t)(h) * SLOT)
/* An index's list, beside its value in its slot. */
enum { OWNER = offsetof(Slot, bytes) + offsetof(PureBytes, owner) };
enum { TREE_FORM = 0, FLAT_FORM = 1, DENSE_FORM = 2 }; /* pure_list_form */

/* A slot's value as compiled: in the slot (MEM), a constant (its bits), a
 * number in a d register, a whole number from 0 below 2^64 in an x register
 * (X; BIT: its bit 0 alone, the rest anything, NEGATE its opposite), or a
 * comparison in the flags (true on cond). An index's list is its owner. A
 * borrowed register is a kept value's (a walk's place), not the slot's own. */
enum { V_MEM, V_CONST, V_D, V_X, V_BIT, V_FLAG };
typedef struct {
    uint8_t kind, reg, cond;
    bool borrowed, negate;
    int16_t owner;
    uint64_t bits;
    int16_t walk; /* a fast walk's place (its kept register), or -1 */
} Value;
#define MEMORY ((Value){V_MEM, 0, 0, false, false, -1, 0, -1})

/* What a loop keeps in a register: of a list, a FLAT column's cells (0
 * when it is not FLAT), a DENSE one's, its key set's bits (NULL when it keeps
 * no bits), the keys they reach (64 a word), its count; of a walk, where it
 * is and where it stops; of a count, its round and how many. A column's
 * address carries its cells' width in its top byte (bit 56 one byte, 58
 * four, 59 eight): loads on AArch64 Linux leave the top byte out
 * (Documentation/arch/arm64/tagged-pointers.rst), so the code reads through
 * it as it is. */
enum { C_COLUMN, C_DENSE, C_BITS, C_LIMIT, C_COUNT, C_AT, C_END, C_K, C_N };
typedef struct { uint8_t kind, field; uint16_t list; uint8_t reg; int loop; } Kept;
enum { KEPT_MOST = 64, NO_REG = 0xFF };
static const uint8_t kept_registers[] = {2, 3, 4, 5, 6, 7, 8, 14, 15, 25, 26};

typedef struct {
    uint32_t start, end; /* its block */
    int parent;          /* the loop around it, or -1 */
    int again, after;    /* its iteration's label, and past it */
    unsigned depth;      /* the loops open in it, itself too */
    uint32_t at;         /* its EACH or LOOP */
    unsigned base;       /* the stack's height in its block (its bounds taken) */
    bool walk;           /* EACH (else LOOP) */
    uint32_t list;       /* a walk's */
    uint32_t writes;     /* the lists its block may change (every bit: any) */
    bool fast;           /* a walk whose list its block does not change: its place kept, its end once */
    bool arena;          /* its block may leave bytes in the arena: given back each round */
    int at_kept, end_kept, k_kept, n_kept; /* its state's kept values */
    int nest;            /* the outermost loop around it (itself when none is) */
    uint64_t region;     /* what its nest keeps: each in a register of its own, all read at each label in it */
    int ahead;           /* a slice it opens whose key it reads ahead (an Ahead), or -1 */
} LoopAt;

/* A key as a loop computes it, followed from its block's start (reading
 * ahead): a constant, the loop's round (a count's value, a walk's place),
 * an item's field at a position, or arithmetic of two. */
enum { S_CONST, S_ROUND, S_ITEM, S_ADD, S_SUB, S_MUL };
typedef struct { uint8_t kind; uint16_t list, field; int a, b; double value; } Sym;
/* A slice read ahead: the loop opening it (an EACH in its block), its list and the column its block reads, its low key. */
typedef struct { unsigned inner; uint32_t list, field; int key; Sym nodes[32]; unsigned count; } Ahead;

typedef struct { int label; PureFault kind; uint32_t pc; } FaultAt;

/* An instruction's step, called out of the fast way's line: its operands
 * stored first (as they were when it branched), the loops' places written
 * back; its result loaded back where the fast way leaves it, what was kept
 * read again; then back. */
typedef struct {
    int label, back;
    uint32_t pc;
    uintptr_t function;
    unsigned height, base; /* the stack's height at the instruction, and its operands' first */
    Value operands[PURE_RECORD_FIELDS + 2];
    int result;            /* its result's height, or -1 */
    uint8_t result_kind, result_reg;
    uint64_t fresh;        /* what is kept at its end */
    int loop;              /* the innermost loop around it */
    int tree_list;         /* ITEM_GET whose kept column was none: the run a tree's code keeps tried first (else -1) */
    unsigned tree_field;
    bool two;              /* the step at pc, then the one at pc + 1 (an ITEM_GET and its HAS, run as one) */
} Slow;

/* A cell of eight bytes or one, read out of the line a four-byte one is read in. */
typedef struct { int label, back; uint8_t result, column, at; } Wide;

typedef struct {
    const PureModule *m;
    PureBlock block;
    Emit *e;
    uint8_t *height;  /* absolute, by instruction of the block */
    int *loop_of;     /* the innermost loop each instruction is in, or -1 */
    int *opens;       /* the loop an EACH or a LOOP opens */
    LoopAt *loops;
    unsigned loop_count;
    int *op_label;    /* each instruction's, and the block's end */
    bool *target;     /* each instruction a branch lands on */
    int exit;         /* w0 holds how it went */
    FaultAt *faults;  /* the faults its code may give, each a short stub after it */
    size_t fault_count, fault_room;
    Slow *slows;
    size_t slow_count, slow_room;
    Wide *wides;
    size_t wide_count, wide_room;
    Kept kept[KEPT_MOST];
    unsigned kept_count;
    Ahead *aheads;
    size_t ahead_count, ahead_room;
    uint64_t fresh;   /* what is kept and holds its value now */
    bool joined;      /* the instruction just compiled took the next one too */
    Value vs[PURE_MODULE_STACK + 1];
} Block;

/* A label whose code gives a fault of kind at pc. */
static int fault_at(Block *k, PureFault kind, uint32_t pc) {
    for (size_t i = 0; i < k->fault_count; ++i)
        if (k->faults[i].kind == kind && k->faults[i].pc == pc) return k->faults[i].label;
    if (!grow_array((void **)&k->faults, &k->fault_room, k->fault_count + 1, sizeof(*k->faults))) {
        k->e->failed = true;
        return k->exit;
    }
    k->faults[k->fault_count] = (FaultAt){new_label(k->e), kind, pc};
    return k->faults[k->fault_count++].label;
}

static void fault_stubs(Block *k) {
    Emit *e = k->e;
    for (size_t i = 0; i < k->fault_count; ++i) {
        bind(e, k->faults[i].label);
        mov_w(e, X9, (uint32_t)k->faults[i].kind);
        store_w(e, X9, X19, offsetof(Frame, kind));
        mov_w(e, X9, k->faults[i].pc);
        store_w(e, X9, X19, offsetof(Frame, fault_pc));
        mov_w(e, X0, STEP_FAULT);
        b(e, k->exit);
    }
}

static void set_sp(Block *k, unsigned height) {
    mov_imm(k->e, X9, height);
    store64(k->e, X9, X19, offsetof(Frame, sp));
}

static size_t loop_offset(const LoopAt *l) { return (l->depth - 1) * sizeof(Loop); }

/* ---- Kept values */

/* What the nest of loop keeps of kind for list and field (a loop's state: list is the loop), or -1. */
static int kept_find(const Block *k, int loop, unsigned kind, unsigned list, unsigned field) {
    if (loop < 0) return -1;
    int nest = k->loops[loop].nest;
    for (unsigned i = 0; i < k->kept_count; ++i) {
        const Kept *c = &k->kept[i];
        if (c->loop == nest && c->kind == kind && c->list == list && c->field == field) return (int)i;
    }
    return -1;
}

/* Kept value c read into its register. */
static void kept_load(Block *k, unsigned c) {
    Emit *e = k->e;
    const Kept *v = &k->kept[c];
    unsigned r = v->reg;
    size_t list_at = (size_t)v->list * sizeof(PureList *);
    switch (v->kind) {
    case C_COLUMN: case C_DENSE:
        load64(e, X16, X24, list_at);
        load8(e, X17, X16, pure_list_layout[LIST_FORM_AT]);
        load64(e, r, X16, pure_list_layout[LIST_COLUMNS_AT] + v->field * sizeof(void *));
        load8(e, X9, X16, pure_list_layout[LIST_WIDTH_AT] + v->field);
        orr_lsl(e, r, r, X9, 56);
        cmp_w_imm(e, X17, v->kind == C_COLUMN ? FLAT_FORM : DENSE_FORM);
        csel(e, r, r, XZR, EQ);
        break;
    case C_BITS:
        load64(e, X16, X24, list_at);
        load64(e, r, X16, pure_list_layout[LIST_MEMBERS_AT]);
        load8(e, X17, X16, pure_list_layout[LIST_BITMAP_AT]);
        cmp_w_imm(e, X17, 0);
        csel(e, r, r, XZR, NE);
        break;
    case C_LIMIT:
        load64(e, X16, X24, list_at);
        load64(e, r, X16, pure_list_layout[LIST_MEMBER_CAPACITY_AT]);
        lsl_imm(e, r, r, 6);
        break;
    case C_COUNT:
        load64(e, X16, X24, list_at);
        load64(e, r, X16, pure_list_layout[LIST_COUNT_AT]);
        break;
    case C_AT:
        load64(e, r, X27, loop_offset(&k->loops[v->list]) + offsetof(Loop, walk) + offsetof(Walk, at));
        break;
    case C_END: { /* where it stops: its end, or the list's count when that comes first */
        const LoopAt *l = &k->loops[v->list];
        load64(e, r, X27, loop_offset(l) + offsetof(Loop, walk) + offsetof(Walk, end));
        load64(e, X16, X24, (size_t)l->list * sizeof(PureList *));
        load64(e, X17, X16, pure_list_layout[LIST_COUNT_AT]);
        cmp_reg(e, r, X17);
        csel(e, r, r, X17, LO);
        break;
    }
    case C_K: load64(e, r, X27, loop_offset(&k->loops[v->list]) + offsetof(Loop, k)); break;
    case C_N: load64(e, r, X27, loop_offset(&k->loops[v->list]) + offsetof(Loop, count)); break;
    }
}

/* The kept values in mask that do not hold theirs now, read again. */
static void refresh(Block *k, uint64_t mask) {
    for (unsigned c = 0; c < k->kept_count; ++c)
        if (mask >> c & 1 && !(k->fresh >> c & 1)) kept_load(k, c);
    k->fresh |= mask;
}

static unsigned kept_reg(Block *k, int c) {
    refresh(k, UINT64_C(1) << c);
    return k->kept[c].reg;
}

/* What the code in loop may count on being kept: its nest's. */
static uint64_t kept_at(const Block *k, int loop) { return loop >= 0 ? k->loops[loop].region : 0; }

/* Before a call from code in loop: the places of the walks open around it,
 * kept in registers, back where the steps read them (the event's row the
 * innermost walk's). */
static void write_back(Block *k, int loop) {
    Emit *e = k->e;
    bool row = false;
    for (int l = loop; l >= 0; l = k->loops[l].parent) {
        const LoopAt *w = &k->loops[l];
        if (!w->walk || !w->fast || w->at_kept < 0 || !(k->fresh >> w->at_kept & 1)) {
            row = row || w->walk;
            continue;
        }
        unsigned r = k->kept[w->at_kept].reg;
        store64(e, r, X27, loop_offset(w) + offsetof(Loop, walk) + offsetof(Walk, at));
        if (!row) store64(e, r, X22, offsetof(Event, row));
        row = true;
    }
}

/* ---- The stack as compiled */

static void owner_is(Emit *e, unsigned height, int owner) {
    mov_w(e, X9, (uint32_t)owner & 0xFFFF);
    store_h(e, X9, X20, AT(height) + OWNER);
}

/* Whether register r (d or x) holds a value below height. */
static bool held(const Block *k, unsigned below, bool d, unsigned r) {
    for (unsigned h = 0; h < below; ++h) {
        const Value *v = &k->vs[h];
        if (v->borrowed || v->reg != r) continue;
        if (d ? v->kind == V_D : v->kind == V_X || v->kind == V_BIT) return true;
    }
    return false;
}

static void spill(Block *k, unsigned h);

/* A d register for a value at height below or above (d16-d31): a free one,
 * or the lowest one held, its value stored first. */
static unsigned alloc_d(Block *k, unsigned below) {
    for (unsigned r = 16; r < 32; ++r)
        if (!held(k, below, true, r)) return r;
    for (unsigned h = 0; h < below; ++h)
        if (k->vs[h].kind == V_D && !k->vs[h].borrowed) {
            unsigned r = k->vs[h].reg;
            spill(k, h);
            return r;
        }
    return 16;
}

static unsigned alloc_x(Block *k, unsigned below) {
    for (unsigned r = X11; r < X11 + 3; ++r)
        if (!held(k, below, false, r)) return r;
    for (unsigned h = 0; h < below; ++h)
        if ((k->vs[h].kind == V_X || k->vs[h].kind == V_BIT) && !k->vs[h].borrowed) {
            unsigned r = k->vs[h].reg;
            spill(k, h);
            return r;
        }
    return X11;
}

/* The value at h into its slot. */
static void spill(Block *k, unsigned h) {
    Emit *e = k->e;
    Value *v = &k->vs[h];
    switch (v->kind) {
    case V_MEM: return;
    case V_CONST: mov_imm(e, X9, v->bits); store64(e, X9, X20, AT(h)); break;
    case V_D: store_d(e, v->reg, X20, AT(h)); break;
    case V_X: fp2(e, UCVTF_X, D0, v->reg); store_d(e, D0, X20, AT(h)); break;
    case V_BIT:
        and_w_one(e, X9, v->reg);
        if (v->negate) eor_w_one(e, X9, X9);
        fp2(e, UCVTF_W, D0, X9);
        store_d(e, D0, X20, AT(h));
        break;
    case V_FLAG: cset(e, X9, v->cond); fp2(e, UCVTF_W, D0, X9); store_d(e, D0, X20, AT(h)); break;
    }
    if (v->owner >= 0) owner_is(e, h, v->owner);
    *v = MEMORY;
}

static void spill_all(Block *k, unsigned height) {
    for (unsigned h = 0; h < height; ++h) spill(k, h);
}

/* A comparison at h held in the flags as a bit in a register (what follows
 * changes the flags), the values below live height kept. */
static void unflag(Block *k, unsigned h, unsigned live) {
    Value *v = &k->vs[h];
    if (v->kind != V_FLAG) return;
    unsigned r = alloc_x(k, live);
    cset(k->e, r, v->cond);
    *v = (Value){V_BIT, (uint8_t)r, 0, false, false, v->owner, 0, -1};
}

/* Every comparison below height out of the flags (the code next sets them). */
static void unflag_below(Block *k, unsigned height) {
    for (unsigned h = 0; h < height; ++h) unflag(k, h, height);
}

static double num_of(uint64_t bits) {
    double x;
    memcpy(&x, &bits, sizeof(x));
    return x;
}

/* The number at h in a d register: its own, or scratch. */
static unsigned value_d(Block *k, unsigned h, unsigned scratch) {
    Emit *e = k->e;
    Value *v = &k->vs[h];
    switch (v->kind) {
    case V_D: return v->reg;
    case V_MEM: load_d(e, scratch, X20, AT(h)); return scratch;
    case V_CONST: mov_imm(e, X9, v->bits); fp2(e, FMOV_DX, scratch, X9); return scratch;
    case V_X: fp2(e, UCVTF_X, scratch, v->reg); return scratch;
    case V_BIT:
        and_w_one(e, X9, v->reg);
        if (v->negate) eor_w_one(e, X9, X9);
        fp2(e, UCVTF_W, scratch, X9);
        return scratch;
    default: cset(e, X9, v->cond); fp2(e, UCVTF_W, scratch, X9); return scratch;
    }
}

/* The value at h's 8 bytes as they are (a number's, a bool's or an i64's) in an x register. */
static unsigned value_bits(Block *k, unsigned h, unsigned scratch) {
    Emit *e = k->e;
    Value *v = &k->vs[h];
    if (v->kind == V_MEM) load64(e, scratch, X20, AT(h));
    else if (v->kind == V_CONST) mov_imm(e, scratch, v->bits);
    else fp2(e, FMOV_XD, scratch, value_d(k, h, D0));
    return scratch;
}

/* The value at h as a position (size_t of it, as the executor takes one: a
 * fraction cut, below 0 or NaN as 0) in an x register. */
static unsigned value_index(Block *k, unsigned h, unsigned scratch) {
    Value *v = &k->vs[h];
    if (v->kind == V_X) return v->reg;
    if (v->kind == V_BIT) {
        and_w_one(k->e, scratch, v->reg);
        if (v->negate) eor_w_one(k->e, scratch, scratch);
        return scratch;
    }
    fp2(k->e, FCVTZU_X, scratch, value_d(k, h, D0));
    return scratch;
}

/* The value at h as a key that is a whole number from 0 (-0 as 0) in an x
 * register, else a branch to other. */
static unsigned value_key(Block *k, unsigned h, unsigned scratch, int other) {
    Emit *e = k->e;
    Value *v = &k->vs[h];
    if (v->kind == V_X) return v->reg;
    if (v->kind == V_BIT) return value_index(k, h, scratch);
    if (v->kind == V_CONST) {
        double x = num_of(v->bits);
        if (x >= 0 && x == floor(x) && x < 0x1p63) mov_imm(e, scratch, (uint64_t)x);
        else b(e, other);
        return scratch;
    }
    unsigned d = value_d(k, h, D0);
    fp2(e, FCVTZU_X, scratch, d);
    fp2(e, UCVTF_X, D1, scratch);
    fcmp(e, d, D1);
    b_cond(e, NE, other);
    return scratch;
}

/* An integer comparison may be made of a value that is a whole number: in an x register, or a constant one. */
static bool whole(const Value *v) {
    if (v->kind == V_X) return true;
    if (v->kind != V_CONST) return false;
    double x = num_of(v->bits);
    return x >= 0 && x < 0x1p53 && x == floor(x) && !(x == 0 && signbit(x));
}

static unsigned value_whole(Block *k, unsigned h, unsigned scratch) {
    Value *v = &k->vs[h];
    if (v->kind == V_X) return v->reg;
    mov_imm(k->e, scratch, (uint64_t)num_of(v->bits));
    return scratch;
}

/* ---- Branches and joins: every value in its slot, what is kept read */

/* Where a branch at from to target lands: the iteration of the innermost loop around from that ends at target, else target's instruction. */
static int landing(const Block *k, uint32_t from, uint32_t target, int *loop) {
    for (int l = k->loop_of[from - k->block.start]; l >= 0; l = k->loops[l].parent)
        if (k->loops[l].end == target) {
            *loop = l;
            return k->loops[l].again;
        }
    *loop = target < k->block.start + k->block.length ? k->loop_of[target - k->block.start] : -1;
    return k->op_label[target - k->block.start];
}

/* Before the code reaches a label (by a branch or by going on): the values
 * in their slots, what that label's loop keeps read. */
static void join(Block *k, unsigned height, int loop) {
    spill_all(k, height);
    refresh(k, kept_at(k, loop));
    k->fresh = kept_at(k, loop); /* as every way here has it */
}

/* ---- Out of the way: steps */

static Slow *slow_at(Block *k, uint32_t pc, uintptr_t function, unsigned height, unsigned base, int result) {
    if (!grow_array((void **)&k->slows, &k->slow_room, k->slow_count + 1, sizeof(*k->slows))) {
        k->e->failed = true;
        return NULL;
    }
    Slow *s = &k->slows[k->slow_count++];
    memset(s, 0, sizeof(*s));
    s->label = new_label(k->e), s->back = new_label(k->e);
    s->pc = pc, s->function = function, s->height = height, s->base = base, s->result = result;
    for (unsigned h = base; h < height; ++h) s->operands[h - base] = k->vs[h];
    s->loop = k->loop_of[pc - k->block.start];
    s->tree_list = -1;
    return s;
}

/* Where the fast way goes on (s by index: the array may move). */
static void slow_back(Block *k, size_t s, unsigned kind, unsigned reg) {
    Slow *slow = &k->slows[s];
    slow->result_kind = (uint8_t)kind, slow->result_reg = (uint8_t)reg;
    slow->fresh = k->fresh;
    bind(k->e, slow->back);
}

/* A step called for s: its operands in their slots, the walks' places
 * written back; its result where the fast way leaves it, what is kept read
 * again; then back. */
static void stub_call(Block *k, const Slow *s, uintptr_t function) {
    Emit *e = k->e;
    for (unsigned h = s->base; h < s->height; ++h) k->vs[h] = s->operands[h - s->base];
    for (unsigned h = s->base; h < s->height; ++h) spill(k, h);
    k->fresh = s->fresh;
    write_back(k, s->loop);
    set_sp(k, s->height);
    mov_reg(e, X0, X19);
    mov_w(e, X1, s->pc);
    call(e, function);
    cbnz_w(e, X0, k->exit);
    if (s->two) {
        mov_reg(e, X0, X19);
        mov_w(e, X1, s->pc + 1);
        call(e, (uintptr_t)pure_run_step);
        cbnz_w(e, X0, k->exit);
    }
    if (s->result >= 0) {
        if (s->result_kind == V_D) load_d(e, s->result_reg, X20, AT(s->result));
        else { /* a bool or a whole number: its number back to an integer */
            load_d(e, D0, X20, AT(s->result));
            fp2(e, FCVTZU_X, s->result_reg, D0);
        }
    }
    k->fresh = 0;
    refresh(k, s->fresh);
    b(e, s->back);
}

static void slow_stubs(Block *k) {
    Emit *e = k->e;
    for (size_t i = 0; i < k->slow_count; ++i) {
        Slow s = k->slows[i];
        Value saved[PURE_MODULE_STACK + 1];
        memcpy(saved, k->vs, sizeof(saved));
        bind(e, s.label);
        if (s.tree_list >= 0) { /* a tree's item: the run kept, else its run kept by the step (pure_run_item); DENSE: the step */
            int item = new_label(e), step = new_label(e);
            for (unsigned h = s.base; h < s.height; ++h) k->vs[h] = s.operands[h - s.base];
            load64(e, X16, X24, (size_t)s.tree_list * sizeof(PureList *));
            load8(e, X17, X16, pure_list_layout[LIST_FORM_AT]);
            cbnz_w(e, X17, step);
            unsigned at = value_index(k, s.base, X9);
            load64(e, X10, X19, offsetof(Frame, run.list));
            cmp_reg(e, X10, X16);
            b_cond(e, NE, item);
            load64(e, X10, X19, offsetof(Frame, run.version));
            load64(e, X17, X16, pure_list_layout[LIST_VERSION_AT]);
            cmp_reg(e, X10, X17);
            b_cond(e, NE, item);
            load64(e, X10, X19, offsetof(Frame, run.start));
            sub_reg(e, X0, at, X10);
            load64(e, X10, X19, offsetof(Frame, run.count));
            cmp_reg(e, X0, X10);
            b_cond(e, HS, item);
            load64(e, X17, X19, offsetof(Frame, run.columns) + s.tree_field * sizeof(void *));
            load_d_index(e, s.result_reg, X17, X0);
            b(e, s.back);
            bind(e, item);
            stub_call(k, &s, (uintptr_t)pure_run_item);
            bind(e, step);
        }
        stub_call(k, &s, s.function);
        memcpy(k->vs, saved, sizeof(saved));
    }
}

/* A cell at at of a column (its address tagged with its width) as a number in r. */
static void load_cell(Block *k, unsigned r, unsigned column, unsigned at) {
    Emit *e = k->e;
    if (!grow_array((void **)&k->wides, &k->wide_room, k->wide_count + 1, sizeof(*k->wides))) {
        e->failed = true;
        return;
    }
    Wide *w = &k->wides[k->wide_count++];
    *w = (Wide){new_label(e), new_label(e), (uint8_t)r, (uint8_t)column, (uint8_t)at};
    tbz(e, column, 58, w->label);
    load32_index(e, X10, column, at);
    fp2(e, UCVTF_W, r, X10);
    bind(e, w->back);
}

static void wide_stubs(Block *k) {
    Emit *e = k->e;
    for (size_t i = 0; i < k->wide_count; ++i) {
        const Wide *w = &k->wides[i];
        int one = new_label(e);
        bind(e, w->label);
        tbz(e, w->column, 59, one);
        load_d_index(e, w->result, w->column, w->at);
        b(e, w->back);
        bind(e, one);
        load8_index(e, X10, w->column, w->at);
        fp2(e, UCVTF_W, w->result, X10);
        b(e, w->back);
    }
}

/* An instruction the step runs, in line: the stack in its slots, what was kept read again when needed. */
static void call_step(Block *k, uintptr_t function, uint32_t pc, unsigned height) {
    Emit *e = k->e;
    spill_all(k, height);
    write_back(k, k->loop_of[pc - k->block.start]);
    set_sp(k, height);
    mov_reg(e, X0, X19);
    mov_w(e, X1, pc);
    call(e, function);
    cbnz_w(e, X0, k->exit);
    k->fresh = 0;
    for (unsigned h = 0; h <= PURE_MODULE_STACK; ++h) k->vs[h] = MEMORY; /* what it left is in the slots */
}

/* ---- Instructions */

static void binary(Block *k, const PureOp *op, uint32_t pc, unsigned height) {
    Emit *e = k->e;
    unsigned h = height - 2;
    unflag_below(k, height);
    Value *a = &k->vs[h], *bv = &k->vs[h + 1];
    unsigned cond;
    switch (op->op) {
    case PURE_OP_LT: case PURE_OP_LE: case PURE_OP_GT: case PURE_OP_GE: case PURE_OP_EQ: case PURE_OP_NE:
        if (whole(a) && whole(bv)) { /* whole numbers below 2^53: compared as integers */
            unsigned x = value_whole(k, h, X9), y = value_whole(k, h + 1, X10);
            cmp_reg(e, x, y);
            cond = op->op == PURE_OP_LT ? LO : op->op == PURE_OP_LE ? LS : op->op == PURE_OP_GT ? HI : op->op == PURE_OP_GE ? HS
                 : op->op == PURE_OP_EQ ? EQ : NE;
        } else {
            unsigned x = value_d(k, h, D0), y = value_d(k, h + 1, D1);
            fcmp(e, x, y);
            cond = op->op == PURE_OP_LT ? MI : op->op == PURE_OP_LE ? LS : op->op == PURE_OP_GT ? GT : op->op == PURE_OP_GE ? GE
                 : op->op == PURE_OP_EQ ? EQ : NE;
        }
        k->vs[h] = (Value){V_FLAG, 0, (uint8_t)cond, false, false, -1, 0, -1};
        return;
    case PURE_OP_AND: case PURE_OP_OR: {
        unsigned r = alloc_x(k, h);
        unsigned x = value_d(k, h, D0);
        fcmp_zero(e, x);
        cset(e, X10, NE);
        unsigned y = value_d(k, h + 1, D1);
        fcmp_zero(e, y);
        cset(e, X9, NE);
        if (op->op == PURE_OP_AND) and_w(e, r, X10, X9);
        else orr_w(e, r, X10, X9);
        k->vs[h] = (Value){V_BIT, (uint8_t)r, 0, false, false, -1, 0, -1};
        return;
    }
    default: break;
    }
    unsigned r = alloc_d(k, h), x = value_d(k, h, D0), y = value_d(k, h + 1, D1);
    switch (op->op) {
    case PURE_OP_ADD: fp3(e, FADD, r, x, y); break;
    case PURE_OP_SUB: fp3(e, FSUB, r, x, y); break;
    case PURE_OP_MUL: fp3(e, FMUL, r, x, y); break;
    case PURE_OP_DIV:
        fcmp_zero(e, y);
        b_cond(e, EQ, fault_at(k, PURE_FAULT_DIVIDE, pc));
        fp3(e, FDIV, r, x, y);
        break;
    case PURE_OP_MIN: fp3(e, FMIN, r, x, y); break;
    default: fp3(e, FMAX, r, x, y); break;
    }
    fp2(e, FABS, D2, r); /* finite: below infinity (a NaN is unordered: HS too) */
    fcmp(e, D2, D8);
    b_cond(e, HS, fault_at(k, PURE_FAULT_NON_FINITE, pc));
    k->vs[h] = (Value){V_D, (uint8_t)r, 0, false, false, -1, 0, -1};
}

/* A key's bit in a list's bits (in bits, reaching limit keys): r's bit 0,
 * else to absent (past them); key at h, not whole: absent too. */
static unsigned key_bit(Block *k, unsigned h, unsigned bits, unsigned limit, int absent, unsigned r) {
    Emit *e = k->e;
    unsigned key = value_key(k, h, X9, absent);
    cmp_reg(e, key, limit);
    b_cond(e, HS, absent);
    lsr_imm(e, X10, key, 6);
    load64_index(e, X10, bits, X10);
    lsr_reg(e, r, X10, key);
    return key;
}

static bool event_list(const PureModule *m, uint32_t list) {
    return m->state_type[list] == PURE_STATE_LIST && m->state_flags[list] & PURE_LIST_ROWS >> 8;
}

static bool numbers_only(const PureModule *m, uint32_t list) {
    const PureRecord *record = &m->record[m->state_ref[list]];
    for (unsigned f = 0; f < record->count; ++f)
        if (m->record_field_type[record->first + f] == PURE_RECORD_TEXT) return false;
    return true;
}

/* The value at h into a column (in col, its cells width bytes, in wbytes)
 * at row at, or to slow when it does not fit (narrow: a whole number from
 * 0, not -0, below 2^8 or 2^32); a DENSE list's key (0) stays out. x0, x1,
 * d0 and d1 are spent. */
static void store_cell(Block *k, unsigned h, unsigned col, unsigned wbytes, unsigned at, int slow) {
    Emit *e = k->e;
    Value *v = &k->vs[h];
    int eight = new_label(e), four = new_label(e), done = new_label(e);
    cmp_w_imm(e, wbytes, 8);
    b_cond(e, EQ, eight);
    cbz_w(e, wbytes, done);
    unsigned x = X0;
    if (v->kind == V_X) x = v->reg;
    else if (v->kind == V_BIT) {
        and_w_one(e, X0, v->reg);
        if (v->negate) eor_w_one(e, X0, X0);
    } else if (v->kind == V_CONST) {
        double n = num_of(v->bits);
        if (n >= 0 && n == floor(n) && !(n == 0 && signbit(n)) && n < 0x1p63) mov_imm(e, X0, (uint64_t)n);
        else b(e, slow);
    } else {
        unsigned d = value_d(k, h, D0);
        fp2(e, FMOV_XD, X0, d);
        tbnz(e, X0, 63, slow); /* below 0, or -0 */
        fp2(e, FCVTZU_X, X0, d);
        fp2(e, UCVTF_X, D1, X0);
        fcmp(e, d, D1);
        b_cond(e, NE, slow);
    }
    cmp_w_imm(e, wbytes, 4);
    b_cond(e, EQ, four);
    cmp_imm(e, x, 256);
    b_cond(e, HS, slow);
    store8_index(e, x, col, at);
    b(e, done);
    bind(e, four);
    mov_imm(e, X1, UINT64_C(1) << 32);
    cmp_reg(e, x, X1);
    b_cond(e, HS, slow);
    store32_index(e, x, col, at);
    b(e, done);
    bind(e, eight);
    if (v->kind == V_BIT) {
        and_w_one(e, X0, v->reg);
        if (v->negate) eor_w_one(e, X0, X0);
        fp2(e, UCVTF_W, D0, X0);
        fp2(e, FMOV_XD, X0, D0);
        store64_index(e, X0, col, at);
    } else
        store64_index(e, value_bits(k, h, X0), col, at);
    bind(e, done);
}

/* LIST_APPEND to an event's list of numbers whose places no index follows:
 * at the end of a FLAT one, or at its key in a DENSE one, with room, each
 * cell fitting its column. */
static void append(Block *k, const PureOp *op, uint32_t pc, unsigned height) {
    Emit *e = k->e;
    const PureModule *m = k->m;
    uint32_t list = op->arg;
    unsigned fields = m->record[m->state_ref[list]].count, base = height - fields;
    bool keyed = m->state_key[list] != 0;
    unflag_below(k, height);
    spill_all(k, base);
    size_t s = (size_t)(slow_at(k, pc, (uintptr_t)pure_run_step, height, base, -1) - k->slows);
    if (e->failed) return;
    int slow = k->slows[s].label;
    load64(e, X16, X24, list * sizeof(PureList *));
    load8(e, X17, X16, pure_list_layout[LIST_FORM_AT]);
    cmp_w_imm(e, X17, keyed ? DENSE_FORM : FLAT_FORM);
    b_cond(e, NE, slow);
    if (!keyed) {
        load64(e, X9, X16, pure_list_layout[LIST_COUNT_AT]);
        load64(e, X10, X16, pure_list_layout[LIST_ROOM_AT]);
        cmp_reg(e, X9, X10);
        b_cond(e, HS, slow);
    } else {
        unsigned key = value_key(k, base + m->state_key[list] - 1u, X9, slow);
        if (key != X9) mov_reg(e, X9, key);
        mov_imm(e, X10, (uint64_t)1 << 22); /* a small key (module-list.c's BITMAP_KEYS) */
        cmp_reg(e, X9, X10);
        b_cond(e, HS, slow);
        load64(e, X10, X16, pure_list_layout[LIST_ROOM_AT]);
        cmp_reg(e, X9, X10);
        b_cond(e, HS, slow);
        load64(e, X0, X16, pure_list_layout[LIST_MEMBERS_AT]);
        cbz_x(e, X0, slow);
        load64(e, X1, X16, pure_list_layout[LIST_MEMBER_CAPACITY_AT]);
        lsr_imm(e, X10, X9, 6);
        cmp_reg(e, X10, X1);
        b_cond(e, HS, slow);
        load64_index(e, X1, X0, X10); /* its word: the key there already is EXISTS */
        lsr_reg(e, X17, X1, X9);
        tbnz(e, X17, 0, fault_at(k, PURE_FAULT_EXISTS, pc));
    }
    for (unsigned f = 0; f < fields; ++f) { /* the cells, past the end or in the key's empty slot: not the list's until counted */
        load64(e, X10, X16, pure_list_layout[LIST_COLUMNS_AT] + f * sizeof(void *));
        load8(e, X17, X16, pure_list_layout[LIST_WIDTH_AT] + f);
        store_cell(k, base + f, X10, X17, X9, slow);
    }
    if (!keyed) {
        add_imm(e, X9, X9, 1);
        store64(e, X9, X16, pure_list_layout[LIST_COUNT_AT]);
    } else {
        load64(e, X0, X16, pure_list_layout[LIST_MEMBERS_AT]);
        lsr_imm(e, X10, X9, 6);
        load64_index(e, X1, X0, X10);
        mov_imm(e, X17, 1);
        lsl_reg(e, X17, X17, X9);
        orr_reg(e, X1, X1, X17);
        store64_index(e, X1, X0, X10);
        load64(e, X10, X16, pure_list_layout[LIST_MEMBER_COUNT_AT]);
        add_imm(e, X10, X10, 1);
        store64(e, X10, X16, pure_list_layout[LIST_MEMBER_COUNT_AT]);
        load64(e, X10, X16, pure_list_layout[LIST_COUNT_AT]);
        add_imm(e, X10, X10, 1);
        store64(e, X10, X16, pure_list_layout[LIST_COUNT_AT]);
    }
    load64(e, X10, X16, pure_list_layout[LIST_VERSION_AT]);
    add_imm(e, X10, X10, 1);
    store64(e, X10, X16, pure_list_layout[LIST_VERSION_AT]);
    /* its stored bytes, and the event's growth: past the budget or the room, MEMORY (the item stays: the event is undone) */
    uint32_t bytes = 8 * fields;
    load64(e, X10, X16, pure_list_layout[LIST_BYTES_AT]);
    add_imm(e, X10, X10, bytes);
    store64(e, X10, X16, pure_list_layout[LIST_BYTES_AT]);
    load64(e, X10, X23, offsetof(PureApp, grown));
    add_imm(e, X10, X10, bytes);
    store64(e, X10, X23, offsetof(PureApp, grown));
    load64(e, X0, X23, offsetof(PureApp, spent));
    add_imm(e, X0, X0, bytes);
    store64(e, X0, X23, offsetof(PureApp, spent));
    int budgeted = new_label(e);
    load64(e, X1, X23, offsetof(PureApp, memory));
    cbz_x(e, X1, budgeted);
    cmp_reg(e, X10, X1);
    b_cond(e, GT, fault_at(k, PURE_FAULT_MEMORY, pc));
    bind(e, budgeted);
    load64(e, X1, X23, offsetof(PureApp, room));
    cmp_reg(e, X0, X1);
    b_cond(e, GT, fault_at(k, PURE_FAULT_MEMORY, pc));
    slow_back(k, s, V_MEM, 0);
}

/* ITEM_GET of a number at pc and the HAS at pc + 1 that takes it, as one:
 * the cell read as a key (a four- or one-byte cell is one already; an
 * eight-byte one when whole) and found in the key bits, kept for the loop
 * around. False when there is nothing kept to do it with (then each alone). */
static bool item_has(Block *k, uint32_t pc, unsigned height) {
    Emit *e = k->e;
    const PureModule *m = k->m;
    const PureOp *get = &m->code[pc], *has = &m->code[pc + 1];
    int loop = k->loop_of[pc - k->block.start];
    if (get->plan & PLAN_TEXT || has->op != PURE_OP_LIST_HAS || has->plan & PLAN_TEXT || k->target[pc + 1 - k->block.start] ||
        k->loop_of[pc + 1 - k->block.start] != loop)
        return false;
    for (unsigned l = 0; l < k->loop_count; ++l)
        if (k->loops[l].end == pc + 1 || k->loops[l].start == pc + 1) return false;
    uint32_t list = list_of(get->arg), field = item_field(get->arg);
    int col_c = kept_find(k, loop, C_COLUMN, list, field), count_c = kept_find(k, loop, C_COUNT, list, 0),
        bits_c = kept_find(k, loop, C_BITS, has->arg, 0), limit_c = kept_find(k, loop, C_LIMIT, has->arg, 0);
    if (col_c < 0 || bits_c < 0 || limit_c < 0) return false;
    unflag_below(k, height);
    spill_all(k, height - 1);
    size_t s = (size_t)(slow_at(k, pc, (uintptr_t)pure_run_step, height, height - 1, (int)height - 1) - k->slows);
    if (e->failed) return true;
    k->slows[s].two = true;
    int slow = k->slows[s].label, other = new_label(e), key = new_label(e), absent = new_label(e), done = new_label(e),
        one = new_label(e);
    unsigned r = alloc_x(k, height), column = kept_reg(k, col_c), bits = kept_reg(k, bits_c), limit = kept_reg(k, limit_c);
    cbz_x(e, column, slow);
    cbz_x(e, bits, slow);
    unsigned at = value_index(k, height - 1, X1);
    int walk = k->vs[height - 1].walk;
    if (walk < 0 || k->loops[walk].list != list) {
        unsigned count;
        if (count_c >= 0) count = kept_reg(k, count_c);
        else {
            load64(e, X16, X24, list * sizeof(PureList *));
            load64(e, X17, X16, pure_list_layout[LIST_COUNT_AT]);
            count = X17;
        }
        cmp_reg(e, at, count);
        b_cond(e, HS, fault_at(k, PURE_FAULT_NOT_FOUND, pc));
    }
    tbz(e, column, 58, other);
    load32_index(e, X9, column, at);
    bind(e, key);
    cmp_reg(e, X9, limit);
    b_cond(e, HS, absent);
    lsr_imm(e, X10, X9, 6);
    load64_index(e, X10, bits, X10);
    lsr_reg(e, r, X10, X9);
    b(e, done);
    bind(e, other); /* eight bytes: a key when whole; one byte: one */
    tbz(e, column, 59, one);
    load_d_index(e, D0, column, at);
    fp2(e, FCVTZU_X, X9, D0);
    fp2(e, UCVTF_X, D1, X9);
    fcmp(e, D0, D1);
    b_cond(e, EQ, key);
    b(e, absent);
    bind(e, one);
    load8_index(e, X9, column, at);
    b(e, key);
    bind(e, absent);
    mov_w(e, r, 0);
    bind(e, done);
    k->vs[height - 1] = (Value){V_BIT, (uint8_t)r, 0, false, false, -1, 0, -1};
    slow_back(k, s, V_BIT, r);
    return true;
}

static void loop_open(Block *k, uint32_t pc, unsigned height);
static void loop_ends(Block *k, uint32_t pc);

static void compile_op(Block *k, uint32_t pc) {
    Emit *e = k->e;
    const PureModule *m = k->m;
    const PureOp *op = &m->code[pc];
    unsigned height = k->height[pc - k->block.start];
    uint32_t arg = op->arg;
    int loop = k->loop_of[pc - k->block.start];
    Value *vs = k->vs;
    switch (op->op) {
    case PURE_OP_CONST:
        vs[height] = (Value){V_CONST, 0, 0, false, false, -1, (uint64_t)op->integer, -1}; /* the bits: a number's, a bool's or an i64's */
        return;
    case PURE_OP_LOAD: {
        unsigned r = alloc_d(k, height);
        load_d(e, r, X21, arg * sizeof(PureValue));
        vs[height] = (Value){V_D, (uint8_t)r, 0, false, false, -1, 0, -1};
        return;
    }
    case PURE_OP_STORE: case PURE_OP_CUR_STORE:
        if (vs[height - 1].kind == V_D) store_d(e, vs[height - 1].reg, X21, arg * sizeof(PureValue));
        else store64(e, value_bits(k, height - 1, X9), X21, arg * sizeof(PureValue));
        return;
    case PURE_OP_LOCAL_LOAD: {
        unsigned r = alloc_d(k, height);
        load_d(e, r, X28, (arg & 0xFF) * sizeof(PureValue));
        vs[height] = (Value){V_D, (uint8_t)r, 0, false, false, (int16_t)(arg >> 16), 0, -1};
        return;
    }
    case PURE_OP_LOCAL_STORE:
        if (vs[height - 1].kind == V_D) store_d(e, vs[height - 1].reg, X28, (arg & 0xFF) * sizeof(PureValue));
        else store64(e, value_bits(k, height - 1, X9), X28, (arg & 0xFF) * sizeof(PureValue));
        return;
    case PURE_OP_ROW: case PURE_OP_OUTER_ROW: {
        int walk = -1;
        for (int l = loop; op->op == PURE_OP_ROW && l >= 0 && walk < 0; l = k->loops[l].parent)
            if (k->loops[l].walk) walk = l;
        int c = walk >= 0 && k->loops[walk].fast ? kept_find(k, loop, C_AT, (unsigned)walk, 0) : -1;
        if (c >= 0) {
            vs[height] = (Value){V_X, (uint8_t)kept_reg(k, c), 0, true, false, (int16_t)arg, 0, (int16_t)walk};
            return;
        }
        unsigned r = alloc_x(k, height);
        load64(e, r, X22, op->op == PURE_OP_ROW ? offsetof(Event, row) : offsetof(Event, outer));
        vs[height] = (Value){V_X, (uint8_t)r, 0, false, false, (int16_t)arg, 0, -1};
        return;
    }
    case PURE_OP_INDEX_FIRST:
        vs[height] = (Value){V_CONST, 0, 0, false, false, (int16_t)arg, 0, -1};
        return;
    case PURE_OP_LIST_COUNT: {
        int c = kept_find(k, loop, C_COUNT, arg, 0);
        if (c >= 0) {
            vs[height] = (Value){V_X, (uint8_t)kept_reg(k, c), 0, true, false, -1, 0, -1};
            return;
        }
        unsigned r = alloc_x(k, height);
        load64(e, X16, X24, arg * sizeof(PureList *));
        load64(e, r, X16, pure_list_layout[LIST_COUNT_AT]);
        vs[height] = (Value){V_X, (uint8_t)r, 0, false, false, -1, 0, -1};
        return;
    }
    case PURE_OP_NOT: {
        Value *v = &vs[height - 1];
        if (v->kind != V_FLAG && v->kind != V_BIT && v->kind != V_CONST) unflag_below(k, height - 1);
        if (v->kind == V_FLAG) v->cond ^= 1;
        else if (v->kind == V_BIT) v->negate = !v->negate;
        else if (v->kind == V_CONST) {
            double x = num_of(v->bits), one = 1, zero = 0;
            memcpy(&v->bits, x == 0 ? &one : &zero, sizeof(v->bits));
        } else if (v->kind == V_X) {
            cmp_imm(e, v->reg, 0);
            *v = (Value){V_FLAG, 0, EQ, false, false, -1, 0, -1};
        } else {
            fcmp_zero(e, value_d(k, height - 1, D0));
            *v = (Value){V_FLAG, 0, EQ, false, false, -1, 0, -1};
        }
        v->owner = -1;
        return;
    }
    case PURE_OP_NEG: case PURE_OP_FLOOR: {
        unsigned r = alloc_d(k, height - 1), x = value_d(k, height - 1, D0);
        fp2(e, op->op == PURE_OP_NEG ? FNEG : FRINTM, r, x);
        vs[height - 1] = (Value){V_D, (uint8_t)r, 0, false, false, -1, 0, -1};
        return;
    }
    case PURE_OP_ADD: case PURE_OP_SUB: case PURE_OP_MUL: case PURE_OP_DIV: case PURE_OP_MIN: case PURE_OP_MAX:
    case PURE_OP_LT: case PURE_OP_LE: case PURE_OP_GT: case PURE_OP_GE: case PURE_OP_EQ: case PURE_OP_NE:
    case PURE_OP_AND: case PURE_OP_OR:
        binary(k, op, pc, height);
        return;
    case PURE_OP_INDEX_MOVE: { /* within its list: from 0 to its last item, by a whole number (else LIMIT) */
        unflag_below(k, height);
        int owner = vs[height - 2].owner;
        bool known = vs[height - 2].kind != V_MEM && owner >= 0;
        unsigned r = alloc_d(k, height - 2), x = value_d(k, height - 2, D0), by = value_d(k, height - 1, D1);
        fp2(e, FRINTM, D2, by);
        fcmp(e, by, D2);
        b_cond(e, NE, fault_at(k, PURE_FAULT_LIMIT, pc));
        if (known) load64(e, X10, X24, (size_t)owner * sizeof(PureList *));
        else {
            load16(e, X10, X20, AT(height - 2) + OWNER);
            load64_index(e, X10, X24, X10);
        }
        load64(e, X10, X10, pure_list_layout[LIST_COUNT_AT]);
        sub_imm(e, X9, X10, 1);
        cmp_imm(e, X10, 0);
        csel(e, X9, XZR, X9, EQ);
        fp2(e, UCVTF_X, D3, X9);
        fp3(e, FADD, r, x, by);
        fp3(e, FMIN, r, r, D3);
        fp2(e, FMOV_DX, D4, XZR);
        fp3(e, FMAX, r, D4, r);
        vs[height - 2] = (Value){V_D, (uint8_t)r, 0, false, false, (int16_t)(known ? owner : -1), 0, -1};
        if (!known) spill(k, height - 2); /* the owner only its slot knows: kept there */
        return;
    }
    case PURE_OP_LIST_HAS: {
        if (op->plan & PLAN_TEXT) break;
        int bits_c = kept_find(k, loop, C_BITS, arg, 0), limit_c = kept_find(k, loop, C_LIMIT, arg, 0);
        unflag_below(k, height);
        spill_all(k, height - 1);
        size_t s = (size_t)(slow_at(k, pc, (uintptr_t)pure_run_step, height, height - 1, (int)height - 1) - k->slows);
        if (e->failed) return;
        int slow = k->slows[s].label, absent = new_label(e), done = new_label(e);
        unsigned bits, limit, r = alloc_x(k, height);
        if (bits_c >= 0 && limit_c >= 0) bits = kept_reg(k, bits_c), limit = kept_reg(k, limit_c);
        else {
            load64(e, X16, X24, arg * sizeof(PureList *));
            load8(e, X17, X16, pure_list_layout[LIST_BITMAP_AT]);
            cbz_w(e, X17, slow);
            load64(e, X0, X16, pure_list_layout[LIST_MEMBERS_AT]);
            load64(e, X1, X16, pure_list_layout[LIST_MEMBER_CAPACITY_AT]);
            lsl_imm(e, X1, X1, 6);
            bits = X0, limit = X1;
        }
        cbz_x(e, bits, slow); /* no bits (a table, or none): the step */
        key_bit(k, height - 1, bits, limit, absent, r);
        b(e, done);
        bind(e, absent);
        mov_w(e, r, 0);
        bind(e, done);
        vs[height - 1] = (Value){V_BIT, (uint8_t)r, 0, false, false, -1, 0, -1};
        slow_back(k, s, V_BIT, r);
        return;
    }
    case PURE_OP_LIST_FIND: { /* joined to its ITEM_GET: a DENSE list's field at its key */
        const PureOp *get = &m->code[pc + 1];
        if (!(op->plan & PLAN_JOINED) || op->plan & PLAN_TEXT || get->plan & PLAN_TEXT) break;
        unsigned field = item_field(get->arg);
        int col_c = kept_find(k, loop, C_DENSE, arg, field), bits_c = kept_find(k, loop, C_BITS, arg, 0),
            limit_c = kept_find(k, loop, C_LIMIT, arg, 0);
        unflag_below(k, height);
        spill_all(k, height - 1);
        size_t s = (size_t)(slow_at(k, pc, (uintptr_t)pure_run_step, height, height - 1, (int)height - 1) - k->slows);
        if (e->failed) return;
        int slow = k->slows[s].label;
        unsigned column, bits, limit, r = alloc_d(k, height);
        if (col_c >= 0 && bits_c >= 0 && limit_c >= 0) column = kept_reg(k, col_c), bits = kept_reg(k, bits_c), limit = kept_reg(k, limit_c);
        else {
            load64(e, X16, X24, arg * sizeof(PureList *));
            load8(e, X17, X16, pure_list_layout[LIST_FORM_AT]);
            cmp_w_imm(e, X17, DENSE_FORM);
            b_cond(e, NE, slow);
            load64(e, X17, X16, pure_list_layout[LIST_COLUMNS_AT] + field * sizeof(void *));
            load8(e, X10, X16, pure_list_layout[LIST_WIDTH_AT] + field);
            orr_lsl(e, X17, X17, X10, 56);
            load64(e, X0, X16, pure_list_layout[LIST_MEMBERS_AT]);
            load64(e, X1, X16, pure_list_layout[LIST_MEMBER_CAPACITY_AT]);
            lsl_imm(e, X1, X1, 6);
            column = X17, bits = X0, limit = X1;
        }
        cbz_x(e, column, slow);
        cbz_x(e, bits, slow);
        unsigned key = value_key(k, height - 1, X9, slow);
        cmp_reg(e, key, limit);
        b_cond(e, HS, fault_at(k, PURE_FAULT_NOT_FOUND, pc));
        lsr_imm(e, X10, key, 6);
        load64_index(e, X10, bits, X10);
        lsr_reg(e, X10, X10, key);
        tbz(e, X10, 0, fault_at(k, PURE_FAULT_NOT_FOUND, pc));
        load_cell(k, r, column, key);
        vs[height - 1] = (Value){V_D, (uint8_t)r, 0, false, false, -1, 0, -1};
        slow_back(k, s, V_D, r);
        return;
    }
    case PURE_OP_ITEM_GET: { /* a FLAT list's cell by its column, a tree's from the run kept */
        if (op->plan & PLAN_TEXT) break;
        if (item_has(k, pc, height)) {
            k->joined = true;
            return;
        }
        uint32_t list = list_of(arg), field = item_field(arg);
        int col_c = kept_find(k, loop, C_COLUMN, list, field), count_c = kept_find(k, loop, C_COUNT, list, 0);
        unflag_below(k, height);
        spill_all(k, height - 1);
        size_t s = (size_t)(slow_at(k, pc, (uintptr_t)pure_run_step, height, height - 1, (int)height - 1) - k->slows);
        if (e->failed) return;
        int slow = k->slows[s].label;
        unsigned r = alloc_d(k, height);
        if (col_c >= 0) {
            unsigned column = kept_reg(k, col_c);
            k->slows[s].tree_list = (int)list, k->slows[s].tree_field = field;
            cbz_x(e, column, slow);
            unsigned at = value_index(k, height - 1, X9), count;
            int walk = vs[height - 1].walk;
            if (walk < 0 || k->loops[walk].list != list) { /* else its place is below its end, which is at most the count */
                if (count_c >= 0) count = kept_reg(k, count_c);
                else {
                    load64(e, X16, X24, list * sizeof(PureList *));
                    load64(e, X17, X16, pure_list_layout[LIST_COUNT_AT]);
                    count = X17;
                }
                cmp_reg(e, at, count);
                b_cond(e, HS, fault_at(k, PURE_FAULT_NOT_FOUND, pc));
            }
            load_cell(k, r, column, at);
        } else { /* FLAT: its column; a tree: the run kept, or the step keeps the item's run (pure_run_item) */
            int flat = new_label(e), done = new_label(e);
            size_t fill = (size_t)(slow_at(k, pc, (uintptr_t)pure_run_item, height, height - 1, (int)height - 1) - k->slows);
            if (e->failed) return;
            int refill = k->slows[fill].label;
            load64(e, X16, X24, list * sizeof(PureList *));
            load8(e, X17, X16, pure_list_layout[LIST_FORM_AT]);
            unsigned at = value_index(k, height - 1, X9);
            cmp_w_imm(e, X17, FLAT_FORM);
            b_cond(e, EQ, flat);
            cbnz_w(e, X17, slow); /* DENSE */
            load64(e, X10, X19, offsetof(Frame, run.list));
            cmp_reg(e, X10, X16);
            b_cond(e, NE, refill);
            load64(e, X10, X19, offsetof(Frame, run.version));
            load64(e, X17, X16, pure_list_layout[LIST_VERSION_AT]);
            cmp_reg(e, X10, X17);
            b_cond(e, NE, refill);
            load64(e, X10, X19, offsetof(Frame, run.start));
            sub_reg(e, X0, at, X10);
            load64(e, X10, X19, offsetof(Frame, run.count));
            cmp_reg(e, X0, X10);
            b_cond(e, HS, refill);
            load64(e, X17, X19, offsetof(Frame, run.columns) + field * sizeof(void *));
            load_d_index(e, r, X17, X0);
            b(e, done);
            bind(e, flat);
            load64(e, X10, X16, pure_list_layout[LIST_COUNT_AT]);
            cmp_reg(e, at, X10);
            b_cond(e, HS, fault_at(k, PURE_FAULT_NOT_FOUND, pc));
            load64(e, X17, X16, pure_list_layout[LIST_COLUMNS_AT] + field * sizeof(void *));
            load8(e, X10, X16, pure_list_layout[LIST_WIDTH_AT] + field);
            orr_lsl(e, X17, X17, X10, 56);
            load_cell(k, r, X17, at);
            bind(e, done);
            vs[height - 1] = (Value){V_D, (uint8_t)r, 0, false, false, -1, 0, -1};
            slow_back(k, fill, V_D, r);
            k->slows[s].back = k->slows[fill].back; /* both steps come back here */
            k->slows[s].result_kind = V_D, k->slows[s].result_reg = (uint8_t)r, k->slows[s].fresh = k->fresh;
            return;
        }
        vs[height - 1] = (Value){V_D, (uint8_t)r, 0, false, false, -1, 0, -1};
        slow_back(k, s, V_D, r);
        return;
    }
    case PURE_OP_LIST_APPEND:
        if (!event_list(m, arg) || !numbers_only(m, arg) || m->state_indexed[arg]) break;
        append(k, op, pc, height);
        return;
    case PURE_OP_JZ: {
        int target_loop, target = landing(k, pc, arg, &target_loop);
        Value v = vs[height - 1];
        uint64_t need = kept_at(k, target_loop) & ~k->fresh;
        if (v.kind == V_FLAG && need) { /* reading what is kept changes the flags */
            unflag(k, height - 1, height);
            v = vs[height - 1];
        }
        spill_all(k, height - 1);
        refresh(k, kept_at(k, target_loop));
        switch (v.kind) {
        case V_FLAG: b_cond(e, v.cond ^ 1, target); break;
        case V_BIT: if (v.negate) tbnz(e, v.reg, 0, target); else tbz(e, v.reg, 0, target); break;
        case V_X: cbz_x(e, v.reg, target); break;
        case V_CONST:
            if (num_of(v.bits) == 0) b(e, target);
            break;
        default: fcmp_zero(e, value_d(k, height - 1, D0)); b_cond(e, EQ, target); break;
        }
        vs[height - 1] = MEMORY;
        return;
    }
    case PURE_OP_JMP: {
        int target_loop, target = landing(k, pc, arg, &target_loop);
        join(k, height, target_loop);
        b(e, target);
        return;
    }
    case PURE_OP_EACH: case PURE_OP_LOOP:
        loop_open(k, pc, height);
        return;
    case PURE_OP_BREAK: {
        int l = loop;
        spill_all(k, height);
        write_back(k, loop);
        mov_reg(e, X0, X19);
        call(e, (uintptr_t)pure_run_break);
        k->fresh = 0;
        refresh(k, kept_at(k, k->loops[l].parent));
        b(e, k->loops[l].after);
        return;
    }
    default: break;
    }
    call_step(k, (uintptr_t)pure_run_step, pc, height);
}

/* ---- Loops */

/* In reg, a whole key: the first position of the packed list (in x16, its
 * pack in x17, its keys column key) whose key is not below it, as
 * pack_seek finds it in integers (module-list.c). x9-x13 and d1, d2 are
 * spent; the stack holds nothing in registers here. */
static void bound(Block *k, unsigned reg, unsigned key) {
    Emit *e = k->e;
    int zero = new_label(e), all = new_label(e), done = new_label(e), found = new_label(e), scan = new_label(e);
    load64(e, X9, X17, pure_pack_layout[PACK_LOW_AT]);
    cmp_reg(e, reg, X9);
    b_cond(e, LE, zero);
    load64(e, X9, X17, pure_pack_layout[PACK_HIGH_AT]);
    cmp_reg(e, reg, X9);
    b_cond(e, GT, all);
    load64(e, X9, X17, pure_pack_layout[PACK_ORIGIN_AT]);
    sub_reg(e, X10, reg, X9);                 /* from the first bucket's start */
    load32(e, X9, X17, pure_pack_layout[PACK_SHIFT_AT]);
    lsr_reg(e, X11, X10, X9);                 /* its bucket */
    lsl_reg(e, X12, X11, X9);
    cmp_reg(e, X12, X10);                     /* at the bucket's start? */
    load64(e, X9, X17, pure_pack_layout[PACK_FIRST_AT]);
    load32_index(e, X13, X9, X11);            /* the bucket's first position */
    b_cond(e, EQ, found);
    add_imm(e, X11, X11, 1);
    load32_index(e, X12, X9, X11);            /* where the bucket ends */
    load64(e, X9, X16, pure_list_layout[LIST_COLUMNS_AT] + key * sizeof(void *));
    fp2(e, SCVTF_X, D1, reg);
    bind(e, scan);                            /* on while the keys are below it */
    cmp_reg(e, X13, X12);
    b_cond(e, HS, found);
    load_d_index(e, D2, X9, X13);
    fcmp(e, D2, D1);
    b_cond(e, PL, found);
    add_imm(e, X13, X13, 1);
    b(e, scan);
    bind(e, found);
    mov_reg(e, reg, X13);
    b(e, done);
    bind(e, zero);
    mov_w(e, reg, 0);
    b(e, done);
    bind(e, all);
    load64(e, reg, X16, pure_list_layout[LIST_COUNT_AT]);
    bind(e, done);
}

/* EACH over a slice of a stored list at pc, opened in code as enter_each
 * opens it (module.c) when the list is packed with whole keys and the
 * slice's bounds are whole numbers, its place and end kept from there;
 * else to slow (the step). */
static void open_slice(Block *k, uint32_t pc, unsigned height, LoopAt *l, int slow) {
    Emit *e = k->e;
    const PureModule *m = k->m;
    const PureOp *op = &m->code[pc];
    unsigned li = (unsigned)(l - k->loops), length = op->arg >> 16, key = m->state_key[l->list] - 1u;
    int skip = new_label(e), body = l->start < l->end ? k->op_label[l->start - k->block.start] : l->again;
    uint64_t fresh = k->fresh;
    load64(e, X16, X24, l->list * sizeof(PureList *));
    load64(e, X17, X16, pure_list_layout[LIST_PACK_AT]);
    cbz_x(e, X17, slow);
    load8(e, X9, X17, pure_pack_layout[PACK_WHOLE_AT]);
    cbz_w(e, X9, slow);
    for (unsigned side = 0; side < 2; ++side) { /* the bounds as whole numbers, in x0 and x1 */
        unsigned h = height - 2 + side, reg = side ? X1 : X0;
        if (k->vs[h].kind == V_X) mov_reg(e, reg, k->vs[h].reg);
        else {
            unsigned d = value_d(k, h, side ? D2 : D0);
            fp2(e, FCVTZS_X, reg, d);
            fp2(e, SCVTF_X, D3, reg);
            fcmp(e, d, D3);
            b_cond(e, NE, slow);
        }
    }
    bound(k, X0, key);
    bound(k, X1, key);
    cmp_reg(e, X1, X0); /* an end below the start: none */
    csel(e, X1, X1, X0, HS);
    /* its work: its block's length times the items it starts with, past the budget a fault at the EACH */
    load64(e, X9, X16, pure_list_layout[LIST_COUNT_AT]);
    cmp_reg(e, X1, X9);
    csel(e, X10, X1, X9, LO);
    sub_reg(e, X10, X10, X0);
    mov_imm(e, X9, length);
    mul_reg(e, X10, X10, X9);
    int budgeted = new_label(e);
    load64(e, X9, X23, offsetof(PureApp, work_budget));
    cbnz_x(e, X9, budgeted);
    mov_imm(e, X9, PURE_WORK_DEFAULT);
    bind(e, budgeted);
    load64(e, X17, X23, offsetof(PureApp, work));
    sub_reg(e, X9, X9, X17);
    cmp_reg(e, X10, X9);
    b_cond(e, HI, fault_at(k, PURE_FAULT_WORK, pc));
    add_reg(e, X17, X17, X10);
    store64(e, X17, X23, offsetof(PureApp, work));
    cmp_reg(e, X0, X1); /* none: past its block */
    b_cond(e, HS, skip);
    load64(e, X9, X16, pure_list_layout[LIST_COUNT_AT]);
    cmp_reg(e, X0, X9);
    b_cond(e, HS, skip);
    size_t at = loop_offset(l);
    add_imm(e, X9, X27, (uint32_t)at);
    mov_w(e, X10, 1);
    store8(e, X10, X9, offsetof(Loop, walking));
    mov_w(e, X10, l->list);
    store_w(e, X10, X9, offsetof(Loop, walk) + offsetof(Walk, list));
    store64(e, X0, X9, offsetof(Loop, walk) + offsetof(Walk, at));
    store64(e, X1, X9, offsetof(Loop, walk) + offsetof(Walk, end));
    store8(e, XZR, X9, offsetof(Loop, walk) + offsetof(Walk, removed));
    mov_w(e, X10, l->start);
    store_w(e, X10, X9, offsetof(Loop, start));
    mov_w(e, X10, l->end);
    store_w(e, X10, X9, offsetof(Loop, end));
    load64(e, X10, X19, offsetof(Frame, arena));
    load64(e, X10, X10, offsetof(PureArena, used));
    store64(e, X10, X9, offsetof(Loop, mark));
    load64(e, X10, X22, offsetof(Event, walk));
    store64(e, X10, X9, offsetof(Loop, around));
    load64(e, X10, X22, offsetof(Event, row));
    store64(e, X10, X9, offsetof(Loop, row));
    add_imm(e, X10, X9, offsetof(Loop, walk));
    store64(e, X10, X22, offsetof(Event, walk));
    store64(e, X0, X22, offsetof(Event, row));
    load32(e, X10, X19, offsetof(Frame, end));
    store_w(e, X10, X9, offsetof(Loop, outer));
    mov_w(e, X10, l->end);
    store_w(e, X10, X19, offsetof(Frame, end));
    load32(e, X10, X19, offsetof(Frame, depth));
    add_imm(e, X10, X10, 1);
    store_w(e, X10, X19, offsetof(Frame, depth));
    if (l->fast) { /* where it is and where it stops, kept */
        mov_reg(e, k->kept[l->at_kept].reg, X0);
        load64(e, X10, X16, pure_list_layout[LIST_COUNT_AT]);
        cmp_reg(e, X1, X10);
        csel(e, k->kept[l->end_kept].reg, X1, X10, LO);
        k->fresh |= UINT64_C(1) << l->at_kept | UINT64_C(1) << l->end_kept;
    }
    refresh(k, kept_at(k, (int)li));
    b(e, body);
    bind(e, skip);
    k->fresh = fresh;
    refresh(k, kept_at(k, l->parent));
    b(e, l->after);
    k->fresh = fresh;
}

/* EACH or LOOP at pc: opened by its step (its block next, or none: past it),
 * what its block keeps read; a packed list's slice in code (open_slice). */
static void loop_open(Block *k, uint32_t pc, unsigned height) {
    Emit *e = k->e;
    const PureOp *op = &k->m->code[pc];
    LoopAt *l = &k->loops[k->opens[pc - k->block.start]];
    unflag_below(k, height);
    if (op->op == PURE_OP_EACH && op->arg & PURE_EACH_SLICED && !event_list(k->m, l->list)) {
        int slow = new_label(e);
        spill_all(k, height - 2);
        Value saved[2] = {k->vs[height - 2], k->vs[height - 1]};
        open_slice(k, pc, height, l, slow);
        bind(e, slow);
        k->vs[height - 2] = saved[0], k->vs[height - 1] = saved[1];
    }
    spill_all(k, height);
    write_back(k, k->loop_of[pc - k->block.start]);
    set_sp(k, height);
    mov_reg(e, X0, X19);
    mov_w(e, X1, pc);
    call(e, op->op == PURE_OP_EACH ? (uintptr_t)pure_run_each : (uintptr_t)pure_run_loop);
    k->fresh = 0;
    for (unsigned h = 0; h <= PURE_MODULE_STACK; ++h) k->vs[h] = MEMORY;
    int open = new_label(e);
    cbz_w(e, X0, open);
    cmp_w_imm(e, X0, STEP_SKIP);
    b_cond(e, NE, k->exit);
    refresh(k, kept_at(k, l->parent)); /* none: past its block, where its iteration would end */
    b(e, l->after);
    k->fresh = 0;
    bind(e, open);
    if (l->start < l->end) { /* what its block keeps, read: then its block, as the code above goes there too */
        refresh(k, kept_at(k, k->opens[pc - k->block.start]));
        b(e, k->op_label[l->start - k->block.start]);
    }
}

/* A loop's block ran once (iterate, module.c, as code): again (its block's
 * first instruction, an empty block's being this), or the loop closed. */
static void iteration(Block *k, LoopAt *l, unsigned li) {
    Emit *e = k->e;
    const PureOp *op = &k->m->code[l->at];
    int again = l->start == l->end ? l->again : k->op_label[l->start - k->block.start], done = new_label(e);
    size_t at = loop_offset(l);
    (void)li;
    if (l->arena) { /* the arena as the block began */
        load64(e, X10, X27, at + offsetof(Loop, mark));
        load64(e, X9, X19, offsetof(Frame, arena));
        store64(e, X10, X9, offsetof(PureArena, used));
    }
    if (op->op == PURE_OP_EACH && l->fast) { /* its list does not change in it: where it is and where it stops kept */
        unsigned place = kept_reg(k, l->at_kept), end = kept_reg(k, l->end_kept);
        add_imm(e, place, place, 1);
        cmp_reg(e, place, end);
        b_cond(e, LO, again);
        bind(e, done);
        load64(e, X10, X27, at + offsetof(Loop, around));
        store64(e, X10, X22, offsetof(Event, walk));
        load64(e, X10, X27, at + offsetof(Loop, row));
        store64(e, X10, X22, offsetof(Event, row));
    } else if (op->op == PURE_OP_EACH) {
        int counted = new_label(e);
        add_imm(e, X9, X27, (uint32_t)at);
        load8(e, X10, X9, offsetof(Loop, walk) + offsetof(Walk, removed));
        load64(e, X17, X9, offsetof(Loop, walk) + offsetof(Walk, at));
        cbnz_w(e, X10, counted);
        add_imm(e, X17, X17, 1);
        bind(e, counted);
        store8(e, XZR, X9, offsetof(Loop, walk) + offsetof(Walk, removed));
        store64(e, X17, X9, offsetof(Loop, walk) + offsetof(Walk, at));
        load64(e, X10, X9, offsetof(Loop, walk) + offsetof(Walk, end));
        cmp_reg(e, X17, X10);
        b_cond(e, HS, done);
        load64(e, X10, X24, (op->arg >> 8 & 0xFF) * sizeof(PureList *)); /* the list walked: at most its items now */
        load64(e, X10, X10, pure_list_layout[LIST_COUNT_AT]);
        cmp_reg(e, X17, X10);
        b_cond(e, HS, done);
        store64(e, X17, X22, offsetof(Event, row));
        b(e, again);
        bind(e, done);
        load64(e, X10, X9, offsetof(Loop, around));
        store64(e, X10, X22, offsetof(Event, walk));
        load64(e, X10, X9, offsetof(Loop, row));
        store64(e, X10, X22, offsetof(Event, row));
    } else {
        unsigned round = kept_reg(k, l->k_kept), count = kept_reg(k, l->n_kept);
        add_imm(e, round, round, 1);
        cmp_reg(e, round, count);
        b_cond(e, HS, done);
        store64(e, round, X27, at + offsetof(Loop, k));
        /* its work counts each time round: its block and the LOOP, past the budget a fault at the LOOP */
        uint64_t cost = (uint64_t)(l->end - l->start) + 1;
        int budgeted = new_label(e);
        load64(e, X10, X23, offsetof(PureApp, work_budget));
        cbnz_x(e, X10, budgeted);
        mov_imm(e, X10, PURE_WORK_DEFAULT);
        bind(e, budgeted);
        load64(e, X17, X23, offsetof(PureApp, work));
        sub_reg(e, X9, X10, X17);
        mov_imm(e, X0, cost);
        cmp_reg(e, X9, X0);
        b_cond(e, LO, fault_at(k, PURE_FAULT_WORK, l->at));
        add_reg(e, X17, X17, X0);
        store64(e, X17, X23, offsetof(PureApp, work));
        size_t slot = (op->arg & 0xFF) * sizeof(PureValue); /* the slot: low + k */
        if (op->arg & PURE_LOOP_INTEGER) {
            load64(e, X10, X27, at + offsetof(Loop, low));
            add_reg(e, X10, X10, round);
            store64(e, X10, X28, slot);
        } else {
            load_d(e, D0, X27, at + offsetof(Loop, low));
            fp2(e, UCVTF_X, D1, round);
            fp3(e, FADD, D0, D0, D1);
            store_d(e, D0, X28, slot);
        }
        b(e, again);
        bind(e, done);
        k->fresh = kept_at(k, (int)(l - k->loops));
    }
    load32(e, X10, X27, at + offsetof(Loop, outer)); /* closed */
    store_w(e, X10, X19, offsetof(Frame, end));
    load32(e, X10, X19, offsetof(Frame, depth));
    sub_w_imm(e, X10, X10, 1);
    store_w(e, X10, X19, offsetof(Frame, depth));
}

/* The iterations of the loops ending at pc, innermost first. */
static void loop_ends(Block *k, uint32_t pc) {
    Emit *e = k->e;
    for (uint64_t done = 0;;) { /* innermost first: the loop ending at pc nested deepest among those not done */
        int pick = -1, deepest = -1;
        for (unsigned l = 0; l < k->loop_count; ++l) {
            if (k->loops[l].end != pc || (l < 64 && done >> l & 1)) continue;
            if ((int)k->loops[l].depth > deepest) deepest = (int)k->loops[l].depth, pick = (int)l;
        }
        if (pick < 0) break;
        done |= UINT64_C(1) << pick;
        LoopAt *l = &k->loops[pick];
        join(k, l->base, pick);
        bind(e, l->again);
        iteration(k, l, (unsigned)pick);
        join(k, l->base, l->parent);
        bind(e, l->after);
    }
}

/* What may change a list: the lists an instruction changes (every bit: any it may). */
static uint32_t changes(const PureModule *m, const PureOp *op) {
    switch (op->op) {
    case PURE_OP_LIST_APPEND: case PURE_OP_LIST_INSERT: case PURE_OP_LIST_REMOVE: return UINT32_C(1) << (op->arg & 31);
    case PURE_OP_ITEM_SET: return UINT32_C(1) << (list_of(op->arg) & 31);
    case PURE_OP_CONST: case PURE_OP_LOAD: case PURE_OP_DT: case PURE_OP_ADD: case PURE_OP_SUB: case PURE_OP_MUL:
    case PURE_OP_DIV: case PURE_OP_NEG: case PURE_OP_LT: case PURE_OP_LE: case PURE_OP_GT: case PURE_OP_GE: case PURE_OP_EQ:
    case PURE_OP_NE: case PURE_OP_AND: case PURE_OP_OR: case PURE_OP_NOT: case PURE_OP_MIN: case PURE_OP_MAX: case PURE_OP_CLAMP:
    case PURE_OP_STORE: case PURE_OP_JZ: case PURE_OP_JMP: case PURE_OP_FIELD: case PURE_OP_LIST_COUNT: case PURE_OP_ITEM_GET:
    case PURE_OP_INDEX_LOAD: case PURE_OP_INDEX_STORE: case PURE_OP_INDEX_MOVE: case PURE_OP_INDEX_FIRST: case PURE_OP_INDEX_LAST:
    case PURE_OP_INDEX_NUM: case PURE_OP_ROW: case PURE_OP_FOLD: case PURE_OP_LIST_FIND: case PURE_OP_LIST_HAS:
    case PURE_OP_NOW: case PURE_OP_FLOOR: case PURE_OP_MOD: case PURE_OP_OUTER_ROW: case PURE_OP_EACH: case PURE_OP_LOCAL_LOAD:
    case PURE_OP_LOCAL_STORE: case PURE_OP_I64_ADD: case PURE_OP_I64_SUB: case PURE_OP_I64_MUL: case PURE_OP_I64_DIV:
    case PURE_OP_I64_MOD: case PURE_OP_I64_NEG: case PURE_OP_I64_COMPARE: case PURE_OP_I64_MIN: case PURE_OP_I64_MAX:
    case PURE_OP_I64_FROM: case PURE_OP_NUM_FROM: case PURE_OP_LOOP: case PURE_OP_BREAK:
        (void)m;
        return 0;
    default: return UINT32_MAX;
    }
}

/* Whether an instruction may leave bytes in the arena (a text's): its loop gives them back each round. */
static bool arena_kept(const PureOp *op) {
    switch (op->op) {
    case PURE_OP_CONST: case PURE_OP_LOAD: case PURE_OP_ADD: case PURE_OP_SUB: case PURE_OP_MUL: case PURE_OP_DIV:
    case PURE_OP_NEG: case PURE_OP_LT: case PURE_OP_LE: case PURE_OP_GT: case PURE_OP_GE: case PURE_OP_EQ: case PURE_OP_NE:
    case PURE_OP_AND: case PURE_OP_OR: case PURE_OP_NOT: case PURE_OP_MIN: case PURE_OP_MAX: case PURE_OP_STORE: case PURE_OP_JZ:
    case PURE_OP_JMP: case PURE_OP_LIST_COUNT: case PURE_OP_INDEX_MOVE: case PURE_OP_INDEX_FIRST: case PURE_OP_ROW:
    case PURE_OP_FLOOR: case PURE_OP_OUTER_ROW: case PURE_OP_EACH: case PURE_OP_LOCAL_LOAD: case PURE_OP_LOCAL_STORE:
    case PURE_OP_LOOP: case PURE_OP_BREAK:
        return false;
    case PURE_OP_ITEM_GET: case PURE_OP_LIST_HAS: case PURE_OP_LIST_FIND: case PURE_OP_LIST_APPEND:
        return op->plan & PLAN_TEXT;
    default: return true;
    }
}

/* What each nest of loops keeps, each in a register of its own: every
 * loop's state first, then what the instructions read of the lists, the
 * innermost loops' first, while registers last. A value is the nest's:
 * every loop in it finds it. */
static void plan_kept(Block *k) {
    const PureModule *m = k->m;
    unsigned *used = calloc(k->loop_count + 1, sizeof(*used));
    if (!used) {
        k->e->failed = true;
        return;
    }
#define KEEP(nest_, kind_, list_, field_)                                                                                \
    do {                                                                                                                \
        if (kept_find(k, nest_, kind_, list_, field_) >= 0 || k->kept_count == KEPT_MOST ||                             \
            used[nest_] == sizeof(kept_registers)) break;                                                               \
        k->kept[k->kept_count++] = (Kept){kind_, (uint8_t)(field_), (uint16_t)(list_), kept_registers[used[nest_]++], nest_}; \
    } while (0)
    for (unsigned pass = 0; pass < 2; ++pass)
        for (unsigned depth = PURE_MODULE_NESTING; depth > 0; --depth)
            for (unsigned li = 0; li < k->loop_count; ++li) {
                LoopAt *l = &k->loops[li];
                if (l->depth != depth) continue;
                int nest = l->nest;
                if (!pass) {
                    if (l->walk && l->fast) {
                        KEEP(nest, C_AT, li, 0);
                        KEEP(nest, C_END, li, 0);
                        l->at_kept = kept_find(k, nest, C_AT, li, 0), l->end_kept = kept_find(k, nest, C_END, li, 0);
                        if (l->at_kept < 0 || l->end_kept < 0) l->fast = false; /* no room: as the executor runs it */
                    } else if (!l->walk) {
                        KEEP(nest, C_K, li, 0);
                        KEEP(nest, C_N, li, 0);
                        l->k_kept = kept_find(k, nest, C_K, li, 0), l->n_kept = kept_find(k, nest, C_N, li, 0);
                        if (l->k_kept < 0 || l->n_kept < 0) k->e->failed = true; /* a count's round needs its registers: no code then */
                    }
                    continue;
                }
                for (uint32_t pc = l->start; pc < l->end; ++pc) {
                    if (k->loop_of[pc - k->block.start] != (int)li) continue;
                    const PureOp *op = &m->code[pc];
                    switch (op->op) {
                    case PURE_OP_ITEM_GET: {
                        if (op->plan & PLAN_TEXT) break;
                        uint32_t list = list_of(op->arg);
                        KEEP(nest, C_COLUMN, list, item_field(op->arg));
                        bool walked = false; /* a fast walk's list read at its place needs no count */
                        for (int w = (int)li; w >= 0; w = k->loops[w].parent)
                            walked = walked || (k->loops[w].walk && k->loops[w].fast && k->loops[w].list == list);
                        if (!walked && !(k->loops[nest].writes >> (list & 31) & 1)) KEEP(nest, C_COUNT, list, 0);
                        break;
                    }
                    case PURE_OP_LIST_COUNT:
                        if (!(k->loops[nest].writes >> (op->arg & 31) & 1)) KEEP(nest, C_COUNT, op->arg, 0);
                        break;
                    case PURE_OP_LIST_HAS:
                        if (op->plan & PLAN_TEXT) break;
                        KEEP(nest, C_BITS, op->arg, 0);
                        KEEP(nest, C_LIMIT, op->arg, 0);
                        break;
                    case PURE_OP_LIST_FIND:
                        if (!(op->plan & PLAN_JOINED) || op->plan & PLAN_TEXT || m->code[pc + 1].plan & PLAN_TEXT) break;
                        KEEP(nest, C_DENSE, op->arg, item_field(m->code[pc + 1].arg));
                        KEEP(nest, C_BITS, op->arg, 0);
                        KEEP(nest, C_LIMIT, op->arg, 0);
                        break;
                    default: break;
                    }
                }
            }
#undef KEEP
    for (unsigned c = 0; c < k->kept_count; ++c) k->loops[k->kept[c].loop].region |= UINT64_C(1) << c;
    for (unsigned li = 0; li < k->loop_count; ++li) k->loops[li].region = k->loops[k->loops[li].nest].region;
    free(used);
}

/* ---- Reading ahead (docs/design/022)
 * A loop that opens, in its block, a slice of a packed list from a key it
 * computes of what it reads at its round (an item of a list at a position
 * that moves with the round, then arithmetic) reads that slice's bucket
 * AHEAD_FAR rounds ahead and the first cell of the column the slice's block
 * reads AHEAD_NEAR rounds ahead: their misses overlap the rounds between
 * (software prefetch of indirect accesses, as compilers do it). A read
 * ahead changes nothing and never faults: what is out of range is not read. */

enum { AHEAD_FAR = 16, AHEAD_NEAR = 8 };

static unsigned sym_depth(const Ahead *a, int n);

static int sym(Ahead *a, Sym node) {
    if (a->count == sizeof(a->nodes) / sizeof(*a->nodes)) return -1;
    a->nodes[a->count] = node;
    return (int)a->count++;
}

static bool has_round(const Ahead *a, int n) {
    if (n < 0) return false;
    const Sym *s = &a->nodes[n];
    return s->kind == S_ROUND || (s->kind == S_ITEM && has_round(a, s->a)) ||
           (s->kind >= S_ADD && (has_round(a, s->a) || has_round(a, s->b)));
}

/* Loop li's block followed from its start to the first loop it opens: when
 * that is a slice of a stored list whose low key comes of the round, the
 * plan to read it ahead. */
static void plan_ahead(Block *k, unsigned li) {
    const PureModule *m = k->m;
    LoopAt *l = &k->loops[li];
    if (!l->walk && m->code[l->at].arg & PURE_LOOP_INTEGER) return;
    Ahead a = {0};
    int stack[PURE_MODULE_STACK + 2], local[PURE_MODULE_LOCALS];
    for (unsigned i = 0; i < PURE_MODULE_STACK + 2; ++i) stack[i] = -1;
    for (unsigned i = 0; i < PURE_MODULE_LOCALS; ++i) local[i] = -1;
    int round = sym(&a, (Sym){S_ROUND, 0, 0, -1, -1, 0});
    if (!l->walk) local[m->code[l->at].arg & 0xFF] = round;
    for (uint32_t pc = l->start; pc < l->end;) {
        const PureOp *op = &m->code[pc];
        unsigned h = k->height[pc - k->block.start];
        if (h + 1 > PURE_MODULE_STACK) return;
        switch (op->op) {
        case PURE_OP_CONST:
            if (op->arg) stack[h] = -1;
            else {
                double v;
                memcpy(&v, &op->integer, sizeof(v));
                stack[h] = sym(&a, (Sym){S_CONST, 0, 0, -1, -1, v});
            }
            break;
        case PURE_OP_LOCAL_LOAD: stack[h] = local[op->arg & 0xFF]; break;
        case PURE_OP_LOCAL_STORE: local[op->arg & 0xFF] = stack[h - 1]; break;
        case PURE_OP_ROW: stack[h] = l->walk && l->list == op->arg ? round : -1; break;
        case PURE_OP_INDEX_FIRST: stack[h] = sym(&a, (Sym){S_CONST, 0, 0, -1, -1, 0}); break;
        case PURE_OP_INDEX_MOVE: /* a position: clamped as it runs, out of range not read ahead */
            stack[h - 2] = stack[h - 2] >= 0 && stack[h - 1] >= 0 ? sym(&a, (Sym){S_ADD, 0, 0, stack[h - 2], stack[h - 1], 0}) : -1;
            break;
        case PURE_OP_ITEM_GET:
            stack[h - 1] = op->plan & PLAN_TEXT || stack[h - 1] < 0 ? -1
                         : sym(&a, (Sym){S_ITEM, (uint16_t)list_of(op->arg), (uint16_t)item_field(op->arg), stack[h - 1], -1, 0});
            break;
        case PURE_OP_ADD: case PURE_OP_SUB: case PURE_OP_MUL:
            stack[h - 2] = stack[h - 2] >= 0 && stack[h - 1] >= 0
                         ? sym(&a, (Sym){op->op == PURE_OP_ADD ? S_ADD : op->op == PURE_OP_SUB ? S_SUB : S_MUL, 0, 0, stack[h - 2],
                                         stack[h - 1], 0})
                         : -1;
            break;
        case PURE_OP_LT: case PURE_OP_LE: case PURE_OP_GT: case PURE_OP_GE: case PURE_OP_EQ: case PURE_OP_NE:
            stack[h - 2] = -1;
            break;
        case PURE_OP_LIST_COUNT: case PURE_OP_LOAD: stack[h] = -1; break;
        case PURE_OP_NOT: stack[h - 1] = -1; break;
        case PURE_OP_JZ: case PURE_OP_BREAK: break; /* the way on keeps what it had */
        case PURE_OP_EACH: {
            unsigned inner = (unsigned)k->opens[pc - k->block.start];
            const LoopAt *in = &k->loops[inner];
            if (!(op->arg & PURE_EACH_SLICED) || event_list(m, in->list) || !has_round(&a, stack[h - 2])) return;
            for (uint32_t at = in->start; at < in->end; ++at) { /* the column its block reads */
                const PureOp *get = &m->code[at];
                if (k->loop_of[at - k->block.start] != (int)inner || get->op != PURE_OP_ITEM_GET || get->plan & PLAN_TEXT ||
                    list_of(get->arg) != in->list)
                    continue;
                a.inner = inner, a.list = in->list, a.field = item_field(get->arg), a.key = stack[h - 2];
                if (sym_depth(&a, a.key) > 4) return; /* d4-d7 */
                if (!grow_array((void **)&k->aheads, &k->ahead_room, k->ahead_count + 1, sizeof(*k->aheads))) return;
                k->aheads[k->ahead_count] = a;
                l->ahead = (int)k->ahead_count++;
                return;
            }
            return;
        }
        default: return;
        }
        pc = after(op, pc);
    }
}

/* The round of loop li moved on by rounds, as a number in d (its place's or its count's value). */
static void round_ahead(Block *k, unsigned li, unsigned d, unsigned by) {
    Emit *e = k->e;
    const LoopAt *l = &k->loops[li];
    if (l->walk) fp2(e, UCVTF_X, d, k->kept[l->at_kept].reg);
    else load_d(e, d, X28, (k->m->code[l->at].arg & 0xFF) * sizeof(PureValue));
    double ahead = (double)by;
    uint64_t bits;
    memcpy(&bits, &ahead, sizeof(bits));
    mov_imm(e, X9, bits);
    fp2(e, FMOV_DX, D3, X9);
    fp3(e, FADD, d, d, D3);
}

/* The registers node n takes (d4 up). */
static unsigned sym_depth(const Ahead *a, int n) {
    const Sym *s = &a->nodes[n];
    if (s->kind == S_ITEM) return sym_depth(a, s->a);
    if (s->kind < S_ADD) return 1;
    unsigned x = sym_depth(a, s->a), y = sym_depth(a, s->b) + 1;
    return x > y ? x : y;
}

/* The key's node n, the round moved on by rounds, into d4 + depth; to skip
 * when it reads past a list's items or a list that is not FLAT. */
static void emit_key(Block *k, unsigned li, const Ahead *a, int n, unsigned depth, unsigned by, int skip) {
    Emit *e = k->e;
    const Sym *s = &a->nodes[n];
    unsigned r = D4 + depth;
    switch (s->kind) {
    case S_CONST: {
        uint64_t bits;
        memcpy(&bits, &s->value, sizeof(bits));
        mov_imm(e, X9, bits);
        fp2(e, FMOV_DX, r, X9);
        return;
    }
    case S_ROUND: round_ahead(k, li, r, by); return;
    case S_ITEM: {
        emit_key(k, li, a, s->a, depth, by, skip); /* its position */
        int column_c = kept_find(k, (int)li, C_COLUMN, s->list, s->field), count_c = kept_find(k, (int)li, C_COUNT, s->list, 0);
        if (column_c >= 0) { /* the column the loop keeps: read through its tagged address */
            unsigned column = kept_reg(k, column_c);
            fp2(e, FCVTZU_X, X0, r);
            cbz_x(e, column, skip);
            if (count_c >= 0) cmp_reg(e, X0, kept_reg(k, count_c));
            else {
                load64(e, X16, X24, (size_t)s->list * sizeof(PureList *));
                load64(e, X10, X16, pure_list_layout[LIST_COUNT_AT]);
                cmp_reg(e, X0, X10);
            }
            b_cond(e, HS, skip);
            load_cell(k, r, column, X0);
            return;
        }
        int four = new_label(e), one = new_label(e), done = new_label(e);
        fp2(e, FCVTZU_X, X0, r);
        load64(e, X16, X24, (size_t)s->list * sizeof(PureList *));
        load8(e, X17, X16, pure_list_layout[LIST_FORM_AT]);
        cmp_w_imm(e, X17, FLAT_FORM);
        b_cond(e, NE, skip);
        load64(e, X10, X16, pure_list_layout[LIST_COUNT_AT]);
        cmp_reg(e, X0, X10);
        b_cond(e, HS, skip);
        load64(e, X17, X16, pure_list_layout[LIST_COLUMNS_AT] + s->field * sizeof(void *));
        load8(e, X10, X16, pure_list_layout[LIST_WIDTH_AT] + s->field);
        cmp_w_imm(e, X10, 4);
        b_cond(e, EQ, four);
        cmp_w_imm(e, X10, 1);
        b_cond(e, EQ, one);
        load_d_index(e, r, X17, X0);
        b(e, done);
        bind(e, four);
        load32_index(e, X10, X17, X0);
        fp2(e, UCVTF_W, r, X10);
        b(e, done);
        bind(e, one);
        load8_index(e, X10, X17, X0);
        fp2(e, UCVTF_W, r, X10);
        bind(e, done);
        return;
    }
    default:
        emit_key(k, li, a, s->a, depth, by, skip);
        emit_key(k, li, a, s->b, depth + 1, by, skip);
        fp3(e, s->kind == S_ADD ? FADD : s->kind == S_SUB ? FSUB : FMUL, r, r, D4 + depth + 1);
        return;
    }
}

/* The key in d4 as the address of its bucket's first position in a's
 * packed list (x16 the list, x17 its pack, x10 the bucket, x9 the address),
 * or to skip when the list is not packed. Not checked against the index's
 * range: a read ahead may go anywhere (PRFM never faults). */
static void key_bucket(Block *k, const Ahead *a, int skip) {
    Emit *e = k->e;
    fp2(e, FCVTZS_X, X0, D4);
    load64(e, X16, X24, (size_t)a->list * sizeof(PureList *));
    load64(e, X17, X16, pure_list_layout[LIST_PACK_AT]);
    cbz_x(e, X17, skip);
    load64(e, X9, X17, pure_pack_layout[PACK_ORIGIN_AT]);
    sub_reg(e, X10, X0, X9);
    load32(e, X9, X17, pure_pack_layout[PACK_SHIFT_AT]);
    lsr_reg(e, X10, X10, X9);
    load64(e, X9, X17, pure_pack_layout[PACK_FIRST_AT]);
    add_lsl(e, X9, X9, X10, 2);
}

/* At the start of loop li's block: its slice's bucket AHEAD_FAR rounds on;
 * AHEAD_NEAR rounds on, the bucket read (ahead before) for the slice's first
 * cell of the column its block reads, and that cell read ahead. */
static void read_ahead(Block *k, unsigned li) {
    Emit *e = k->e;
    const LoopAt *l = &k->loops[li];
    const Ahead *a = &k->aheads[l->ahead];
    int near = new_label(e), done = new_label(e), column_c = kept_find(k, (int)li, C_COLUMN, a->list, a->field);
    emit_key(k, li, a, a->key, 0, AHEAD_FAR, near);
    key_bucket(k, a, near);
    prefetch(e, X9);
    bind(e, near);
    emit_key(k, li, a, a->key, 0, AHEAD_NEAR, done);
    key_bucket(k, a, done);
    load64(e, X0, X17, pure_pack_layout[PACK_BUCKETS_AT]); /* a bucket of the index: its first position is read */
    cmp_reg(e, X10, X0);
    b_cond(e, HS, done);
    load32(e, X10, X9, 0);
    if (column_c >= 0) { /* the tagged address the inner loop keeps: its top byte the cells' width */
        unsigned column = kept_reg(k, column_c);
        cbz_x(e, column, done);
        lsr_imm(e, X9, column, 56);
        mul_reg(e, X10, X10, X9);
        add_reg(e, X17, column, X10);
    } else {
        load64(e, X17, X16, pure_list_layout[LIST_COLUMNS_AT] + a->field * sizeof(void *));
        load8(e, X9, X16, pure_list_layout[LIST_WIDTH_AT] + a->field);
        mul_reg(e, X10, X10, X9);
        add_reg(e, X17, X17, X10);
    }
    prefetch(e, X17);
    bind(e, done);
}

/* The block's heights (absolute: a loop's block is verified from its own start), its loops, and each instruction's. */
static bool shape_block(Block *k) {
    const PureModule *m = k->m;
    uint32_t n = k->block.length;
    k->height = malloc(n + 1);
    k->loop_of = malloc((n + 1) * sizeof(*k->loop_of));
    k->op_label = malloc((n + 1) * sizeof(*k->op_label));
    k->loops = malloc((n + 1) * sizeof(*k->loops));
    k->opens = malloc((n + 1) * sizeof(*k->opens));
    k->target = calloc(n + 1, sizeof(*k->target));
    if (!k->height || !k->loop_of || !k->op_label || !k->loops || !k->opens || !k->target) return false;
    memset(k->height, 0, n + 1);
    for (uint32_t i = 0; i <= n; ++i) k->loop_of[i] = -1; /* an instruction joined to the one before: never compiled alone */
    int open[PURE_MODULE_NESTING + 1];
    unsigned base[PURE_MODULE_NESTING + 1], depth = 0;
    base[0] = 0;
    for (uint32_t pc = k->block.start; pc < k->block.start + n;) {
        while (depth && pc >= k->loops[open[depth - 1]].end) --depth;
        const PureOp *op = &m->code[pc];
        unsigned at = base[depth] + m->height[pc];
        k->height[pc - k->block.start] = (uint8_t)at;
        k->loop_of[pc - k->block.start] = depth ? open[depth - 1] : -1;
        if ((op->op == PURE_OP_JZ || op->op == PURE_OP_JMP) && op->arg >= k->block.start && op->arg <= k->block.start + n)
            k->target[op->arg - k->block.start] = true;
        if (op->op == PURE_OP_EACH || op->op == PURE_OP_LOOP) {
            unsigned pops = op->op == PURE_OP_LOOP || op->arg & PURE_EACH_SLICED ? 2 : 0;
            LoopAt *l = &k->loops[k->loop_count];
            memset(l, 0, sizeof(*l));
            l->start = pc + 1, l->end = pc + 1 + (op->arg >> 16), l->parent = depth ? open[depth - 1] : -1;
            l->again = new_label(k->e), l->after = new_label(k->e), l->depth = depth + 1, l->at = pc;
            l->walk = op->op == PURE_OP_EACH, l->list = op->arg >> 8 & 0xFF, l->base = at - pops;
            l->nest = depth ? k->loops[open[0]].nest : (int)k->loop_count;
            l->ahead = -1;
            l->at_kept = l->end_kept = l->k_kept = l->n_kept = -1;
            if (l->start < l->end) k->target[l->start - k->block.start] = true; /* its block begins: what it keeps read */
            k->opens[pc - k->block.start] = (int)k->loop_count;
            open[depth] = (int)k->loop_count++;
            base[++depth] = at - pops;
            ++pc;
            continue;
        }
        if (op->op == PURE_OP_FOLD) { /* its block is its own: not this one's code */
            for (uint32_t i = pc + 1; i < after(op, pc); ++i) k->loop_of[i - k->block.start] = -1, k->height[i - k->block.start] = 0;
        }
        pc = after(op, pc);
    }
    for (unsigned li = 0; li < k->loop_count; ++li) { /* what each loop's block may change */
        LoopAt *l = &k->loops[li];
        for (uint32_t pc = l->start; pc < l->end; pc = after(&m->code[pc], pc)) {
            l->writes |= changes(m, &m->code[pc]);
            l->arena = l->arena || arena_kept(&m->code[pc]);
        }
        l->fast = l->walk && !(l->writes >> (l->list & 31) & 1);
    }
    for (uint32_t i = 0; i <= n; ++i) k->op_label[i] = new_label(k->e);
    k->exit = new_label(k->e);
    plan_kept(k);
    for (unsigned li = 0; li < k->loop_count; ++li)
        if (k->loops[li].start < k->loops[li].end && (k->loops[li].walk ? k->loops[li].fast : true)) plan_ahead(k, li);
    return !k->e->failed;
}

/* The block's code into e (leaving the stack end_height high), its start's word in *entry. */
static bool compile_block(const PureModule *m, PureBlock block, unsigned end_height, Emit *e, size_t *entry) {
    Block *k = calloc(1, sizeof(*k));
    if (!k) return false;
    k->m = m, k->block = block, k->e = e;
    for (unsigned h = 0; h <= PURE_MODULE_STACK; ++h) k->vs[h] = MEMORY;
    bool ok = shape_block(k);
    if (ok) {
        *entry = e->count;
        stp(e, X29, X30, -112, true);
        put(e, 0x910003FD);         /* MOV x29, sp */
        stp(e, X19, X20, 16, false);
        stp(e, X21, X22, 32, false);
        stp(e, X23, X24, 48, false);
        stp(e, X25, X26, 64, false);
        stp(e, X27, X28, 80, false);
        put(e, 0xFD000000 | (96 / 8) << 10 | SP << 5 | D8); /* STR d8, [sp, #96] */
        mov_reg(e, X19, X0);
        load64(e, X20, X19, offsetof(Frame, stack));
        load64(e, X21, X19, offsetof(Frame, values));
        load64(e, X22, X19, offsetof(Frame, ev));
        load64(e, X23, X19, offsetof(Frame, app));
        mov_imm(e, X24, offsetof(PureApp, lists));
        add_reg(e, X24, X23, X24);        /* the app's lists */
        load64(e, X27, X19, offsetof(Frame, loops));
        load64(e, X28, X22, offsetof(Event, locals));
        mov_imm(e, X9, UINT64_C(0x7FF0000000000000));
        fp2(e, FMOV_DX, D8, X9);          /* infinity */
        for (uint32_t pc = block.start; pc < block.start + block.length;) {
            loop_ends(k, pc);
            if (k->target[pc - block.start]) join(k, k->height[pc - block.start], k->loop_of[pc - block.start]);
            bind(e, k->op_label[pc - block.start]);
            int l = k->loop_of[pc - block.start];
            if (l >= 0 && k->loops[l].start == pc && k->loops[l].ahead >= 0) read_ahead(k, (unsigned)l);
            compile_op(k, pc);
            const PureOp *op = &m->code[pc];
            pc = op->op == PURE_OP_EACH || op->op == PURE_OP_LOOP ? pc + 1 : after(op, pc) + k->joined;
            k->joined = false;
        }
        uint32_t end = block.start + block.length;
        loop_ends(k, end);
        join(k, end_height, -1);
        bind(e, k->op_label[block.length]);
        set_sp(k, end_height); /* the block ran: what it leaves (a view's value) is the frame's to give */
        mov_w(e, X0, STEP_NEXT);
        bind(e, k->exit);
        put(e, 0xFD400000 | (96 / 8) << 10 | SP << 5 | D8); /* LDR d8, [sp, #96] */
        ldp(e, X27, X28, 80, false);
        ldp(e, X25, X26, 64, false);
        ldp(e, X23, X24, 48, false);
        ldp(e, X21, X22, 32, false);
        ldp(e, X19, X20, 16, false);
        ldp(e, X29, X30, 112, true);
        put(e, 0xD65F03C0);         /* RET */
        fault_stubs(k);
        slow_stubs(k);
        wide_stubs(k);
    }
    ok = ok && !e->failed;
    free(k->height);
    free(k->loop_of);
    free(k->op_label);
    free(k->loops);
    free(k->opens);
    free(k->target);
    free(k->faults);
    free(k->slows);
    free(k->wides);
    free(k->aheads);
    free(k);
    return ok;
}

/* ---- The module */

typedef struct { PureBlock block; unsigned leaves; size_t entry; } Made;

/* Whether a block has a loop: compiled code pays where blocks repeat; a
 * short block runs once (a view's cell, a fold's item, most handlers) as
 * fast in the executor, without the code's entry and its calls of step. */
static bool loops_in(const PureModule *m, PureBlock block) {
    for (uint32_t pc = block.start; pc < block.start + block.length; ++pc)
        if (m->code[pc].op == PURE_OP_EACH || m->code[pc].op == PURE_OP_LOOP) return true;
    return false;
}

/* A block to make (one with a loop), leaving leaves values on the stack. */
static bool want(const PureModule *m, PureBlock block, unsigned leaves, Made **made, size_t *count, size_t *room) {
    if (!loops_in(m, block)) return true;
    for (size_t i = 0; i < *count; ++i)
        if ((*made)[i].block.start == block.start && (*made)[i].block.length == block.length) return true;
    if (!grow_array((void **)made, room, *count + 1, sizeof(**made))) return false;
    (*made)[(*count)++] = (Made){block, leaves, 0};
    return true;
}

struct PureJit *pure_jit_make(const PureModule *m) {
    const char *enabled = getenv("PURE_JIT");
    if ((enabled && !strcmp(enabled, "0")) || !m->height || !m->code_count) return NULL;
    Made *made = NULL;
    size_t count = 0, room = 0;
    bool ok = true;
    /* The handlers' and the views' code, then the folds' blocks in what is made (they run as blocks of their own). */
    for (uint32_t h = 0; ok && h < m->handlers; ++h) ok = want(m, m->handler[h].code, 0, &made, &count, &room);
    for (uint32_t v = 0; ok && v < m->views; ++v)
        if (m->view_code[v].length) ok = want(m, m->view_code[v], 1, &made, &count, &room);
    for (size_t i = 0; ok && i < count; ++i)
        for (uint32_t pc = made[i].block.start; ok && pc < made[i].block.start + made[i].block.length; ++pc) {
            const PureOp *op = &m->code[pc];
            unsigned how = op->arg & PURE_FOLD_KIND; /* count and first leave a bool; the others a value and a bool */
            if (op->op == PURE_OP_FOLD)
                ok = want(m, (PureBlock){pc + 1, op->arg >> 16}, how == PURE_FOLD_COUNT || how == PURE_FOLD_FIRST ? 1 : 2, &made,
                          &count, &room);
        }
    if (ok && !count) { /* nothing worth it: run() then looks for no code at all */
        free(made);
        return NULL;
    }
    Emit e = {0};
    for (size_t i = 0; ok && i < count; ++i) ok = compile_block(m, made[i].block, made[i].leaves, &e, &made[i].entry);
    ok = ok && resolve(&e);
    const char *dump = getenv("PURE_JIT_DUMP"); /* the code as it is made, to read with objdump -b binary -m aarch64 */
    FILE *file = ok && dump ? fopen(dump, "wb") : NULL;
    if (file) {
        fwrite(e.words, sizeof(uint32_t), e.count, file);
        fclose(file);
    }
    struct PureJit *jit = NULL;
    /* by start, an empty block's at the code's end too */
    if (ok && (jit = calloc(1, sizeof(*jit))) && (jit->by_start = calloc(m->code_count + 1, sizeof(*jit->by_start)))) {
        jit->size = (e.count * sizeof(uint32_t) + 4095) & ~(size_t)4095;
        jit->code_count = m->code_count;
        void *memory = mmap(NULL, jit->size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (memory != MAP_FAILED) {
            jit->memory = memory;
            memcpy(memory, e.words, e.count * sizeof(uint32_t));
            __builtin___clear_cache((char *)memory, (char *)memory + e.count * sizeof(uint32_t));
            if (mprotect(memory, jit->size, PROT_READ | PROT_EXEC)) ok = false;
            for (size_t i = 0; ok && i < count; ++i)
                jit->by_start[made[i].block.start] =
                    (struct Entry){made[i].block.length, (PureCompiled)((uintptr_t)jit->memory + made[i].entry * sizeof(uint32_t))};
        } else
            ok = false;
    } else
        ok = false;
    free(e.words);
    free(e.labels);
    free(e.fixups);
    free(made);
    if (!ok) {
        pure_jit_free(jit);
        return NULL;
    }
    return jit;
}

void pure_jit_free(struct PureJit *jit) {
    if (!jit) return;
    if (jit->memory) munmap(jit->memory, jit->size);
    free(jit->by_start);
    free(jit);
}

PureCompiled pure_jit_code(const struct PureJit *jit, PureBlock block) {
    if (!jit || block.start > jit->code_count) return NULL;
    const struct Entry *entry = &jit->by_start[block.start];
    return entry->code && entry->length == block.length ? entry->code : NULL;
}

#else

struct PureJit *pure_jit_make(const PureModule *m) {
    (void)m;
    return NULL;
}

void pure_jit_free(struct PureJit *jit) { (void)jit; }

PureCompiled pure_jit_code(const struct PureJit *jit, PureBlock block) {
    (void)jit, (void)block;
    return NULL;
}

#endif
