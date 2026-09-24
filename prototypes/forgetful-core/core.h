#ifndef PURE_FORGETFUL_CORE_H
#define PURE_FORGETFUL_CORE_H

#include <stddef.h>
#include <stdint.h>

/* An executable contract, not a replacement for the workbench VM. */
enum { PF_NODES = 64, PF_INPUTS = 32, PF_CACHE = 8, PF_HISTORY = 8 };
enum { PF_CONST, PF_INPUT, PF_ADD, PF_MUL, PF_XOR, PF_SELECT };
enum { PF_OK, PF_INVALID, PF_WORK_LIMIT };
enum { PF_FORGET_VALUES = 1, PF_FORGET_HISTORY = 2 };

typedef struct {
    uint32_t op, a, b, c;
    uint64_t literal;
} PfNode;

typedef struct {
    uint64_t value, revision;
    uint32_t dependencies, node_plus_one;
} PfMemo;

typedef struct {
    uint64_t revision;
    uint32_t changed;
} PfChange;

typedef struct {
    uint64_t first;
    uint32_t node, stage, dependencies;
} PfFrame;

/* All persistent arrays and the nonrecursive evaluation stack live here.
 * Callers must treat these fields as private; public exposure allows static
 * allocation and sizeof accounting without a heap or a platform allocator. */
typedef struct {
    PfNode nodes[PF_NODES];
    uint64_t inputs[PF_INPUTS];
    PfMemo memo[PF_CACHE];
    PfChange history[PF_HISTORY];
    PfFrame frames[PF_NODES];
    uint64_t revision, history_floor, work;
    uint32_t node_count, cache_capacity, history_capacity;
    uint32_t next_memo, next_change, depth;
} PfCore;

int pf_init(PfCore *core, uint32_t cache_capacity, uint32_t history_capacity);
/* Load validates every operand before replacement. Inputs survive replacement;
 * cached results and history do not. Caller supplies nonoverlapping nodes. */
int pf_load(PfCore *core, const PfNode *nodes, uint32_t count);
int pf_write(PfCore *core, uint32_t input, uint64_t value);
void pf_forget(PfCore *core, unsigned what);
/* Queries are pure. Only PF_OK publishes *out. A failed query may leave valid
 * intermediate memos, but never changes inputs or publishes a partial result.
 * work counts evaluator stages, memo probes and history probes, not CPU time. */
int pf_eval(PfCore *core, uint32_t node, uint64_t work_limit, uint64_t *out);

#endif
