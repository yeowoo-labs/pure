#include "module-run.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Compiled code (docs/design/021)
 * A module's blocks (its handlers' and views' code, and the folds' blocks
 * in them) as AArch64 code, made when the module loads. A block's code runs
 * a frame as run does: branches and loops are branches and calls, every
 * other instruction a call of the executor's step (module.c) unless it is
 * written here, as the hot ones are. The operand stack stays in the frame;
 * its height at each instruction is known when the module loads (the
 * verifier records it), so a slot is a fixed place in it. */

#if defined(__aarch64__) && defined(__linux__)
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

enum { X0 = 0, X1 = 1, X9 = 9, X10 = 10, X11 = 11, X12 = 12, X13 = 13, X14 = 14, X15 = 15, X16 = 16, X19 = 19, X20 = 20,
       X21 = 21, X22 = 22, X23 = 23, X24 = 24, X29 = 29, X30 = 30, SP = 31, XZR = 31 };
enum { D0 = 0, D1 = 1, D2 = 2, D3 = 3, D4 = 4, D8 = 8 };
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
static void load64_index(Emit *e, unsigned t, unsigned base, unsigned index) { put(e, 0xF8607800 | index << 16 | base << 5 | t); } /* [base, index, LSL #3] */
static void add_imm(Emit *e, unsigned d, unsigned n, uint32_t imm) { put(e, 0x91000000 | imm << 10 | n << 5 | d); } /* imm < 4096 */
static void sub_imm(Emit *e, unsigned d, unsigned n, uint32_t imm) { put(e, 0xD1000000 | imm << 10 | n << 5 | d); }
static void sub_w_imm(Emit *e, unsigned d, unsigned n, uint32_t imm) { put(e, 0x51000000 | imm << 10 | n << 5 | d); }
static void add_reg(Emit *e, unsigned d, unsigned n, unsigned m) { put(e, 0x8B000000 | m << 16 | n << 5 | d); }
static void sub_reg(Emit *e, unsigned d, unsigned n, unsigned m) { put(e, 0xCB000000 | m << 16 | n << 5 | d); }
static void cmp_reg(Emit *e, unsigned n, unsigned m) { put(e, 0xEB00001F | m << 16 | n << 5); }
static void cmp_imm(Emit *e, unsigned n, uint32_t imm) { put(e, 0xF100001F | imm << 10 | n << 5); }
static void csel(Emit *e, unsigned d, unsigned n, unsigned m, unsigned cond) { put(e, 0x9A800000 | m << 16 | cond << 12 | n << 5 | d); }
static void lsr_imm(Emit *e, unsigned d, unsigned n, unsigned shift) { put(e, 0xD340FC00 | shift << 16 | n << 5 | d); }
static void lsr_reg(Emit *e, unsigned d, unsigned n, unsigned m) { put(e, 0x9AC02400 | m << 16 | n << 5 | d); }
static void cset(Emit *e, unsigned d, unsigned cond) { put(e, 0x1A9F07E0 | (cond ^ 1) << 12 | d); }
static void and_w(Emit *e, unsigned d, unsigned n, unsigned m) { put(e, 0x0A000000 | m << 16 | n << 5 | d); }
static void orr_w(Emit *e, unsigned d, unsigned n, unsigned m) { put(e, 0x2A000000 | m << 16 | n << 5 | d); }
static void and_w_one(Emit *e, unsigned d, unsigned n) { put(e, 0x12000000 | n << 5 | d); } /* AND Wd, Wn, #1 */
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
static const uint32_t FMOV_DX = 0x9E670000;
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
static void cmp_w_imm(Emit *e, unsigned n, uint32_t imm) { put(e, 0x7100001F | imm << 10 | n << 5); }

/* ---- A block */

enum { SLOT = sizeof(Slot) };

typedef struct { uint32_t start, end; int parent; int again, after; unsigned depth; uint32_t at; } LoopAt; /* again: its iteration's label; after: past it; depth: the loops open in it; at: its EACH or LOOP */
typedef struct { int label; PureFault kind; uint32_t pc; } FaultAt;

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
    int exit;         /* w0 holds how it went */
    FaultAt *faults;  /* the faults its code may give, each a short stub after it */
    size_t fault_count, fault_room;
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

/* Where a branch at from to target lands: the iteration of the innermost loop around from that ends at target, else target's instruction. */
static int landing(const Block *k, uint32_t from, uint32_t target) {
    for (int l = k->loop_of[from - k->block.start]; l >= 0; l = k->loops[l].parent)
        if (k->loops[l].end == target) return k->loops[l].again;
    return k->op_label[target - k->block.start];
}

/* A loop's block ran once (iterate, module.c, as code): again (its block's
 * first instruction, an empty block's being this), or the loop closed. */
static void iteration(Block *k, const LoopAt *l) {
    Emit *e = k->e;
    const PureOp *op = &k->m->code[l->at];
    int again = l->start == l->end ? l->again : k->op_label[l->start - k->block.start], done = new_label(e);
    load64(e, X9, X19, offsetof(Frame, loops));
    add_imm(e, X9, X9, (uint32_t)((l->depth - 1) * sizeof(Loop)));
    load64(e, X10, X9, offsetof(Loop, mark)); /* the arena as the block began */
    load64(e, X11, X19, offsetof(Frame, arena));
    store64(e, X10, X11, offsetof(PureArena, used));
    if (op->op == PURE_OP_EACH) {
        int counted = new_label(e);
        load8(e, X12, X9, offsetof(Loop, walk) + offsetof(Walk, removed));
        load64(e, X13, X9, offsetof(Loop, walk) + offsetof(Walk, at));
        cbnz_w(e, X12, counted);
        add_imm(e, X13, X13, 1);
        bind(e, counted);
        store8(e, XZR, X9, offsetof(Loop, walk) + offsetof(Walk, removed));
        store64(e, X13, X9, offsetof(Loop, walk) + offsetof(Walk, at));
        load64(e, X14, X9, offsetof(Loop, walk) + offsetof(Walk, end));
        cmp_reg(e, X13, X14);
        b_cond(e, HS, done);
        load64(e, X10, X24, (op->arg >> 8 & 0xFF) * sizeof(PureList *)); /* the list walked: at most its items now */
        load64(e, X10, X10, pure_list_layout[LIST_COUNT_AT]);
        cmp_reg(e, X13, X10);
        b_cond(e, HS, done);
        store64(e, X13, X22, offsetof(Event, row));
        b(e, again);
        bind(e, done);
        load64(e, X10, X9, offsetof(Loop, around));
        store64(e, X10, X22, offsetof(Event, walk));
        load64(e, X10, X9, offsetof(Loop, row));
        store64(e, X10, X22, offsetof(Event, row));
    } else {
        load64(e, X13, X9, offsetof(Loop, k));
        add_imm(e, X13, X13, 1);
        load64(e, X14, X9, offsetof(Loop, count));
        cmp_reg(e, X13, X14);
        b_cond(e, HS, done);
        store64(e, X13, X9, offsetof(Loop, k));
        /* its work counts each time round: its block and the LOOP, past the budget a fault at the LOOP */
        uint64_t cost = (uint64_t)(l->end - l->start) + 1;
        int budgeted = new_label(e);
        load64(e, X10, X23, offsetof(PureApp, work_budget));
        cbnz_x(e, X10, budgeted);
        mov_imm(e, X10, PURE_WORK_DEFAULT);
        bind(e, budgeted);
        load64(e, X11, X23, offsetof(PureApp, work));
        sub_reg(e, X12, X10, X11);
        mov_imm(e, X15, cost);
        cmp_reg(e, X12, X15);
        b_cond(e, LO, fault_at(k, PURE_FAULT_WORK, l->at));
        add_reg(e, X11, X11, X15);
        store64(e, X11, X23, offsetof(PureApp, work));
        load64(e, X10, X9, offsetof(Loop, slot)); /* the slot: low + k */
        if (op->arg & PURE_LOOP_INTEGER) {
            load64(e, X11, X9, offsetof(Loop, low));
            add_reg(e, X11, X11, X13);
            store64(e, X11, X10, 0);
        } else {
            load_d(e, D0, X9, offsetof(Loop, low));
            fp2(e, UCVTF_X, D1, X13);
            fp3(e, FADD, D0, D0, D1);
            store_d(e, D0, X10, 0);
        }
        b(e, again);
        bind(e, done);
    }
    load32(e, X10, X9, offsetof(Loop, outer)); /* closed */
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
            int depth = 0;
            for (int p = k->loops[l].parent; p >= 0; p = k->loops[p].parent) ++depth;
            if (depth > deepest) deepest = depth, pick = (int)l;
        }
        if (pick < 0) break;
        done |= UINT64_C(1) << pick;
        bind(e, k->loops[pick].again);
        iteration(k, &k->loops[pick]);
        bind(e, k->loops[pick].after);
    }
}

static void call_step(Block *k, uintptr_t function, uint32_t pc, unsigned height) {
    Emit *e = k->e;
    set_sp(k, height);
    mov_reg(e, X0, X19);
    mov_w(e, X1, pc);
    call(e, function);
}

/* Slots: a value's 8 bytes at its start, an index's list at owner. */
#define AT(h) ((size_t)(h) * SLOT)
enum { OWNER = offsetof(Slot, bytes) + offsetof(PureBytes, owner) };

static void owner_is(Emit *e, unsigned height, uint32_t owner) {
    mov_w(e, X11, owner & 0xFFFF);
    store_h(e, X11, X20, AT(height) + OWNER);
}

/* The key in d0 as a bit of a list's (in x10) key set when it keeps one as
 * bits: the bit in w15 (0 or 1); to slow when it keeps none or a table. */
static void member_bit(Block *k, int slow, int out) {
    Emit *e = k->e;
    int none = new_label(e), got = new_label(e);
    load64(e, X11, X10, pure_list_layout[LIST_MEMBERS_AT]);
    cbz_x(e, X11, slow);
    load8(e, X12, X10, pure_list_layout[LIST_BITMAP_AT]);
    cbz_w(e, X12, slow);
    fp2(e, FCVTZU_X, X13, D0); /* a whole number from 0 below 2^22 (-0 as 0), else none */
    fp2(e, UCVTF_X, D1, X13);
    fcmp(e, D0, D1);
    b_cond(e, NE, none);
    mov_imm(e, X14, (uint64_t)1 << 22);
    cmp_reg(e, X13, X14);
    b_cond(e, HS, none);
    load64(e, X14, X10, pure_list_layout[LIST_MEMBER_CAPACITY_AT]);
    lsr_imm(e, X15, X13, 6);
    cmp_reg(e, X15, X14);
    b_cond(e, HS, none);
    load64_index(e, X15, X11, X15);
    lsr_reg(e, X15, X15, X13);
    and_w_one(e, X15, X15);
    b(e, got);
    bind(e, none);
    mov_w(e, X15, 0);
    bind(e, got);
    (void)out;
}

/* Two numbers (d0 below d1) to one, a fault past the finite (NON_FINITE). */
static void binary(Block *k, const PureOp *op, uint32_t pc, unsigned height) {
    Emit *e = k->e;
    load_d(e, D0, X20, AT(height - 2));
    load_d(e, D1, X20, AT(height - 1));
    unsigned cond = EQ;
    switch (op->op) {
    case PURE_OP_ADD: fp3(e, FADD, D0, D0, D1); break;
    case PURE_OP_SUB: fp3(e, FSUB, D0, D0, D1); break;
    case PURE_OP_MUL: fp3(e, FMUL, D0, D0, D1); break;
    case PURE_OP_DIV:
        fcmp_zero(e, D1);
        b_cond(e, EQ, fault_at(k, PURE_FAULT_DIVIDE, pc));
        fp3(e, FDIV, D0, D0, D1);
        break;
    case PURE_OP_MIN: fp3(e, FMIN, D0, D0, D1); break;
    case PURE_OP_MAX: fp3(e, FMAX, D0, D0, D1); break;
    case PURE_OP_AND: case PURE_OP_OR:
        fcmp_zero(e, D0);
        cset(e, X9, NE);
        fcmp_zero(e, D1);
        cset(e, X10, NE);
        if (op->op == PURE_OP_AND) and_w(e, X9, X9, X10);
        else orr_w(e, X9, X9, X10);
        fp2(e, UCVTF_W, D0, X9);
        store_d(e, D0, X20, AT(height - 2));
        return;
    default: /* a comparison: a bool */
        cond = op->op == PURE_OP_LT ? MI : op->op == PURE_OP_LE ? LS : op->op == PURE_OP_GT ? GT : op->op == PURE_OP_GE ? GE
             : op->op == PURE_OP_EQ ? EQ : NE;
        fcmp(e, D0, D1);
        cset(e, X9, cond);
        fp2(e, UCVTF_W, D0, X9);
        store_d(e, D0, X20, AT(height - 2));
        return;
    }
    fp2(e, FABS, D2, D0); /* finite: below infinity (a NaN is unordered: HS too) */
    fcmp(e, D2, D8);
    b_cond(e, HS, fault_at(k, PURE_FAULT_NON_FINITE, pc));
    store_d(e, D0, X20, AT(height - 2));
}

static void compile_op(Block *k, uint32_t pc) {
    Emit *e = k->e;
    const PureOp *op = &k->m->code[pc];
    unsigned height = k->height[pc - k->block.start];
    uint32_t arg = op->arg;
    switch (op->op) {
    case PURE_OP_CONST:
        mov_imm(e, X9, (uint64_t)op->integer); /* the bits: a number's, a bool's or an i64's */
        store64(e, X9, X20, AT(height));
        return;
    case PURE_OP_LOAD:
        load64(e, X9, X21, arg * sizeof(PureValue));
        store64(e, X9, X20, AT(height));
        return;
    case PURE_OP_STORE: case PURE_OP_CUR_STORE:
        load64(e, X9, X20, AT(height - 1));
        store64(e, X9, X21, arg * sizeof(PureValue));
        return;
    case PURE_OP_LOCAL_LOAD:
        load64(e, X10, X22, offsetof(Event, locals));
        load64(e, X9, X10, (arg & 0xFF) * sizeof(PureValue));
        store64(e, X9, X20, AT(height));
        owner_is(e, height, arg >> 16);
        return;
    case PURE_OP_LOCAL_STORE:
        load64(e, X9, X20, AT(height - 1));
        load64(e, X10, X22, offsetof(Event, locals));
        store64(e, X9, X10, (arg & 0xFF) * sizeof(PureValue));
        return;
    case PURE_OP_ROW: case PURE_OP_OUTER_ROW:
        load64(e, X9, X22, op->op == PURE_OP_ROW ? offsetof(Event, row) : offsetof(Event, outer));
        fp2(e, UCVTF_X, D0, X9);
        store_d(e, D0, X20, AT(height));
        owner_is(e, height, arg);
        return;
    case PURE_OP_INDEX_FIRST:
        store64(e, XZR, X20, AT(height));
        owner_is(e, height, arg);
        return;
    case PURE_OP_LIST_COUNT:
        load64(e, X10, X24, arg * sizeof(PureList *));
        load64(e, X9, X10, pure_list_layout[LIST_COUNT_AT]);
        fp2(e, UCVTF_X, D0, X9);
        store_d(e, D0, X20, AT(height));
        return;
    case PURE_OP_NOT:
        load_d(e, D0, X20, AT(height - 1));
        fcmp_zero(e, D0);
        cset(e, X9, EQ);
        fp2(e, UCVTF_W, D0, X9);
        store_d(e, D0, X20, AT(height - 1));
        return;
    case PURE_OP_NEG: case PURE_OP_FLOOR:
        load_d(e, D0, X20, AT(height - 1));
        fp2(e, op->op == PURE_OP_NEG ? FNEG : FRINTM, D0, D0);
        store_d(e, D0, X20, AT(height - 1));
        return;
    case PURE_OP_ADD: case PURE_OP_SUB: case PURE_OP_MUL: case PURE_OP_DIV: case PURE_OP_MIN: case PURE_OP_MAX:
    case PURE_OP_LT: case PURE_OP_LE: case PURE_OP_GT: case PURE_OP_GE: case PURE_OP_EQ: case PURE_OP_NE:
    case PURE_OP_AND: case PURE_OP_OR:
        binary(k, op, pc, height);
        return;
    case PURE_OP_INDEX_MOVE: { /* within its list: from 0 to its last item, by a whole number (else LIMIT) */
        load_d(e, D0, X20, AT(height - 2));
        load_d(e, D1, X20, AT(height - 1));
        fp2(e, FRINTM, D2, D1);
        fcmp(e, D1, D2);
        b_cond(e, NE, fault_at(k, PURE_FAULT_LIMIT, pc));
        load16(e, X10, X20, AT(height - 2) + OWNER);
        load64_index(e, X11, X24, X10);
        load64(e, X12, X11, pure_list_layout[LIST_COUNT_AT]);
        sub_imm(e, X13, X12, 1);
        cmp_imm(e, X12, 0);
        csel(e, X13, XZR, X13, EQ);
        fp2(e, UCVTF_X, D3, X13);
        fp3(e, FADD, D0, D0, D1);
        fp3(e, FMIN, D0, D0, D3);
        fp2(e, FMOV_DX, D4, XZR);
        fp3(e, FMAX, D0, D4, D0);
        store_d(e, D0, X20, AT(height - 2));
        return;
    }
    case PURE_OP_LIST_HAS: {
        if (op->plan & PLAN_TEXT) break;
        int slow = new_label(e), done = new_label(e);
        load64(e, X10, X24, arg * sizeof(PureList *));
        load_d(e, D0, X20, AT(height - 1));
        member_bit(k, slow, done);
        fp2(e, UCVTF_W, D0, X15);
        store_d(e, D0, X20, AT(height - 1));
        b(e, done);
        bind(e, slow);
        call_step(k, (uintptr_t)pure_run_step, pc, height);
        cbnz_w(e, X0, k->exit);
        bind(e, done);
        return;
    }
    case PURE_OP_LIST_FIND: { /* joined to its ITEM_GET: a DENSE list's field at its key */
        const PureOp *get = &k->m->code[pc + 1];
        if (!(op->plan & PLAN_JOINED) || op->plan & PLAN_TEXT || get->plan & PLAN_TEXT) break;
        int slow = new_label(e), done = new_label(e);
        load64(e, X10, X24, arg * sizeof(PureList *));
        load8(e, X12, X10, pure_list_layout[LIST_FORM_AT]);
        cmp_w_imm(e, X12, 2); /* DENSE */
        b_cond(e, NE, slow);
        load_d(e, D0, X20, AT(height - 1));
        member_bit(k, slow, done);
        cbz_w(e, X15, fault_at(k, PURE_FAULT_NOT_FOUND, pc));
        load64(e, X14, X10, pure_list_layout[LIST_COLUMNS_AT] + item_field(get->arg) * sizeof(void *));
        load64_index(e, X9, X14, X13);
        store64(e, X9, X20, AT(height - 1));
        b(e, done);
        bind(e, slow);
        call_step(k, (uintptr_t)pure_run_step, pc, height);
        cbnz_w(e, X0, k->exit);
        bind(e, done);
        return;
    }
    case PURE_OP_ITEM_GET: { /* a FLAT list's cell by its column, a tree's from the run kept */
        if (op->plan & PLAN_TEXT) break;
        int slow = new_label(e), done = new_label(e), flat = new_label(e), refill = new_label(e);
        size_t field = item_field(arg) * sizeof(void *);
        load64(e, X10, X24, list_of(arg) * sizeof(PureList *));
        load8(e, X12, X10, pure_list_layout[LIST_FORM_AT]);
        cmp_w_imm(e, X12, 1); /* FLAT */
        b_cond(e, EQ, flat);
        cbnz_w(e, X12, slow); /* DENSE */
        load_d(e, D0, X20, AT(height - 1));
        fp2(e, FCVTZU_X, X13, D0);
        load64(e, X11, X19, offsetof(Frame, run.list));
        cmp_reg(e, X11, X10);
        b_cond(e, NE, refill);
        load64(e, X11, X19, offsetof(Frame, run.version));
        load64(e, X12, X10, pure_list_layout[LIST_VERSION_AT]);
        cmp_reg(e, X11, X12);
        b_cond(e, NE, refill);
        load64(e, X11, X19, offsetof(Frame, run.start));
        sub_reg(e, X14, X13, X11);
        load64(e, X11, X19, offsetof(Frame, run.count));
        cmp_reg(e, X14, X11);
        b_cond(e, HS, refill);
        load64(e, X15, X19, offsetof(Frame, run.columns) + field);
        load64_index(e, X9, X15, X14);
        store64(e, X9, X20, AT(height - 1));
        b(e, done);
        bind(e, refill);
        call_step(k, (uintptr_t)pure_run_item, pc, height);
        cbnz_w(e, X0, k->exit);
        b(e, done);
        bind(e, flat);
        load_d(e, D0, X20, AT(height - 1));
        fp2(e, FCVTZU_X, X13, D0);
        load64(e, X12, X10, pure_list_layout[LIST_COUNT_AT]);
        cmp_reg(e, X13, X12);
        b_cond(e, HS, fault_at(k, PURE_FAULT_NOT_FOUND, pc));
        load64(e, X14, X10, pure_list_layout[LIST_COLUMNS_AT] + item_field(arg) * sizeof(void *));
        load64_index(e, X9, X14, X13);
        store64(e, X9, X20, AT(height - 1));
        b(e, done);
        bind(e, slow);
        call_step(k, (uintptr_t)pure_run_step, pc, height);
        cbnz_w(e, X0, k->exit);
        bind(e, done);
        return;
    }
    case PURE_OP_JZ:
        load_d(e, 0, X20, (height - 1) * SLOT);
        put(e, 0x1E602008); /* FCMP d0, #0.0 */
        b_cond(e, EQ, landing(k, pc, op->arg));
        return;
    case PURE_OP_JMP:
        b(e, landing(k, pc, op->arg));
        return;
    case PURE_OP_EACH: case PURE_OP_LOOP: {
        call_step(k, op->op == PURE_OP_EACH ? (uintptr_t)pure_run_each : (uintptr_t)pure_run_loop, pc, height);
        int open = new_label(e);
        cbz_w(e, X0, open);
        cmp_w_imm(e, X0, STEP_SKIP);
        b_cond(e, NE, k->exit);
        b(e, k->loops[k->opens[pc - k->block.start]].after); /* none: past its block, where its iteration would end */
        bind(e, open);
        return;
    }
    case PURE_OP_BREAK: {
        int l = k->loop_of[pc - k->block.start];
        mov_reg(e, X0, X19);
        call(e, (uintptr_t)pure_run_break);
        b(e, k->loops[l].after);
        return;
    }
    default: break;
    }
    call_step(k, (uintptr_t)pure_run_step, pc, height);
    cbnz_w(e, X0, k->exit);
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
    if (!k->height || !k->loop_of || !k->op_label || !k->loops || !k->opens) return false;
    int open[PURE_MODULE_NESTING + 1];
    unsigned base[PURE_MODULE_NESTING + 1], depth = 0;
    base[0] = 0;
    for (uint32_t pc = k->block.start; pc < k->block.start + n;) {
        while (depth && pc >= k->loops[open[depth - 1]].end) --depth;
        const PureOp *op = &m->code[pc];
        unsigned at = base[depth] + m->height[pc];
        k->height[pc - k->block.start] = (uint8_t)at;
        k->loop_of[pc - k->block.start] = depth ? open[depth - 1] : -1;
        if (op->op == PURE_OP_EACH || op->op == PURE_OP_LOOP) {
            unsigned pops = op->op == PURE_OP_LOOP || op->arg & PURE_EACH_SLICED ? 2 : 0;
            LoopAt *l = &k->loops[k->loop_count];
            *l = (LoopAt){pc + 1, pc + 1 + (op->arg >> 16), depth ? open[depth - 1] : -1, new_label(k->e), new_label(k->e), depth + 1,
                          pc};
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
    for (uint32_t i = 0; i <= n; ++i) k->op_label[i] = new_label(k->e);
    k->exit = new_label(k->e);
    return !k->e->failed;
}

/* The block's code into e (leaving the stack end_height high), its start's word in *entry. */
static bool compile_block(const PureModule *m, PureBlock block, unsigned end_height, Emit *e, size_t *entry) {
    Block k = {.m = m, .block = block, .e = e};
    bool ok = shape_block(&k);
    if (ok) {
        *entry = e->count;
        stp(e, X29, X30, -80, true);
        put(e, 0x910003FD);         /* MOV x29, sp */
        stp(e, X19, X20, 16, false);
        stp(e, X21, X22, 32, false);
        stp(e, X23, X24, 48, false);
        put(e, 0xFD000000 | (64 / 8) << 10 | SP << 5 | D8); /* STR d8, [sp, #64] */
        mov_reg(e, X19, X0);
        load64(e, X20, X19, offsetof(Frame, stack));
        load64(e, X21, X19, offsetof(Frame, values));
        load64(e, X22, X19, offsetof(Frame, ev));
        load64(e, X23, X19, offsetof(Frame, app));
        mov_imm(e, X24, offsetof(PureApp, lists));
        add_reg(e, X24, X23, X24);        /* the app's lists */
        mov_imm(e, X9, UINT64_C(0x7FF0000000000000));
        fp2(e, FMOV_DX, D8, X9);          /* infinity */
        for (uint32_t pc = block.start; pc < block.start + block.length;) {
            loop_ends(&k, pc);
            bind(e, k.op_label[pc - block.start]);
            compile_op(&k, pc);
            const PureOp *op = &m->code[pc];
            pc = op->op == PURE_OP_EACH || op->op == PURE_OP_LOOP ? pc + 1 : after(op, pc);
        }
        uint32_t end = block.start + block.length;
        loop_ends(&k, end);
        bind(e, k.op_label[block.length]);
        set_sp(&k, end_height); /* the block ran: what it leaves (a view's value) is the frame's to give */
        mov_w(e, X0, STEP_NEXT);
        bind(e, k.exit);
        put(e, 0xFD400000 | (64 / 8) << 10 | SP << 5 | D8); /* LDR d8, [sp, #64] */
        ldp(e, X23, X24, 48, false);
        ldp(e, X21, X22, 32, false);
        ldp(e, X19, X20, 16, false);
        ldp(e, X29, X30, 80, true);
        put(e, 0xD65F03C0);         /* RET */
        fault_stubs(&k);
    }
    free(k.height);
    free(k.loop_of);
    free(k.op_label);
    free(k.loops);
    free(k.opens);
    free(k.faults);
    return ok && !e->failed;
}

/* ---- The module */

typedef struct { PureBlock block; unsigned leaves; size_t entry; } Made;

/* A block to make, leaving leaves values on the stack. */
static bool want(PureBlock block, unsigned leaves, Made **made, size_t *count, size_t *room) {
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
    for (uint32_t h = 0; ok && h < m->handlers; ++h) ok = want(m->handler[h].code, 0, &made, &count, &room);
    for (uint32_t v = 0; ok && v < m->views; ++v)
        if (m->view_code[v].length) ok = want(m->view_code[v], 1, &made, &count, &room);
    for (size_t i = 0; ok && i < count; ++i)
        for (uint32_t pc = made[i].block.start; ok && pc < made[i].block.start + made[i].block.length; ++pc) {
            const PureOp *op = &m->code[pc];
            unsigned how = op->arg & PURE_FOLD_KIND; /* count and first leave a bool; the others a value and a bool */
            if (op->op == PURE_OP_FOLD)
                ok = want((PureBlock){pc + 1, op->arg >> 16}, how == PURE_FOLD_COUNT || how == PURE_FOLD_FIRST ? 1 : 2, &made, &count,
                          &room);
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
