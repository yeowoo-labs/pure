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
 * code; x11-x13 and d16-d31 hold the stack's values; x2-x8, x14, x15, x18
 * (free on Linux), x25 and x26 the loops' kept values; x19 the frame, x20 its stack, x21 the values, x22 the
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
static void subs_imm(Emit *e, unsigned d, unsigned n, uint32_t imm) { put(e, 0xF1000000 | imm << 10 | n << 5 | d); }
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
static void cmp_lsl(Emit *e, unsigned n, unsigned m, unsigned shift) { put(e, 0xEB00001F | m << 16 | shift << 10 | n << 5); }
static void smulh(Emit *e, unsigned d, unsigned n, unsigned m) { put(e, 0x9B407C00 | m << 16 | n << 5 | d); }
static void cmp_asr(Emit *e, unsigned n, unsigned m, unsigned shift) { put(e, 0xEB80001F | m << 16 | shift << 10 | n << 5); }

/* ---- What the compiler knows */

enum { SLOT = sizeof(Slot) };
#define AT(h) ((size_t)(h) * SLOT)
/* An index's list, beside its value in its slot. */
enum { OWNER = offsetof(Slot, bytes) + offsetof(PureBytes, owner) };
enum { TREE_FORM = 0, FLAT_FORM = 1, DENSE_FORM = 2, HASHED_FORM = 3 }; /* pure_list_form */

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
    bool sign;    /* V_X: may be below 0 (else 0 or more) */
    uint8_t mag;  /* V_X: below 2^mag in size (0: not known) */
} Value;
#define MEMORY ((Value){V_MEM, 0, 0, false, false, -1, 0, -1, false, 0})

/* What a loop keeps in a register: of a list, a FLAT column's cells (0
 * when it is not FLAT), a DENSE one's, its key set's bits (NULL when it keeps
 * no bits), the keys they reach (64 a word), its count; of a walk, where it
 * is and where it stops; of a count, its round and how many. A nest's books
 * (docs/design/022): the count of a FLAT list it appends to, as its appends
 * have made it (LIVE), the items added to a DENSE one since it was read
 * (PENDING), and the bytes the event may still grow by (HEADROOM); they go
 * to the list and the app before a call (write_back) and as the nest ends;
 * so does the work the event may still do (WORK: its budget less what it
 * did), which its slices and counts take from as they run. A column's
 * address carries its cells' width in its top byte (bit 56 one byte, 58
 * four, 59 eight): loads on AArch64 Linux leave the top byte out
 * (Documentation/arch/arm64/tagged-pointers.rst), so the code reads through
 * it as it is. */
enum { C_COLUMN, C_DENSE, C_BITS, C_LIMIT, C_COUNT, C_AT, C_END, C_K, C_N, C_LIVE, C_PENDING, C_HEADROOM, C_WORK };
typedef struct { uint8_t kind, field; uint16_t list; uint8_t reg; int loop; } Kept;
enum { KEPT_MOST = 64, NO_REG = 0xFF };
static const uint8_t kept_registers[] = {2, 3, 4, 5, 6, 7, 8, 14, 15, 18, 25, 26, 30}; /* x30: a call writes it, and what is kept is read again after one */

typedef struct {
    uint32_t start, end; /* its block */
    int parent;          /* the loop around it, or -1 */
    int again[2], after[2]; /* its iteration's label, and past it (each copy's) */
    unsigned depth;      /* the loops open in it, itself too */
    uint32_t at;         /* its EACH or LOOP */
    unsigned base;       /* the stack's height in its block (its bounds taken) */
    bool walk;           /* EACH (else LOOP) */
    uint32_t list;       /* a walk's */
    uint32_t writes;     /* the lists its block may change (every bit: any) */
    bool fast;           /* a walk whose list its block does not change: its place kept, its end once */
    bool sliced;         /* a fast walk of a slice of a stored list (open_slice): the integers' code walks only those it opens */
    bool arena;          /* its block may leave bytes in the arena: given back each round */
    int at_kept, end_kept, k_kept, n_kept; /* its state's kept values */
    int nest;            /* the outermost loop around it (itself when none is) */
    uint64_t region;     /* what its nest keeps: each in a register of its own, all read at each label in it */
    int ahead;           /* a slice it opens whose key it reads ahead (an Ahead), or -1 */
    uint64_t stores;     /* the locals its block stores (a count's slot too) */
    uint32_t books;      /* a nest's: the lists its appends count in registers (C_LIVE, C_PENDING) */
    uint64_t ints;       /* the integers' code: the locals that hold whole numbers at its labels */
    int tier;            /* an outermost loop the tier runs (docs/design/023): its entry's label, else -1 */
} LoopAt;

/* A key as a loop computes it, followed from its block's start (reading
 * ahead): a constant, the loop's round (a count's value, a walk's place),
 * an item's field at a position, or arithmetic of two. */
enum { S_CONST, S_ROUND, S_ITEM, S_ADD, S_SUB, S_MUL };
typedef struct { uint8_t kind; uint16_t list, field; int a, b; double value; } Sym;
/* A slice read ahead: the loop opening it (an EACH in its block), its list and the column its block reads, its low key. */
typedef struct { unsigned inner; uint32_t list, field; int key; Sym nodes[32]; unsigned count; } Ahead;

typedef struct { int label; PureFault kind; uint32_t pc; uint64_t fresh; } FaultAt;

/* An instruction's step, called out of the fast way's line: its operands
 * stored first (as they were when it branched), the loops' places written
 * back; its result loaded back where the fast way leaves it, what was kept
 * read again; then back. */
typedef struct {
    int label, back;
    uint32_t pc;
    uintptr_t function;
    unsigned height, base; /* the stack's height at the instruction, and its operands' first */
    Value vs[PURE_MODULE_STACK + 1]; /* the stack at the branch: its operands base up, what lies below kept in registers */
    int result;            /* its result's height, or -1 */
    uint8_t result_kind, result_reg;
    uint64_t fresh;        /* what is kept at its end */
    int loop;              /* the innermost loop around it */
    int tree_list;         /* ITEM_GET whose kept column was none: the run a tree's code keeps tried first (else -1) */
    unsigned tree_field;
    bool two;              /* the step at pc, then the one at pc + 1 (an ITEM_GET and its HAS, run as one) */
    int deopt;             /* the integers' code: where it gives up when the result is no whole number (else -1) */
    uint64_t entry;        /* what was kept as the instruction began: what it reads after holds nothing to write back */
} Slow;

/* item_has out of the fast way's line: a cell of eight bytes or one read as
 * a key, and a key past the bits (absent; or no bits at all: the steps). */
typedef struct { int other, absent, key, back, slow; uint8_t column, at, bits, r; } HasCold;

/* A cell of eight bytes or one, read out of the line a four-byte one is read in
 * (in the integers' code, into an x register: eight bytes give it up, deopt). */
typedef struct { int label, back; uint8_t result, column, at; int deopt; } Wide;

/* Where the integers' code gives up (docs/design/022): the stack as it was
 * then, to its slots as numbers; the executor runs the rest of the round
 * from pc (pure_run_rest); then the code's own iteration, or past the loop. */
typedef struct {
    int label;
    uint32_t pc;
    unsigned height;
    int loop;
    uint64_t fresh;
    Value vs[PURE_MODULE_STACK + 1];
} Deopt;

typedef struct {
    const PureModule *m;
    PureBlock block;
    Emit *e;
    uint8_t *height;  /* absolute, by instruction of the block */
    int *loop_of;     /* the innermost loop each instruction is in, or -1 */
    int *opens;       /* the loop an EACH or a LOOP opens */
    LoopAt *loops;
    unsigned loop_count;
    int *labels[2];   /* each instruction's, and the block's end: the code's, and the integers' (a nest's again, docs/design/022) */
    unsigned copy;    /* which code is being made: 0, or 1 the integers' */
    bool *target;     /* each instruction a branch lands on */
    int exit;         /* w0 holds how it went */
    FaultAt *faults;  /* the faults its code may give, each a short stub after it */
    size_t fault_count, fault_room;
    Slow *slows;
    size_t slow_count, slow_room;
    Wide *wides;
    size_t wide_count, wide_room;
    Deopt *deopts;
    size_t deopt_count, deopt_room;
    HasCold *colds;
    size_t cold_count, cold_room;
    uint64_t ints;    /* the integers' code: the locals known to hold whole numbers (as their slots' numbers) */
    Kept kept[KEPT_MOST];
    unsigned kept_count;
    Ahead *aheads;
    size_t ahead_count, ahead_room;
    uint64_t fresh;   /* what is kept and holds its value now */
    bool joined;      /* the instruction just compiled took the next one too */
    Value vs[PURE_MODULE_STACK + 1];
} Block;

#define OP_LABEL(k, i) ((k)->labels[(k)->copy][i])
#define AGAIN(k, l) ((l)->again[(k)->copy])
#define AFTER(k, l) ((l)->after[(k)->copy])

/* A label whose code gives a fault of kind at pc. */
static int fault_at(Block *k, PureFault kind, uint32_t pc) {
    for (size_t i = 0; i < k->fault_count; ++i)
        if (k->faults[i].kind == kind && k->faults[i].pc == pc && k->faults[i].fresh == k->fresh) return k->faults[i].label;
    if (!grow_array((void **)&k->faults, &k->fault_room, k->fault_count + 1, sizeof(*k->faults))) {
        k->e->failed = true;
        return k->exit;
    }
    k->faults[k->fault_count] = (FaultAt){new_label(k->e), kind, pc, k->fresh};
    return k->faults[k->fault_count++].label;
}

static void books_back(Block *k, uint64_t mask);

static void fault_stubs(Block *k) {
    Emit *e = k->e;
    for (size_t i = 0; i < k->fault_count; ++i) {
        bind(e, k->faults[i].label);
        books_back(k, k->faults[i].fresh); /* the lists as they are, whatever comes of them */
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
    case C_LIMIT: /* none without bits: a key past them asks whether there are any (item_has, append_books) */
        load64(e, X16, X24, list_at);
        load64(e, r, X16, pure_list_layout[LIST_MEMBER_CAPACITY_AT]);
        lsl_imm(e, r, r, 6);
        load8(e, X17, X16, pure_list_layout[LIST_BITMAP_AT]);
        cmp_w_imm(e, X17, 0);
        csel(e, r, r, XZR, NE);
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
    case C_LIVE:
        load64(e, X16, X24, list_at);
        load64(e, r, X16, pure_list_layout[LIST_COUNT_AT]);
        break;
    case C_PENDING: mov_w(e, r, 0); break;
    case C_HEADROOM: { /* the least of the budget less what the event grew, and its room less what it holds */
        int unbudgeted = new_label(e);
        load64(e, X9, X23, offsetof(PureApp, room));
        load64(e, X10, X23, offsetof(PureApp, spent));
        sub_reg(e, r, X9, X10);
        load64(e, X9, X23, offsetof(PureApp, memory));
        cbz_x(e, X9, unbudgeted);
        load64(e, X10, X23, offsetof(PureApp, grown));
        sub_reg(e, X9, X9, X10);
        cmp_reg(e, X9, r);
        csel(e, r, X9, r, LT);
        bind(e, unbudgeted);
        break;
    }
    case C_WORK: { /* its budget (else the default) less what it did */
        int budgeted = new_label(e);
        load64(e, r, X23, offsetof(PureApp, work_budget));
        cbnz_x(e, r, budgeted);
        mov_imm(e, r, PURE_WORK_DEFAULT);
        bind(e, budgeted);
        load64(e, X9, X23, offsetof(PureApp, work));
        sub_reg(e, r, r, X9);
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

static size_t row_bytes(const PureModule *m, uint32_t list) { return 8 * (size_t)m->record[m->state_ref[list]].count; }

/* The nest's books in mask, as they stand, into the lists and the app: a
 * FLAT list's count (the items past it its appends), a DENSE one's items
 * added; with them their versions, stored bytes and the event's growth.
 * What is kept and not read holds nothing of its own: the books change only
 * where they are read (append_books), and go back before they are let go. */
static void books_back(Block *k, uint64_t mask) {
    Emit *e = k->e;
    for (unsigned c = 0; c < k->kept_count; ++c) {
        const Kept *v = &k->kept[c];
        if (mask >> c & 1 && v->kind == C_WORK) { /* what it did: its budget less what it may still do */
            int budgeted = new_label(e);
            load64(e, X10, X23, offsetof(PureApp, work_budget));
            cbnz_x(e, X10, budgeted);
            mov_imm(e, X10, PURE_WORK_DEFAULT);
            bind(e, budgeted);
            sub_reg(e, X10, X10, v->reg);
            store64(e, X10, X23, offsetof(PureApp, work));
            continue;
        }
        if (!(mask >> c & 1) || (v->kind != C_LIVE && v->kind != C_PENDING)) continue;
        int none = new_label(e);
        size_t bytes = row_bytes(k->m, v->list);
        load64(e, X16, X24, (size_t)v->list * sizeof(PureList *));
        load64(e, X17, X16, pure_list_layout[LIST_COUNT_AT]);
        if (v->kind == C_LIVE) {
            sub_reg(e, X9, v->reg, X17);
            cbz_x(e, X9, none);
            store64(e, v->reg, X16, pure_list_layout[LIST_COUNT_AT]);
        } else {
            mov_reg(e, X9, v->reg);
            cbz_x(e, X9, none);
            add_reg(e, X17, X17, X9);
            store64(e, X17, X16, pure_list_layout[LIST_COUNT_AT]);
            load64(e, X10, X16, pure_list_layout[LIST_MEMBER_COUNT_AT]);
            add_reg(e, X10, X10, X9);
            store64(e, X10, X16, pure_list_layout[LIST_MEMBER_COUNT_AT]);
            mov_w(e, v->reg, 0);
        }
        load64(e, X10, X16, pure_list_layout[LIST_VERSION_AT]);
        add_imm(e, X10, X10, 1);
        store64(e, X10, X16, pure_list_layout[LIST_VERSION_AT]);
        mov_imm(e, X10, bytes);
        mul_reg(e, X9, X9, X10); /* the bytes the items added */
        load64(e, X10, X16, pure_list_layout[LIST_BYTES_AT]);
        add_reg(e, X10, X10, X9);
        store64(e, X10, X16, pure_list_layout[LIST_BYTES_AT]);
        load64(e, X10, X23, offsetof(PureApp, grown));
        add_reg(e, X10, X10, X9);
        store64(e, X10, X23, offsetof(PureApp, grown));
        load64(e, X10, X23, offsetof(PureApp, spent));
        add_reg(e, X10, X10, X9);
        store64(e, X10, X23, offsetof(PureApp, spent));
        bind(e, none);
    }
}

/* Before a call from code in loop: the places of the walks open around it,
 * kept in registers, back where the steps read them (the event's row the
 * innermost walk's). */
static void write_back(Block *k, int loop) {
    Emit *e = k->e;
    bool row = false;
    books_back(k, k->fresh);
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

/* Whether the nest of loop keeps list's books in registers. */
static bool books(const Block *k, int loop, uint32_t list) { return loop >= 0 && k->loops[k->loops[loop].nest].books >> (list & 31) & 1; }

/* The count of list as the code in loop has it now, in a register: what
 * the nest keeps (its appends counted), else its memory's into scratch
 * (x16 spent). */
static unsigned count_reg(Block *k, int loop, uint32_t list, unsigned scratch) {
    Emit *e = k->e;
    int live = kept_find(k, loop, C_LIVE, list, 0), pending = kept_find(k, loop, C_PENDING, list, 0),
        counted = kept_find(k, loop, C_COUNT, list, 0);
    if (books(k, loop, list) && live >= 0) return kept_reg(k, live);
    if (counted >= 0) return kept_reg(k, counted);
    unsigned added = books(k, loop, list) && pending >= 0 ? kept_reg(k, pending) : 0;
    load64(e, X16, X24, list * sizeof(PureList *));
    load64(e, scratch, X16, pure_list_layout[LIST_COUNT_AT]);
    if (added) add_reg(e, scratch, scratch, added);
    return scratch;
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
    case V_X: fp2(e, SCVTF_X, D0, v->reg); store_d(e, D0, X20, AT(h)); break;
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
    *v = (Value){V_BIT, (uint8_t)r, 0, false, false, v->owner, 0, -1, false, 0};
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
    case V_X: fp2(e, SCVTF_X, scratch, v->reg); return scratch;
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
    if (v->kind == V_X && v->sign) { /* below 0 as 0 (size_t of a number below 0 here) */
        cmp_imm(k->e, v->reg, 0);
        csel(k->e, scratch, v->reg, XZR, GE);
        return scratch;
    }
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

/* Integer code may be made of a value that is a whole number below 2^53 in
 * size: in an x register, or a constant one (not -0). */
static bool whole(const Value *v) {
    if (v->kind == V_X) return true;
    if (v->kind != V_CONST) return false;
    double x = num_of(v->bits);
    return fabs(x) < 0x1p53 && x == floor(x) && !(x == 0 && signbit(x));
}

static unsigned value_whole(Block *k, unsigned h, unsigned scratch) {
    Value *v = &k->vs[h];
    if (v->kind == V_X) return v->reg;
    mov_imm(k->e, scratch, (uint64_t)(int64_t)num_of(v->bits));
    return scratch;
}

static bool may_be_negative(const Value *v) { return v->kind == V_X ? v->sign : num_of(v->bits) < 0; }

/* How many bits a whole value's size takes at most (64: not known). */
static unsigned mag_of(const Value *v) {
    if (v->kind == V_X) return v->mag ? v->mag : 64;
    double x = fabs(num_of(v->bits));
    unsigned m = 0;
    while (m < 64 && x >= ldexp(1.0, (int)m)) ++m;
    return m;
}

/* A whole number in r, below 2^53 in size, else to fail (x9, x10 spent). */
static void in_range(Block *k, unsigned r, int fail) {
    mov_imm(k->e, X9, UINT64_C(1) << 53);
    add_reg(k->e, X10, r, X9);
    cmp_lsl(k->e, X10, X9, 1);
    b_cond(k->e, HS, fail);
}

/* The number in d as a whole number in x register r (below 2^53 in size, not -0), else to fail (d1, x9, x10 spent). */
static void int_from_d(Block *k, unsigned r, unsigned d, int fail) {
    Emit *e = k->e;
    fp2(e, FCVTZS_X, r, d);
    fp2(e, SCVTF_X, D1, r);
    fcmp(e, d, D1);
    b_cond(e, NE, fail);
    fp2(e, FMOV_XD, X9, d);
    mov_imm(e, X10, UINT64_C(1) << 63);
    cmp_reg(e, X9, X10);
    b_cond(e, EQ, fail); /* -0 */
    in_range(k, r, fail);
}

/* ---- Branches and joins: every value in its slot, what is kept read */

/* Where a branch at from to target lands: the iteration of the innermost loop around from that ends at target, else target's instruction. */
static int landing(const Block *k, uint32_t from, uint32_t target, int *loop) {
    for (int l = k->loop_of[from - k->block.start]; l >= 0; l = k->loops[l].parent)
        if (k->loops[l].end == target) {
            *loop = l;
            return AGAIN(k, &k->loops[l]);
        }
    *loop = target < k->block.start + k->block.length ? k->loop_of[target - k->block.start] : -1;
    return OP_LABEL(k, target - k->block.start);
}

/* Before the code reaches a label (by a branch or by going on): the values
 * in their slots, what that label's loop keeps read. */
static void join(Block *k, unsigned height, int loop) {
    spill_all(k, height);
    books_back(k, k->fresh & ~kept_at(k, loop)); /* out of the nest: its books back */
    refresh(k, kept_at(k, loop));
    k->fresh = kept_at(k, loop); /* as every way here has it */
    k->ints = k->copy && loop >= 0 ? k->loops[loop].ints : 0;
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
    memcpy(s->vs, k->vs, sizeof(s->vs));
    s->loop = k->loop_of[pc - k->block.start];
    s->tree_list = -1;
    s->deopt = -1;
    s->entry = k->fresh;
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
    memcpy(k->vs, s->vs, sizeof(k->vs));
    for (unsigned h = 0; h < s->height; ++h) spill(k, h);
    k->fresh = s->fresh & s->entry;
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
        else if (s->deopt >= 0) { /* the integers' code: a whole number, else it gives up */
            load_d(e, D0, X20, AT(s->result));
            int_from_d(k, s->result_reg, D0, s->deopt);
        } else { /* a bool or a whole number: its number back to an integer */
            load_d(e, D0, X20, AT(s->result));
            fp2(e, FCVTZU_X, s->result_reg, D0);
        }
    }
    for (unsigned h = 0; h < s->base; ++h) { /* what lies below its operands, back where the fast way keeps it */
        const Value *v = &s->vs[h];
        if (v->borrowed) continue;
        if (v->kind == V_D) load_d(e, v->reg, X20, AT(h));
        else if (v->kind == V_X || v->kind == V_BIT) {
            load_d(e, D0, X20, AT(h));
            fp2(e, v->kind == V_X ? FCVTZS_X : FCVTZU_X, v->reg, D0);
            if (v->kind == V_BIT && v->negate) eor_w_one(e, v->reg, v->reg);
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
            memcpy(k->vs, s.vs, sizeof(k->vs));
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
            if (s.deopt >= 0) {
                load_d_index(e, D0, X17, X0);
                int_from_d(k, s.result_reg, D0, s.deopt);
            } else
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
    *w = (Wide){new_label(e), new_label(e), (uint8_t)r, (uint8_t)column, (uint8_t)at, -1};
    tbz(e, column, 58, w->label);
    load32_index(e, X10, column, at);
    fp2(e, UCVTF_W, r, X10);
    bind(e, w->back);
}

/* The integers' code: a cell of four bytes or one into x register r; eight bytes, to deopt. */
static void load_int(Block *k, unsigned r, unsigned column, unsigned at, int deopt) {
    Emit *e = k->e;
    if (!grow_array((void **)&k->wides, &k->wide_room, k->wide_count + 1, sizeof(*k->wides))) {
        e->failed = true;
        return;
    }
    Wide *w = &k->wides[k->wide_count++];
    *w = (Wide){new_label(e), new_label(e), (uint8_t)r, (uint8_t)column, (uint8_t)at, deopt};
    tbz(e, column, 58, w->label);
    load32_index(e, r, column, at);
    bind(e, w->back);
}

static void wide_stubs(Block *k) {
    Emit *e = k->e;
    for (size_t i = 0; i < k->wide_count; ++i) {
        const Wide *w = &k->wides[i];
        int one = new_label(e);
        bind(e, w->label);
        tbz(e, w->column, 59, one);
        if (w->deopt >= 0) b(e, w->deopt);
        else {
            load_d_index(e, w->result, w->column, w->at);
            b(e, w->back);
        }
        bind(e, one);
        if (w->deopt >= 0) load8_index(e, w->result, w->column, w->at);
        else {
            load8_index(e, X10, w->column, w->at);
            fp2(e, UCVTF_W, w->result, X10);
        }
        b(e, w->back);
    }
}

/* Where the integers' code gives up at pc, the stack as it is now (height high). */
static int deopt_at(Block *k, uint32_t pc, unsigned height) {
    Emit *e = k->e;
    if (!grow_array((void **)&k->deopts, &k->deopt_room, k->deopt_count + 1, sizeof(*k->deopts))) {
        e->failed = true;
        return k->exit;
    }
    Deopt *d = &k->deopts[k->deopt_count++];
    d->label = new_label(e), d->pc = pc, d->height = height, d->loop = k->loop_of[pc - k->block.start], d->fresh = k->fresh;
    memcpy(d->vs, k->vs, sizeof(d->vs));
    return d->label;
}

/* Where it gives up after a step at pc ran (the stack in its slots, height high, nothing kept read), the executor on from next. */
static int deopt_after(Block *k, uint32_t pc, uint32_t next, unsigned height) {
    Value saved[PURE_MODULE_STACK + 1];
    uint64_t fresh = k->fresh;
    memcpy(saved, k->vs, sizeof(saved));
    for (unsigned h = 0; h <= PURE_MODULE_STACK; ++h) k->vs[h] = MEMORY;
    k->fresh = 0;
    int label = deopt_at(k, pc, height);
    k->deopts[k->deopt_count - 1].pc = next;
    memcpy(k->vs, saved, sizeof(saved));
    k->fresh = fresh;
    return label;
}

static void deopt_stubs(Block *k) {
    Emit *e = k->e;
    Value saved[PURE_MODULE_STACK + 1];
    memcpy(saved, k->vs, sizeof(saved));
    for (size_t i = 0; i < k->deopt_count; ++i) {
        const Deopt *d = &k->deopts[i];
        const LoopAt *l = &k->loops[d->loop];
        int left = new_label(e);
        bind(e, d->label);
        memcpy(k->vs, d->vs, sizeof(k->vs));
        spill_all(k, d->height);
        k->fresh = d->fresh;
        write_back(k, d->loop);
        set_sp(k, d->height);
        mov_reg(e, X0, X19);
        mov_w(e, X1, d->pc);
        call(e, (uintptr_t)pure_run_rest);
        k->fresh = 0;
        cbnz_w(e, X0, left);
        refresh(k, kept_at(k, d->loop)); /* the round ran: the code's own iteration, as made first */
        b(e, l->again[0]);
        bind(e, left);
        k->fresh = 0;
        cmp_w_imm(e, X0, STEP_SKIP);
        b_cond(e, NE, k->exit);
        refresh(k, kept_at(k, l->parent)); /* it left the loop */
        b(e, l->after[0]);
    }
    memcpy(k->vs, saved, sizeof(saved));
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
            cond = op->op == PURE_OP_LT ? LT : op->op == PURE_OP_LE ? LE : op->op == PURE_OP_GT ? GT : op->op == PURE_OP_GE ? GE
                 : op->op == PURE_OP_EQ ? EQ : NE;
            (void)pc;
        } else {
            unsigned x = value_d(k, h, D0), y = value_d(k, h + 1, D1);
            fcmp(e, x, y);
            cond = op->op == PURE_OP_LT ? MI : op->op == PURE_OP_LE ? LS : op->op == PURE_OP_GT ? GT : op->op == PURE_OP_GE ? GE
                 : op->op == PURE_OP_EQ ? EQ : NE;
        }
        k->vs[h] = (Value){V_FLAG, 0, (uint8_t)cond, false, false, -1, 0, -1, false, 0};
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
        k->vs[h] = (Value){V_BIT, (uint8_t)r, 0, false, false, -1, 0, -1, false, 0};
        return;
    }
    default: break;
    }
    if (k->copy && whole(a) && whole(bv) &&
        (op->op == PURE_OP_ADD || op->op == PURE_OP_SUB || op->op == PURE_OP_MIN || op->op == PURE_OP_MAX ||
         (op->op == PURE_OP_MUL && ((a->kind == V_CONST && num_of(a->bits) > 0 && num_of(a->bits) < 0x1p32) ||
                                    (bv->kind == V_CONST && num_of(bv->bits) > 0 && num_of(bv->bits) < 0x1p32))))) {
        /* the integers' code: whole numbers stay whole (below 2^53 in size, else it gives up; a positive constant's product is never -0) */
        unsigned r = alloc_x(k, h), ma = mag_of(a), mb = mag_of(bv), mag = (ma > mb ? ma : mb) + 1;
        int give_up = deopt_at(k, pc, height);
        bool sign = may_be_negative(a) || may_be_negative(bv) || op->op == PURE_OP_SUB;
        /* r may be an operand's register: where a check may give up, the result is made in x16 and moved
         * to r after it, the operands as they were for the executor to take them again */
        unsigned t = r;
        if (op->op == PURE_OP_MUL) {
            bool left = a->kind == V_CONST && num_of(a->bits) > 0 && num_of(a->bits) < 0x1p32;
            unsigned x = value_whole(k, left ? h + 1 : h, X9), p = 0;
            uint64_t c = (uint64_t)num_of((left ? a : bv)->bits);
            bool shift = !(c & (c - 1)); /* a power of two: a shift, below 2^(the other's size + its exponent) */
            if (shift) {
                while (c >> p != 1) ++p;
                mag = mag_of(left ? bv : a) + p;
            } else
                mag = ma + mb;
            if (mag > 53) t = X16;
            if (shift) lsl_imm(e, t, x, p);
            else {
                mov_imm(e, X10, c);
                put(e, 0x9B007C00 | X10 << 16 | x << 5 | t); /* MUL */
            }
            if (mag > 63) { /* past 64 bits: what the shift dropped, or the high half other than the sign */
                if (shift) cmp_asr(e, x, t, p);
                else {
                    smulh(e, X17, x, X10);
                    cmp_asr(e, X17, t, 63);
                }
                b_cond(e, NE, give_up);
            }
            sign = may_be_negative(left ? bv : a);
        } else {
            double cy = bv->kind == V_CONST ? num_of(bv->bits) : -1, cx = a->kind == V_CONST ? num_of(a->bits) : -1;
            bool add = op->op == PURE_OP_ADD || op->op == PURE_OP_SUB;
            if (add && cy >= 0 && cy < 2) mag = ma + (cy > 0);
            else if (op->op == PURE_OP_ADD && cx >= 0 && cx < 2) mag = mb + (cx > 0);
            if (add && mag > 53) t = X16;
            if (add && cy >= 0 && cy < 4096) { /* a small constant: in the instruction */
                unsigned x = value_whole(k, h, X9);
                if (op->op == PURE_OP_ADD) add_imm(e, t, x, (uint32_t)cy);
                else sub_imm(e, t, x, (uint32_t)cy);
            } else if (op->op == PURE_OP_ADD && cx >= 0 && cx < 4096) {
                add_imm(e, t, value_whole(k, h + 1, X10), (uint32_t)cx);
            } else {
                unsigned x = value_whole(k, h, X9), y = value_whole(k, h + 1, X10);
                if (op->op == PURE_OP_ADD) add_reg(e, t, x, y);
                else if (op->op == PURE_OP_SUB) sub_reg(e, t, x, y);
                else {
                    cmp_reg(e, x, y);
                    csel(e, r, x, y, op->op == PURE_OP_MIN ? LT : GT);
                    k->vs[h] = (Value){V_X, (uint8_t)r, 0, false, false, -1, 0, -1, sign, (uint8_t)(mag - 1)};
                    return;
                }
            }
        }
        if (mag > 53) in_range(k, t, give_up);
        if (t != r) mov_reg(e, r, t);
        k->vs[h] = (Value){V_X, (uint8_t)r, 0, false, false, -1, 0, -1, sign, (uint8_t)(mag > 53 ? 53 : mag)};
        return;
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
    k->vs[h] = (Value){V_D, (uint8_t)r, 0, false, false, -1, 0, -1, false, 0};
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

/* The value at h into a column whose address carries its width (bit 59
 * eight bytes, 58 four, else one) at row at, or to slow when it does not
 * fit; x0, x1, d0, d1 spent. */
static void store_tagged(Block *k, unsigned h, unsigned col, unsigned at, int slow) {
    Emit *e = k->e;
    Value *v = &k->vs[h];
    int eight = new_label(e), four = new_label(e), done = new_label(e);
    tbnz(e, col, 59, eight);
    unsigned x = X0;
    if (v->kind == V_X) x = v->reg; /* below 0: past either width */
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
    tbnz(e, col, 58, four);
    cmp_imm(e, x, 256);
    b_cond(e, HS, slow);
    store8_index(e, x, col, at);
    b(e, done);
    bind(e, four);
    lsr_imm(e, X1, x, 32);
    cbnz_x(e, X1, slow);
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

/* LIST_APPEND with the nest's books (docs/design/022): the cells into the
 * columns kept (FLAT: at the count kept, DENSE: at the key, absent, below
 * its room and its bits' reach), the count or the items added counted in
 * their register, the event's headroom less the item's bytes; nothing else
 * written until a call or the nest's end (books_back). */
static void append(Block *k, const PureOp *op, uint32_t pc, unsigned height);

static void append_books(Block *k, const PureOp *op, uint32_t pc, unsigned height) {
    Emit *e = k->e;
    const PureModule *m = k->m;
    uint32_t list = op->arg;
    int loop = k->loop_of[pc - k->block.start];
    unsigned fields = m->record[m->state_ref[list]].count, base = height - fields, key = m->state_key[list];
    unflag_below(k, height);
    size_t s = (size_t)(slow_at(k, pc, (uintptr_t)pure_run_step, height, base, -1) - k->slows);
    if (e->failed) return;
    int slow = k->slows[s].label;
    unsigned columns[16], head = kept_reg(k, kept_find(k, loop, C_HEADROOM, 0, 0)), at = 0, bits = 0, limit = 0, pending = 0;
    for (unsigned f = 0; f < fields; ++f)
        columns[f] = key && f == key - 1u ? 0 : kept_reg(k, kept_find(k, loop, key ? C_DENSE : C_COLUMN, list, f));
    if (!key) at = kept_reg(k, kept_find(k, loop, C_LIVE, list, 0));
    else {
        bits = kept_reg(k, kept_find(k, loop, C_BITS, list, 0)), limit = kept_reg(k, kept_find(k, loop, C_LIMIT, list, 0));
        pending = kept_reg(k, kept_find(k, loop, C_PENDING, list, 0));
    }
    unsigned some = key == 1 ? columns[1] : columns[0]; /* a column kept: 0 when the list is not in that form */
    size_t bytes = row_bytes(m, list);
    int other = new_label(e), done = new_label(e); /* a keyed list not DENSE (HASHED): as append does it, the headroom kept */
    cbz_x(e, some, key ? other : slow);
    if (!key) {
        load64(e, X16, X24, list * sizeof(PureList *));
        load64(e, X17, X16, pure_list_layout[LIST_ROOM_AT]);
        cmp_reg(e, at, X17);
        b_cond(e, HS, slow);
    } else {
        at = value_key(k, base + key - 1u, X9, slow);
        if (at != X9) mov_reg(e, X9, at);
        at = X9;
        cmp_reg(e, X9, limit); /* the bits reach it (small keys only; none without bits) */
        b_cond(e, HS, slow);
        load64(e, X16, X24, list * sizeof(PureList *));
        load64(e, X17, X16, pure_list_layout[LIST_ROOM_AT]);
        cmp_reg(e, X9, X17);
        b_cond(e, HS, slow);
        lsr_imm(e, X10, X9, 6);
        load64_index(e, X16, bits, X10); /* its word (x16, at x10, kept to the end): the key there already is EXISTS */
        lsr_reg(e, X17, X16, X9);
        tbnz(e, X17, 0, fault_at(k, PURE_FAULT_EXISTS, pc));
    }
    for (unsigned f = 0; f < fields; ++f)
        if (columns[f]) store_tagged(k, base + f, columns[f], at, slow);
    subs_imm(e, head, head, (uint32_t)bytes);
    b_cond(e, LT, slow); /* past the budget or the room: the step says MEMORY (the headroom read again after) */
    if (!key) add_imm(e, at, at, 1);
    else {
        mov_imm(e, X17, 1);
        lsl_reg(e, X17, X17, X9);
        orr_reg(e, X16, X16, X17);
        store64_index(e, X16, bits, X10);
        add_imm(e, pending, pending, 1);
    }
    slow_back(k, s, V_MEM, 0);
    if (key) {
        b(e, done);
        bind(e, other);
        append(k, op, pc, height);
    }
    bind(e, done);
}

/* A key's bits in a HASHED list's table (module-list.c's key_bits: -0 as +0) into x17. */
static void hashed_bits(Block *k, unsigned h) {
    Emit *e = k->e;
    int signed_zero = new_label(e);
    unsigned bits = value_bits(k, h, X17);
    if (bits != X17) mov_reg(e, X17, bits);
    lsl_imm(e, X10, X17, 1);
    cbnz_x(e, X10, signed_zero);
    mov_imm(e, X17, 0);
    bind(e, signed_zero);
}

/* LIST_APPEND to an event's list of numbers whose places no index follows:
 * at the end of a FLAT one, at its key in a DENSE one, or at its key's slot
 * in a HASHED one (its table not to grow), with room, each cell fitting its
 * column. */
static void append(Block *k, const PureOp *op, uint32_t pc, unsigned height) {
    Emit *e = k->e;
    const PureModule *m = k->m;
    uint32_t list = op->arg;
    unsigned fields = m->record[m->state_ref[list]].count, base = height - fields;
    bool keyed = m->state_key[list] != 0;
    unflag_below(k, height);
    size_t s = (size_t)(slow_at(k, pc, (uintptr_t)pure_run_step, height, base, -1) - k->slows);
    if (e->failed) return;
    int slow = k->slows[s].label;
    load64(e, X16, X24, list * sizeof(PureList *));
    load8(e, X17, X16, pure_list_layout[LIST_FORM_AT]);
    int cells = new_label(e);
    if (!keyed) {
        cmp_w_imm(e, X17, FLAT_FORM);
        b_cond(e, NE, slow);
        load64(e, X9, X16, pure_list_layout[LIST_COUNT_AT]);
        load64(e, X10, X16, pure_list_layout[LIST_ROOM_AT]);
        cmp_reg(e, X9, X10);
        b_cond(e, HS, slow);
    } else {
        int dense = new_label(e), probe = new_label(e), vacant = new_label(e);
        cmp_w_imm(e, X17, DENSE_FORM);
        b_cond(e, EQ, dense);
        cmp_w_imm(e, X17, HASHED_FORM);
        b_cond(e, NE, slow);
        if (k->vs[base + m->state_key[list] - 1u].kind == V_BIT) b(e, slow); /* (its bits made in x9, the slot's) */
        load64(e, X10, X16, pure_list_layout[LIST_COUNT_AT]); /* room: its items and the new one four fifths of its table at most, else it grows (the step) */
        add_imm(e, X10, X10, 1);
        add_lsl(e, X10, X10, X10, 2); /* five times */
        load64(e, X1, X16, pure_list_layout[LIST_MEMBER_CAPACITY_AT]);
        cmp_lsl(e, X10, X1, 2);       /* against four times the slots */
        b_cond(e, HI, slow);
        hashed_bits(k, base + m->state_key[list] - 1u);
        mov_imm(e, X10, UINT64_C(0xff51afd7ed558ccd)); /* member_slot */
        put(e, 0xCA400000 | X17 << 16 | 33u << 10 | X17 << 5 | X0); /* EOR x0, x17, x17, LSR #33 */
        mul_reg(e, X9, X0, X10);
        put(e, 0xCA400000 | X9 << 16 | 33u << 10 | X9 << 5 | X9);  /* EOR x9, x9, x9, LSR #33 */
        sub_imm(e, X1, X1, 1);
        put(e, 0x8A000000 | X1 << 16 | X9 << 5 | X9);              /* AND x9, x9, x1 (the mask) */
        load64(e, X0, X16, pure_list_layout[LIST_MEMBERS_AT]);
        bind(e, probe);
        load64_index(e, X10, X0, X9);
        cmp_reg(e, X10, X17);
        b_cond(e, EQ, fault_at(k, PURE_FAULT_EXISTS, pc));
        fp2(e, FMOV_DX, D1, X10);
        fcmp(e, D1, D1);
        b_cond(e, VS, vacant); /* a NaN: VACANT (no key is one) */
        add_imm(e, X9, X9, 1);
        put(e, 0x8A000000 | X1 << 16 | X9 << 5 | X9);
        b(e, probe);
        bind(e, vacant);
        b(e, cells);
        bind(e, dense);
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
    bind(e, cells);
    for (unsigned f = 0; f < fields; ++f) { /* the cells, past the end or in the key's empty slot: not the list's until counted */
        load64(e, X10, X16, pure_list_layout[LIST_COLUMNS_AT] + f * sizeof(void *));
        load8(e, X17, X16, pure_list_layout[LIST_WIDTH_AT] + f);
        store_cell(k, base + f, X10, X17, X9, slow);
    }
    if (!keyed) {
        add_imm(e, X9, X9, 1);
        store64(e, X9, X16, pure_list_layout[LIST_COUNT_AT]);
    } else {
        int dense = new_label(e), counted = new_label(e);
        load8(e, X17, X16, pure_list_layout[LIST_FORM_AT]);
        cmp_w_imm(e, X17, HASHED_FORM);
        b_cond(e, NE, dense);
        hashed_bits(k, base + m->state_key[list] - 1u); /* its key in its slot: the item the list's */
        load64(e, X0, X16, pure_list_layout[LIST_MEMBERS_AT]);
        store64_index(e, X17, X0, X9);
        b(e, counted);
        bind(e, dense);
        load64(e, X0, X16, pure_list_layout[LIST_MEMBERS_AT]);
        lsr_imm(e, X10, X9, 6);
        load64_index(e, X1, X0, X10);
        mov_imm(e, X17, 1);
        lsl_reg(e, X17, X17, X9);
        orr_reg(e, X1, X1, X17);
        store64_index(e, X1, X0, X10);
        bind(e, counted);
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
    int head = kept_find(k, k->loop_of[pc - k->block.start], C_HEADROOM, 0, 0);
    if (head >= 0 && k->fresh >> head & 1) sub_imm(e, k->kept[head].reg, k->kept[head].reg, bytes); /* the nest's books' */
    slow_back(k, s, V_MEM, 0);
}

/* Whether a column kept of list is there for certain at code in loop: in
 * the integers' code, inside a walk of that list's slice opened in code (a
 * packed list is FLAT; the step's walks of such slices it gives up to the
 * executor, loop_open), whose block does not change the list. */
static bool column_there(const Block *k, int loop, uint32_t list) {
    if (!k->copy) return false;
    for (int l = loop; l >= 0; l = k->loops[l].parent)
        if (k->loops[l].sliced && k->loops[l].list == list) return true;
    return false;
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
    size_t s = (size_t)(slow_at(k, pc, (uintptr_t)pure_run_step, height, height - 1, (int)height - 1) - k->slows);
    if (e->failed) return true;
    k->slows[s].two = true;
    int slow = k->slows[s].label, other = new_label(e), key = new_label(e), absent = new_label(e), done = new_label(e),
        one = new_label(e);
    unsigned r = alloc_x(k, height), column = kept_reg(k, col_c), bits = kept_reg(k, bits_c), limit = kept_reg(k, limit_c);
    if (!column_there(k, loop, list)) cbz_x(e, column, slow);
    unsigned at = value_index(k, height - 1, X1);
    int walk = k->vs[height - 1].walk;
    if (walk < 0 || k->loops[walk].list != list) {
        unsigned count = count_c >= 0 ? kept_reg(k, count_c) : count_reg(k, loop, list, X17);
        cmp_reg(e, at, count);
        b_cond(e, HS, fault_at(k, PURE_FAULT_NOT_FOUND, pc));
    }
    (void)one;
    tbz(e, column, 58, other);
    load32_index(e, X9, column, at);
    bind(e, key);
    cmp_reg(e, X9, limit); /* past the bits (none: no bits at all): out of the line */
    b_cond(e, HS, absent);
    lsr_imm(e, X10, X9, 6);
    load64_index(e, X10, bits, X10);
    lsr_reg(e, r, X10, X9);
    bind(e, done);
    if (!grow_array((void **)&k->colds, &k->cold_room, k->cold_count + 1, sizeof(*k->colds))) {
        e->failed = true;
        return true;
    }
    k->colds[k->cold_count++] = (HasCold){other, absent, key, done, slow, (uint8_t)column, (uint8_t)at, (uint8_t)bits, (uint8_t)r};
    k->vs[height - 1] = (Value){V_BIT, (uint8_t)r, 0, false, false, -1, 0, -1, false, 0};
    slow_back(k, s, V_BIT, r);
    return true;
}

static void has_stubs(Block *k) {
    Emit *e = k->e;
    for (size_t i = 0; i < k->cold_count; ++i) {
        const HasCold *c = &k->colds[i];
        int one = new_label(e);
        bind(e, c->other); /* eight bytes: a key when whole; one byte: one */
        tbz(e, c->column, 59, one);
        load_d_index(e, D0, c->column, c->at);
        fp2(e, FCVTZU_X, X9, D0);
        fp2(e, UCVTF_X, D1, X9);
        fcmp(e, D0, D1);
        b_cond(e, EQ, c->key);
        b(e, c->absent);
        bind(e, one);
        load8_index(e, X9, c->column, c->at);
        b(e, c->key);
        bind(e, c->absent);
        cbz_x(e, c->bits, c->slow);
        mov_w(e, c->r, 0);
        b(e, c->back);
    }
}

static void loop_open(Block *k, uint32_t pc, unsigned height);
static void loop_ends(Block *k, uint32_t pc);
static void enter_body(Block *k, unsigned li);

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
        vs[height] = (Value){V_CONST, 0, 0, false, false, -1, (uint64_t)op->integer, -1, false, 0}; /* the bits: a number's, a bool's or an i64's */
        return;
    case PURE_OP_LOAD: {
        unsigned r = alloc_d(k, height);
        load_d(e, r, X21, arg * sizeof(PureValue));
        vs[height] = (Value){V_D, (uint8_t)r, 0, false, false, -1, 0, -1, false, 0};
        return;
    }
    case PURE_OP_STORE: case PURE_OP_CUR_STORE:
        if (vs[height - 1].kind == V_D) store_d(e, vs[height - 1].reg, X21, arg * sizeof(PureValue));
        else store64(e, value_bits(k, height - 1, X9), X21, arg * sizeof(PureValue));
        return;
    case PURE_OP_LOCAL_LOAD: {
        if (k->copy && k->ints >> (arg & 0x3F) & 1 && (arg & 0xFF) < 64) { /* a whole number its slot holds */
            unsigned r = alloc_x(k, height);
            load_d(e, D0, X28, (arg & 0xFF) * sizeof(PureValue));
            fp2(e, FCVTZS_X, r, D0);
            vs[height] = (Value){V_X, (uint8_t)r, 0, false, false, (int16_t)(arg >> 16), 0, -1, true, 53};
            return;
        }
        unsigned r = alloc_d(k, height);
        load_d(e, r, X28, (arg & 0xFF) * sizeof(PureValue));
        vs[height] = (Value){V_D, (uint8_t)r, 0, false, false, (int16_t)(arg >> 16), 0, -1, false, 0};
        return;
    }
    case PURE_OP_LOCAL_STORE:
        if ((arg & 0xFF) < 64) { /* whether its slot holds a whole number now (the integers' code reads it as one) */
            uint64_t bit = UINT64_C(1) << (arg & 0x3F);
            k->ints = vs[height - 1].kind == V_X ? k->ints | bit : k->ints & ~bit;
        }
        if (vs[height - 1].kind == V_D) store_d(e, vs[height - 1].reg, X28, (arg & 0xFF) * sizeof(PureValue));
        else store64(e, value_bits(k, height - 1, X9), X28, (arg & 0xFF) * sizeof(PureValue));
        return;
    case PURE_OP_ROW: case PURE_OP_OUTER_ROW: {
        int walk = -1;
        for (int l = loop; op->op == PURE_OP_ROW && l >= 0 && walk < 0; l = k->loops[l].parent)
            if (k->loops[l].walk) walk = l;
        int c = walk >= 0 && k->loops[walk].fast ? kept_find(k, loop, C_AT, (unsigned)walk, 0) : -1;
        if (c >= 0) {
            vs[height] = (Value){V_X, (uint8_t)kept_reg(k, c), 0, true, false, (int16_t)arg, 0, (int16_t)walk, false, 25};
            return;
        }
        unsigned r = alloc_x(k, height);
        load64(e, r, X22, op->op == PURE_OP_ROW ? offsetof(Event, row) : offsetof(Event, outer));
        vs[height] = (Value){V_X, (uint8_t)r, 0, false, false, (int16_t)arg, 0, -1, false, 25};
        return;
    }
    case PURE_OP_INDEX_FIRST:
        vs[height] = (Value){V_CONST, 0, 0, false, false, (int16_t)arg, 0, -1, false, 0};
        return;
    case PURE_OP_LIST_COUNT: {
        int c = kept_find(k, loop, C_COUNT, arg, 0);
        if (c >= 0) {
            vs[height] = (Value){V_X, (uint8_t)kept_reg(k, c), 0, true, false, -1, 0, -1, false, 25};
            return;
        }
        unsigned r = alloc_x(k, height), n = count_reg(k, loop, arg, r); /* the books' own: copied, the appends change it */
        if (n != r) mov_reg(e, r, n);
        vs[height] = (Value){V_X, (uint8_t)r, 0, false, false, -1, 0, -1, false, 25};
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
            *v = (Value){V_FLAG, 0, EQ, false, false, -1, 0, -1, false, 0};
        } else {
            fcmp_zero(e, value_d(k, height - 1, D0));
            *v = (Value){V_FLAG, 0, EQ, false, false, -1, 0, -1, false, 0};
        }
        v->owner = -1;
        return;
    }
    case PURE_OP_NEG: case PURE_OP_FLOOR: {
        unsigned r = alloc_d(k, height - 1), x = value_d(k, height - 1, D0);
        fp2(e, op->op == PURE_OP_NEG ? FNEG : FRINTM, r, x);
        vs[height - 1] = (Value){V_D, (uint8_t)r, 0, false, false, -1, 0, -1, false, 0};
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
        if (!known && loop >= 0 && k->loops[k->loops[loop].nest].books) break; /* whose count: the step's, the books written back */
        if (k->copy && known && whole(&vs[height - 2]) && whole(&vs[height - 1])) { /* the integers' code */
            unsigned r = alloc_x(k, height - 2);
            if (vs[height - 2].kind == V_CONST && num_of(vs[height - 2].bits) == 0) mov_reg(e, r, value_whole(k, height - 1, X10));
            else add_reg(e, r, value_whole(k, height - 2, X9), value_whole(k, height - 1, X10));
            unsigned n = count_reg(k, loop, (uint32_t)owner, X17);
            sub_imm(e, X9, n, 1);
            cmp_imm(e, n, 0);
            csel(e, X9, XZR, X9, EQ);
            cmp_reg(e, r, X9);
            csel(e, r, r, X9, LT);
            cmp_imm(e, r, 0);
            csel(e, r, r, XZR, GT);
            vs[height - 2] = (Value){V_X, (uint8_t)r, 0, false, false, (int16_t)owner, 0, -1, false, 25};
            return;
        }
        unsigned r = alloc_d(k, height - 2), x = value_d(k, height - 2, D0), by = value_d(k, height - 1, D1);
        fp2(e, FRINTM, D2, by);
        fcmp(e, by, D2);
        b_cond(e, NE, fault_at(k, PURE_FAULT_LIMIT, pc));
        unsigned n = X10;
        if (known) n = count_reg(k, loop, (uint32_t)owner, X10);
        else {
            load16(e, X10, X20, AT(height - 2) + OWNER);
            load64_index(e, X10, X24, X10);
            load64(e, X10, X10, pure_list_layout[LIST_COUNT_AT]);
        }
        sub_imm(e, X9, n, 1);
        cmp_imm(e, n, 0);
        csel(e, X9, XZR, X9, EQ);
        fp2(e, UCVTF_X, D3, X9);
        fp3(e, FADD, r, x, by);
        fp3(e, FMIN, r, r, D3);
        fp2(e, FMOV_DX, D4, XZR);
        fp3(e, FMAX, r, D4, r);
        vs[height - 2] = (Value){V_D, (uint8_t)r, 0, false, false, (int16_t)(known ? owner : -1), 0, -1, false, 0};
        if (!known) spill(k, height - 2); /* the owner only its slot knows: kept there */
        return;
    }
    case PURE_OP_LIST_HAS: {
        if (op->plan & PLAN_TEXT) break;
        int bits_c = kept_find(k, loop, C_BITS, arg, 0), limit_c = kept_find(k, loop, C_LIMIT, arg, 0);
        unflag_below(k, height);
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
        if (bits_c < 0 || limit_c < 0) cbz_x(e, bits, slow); /* no bits (a table, or none): the step */
        key_bit(k, height - 1, bits, limit, absent, r);
        b(e, done);
        bind(e, absent);
        if (bits_c >= 0 && limit_c >= 0) cbz_x(e, bits, slow); /* the keys kept reach none without bits */
        mov_w(e, r, 0);
        bind(e, done);
        vs[height - 1] = (Value){V_BIT, (uint8_t)r, 0, false, false, -1, 0, -1, false, 0};
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
        size_t s = (size_t)(slow_at(k, pc, (uintptr_t)pure_run_step, height, height - 1, (int)height - 1) - k->slows);
        if (e->failed) return;
        int slow = k->slows[s].label;
        bool ints = k->copy && col_c >= 0 && bits_c >= 0 && limit_c >= 0;
        if (ints) k->slows[s].deopt = deopt_after(k, pc, pc + 2, height);
        unsigned column, bits, limit, r = ints ? alloc_x(k, height) : alloc_d(k, height);
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
        if (ints) {
            load_int(k, r, column, key, deopt_at(k, pc, height));
            vs[height - 1] = (Value){V_X, (uint8_t)r, 0, false, false, -1, 0, -1, false, 32};
            slow_back(k, s, V_X, r);
            return;
        }
        load_cell(k, r, column, key);
        vs[height - 1] = (Value){V_D, (uint8_t)r, 0, false, false, -1, 0, -1, false, 0};
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
        size_t s = (size_t)(slow_at(k, pc, (uintptr_t)pure_run_step, height, height - 1, (int)height - 1) - k->slows);
        if (e->failed) return;
        int slow = k->slows[s].label;
        bool ints = k->copy && col_c >= 0;
        if (ints) k->slows[s].deopt = deopt_after(k, pc, pc + 1, height);
        unsigned r = ints ? alloc_x(k, height) : alloc_d(k, height);
        if (col_c >= 0) {
            unsigned column = kept_reg(k, col_c);
            k->slows[s].tree_list = (int)list, k->slows[s].tree_field = field;
            if (!column_there(k, loop, list)) cbz_x(e, column, slow);
            unsigned at = value_index(k, height - 1, X9), count;
            int walk = vs[height - 1].walk;
            if (walk < 0 || k->loops[walk].list != list) { /* else its place is below its end, which is at most the count */
                count = count_c >= 0 ? kept_reg(k, count_c) : count_reg(k, loop, list, X17);
                cmp_reg(e, at, count);
                b_cond(e, HS, fault_at(k, PURE_FAULT_NOT_FOUND, pc));
            }
            if (ints) {
                load_int(k, r, column, at, deopt_at(k, pc, height));
                vs[height - 1] = (Value){V_X, (uint8_t)r, 0, false, false, -1, 0, -1, false, 32};
                slow_back(k, s, V_X, r);
                return;
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
            vs[height - 1] = (Value){V_D, (uint8_t)r, 0, false, false, -1, 0, -1, false, 0};
            slow_back(k, fill, V_D, r);
            k->slows[s].back = k->slows[fill].back; /* both steps come back here */
            k->slows[s].result_kind = V_D, k->slows[s].result_reg = (uint8_t)r, k->slows[s].fresh = k->fresh;
            return;
        }
        vs[height - 1] = (Value){V_D, (uint8_t)r, 0, false, false, -1, 0, -1, false, 0};
        slow_back(k, s, V_D, r);
        return;
    }
    case PURE_OP_LIST_APPEND:
        if (!event_list(m, arg) || !numbers_only(m, arg) || m->state_indexed[arg]) break;
        if (books(k, loop, arg)) append_books(k, op, pc, height);
        else append(k, op, pc, height);
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
        books_back(k, k->fresh & ~kept_at(k, target_loop)); /* out of the nest: its books back (both ways: they hold) */
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
        b(e, AFTER(k, &k->loops[l]));
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
    int skip = new_label(e), body = l->start < l->end ? OP_LABEL(k, l->start - k->block.start) : AGAIN(k, l);
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
    int work_c = kept_find(k, k->loop_of[pc - k->block.start], C_WORK, 0, 0);
    if (work_c >= 0) { /* what the event may still do, kept */
        unsigned left = kept_reg(k, work_c);
        cmp_reg(e, X10, left);
        b_cond(e, HI, fault_at(k, PURE_FAULT_WORK, pc));
        sub_reg(e, left, left, X10);
    } else {
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
    }
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
    enter_body(k, li);
    (void)body;
    bind(e, skip);
    k->fresh = fresh;
    refresh(k, kept_at(k, l->parent));
    b(e, AFTER(k, l));
    k->fresh = fresh;
}

/* A count's low value whole (below 2^53 in size, not -0), else to fail: its slot then holds whole numbers. */
static void whole_low(Block *k, const LoopAt *l, int fail) {
    load_d(k->e, D0, X27, loop_offset(l) + offsetof(Loop, low));
    int_from_d(k, X0, D0, fail);
}

/* Loop li opened: what its block keeps read, then its block. The code made
 * first, at a nest's outermost loop, goes on in the integers' code when
 * what it assumes holds: the columns kept are of one or four bytes, a
 * count's low value whole. The integers' code at a count: its slot whole,
 * else it gives up. */
static void enter_body(Block *k, unsigned li) {
    Emit *e = k->e;
    LoopAt *l = &k->loops[li];
    const PureOp *op = &k->m->code[l->at];
    refresh(k, kept_at(k, (int)li));
    if (l->start == l->end) {
        b(e, AGAIN(k, l));
        return;
    }
    bool counted = !l->walk && !(op->arg & PURE_LOOP_INTEGER) && (op->arg & 0xFF) < 64;
    if (!k->copy && l->parent < 0 && l->tier >= 0) { /* the tier runs its rounds, the generic code's when it does not */
        write_back(k, (int)li);
        b(e, l->tier);
        return;
    }
    if (!k->copy && l->parent < 0) {
        int made = new_label(e);
        for (unsigned c = 0; c < k->kept_count; ++c)
            if (l->region >> c & 1 && (k->kept[c].kind == C_COLUMN || k->kept[c].kind == C_DENSE))
                tbnz(e, k->kept[c].reg, 59, made); /* eight bytes */
        if (counted) whole_low(k, l, made);
        b(e, k->labels[1][l->start - k->block.start]);
        bind(e, made);
    } else if (k->copy) {
        l->ints = k->ints & ~l->stores;
        if (counted) {
            whole_low(k, l, deopt_at(k, l->start, l->base));
            l->ints |= UINT64_C(1) << (op->arg & 0x3F);
        }
    }
    b(e, OP_LABEL(k, l->start - k->block.start));
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
    b(e, AFTER(k, l));
    k->fresh = 0;
    bind(e, open);
    if (k->copy && l->sliced) { /* the integers' code walks only the slices it opens: the executor walks this one */
        b(e, deopt_at(k, l->start, l->base));
        return;
    }
    enter_body(k, (unsigned)k->opens[pc - k->block.start]);
}

/* A loop's block ran once (iterate, module.c, as code): again (its block's
 * first instruction, an empty block's being this), or the loop closed. */
static void iteration(Block *k, LoopAt *l, unsigned li) {
    Emit *e = k->e;
    const PureOp *op = &k->m->code[l->at];
    int again = l->start == l->end ? AGAIN(k, l) : OP_LABEL(k, l->start - k->block.start), done = new_label(e);
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
        if (!k->copy && l->tier >= 0) { /* the next round the tier's */
            b_cond(e, HS, done);
            write_back(k, (int)(l - k->loops));
            b(e, l->tier);
        } else
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
        unsigned n = count_reg(k, (int)li, op->arg >> 8 & 0xFF, X10); /* the list walked: at most its items now */
        cmp_reg(e, X17, n);
        b_cond(e, HS, done);
        store64(e, X17, X22, offsetof(Event, row));
        if (!k->copy && l->tier >= 0) write_back(k, (int)(l - k->loops)), b(e, l->tier);
        else b(e, again);
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
        int work_c = kept_find(k, (int)li, C_WORK, 0, 0);
        if (work_c >= 0) { /* what the event may still do, kept */
            unsigned left = kept_reg(k, work_c);
            if (cost < 4096) cmp_imm(e, left, (uint32_t)cost);
            else {
                mov_imm(e, X0, cost);
                cmp_reg(e, left, X0);
            }
            b_cond(e, LO, fault_at(k, PURE_FAULT_WORK, l->at));
            if (cost < 4096) sub_imm(e, left, left, (uint32_t)cost);
            else sub_reg(e, left, left, X0);
        } else {
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
        }
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
        if (!k->copy && l->tier >= 0) write_back(k, (int)(l - k->loops)), b(e, l->tier);
        else b(e, again);
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
        bind(e, AGAIN(k, l));
        iteration(k, l, (unsigned)pick);
        join(k, l->base, l->parent);
        bind(e, AFTER(k, l));
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
                    case PURE_OP_LIST_APPEND: { /* an event's list of numbers: its books in registers, when they all fit */
                        uint32_t list = op->arg;
                        if (!event_list(m, list) || !numbers_only(m, list) || m->state_indexed[list]) break;
                        unsigned fields = m->record[m->state_ref[list]].count, key = m->state_key[list];
                        bool all = !key || fields > 1;
                        for (unsigned f = 0; f < fields && all; ++f) {
                            if (key && f == key - 1u) continue;
                            KEEP(nest, key ? C_DENSE : C_COLUMN, list, f);
                            all = kept_find(k, (int)li, key ? C_DENSE : C_COLUMN, list, f) >= 0;
                        }
                        if (key) {
                            KEEP(nest, C_BITS, list, 0);
                            KEEP(nest, C_LIMIT, list, 0);
                            KEEP(nest, C_PENDING, list, 0);
                            all = all && kept_find(k, (int)li, C_BITS, list, 0) >= 0 && kept_find(k, (int)li, C_LIMIT, list, 0) >= 0 &&
                                  kept_find(k, (int)li, C_PENDING, list, 0) >= 0;
                        } else {
                            KEEP(nest, C_LIVE, list, 0);
                            all = all && kept_find(k, (int)li, C_LIVE, list, 0) >= 0;
                        }
                        KEEP(nest, C_HEADROOM, 0, 0);
                        all = all && kept_find(k, (int)li, C_HEADROOM, 0, 0) >= 0;
                        if (all) k->loops[nest].books |= UINT32_C(1) << (list & 31);
                        break;
                    }
                    default: break;
                    }
                }
            }
    for (unsigned li = 0; li < k->loop_count; ++li) { /* what the event may still do: a nest's counts and slices take from it */
        const LoopAt *l = &k->loops[li];
        const PureOp *op = &m->code[l->at];
        if (l->parent >= 0 && ((!l->walk && !(op->arg & PURE_LOOP_INTEGER)) || (l->walk && op->arg & PURE_EACH_SLICED && !event_list(m, l->list))))
            KEEP(l->nest, C_WORK, 0, 0);
        else if (l->parent < 0 && !l->walk && l->start < l->end)
            KEEP(l->nest, C_WORK, 0, 0);
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
    const char *ahead = getenv("PURE_AHEAD"); /* 0: none (measuring what the code costs without it, research 124) */
    if (ahead && !strcmp(ahead, "0")) return;
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
            cmp_reg(e, X0, count_c >= 0 ? kept_reg(k, count_c) : count_reg(k, (int)li, s->list, X10));
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

/* The integers' code: the key's node n as an integer into ahead_x[depth]
 * (what key_ints allows: the round moved on, cells of the columns kept,
 * whole constants, sums and products); to skip when it reads past a list's
 * items or an eight-byte cell. What it computes past 64 bits is read ahead
 * wrong, never read: a bucket past the index is not read. */
static const unsigned ahead_x[] = {X11, X12, X13, X1};

static bool key_ints(const Block *k, unsigned li, const Ahead *a, int n) {
    const Sym *s = &a->nodes[n];
    switch (s->kind) {
    case S_CONST: return s->value == floor(s->value) && fabs(s->value) < 0x1p62;
    case S_ROUND: return true;
    case S_ITEM: return kept_find(k, (int)li, C_COLUMN, s->list, s->field) >= 0 && key_ints(k, li, a, s->a);
    default: return key_ints(k, li, a, s->a) && key_ints(k, li, a, s->b);
    }
}

static void emit_key_x(Block *k, unsigned li, const Ahead *a, int n, unsigned depth, unsigned by, int skip) {
    Emit *e = k->e;
    const Sym *s = &a->nodes[n];
    unsigned r = ahead_x[depth];
    switch (s->kind) {
    case S_CONST: mov_imm(e, r, (uint64_t)(int64_t)s->value); return;
    case S_ROUND: {
        const LoopAt *l = &k->loops[li];
        if (l->walk) add_imm(e, r, kept_reg(k, l->at_kept), by);
        else { /* a count's value: whole in the integers' code */
            load_d(e, D0, X28, (k->m->code[l->at].arg & 0xFF) * sizeof(PureValue));
            fp2(e, FCVTZS_X, r, D0);
            add_imm(e, r, r, by);
        }
        return;
    }
    case S_ITEM: {
        emit_key_x(k, li, a, s->a, depth, by, skip); /* its position */
        int column_c = kept_find(k, (int)li, C_COLUMN, s->list, s->field), count_c = kept_find(k, (int)li, C_COUNT, s->list, 0);
        unsigned column = kept_reg(k, column_c);
        int one = new_label(e), done = new_label(e);
        cbz_x(e, column, skip);
        cmp_reg(e, r, count_c >= 0 ? kept_reg(k, count_c) : count_reg(k, (int)li, s->list, X10));
        b_cond(e, HS, skip);
        tbnz(e, column, 59, skip);
        tbz(e, column, 58, one);
        load32_index(e, r, column, r);
        b(e, done);
        bind(e, one);
        load8_index(e, r, column, r);
        bind(e, done);
        return;
    }
    default: {
        const Sym *x = &a->nodes[s->a], *y = &a->nodes[s->b];
        if (s->kind == S_ADD && x->kind == S_CONST && x->value >= 0 && x->value < 4096) { /* a small constant and a node */
            emit_key_x(k, li, a, s->b, depth, by, skip);
            if (x->value) add_imm(e, r, r, (uint32_t)x->value);
            return;
        }
        emit_key_x(k, li, a, s->a, depth, by, skip);
        if (y->kind == S_CONST && y->value >= 0 && y->value < 4096 && s->kind != S_MUL) {
            if (s->kind == S_ADD) add_imm(e, r, r, (uint32_t)y->value);
            else sub_imm(e, r, r, (uint32_t)y->value);
            return;
        }
        uint64_t c = y->kind == S_CONST && y->value > 0 ? (uint64_t)y->value : 0;
        if (s->kind == S_MUL && c && !(c & (c - 1))) { /* a power of two: a shift */
            unsigned p = 0;
            while (c >> p != 1) ++p;
            lsl_imm(e, r, r, p);
            return;
        }
        emit_key_x(k, li, a, s->b, depth + 1, by, skip);
        unsigned q = ahead_x[depth + 1];
        if (s->kind == S_ADD) add_reg(e, r, r, q);
        else if (s->kind == S_SUB) sub_reg(e, r, r, q);
        else put(e, 0x9B007C00 | q << 16 | r << 5 | r); /* MUL */
        return;
    }
    }
}

/* The key (in d4, or in x register key) as the address of its bucket's
 * first position in a's packed list (x16 the list, x17 its pack, x10 the
 * bucket, x9 the address), or to skip when the list is not packed. Not
 * checked against the index's range: a read ahead may go anywhere (PRFM
 * never faults). */
static void key_bucket(Block *k, const Ahead *a, int skip, int key) {
    Emit *e = k->e;
    if (key < 0) fp2(e, FCVTZS_X, X0, D4);
    load64(e, X16, X24, (size_t)a->list * sizeof(PureList *));
    load64(e, X17, X16, pure_list_layout[LIST_PACK_AT]);
    cbz_x(e, X17, skip);
    load64(e, X9, X17, pure_pack_layout[PACK_ORIGIN_AT]);
    sub_reg(e, X10, key < 0 ? X0 : (unsigned)key, X9);
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
    bool ints = k->copy && key_ints(k, li, a, a->key); /* the integers' code: the key as an integer */
    if (ints) emit_key_x(k, li, a, a->key, 0, AHEAD_FAR, near);
    else emit_key(k, li, a, a->key, 0, AHEAD_FAR, near);
    key_bucket(k, a, near, ints ? (int)ahead_x[0] : -1);
    prefetch(e, X9);
    bind(e, near);
    if (ints) emit_key_x(k, li, a, a->key, 0, AHEAD_NEAR, done);
    else emit_key(k, li, a, a->key, 0, AHEAD_NEAR, done);
    key_bucket(k, a, done, ints ? (int)ahead_x[0] : -1);
    load64(e, X0, X17, pure_pack_layout[PACK_BUCKETS_AT]); /* a bucket of the index: its first position is read, and its end */
    cmp_reg(e, X10, X0);
    b_cond(e, HS, done);
    load32(e, X10, X9, 0);
    load32(e, X1, X9, 4);
    if (column_c >= 0) { /* the tagged address the inner loop keeps: its top byte the cells' width */
        unsigned column = kept_reg(k, column_c);
        cbz_x(e, column, done);
        lsr_imm(e, X9, column, 56);
        mul_reg(e, X10, X10, X9);
        add_reg(e, X17, column, X10);
        mul_reg(e, X1, X1, X9);
        add_reg(e, X1, column, X1);
    } else {
        load64(e, X17, X16, pure_list_layout[LIST_COLUMNS_AT] + a->field * sizeof(void *));
        load8(e, X9, X16, pure_list_layout[LIST_WIDTH_AT] + a->field);
        mul_reg(e, X10, X10, X9);
        mul_reg(e, X1, X1, X9);
        add_reg(e, X1, X17, X1);
        add_reg(e, X17, X17, X10);
    }
    prefetch(e, X17);
    sub_imm(e, X1, X1, 1); /* the bucket's last cell: a slice of more than a line misses there too */
    prefetch(e, X1);
    bind(e, done);
}

/* The block's heights (absolute: a loop's block is verified from its own start), its loops, and each instruction's. */
static bool shape_block(Block *k) {
    const PureModule *m = k->m;
    uint32_t n = k->block.length;
    k->height = malloc(n + 1);
    k->loop_of = malloc((n + 1) * sizeof(*k->loop_of));
    k->labels[0] = malloc((n + 1) * sizeof(*k->labels[0]));
    k->labels[1] = malloc((n + 1) * sizeof(*k->labels[1]));
    k->loops = malloc((n + 1) * sizeof(*k->loops));
    k->opens = malloc((n + 1) * sizeof(*k->opens));
    k->target = calloc(n + 1, sizeof(*k->target));
    if (!k->height || !k->loop_of || !k->labels[0] || !k->labels[1] || !k->loops || !k->opens || !k->target) return false;
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
            for (unsigned c = 0; c < 2; ++c) l->again[c] = new_label(k->e), l->after[c] = new_label(k->e);
            l->depth = depth + 1, l->at = pc;
            l->walk = op->op == PURE_OP_EACH, l->list = op->arg >> 8 & 0xFF, l->base = at - pops;
            l->nest = depth ? k->loops[open[0]].nest : (int)k->loop_count;
            l->ahead = -1;
            l->at_kept = l->end_kept = l->k_kept = l->n_kept = -1;
            l->tier = -1;
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
            const PureOp *op = &m->code[pc];
            l->writes |= changes(m, op);
            l->arena = l->arena || arena_kept(op);
            if (op->op == PURE_OP_LOCAL_STORE || op->op == PURE_OP_LOOP) l->stores |= UINT64_C(1) << (op->arg & 0x3F);
        }
        l->fast = l->walk && !(l->writes >> (l->list & 31) & 1);
        l->sliced = l->fast && m->code[l->at].arg & PURE_EACH_SLICED && !event_list(m, l->list) && l->start < l->end;
    }
    for (uint32_t i = 0; i <= n; ++i) k->labels[0][i] = new_label(k->e), k->labels[1][i] = new_label(k->e);
    k->exit = new_label(k->e);
    plan_kept(k);
    for (unsigned li = 0; li < k->loop_count; ++li)
        if (k->loops[li].start < k->loops[li].end && (k->loops[li].walk ? k->loops[li].fast : true)) plan_ahead(k, li);
    return !k->e->failed;
}

/* The integers' code for the nest of loop li (docs/design/022): its block
 * again, values read from narrow columns and their arithmetic as integers;
 * where that does not hold it gives up (deopt_stubs) into the code made
 * first, which it also leaves for past the loop. */
static void compile_ints(Block *k, unsigned li) {
    Emit *e = k->e;
    LoopAt *l = &k->loops[li];
    const PureOp *op = &k->m->code[l->at];
    k->copy = 1;
    for (unsigned h = 0; h <= PURE_MODULE_STACK; ++h) k->vs[h] = MEMORY;
    k->fresh = kept_at(k, (int)li);
    l->ints = !l->walk && !(op->arg & PURE_LOOP_INTEGER) && (op->arg & 0xFF) < 64 ? UINT64_C(1) << (op->arg & 0x3F) : 0;
    k->ints = l->ints;
    for (uint32_t pc = l->start; pc < l->end;) {
        loop_ends(k, pc);
        if (k->target[pc - k->block.start]) join(k, k->height[pc - k->block.start], k->loop_of[pc - k->block.start]);
        bind(e, OP_LABEL(k, pc - k->block.start));
        int in = k->loop_of[pc - k->block.start];
        if (in >= 0 && k->loops[in].start == pc && k->loops[in].ahead >= 0) read_ahead(k, (unsigned)in);
        compile_op(k, pc);
        const PureOp *at = &k->m->code[pc];
        pc = at->op == PURE_OP_EACH || at->op == PURE_OP_LOOP ? pc + 1 : after(at, pc) + k->joined;
        k->joined = false;
    }
    loop_ends(k, l->end);
    b(e, l->after[0]); /* past the loop: the code made first goes on */
    k->copy = 0;
}

/* ---- The tier (docs/design/023)
 * A nest of loops (an outermost loop and those in it) run as SSA: the
 * stack's values, the locals and the states its code uses are values, held
 * in registers for as long as they live; a whole number is an integer where
 * its facts say it is (a count, a narrow cell, a message's u32, a sum of
 * those), with no check its range does not need. Nothing is called on the
 * way: what is rare (a fault, a cell that does not fit its column, a list in
 * another form, a check the facts could not drop) gives up where it is,
 * the stack and what changed written as the executor keeps them, the rest
 * of the round run by the executor (pure_run_rest), the generic code going
 * on from there (copy 0), and the next round of the outermost loop coming
 * back. It is entered at a round of the outermost loop from the generic
 * code (the loop opened, its state in memory), and when the loop ends it
 * writes the loop's state as after its last round and goes on at the
 * generic code's iteration of it, which closes it. */

enum { R_NUM, R_INT, R_BOOL }; /* a value's register: d a number; x a whole number as an integer; x 0 or 1 */
enum {
    T_CONST, T_IN, T_FIELD, T_PHI, T_COUNT, T_ADD, T_SUB, T_MUL, T_DIV, T_MIN, T_MAX, T_NEG, T_FLOOR,
    T_LT, T_LE, T_GT, T_GE, T_EQ, T_NE, T_NOT, T_AND, T_OR, T_GET, T_FIND_GET, T_FIND, T_HAS, T_SET, T_MOVE, T_SPAN, T_STEP0, T_STRIDE
};
/* T_IN's variables past the locals, states and stack: the outermost loop's round, its rounds or items, its low value. */
enum { V_ROUND0 = -10, V_COUNT0 = -11, V_LOW0 = -12 };
/* What a number is to the code that reads it: a number, a bool, an index (of the list in list). */
enum { K_NUM, K_BOOL, K_INDEX };
/* A variable: a local (0-63), a state (64-95), a stack slot where ways meet (96-160). */
/* Past them, a loop's pointer to a column (tier_steps): a phi at its head, no other code's variable. */
enum { V_STATE = 64, V_STACK = 96, V_STEP = 96 + PURE_MODULE_STACK + 1, STEPS = 8, VARS = V_STEP + STEPS };

typedef struct {
    uint8_t op, rep, kind;
    bool integers;     /* T_DIV: whole numbers divided as integers (its one use a FLOOR, docs/design/023) */
    bool negzero;      /* T_MUL of integers: 0 times a number below 0 is -0 (checked: else it gives up) */
    bool bounded;      /* an integer its loop's opening bounds (its ends checked there: no check of its own) */
    bool proven;       /* a list access whose key or position its loop's opening found in range */
    bool fused;        /* a comparison only the branch right after it reads: its flags, no register */
    bool deferred;     /* a number's arithmetic whose finiteness its one reader, right after it, checks (tier_defer) */
    int step;          /* a list access: the pointer to its cell (a loop's phi, tier_steps), or -1 */
    int retry;         /* where its finiteness check gives up, the first of such a run's snapshot; or -1: its own */
    bool aliased;      /* a full list's position found: its key's register (they are equal) */
    uint16_t list, field;
    int a, b, c, d;    /* operands: values, or -1 */
    uint32_t pc;       /* its instruction */
    int snap;          /* where it gives up (a snapshot), or -1 */
    int loop;          /* the tier's loop it is in, or -1 (the outermost's preheader) */
    double num;        /* T_CONST */
    int var;           /* T_IN, T_PHI: the variable */
    int label;         /* T_PHI: the merge it is at */
    int *in;           /* T_PHI: a value for each way in */
    size_t ins, in_room;
    double lo, hi;     /* what its values may be: from lo to hi, */
    bool whole, zero;  /* each a whole number; -0 possible */
    unsigned uses;
    int at, last;      /* where it is made and last read (positions in the code) */
    int reg;           /* its register, or -1 */
} TVal;

/* A loop of the nest: its Block loop, its kind, and its values. */
typedef struct {
    int li;            /* k->loops index */
    int bounds[16][2]; /* keys and positions in range for every round: (value, the limit it is below), checked as it opens */
    unsigned bound_count;
    bool walk;
    uint32_t list;     /* a walk's */
    int parent;        /* the tier's loop around it, or -1 */
    int count, low, k; /* a count's values (k: its round, a phi), or a walk's: count its items, k its place */
    int head, end, exit; /* merges: its round's start (its phis), its block's end (iteration), past it */
    unsigned length;   /* its block's instructions */
    int slot;          /* a count's local slot */
    int open_snap;     /* where its opening (its work) gives up */
    int last;          /* past it: its round when it ended (the outermost's, the generic code's iteration closes it) */
} TLoop;

/* Where the code may give up: the instruction, the stack's height, and the
 * value of every variable there (a stack slot's too). */
typedef struct {
    uint32_t pc;
    unsigned height;
    int loop;          /* the innermost tier loop open there, or -1 */
    int *vars;         /* VARS values */
    int label;         /* its stub's */
    bool needed;       /* something gives up there (a stub made) */
} TSnap;

/* A merge: where ways meet (a branch's target, a loop's head, end or exit):
 * its label, its phis, the variables' values along each way in. */
typedef struct {
    int label;
    int *phis;         /* a phi per variable differing between ways, or -1 (VARS) */
    int **ways;        /* each way's variables (VARS each) */
    size_t way_count, way_room;
    unsigned height;
    bool reached, back; /* a way has come; a loop's head (its last way, the back edge, comes after) */
    int loop;          /* a loop's merge: the loop */
} TMerge;

/* The code in order: a value made, a merge bound, a branch or jump to one, a loop's parts. */
enum { I_VALUE, I_MERGE, I_UNLESS, I_JUMP, I_OPEN, I_LATCH, I_BREAK, I_DONE };
typedef struct { uint8_t kind; int value, merge, loop, way, snap; } TItem; /* way: into merge; snap: where it gives up */

typedef struct {
    Block *k;
    int nest;              /* k->loops index of the outermost loop */
    TVal *vals;
    size_t val_count, val_room;
    TLoop loops[PURE_MODULE_NESTING];
    unsigned loop_count;
    TSnap *snaps;
    size_t snap_count, snap_room;
    TMerge *merges;
    size_t merge_count, merge_room;
    TItem *items;
    size_t item_count, item_room;
    int *merge_at;         /* by instruction (from the nest's start): the merge a branch lands on, or -1 */
    int *entry;            /* the values made where the tier begins, in order */
    size_t entry_count, entry_room;
    int state[VARS];       /* the values as the code goes: each variable's, -1 none yet */
    unsigned height;       /* the stack's */
    bool live;             /* the code here is reached */
    int cur_loop;
    uint64_t assigned[3];  /* the variables the nest assigns (locals, states) */
    uint32_t written;      /* the lists it changes (ITEM_SET) */
    uint32_t used;         /* the lists it reads */
    bool failed;           /* it cannot be the tier's */
    const char *why;
    int input;             /* its handler's message input, or -1 */
    struct TList *lists;   /* what each list needs (tier_lists) */
    uint64_t *live_in;     /* by instruction from the block's start: the locals read before written from there (tier_liveness) */
    int fields[64];        /* the message's fields read: each once (+1; 0 none yet) */
    uint64_t slots;        /* the nest's loops' slots (kept as live: their loops write them) */
    struct TProbe *probes; /* HASHED lists' finds, made after the items (tier_probe) */
    size_t probe_count, probe_room;
} Tier;

/* A find in a HASHED list, out of the line: value v's, its result r, its key's and bits' registers, its labels. */
typedef struct TProbe { int v; unsigned r, key, bits; int hashed, fail, done; } TProbe;

static void titem(Tier *t, unsigned kind, int value, int merge, int loop);

static int tval(Tier *t, unsigned op, unsigned rep, int a, int b, int c) {
    if (!grow_array((void **)&t->vals, &t->val_room, t->val_count + 1, sizeof(*t->vals))) {
        t->failed = true;
        return 0;
    }
    TVal *v = &t->vals[t->val_count];
    memset(v, 0, sizeof(*v));
    v->op = (uint8_t)op, v->rep = (uint8_t)rep, v->a = a, v->b = b, v->c = c;
    v->snap = -1, v->loop = t->cur_loop, v->var = -1, v->label = -1, v->reg = -1, v->d = -1, v->retry = -1, v->step = -1;
    v->lo = -INFINITY, v->hi = INFINITY;
    int made = (int)t->val_count++;
    if (op == T_PHI) return made; /* bound at its merge */
    if (t->cur_loop < 0) { /* where the tier begins */
        if (!grow_array((void **)&t->entry, &t->entry_room, t->entry_count + 1, sizeof(*t->entry))) t->failed = true;
        else t->entry[t->entry_count++] = made;
    } else
        titem(t, I_VALUE, made, -1, t->cur_loop);
    return made;
}

static void titem(Tier *t, unsigned kind, int value, int merge, int loop) {
    if (!grow_array((void **)&t->items, &t->item_room, t->item_count + 1, sizeof(*t->items))) {
        t->failed = true;
        return;
    }
    t->items[t->item_count++] = (TItem){(uint8_t)kind, value, merge, loop, -1, -1};
}

static int tmerge(Tier *t, int loop) {
    if (!grow_array((void **)&t->merges, &t->merge_room, t->merge_count + 1, sizeof(*t->merges))) {
        t->failed = true;
        return 0;
    }
    TMerge *m = &t->merges[t->merge_count];
    memset(m, 0, sizeof(*m));
    m->label = new_label(t->k->e);
    m->loop = loop;
    return (int)t->merge_count++;
}

/* The way here into merge m: the variables as they are. */
static int tway(Tier *t, int m) {
    TMerge *g = &t->merges[m];
    int *copy = malloc(VARS * sizeof(int));
    if (!copy || !grow_array((void **)&g->ways, &g->way_room, g->way_count + 1, sizeof(*g->ways))) {
        free(copy);
        t->failed = true;
        return 0;
    }
    memcpy(copy, t->state, VARS * sizeof(int));
    g->ways[g->way_count] = copy;
    g->height = t->height;
    g->reached = true;
    return (int)g->way_count++;
}

static void tphi_in(Tier *t, int phi, int value) {
    TVal *p = &t->vals[phi];
    if (!grow_array((void **)&p->in, &p->in_room, p->ins + 1, sizeof(*p->in))) {
        t->failed = true;
        return;
    }
    p->in[p->ins++] = value;
}

/* Where the code gives up at pc, the variables as they are (the stack's below height). */
static int tsnap(Tier *t, uint32_t pc) {
    if (!grow_array((void **)&t->snaps, &t->snap_room, t->snap_count + 1, sizeof(*t->snaps))) {
        t->failed = true;
        return 0;
    }
    TSnap *s = &t->snaps[t->snap_count];
    s->vars = malloc(VARS * sizeof(int));
    if (!s->vars) {
        t->failed = true;
        return 0;
    }
    memcpy(s->vars, t->state, VARS * sizeof(int));
    uint64_t live = t->slots; /* the locals the executor may read from pc: the rest need not be written */
    if (t->live_in) {
        live |= t->live_in[pc - t->k->block.start];
        for (unsigned l = 0; l < t->k->loop_count; ++l) /* a loop's end: its next round too */
            if (t->k->loops[l].end == pc && t->k->loops[l].at >= t->k->loops[t->nest].at) live |= t->live_in[t->k->loops[l].start - t->k->block.start];
    }
    for (int var = 0; var < V_STATE; ++var)
        if (!(live >> var & 1)) s->vars[var] = -1;
    s->pc = pc, s->height = t->height, s->loop = t->cur_loop, s->label = new_label(t->k->e), s->needed = false;
    return (int)t->snap_count++;
}

/* ---- Which nests: what the tier's code can do */

static bool tier_list(const PureModule *m, uint32_t list) { return event_list(m, list) && numbers_only(m, list); }

/* Whether the tier has code for every instruction from at to end (a nest), its handler's message input given; what it assigns and which lists it reads and sets, in t when given. */
static bool tier_supports(const PureModule *m, int input, uint32_t at, uint32_t end, Tier *t) {
    Tier none = {0};
    if (!t) t = &none;
    const char *off = getenv("PURE_TIER");
    if ((off && !strcmp(off, "0")) || input < 0) return false;
    t->input = input;
    for (uint32_t pc = at; pc < end; pc = after(&m->code[pc], pc)) {
        const PureOp *op = &m->code[pc];
        uint32_t arg = op->arg;
        switch (op->op) {
        case PURE_OP_CONST: if (arg > 1) return false; break; /* a number or a bool */
        case PURE_OP_LOAD: case PURE_OP_STORE:
            if (m->state_type[arg] != PURE_STATE_NUM && m->state_type[arg] != PURE_STATE_BOOL) return false;
            t->assigned[1] |= op->op == PURE_OP_STORE ? UINT64_C(1) << arg : 0;
            break;
        case PURE_OP_LOCAL_LOAD: case PURE_OP_LOCAL_STORE:
            if ((arg & 0xFF) >= 64 || (arg >> 8 & 0xFF) > 2) return false; /* a number, an index or a bool */
            t->assigned[0] |= op->op == PURE_OP_LOCAL_STORE ? UINT64_C(1) << (arg & 0x3F) : 0;
            break;
        case PURE_OP_FIELD: {
            const PureField *f = &m->field[m->input[t->input].field_start + arg];
            if (f->type != PURE_FIELD_U32 && f->type != PURE_FIELD_NUMBER && f->type != PURE_FIELD_BOOL) return false;
            break;
        }
        case PURE_OP_ADD: case PURE_OP_SUB: case PURE_OP_MUL: case PURE_OP_DIV: case PURE_OP_MIN: case PURE_OP_MAX:
        case PURE_OP_NEG: case PURE_OP_FLOOR: case PURE_OP_LT: case PURE_OP_LE: case PURE_OP_GT: case PURE_OP_GE: case PURE_OP_EQ:
        case PURE_OP_NE: case PURE_OP_NOT: case PURE_OP_AND: case PURE_OP_OR: case PURE_OP_JZ: case PURE_OP_JMP:
        case PURE_OP_BREAK:
            break;
        case PURE_OP_LOOP: if (arg & PURE_LOOP_INTEGER || (arg & 0xFF) >= 64) return false; break;
        case PURE_OP_EACH:
            if (arg & PURE_EACH_SLICED || op->range || !tier_list(m, arg >> 8 & 0xFF)) return false;
            t->used |= UINT32_C(1) << (arg >> 8 & 31);
            break;
        case PURE_OP_ROW: case PURE_OP_INDEX_FIRST: case PURE_OP_LIST_COUNT:
            if (!tier_list(m, arg)) return false;
            t->used |= UINT32_C(1) << (arg & 31);
            break;
        case PURE_OP_INDEX_MOVE: break;
        case PURE_OP_ITEM_GET: case PURE_OP_ITEM_SET:
            if (op->plan & PLAN_TEXT || !tier_list(m, list_of(arg))) return false;
            t->used |= UINT32_C(1) << (list_of(arg) & 31);
            if (op->op == PURE_OP_ITEM_SET) {
                if (m->state_key[list_of(arg)] && item_field(arg) == m->state_key[list_of(arg)] - 1u) return false; /* a key moves its item */
                t->written |= UINT32_C(1) << (list_of(arg) & 31);
            }
            break;
        case PURE_OP_LIST_FIND: case PURE_OP_LIST_HAS:
            if (op->plan & PLAN_TEXT || !tier_list(m, arg) || !m->state_key[arg]) return false;
            if (op->op == PURE_OP_LIST_FIND && op->plan & PLAN_JOINED && m->code[pc + 1].plan & PLAN_TEXT) return false;
            t->used |= UINT32_C(1) << (arg & 31);
            break;
        default: return false;
        }
    }
    return true;
}

/* Each instruction's locals read before written from it on (the block's ways: branches, a loop's next round, BREAK), by backward passes. */
static void tier_liveness(Tier *t) {
    Block *k = t->k;
    const PureModule *m = k->m;
    uint32_t n = k->block.length, start = k->block.start;
    t->live_in = calloc(n + 1, sizeof(uint64_t));
    if (!t->live_in) {
        t->failed = true;
        return;
    }
    for (bool changed = true; changed;) {
        changed = false;
        for (uint32_t i = n; i-- > 0;) {
            uint32_t pc = start + i;
            const PureOp *op = &m->code[pc];
            uint32_t next = op->op == PURE_OP_EACH || op->op == PURE_OP_LOOP ? pc + 1 : after(op, pc);
            uint32_t succ[4];
            unsigned count = 0;
            if (op->op == PURE_OP_JMP) succ[count++] = op->arg;
            else if (op->op == PURE_OP_BREAK) {
                int l = k->loop_of[i];
                succ[count++] = l >= 0 ? k->loops[l].end : start + n;
            } else {
                succ[count++] = next;
                if (op->op == PURE_OP_JZ) succ[count++] = op->arg;
                if (op->op == PURE_OP_EACH || op->op == PURE_OP_LOOP) succ[count++] = pc + 1 + (op->arg >> 16);
            }
            uint64_t out = 0;
            for (unsigned c = 0; c < count; ++c) {
                uint32_t to = succ[c];
                for (int l = k->loop_of[i]; l >= 0; l = k->loops[l].parent) /* a loop's end: its next round as well */
                    if (k->loops[l].end == to) out |= t->live_in[k->loops[l].start - start];
                if (to >= start && to - start <= n) out |= t->live_in[to - start];
            }
            uint64_t in = out;
            if (op->op == PURE_OP_LOCAL_STORE && (op->arg & 0xFF) < 64) in &= ~(UINT64_C(1) << (op->arg & 0x3F));
            if (op->op == PURE_OP_LOCAL_LOAD && (op->arg & 0xFF) < 64) in |= UINT64_C(1) << (op->arg & 0x3F);
            if (in != t->live_in[i]) t->live_in[i] = in, changed = true;
        }
    }
}

/* Whether the nest at li is the tier's to run, its every instruction one it has code for. */
static bool tier_nest(Tier *t, unsigned li) {
    const LoopAt *l = &t->k->loops[li];
    return l->parent < 0 && l->start < l->end && tier_supports(t->k->m, t->input, l->at, l->end, t);
}

/* ---- The tier: the nest as values */

static void tfacts(Tier *t, int v, bool whole, double lo, double hi, bool zero) {
    TVal *x = &t->vals[v];
    x->whole = whole, x->lo = lo, x->hi = hi, x->zero = zero;
}

static int tconst(Tier *t, double n) {
    int v = tval(t, T_CONST, R_NUM, -1, -1, -1);
    t->vals[v].num = n;
    bool whole = n == floor(n) && fabs(n) < 0x1p53 && !(n == 0 && signbit(n));
    tfacts(t, v, whole, n, n, n == 0 && signbit(n));
    return v;
}

/* Something read where the tier begins: a variable (a local or a state, as it is in memory), or the outermost loop's round, rounds or low value. */
static int tin(Tier *t, int var, unsigned kind, uint16_t owner) {
    int saved = t->cur_loop;
    t->cur_loop = -1;
    int v = tval(t, T_IN, R_NUM, -1, -1, -1);
    t->cur_loop = saved;
    t->vals[v].var = var, t->vals[v].kind = (uint8_t)kind, t->vals[v].list = owner;
    return v;
}

/* Variable var as the code has it here (read where the tier begins when it has none yet). */
static int tvar(Tier *t, int var, unsigned kind, uint16_t owner) {
    if (t->state[var] < 0) t->state[var] = tin(t, var, kind, owner);
    return t->state[var];
}

static void tpush(Tier *t, int v) {
    if (t->height >= PURE_MODULE_STACK) {
        t->failed = true;
        return;
    }
    t->state[V_STACK + t->height++] = v;
}

static int tpop(Tier *t) {
    if (!t->height) {
        t->failed = true;
        return 0;
    }
    return t->state[V_STACK + --t->height];
}

/* The merges where forward branches land (a loop's end is its loop's own); false when one goes back or out. */
static void tier_merges(Tier *t) {
    Block *k = t->k;
    const PureModule *m = k->m;
    const LoopAt *l0 = &k->loops[t->nest];
    for (uint32_t pc = l0->start; pc < l0->end; pc = after(&m->code[pc], pc)) {
        const PureOp *op = &m->code[pc];
        if (op->op != PURE_OP_JZ && op->op != PURE_OP_JMP) continue;
        bool landing = false; /* the end of a loop around it: that loop's iteration */
        for (int l = k->loop_of[pc - k->block.start]; l >= 0; l = k->loops[l].parent)
            landing = landing || k->loops[l].end == op->arg;
        if (landing) continue;
        if (op->arg <= pc || op->arg >= l0->end) {
            t->failed = true;
            return;
        }
        if (t->merge_at[op->arg - l0->start] < 0) t->merge_at[op->arg - l0->start] = tmerge(t, -1);
    }
}

/* The tier's loop whose iteration a branch at pc to target lands on, or -1. */
static int tier_landing(Tier *t, uint32_t pc, uint32_t target) {
    for (int l = t->k->loop_of[pc - t->k->block.start]; l >= 0; l = t->k->loops[l].parent)
        if (t->k->loops[l].end == target)
            for (unsigned i = 0; i < t->loop_count; ++i)
                if (t->loops[i].li == l) return (int)i;
    return -1;
}

/* A way from here into merge g: a branch (cond its bool, I_UNLESS) or a jump. */
static void tier_go(Tier *t, int g, unsigned kind, int cond) {
    int way = tway(t, g);
    titem(t, kind, cond, g, t->cur_loop);
    if (!t->failed) t->items[t->item_count - 1].way = way;
}

/* Whether a loop's block assigns var (the loop's own round: always). */
static bool tassigned(const Tier *t, int var) {
    if (var < V_STATE) return t->assigned[0] >> var & 1;
    if (var < V_STACK) return t->assigned[1] >> (var - V_STATE) & 1;
    return var == V_STACK + PURE_MODULE_STACK;
}

/* Merge g reached by the code going on (when it does) and its ways: a phi
 * for each variable they differ in (a loop's head: each its block assigns,
 * the back edge's input to come), bound. */
static void tier_arrive(Tier *t, int g) {
    TMerge *m = &t->merges[g];
    if (t->live) tier_go(t, g, I_JUMP, -1);
    if (!m->reached) {
        t->live = false;
        return;
    }
    m->phis = malloc(VARS * sizeof(int));
    if (!m->phis) {
        t->failed = true;
        return;
    }
    for (int var = 0; var < VARS; ++var) {
        m->phis[var] = -1;
        if (var >= V_STACK && var < V_STACK + PURE_MODULE_STACK && (unsigned)(var - V_STACK) >= m->height) {
            t->state[var] = -1;
            continue;
        }
        int first = m->ways[0][var];
        bool differ = m->back && tassigned(t, var);
        for (unsigned w = 1; w < m->way_count; ++w) differ = differ || m->ways[w][var] != first;
        if (differ && var < V_STACK) { /* a variable one way has not read: as it is where the tier begins (the same value) */
            int known = -1;
            for (unsigned w = 0; w < m->way_count && known < 0; ++w) known = m->ways[w][var];
            bool in = known >= 0 && t->vals[known].op == T_IN && t->vals[known].var == var;
            for (unsigned w = 0; w < m->way_count; ++w)
                if (m->ways[w][var] < 0) m->ways[w][var] = in ? known : tvar(t, var, K_NUM, 0);
            first = m->ways[0][var];
            differ = m->back && tassigned(t, var);
            for (unsigned w = 1; w < m->way_count; ++w) differ = differ || m->ways[w][var] != first;
        }
        if (!differ) {
            t->state[var] = first;
            continue;
        }
        first = m->ways[0][var];
        int phi = tval(t, T_PHI, R_NUM, -1, -1, -1);
        t->vals[phi].var = var, t->vals[phi].label = g, t->vals[phi].loop = t->cur_loop; /* the loop the merge is in (a loop's exit: around it) */
        t->vals[phi].kind = first >= 0 ? t->vals[first].kind : K_NUM, t->vals[phi].list = first >= 0 ? t->vals[first].list : 0;
        for (unsigned w = 0; w < m->way_count; ++w) tphi_in(t, phi, m->ways[w][var]);
        m->phis[var] = phi;
        t->state[var] = phi;
    }
    t->height = m->height;
    t->live = true;
    titem(t, I_MERGE, -1, g, t->cur_loop);
}

/* The way back from here to a loop's head g: its phis' last inputs. */
static void tier_back(Tier *t, int g) {
    TMerge *m = &t->merges[g];
    (void)tway(t, g);
    for (int var = 0; var < VARS; ++var)
        if (m->phis[var] >= 0) tphi_in(t, m->phis[var], t->state[var] >= 0 ? t->state[var] : tvar(t, var, K_NUM, 0));
}

/* A value of two operands at pc; where it gives up, its operands still on the stack. */
static int tbinary(Tier *t, unsigned op, uint32_t pc, bool fails) {
    int snap = fails ? tsnap(t, pc) : -1;
    int b = tpop(t), a = tpop(t);
    int v = tval(t, op, R_NUM, a, b, -1);
    t->vals[v].pc = pc, t->vals[v].snap = snap;
    return v;
}

/* A value of one operand at pc, its operand on the stack where it gives up. */
static int tunary(Tier *t, unsigned op, unsigned rep, uint32_t pc, bool fails) {
    int snap = fails ? tsnap(t, pc) : -1;
    int v = tval(t, op, rep, tpop(t), -1, -1);
    t->vals[v].pc = pc, t->vals[v].snap = snap;
    return v;
}

static void tier_code(Tier *t, uint32_t pc, uint32_t end);

/* Loop n's head (a phi for each variable its block assigns, and its round), its block, its iteration, and past it. */
static void tier_round(Tier *t, int n, uint32_t at) {
    Block *k = t->k;
    const PureModule *m = k->m;
    TLoop *l = &t->loops[n];
    const LoopAt *bl = &k->loops[l->li];
    uint64_t outer[2] = {t->assigned[0], t->assigned[1]};
    t->assigned[0] = t->assigned[1] = 0;
    for (uint32_t pc = bl->start; pc < bl->end; ++pc) { /* what its block assigns */
        const PureOp *o = &m->code[pc];
        if (o->op == PURE_OP_LOCAL_STORE) t->assigned[0] |= UINT64_C(1) << (o->arg & 0x3F);
        if (o->op == PURE_OP_STORE) t->assigned[1] |= UINT64_C(1) << o->arg;
        if (o->op == PURE_OP_LOOP) t->assigned[0] |= UINT64_C(1) << (o->arg & 0x3F);
    }
    if (!l->walk) t->assigned[0] |= UINT64_C(1) << (l->slot & 0x3F);
    l->head = tmerge(t, n);
    TMerge *head = &t->merges[l->head];
    head->back = true;
    int way = tway(t, l->head);
    (void)way;
    head = &t->merges[l->head];
    head->phis = malloc(VARS * sizeof(int));
    if (!head->phis) {
        t->failed = true;
        return;
    }
    titem(t, I_JUMP, -1, l->head, t->cur_loop);
    t->items[t->item_count - 1].way = 0;
    t->cur_loop = n;
    for (int var = 0; var < VARS; ++var) {
        head->phis[var] = -1;
        if (var >= V_STACK && var < V_STACK + PURE_MODULE_STACK) {
            t->state[var] = var - V_STACK < (int)bl->base ? head->ways[0][var] : -1;
            continue;
        }
        int in = head->ways[0][var];
        if (!tassigned(t, var)) {
            t->state[var] = in;
            continue;
        }
        if (in < 0) in = head->ways[0][var] = tin(t, var, K_NUM, 0);
        int phi = tval(t, T_PHI, R_NUM, -1, -1, -1);
        t->vals[phi].var = var, t->vals[phi].label = l->head, t->vals[phi].loop = n;
        t->vals[phi].kind = t->vals[in].kind, t->vals[phi].list = t->vals[in].list;
        tphi_in(t, phi, in);
        head->phis[var] = phi;
        t->state[var] = phi;
    }
    t->assigned[0] |= outer[0], t->assigned[1] |= outer[1];
    l->k = head->phis[V_STACK + PURE_MODULE_STACK];
    if (l->walk) t->vals[l->k].kind = K_INDEX, t->vals[l->k].list = (uint16_t)l->list;
    t->height = head->height = bl->base;
    t->live = true;
    titem(t, I_MERGE, -1, l->head, n);
    if (!l->walk) { /* its slot holds low + its round (its round when low is 0) */
        const TVal *low = &t->vals[l->low];
        if (low->op == T_CONST && low->num == 0 && !signbit(low->num)) t->state[l->slot] = l->k;
        else {
            int i = tval(t, T_ADD, R_NUM, l->low, l->k, -1);
            t->vals[i].pc = at;
            t->state[l->slot] = i;
        }
    }
    l->end = tmerge(t, n);
    tier_code(t, bl->start, bl->end);
    if (t->failed) return;
    tier_arrive(t, l->end); /* its block ran: its iteration */
    if (t->live) {
        int one = tconst(t, 1);
        int next = tval(t, T_ADD, R_INT, l->k, one, -1);
        t->vals[next].pc = at;
        t->state[V_STACK + PURE_MODULE_STACK] = next;
        titem(t, I_LATCH, next, l->head, n);
        int latch = (int)t->item_count - 1, snap = tsnap(t, bl->end); /* a count's work for its next round: where it gives up, the generic code's iteration */
        tier_back(t, l->head);
        int out = tway(t, l->exit); /* done: past it */
        if (!t->failed) t->items[latch].snap = snap, t->items[latch].way = out;
    }
    t->cur_loop = l->parent;
    t->live = false;
}

/* A loop opened at pc (EACH or LOOP): what it runs over, its work, its rounds, and past it. */
static void tier_loop(Tier *t, uint32_t pc) {
    Block *k = t->k;
    const PureOp *op = &k->m->code[pc];
    if (t->loop_count == PURE_MODULE_NESTING) {
        t->failed = true;
        return;
    }
    int n = (int)t->loop_count++;
    TLoop *l = &t->loops[n];
    memset(l, 0, sizeof(*l));
    l->li = k->opens[pc - k->block.start], l->walk = op->op == PURE_OP_EACH, l->parent = t->cur_loop, l->length = op->arg >> 16;
    l->list = op->arg >> 8 & 0xFF, l->slot = op->arg & 0xFF;
    l->exit = tmerge(t, n);
    l->open_snap = tsnap(t, pc); /* its bounds (a count's) and its work, where they give up */
    if (l->walk) {
        l->count = tval(t, T_COUNT, R_INT, -1, -1, -1);
        t->vals[l->count].list = (uint16_t)l->list, t->vals[l->count].pc = pc;
        tfacts(t, l->count, true, 0, 0x1p32, false);
        l->low = -1;
    } else { /* its rounds: max(high - low, 0), its bounds whole numbers within 2^53 */
        int high = tpop(t), low = tpop(t);
        l->count = tval(t, T_SPAN, R_INT, low, high, -1);
        t->vals[l->count].pc = pc, t->vals[l->count].snap = l->open_snap;
        tfacts(t, l->count, true, 0, 0x1p54, false);
        l->low = low;
    }
    titem(t, I_OPEN, -1, l->exit, n);
    int skip = tway(t, l->exit); /* none to run: past it */
    if (!t->failed) t->items[t->item_count - 1].way = skip, t->items[t->item_count - 1].snap = l->open_snap;
    t->state[V_STACK + PURE_MODULE_STACK] = tconst(t, 0);
    tier_round(t, n, pc);
    if (t->failed) return;
    tier_arrive(t, t->loops[n].exit);
}

/* The instructions from pc to end, as values and items. */
static void tier_code(Tier *t, uint32_t pc, uint32_t end) {
    Block *k = t->k;
    const PureModule *m = k->m;
    const LoopAt *l0 = &k->loops[t->nest];
    while (pc < end && !t->failed) {
        int g = t->merge_at[pc - l0->start];
        if (g >= 0) tier_arrive(t, g);
        const PureOp *op = &m->code[pc];
        uint32_t arg = op->arg, next = after(op, pc);
        if (!t->live) { /* nothing reaches it */
            pc = op->op == PURE_OP_EACH || op->op == PURE_OP_LOOP ? pc + 1 + (arg >> 16) : next;
            continue;
        }
        switch (op->op) {
        case PURE_OP_CONST: {
            int v = tconst(t, op->value);
            if (arg == 1) t->vals[v].kind = K_BOOL, t->vals[v].rep = R_BOOL;
            tpush(t, v);
            break;
        }
        case PURE_OP_LOAD: tpush(t, tvar(t, V_STATE + (int)arg, m->state_type[arg] == PURE_STATE_BOOL ? K_BOOL : K_NUM, 0)); break;
        case PURE_OP_STORE: t->state[V_STATE + arg] = tpop(t); break;
        case PURE_OP_LOCAL_LOAD: {
            unsigned kind = (arg >> 8 & 0xFF) == 1 ? K_INDEX : (arg >> 8 & 0xFF) == 2 ? K_BOOL : K_NUM;
            tpush(t, tvar(t, (int)(arg & 0x3F), kind, (uint16_t)(arg >> 16)));
            break;
        }
        case PURE_OP_LOCAL_STORE: t->state[arg & 0x3F] = tpop(t); break;
        case PURE_OP_FIELD: {
            if (arg < 64 && t->fields[arg]) { /* read once */
                tpush(t, t->fields[arg] - 1);
                break;
            }
            int saved = t->cur_loop;
            t->cur_loop = -1; /* the message's: where the tier begins */
            int v = tval(t, T_FIELD, R_NUM, -1, -1, -1);
            if (arg < 64) t->fields[arg] = v + 1;
            t->cur_loop = saved;
            t->vals[v].field = (uint16_t)arg, t->vals[v].pc = pc;
            const PureField *f = &m->field[m->input[t->input].field_start + arg];
            if (f->type == PURE_FIELD_U32) tfacts(t, v, true, 0, 0x1p32 - 1, false);
            if (f->type == PURE_FIELD_BOOL) t->vals[v].kind = K_BOOL, t->vals[v].rep = R_BOOL;
            tpush(t, v);
            break;
        }
        case PURE_OP_ADD: case PURE_OP_SUB: case PURE_OP_MUL: case PURE_OP_DIV: case PURE_OP_MIN: case PURE_OP_MAX: {
            unsigned to = op->op == PURE_OP_ADD ? T_ADD : op->op == PURE_OP_SUB ? T_SUB : op->op == PURE_OP_MUL ? T_MUL
                        : op->op == PURE_OP_DIV ? T_DIV : op->op == PURE_OP_MIN ? T_MIN : T_MAX;
            tpush(t, tbinary(t, to, pc, true));
            break;
        }
        case PURE_OP_LT: case PURE_OP_LE: case PURE_OP_GT: case PURE_OP_GE: case PURE_OP_EQ: case PURE_OP_NE: {
            unsigned to = op->op == PURE_OP_LT ? T_LT : op->op == PURE_OP_LE ? T_LE : op->op == PURE_OP_GT ? T_GT
                        : op->op == PURE_OP_GE ? T_GE : op->op == PURE_OP_EQ ? T_EQ : T_NE;
            int v = tbinary(t, to, pc, false);
            t->vals[v].rep = R_BOOL, t->vals[v].kind = K_BOOL;
            tpush(t, v);
            break;
        }
        case PURE_OP_AND: case PURE_OP_OR: {
            int v = tbinary(t, op->op == PURE_OP_AND ? T_AND : T_OR, pc, false);
            t->vals[v].rep = R_BOOL, t->vals[v].kind = K_BOOL;
            tpush(t, v);
            break;
        }
        case PURE_OP_NOT: {
            int v = tunary(t, T_NOT, R_BOOL, pc, false);
            t->vals[v].kind = K_BOOL;
            tpush(t, v);
            break;
        }
        case PURE_OP_NEG: case PURE_OP_FLOOR: tpush(t, tunary(t, op->op == PURE_OP_NEG ? T_NEG : T_FLOOR, R_NUM, pc, true)); break;
        case PURE_OP_JZ: case PURE_OP_JMP: {
            int landing = tier_landing(t, pc, arg);
            int g = landing >= 0 ? t->loops[landing].end : t->merge_at[arg - l0->start];
            if (op->op == PURE_OP_JZ) {
                int cond = tpop(t);
                tier_go(t, g, I_UNLESS, cond);
            } else {
                tier_go(t, g, I_JUMP, -1);
                t->live = false;
            }
            break;
        }
        case PURE_OP_BREAK: {
            int l = t->cur_loop;
            if (l < 0) {
                t->failed = true;
                break;
            }
            if (t->loops[l].li == t->nest) { /* the outermost loop's: the generic code breaks it */
                titem(t, I_BREAK, -1, -1, l);
                int snap = tsnap(t, pc);
                if (!t->failed) t->items[t->item_count - 1].snap = snap;
            } else
                tier_go(t, t->loops[l].exit, I_JUMP, -1);
            t->live = false;
            break;
        }
        case PURE_OP_EACH: case PURE_OP_LOOP:
            tier_loop(t, pc);
            pc += 1 + (arg >> 16);
            continue;
        case PURE_OP_ROW: {
            int l = t->cur_loop;
            while (l >= 0 && !t->loops[l].walk) l = t->loops[l].parent;
            if (l < 0) t->failed = true;
            else tpush(t, t->loops[l].k);
            break;
        }
        case PURE_OP_INDEX_FIRST: {
            int v = tconst(t, 0);
            t->vals[v].kind = K_INDEX, t->vals[v].list = (uint16_t)arg;
            tpush(t, v);
            break;
        }
        case PURE_OP_LIST_COUNT: {
            int v = tval(t, T_COUNT, R_INT, -1, -1, -1);
            t->vals[v].list = (uint16_t)arg, t->vals[v].pc = pc;
            tfacts(t, v, true, 0, 0x1p32, false);
            tpush(t, v);
            break;
        }
        case PURE_OP_INDEX_MOVE: {
            int owner = t->vals[t->state[V_STACK + t->height - 2]].list;
            int v = tbinary(t, T_MOVE, pc, true);
            t->vals[v].kind = K_INDEX, t->vals[v].list = (uint16_t)owner;
            tpush(t, v);
            break;
        }
        case PURE_OP_ITEM_GET: {
            int v = tunary(t, T_GET, R_NUM, pc, true);
            t->vals[v].list = (uint16_t)list_of(arg), t->vals[v].field = (uint16_t)item_field(arg);
            tpush(t, v);
            break;
        }
        case PURE_OP_ITEM_SET: {
            int v = tbinary(t, T_SET, pc, true);
            t->vals[v].list = (uint16_t)list_of(arg), t->vals[v].field = (uint16_t)item_field(arg);
            break;
        }
        case PURE_OP_LIST_FIND: case PURE_OP_LIST_HAS: {
            unsigned to = op->op == PURE_OP_LIST_HAS ? T_HAS : op->plan & PLAN_JOINED ? T_FIND_GET : T_FIND;
            int v = tunary(t, to, to == T_HAS ? R_BOOL : R_NUM, pc, true);
            t->vals[v].list = (uint16_t)arg;
            if (to == T_FIND_GET) t->vals[v].field = (uint16_t)item_field(m->code[pc + 1].arg);
            if (to == T_HAS) t->vals[v].kind = K_BOOL;
            if (to == T_FIND) t->vals[v].kind = K_INDEX;
            tpush(t, v);
            break;
        }
        default: t->failed = true; break;
        }
        pc = next;
    }
}

/* The nest from a round of its outermost loop (opened by the generic code, its state in memory) to its end. */
static void tier_build(Tier *t) {
    Block *k = t->k;
    const LoopAt *l0 = &k->loops[t->nest];
    const PureOp *op = &k->m->code[l0->at];
    for (int var = 0; var < VARS; ++var) t->state[var] = -1;
    t->height = l0->base;
    t->cur_loop = -1;
    tier_merges(t);
    if (t->failed) return;
    TLoop *l = &t->loops[0];
    t->loop_count = 1;
    memset(l, 0, sizeof(*l));
    l->li = t->nest, l->walk = l0->walk, l->parent = -1, l->length = op->arg >> 16;
    l->list = op->arg >> 8 & 0xFF, l->slot = op->arg & 0xFF;
    l->exit = tmerge(t, 0);
    l->open_snap = -1;
    l->count = tin(t, V_COUNT0, K_NUM, 0);
    tfacts(t, l->count, true, 0, 0x1p54, false);
    l->low = l->walk ? -1 : tin(t, V_LOW0, K_NUM, 0);
    t->state[V_STACK + PURE_MODULE_STACK] = tin(t, V_ROUND0, K_NUM, 0);
    tfacts(t, t->state[V_STACK + PURE_MODULE_STACK], true, 0, 0x1p54, false);
    t->live = true;
    tier_round(t, 0, l0->at);
    if (t->failed) return;
    tier_arrive(t, t->loops[0].exit);
    if (t->live) {
        titem(t, I_DONE, t->state[V_STACK + PURE_MODULE_STACK], -1, -1);
        int snap = tsnap(t, l0->end); /* what changed, written back */
        if (!t->failed) t->items[t->item_count - 1].snap = snap;
    }
}

/* ---- The tier: values that do not change in a loop, made before it */

/* Whether loop a is loop b or in it. */
static bool tin_loop(const Tier *t, int a, int b) {
    for (; a >= 0; a = t->loops[a].parent)
        if (a == b) return true;
    return false;
}

/* Each value of an inner loop's whose operands the loop does not make (and,
 * reading a list, one the nest does not set) is made before the loop opens
 * instead, as far out as it holds. Where it gives up then is where the loop
 * opens: the executor runs the loop from its start (none of its rounds has
 * run), so a check moved out of an if or ahead of a BREAK still ends as the
 * executor ends. */
static void tier_hoist(Tier *t) {
    for (bool moved = true; moved && !t->failed;) {
        moved = false;
        for (size_t i = 0; i < t->item_count; ++i) {
            if (t->items[i].kind != I_VALUE) continue;
            int vi = t->items[i].value;
            TVal *v = &t->vals[vi];
            int l = v->loop;
            if (l <= 0) continue; /* the outermost loop's are where the tier's rounds are */
            switch (v->op) {
            case T_ADD: case T_SUB: case T_MUL: case T_DIV: case T_MIN: case T_MAX: case T_NEG: case T_FLOOR: case T_LT: case T_LE:
            case T_GT: case T_GE: case T_EQ: case T_NE: case T_NOT: case T_AND: case T_OR:
                break;
            case T_GET: case T_FIND_GET: case T_FIND: case T_HAS:
                if (t->written >> (v->list & 31) & 1) continue;
                break;
            default: continue;
            }
            int ops[4] = {v->a, v->b, v->c, v->d};
            bool outside = true;
            for (int o = 0; o < 4 && outside; ++o)
                outside = ops[o] < 0 || !tin_loop(t, t->vals[ops[o]].loop, l) || t->vals[ops[o]].loop == -1 ||
                          (t->vals[ops[o]].op == T_CONST && t->vals[ops[o]].loop != l);
            for (int o = 0; o < 4 && outside; ++o) /* a constant made in the loop: made again outside is the same */
                if (ops[o] >= 0 && t->vals[ops[o]].op == T_CONST && tin_loop(t, t->vals[ops[o]].loop, l)) outside = false;
            if (!outside) continue;
            size_t open = 0; /* the loop's opening: made just before it */
            while (open < t->item_count && !(t->items[open].kind == I_OPEN && t->items[open].loop == l)) ++open;
            if (open >= i) continue;
            TItem it = t->items[i];
            memmove(&t->items[open + 1], &t->items[open], (i - open) * sizeof(TItem));
            t->items[open] = it;
            v->loop = t->loops[l].parent;
            if (v->snap >= 0) v->snap = t->loops[l].open_snap;
            moved = true;
        }
    }
}

/* ---- The tier: keys in range for every round, checked once */

/* The height of value x as affine in loop l's round (made of it, of what
 * the loop does not make, by + - and *, as integers: its value at each end
 * of the rounds bounds it in between), or -1 when it is not. */
static int taffine(const Tier *t, int x, int l) {
    const TVal *v = &t->vals[x];
    if (x == t->loops[l].k) return 0;
    if (!tin_loop(t, v->loop, l) || v->loop == -1) return v->rep == R_INT ? 0 : -1; /* made outside it: the same each round */
    if (v->rep != R_INT || (v->op != T_ADD && v->op != T_SUB && v->op != T_MUL)) return -1;
    if (v->op == T_MUL) { /* one side the same each round */
        bool a = !tin_loop(t, t->vals[v->a].loop, l) || t->vals[v->a].loop == -1;
        bool b = !tin_loop(t, t->vals[v->b].loop, l) || t->vals[v->b].loop == -1;
        if (!a && !b) return -1;
    }
    int ha = taffine(t, v->a, l), hb = taffine(t, v->b, l);
    return ha < 0 || hb < 0 ? -1 : 1 + (ha > hb ? ha : hb);
}

/* The loop's own nodes of an affine x: checked at its ends, none of their own. */
static void tbound_nodes(Tier *t, int x, int l) {
    TVal *v = &t->vals[x];
    if (x == t->loops[l].k || !tin_loop(t, v->loop, l) || v->loop == -1) return;
    v->bounded = true;
    v->snap = -1; /* nothing to give up: needed only where something reads it */
    tbound_nodes(t, v->a, l);
    tbound_nodes(t, v->b, l);
}

/* Each access of an inner loop's own (not a loop's in it) by a key or
 * position affine in its round, every node of it from 0: in range for
 * every round when it is at both ends, checked as the loop opens (else it
 * gives up there, the executor running the loop). Its nodes are then below
 * 2^53 each round (none of their own checks). */
static void tier_bounds(Tier *t) {
    for (size_t i = 0; i < t->val_count; ++i) {
        TVal *v = &t->vals[i];
        int l = v->loop, key = v->a, limit;
        if (l <= 0) continue;
        switch (v->op) {
        case T_FIND_GET: limit = v->d; break;
        case T_FIND: case T_HAS: limit = v->c; break;
        case T_GET: limit = v->b; break;
        case T_SET: limit = v->d; break;
        default: continue;
        }
        if (v->op == T_SET && t->vals[key].op == T_FIND) continue; /* a position found: in range already */
        int height = key >= 0 ? taffine(t, key, l) : -1;
        if (limit < 0 || t->vals[key].rep != R_INT || height < 0 || height > 2) continue; /* two scratch registers for its nodes */
        TLoop *loop = &t->loops[l];
        unsigned b = 0;
        while (b < loop->bound_count && !(loop->bounds[b][0] == key && loop->bounds[b][1] == limit)) ++b;
        if (b == loop->bound_count) {
            if (b == 16) continue;
            loop->bounds[b][0] = key, loop->bounds[b][1] = limit, ++loop->bound_count;
        }
        v->proven = true;
        tbound_nodes(t, key, l);
    }
}

/* ---- The tier: facts, registers' kinds, and what each list needs */

/* What the tier keeps of a list where it begins: its form's checks and values. */
enum { L_COUNT = 16, L_BITS = 17, L_LIMIT = 18, L_PARTS = 19 }; /* parts of a list: its columns (0-15), its count, bits, limit */
enum { T_LIST = -20 };                                             /* T_IN's variable for a list's part */
typedef struct TList {
    int part[L_PARTS];  /* the values, or -1 */
    bool position;      /* read or set by position: FLAT, or DENSE full */
    bool key;           /* found by key: DENSE */
    bool full;          /* found by key and by position, or a key's position found: DENSE and full (every key below its count there) */
} TList;

static bool tfinite(const TVal *v) { return v->lo > -INFINITY && v->hi < INFINITY; }
static bool tsmall(double lo, double hi) { return lo > -0x1p53 && hi < 0x1p53; }

/* The facts of each value, from its operands' (a phi from its ways', widened past a few passes). */
static void tier_facts(Tier *t) {
    for (size_t i = 0; i < t->val_count; ++i) { /* what is computed from nothing yet (optimistic: grown to what it may be) */
        TVal *v = &t->vals[i];
        if (v->rep == R_BOOL || v->op == T_CONST || v->op == T_FIELD || v->op == T_COUNT || v->op == T_SPAN || v->op == T_SET ||
            (v->op == T_IN && v->var == T_LIST))
            continue;
        v->lo = INFINITY, v->hi = -INFINITY, v->whole = true, v->zero = false;
    }
    for (unsigned pass = 0; pass < 16; ++pass) {
        bool changed = false;
        for (size_t i = 0; i < t->val_count; ++i) {
            TVal *v = &t->vals[i];
            if (v->rep == R_BOOL || v->op == T_CONST || v->op == T_FIELD || v->op == T_COUNT) continue;
            const TVal *a = v->a >= 0 ? &t->vals[v->a] : NULL, *b = v->b >= 0 ? &t->vals[v->b] : NULL;
            if (v->op == T_SPAN) { /* a count's rounds: high - low, from 0 */
                double most = a->whole && b->whole ? fmax(b->hi - a->lo, 0) : 0x1p54;
                if (a->lo > a->hi || b->lo > b->hi) continue;
                if (v->hi != most) changed = true;
                v->whole = true, v->lo = 0, v->hi = fmin(most, 0x1p54), v->zero = false;
                continue;
            }
            if (v->op != T_PHI && v->op != T_IN && ((a && a->lo > a->hi && a->rep != R_BOOL) || (b && b->lo > b->hi && b->rep != R_BOOL)))
                continue; /* an operand not known yet */
            bool whole = false, zero = true;
            double lo = -INFINITY, hi = INFINITY;
            switch (v->op) {
            case T_IN:
                if (v->var == T_LIST) continue;
                if (v->var == V_ROUND0 || v->var == V_COUNT0) whole = true, lo = 0, hi = 0x1p32 - 1, zero = false; /* checked where the tier begins */
                else if (v->var == V_LOW0) whole = true, lo = 0, hi = 0x1p53, zero = false; /* from 0 (checked where the tier begins) */
                else if (v->kind == K_INDEX) whole = true, lo = 0, hi = 0x1p32, zero = false;
                else if (v->var >= 0 && v->var < V_STACK) { /* whole where every value the nest gives it is (checked where the tier begins) */
                    whole = true, lo = -0x1p53, hi = 0x1p53, zero = false;
                    for (size_t j = 0; j < t->val_count && whole; ++j) {
                        const TVal *p = &t->vals[j];
                        if (p->op != T_PHI || p->var != v->var) continue;
                        for (unsigned w = 0; w < p->ins && whole; ++w) {
                            const TVal *in = &t->vals[p->in[w]];
                            whole = p->in[w] == (int)i || in->op == T_PHI || in->lo > in->hi || in->whole;
                        }
                    }
                    if (!whole) lo = -INFINITY, hi = INFINITY, zero = true;
                    bool assigned = false; /* none given: as it is (a number) */
                    for (size_t j = 0; j < t->val_count && !assigned; ++j) assigned = t->vals[j].op == T_PHI && t->vals[j].var == v->var;
                    if (!assigned) whole = false, lo = -INFINITY, hi = INFINITY, zero = true;
                }
                break;
            case T_PHI: {
                whole = true, zero = false, lo = INFINITY, hi = -INFINITY;
                for (unsigned w = 0; w < v->ins; ++w) {
                    const TVal *in = &t->vals[v->in[w]];
                    if (in->lo > in->hi && in->rep != R_BOOL) continue; /* not known yet */
                    whole = whole && in->whole, zero = zero || in->zero;
                    lo = fmin(lo, in->lo), hi = fmax(hi, in->hi);
                }
                if (pass >= 4 && v->lo <= v->hi && (lo < v->lo || hi > v->hi)) { /* still growing: as far as it may */
                    if (lo < v->lo) lo = whole ? -0x1p53 : -INFINITY;
                    if (hi > v->hi) hi = whole ? 0x1p53 : INFINITY;
                }
                for (unsigned l = 0; l < t->loop_count; ++l) /* a loop's round: below its rounds (its iteration goes back only then) */
                    if (t->loops[l].k == (int)i) {
                        const TVal *count = &t->vals[t->loops[l].count];
                        if (count->lo <= count->hi) whole = true, lo = 0, hi = fmax(count->hi - 1, 0), zero = false;
                    }
                break;
            }
            case T_ADD: case T_SUB:
                whole = a->whole && b->whole, zero = !whole && a->zero && (v->op == T_ADD ? b->zero : true);
                lo = v->op == T_ADD ? a->lo + b->lo : a->lo - b->hi, hi = v->op == T_ADD ? a->hi + b->hi : a->hi - b->lo;
                if (isnan(lo)) lo = -INFINITY;
                if (isnan(hi)) hi = INFINITY;
                break;
            case T_MUL: {
                double c[4] = {a->lo * b->lo, a->lo * b->hi, a->hi * b->lo, a->hi * b->hi};
                lo = INFINITY, hi = -INFINITY;
                for (int j = 0; j < 4; ++j) {
                    if (isnan(c[j])) c[j] = 0; /* 0 times infinity: the bound is 0's side */
                    lo = fmin(lo, c[j]), hi = fmax(hi, c[j]);
                }
                bool negzero = (a->lo <= 0 && a->hi >= 0 && b->lo < 0) || (b->lo <= 0 && b->hi >= 0 && a->lo < 0);
                whole = a->whole && b->whole; /* 0 times a number below 0 is -0: the code checks it */
                v->negzero = whole && negzero;
                zero = !whole && (negzero || a->zero || b->zero);
                break;
            }
            case T_DIV:
                if (v->integers) whole = true, lo = 0, hi = a->hi, zero = false;
                else if (b->lo > 0 || b->hi < 0) {
                    double c[4] = {a->lo / b->lo, a->lo / b->hi, a->hi / b->lo, a->hi / b->hi};
                    lo = INFINITY, hi = -INFINITY;
                    for (int j = 0; j < 4; ++j) lo = fmin(lo, isnan(c[j]) ? -INFINITY : c[j]), hi = fmax(hi, isnan(c[j]) ? INFINITY : c[j]);
                    /* -0 only of -0, of 0 over a number below 0, or of a fraction's underflow: a whole number over a
                     * whole one is at least 1 / 2^53 in size unless it is 0 */
                    zero = a->zero || !a->whole || !b->whole || (b->hi < 0 && a->lo <= 0 && a->hi >= 0);
                }
                break;
            case T_FLOOR:
                if (a->whole || (a->op == T_DIV && a->integers)) whole = true, lo = a->lo, hi = a->hi, zero = false;
                else {
                    lo = floor(a->lo), hi = floor(a->hi), zero = a->zero;
                    whole = !zero && tsmall(lo, hi);
                }
                break;
            case T_MIN: case T_MAX:
                whole = a->whole && b->whole, zero = a->zero || b->zero;
                lo = v->op == T_MIN ? fmin(a->lo, b->lo) : fmax(a->lo, b->lo);
                hi = v->op == T_MIN ? fmin(a->hi, b->hi) : fmax(a->hi, b->hi);
                break;
            case T_NEG:
                lo = -a->hi, hi = -a->lo;
                whole = a->whole && (a->lo > 0 || a->hi < 0), zero = true; /* -0 of 0 */
                break;
            case T_GET: case T_FIND_GET: whole = true, lo = 0, hi = 0x1p32 - 1, zero = false; break; /* a narrow cell (checked where the tier begins) */
            case T_FIND: case T_MOVE: whole = true, lo = 0, hi = 0x1p32, zero = false; break;
            default: continue;
            }
            if (whole && !tsmall(lo, hi) && v->op != T_IN) { /* past 2^53: whole, the code checks it (else gives up) */
                lo = fmax(lo, -0x1p53), hi = fmin(hi, 0x1p53);
            }
            if (v->whole != whole || v->lo != lo || v->hi != hi || v->zero != zero) changed = true;
            v->whole = whole, v->lo = lo, v->hi = hi, v->zero = zero;
        }
        if (!changed && pass) break;
    }
    for (size_t i = 0; i < t->val_count; ++i) /* never known (never reached): any number */
        if (t->vals[i].lo > t->vals[i].hi && t->vals[i].rep != R_BOOL) t->vals[i].lo = -INFINITY, t->vals[i].hi = INFINITY, t->vals[i].whole = false, t->vals[i].zero = true;
}

/* A run of numbers' arithmetic each read by the next alone, right after it
 * (no other code between but constants): only the last checks it is finite. A number not
 * finite stays not finite through an add, a sub, a mul, or as what a
 * division divides; where it is not, the code gives up at the run's first
 * snapshot, so the executor makes the run again and fails where the
 * executor does. */
static void tier_defer(Tier *t) {
    for (size_t i = 0; i + 1 < t->item_count; ++i) {
        size_t next = i + 1; /* the next value but constants (made as they are: no check, nothing changed) */
        while (next < t->item_count && t->items[next].kind == I_VALUE && t->vals[t->items[next].value].op == T_CONST) ++next;
        if (t->items[i].kind != I_VALUE || next == t->item_count || t->items[next].kind != I_VALUE) continue;
        TVal *v = &t->vals[t->items[i].value], *w = &t->vals[t->items[next].value];
        int vi = t->items[i].value;
        bool arith = v->rep == R_NUM && v->op >= T_ADD && v->op <= T_DIV && v->snap >= 0 && v->uses == 1;
        bool reads = w->rep == R_NUM && w->snap >= 0 && w->loop == v->loop &&
                     (((w->op == T_ADD || w->op == T_SUB || w->op == T_MUL) && (w->a == vi || w->b == vi)) ||
                      (w->op == T_DIV && w->a == vi && w->b != vi && (t->vals[w->b].lo > 0 || t->vals[w->b].hi < 0)));
        if (!arith || !reads || tfinite(v) || tfinite(w)) continue;
        v->deferred = true;
        w->retry = v->retry >= 0 ? v->retry : v->snap;
    }
}

/* Each value's register: a whole number an integer, else a number (a bool's as made). Uses counted. */
static void tier_reps(Tier *t) {
    for (size_t i = 0; i < t->val_count; ++i) {
        TVal *v = &t->vals[i];
        if (v->a >= 0) ++t->vals[v->a].uses;
        if (v->b >= 0) ++t->vals[v->b].uses;
        if (v->c >= 0) ++t->vals[v->c].uses;
        if (v->d >= 0) ++t->vals[v->d].uses;
        for (unsigned w = 0; w < v->ins; ++w) ++t->vals[v->in[w]].uses;
    }
    for (size_t i = 0; i < t->val_count; ++i) { /* floor(a / b) of whole numbers, a from 0 and b above it: an integer division */
        TVal *v = &t->vals[i];
        if (v->op != T_FLOOR || t->vals[v->a].op != T_DIV || t->vals[v->a].uses != 1) continue;
        TVal *d = &t->vals[v->a];
        const TVal *a = &t->vals[d->a], *b = &t->vals[d->b];
        if (a->whole && b->whole && a->lo >= 0 && b->lo > 0) d->integers = true;
    }
    size_t made = t->val_count;
    for (size_t i = 0; i < made; ++i) { /* a number over a power of two: times its reciprocal (the same number, exactly) */
        TVal *v = &t->vals[i];
        if (v->op != T_DIV || v->integers || t->vals[v->b].op != T_CONST) continue;
        double b = t->vals[v->b].num;
        int exp;
        if (fabs(frexp(b, &exp)) != 0.5 || !(fabs(b) >= 0x1p-1000 && fabs(b) <= 0x1p1000)) continue;
        int c = tconst(t, 1 / b); /* where the tier begins */
        t->vals[c].uses = 1, --t->vals[v->b].uses;
        t->vals[i].op = T_MUL, t->vals[i].b = c;
    }
    tier_facts(t);
    for (unsigned l = 0; l < t->loop_count; ++l) /* a loop's round and rounds are integers */
        if (!t->vals[t->loops[l].k].whole || !t->vals[t->loops[l].count].whole) t->failed = true, t->why = "rounds";
    for (size_t i = 0; i < t->val_count; ++i) {
        TVal *v = &t->vals[i];
        if (v->rep == R_BOOL || v->kind == K_BOOL) {
            v->rep = R_BOOL;
            continue;
        }
        if (v->op == T_SET) continue;
        v->rep = v->whole ? R_INT : R_NUM;
    }
    for (size_t i = 0; i < t->val_count; ++i) { /* a constant only numbers' arithmetic reads: a number, made where the tier begins */
        TVal *c = &t->vals[i];
        if (c->op != T_CONST || c->rep != R_INT) continue;
        bool numbers = c->uses > 0;
        for (size_t j = 0; j < t->val_count && numbers; ++j) {
            const TVal *w = &t->vals[j];
            bool reads = w->a == (int)i || w->b == (int)i || w->c == (int)i || w->d == (int)i;
            for (unsigned k = 0; k < w->ins && !reads; ++k) reads = w->in[k] == (int)i;
            numbers = !reads || (w->rep == R_NUM && w->op >= T_ADD && w->op <= T_MAX);
        }
        if (!numbers) continue;
        c->rep = R_NUM;
        if (c->loop < 0) continue;
        size_t at = 0;
        while (at < t->item_count && !(t->items[at].kind == I_VALUE && t->items[at].value == (int)i)) ++at;
        if (at == t->item_count || !grow_array((void **)&t->entry, &t->entry_room, t->entry_count + 1, sizeof(*t->entry))) continue;
        memmove(&t->items[at], &t->items[at + 1], (t->item_count - at - 1) * sizeof(TItem));
        --t->item_count;
        t->entry[t->entry_count++] = (int)i;
        c->loop = -1;
    }
}

/* The values a list's part takes where the tier begins (made once). */
static int tlist(Tier *t, TList *lists, unsigned list, unsigned part) {
    TList *l = &lists[list];
    if (l->part[part] < 0) {
        int saved = t->cur_loop;
        t->cur_loop = -1;
        int v = tval(t, T_IN, part == L_BITS || part < 16 ? R_INT : R_INT, -1, -1, -1);
        t->cur_loop = saved;
        t->vals[v].var = T_LIST, t->vals[v].list = (uint16_t)list, t->vals[v].field = (uint16_t)part;
        tfacts(t, v, true, 0, 0x1p62, false);
        l->part[part] = v;
    }
    return l->part[part];
}

/* What each list access needs, as values where the tier begins: a column (its address tagged with its cells' width), the count, the key bits and their reach. */
static void tier_lists(Tier *t, TList *lists) {
    for (unsigned i = 0; i < PURE_MODULE_STATES; ++i) {
        for (unsigned p = 0; p < L_PARTS; ++p) lists[i].part[p] = -1;
        lists[i].position = lists[i].key = lists[i].full = false;
    }
    size_t made = t->val_count;
    for (size_t i = 0; i < made && !t->failed; ++i) {
        TVal *v = &t->vals[i];
        switch (v->op) {
        case T_COUNT: { int c = tlist(t, lists, v->list, L_COUNT); t->vals[i].b = c; break; }
        case T_GET: case T_SET: {
            lists[v->list].position = true;
            int column = tlist(t, lists, t->vals[i].list, t->vals[i].field), count = tlist(t, lists, t->vals[i].list, L_COUNT);
            t->vals[i].c = column;
            if (t->vals[i].op == T_GET) t->vals[i].b = count;
            else t->vals[i].d = count;
            break;
        }
        case T_FIND_GET: {
            lists[v->list].key = true;
            int column = tlist(t, lists, t->vals[i].list, t->vals[i].field), bits = tlist(t, lists, t->vals[i].list, L_BITS),
                limit = tlist(t, lists, t->vals[i].list, L_LIMIT);
            t->vals[i].c = column, t->vals[i].b = bits, t->vals[i].d = limit;
            break;
        }
        case T_FIND: case T_HAS: {
            lists[v->list].key = true;
            int bits = tlist(t, lists, t->vals[i].list, L_BITS), limit = tlist(t, lists, t->vals[i].list, L_LIMIT);
            t->vals[i].b = bits, t->vals[i].c = limit;
            break;
        }
        case T_MOVE: { int c = tlist(t, lists, t->vals[i].list, L_COUNT); t->vals[i].c = c; break; }
        default: break;
        }
    }
    for (size_t i = 0; i < made; ++i)
        if (t->vals[i].op == T_FIND) lists[t->vals[i].list].full = true;
    for (unsigned i = 0; i < PURE_MODULE_STATES; ++i) { /* found by key and by position: DENSE and full, its count its keys' reach */
        if (lists[i].key && lists[i].position) lists[i].full = true;
        if (lists[i].full) (void)tlist(t, lists, i, L_COUNT);
    }
}

/* Each access of an inner count's own by a key or position its opening
 * found in range for every round (tier_bounds), affine in its round, in a
 * full list (the tier's code reads its cell at its key): a pointer to its
 * cell, a phi at the loop's head (V_STEP), made as the loop opens (the
 * column plus the key at round 0, times four: T_STEP0) and stepped at its
 * latch (the key's step a round, times four: T_STRIDE). A list found by
 * key alone with such an access is taken as full where the tier begins
 * (else the generic code runs the nest). The access reads its pointer, not
 * its key (dead unless read elsewhere), and gives up nowhere. */
/* Whether affine x moves with loop l's round (its round in it). */
static bool tvaries(const Tier *t, int x, int l) {
    const TVal *v = &t->vals[x];
    if (x == t->loops[l].k) return true;
    if (!tin_loop(t, v->loop, l) || v->loop == -1) return false;
    return (v->a >= 0 && tvaries(t, v->a, l)) || (v->b >= 0 && tvaries(t, v->b, l));
}

static void tier_steps(Tier *t) {
    TList *lists = t->lists;
    unsigned made = 0;
    size_t values = t->val_count;
    for (size_t i = 0; i < values && !t->failed; ++i) {
        TVal *v = &t->vals[i];
        int l = v->loop;
        if (!v->proven || (v->op != T_GET && v->op != T_FIND_GET) || l <= 0 || t->loops[l].walk || !tvaries(t, v->a, l)) continue;
        if (v->op == T_FIND_GET && !lists[v->list].full) { /* by key alone: full where the tier begins */
            lists[v->list].full = true;
            (void)tlist(t, lists, v->list, L_COUNT);
        }
        int key = v->a, column = v->c, phi = -1;
        for (size_t j = values; j < t->val_count && phi < 0; ++j) /* one for a key and a column */
            if (t->vals[j].op == T_PHI && t->vals[j].var >= V_STEP && t->vals[j].loop == l) {
                const TVal *start = &t->vals[t->vals[j].in[0]];
                if (start->a == key && start->b == column) phi = (int)j;
            }
        if (phi < 0) {
            if (made == STEPS) continue;
            TLoop *loop = &t->loops[l];
            TMerge *head = &t->merges[loop->head];
            size_t open = SIZE_MAX, latch = SIZE_MAX;
            for (size_t k = 0; k < t->item_count; ++k) {
                if (t->items[k].kind == I_OPEN && t->items[k].loop == l) open = k;
                if (t->items[k].kind == I_LATCH && t->items[k].loop == l) latch = k;
            }
            if (open == SIZE_MAX || latch == SIZE_MAX || head->way_count != 2 || !head->phis) continue;
            int saved = t->cur_loop;
            t->cur_loop = -1; /* made with no item (placed below) */
            int start = tval(t, T_STEP0, R_INT, key, column, l), stride = tval(t, T_STRIDE, R_INT, key, -1, l);
            phi = tval(t, T_PHI, R_INT, -1, -1, -1);
            int next = tval(t, T_ADD, R_INT, phi, stride, -1);
            t->cur_loop = saved;
            t->entry_count -= 3; /* (tval put three where the tier begins; the phi none) */
            if (t->failed || !grow_array((void **)&t->items, &t->item_room, t->item_count + 3, sizeof(*t->items))) {
                t->failed = true;
                break;
            }
            int parent = loop->parent;
            t->vals[start].loop = parent, t->vals[stride].loop = parent, t->vals[next].loop = l;
            t->vals[phi].var = V_STEP + (int)made, t->vals[phi].label = loop->head, t->vals[phi].loop = l;
            tphi_in(t, phi, start);
            tphi_in(t, phi, next);
            head->phis[V_STEP + made] = phi;
            for (int w = start; w <= next; ++w) {
                TVal *x = &t->vals[w];
                x->whole = true, x->lo = 0, x->hi = 0x1p62, x->zero = false, x->rep = R_INT;
            }
            t->vals[stride].lo = -0x1p62; /* a key may step down */
            t->vals[next].bounded = true; /* a pointer: no check of its own */
            /* the latch's step before it; the start and stride after the opening (as it goes on: rounds to run) */
            memmove(&t->items[latch + 1], &t->items[latch], (t->item_count - latch) * sizeof(TItem));
            t->items[latch] = (TItem){I_VALUE, next, -1, l, -1, -1};
            memmove(&t->items[open + 3], &t->items[open + 1], (t->item_count + 1 - open - 1) * sizeof(TItem));
            t->items[open + 1] = (TItem){I_VALUE, start, -1, parent, -1, -1};
            t->items[open + 2] = (TItem){I_VALUE, stride, -1, parent, -1, -1};
            t->item_count += 3;
            ++made;
        }
        v = &t->vals[i];
        v->step = phi;
        v->snap = -1; /* in range, in a full list: nothing to give up */
    }
}

/* ---- The tier: where each value lives, and its register */

/* Registers the tier gives values: x ones for integers and bools, d ones for numbers (x0, x1, x9, x10, x16, x17, d0-d3 scratch). */
static const uint8_t tier_x[] = {2, 3, 4, 5, 6, 7, 8, 11, 12, 13, 14, 15, 18, 25, 26, 20, 21, 22, 23, 24, 27, 28}; /* x30: the work (TIER_WORK) */
static const uint8_t tier_d[] = {4, 5, 6, 7, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31};

/* Whether a snapshot (or a write back) needs variable var's value v: it changed from what memory holds. */
static bool tchanged(const Tier *t, int var, int v) {
    if (v < 0) return false;
    const TVal *x = &t->vals[v];
    return !(x->op == T_IN && x->var == var);
}

/* Whether value v is a constant an instruction takes as its immediate (a whole number from 0 below 4096). */
static bool timm(const Tier *t, int v) {
    const TVal *x = &t->vals[v];
    return x->op == T_CONST && x->rep == R_INT && x->num >= 0 && x->num < 4096;
}

static void tuse(Tier *t, int v, int at) {
    if (v >= 0 && t->vals[v].last < at) t->vals[v].last = at;
}

/* What an affine key of loop l is made of outside it, read at at (its ends computed there). */
static void tuse_affine(Tier *t, int x, int l, int at) {
    const TVal *v = &t->vals[x];
    if (x == t->loops[l].k) return;
    if (!tin_loop(t, v->loop, l) || v->loop == -1) {
        if (v->op != T_CONST) tuse(t, x, at);
        return;
    }
    tuse_affine(t, v->a, l, at);
    tuse_affine(t, v->b, l, at);
}

/* A snapshot's values read at at (it gives up there: they are written). */
static void tuse_snap(Tier *t, int s, int at) {
    if (s < 0) return;
    const TSnap *snap = &t->snaps[s];
    for (int var = 0; var < VARS; ++var) /* (a constant written as the constant it is) */
        if ((var < V_STACK ? tchanged(t, var, snap->vars[var]) : snap->vars[var] >= 0) && t->vals[snap->vars[var]].op != T_CONST)
            tuse(t, snap->vars[var], at);
    for (int l = snap->loop; l >= 0; l = t->loops[l].parent) { /* the open loops: their rounds, counts and lows */
        tuse(t, t->loops[l].k, at), tuse(t, t->loops[l].count, at);
        if (t->loops[l].low >= 0 && t->vals[t->loops[l].low].op != T_CONST) tuse(t, t->loops[l].low, at); /* a constant written as one */
    }
}

/* A way into merge g read at at: the inputs along it of the phis something needs (value k read as next - 1 when k >= 0, tier_moves). */
static void tuse_way(Tier *t, const bool *need, int g, int way, int at, int k, int next) {
    const TMerge *m = &t->merges[g];
    if (way < 0 || !m->phis) return;
    for (int var = 0; var < VARS; ++var) {
        int phi = m->phis[var];
        if (phi < 0 || !need[phi] || (unsigned)way >= t->vals[phi].ins) continue;
        tuse(t, k >= 0 && t->vals[phi].in[way] == k ? next : t->vals[phi].in[way], at);
    }
}

/* Whether value v finds by key in a list that may be HASHED (found by key alone, not full). */
static bool thashes(const Tier *t, const TVal *v) {
    return (v->op == T_FIND_GET || v->op == T_HAS) && t->lists && !t->lists[v->list].full;
}

/* Whether loop l's latch pays for its next round: the outermost count's (the generic code opened it); the others paid for all their rounds as they opened. */
static bool tlatch_pays(const Tier *t, int l) { return l == 0 && !t->loops[l].walk; }

/* A latch's round when its way out takes it as the next round less one (the
 * round not read past the next then: its register the next's), or -1: a
 * loop that does not pay there (the outermost count's work check reads it). */
static int tlatch_k(const Tier *t, const TItem *it) {
    const TLoop *l = &t->loops[it->loop];
    return !tlatch_pays(t, it->loop) && t->vals[l->k].rep == R_INT && t->vals[it->value].rep == R_INT ? l->k : -1;
}

/* Whether item it leaves its loop early (a break): to its exit. */
static bool tbreaks(const Tier *t, const TItem *it) { return it->kind == I_JUMP && it->loop > 0 && it->merge == t->loops[it->loop].exit; }

static void tneed_affine(Tier *t, bool *need, int *work, size_t *top, int x, int l) {
    const TVal *v = &t->vals[x];
    if (x == t->loops[l].k) return;
    if (!tin_loop(t, v->loop, l) || v->loop == -1) {
        if (v->op != T_CONST && !need[x]) need[x] = true, work[(*top)++] = x;
        return;
    }
    tneed_affine(t, need, work, top, v->a, l);
    tneed_affine(t, need, work, top, v->b, l);
}

/* The values something needs: the items', what may give up (its snapshot's too), and their operands. */
static void tier_needed(Tier *t, bool *need) {
    int *work = malloc(t->val_count * sizeof(int));
    size_t top = 0;
    if (!work) {
        t->failed = true;
        return;
    }
#define NEED(v) do { int v_ = (v); if (v_ >= 0 && !need[v_]) need[v_] = true, work[top++] = v_; } while (0)
#define NEED_SNAP(s) do { if ((s) >= 0) { const TSnap *sn = &t->snaps[s]; \
        for (int var = 0; var < VARS; ++var) if ((var < V_STACK ? tchanged(t, var, sn->vars[var]) : sn->vars[var] >= 0) && \
                                                 t->vals[sn->vars[var]].op != T_CONST) NEED(sn->vars[var]); \
        for (int l = sn->loop; l >= 0; l = t->loops[l].parent) { NEED(t->loops[l].k); NEED(t->loops[l].count); \
            if (t->loops[l].low >= 0 && t->vals[t->loops[l].low].op != T_CONST) NEED(t->loops[l].low); } } } while (0)
    for (size_t i = 0; i < t->item_count; ++i) {
        const TItem *it = &t->items[i];
        const TMerge *m = it->merge >= 0 ? &t->merges[it->merge] : NULL;
        (void)m;
        if (it->kind == I_UNLESS || it->kind == I_LATCH || it->kind == I_DONE) NEED(it->value);
        if (it->kind == I_OPEN || it->kind == I_LATCH) {
            NEED(t->loops[it->loop].count);
            NEED(t->loops[it->loop].k);
        }
        if (it->kind == I_OPEN)
            for (unsigned b = 0; b < t->loops[it->loop].bound_count; ++b) {
                tneed_affine(t, need, work, &top, t->loops[it->loop].bounds[b][0], it->loop);
                NEED(t->loops[it->loop].bounds[b][1]);
            }
        if (it->kind == I_OPEN || it->kind == I_BREAK || it->kind == I_DONE || (it->kind == I_LATCH && tlatch_pays(t, it->loop))) NEED_SNAP(it->snap);
        if (tbreaks(t, it)) { /* the rounds it will not run given back */
            NEED(t->loops[it->loop].count);
            NEED(t->loops[it->loop].k);
        }
    }
    for (size_t i = 0; i < t->val_count; ++i)
        if (t->vals[i].snap >= 0 || t->vals[i].op == T_SET) NEED((int)i);
    while (top) {
        const TVal *v = &t->vals[work[--top]];
        bool folds = v->rep == R_INT && (v->op == T_ADD || v->op == T_SUB); /* a constant operand in the instruction */
        bool compares = v->op >= T_LT && v->op <= T_NE && t->vals[v->a].rep != R_NUM && t->vals[v->b].rep != R_NUM;
        if ((folds || compares) && timm(t, v->b)) {
            NEED(v->a);
            NEED_SNAP(v->snap);
            continue;
        }
        if (folds && v->op == T_ADD && timm(t, v->a)) {
            NEED(v->b);
            NEED_SNAP(v->snap);
            continue;
        }
        if (v->op == T_STEP0 || v->op == T_STRIDE) { /* the key's nodes made outside its loop, and the column */
            tneed_affine(t, need, work, &top, v->a, v->c);
            continue;
        }
        if (v->step >= 0) { /* an access by its pointer: its key, bits, column and limit not read */
            NEED(v->step);
            continue;
        }
        NEED(v->a);
        NEED(v->b);
        NEED(v->c);
        NEED(v->d);
        NEED_SNAP(v->snap);
        NEED_SNAP(v->retry);
        for (unsigned w = 0; w < v->ins; ++w) NEED(v->in[w]);
    }
#undef NEED
#undef NEED_SNAP
    free(work);
}

/* Where each value is made and last read (a value made before a loop and read in it, to the loop's end); registers by linear scan. False when they run out. */
static bool tier_registers(Tier *t) {
    int at = 0;
    bool *need = calloc(t->val_count + 1, sizeof(bool));
    if (!need) return false;
    tier_needed(t, need);
    for (size_t i = 0; i < t->val_count; ++i) t->vals[i].at = -1, t->vals[i].last = -1;
    for (size_t e = 0; e < t->entry_count; ++e) t->vals[t->entry[e]].at = at++;
    int base = at;
    for (size_t i = 0; i < t->item_count; ++i) {
        TItem *it = &t->items[i];
        int p = base + (int)i;
        if (it->kind == I_VALUE) t->vals[it->value].at = p;
        if (it->kind == I_MERGE)
            for (int var = 0; var < VARS; ++var)
                if (t->merges[it->merge].phis && t->merges[it->merge].phis[var] >= 0) t->vals[t->merges[it->merge].phis[var]].at = p;
    }
    for (size_t i = 0; i < t->item_count; ++i) {
        TItem *it = &t->items[i];
        int p = base + (int)i;
        switch (it->kind) {
        case I_VALUE: {
            TVal *v = &t->vals[it->value];
            if (v->op == T_STEP0 || v->op == T_STRIDE) {
                tuse_affine(t, v->a, v->c, p);
                break;
            }
            if (v->step >= 0) { /* by its pointer alone */
                tuse(t, v->step, p);
                break;
            }
            tuse(t, v->a, p), tuse(t, v->b, p), tuse(t, v->c, p), tuse(t, v->d, p);
            tuse_snap(t, v->snap, p);
            tuse_snap(t, v->retry, p); /* the run's first snapshot: given up to from here */
            break;
        }
        case I_UNLESS: tuse(t, it->value, p); /* fall through */
        case I_JUMP:
            tuse_way(t, need, it->merge, it->way, p, -1, -1);
            if (tbreaks(t, it)) tuse(t, t->loops[it->loop].count, p), tuse(t, t->loops[it->loop].k, p);
            break;
        case I_OPEN: {
            const TLoop *l = &t->loops[it->loop];
            tuse(t, l->count, p);
            tuse_way(t, need, it->merge, it->way, p, -1, -1);
            tuse_snap(t, it->snap, p);
            for (unsigned b = 0; b < l->bound_count; ++b) tuse_affine(t, l->bounds[b][0], it->loop, p), tuse(t, l->bounds[b][1], p);
            break;
        }
        case I_LATCH: {
            const TLoop *l = &t->loops[it->loop];
            tuse(t, it->value, p), tuse(t, l->count, p);
            tuse_way(t, need, l->head, (int)t->merges[l->head].way_count - 1, p, -1, -1);
            tuse_way(t, need, it->merge == l->head ? l->exit : it->merge, it->way, p, tlatch_k(t, it), it->value);
            if (tlatch_pays(t, it->loop)) tuse_snap(t, it->snap, p);
            break;
        }
        case I_BREAK: case I_DONE: tuse(t, it->value, p), tuse_snap(t, it->snap, p); break;
        default: break;
        }
    }
    for (size_t v = 0; v < t->val_count; ++v) { /* a full list's position found: its key, in its key's register */
        TVal *x = &t->vals[v];
        x->aliased = x->op == T_FIND && t->lists[x->list].full && need[v] && x->at >= 0 && need[x->a] && t->vals[x->a].at >= 0 &&
                     t->vals[x->a].rep == R_INT && t->vals[x->a].op != T_SET;
    }
    for (size_t i = 1; i < t->item_count; ++i) { /* a comparison only the branch right after it reads: its flags */
        const TItem *it = &t->items[i];
        if (it->kind != I_UNLESS || t->items[i - 1].kind != I_VALUE || t->items[i - 1].value != it->value) continue;
        TVal *x = &t->vals[it->value];
        x->fused = x->op >= T_LT && x->op <= T_NE && x->uses == 0 && x->last <= base + (int)i;
    }
    /* A value read in a loop it was made before lives to the loop's iteration (its back edge reads it again); a key its alias's reads too. */
    for (bool changed = true; changed;) {
        changed = false;
        for (size_t v = 0; v < t->val_count; ++v)
            if (t->vals[v].aliased && t->vals[t->vals[v].a].last < t->vals[v].last) t->vals[t->vals[v].a].last = t->vals[v].last, changed = true;
        for (unsigned l = 0; l < t->loop_count; ++l) {
            int head = -1, latch = -1;
            for (size_t i = 0; i < t->item_count; ++i) {
                if (t->items[i].kind == I_MERGE && t->items[i].merge == t->loops[l].head) head = base + (int)i;
                if (t->items[i].kind == I_LATCH && t->items[i].loop == (int)l) latch = base + (int)i;
            }
            if (head < 0 || latch < 0) continue;
            for (size_t v = 0; v < t->val_count; ++v) {
                TVal *x = &t->vals[v];
                if (x->at >= 0 && x->at < head && x->last >= head && x->last < latch) x->last = latch, changed = true;
            }
        }
    }
    /* Linear scan: by where they are made, a register each until its last read. */
    int *order = malloc(t->val_count * sizeof(int));
    int x_until[32], d_until[32];
    if (!order) return false;
    size_t n = 0;
    for (size_t v = 0; v < t->val_count; ++v)
        if (t->vals[v].at >= 0 && t->vals[v].op != T_SET && need[v] && !t->vals[v].fused) order[n++] = (int)v;
    free(need);
    for (size_t i = 1; i < n; ++i) { /* by at (stable) */
        int v = order[i];
        size_t j = i;
        while (j && t->vals[order[j - 1]].at > t->vals[v].at) order[j] = order[j - 1], --j;
        order[j] = v;
    }
    for (int r = 0; r < 32; ++r) x_until[r] = d_until[r] = -1;
    bool ok = true;
    for (size_t i = 0; i < n && ok; ++i) {
        TVal *v = &t->vals[order[i]];
        if (v->last < 0) v->last = v->at; /* made, never read: a register for the moment */
        bool d = v->rep == R_NUM;
        const uint8_t *pool = d ? tier_d : tier_x;
        unsigned size = d ? sizeof(tier_d) : sizeof(tier_x);
        int *until = d ? d_until : x_until;
        int pick = -1;
        if (v->aliased) {
            v->reg = t->vals[v->a].reg;
            continue;
        }
        if (v->rep == R_INT && (v->op == T_ADD || v->op == T_SUB)) /* an operand's register where it is read last (where it may give up the code computes in scratch) */
            for (int o = 0; o < 2 && pick < 0; ++o) {
                const TVal *x = &t->vals[o ? v->b : v->a];
                if (x->reg >= 0 && x->rep != R_NUM && x->last == v->at && until[x->reg] == v->at) pick = x->reg;
            }
        if (v->op == T_PHI) /* a phi: an input's register when it is free (no move on that way) */
            for (unsigned w = 0; w < v->ins && pick < 0; ++w) {
                const TVal *in = &t->vals[v->in[w]];
                if (in->reg >= 0 && (in->rep == R_NUM) == d && until[in->reg] < v->at) pick = in->reg;
            }
        for (unsigned r = 0; r < size && pick < 0; ++r)
            if (until[pool[r]] < v->at) pick = pool[r];
        if (pick < 0) {
            if (getenv("PURE_TIER_DUMP")) {
                fprintf(stderr, "tier: no register for v%d at %d; held:", order[i], v->at);
                for (size_t j = 0; j < i; ++j)
                    if (t->vals[order[j]].reg >= 0 && t->vals[order[j]].last >= v->at && (t->vals[order[j]].rep == R_NUM) == d)
                        fprintf(stderr, " v%d(%d-%d)", order[j], t->vals[order[j]].at, t->vals[order[j]].last);
                fprintf(stderr, "\n");
            }
            ok = false;
            break;
        }
        v->reg = pick;
        until[pick] = v->last;
    }
    free(order);
    if (!ok) t->why = "registers";
    return ok;
}

/* ---- The tier: its code */

enum { TIER_WORK = 30 }; /* x30: the work the event may still do (its budget less what it did) */
enum { NEEDS_KEY = 1, NEEDS_POSITION = 2, NEEDS_FULL = 4 };

static unsigned treg(const Tier *t, int v) { return (unsigned)t->vals[v].reg; }

/* The tier's values may hold x20-x24, x27 and x28 (the generic code's
 * stack, values, event, app, lists, loops and locals): where it begins and
 * ends it reads what it needs of them from the frame (x19), and gives them
 * back before the generic code or a step runs. */
enum { B_LOCALS, B_VALUES, B_APP, B_LISTS, B_LOOPS, B_EV, B_STACK };
static unsigned tbase(Tier *t, unsigned which, unsigned into) {
    Emit *e = t->k->e;
    switch (which) {
    case B_LOCALS: load64(e, into, X19, offsetof(Frame, ev)), load64(e, into, into, offsetof(Event, locals)); break;
    case B_VALUES: load64(e, into, X19, offsetof(Frame, values)); break;
    case B_APP: load64(e, into, X19, offsetof(Frame, app)); break;
    case B_LISTS:
        load64(e, into, X19, offsetof(Frame, app));
        mov_imm(e, X10, offsetof(PureApp, lists));
        add_reg(e, into, into, X10);
        break;
    case B_LOOPS: load64(e, into, X19, offsetof(Frame, loops)); break;
    case B_EV: load64(e, into, X19, offsetof(Frame, ev)); break;
    default: load64(e, into, X19, offsetof(Frame, stack)); break;
    }
    return into;
}

/* x20-x24, x27 and x28 as the generic code keeps them. */
static void tier_fixed(Tier *t) {
    Emit *e = t->k->e;
    load64(e, X20, X19, offsetof(Frame, stack));
    load64(e, X21, X19, offsetof(Frame, values));
    load64(e, X22, X19, offsetof(Frame, ev));
    load64(e, X23, X19, offsetof(Frame, app));
    mov_imm(e, X24, offsetof(PureApp, lists));
    add_reg(e, X24, X23, X24);
    load64(e, X27, X19, offsetof(Frame, loops));
    load64(e, X28, X22, offsetof(Event, locals));
}

/* Value v as a number in a d register (its own, or scratch made of its integer). */
static unsigned td(Tier *t, int v, unsigned scratch) {
    const TVal *x = &t->vals[v];
    if (x->rep == R_NUM) return (unsigned)x->reg;
    fp2(t->k->e, x->rep == R_BOOL ? UCVTF_X : SCVTF_X, scratch, (unsigned)x->reg);
    return scratch;
}

/* Value v as an integer in an x register: an integer's or a bool's own; a number's made whole into scratch, else to fail. */
static unsigned tx(Tier *t, int v, unsigned scratch, int fail) {
    Emit *e = t->k->e;
    const TVal *x = &t->vals[v];
    if (x->rep != R_NUM) return (unsigned)x->reg;
    unsigned d = (unsigned)x->reg;
    fp2(e, FCVTZS_X, scratch, d);
    fp2(e, SCVTF_X, D1, scratch);
    fcmp(e, d, D1);
    b_cond(e, NE, fail);
    fp2(e, FMOV_XD, X17, d);
    mov_imm(e, X9, UINT64_C(0x8000000000000000));
    cmp_reg(e, X17, X9); /* -0 */
    b_cond(e, EQ, fail);
    return scratch;
}

static int tfail(Tier *t, int snap) {
    t->snaps[snap].needed = true;
    return t->snaps[snap].label;
}

/* An integer below 2^53 in size in r, else to fail (from 0: its bits past 53 none). */
static void tsmall_check(Tier *t, unsigned r, int fail) { /* from -2^53 below 2^53: its bits from 53 up its sign's */
    Emit *e = t->k->e;
    put(e, 0x9340FC00 | 53u << 16 | r << 5 | X10); /* ASR x10, r, #53 */
    cmp_asr(e, X10, r, 63);
    b_cond(e, NE, fail);
}

static void tsmall_from_0(Tier *t, unsigned r, int fail) {
    lsr_imm(t->k->e, X10, r, 53);
    cbnz_x(t->k->e, X10, fail);
}

/* A number in d finite, else to fail. */
static void tfinite_check(Tier *t, unsigned d, int fail) { /* its exponent's bits all ones: not finite (in integers: off the number pipes, 040 newton.c) */
    Emit *e = t->k->e;
    fp2(e, FMOV_XD, X10, d);
    put(e, 0xD374F800 | X10 << 5 | X10); /* UBFX x10, x10, #52, #11 */
    cmp_imm(e, X10, 0x7FF);
    b_cond(e, EQ, fail);
}

/* Affine value x of loop l at the round in register kr (XZR: round 0)
 * into register into (temps: two scratch registers past it), each node from
 * 0 and below 2^53 (else to fail; x10 spent). */
static void tier_affine_at(Tier *t, int x, int l, unsigned kr, unsigned into, const unsigned *temps, int fail) {
    Emit *e = t->k->e;
    const TVal *v = &t->vals[x];
    if (x == t->loops[l].k) {
        mov_reg(e, into, kr);
        return;
    }
    if (!tin_loop(t, v->loop, l) || v->loop == -1) {
        if (v->op == T_CONST) mov_imm(e, into, (uint64_t)(int64_t)v->num);
        else mov_reg(e, into, treg(t, x));
        tsmall_from_0(t, into, fail);
        return;
    }
    tier_affine_at(t, v->a, l, kr, into, temps, fail);
    tier_affine_at(t, v->b, l, kr, temps[0], temps + 1, fail);
    if (v->op == T_ADD) add_reg(e, into, into, temps[0]);
    else if (v->op == T_SUB) sub_reg(e, into, into, temps[0]);
    else {
        put(e, 0x9BC07C00 | temps[0] << 16 | into << 5 | X10); /* UMULH */
        cbnz_x(e, X10, fail);
        mul_reg(e, into, into, temps[0]);
    }
    tsmall_from_0(t, into, fail);
}

/* The step of an affine x (tier_bounds) a round of loop l: 1 for its round,
 * 0 for what the loop does not make, a sum's or difference's of its sides',
 * a product's its varying side's times the other. */
static void tier_affine_step(Tier *t, int x, int l, unsigned into, const unsigned *temps) {
    Emit *e = t->k->e;
    const TVal *v = &t->vals[x];
    if (x == t->loops[l].k) {
        mov_imm(e, into, 1);
        return;
    }
    if (!tin_loop(t, v->loop, l) || v->loop == -1) {
        mov_imm(e, into, 0);
        return;
    }
    if (v->op == T_MUL) {
        bool a_out = !tin_loop(t, t->vals[v->a].loop, l) || t->vals[v->a].loop == -1;
        int same = a_out ? v->a : v->b, varies = a_out ? v->b : v->a;
        tier_affine_step(t, varies, l, into, temps);
        if (t->vals[same].op == T_CONST) mov_imm(e, temps[0], (uint64_t)(int64_t)t->vals[same].num);
        else mov_reg(e, temps[0], treg(t, same));
        mul_reg(e, into, into, temps[0]);
        return;
    }
    tier_affine_step(t, v->a, l, into, temps);
    tier_affine_step(t, v->b, l, temps[0], temps + 1);
    if (v->op == T_ADD) add_reg(e, into, into, temps[0]);
    else sub_reg(e, into, into, temps[0]);
}

/* The moves a way into merge g makes: each phi given its input (a parallel move: what a move would overwrite is moved first, a cycle through scratch). */
typedef struct { unsigned src, dst; bool src_d, dst_d, convert_bool, less; } TMove; /* less: the source less one */
static void tier_moves_as(Tier *t, int g, int way, int k, int next);
static void tier_moves(Tier *t, int g, int way) { tier_moves_as(t, g, way, -1, -1); }

/* Value k (a latch's round, tlatch_k) taken as next - 1. */
static void tier_moves_as(Tier *t, int g, int way, int k, int next) {
    Emit *e = t->k->e;
    const TMerge *m = &t->merges[g];
    TMove moves[VARS];
    unsigned n = 0;
    if (!m->phis || way < 0) return;
    for (int var = 0; var < VARS; ++var) {
        int phi = m->phis[var];
        if (phi < 0 || t->vals[phi].reg < 0 || (unsigned)way >= t->vals[phi].ins) continue;
        const TVal *p = &t->vals[phi], *in = &t->vals[p->in[way]];
        bool less = k >= 0 && p->in[way] == k;
        if (less) in = &t->vals[next];
        if (in->reg < 0) continue;
        bool src_d = in->rep == R_NUM, dst_d = p->rep == R_NUM;
        if (!less && src_d == dst_d && in->reg == p->reg) continue;
        moves[n++] = (TMove){(unsigned)in->reg, (unsigned)p->reg, src_d, dst_d, in->rep == R_BOOL, less};
    }
    while (n) {
        bool done = false;
        for (unsigned i = 0; i < n; ++i) {
            bool blocked = false; /* another move still reads what this one writes */
            for (unsigned j = 0; j < n && !blocked; ++j)
                blocked = j != i && moves[j].src == moves[i].dst && moves[j].src_d == moves[i].dst_d;
            if (blocked) continue;
            TMove mv = moves[i];
            if (mv.less && mv.dst_d) { /* a round, as a number */
                sub_imm(e, X17, mv.src, 1);
                fp2(e, SCVTF_X, mv.dst, X17);
            } else if (mv.less) sub_imm(e, mv.dst, mv.src, 1);
            else if (mv.src_d && mv.dst_d) put(e, 0x1E604000 | mv.src << 5 | mv.dst); /* FMOV Dd, Dn */
            else if (!mv.src_d && !mv.dst_d) mov_reg(e, mv.dst, mv.src);
            else if (!mv.src_d) fp2(e, mv.convert_bool ? UCVTF_X : SCVTF_X, mv.dst, mv.src);
            else fp2(e, FCVTZS_X, mv.dst, mv.src); /* a whole number's (its phi an integer's) */
            moves[i] = moves[--n];
            done = true;
            break;
        }
        if (!done) { /* a cycle: one source through scratch */
            TMove *mv = &moves[0];
            if (mv->src_d) put(e, 0x1E604000 | mv->src << 5 | D0), mv->src = D0;
            else mov_reg(e, X16, mv->src), mv->src = X16;
        }
    }
}

/* Whether a way into merge g moves anything (its phis' registers their inputs'). */
static bool tier_has_moves(const Tier *t, int g, int way) {
    const TMerge *m = &t->merges[g];
    if (!m->phis || way < 0) return false;
    for (int var = 0; var < VARS; ++var) {
        int phi = m->phis[var];
        if (phi < 0 || t->vals[phi].reg < 0 || (unsigned)way >= t->vals[phi].ins) continue;
        const TVal *p = &t->vals[phi], *in = &t->vals[p->in[way]];
        if (in->reg < 0 || ((in->rep == R_NUM) == (p->rep == R_NUM) && in->reg == p->reg)) continue;
        return true;
    }
    return false;
}

/* A value a snapshot holds as a number in d0 (or its own d register): a
 * constant made there, as the constant it is (its register, if it has one,
 * not kept for the snapshot: another value's by then). */
static unsigned tsnap_d(Tier *t, int v) {
    const TVal *x = &t->vals[v];
    if (x->op != T_CONST) return td(t, v, D0);
    uint64_t bits;
    memcpy(&bits, &x->num, sizeof(bits));
    mov_imm(t->k->e, X9, bits);
    fp2(t->k->e, FMOV_DX, D0, X9);
    return D0;
}

/* What the tier changed, back where the executor keeps it: the variables
 * whose values differ from memory's (in vars: a snapshot's or the end's),
 * the work, the lists it set (their versions). */
static void tier_write(Tier *t, const int *vars) {
    Block *k = t->k;
    Emit *e = k->e;
    tbase(t, B_LOCALS, X17);
    tbase(t, B_VALUES, X16);
    for (int var = 0; var < V_STACK; ++var) {
        int v = vars[var];
        if (!tchanged(t, var, v) || (t->vals[v].reg < 0 && t->vals[v].op != T_CONST)) continue;
        unsigned d = tsnap_d(t, v);
        if (var < V_STATE) store_d(e, d, X17, (size_t)var * sizeof(PureValue));
        else store_d(e, d, X16, (size_t)(var - V_STATE) * sizeof(PureValue));
    }
    int budgeted = new_label(e); /* what it did: its budget less what it may still do */
    tbase(t, B_APP, X17);
    load64(e, X10, X17, offsetof(PureApp, work_budget));
    cbnz_x(e, X10, budgeted);
    mov_imm(e, X10, PURE_WORK_DEFAULT);
    bind(e, budgeted);
    sub_reg(e, X10, X10, TIER_WORK);
    store64(e, X10, X17, offsetof(PureApp, work));
    tbase(t, B_LISTS, X17);
    for (unsigned list = 0; list < 32; ++list)
        if (t->written >> list & 1) {
            load64(e, X16, X17, list * sizeof(PureList *));
            load64(e, X10, X16, pure_list_layout[LIST_VERSION_AT]);
            add_imm(e, X10, X10, 1);
            store64(e, X10, X16, pure_list_layout[LIST_VERSION_AT]);
        }
}

/* The open loops' records as the executor keeps them, from the outermost
 * (opened by the generic code: its round) to loop n; the frame's depth and
 * end, the event's walk and row. */
static void tier_records(Tier *t, int n) {
    Block *k = t->k;
    Emit *e = k->e;
    int chain[PURE_MODULE_NESTING], depth = 0;
    for (int l = n; l >= 0; l = t->loops[l].parent) chain[depth++] = l;
    load64(e, X0, X19, offsetof(Frame, arena));
    load64(e, X0, X0, offsetof(PureArena, used)); /* the arena as the loops began (the tier takes none of it) */
    tbase(t, B_LOOPS, X17);
    tbase(t, B_EV, X16);
    for (int d = depth - 1, at = 0; d >= 0; --d, ++at) {
        const TLoop *l = &t->loops[chain[d]];
        const LoopAt *bl = &k->loops[l->li];
        size_t off = loop_offset(bl);
        unsigned round = treg(t, l->k);
        if (at == 0) { /* the outermost: its round */
            if (l->walk) {
                store64(e, round, X17, off + offsetof(Loop, walk) + offsetof(Walk, at));
                store64(e, round, X16, offsetof(Event, row));
            } else
                store64(e, round, X17, off + offsetof(Loop, k));
            continue;
        }
        add_imm(e, X9, X17, (uint32_t)off);
        mov_w(e, X10, bl->start);
        store_w(e, X10, X9, offsetof(Loop, start));
        mov_w(e, X10, bl->end);
        store_w(e, X10, X9, offsetof(Loop, end));
        mov_w(e, X10, k->loops[t->loops[l->parent].li].end);
        store_w(e, X10, X9, offsetof(Loop, outer));
        store64(e, X0, X9, offsetof(Loop, mark));
        mov_w(e, X10, l->walk);
        store8(e, X10, X9, offsetof(Loop, walking));
        store8(e, XZR, X9, offsetof(Loop, integer));
        if (l->walk) {
            mov_w(e, X10, l->list);
            store_w(e, X10, X9, offsetof(Loop, walk) + offsetof(Walk, list));
            store64(e, round, X9, offsetof(Loop, walk) + offsetof(Walk, at));
            mov_imm(e, X10, SIZE_MAX);
            store64(e, X10, X9, offsetof(Loop, walk) + offsetof(Walk, end));
            store8(e, XZR, X9, offsetof(Loop, walk) + offsetof(Walk, removed));
            load64(e, X10, X16, offsetof(Event, walk));
            store64(e, X10, X9, offsetof(Loop, around));
            load64(e, X10, X16, offsetof(Event, row));
            store64(e, X10, X9, offsetof(Loop, row));
            add_imm(e, X10, X9, offsetof(Loop, walk));
            store64(e, X10, X16, offsetof(Event, walk));
            store64(e, round, X16, offsetof(Event, row));
        } else {
            store64(e, treg(t, l->count), X9, offsetof(Loop, count));
            store64(e, round, X9, offsetof(Loop, k));
            if (t->vals[l->low].op == T_CONST) { /* written as the constant it is */
                uint64_t bits;
                memcpy(&bits, &t->vals[l->low].num, sizeof(bits));
                mov_imm(e, X10, bits);
                store64(e, X10, X9, offsetof(Loop, low));
            } else
                store_d(e, td(t, l->low, D0), X9, offsetof(Loop, low));
            load64(e, X10, X16, offsetof(Event, locals));
            add_imm(e, X10, X10, (uint32_t)(l->slot * sizeof(PureValue)));
            store64(e, X10, X9, offsetof(Loop, slot));
        }
    }
    const LoopAt *inner = &k->loops[t->loops[n].li];
    mov_w(e, X10, inner->end);
    store_w(e, X10, X19, offsetof(Frame, end));
    mov_w(e, X10, (uint32_t)depth);
    store_w(e, X10, X19, offsetof(Frame, depth));
}

/* A count opened in the tier paid for its every round: where it leaves in
 * round k (a break, or giving up), the rounds after k given back (the
 * generic code pays for each it runs). */
static void tier_refund(Tier *t, int l) {
    Emit *e = t->k->e;
    const TLoop *lp = &t->loops[l];
    if (l == 0 || lp->walk) return;
    sub_reg(e, X16, treg(t, lp->count), treg(t, lp->k));
    sub_imm(e, X16, X16, 1);
    mov_imm(e, X17, (uint64_t)lp->length + 1);
    mul_reg(e, X16, X16, X17);
    add_reg(e, TIER_WORK, TIER_WORK, X16);
}

/* Where snapshot s gives up: what changed written back, the stack and the
 * loops as the executor keeps them at its instruction, the rest of the
 * round the executor's, the generic code going on. */
static void tier_stub(Tier *t, int s) {
    Block *k = t->k;
    Emit *e = k->e;
    const TSnap *snap = &t->snaps[s];
    const LoopAt *l0 = &k->loops[t->nest];
    bind(e, snap->label);
    for (int l = snap->loop; l > 0; l = t->loops[l].parent) tier_refund(t, l);
    tier_write(t, snap->vars);
    tbase(t, B_STACK, X17);
    for (unsigned h = l0->base; h < snap->height; ++h) {
        int v = snap->vars[V_STACK + h];
        if (v < 0 || (t->vals[v].reg < 0 && t->vals[v].op != T_CONST)) continue;
        store_d(e, tsnap_d(t, v), X17, AT(h));
        if (t->vals[v].kind == K_INDEX) {
            mov_w(e, X9, (uint32_t)t->vals[v].list & 0xFFFF);
            store_h(e, X9, X17, AT(h) + OWNER);
        }
    }
    tier_records(t, snap->loop);
    tier_fixed(t);
    set_sp(k, snap->height);
    mov_reg(e, X0, X19);
    mov_w(e, X1, snap->pc);
    call(e, (uintptr_t)pure_run_rest);
    const LoopAt *l = &k->loops[t->loops[snap->loop].li];
    int left = new_label(e);
    k->fresh = 0;
    cbnz_w(e, X0, left);
    refresh(k, kept_at(k, t->loops[snap->loop].li)); /* the round ran: the generic code's iteration */
    b(e, l->again[0]);
    bind(e, left);
    k->fresh = 0;
    cmp_w_imm(e, X0, STEP_SKIP);
    b_cond(e, NE, k->exit);
    refresh(k, kept_at(k, l->parent)); /* it left the loop */
    b(e, l->after[0]);
}

/* A comparison's flags; the condition its true sets. */
static unsigned tcompare(Tier *t, const TVal *v) {
    Emit *e = t->k->e;
    bool ints = t->vals[v->a].rep != R_NUM && t->vals[v->b].rep != R_NUM;
    if (ints && timm(t, v->b)) cmp_imm(e, treg(t, v->a), (uint32_t)t->vals[v->b].num);
    else if (ints) cmp_reg(e, treg(t, v->a), treg(t, v->b));
    else {
        fcmp(e, td(t, v->a, D0), td(t, v->b, D1));
        return v->op == T_LT ? MI : v->op == T_LE ? LS : v->op == T_GT ? GT : v->op == T_GE ? GE : v->op == T_EQ ? EQ : NE; /* false unordered (NE true) */
    }
    return v->op == T_LT ? LT : v->op == T_LE ? LE : v->op == T_GT ? GT : v->op == T_GE ? GE : v->op == T_EQ ? EQ : NE;
}

/* A HASHED list's key (value v's) found by its table, out of the line (at
 * label hashed): the slot of the key's bits (member_slot) and on while
 * another key holds it; there, its cell at that slot (FIND_GET) or 1 (HAS);
 * a VACANT slot, not there: to fail (FIND_GET) or 0 (HAS). Its members
 * from its bits (their bit 1 cleared), its mask from the list. */
static void tier_probe(Tier *t, const TVal *v, unsigned r, unsigned key, unsigned bits, int hashed, int fail, int done) {
    Emit *e = t->k->e;
    int probe = new_label(e), there = new_label(e);
    bind(e, hashed);
    fp2(e, SCVTF_X, D0, key);
    fp2(e, FMOV_XD, X16, D0); /* the key's bits (a whole number's: never -0) */
    mov_imm(e, X17, UINT64_C(0xff51afd7ed558ccd));
    put(e, 0xCA400000 | X16 << 16 | 33u << 10 | X16 << 5 | X10); /* EOR x10, x16, x16, LSR #33 */
    mul_reg(e, X17, X10, X17);
    put(e, 0xCA400000 | X17 << 16 | 33u << 10 | X17 << 5 | X17); /* EOR x17, x17, x17, LSR #33 */
    load64(e, X9, tbase(t, B_LISTS, X9), (size_t)v->list * sizeof(PureList *));
    load64(e, X10, X9, pure_list_layout[LIST_MEMBER_CAPACITY_AT]);
    sub_imm(e, X10, X10, 1);
    put(e, 0x8A000000 | X10 << 16 | X17 << 5 | X17);  /* AND x17, x17, x10: the slot */
    put(e, 0x927EF400 | bits << 5 | X9);               /* AND x9, bits, #~3: the members */
    mov_imm(e, X1, UINT64_C(0x7FF8000000000001));      /* VACANT */
    bind(e, probe);
    load64_index(e, X0, X9, X17);
    cmp_reg(e, X0, X16);
    b_cond(e, EQ, there);
    cmp_reg(e, X0, X1);
    if (v->op == T_HAS) {
        cset(e, r, NE);
        b_cond(e, EQ, done);
    } else
        b_cond(e, EQ, fail);
    add_imm(e, X17, X17, 1);
    put(e, 0x8A000000 | X10 << 16 | X17 << 5 | X17);
    b(e, probe);
    bind(e, there);
    if (v->op == T_HAS) mov_w(e, r, 1);
    else load32_index(e, r, treg(t, v->c), X17); /* its cell at its slot: four bytes (pure_list_suits widened one) */
    b(e, done);
}

/* A find in a HASHED list kept to be made after the items. */
static void tier_later(Tier *t, TProbe probe) {
    if (!grow_array((void **)&t->probes, &t->probe_room, t->probe_count + 1, sizeof(*t->probes))) t->k->e->failed = true;
    else t->probes[t->probe_count++] = probe;
}

/* A value's code. */
static void tier_value(Tier *t, int vi) {
    Block *k = t->k;
    Emit *e = k->e;
    TVal *v = &t->vals[vi];
    const PureModule *m = k->m;
    unsigned r = v->reg >= 0 ? (unsigned)v->reg : X16;
    int fail = v->snap >= 0 ? tfail(t, v->snap) : -1;
    const TVal *a = v->a >= 0 ? &t->vals[v->a] : NULL, *b2 = v->b >= 0 ? &t->vals[v->b] : NULL;
    if (v->step >= 0 && (v->op == T_GET || v->op == T_FIND_GET)) { /* its cell where its pointer is (tier_steps) */
        put(e, 0xB9400000 | treg(t, v->step) << 5 | r); /* LDR Wr, [pointer] */
        return;
    }
    switch (v->op) {
    case T_CONST:
        if (v->rep == R_INT) mov_imm(e, r, (uint64_t)(int64_t)v->num);
        else if (v->rep == R_BOOL) mov_w(e, r, v->num != 0);
        else {
            uint64_t bits;
            memcpy(&bits, &v->num, sizeof(bits));
            mov_imm(e, X16, bits);
            fp2(e, FMOV_DX, r, X16);
        }
        return;
    case T_IN: {
        const LoopAt *l0 = &k->loops[t->nest];
        size_t off = loop_offset(l0);
        if (v->var == T_LIST) { /* a list's part */
            tbase(t, B_LISTS, X17);
            load64(e, X16, X17, (size_t)v->list * sizeof(PureList *));
            if (v->field < 16) { /* a column, its address tagged with its cells' width */
                load64(e, r, X16, pure_list_layout[LIST_COLUMNS_AT] + v->field * sizeof(void *));
                load8(e, X9, X16, pure_list_layout[LIST_WIDTH_AT] + v->field);
                orr_lsl(e, r, r, X9, 56);
            } else if (v->field == L_COUNT) load64(e, r, X16, pure_list_layout[LIST_COUNT_AT]);
            else if (v->field == L_BITS) { /* bit 0 set: every key below the count there (as pure_list_suits found it), no bit to test; bit 1: HASHED */
                load64(e, r, X16, pure_list_layout[LIST_MEMBERS_AT]);
                load8(e, X17, X16, pure_list_layout[LIST_FULL_AT]);
                orr_reg(e, r, r, X17);
                load8(e, X17, X16, pure_list_layout[LIST_FORM_AT]);
                cmp_w_imm(e, X17, HASHED_FORM);
                cset(e, X17, EQ);
                orr_lsl(e, r, r, X17, 1);
            } else if (v->field == L_LIMIT) { /* the keys the bits reach: none without bits; a full list's, its count; a HASHED one's all */
                load64(e, r, X16, pure_list_layout[LIST_MEMBER_CAPACITY_AT]);
                lsl_imm(e, r, r, 6);
                load8(e, X17, X16, pure_list_layout[LIST_BITMAP_AT]);
                cmp_w_imm(e, X17, 0);
                csel(e, r, r, XZR, NE);
                load8(e, X9, X16, pure_list_layout[LIST_FULL_AT]);
                load64(e, X17, X16, pure_list_layout[LIST_COUNT_AT]);
                cmp_w_imm(e, X9, 0);
                csel(e, r, X17, r, NE);
                load8(e, X17, X16, pure_list_layout[LIST_FORM_AT]);
                cmp_w_imm(e, X17, HASHED_FORM);
                put(e, 0xDA9F1000 | r << 5 | r); /* CSINV r, r, xzr, NE: all ones where HASHED */
            }
            return;
        }
        if (v->var == V_ROUND0) {
            tbase(t, B_LOOPS, X17);
            load64(e, r, X17, off + (k->loops[t->nest].walk ? offsetof(Loop, walk) + offsetof(Walk, at) : offsetof(Loop, k)));
            return;
        }
        if (v->var == V_COUNT0) {
            tbase(t, B_LOOPS, X17);
            if (!l0->walk) {
                load64(e, r, X17, off + offsetof(Loop, count));
                return;
            }
            load64(e, r, X17, off + offsetof(Loop, walk) + offsetof(Walk, end));
            tbase(t, B_LISTS, X17);
            load64(e, X16, X17, (size_t)l0->list * sizeof(PureList *));
            load64(e, X17, X16, pure_list_layout[LIST_COUNT_AT]);
            cmp_reg(e, r, X17);
            csel(e, r, r, X17, LO);
            return;
        }
        size_t at = v->var == V_LOW0 ? off + offsetof(Loop, low) : 0;
        unsigned base = tbase(t, v->var == V_LOW0 ? B_LOOPS : v->var < V_STATE ? B_LOCALS : B_VALUES, X17);
        if (v->var != V_LOW0) at = (size_t)(v->var < V_STATE ? v->var : v->var - V_STATE) * sizeof(PureValue);
        if (v->rep == R_NUM) {
            load_d(e, r, base, at);
            return;
        }
        load_d(e, D0, base, at);
        if (v->rep == R_BOOL) {
            fcmp_zero(e, D0);
            cset(e, r, NE);
            return;
        }
        fp2(e, FCVTZS_X, r, D0); /* whole where the tier begins (checked there) */
        return;
    }
    case T_FIELD: {
        const PureField *f = &m->field[m->input[t->input].field_start + v->field];
        tbase(t, B_APP, X16);
        load64(e, X16, X16, offsetof(PureApp, payload) + offsetof(PureSpan, data));
        if (f->offset) {
            mov_imm(e, X17, f->offset);
            add_reg(e, X16, X16, X17);
        }
        if (f->type == PURE_FIELD_U32) put(e, 0xB9400000 | X16 << 5 | r); /* LDR Wr, [x16] */
        else if (f->type == PURE_FIELD_BOOL) load8(e, r, X16, 0);
        else put(e, 0xFD400000 | X16 << 5 | r); /* LDR Dr, [x16] */
        return;
    }
    case T_COUNT: mov_reg(e, r, treg(t, v->b)); return;
    case T_STEP0: { /* a loop's pointer at its first round: the column (read from its list: no register kept for it) plus the key there, times four */
        static const unsigned temps[] = {X16, X17, X9};
        tier_affine_at(t, v->a, v->c, XZR, X0, temps, tfail(t, t->loops[v->c].open_snap));
        const TVal *column = &t->vals[v->b];
        load64(e, X16, tbase(t, B_LISTS, X16), (size_t)column->list * sizeof(PureList *));
        load64(e, X16, X16, pure_list_layout[LIST_COLUMNS_AT] + column->field * sizeof(void *));
        add_lsl(e, r, X16, X0, 2);
        return;
    }
    case T_STRIDE: { /* its step a round, times four */
        static const unsigned temps[] = {X16, X17, X9};
        tier_affine_step(t, v->a, v->c, X0, temps);
        lsl_imm(e, r, X0, 2);
        return;
    }
    case T_SPAN: { /* a count's rounds: its bounds whole within 2^53, else LIMIT */
        unsigned low = tx(t, v->a, X0, fail), high = tx(t, v->b, X1, fail);
        if (a->rep == R_NUM) tsmall_check(t, low, fail);
        if (b2->rep == R_NUM) tsmall_check(t, high, fail);
        sub_reg(e, X16, high, low);
        cmp_reg(e, high, low);
        csel(e, r, X16, XZR, GT);
        return;
    }
    case T_ADD: case T_SUB: case T_MUL: case T_MIN: case T_MAX:
        if (v->rep == R_INT && (v->op == T_ADD || v->op == T_SUB) && (timm(t, v->b) || (v->op == T_ADD && timm(t, v->a)))) {
            bool right = timm(t, v->b); /* a constant in the instruction */
            unsigned x = tx(t, right ? v->a : v->b, X0, fail);
            uint32_t imm = (uint32_t)t->vals[right ? v->b : v->a].num;
            const TVal *o = &t->vals[right ? v->a : v->b];
            bool check = fail >= 0 && !v->bounded && !tsmall(v->op == T_ADD ? o->lo + imm : o->lo - imm, v->op == T_ADD ? o->hi + imm : o->hi - imm);
            unsigned to = check && r == x ? X16 : r;
            if (v->op == T_ADD) add_imm(e, to, x, imm);
            else sub_imm(e, to, x, imm);
            if (check) {
                if (o->lo >= 0 && v->op == T_ADD) tsmall_from_0(t, to, fail);
                else tsmall_check(t, to, fail);
                if (to != r) mov_reg(e, r, to);
            }
            return;
        }
        if (v->rep == R_INT) {
            unsigned x = tx(t, v->a, X0, fail), y = tx(t, v->b, X1, fail);
            bool check = fail >= 0 && !v->bounded && !(tsmall(a->lo, a->hi) && tsmall(b2->lo, b2->hi) &&
                                          (v->op == T_MUL ? tsmall(fmin(fmin(a->lo * b2->lo, a->lo * b2->hi), fmin(a->hi * b2->lo, a->hi * b2->hi)),
                                                                   fmax(fmax(a->lo * b2->lo, a->lo * b2->hi), fmax(a->hi * b2->lo, a->hi * b2->hi)))
                                                           : v->op == T_ADD ? tsmall(a->lo + b2->lo, a->hi + b2->hi)
                                                           : v->op == T_SUB ? tsmall(a->lo - b2->hi, a->hi - b2->lo) : true));
            bool from0 = a->lo >= 0 && b2->lo >= 0;
            unsigned to = check && (r == x || r == y) ? X16 : r; /* r an operand's: where it gives up the operands stay as they were */
            if (v->op == T_ADD) add_reg(e, to, x, y);
            else if (v->op == T_SUB) sub_reg(e, to, x, y);
            else if (v->op == T_MUL) {
                double most = fmax(fabs(a->lo), fabs(a->hi)) * fmax(fabs(b2->lo), fabs(b2->hi));
                mul_reg(e, to, x, y);
                if (check && most >= (from0 ? 0x1p64 : 0x1p63)) { /* past 64 bits: the high half */
                    if (from0) {
                        put(e, 0x9BC07C00 | y << 16 | x << 5 | X17); /* UMULH */
                        cbnz_x(e, X17, fail);
                    } else {
                        smulh(e, X17, x, y);
                        cmp_asr(e, X17, to, 63);
                        b_cond(e, NE, fail);
                    }
                }
                if (v->negzero && !v->bounded) { /* 0 of a number below 0: -0, not an integer (a bounded node is from 0) */
                    int nonzero = new_label(e);
                    cbnz_x(e, to, nonzero);
                    orr_reg(e, X17, x, y);
                    tbnz(e, X17, 63, fail);
                    bind(e, nonzero);
                }
            } else {
                cmp_reg(e, x, y);
                csel(e, to, x, y, v->op == T_MIN ? LT : GT);
            }
            if (check) {
                if (from0 && v->op != T_SUB) tsmall_from_0(t, to, fail);
                else tsmall_check(t, to, fail);
                if (to != r) mov_reg(e, r, to);
            }
            return;
        }
        {
            unsigned x = td(t, v->a, D0), y = td(t, v->b, D1);
            uint32_t fop = v->op == T_ADD ? FADD : v->op == T_SUB ? FSUB : v->op == T_MUL ? FMUL : v->op == T_MIN ? FMIN : FMAX;
            fp3(e, fop, r, x, y);
            if (fail >= 0 && (v->op == T_ADD || v->op == T_SUB || v->op == T_MUL) && !tfinite(v) && !v->deferred)
                tfinite_check(t, r, v->retry >= 0 ? tfail(t, v->retry) : fail);
            return;
        }
    case T_DIV:
        if (v->integers) { /* whole a from 0 over whole b above 0: the quotient's floor */
            put(e, 0x9AC00800 | treg(t, v->b) << 16 | treg(t, v->a) << 5 | r); /* UDIV */
            return;
        }
        {
            unsigned x = td(t, v->a, D0), y = td(t, v->b, D1);
            if (!(b2->lo > 0 || b2->hi < 0)) {
                fcmp_zero(e, y);
                b_cond(e, EQ, fail);
            }
            fp3(e, FDIV, r, x, y);
            if (!tfinite(v) && !v->deferred) tfinite_check(t, r, v->retry >= 0 ? tfail(t, v->retry) : fail);
            return;
        }
    case T_NEG:
        if (v->rep == R_INT) sub_reg(e, r, XZR, treg(t, v->a));
        else fp2(e, FNEG, r, td(t, v->a, D0));
        return;
    case T_FLOOR:
        if (a->rep != R_NUM) {
            if (r != treg(t, v->a)) mov_reg(e, r, treg(t, v->a));
            return;
        }
        if (v->rep == R_INT) {
            fp2(e, FRINTM, D0, treg(t, v->a));
            fp2(e, FCVTZS_X, r, D0);
        } else
            fp2(e, FRINTM, r, treg(t, v->a));
        return;
    case T_LT: case T_LE: case T_GT: case T_GE: case T_EQ: case T_NE:
        cset(e, r, tcompare(t, v));
        return;
    case T_NOT: eor_w_one(e, r, treg(t, v->a)); return;
    case T_AND: and_w(e, r, treg(t, v->a), treg(t, v->b)); return;
    case T_OR: orr_w(e, r, treg(t, v->a), treg(t, v->b)); return;
    case T_MOVE: { /* within its list: from 0 to its last item */
        unsigned at = treg(t, v->a), by = tx(t, v->b, X1, fail), count = treg(t, v->c);
        add_reg(e, X16, at, by);
        sub_imm(e, X17, count, 1);
        cmp_imm(e, count, 0);
        csel(e, X17, XZR, X17, EQ);
        cmp_reg(e, X16, X17);
        csel(e, X16, X16, X17, LT);
        cmp_imm(e, X16, 0);
        csel(e, r, X16, XZR, GT);
        return;
    }
    case T_GET: { /* by position: below the count (a walk's own place is), its cell (one or four bytes) */
        unsigned at = tx(t, v->a, X0, fail), column = treg(t, v->c);
        bool walked = false;
        for (unsigned l = 0; l < t->loop_count; ++l) walked = walked || (t->loops[l].walk && t->loops[l].k == v->a && t->loops[l].list == v->list);
        if (!walked && t->vals[v->a].op != T_FIND && !v->proven) {
            cmp_reg(e, at, treg(t, v->b));
            b_cond(e, HS, fail);
        }
        load32_index(e, r, column, at); /* four bytes (pure_list_suits widened one) */
        return;
    }
    case T_FIND_GET: case T_FIND: case T_HAS: { /* by key: in the bits (below their reach), its cell at its key; a HASHED list's by its table */
        unsigned key = tx(t, v->a, X0, fail), bits = treg(t, v->b);
        unsigned limit = treg(t, v->op == T_FIND_GET ? v->d : v->c);
        bool full = t->lists[v->list].full; /* every key below its count there (pure_list_suits): the bits not read */
        int hashed = thashes(t, v) ? new_label(e) : -1, done = new_label(e); /* its bits' bit 1: HASHED (its limit all ones) */
        if (v->op == T_HAS && full) {
            if (v->proven) mov_w(e, r, 1);
            else {
                cmp_reg(e, key, limit);
                cset(e, r, LO);
            }
            return;
        }
        if (v->op == T_HAS) {
            int no = new_label(e), bit = new_label(e);
            if (!v->proven) {
                cmp_reg(e, key, limit);
                b_cond(e, HS, no);
            }
            tbz(e, bits, 0, bit);
            mov_w(e, r, 1); /* full: there */
            b(e, done);
            bind(e, bit);
            if (hashed >= 0) tbnz(e, bits, 1, hashed);
            lsr_imm(e, X16, key, 6);
            load64_index(e, X16, bits, X16);
            lsr_reg(e, X16, X16, key);
            and_w_one(e, r, X16);
            b(e, done);
            bind(e, no);
            if (hashed >= 0) tbnz(e, bits, 1, hashed); /* a HASHED list's key past its limit: -1 */
            mov_w(e, r, 0);
            if (hashed >= 0) tier_later(t, (TProbe){vi, r, key, bits, hashed, -1, done});
            bind(e, done);
            return;
        }
        if (!v->proven) {
            cmp_reg(e, key, limit);
            b_cond(e, HS, fail);
        }
        if (!full) {
            int there = new_label(e);
            tbnz(e, bits, 0, there); /* full as it began: every key below its count there */
            if (hashed >= 0) tbnz(e, bits, 1, hashed);
            lsr_imm(e, X16, key, 6);
            load64_index(e, X16, bits, X16);
            lsr_reg(e, X16, X16, key);
            tbz(e, X16, 0, fail);
            bind(e, there);
        }
        if (v->op == T_FIND) { /* a full list's position: its key */
            if (r != key) mov_reg(e, r, key);
            return;
        }
        load32_index(e, r, treg(t, v->c), key); /* four bytes (pure_list_suits widened one) */
        if (hashed >= 0) tier_later(t, (TProbe){vi, r, key, bits, hashed, fail, done});
        bind(e, done);
        return;
    }
    case T_SET: { /* by position (below the count; a position found is), a whole number fitting its column's cells */
        unsigned at = tx(t, v->a, X0, fail), value = tx(t, v->b, X1, fail), column = treg(t, v->c);
        if (t->vals[v->a].op != T_FIND && !v->proven) {
            cmp_reg(e, at, treg(t, v->d));
            b_cond(e, HS, fail);
        }
        lsr_imm(e, X16, value, 32); /* fits its cells of four bytes (else the executor widens them) */
        cbnz_x(e, X16, fail);
        store32_index(e, value, column, at);
        return;
    }
    default: e->failed = true; return;
    }
}

/* The tier's code for the nest: its entry (each list as it assumes, each
 * variable it takes as whole a whole number; else the generic code runs the
 * round), its values and items, where it gives up. */
static void tier_emit(Tier *t, int entry, int fallback) {
    Block *k = t->k;
    Emit *e = k->e;
    const LoopAt *l0 = &k->loops[t->nest];
    bind(e, entry);
    for (unsigned list = 0; list < PURE_MODULE_STATES; ++list) { /* each list as the tier reads it (pure_list_suits) */
        const TList *l = &t->lists[list];
        unsigned needs = 0, narrow = 0;
        for (unsigned f = 0; f < 16; ++f) narrow |= l->part[f] >= 0 ? 1u << f : 0;
        if (l->key) needs |= NEEDS_KEY;
        if (l->position) needs |= NEEDS_POSITION;
        if (l->full) needs |= NEEDS_FULL;
        if (!needs && !narrow) continue;
        load64(e, X0, X24, list * sizeof(PureList *));
        mov_w(e, X1, needs);
        mov_w(e, X2, narrow);
        call(e, (uintptr_t)pure_list_suits);
        cbz_w(e, X0, fallback);
    }
    for (size_t i = 0; i < t->entry_count; ++i) { /* what it reads where it begins */
        int v = t->entry[i];
        TVal *x = &t->vals[v];
        if (x->reg < 0) continue;
        if (x->op == T_IN && x->var == V_LOW0) { /* the outermost count's low: from 0 */
            tier_value(t, v);
            tbnz(e, (unsigned)x->reg, 63, fallback);
            continue;
        }
        if (x->op == T_IN && (x->var == V_COUNT0 || x->var == V_ROUND0)) { /* its rounds below 2^32 */
            tier_value(t, v);
            lsr_imm(e, X16, (unsigned)x->reg, 32);
            cbnz_x(e, X16, fallback);
            continue;
        }
        if (x->op == T_IN && x->var >= 0 && x->var < V_STACK && x->rep == R_INT && x->kind != K_INDEX) { /* taken as whole: checked */
            size_t at = (size_t)(x->var < V_STATE ? x->var : x->var - V_STATE) * sizeof(PureValue);
            load_d(e, D0, tbase(t, x->var < V_STATE ? B_LOCALS : B_VALUES, X17), at);
            fp2(e, FCVTZS_X, (unsigned)x->reg, D0);
            fp2(e, SCVTF_X, D1, (unsigned)x->reg);
            fcmp(e, D0, D1);
            b_cond(e, NE, fallback);
            fp2(e, FMOV_XD, X17, D0);
            mov_imm(e, X9, UINT64_C(0x8000000000000000));
            cmp_reg(e, X17, X9);
            b_cond(e, EQ, fallback);
            tsmall_check(t, (unsigned)x->reg, fallback);
            continue;
        }
        tier_value(t, v);
    }
    int budgeted = new_label(e); /* the work it may still do */
    tbase(t, B_APP, X17);
    load64(e, TIER_WORK, X17, offsetof(PureApp, work_budget));
    cbnz_x(e, TIER_WORK, budgeted);
    mov_imm(e, TIER_WORK, PURE_WORK_DEFAULT);
    bind(e, budgeted);
    load64(e, X9, X17, offsetof(PureApp, work));
    sub_reg(e, TIER_WORK, TIER_WORK, X9);
    unsigned flags = EQ; /* a fused comparison's condition */
    for (size_t i = 0; i < t->item_count && !e->failed; ++i) {
        const TItem *it = &t->items[i];
        switch (it->kind) {
        case I_VALUE:
            if (t->vals[it->value].fused) flags = tcompare(t, &t->vals[it->value]);
            else if (t->vals[it->value].reg >= 0 || t->vals[it->value].op == T_SET) tier_value(t, it->value);
            break;
        case I_MERGE: bind(e, t->merges[it->merge].label); break;
        case I_JUMP:
            if (tbreaks(t, it)) tier_refund(t, it->loop);
            tier_moves(t, it->merge, it->way);
            if (!(i + 1 < t->item_count && t->items[i + 1].kind == I_MERGE && t->items[i + 1].merge == it->merge))
                b(e, t->merges[it->merge].label);
            break;
        case I_UNLESS: { /* its bool 0 (a fused comparison's condition false): to the merge */
            bool fused = t->vals[it->value].fused;
            int to = t->merges[it->merge].label;
            if (!tier_has_moves(t, it->merge, it->way)) {
                if (fused) b_cond(e, flags ^ 1, to);
                else cbz_w(e, treg(t, it->value), to);
                break;
            }
            int go_on = new_label(e);
            if (fused) b_cond(e, flags, go_on);
            else cbnz_w(e, treg(t, it->value), go_on);
            tier_moves(t, it->merge, it->way);
            b(e, to);
            bind(e, go_on);
            break;
        }
        case I_OPEN: { /* none to run: past it; else its work */
            const TLoop *l = &t->loops[it->loop];
            unsigned count = treg(t, l->count);
            int go_on = new_label(e), fail = tfail(t, it->snap);
            if (!tier_has_moves(t, it->merge, it->way)) cbz_x(e, count, t->merges[it->merge].label); /* none (its work then none too) */
            else {
                cbnz_x(e, count, go_on);
                tier_moves(t, it->merge, it->way);
                b(e, t->merges[it->merge].label);
                bind(e, go_on);
            }
            for (unsigned bi = 0; bi < l->bound_count; ++bi) { /* each key or position in range at both ends of its rounds */
                static const unsigned first[] = {X1, X17}, last[] = {X17, X9};
                unsigned limit = treg(t, l->bounds[bi][1]);
                tier_affine_at(t, l->bounds[bi][0], it->loop, XZR, X0, first, fail);
                sub_imm(e, X16, count, 1);
                tier_affine_at(t, l->bounds[bi][0], it->loop, X16, X1, last, fail);
                cmp_reg(e, X0, limit); /* each end below the limit (and from 0: each node is) */
                b_cond(e, HS, fail);
                cmp_reg(e, X1, limit);
                b_cond(e, HS, fail);
            }
            /* its work at once (after what may give up there): a walk's block's length times its items (the executor's
             * EACH charges it so), a count's length and iteration times its rounds (given back for those it does not run, tier_refund) */
            mov_imm(e, X16, l->walk ? l->length : (uint64_t)l->length + 1);
            if (!l->walk) { /* past 64 bits: more than it may do */
                put(e, 0x9BC07C00 | count << 16 | X16 << 5 | X17); /* UMULH */
                cbnz_x(e, X17, fail);
            }
            mul_reg(e, X16, X16, count);
            cmp_reg(e, TIER_WORK, X16);
            b_cond(e, LO, fail);
            sub_reg(e, TIER_WORK, TIER_WORK, X16);
            break;
        }
        case I_LATCH: { /* again while below its rounds (the outermost count's work each), else past it */
            const TLoop *l = &t->loops[it->loop];
            int out = new_label(e), back = (int)t->merges[l->head].way_count - 1;
            cmp_reg(e, treg(t, it->value), treg(t, l->count));
            if (!tlatch_pays(t, it->loop) && !tier_has_moves(t, l->head, back)) {
                b_cond(e, LO, t->merges[l->head].label);
                tier_moves_as(t, l->exit, it->way, tlatch_k(t, it), it->value);
                break;
            }
            b_cond(e, HS, out);
            if (!tlatch_pays(t, it->loop)) {
            } else if (l->length + 1 < 4096) {
                cmp_imm(e, TIER_WORK, l->length + 1);
                b_cond(e, LO, tfail(t, it->snap));
                sub_imm(e, TIER_WORK, TIER_WORK, l->length + 1);
            } else {
                mov_imm(e, X16, (uint64_t)l->length + 1);
                cmp_reg(e, TIER_WORK, X16);
                b_cond(e, LO, tfail(t, it->snap));
                sub_reg(e, TIER_WORK, TIER_WORK, X16);
            }
            tier_moves(t, l->head, back);
            b(e, t->merges[l->head].label);
            bind(e, out);
            tier_moves_as(t, l->exit, it->way, tlatch_k(t, it), it->value);
            break;
        }
        case I_BREAK: { /* the outermost loop's: what changed written, the generic code breaks it */
            const TSnap *snap = &t->snaps[it->snap];
            tier_write(t, snap->vars);
            tier_records(t, 0);
            tier_fixed(t);
            mov_reg(e, X0, X19);
            call(e, (uintptr_t)pure_run_break);
            k->fresh = 0;
            refresh(k, kept_at(k, l0->parent));
            b(e, l0->after[0]);
            break;
        }
        case I_DONE: { /* the outermost loop ran out: its round the last, the generic code's iteration closes it */
            const TSnap *snap = &t->snaps[it->snap];
            tier_write(t, snap->vars);
            unsigned last = treg(t, it->value);
            sub_imm(e, X9, last, 1);
            size_t off = loop_offset(l0);
            tbase(t, B_LOOPS, X17);
            tbase(t, B_EV, X16);
            if (l0->walk) {
                store64(e, X9, X17, off + offsetof(Loop, walk) + offsetof(Walk, at));
                store64(e, X9, X16, offsetof(Event, row));
            } else
                store64(e, X9, X17, off + offsetof(Loop, k));
            tier_fixed(t);
            k->fresh = 0;
            refresh(k, kept_at(k, t->nest));
            b(e, l0->again[0]);
            break;
        }
        default: break;
        }
    }
    for (size_t i = 0; i < t->probe_count; ++i) {
        const TProbe *p = &t->probes[i];
        tier_probe(t, &t->vals[p->v], p->r, p->key, p->bits, p->hashed, p->fail, p->done);
    }
    for (size_t s = 0; s < t->snap_count; ++s)
        if (t->snaps[s].needed) tier_stub(t, (int)s);
}

static void tier_free(Tier *t) {
    for (size_t i = 0; i < t->val_count; ++i) free(t->vals[i].in);
    for (size_t i = 0; i < t->snap_count; ++i) free(t->snaps[i].vars);
    for (size_t i = 0; i < t->merge_count; ++i) {
        for (unsigned w = 0; w < t->merges[i].way_count; ++w) free(t->merges[i].ways[w]);
        free(t->merges[i].ways);
        free(t->merges[i].phis);
    }
    free(t->vals), free(t->snaps), free(t->merges), free(t->items), free(t->merge_at), free(t->entry), free(t->lists), free(t->live_in), free(t->probes);
}

/* The handler's message input the block is, or -1. */
static int tier_input(const PureModule *m, PureBlock block) {
    for (uint32_t i = 0; i < m->handlers; ++i)
        if (m->handler[i].code.start == block.start && m->handler[i].code.length == block.length && m->handler[i].event) {
            int input = (int)((m->handler[i].event - 1) / 2);
            return m->input[input].kind == PURE_KIND_MESSAGE ? input : -1;
        }
    return -1;
}

/* The tier's values and items on stderr (PURE_TIER_DUMP), to read what it made. */
static void tier_dump(const Tier *t) {
    static const char *ops[] = {"const", "in", "field", "phi", "count", "add", "sub", "mul", "div", "min", "max", "neg", "floor",
                                "lt", "le", "gt", "ge", "eq", "ne", "not", "and", "or", "get", "find_get", "find", "has", "set", "move", "span", "step0", "stride"};
    static const char *items[] = {"value", "merge", "unless", "jump", "open", "latch", "break", "done"};
    for (size_t i = 0; i < t->val_count; ++i) {
        const TVal *v = &t->vals[i];
        fprintf(stderr, "v%zu %s rep=%d a=%d b=%d c=%d d=%d var=%d loop=%d snap=%d [%g,%g] whole=%d reg=%d at=%d last=%d", i, ops[v->op],
                v->rep, v->a, v->b, v->c, v->d, v->var, v->loop, v->snap, v->lo, v->hi, v->whole, v->reg, v->at, v->last);
        if (v->op == T_CONST) fprintf(stderr, " =%g", v->num);
        if (v->op == T_PHI) {
            fprintf(stderr, " in:");
            for (unsigned w = 0; w < v->ins; ++w) fprintf(stderr, " v%d", v->in[w]);
        }
        fprintf(stderr, "\n");
    }
    for (size_t i = 0; i < t->item_count; ++i)
        fprintf(stderr, "i%zu %s value=%d merge=%d loop=%d way=%d snap=%d\n", i, items[t->items[i].kind], t->items[i].value,
                t->items[i].merge, t->items[i].loop, t->items[i].way, t->items[i].snap);
    for (size_t i = 0; i < t->snap_count; ++i) {
        const TSnap *sn = &t->snaps[i];
        fprintf(stderr, "s%zu pc=%u height=%u loop=%d:", i, sn->pc, sn->height, sn->loop);
        for (int var = 0; var < VARS; ++var)
            if (sn->vars[var] >= 0 && (var >= V_STACK || tchanged(t, var, sn->vars[var]))) fprintf(stderr, " %d=v%d", var, sn->vars[var]);
        fprintf(stderr, "\n");
    }
}

/* Whether the nest at li is the tier's: built, its facts, its registers. Its code goes after the block's (tier_finish). */
static Tier *tier_plan(Block *k, unsigned li) {
    Tier *t = calloc(1, sizeof(*t));
    if (!t) return NULL;
    t->k = k, t->nest = (int)li, t->input = tier_input(k->m, k->block);
    const LoopAt *l0 = &k->loops[li];
    t->merge_at = malloc((l0->end - l0->start + 1) * sizeof(int));
    if (!t->merge_at || !tier_nest(t, li)) {
        tier_free(t);
        free(t);
        return NULL;
    }
    for (uint32_t i = 0; i <= l0->end - l0->start; ++i) t->merge_at[i] = -1;
    for (unsigned l = 0; l < k->loop_count; ++l) /* its loops' slots */
        if (!k->loops[l].walk && k->loops[l].at >= l0->at && k->loops[l].end <= l0->end) t->slots |= UINT64_C(1) << (k->m->code[k->loops[l].at].arg & 0x3F);
    tier_liveness(t);
    tier_build(t);
    if (!t->failed && !(t->lists = calloc(PURE_MODULE_STATES, sizeof(*t->lists)))) t->failed = true;
    if (!t->failed) tier_lists(t, t->lists);
    const char *hoist = getenv("PURE_TIER_HOIST");
    if (!t->failed && !(hoist && !strcmp(hoist, "0"))) tier_hoist(t);
    if (!t->failed) tier_reps(t);
    if (!t->failed) tier_defer(t);
    const char *bounds = getenv("PURE_TIER_BOUNDS");
    if (!t->failed && !(bounds && !strcmp(bounds, "0"))) tier_bounds(t);
    const char *steps = getenv("PURE_TIER_STEPS");
    if (!t->failed && !(steps && !strcmp(steps, "0"))) tier_steps(t);
    if (t->failed || !tier_registers(t)) {
        if (getenv("PURE_TIER_DUMP")) {
            fprintf(stderr, "tier: nest %u not taken (%s)\n", li, t->why ? t->why : "built");
            if (!t->failed) tier_dump(t);
        }
        tier_free(t);
        free(t);
        return NULL;
    }
    if (getenv("PURE_TIER_DUMP")) tier_dump(t);
    return t;
}

/* The block's code into e (leaving the stack end_height high), its start's word in *entry. */
static bool compile_block(const PureModule *m, PureBlock block, unsigned end_height, Emit *e, size_t *entry) {
    Block *k = calloc(1, sizeof(*k));
    if (!k) return false;
    k->m = m, k->block = block, k->e = e;
    for (unsigned h = 0; h <= PURE_MODULE_STACK; ++h) k->vs[h] = MEMORY;
    bool ok = shape_block(k);
    Tier *tiers[64];
    unsigned tier_count = 0;
    for (unsigned li = 0; ok && li < k->loop_count && tier_count < 64; ++li) /* the nests the tier runs (docs/design/023) */
        if (k->loops[li].parent < 0 && k->loops[li].start < k->loops[li].end && (tiers[tier_count] = tier_plan(k, li)))
            k->loops[li].tier = new_label(e), ++tier_count;
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
            bind(e, OP_LABEL(k, pc - block.start));
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
        bind(e, OP_LABEL(k, block.length));
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
        for (unsigned li = 0; li < k->loop_count; ++li) /* each nest's integers' code */
            if (k->loops[li].parent < 0 && k->loops[li].start < k->loops[li].end && k->loops[li].tier < 0) compile_ints(k, li);
        for (unsigned i = 0; i < tier_count; ++i) { /* each tier's: where it does not hold, the generic code's round */
            Tier *t = tiers[i];
            const LoopAt *l = &k->loops[t->nest];
            int fallback = new_label(e);
            k->copy = 0;
            tier_emit(t, l->tier, fallback);
            bind(e, fallback);
            tier_fixed(t);
            k->fresh = 0;
            refresh(k, kept_at(k, t->nest));
            b(e, k->labels[0][l->start - k->block.start]);
        }
        fault_stubs(k);
        slow_stubs(k);
        wide_stubs(k);
        has_stubs(k);
        deopt_stubs(k);
    }
    ok = ok && !e->failed;
    for (unsigned i = 0; i < tier_count; ++i) tier_free(tiers[i]), free(tiers[i]);
    free(k->height);
    free(k->loop_of);
    free(k->labels[0]);
    free(k->labels[1]);
    free(k->loops);
    free(k->opens);
    free(k->target);
    free(k->faults);
    free(k->slows);
    free(k->wides);
    free(k->deopts);
    free(k->colds);
    free(k->aheads);
    free(k);
    return ok;
}

/* ---- The module */

typedef struct { PureBlock block; unsigned leaves; size_t entry; } Made;

/* Whether an instruction has code of its own here (else it calls its step). */
static bool in_code(const PureModule *m, const PureOp *op) {
    switch (op->op) {
    case PURE_OP_CONST: case PURE_OP_LOAD: case PURE_OP_STORE: case PURE_OP_CUR_STORE: case PURE_OP_LOCAL_LOAD:
    case PURE_OP_LOCAL_STORE: case PURE_OP_ROW: case PURE_OP_OUTER_ROW: case PURE_OP_INDEX_FIRST: case PURE_OP_INDEX_MOVE:
    case PURE_OP_LIST_COUNT: case PURE_OP_NOT: case PURE_OP_NEG: case PURE_OP_FLOOR: case PURE_OP_ADD: case PURE_OP_SUB:
    case PURE_OP_MUL: case PURE_OP_DIV: case PURE_OP_MIN: case PURE_OP_MAX: case PURE_OP_LT: case PURE_OP_LE: case PURE_OP_GT:
    case PURE_OP_GE: case PURE_OP_EQ: case PURE_OP_NE: case PURE_OP_AND: case PURE_OP_OR: case PURE_OP_JZ: case PURE_OP_JMP:
    case PURE_OP_EACH: case PURE_OP_LOOP: case PURE_OP_BREAK:
        return true;
    case PURE_OP_ITEM_GET: case PURE_OP_LIST_HAS: return !(op->plan & PLAN_TEXT);
    case PURE_OP_LIST_FIND: return op->plan & PLAN_JOINED && !(op->plan & PLAN_TEXT) && !(op[1].plan & PLAN_TEXT) && event_list(m, op->arg);
    case PURE_OP_LIST_APPEND: return event_list(m, op->arg) && numbers_only(m, op->arg) && !m->state_indexed[op->arg];
    default: return false;
    }
}

/* Whether a block is worth code: it has a loop, and its loops' blocks have
 * code for every instruction. Each step a loop calls gives back what the
 * code keeps in registers (read again after it), and costs more than the
 * executor's own dispatch: a loop of steps (TPC-C's lines, 123) runs as
 * fast in the executor. PURE_JIT=all makes every block with a loop (for
 * the checks). */
static bool worth(const PureModule *m, PureBlock block) {
    const char *all = getenv("PURE_JIT");
    bool loops = false, steps = false, tier = false;
    uint32_t end = 0;
    int input = tier_input(m, block);
    for (uint32_t pc = block.start; pc < block.start + block.length; pc = after(&m->code[pc], pc)) {
        const PureOp *op = &m->code[pc];
        if (op->op == PURE_OP_EACH || op->op == PURE_OP_LOOP) {
            loops = true;
            uint32_t ends = pc + 1 + (op->arg >> 16);
            if (pc >= end && tier_supports(m, input, pc, ends, NULL)) tier = true; /* an outermost loop the tier runs (docs/design/023) */
            if (ends > end) end = ends;
        } else if (pc < end && !in_code(m, op))
            steps = true;
    }
    return loops && (!steps || tier || (all && !strcmp(all, "all")));
}

/* A block to make (one worth it), leaving leaves values on the stack. */
static bool want(const PureModule *m, PureBlock block, unsigned leaves, Made **made, size_t *count, size_t *room) {
    if (!worth(m, block)) return true;
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
