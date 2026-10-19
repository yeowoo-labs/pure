/* Research 045 (029's principle 1, experiment (b)): the executor's own instruction bodies stitched without dispatch.
 * Reads a module as Pure loads it and writes C for Pure's core to include (native.inc): the module's code as a
 * constant array; a function for each basic block calling the executor's step() on each of its constant instructions,
 * so the C compiler specializes each body to its instruction alone (no fetch, no decode, no dispatch within a block);
 * and run_native, run_calls's loop dispatching once a block. Calls, returns and the ends of loops go through
 * run_calls's own code. The semantics are the executor's source, unchanged.
 *
 *   cc -O2 -std=c17 -I../../../core stitch.c ../../../build/libpure.a -lm -pthread -o stitch
 *   ./stitch MODULE > native.inc     (then core/module.c built with -DPURE_NATIVE_INC='"native.inc"') */
#include "module.h"
#include <inttypes.h>
#include <stdio.h>

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: %s MODULE\n", argv[0]); return 2; }
    PureModule m = {0};
    char error[160];
    if (pure_module_read(argv[1], &m, error, sizeof(error)) != PURE_OK) { fprintf(stderr, "%s\n", error); return 1; }
    uint32_t n = m.code_count;
    printf("/* Stitched from %s: %u instructions (research 045). */\n", argv[1], n);
    printf("enum { NATIVE_COUNT = %u, STEP_ENDED = 9 }; /* STEP_ENDED: a block reached its end (frame.end) */\n", n);
    printf("static const PureOp native_code[NATIVE_COUNT] = {\n");
    for (uint32_t pc = 0; pc < n; ++pc) {
        const PureOp *o = &m.code[pc];
        printf("    {.integer = INT64_C(%" PRId64 "), .arg = %" PRIu32 "u, .op = %u, .plan = %u, .shape = %u, .range = %u},\n",
               o->integer, o->arg, o->op, o->plan, o->shape, o->range);
    }
    printf("};\n\n");
    printf("static bool native_matches(const PureModule *m) {\n"
           "    if (m->code_count != NATIVE_COUNT) return false;\n"
           "    for (uint32_t pc = 0; pc < NATIVE_COUNT; ++pc)\n"
           "        if (m->code[pc].op != native_code[pc].op || m->code[pc].arg != native_code[pc].arg ||\n"
           "            m->code[pc].integer != native_code[pc].integer || m->code[pc].plan != native_code[pc].plan)\n"
           "            return false;\n"
           "    return true;\n"
           "}\n\n");
    /* Basic blocks: a leader at each block's and function's start, each jump's target, past each instruction that may
     * go elsewhere (a branch, a loop's, a call, a return), and every 16 instructions. Each is a function of its own (one
     * enormous function took the compiler past 3 GiB): stitched within, dispatched once a block. */
    static bool leader[1 << 16];
    for (uint32_t h = 0; h < m.handlers; ++h) leader[m.handler[h].code.start] = true;
    for (uint32_t f = 0; f < m.functions; ++f) leader[m.function[f].code.start] = true;
    for (uint32_t pc = 0; pc < n; ++pc) {
        uint8_t op = m.code[pc].op;
        bool control = op == PURE_OP_JZ || op == PURE_OP_JMP || op == PURE_OP_LOOP || op == PURE_OP_EACH || op == PURE_OP_BREAK ||
                       op == PURE_OP_CALL || op == PURE_OP_RETURN || op == PURE_OP_FOLD;
        if ((op == PURE_OP_JZ || op == PURE_OP_JMP) && m.code[pc].arg < n) leader[m.code[pc].arg] = true;
        if (control && pc + 1 < n) leader[pc + 1] = true;
    }
    leader[0] = true;
    for (uint32_t pc = 0, run = 0; pc < n; ++pc, ++run) {
        if (leader[pc]) run = 0;
        else if (run == 16) leader[pc] = true, run = 0;
    }
    for (uint32_t pc = 0; pc < n;) {
        uint32_t end = pc + 1;
        while (end < n && !leader[end]) ++end;
        printf("static __attribute__((noinline)) int native_%u(Frame *frame, uint32_t *pc) {\n    int how;\n", pc);
        for (uint32_t k = pc; k < end; ++k) {
            printf("    if (%uu >= frame->end) { *pc = %uu; return STEP_ENDED; }\n", k, k);
            printf("    if ((how = step(frame, &native_code[%u], %uu, pc, true)) != STEP_NEXT || *pc != %uu) return how;\n", k, k, k + 1);
        }
        printf("    return STEP_NEXT;\n}\n");
        pc = end;
    }
    printf("typedef int (*NativeBlock)(Frame *, uint32_t *);\n");
    printf("static const NativeBlock native_blocks[NATIVE_COUNT] = {\n");
    for (uint32_t pc = 0; pc < n; ++pc) if (leader[pc]) printf("    [%u] = native_%u,\n", pc, pc);
    printf("};\n\n");
    printf("static __attribute__((noinline)) PureResult run_native(const PureModule *m, PureValue *values, PureBlock block, Event *ev,\n"
           "                                                     Slot *result) {\n"
           "    Slot stack[PURE_MODULE_STACK];\n"
           "    Loop few[PURE_MODULE_NESTING], *loops = m->nesting > PURE_MODULE_NESTING ? deep_loops(ev->app, m->nesting) : few;\n"
           "    if (!loops) return PURE_ALLOCATION;\n"
           "    Frame frame;\n"
           "    open_frame(&frame, m, values, block, ev, stack, loops);\n"
           "    Call *top = NULL;\n"
           "    int how = STEP_NEXT;\n"
           "    for (uint32_t pc = block.start;;) {\n"
           "        if (pc >= frame.end) {\n"
           "            if (!frame.depth || (how = iterate(&frame, &pc)) != STEP_NEXT) break;\n"
           "            continue;\n"
           "        }\n"
           "        NativeBlock b = pc < NATIVE_COUNT ? native_blocks[pc] : NULL;\n"
           "        how = b ? b(&frame, &pc) : step(&frame, &m->code[pc], pc, &pc, true);\n"
           "        if (how == STEP_NEXT || how == STEP_ENDED) continue;\n"
           "        if (how != STEP_CALL && how != STEP_BACK && how != STEP_BATCH) break;\n"
           "        Frame apart = frame;\n"
           "        if (how == STEP_CALL) how = enter_call(&apart, &top, &pc);\n"
           "        else if (how == STEP_BACK) how = leave_call(&apart, &top, &pc);\n"
           "        else {\n"
           "            uint32_t past = 0;\n"
           "            how = open_batches(&apart, pc - 1, &past);\n"
           "            pc = past;\n"
           "        }\n"
           "        frame = apart;\n"
           "        if (how != STEP_NEXT) break;\n"
           "    }\n"
           "    if (top) {\n"
           "        Frame apart = frame;\n"
           "        unwind_calls(&apart, top);\n"
           "        frame = apart;\n"
           "    }\n"
           "    return finish(&frame, how, result);\n"
           "}\n");
    pure_module_release(&m);
    return 0;
}
