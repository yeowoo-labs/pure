#include "core.h"

#include <string.h>

int pf_init(PfCore *core, uint32_t cache_capacity, uint32_t history_capacity) {
    if (!core || cache_capacity > PF_CACHE || history_capacity > PF_HISTORY)
        return PF_INVALID;
    memset(core, 0, sizeof(*core));
    core->cache_capacity = cache_capacity;
    core->history_capacity = history_capacity;
    core->revision = 1;
    core->history_floor = 1;
    return PF_OK;
}

void pf_forget(PfCore *core, unsigned what) {
    if (what & PF_FORGET_VALUES) {
        memset(core->memo, 0, sizeof(core->memo));
        core->next_memo = 0;
    }
    if (what & PF_FORGET_HISTORY) {
        memset(core->history, 0, sizeof(core->history));
        core->next_change = 0;
        /* Records older than this floor can no longer justify reuse. */
        core->history_floor = core->revision;
    }
}

int pf_load(PfCore *core, const PfNode *nodes, uint32_t count) {
    if (!nodes || !count || count > PF_NODES) return PF_INVALID;
    for (uint32_t i = 0; i < count; ++i) {
        const PfNode n = nodes[i];
        if (n.op > PF_SELECT) return PF_INVALID;
        if (n.op == PF_INPUT && n.a >= PF_INPUTS) return PF_INVALID;
        if (n.op >= PF_ADD && (n.a >= i || n.b >= i)) return PF_INVALID;
        if (n.op == PF_SELECT && n.c >= i) return PF_INVALID;
    }
    memcpy(core->nodes, nodes, count * sizeof(*nodes));
    core->node_count = count;
    pf_forget(core, PF_FORGET_VALUES | PF_FORGET_HISTORY);
    return PF_OK;
}

int pf_write(PfCore *core, uint32_t input, uint64_t value) {
    if (input >= PF_INPUTS) return PF_INVALID;
    if (core->inputs[input] == value) return PF_OK;
    /* No old certificate is allowed to become current after epoch wrap. */
    if (core->revision == UINT64_MAX) {
        core->revision = 1;
        pf_forget(core, PF_FORGET_VALUES | PF_FORGET_HISTORY);
    }
    ++core->revision;
    core->inputs[input] = value;
    if (!core->history_capacity) {
        core->history_floor = core->revision;
    } else {
        PfChange *change = &core->history[core->next_change];
        if (change->revision > core->history_floor)
            core->history_floor = change->revision;
        *change = (PfChange){core->revision, UINT32_C(1) << input};
        core->next_change = (core->next_change + 1) % core->history_capacity;
    }
    return PF_OK;
}

static int step(PfCore *core, uint64_t limit) {
    if (core->work == limit) return 0;
    ++core->work;
    return 1;
}

/* Return 1 for a proven hit, 0 for a miss, -1 for work exhaustion. No reverse
 * dependency edges or version tables grow when nodes are queried. */
static int lookup(PfCore *core, uint32_t node, uint64_t limit, PfMemo *found) {
    for (uint32_t i = 0; i < core->cache_capacity; ++i) {
        if (!step(core, limit)) return -1;
        const PfMemo memo = core->memo[i];
        if (memo.node_plus_one != node + 1) continue;
        if (memo.revision < core->history_floor) return 0;
        if (memo.revision != core->revision) {
            for (uint32_t j = 0; j < core->history_capacity; ++j) {
                if (!step(core, limit)) return -1;
                const PfChange change = core->history[j];
                if (change.revision > memo.revision &&
                    (change.changed & memo.dependencies)) return 0;
            }
        }
        *found = memo;
        /* Validation can extend the certificate without evaluating the node. */
        core->memo[i].revision = core->revision;
        return 1;
    }
    return 0;
}

static void remember(PfCore *core, uint32_t node, uint64_t value, uint32_t deps) {
    if (!core->cache_capacity) return;
    /* Round-robin keeps replacement state fixed. Duplicates are safe: every
     * entry has its own certificate; the first matching entry may cause extra
     * recomputation but cannot authorize a stale value. */
    core->memo[core->next_memo] = (PfMemo){value, core->revision, deps, node + 1};
    core->next_memo = (core->next_memo + 1) % core->cache_capacity;
}

static void push(PfCore *core, uint32_t node) {
    core->frames[core->depth++] = (PfFrame){.node = node};
}

int pf_eval(PfCore *core, uint32_t node, uint64_t work_limit, uint64_t *out) {
    core->work = 0;
    core->depth = 0;
    if (!out || node >= core->node_count) return PF_INVALID;
    push(core, node);
    uint64_t value = 0;
    uint32_t deps = 0;
    while (core->depth) {
        if (!step(core, work_limit)) goto exhausted;
        PfFrame *frame = &core->frames[core->depth - 1];
        const PfNode n = core->nodes[frame->node];
        if (frame->stage == 0) {
            PfMemo memo;
            const int hit = lookup(core, frame->node, work_limit, &memo);
            if (hit < 0) goto exhausted;
            if (hit) {
                value = memo.value;
                deps = memo.dependencies;
                --core->depth;
                continue;
            }
            if (n.op == PF_CONST) {
                value = n.literal;
                deps = 0;
            } else if (n.op == PF_INPUT) {
                value = core->inputs[n.a];
                deps = UINT32_C(1) << n.a;
            } else {
                frame->stage = 1;
                push(core, n.a);
                continue;
            }
        } else if (frame->stage == 1) {
            frame->first = value;
            frame->dependencies = deps;
            frame->stage = 2;
            push(core, n.op == PF_SELECT && !value ? n.c : n.b);
            continue;
        } else {
            deps |= frame->dependencies;
            if (n.op == PF_ADD) value = frame->first + value;
            else if (n.op == PF_MUL) value = frame->first * value;
            else if (n.op == PF_XOR) value = frame->first ^ value;
            /* SELECT preserves the chosen result and includes control deps. */
        }
        remember(core, frame->node, value, deps);
        --core->depth;
    }
    *out = value;
    return PF_OK;

exhausted:
    core->depth = 0;
    return PF_WORK_LIMIT;
}
