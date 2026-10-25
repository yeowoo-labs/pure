/* Research 049: a loaded module's code as the load leaves it (its plans set, its anchors the runtime's instructions),
 * for emit.py: every instruction's op, argument, plan, constant bits and the operand stack's height before it (the
 * verifier's), and the blocks: handlers, functions, views. One JSON object.
 *
 *   cc -O2 -std=c17 -I../../../core dump.c ../../../build/libpure.a -lm -pthread -o dump
 *   ./dump MODULE */
#include "module.h"
#include "module-run.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: %s MODULE\n", argv[0]); return 2; }
    PureModule m;
    memset(&m, 0, sizeof(m));
    char error[256];
    if (pure_module_read(argv[1], &m, error, sizeof(error)) != PURE_OK) { fprintf(stderr, "%s\n", error); return 1; }
    printf("{\"code_count\":%u,\"states\":%u,\"runner\":%u,\"handlers\":[", m.code_count, m.states, m.runner);
    for (uint32_t h = 0; h < m.handlers; ++h)
        printf("%s{\"event\":%u,\"start\":%u,\"length\":%u}", h ? "," : "", m.handler[h].event, m.handler[h].code.start,
               m.handler[h].code.length);
    printf("],\"functions\":[");
    for (uint32_t f = 0; f < m.functions; ++f)
        printf("%s{\"start\":%u,\"length\":%u,\"params\":%u,\"results\":%u}", f ? "," : "", m.function[f].code.start,
               m.function[f].code.length, m.function[f].params, m.function[f].results);
    printf("],\"state_types\":[");
    for (uint32_t s = 0; s < m.states; ++s) printf("%s%u", s ? "," : "", m.state_type[s]);
    printf("],\"code\":[");
    for (uint32_t pc = 0; pc < m.code_count; ++pc) {
        const PureOp *op = &m.code[pc];
        printf("%s[%u,%u,%u,\"%" PRId64 "\",%u,%u,%u]", pc ? "," : "", op->op, op->arg, op->plan, op->integer, m.height[pc], op->shape,
               op->range);
    }
    printf("]}\n");
    pure_module_release(&m);
    return 0;
}
