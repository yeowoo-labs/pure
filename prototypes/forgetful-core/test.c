#include "core.h"

#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static uint64_t checks, queries, exhausted_queries;
static uint64_t rng = UINT64_C(20260922);

static uint64_t random_word(void) {
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return rng;
}

/* Independent forward evaluator: no memo, history, stack machine, or demand
 * scheduling. All nodes are total/pure, so eager SELECT arms are safe here. */
static uint64_t reference(const PfNode *nodes, uint32_t count,
                          const uint64_t *inputs, uint32_t target) {
    uint64_t values[PF_NODES] = {0};
    for (uint32_t i = 0; i < count; ++i) {
        const PfNode n = nodes[i];
        switch (n.op) {
            case PF_CONST: values[i] = n.literal; break;
            case PF_INPUT: values[i] = inputs[n.a]; break;
            case PF_ADD: values[i] = values[n.a] + values[n.b]; break;
            case PF_MUL: values[i] = values[n.a] * values[n.b]; break;
            case PF_XOR: values[i] = values[n.a] ^ values[n.b]; break;
            case PF_SELECT: values[i] = values[n.a] ? values[n.b] : values[n.c]; break;
            default: assert(0);
        }
    }
    return values[target];
}

static void check_query(PfCore *core, const PfNode *nodes, uint32_t count,
                        const uint64_t *inputs, uint32_t target, uint64_t limit) {
    const uint64_t sentinel = UINT64_C(0xbaadc0de12345678);
    uint64_t out = sentinel;
    const int status = pf_eval(core, target, limit, &out);
    assert(core->work <= limit && core->depth == 0);
    assert(memcmp(core->inputs, inputs, sizeof(core->inputs)) == 0);
    assert(status == PF_OK || status == PF_WORK_LIMIT);
    if (status == PF_OK) {
        assert(out == reference(nodes, count, inputs, target));
        ++queries;
    } else {
        assert(out == sentinel);
        ++exhausted_queries;
        /* Completion is also checked after the interrupted query's memos. */
        assert(pf_eval(core, target, UINT64_C(10000000), &out) == PF_OK);
        assert(out == reference(nodes, count, inputs, target));
        ++queries;
    }
    ++checks;
}

static const PfNode branch_program[] = {
    {.op = PF_INPUT, .a = 0},
    {.op = PF_INPUT, .a = 1},
    {.op = PF_CONST, .literal = 7},
    {.op = PF_ADD, .a = 1, .b = 2},
    {.op = PF_XOR, .a = 1, .b = 2},
    {.op = PF_SELECT, .a = 0, .b = 3, .c = 4},
    {.op = PF_MUL, .a = 5, .b = 3},
    {.op = PF_SELECT, .a = 0, .b = 6, .c = 4},
};

static void walk(const PfCore *parent, const uint64_t *inputs, unsigned depth) {
    for (unsigned action = 0; action < 6; ++action) {
        PfCore core = *parent;
        uint64_t next[PF_INPUTS];
        memcpy(next, inputs, sizeof(next));
        if (action < 2) {
            next[action] ^= 1;
            assert(pf_write(&core, action, next[action]) == PF_OK);
        } else if (action < 5) {
            pf_forget(&core, action - 1);
        } else {
            uint64_t untouched = 123;
            assert(pf_eval(&core, 7, 0, &untouched) == PF_WORK_LIMIT);
            assert(untouched == 123);
            ++exhausted_queries;
        }
        check_query(&core, branch_program, 8, next, 7, 10000);
        if (depth > 1) walk(&core, next, depth - 1);
    }
}

static void exhaustive(void) {
    const uint32_t capacities[] = {0, 1, 8};
    for (unsigned c = 0; c < 3; ++c) {
        for (unsigned h = 0; h < 3; ++h) {
            PfCore core;
            uint64_t inputs[PF_INPUTS] = {0};
            assert(pf_init(&core, capacities[c], capacities[h]) == PF_OK);
            assert(pf_load(&core, branch_program, 8) == PF_OK);
            walk(&core, inputs, 5);
        }
    }
}

static void directed(void) {
    PfCore core, saved;
    uint64_t out, inputs[PF_INPUTS] = {0};
    assert(pf_init(&core, 8, 8) == PF_OK);
    assert(pf_load(&core, branch_program, 8) == PF_OK);
    saved = core;
    assert(pf_init(&core, 9, 8) == PF_INVALID);
    assert(memcmp(&core, &saved, sizeof(core)) == 0);
    assert(pf_load(&core, NULL, 0) == PF_INVALID);
    const PfNode invalid[] = {{.op = PF_ADD, .a = 0, .b = 0}};
    assert(pf_load(&core, invalid, 1) == PF_INVALID);
    assert(memcmp(&core, &saved, sizeof(core)) == 0);
    assert(pf_write(&core, 32, 1) == PF_INVALID);
    assert(memcmp(&core, &saved, sizeof(core)) == 0);
    out = 456;
    assert(pf_eval(&core, 8, 100, &out) == PF_INVALID && out == 456);

    check_query(&core, branch_program, 8, inputs, 7, 10000);
    /* An unrelated write still permits a proven hit; forgetting its history
     * must make the old memo unprovable even though its value happens to match. */
    inputs[31] = 99;
    assert(pf_write(&core, 31, 99) == PF_OK);
    saved = core;
    assert(pf_write(&core, 31, 99) == PF_OK);
    assert(memcmp(&core, &saved, sizeof(core)) == 0);
    check_query(&core, branch_program, 8, inputs, 7, 10000);
    assert(core.work <= 1 + PF_CACHE + PF_HISTORY);
    inputs[31] = 100;
    assert(pf_write(&core, 31, 100) == PF_OK);
    pf_forget(&core, PF_FORGET_HISTORY);
    check_query(&core, branch_program, 8, inputs, 7, 10000);
    const uint64_t recompute_work = core.work;
    check_query(&core, branch_program, 8, inputs, 7, 10000);
    assert(core.work < recompute_work);
    inputs[0] = 1;
    assert(pf_write(&core, 0, 1) == PF_OK);
    check_query(&core, branch_program, 8, inputs, 7, 10000);

    /* Overwrite the journal many times without querying. */
    for (unsigned i = 0; i < 40; ++i) {
        inputs[1] = i;
        assert(pf_write(&core, 1, i) == PF_OK);
    }
    check_query(&core, branch_program, 8, inputs, 7, 10000);
    /* White-box setup of the otherwise impractical epoch rollover case. */
    core.revision = UINT64_MAX;
    inputs[1] = UINT64_MAX;
    assert(pf_write(&core, 1, inputs[1]) == PF_OK);
    assert(core.revision == 2);
    check_query(&core, branch_program, 8, inputs, 7, 10000);
    for (uint64_t budget = 0; budget < 160; ++budget) {
        pf_forget(&core, PF_FORGET_VALUES | PF_FORGET_HISTORY);
        check_query(&core, branch_program, 8, inputs, 7, budget);
    }

    const PfNode select[] = {
        {.op = PF_INPUT, .a = 0}, {.op = PF_INPUT, .a = 1},
        {.op = PF_INPUT, .a = 31}, {.op = PF_SELECT, .a = 0, .b = 1, .c = 2},
    };
    assert(pf_load(&core, select, 4) == PF_OK);
    inputs[0] = 0;
    assert(pf_write(&core, 0, 0) == PF_OK);
    check_query(&core, select, 4, inputs, 3, 10000);
    int saw_dependencies = 0;
    for (unsigned i = 0; i < PF_CACHE; ++i) {
        if (core.memo[i].node_plus_one == 4) {
            assert(core.memo[i].dependencies == UINT32_C(0x80000001));
            saw_dependencies = 1;
        }
    }
    assert(saw_dependencies);
    inputs[1] = 123;
    assert(pf_write(&core, 1, 123) == PF_OK);
    check_query(&core, select, 4, inputs, 3, 10000);
    assert(core.work <= 1 + PF_CACHE + PF_HISTORY);
    inputs[31] = 456;
    assert(pf_write(&core, 31, 456) == PF_OK);
    check_query(&core, select, 4, inputs, 3, 10000);
    inputs[0] = 1;
    assert(pf_write(&core, 0, 1) == PF_OK);
    check_query(&core, select, 4, inputs, 3, 10000);

    PfNode chain[PF_NODES] = {{.op = PF_CONST, .literal = UINT64_MAX}};
    for (uint32_t i = 1; i < PF_NODES; ++i)
        chain[i] = (PfNode){.op = PF_ADD, .a = i - 1, .b = 0};
    assert(pf_load(&core, chain, PF_NODES) == PF_OK);
    check_query(&core, chain, PF_NODES, inputs, PF_NODES - 1, 10000);
    chain[0].literal = 2;
    assert(pf_load(&core, chain, PF_NODES) == PF_OK);
    check_query(&core, chain, PF_NODES, inputs, PF_NODES - 1, 10000);

    /* A compact DAG can require exponential work without retained results.
     * The core must reject within the budget, not promise bounded latency. */
    assert(pf_init(&core, 0, 0) == PF_OK);
    for (uint32_t i = 1; i < PF_NODES; ++i) chain[i].b = i - 1;
    assert(pf_load(&core, chain, PF_NODES) == PF_OK);
    out = 456;
    assert(pf_eval(&core, PF_NODES - 1, 100, &out) == PF_WORK_LIMIT);
    assert(core.work == 100 && out == 456 && core.depth == 0);
    ++exhausted_queries;
}

static void random_trace(void) {
    for (unsigned trial = 0; trial < 32; ++trial) {
        PfCore core;
        PfNode nodes[16] = {0};
        uint64_t inputs[PF_INPUTS] = {0};
        assert(pf_init(&core, trial % 9, (trial / 3) % 9) == PF_OK);
        for (uint32_t i = 0; i < 16; ++i) {
            nodes[i].op = i < 4 ? PF_INPUT : (uint32_t)(random_word() % 6);
            if (nodes[i].op == PF_INPUT) nodes[i].a = (uint32_t)(random_word() % 32);
            else if (nodes[i].op >= PF_ADD) {
                nodes[i].a = (uint32_t)(random_word() % i);
                nodes[i].b = (uint32_t)(random_word() % i);
                nodes[i].c = (uint32_t)(random_word() % i);
            }
            nodes[i].literal = random_word();
        }
        assert(pf_load(&core, nodes, 16) == PF_OK);
        for (unsigned action = 0; action < 1000; ++action) {
            const uint64_t choice = random_word();
            if (choice % 11 < 6) {
                const uint32_t id = (uint32_t)(random_word() % 32);
                inputs[id] = random_word();
                assert(pf_write(&core, id, inputs[id]) == PF_OK);
            } else if (choice % 11 < 9) {
                pf_forget(&core, (unsigned)(choice % 3 + 1));
            } else if (choice % 11 == 9) {
                const uint32_t id = 4 + (uint32_t)(random_word() % 12);
                nodes[id] = (PfNode){.op = PF_SELECT,
                    .a = (uint32_t)(random_word() % id),
                    .b = (uint32_t)(random_word() % id),
                    .c = (uint32_t)(random_word() % id)};
                assert(pf_load(&core, nodes, 16) == PF_OK);
            }
            check_query(&core, nodes, 16, inputs, (uint32_t)(choice % 16), choice % 300);
        }
    }
}

int main(void) {
    directed();
    exhaustive();
    random_trace();
    printf("{\"checks\":%" PRIu64 ",\"successful_queries\":%" PRIu64
           ",\"exhausted_queries\":%" PRIu64 ",\"core_bytes\":%zu,"
           "\"node_bytes\":%zu,\"memo_bytes\":%zu,\"history_entry_bytes\":%zu,"
           "\"frame_bytes\":%zu,\"seed\":20260922}\n",
           checks, queries, exhausted_queries, sizeof(PfCore), sizeof(PfNode),
           sizeof(PfMemo), sizeof(PfChange), sizeof(PfFrame));
    return 0;
}
