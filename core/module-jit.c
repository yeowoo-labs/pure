#include "module-run.h"
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

typedef struct { size_t at; int label; unsigned kind; } Fixup; /* kind: 0 B/BL, 1 B.cond, CBZ/CBNZ */
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
        } else {
            if (offset < -(1L << 18) || offset >= (1L << 18)) return false;
            e->words[f.at] |= ((uint32_t)offset & 0x7FFFF) << 5;
        }
    }
    return !e->failed;
}

enum { X0 = 0, X1 = 1, X9 = 9, X10 = 10, X16 = 16, X19 = 19, X20 = 20, X21 = 21, X22 = 22, X29 = 29, X30 = 30, SP = 31, XZR = 31 };
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

static __attribute__((unused)) void load_d(Emit *e, unsigned t, unsigned base, size_t offset) { put(e, 0xFD400000 | (uint32_t)(offset / 8) << 10 | base << 5 | t); }
static __attribute__((unused)) void store_d(Emit *e, unsigned t, unsigned base, size_t offset) { put(e, 0xFD000000 | (uint32_t)(offset / 8) << 10 | base << 5 | t); }
static __attribute__((unused)) void store_h(Emit *e, unsigned t, unsigned base, size_t offset) { put(e, 0x79000000 | (uint32_t)(offset / 2) << 10 | base << 5 | t); }
static __attribute__((unused)) void store_w(Emit *e, unsigned t, unsigned base, size_t offset) { put(e, 0xB9000000 | (uint32_t)(offset / 4) << 10 | base << 5 | t); }
static void mov_reg(Emit *e, unsigned d, unsigned n) { put(e, 0xAA0003E0 | n << 16 | d); }
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
static void cmp_w_imm(Emit *e, unsigned n, uint32_t imm) { put(e, 0x7100001F | imm << 10 | n << 5); }

/* ---- A block */

enum { SLOT = sizeof(Slot) };

typedef struct { uint32_t start, end; int parent; int again, after; } LoopAt; /* again: its iteration's label; after: past it */

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
} Block;

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
        mov_reg(e, X0, X19);
        call(e, (uintptr_t)pure_run_iterate);
        /* again: its block's first instruction; an empty block's is this */
        const LoopAt *l = &k->loops[pick];
        cbz_w(e, X0, l->start == l->end ? l->again : k->op_label[l->start - k->block.start]);
        cmp_w_imm(e, X0, STEP_DONE);
        b_cond(e, NE, k->exit);
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

static void compile_op(Block *k, uint32_t pc) {
    Emit *e = k->e;
    const PureOp *op = &k->m->code[pc];
    unsigned height = k->height[pc - k->block.start];
    switch (op->op) {
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
    default:
        call_step(k, (uintptr_t)pure_run_step, pc, height);
        cbnz_w(e, X0, k->exit);
        return;
    }
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
            *l = (LoopAt){pc + 1, pc + 1 + (op->arg >> 16), depth ? open[depth - 1] : -1, new_label(k->e), new_label(k->e)};
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
        put(e, 0xA9BC7BFD);         /* STP x29, x30, [sp, #-64]! */
        put(e, 0x910003FD);         /* MOV x29, sp */
        put(e, 0xA90153F3);         /* STP x19, x20, [sp, #16] */
        put(e, 0xA9025BF5);         /* STP x21, x22, [sp, #32] */
        mov_reg(e, X19, X0);
        load64(e, X20, X19, offsetof(Frame, stack));
        load64(e, X21, X19, offsetof(Frame, values));
        load64(e, X22, X19, offsetof(Frame, ev));
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
        put(e, 0xA94153F3);         /* LDP x19, x20, [sp, #16] */
        put(e, 0xA9425BF5);         /* LDP x21, x22, [sp, #32] */
        put(e, 0xA8C47BFD);         /* LDP x29, x30, [sp], #64 */
        put(e, 0xD65F03C0);         /* RET */
    }
    free(k.height);
    free(k.loop_of);
    free(k.op_label);
    free(k.loops);
    free(k.opens);
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
