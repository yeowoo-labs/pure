#include "module.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); abort(); } } while (0)

typedef struct { const char *name; double initial; } S;
typedef struct { const char *name, *key; } I;
typedef struct { const char *name; uint32_t start, length; } V;
typedef struct { uint32_t event, start, length; } H;
typedef struct { uint8_t op; uint32_t arg; double value; } O;
typedef struct {
    const S *s; size_t ns; const I *in; size_t ni; const V *v; size_t nv;
    const H *h; size_t nh; const O *o; size_t no;
} Spec;

static unsigned char *put32(unsigned char *p, uint32_t v) {
    for (unsigned i = 0; i < 4; ++i) p[i] = (unsigned char)(v >> (8 * i));
    return p + 4;
}

static unsigned char *put_double(unsigned char *p, double value) {
    uint64_t bits;
    memcpy(&bits, &value, sizeof(bits));
    put32(p, (uint32_t)bits);
    return put32(p + 4, (uint32_t)(bits >> 32));
}

/* Test-side encoder for format v1; the toolchain has its own. */
static size_t build(unsigned char *out, Spec spec) {
    unsigned char strings[4096];
    size_t used = 0;
    uint32_t offset[256];
    size_t names = 0;
    const char *text[256];
    for (size_t i = 0; i < spec.ns; ++i) text[names++] = spec.s[i].name;
    for (size_t i = 0; i < spec.ni; ++i) { text[names++] = spec.in[i].name; text[names++] = spec.in[i].key; }
    for (size_t i = 0; i < spec.nv; ++i) text[names++] = spec.v[i].name;
    for (size_t i = 0; i < names; ++i) {
        size_t length = strlen(text[i]);
        offset[i] = (uint32_t)used;
        strings[used++] = (unsigned char)length;
        memcpy(strings + used, text[i], length);
        used += length;
    }
    unsigned char *p = out;
    memcpy(p, "PUREMOD1", 8);
    p[8] = (unsigned char)spec.ns; p[9] = (unsigned char)(spec.ns >> 8);
    p[10] = (unsigned char)spec.ni; p[11] = 0;
    p[12] = (unsigned char)spec.nv; p[13] = 0;
    p[14] = (unsigned char)spec.nh; p[15] = 0;
    put32(p + 16, (uint32_t)spec.no);
    put32(p + 20, (uint32_t)used);
    memset(p + 24, 0, 8);
    p += 32;
    size_t n = 0;
    for (size_t i = 0; i < spec.ns; ++i) { p = put32(p, offset[n++]); p = put32(p, 0); p = put_double(p, spec.s[i].initial); }
    for (size_t i = 0; i < spec.ni; ++i) { p = put32(p, offset[n++]); p = put32(p, 1); p = put32(p, offset[n++]); }
    for (size_t i = 0; i < spec.nv; ++i) { p = put32(p, offset[n++]); p = put32(p, spec.v[i].start); p = put32(p, spec.v[i].length); }
    for (size_t i = 0; i < spec.nh; ++i) { p = put32(p, spec.h[i].event); p = put32(p, spec.h[i].start); p = put32(p, spec.h[i].length); }
    for (size_t i = 0; i < spec.no; ++i) {
        *p++ = spec.o[i].op; *p++ = 0; *p++ = 0; *p++ = 0;
        p = put32(p, spec.o[i].arg);
        p = put_double(p, spec.o[i].value);
    }
    memcpy(p, strings, used);
    return (size_t)(p + used - out);
}

static PureModule module_a, module_b;
static char error[128];

static PureResult load(Spec spec, PureModule *out) {
    static unsigned char bytes[65536];
    size_t size = build(bytes, spec);
    return pure_module_load(bytes, size, out, error, sizeof(error));
}

#define LEN(a) (sizeof(a) / sizeof(*(a)))
#define SPEC(s, in, v, h, o) ((Spec){s, LEN(s), in, LEN(in), v, LEN(v), h, LEN(h), o, LEN(o)})

enum { C = PURE_OP_CONST, L = PURE_OP_LOAD, DT = PURE_OP_DT, ADD = PURE_OP_ADD,
       MUL = PURE_OP_MUL, DIV = PURE_OP_DIV, GT = PURE_OP_GT, MIN = PURE_OP_MIN,
       MAX = PURE_OP_MAX, CL = PURE_OP_CLAMP, ST = PURE_OP_STORE, JZ = PURE_OP_JZ,
       JMP = PURE_OP_JMP, NEG = PURE_OP_NEG };

static const S counter_states[] = {{"count", 0}, {"x", 3}};
static const I counter_inputs[] = {{"tap", "Space"}, {"move", "ArrowRight"}};
static const V counter_views[] = {{"label", 0, 1}, {"width", 1, 6}};
static const O counter_code[] = {
    {L, 0, 0},                                                   /* 0 view label */
    {C, 0, 40}, {L, 0, 0}, {C, 0, 20}, {MIN, 0, 0}, {C, 0, 8},   /* 1 view width */
    {MUL, 0, 0},
    /* 7: fixed-up below: 40 + min(count, 20) * 8 needs an ADD; see width2 */
    {L, 0, 0}, {C, 0, 1}, {ADD, 0, 0}, {ST, 0, 0},               /* 7 tap.down */
    /* 11 move.down: if x > 5 { x = 0 } else { x = x + 1 } */
    {L, 1, 0}, {C, 0, 5}, {GT, 0, 0}, {JZ, 18, 0}, {C, 0, 0}, {ST, 1, 0}, {JMP, 22, 0},
    {L, 1, 0}, {C, 0, 1}, {ADD, 0, 0}, {ST, 1, 0},
    /* 22 frame: x = x + dt */
    {L, 1, 0}, {DT, 0, 0}, {ADD, 0, 0}, {ST, 1, 0},
    /* 26 view width2 = 40 + min(count, 20) * 8 */
    {C, 0, 40}, {L, 0, 0}, {C, 0, 20}, {MIN, 0, 0}, {C, 0, 8}, {MUL, 0, 0}, {ADD, 0, 0},
};
static const H counter_handlers[] = {{1, 7, 4}, {3, 11, 11}, {0, 22, 4}};

static void behaviour(void) {
    /* width at 1..7 leaves two values: rejected at load. */
    CHECK(load(SPEC(counter_states, counter_inputs, counter_views, counter_handlers, counter_code), &module_a) == PURE_INVALID);
    CHECK(strstr(error, "view must leave one value"));
    static const V views[] = {{"label", 0, 1}, {"width", 26, 7}};
    CHECK(load(SPEC(counter_states, counter_inputs, views, counter_handlers, counter_code), &module_a) == PURE_OK);
    PureApp app;
    pure_app_start(&app, &module_a);
    double value;
    CHECK(pure_app_view(&app, 1, &value) && value == 40);
    for (int i = 0; i < 25; ++i) CHECK(pure_app_input(&app, 0, PURE_EDGE_DOWN) == PURE_OK);
    CHECK(app.values[0] == 25 && pure_app_view(&app, 1, &value) && value == 200);
    CHECK(pure_app_input(&app, 0, PURE_EDGE_UP) == PURE_OK && app.events == 26);
    /* if / else with forward JZ and JMP */
    CHECK(pure_app_input(&app, 1, PURE_EDGE_DOWN) == PURE_OK && app.values[1] == 4);
    CHECK(pure_app_input(&app, 1, PURE_EDGE_DOWN) == PURE_OK && app.values[1] == 5);
    CHECK(pure_app_input(&app, 1, PURE_EDGE_DOWN) == PURE_OK && app.values[1] == 6);
    CHECK(pure_app_input(&app, 1, PURE_EDGE_DOWN) == PURE_OK && app.values[1] == 0);
    CHECK(pure_app_frame(&app) == PURE_OK && app.values[1] == 1.0 / 60.0 && app.ticks == 1);
    CHECK(pure_app_input(&app, 2, PURE_EDGE_DOWN) == PURE_INVALID);
    CHECK(pure_module_input(&module_a, "move") == 1 && pure_module_view(&module_a, "width") == 1 &&
          pure_module_state(&module_a, "missing") == -1);

    /* Replacement keeps values by name. */
    static const S next_states[] = {{"fresh", 7}, {"count", 99}};
    static const I no_inputs[1];
    static const V no_views[1];
    static const H no_handlers[1];
    static const O no_code[1];
    CHECK(load((Spec){next_states, 2, no_inputs, 0, no_views, 0, no_handlers, 0, no_code, 0}, &module_b) == PURE_OK);
    pure_app_replace(&app, &module_b);
    CHECK(app.module == &module_b && app.values[0] == 7 && app.values[1] == 25);
    pure_app_replace(&app, &module_a);
    CHECK(app.values[0] == 25 && app.values[1] == 3);
}

/* A fault leaves every state exactly as before the event. */
static void faults(void) {
    static const S states[] = {{"a", 1}, {"b", 2}};
    static const I inputs[] = {{"one", "KeyA"}, {"two", "KeyB"}, {"three", "Digit3"}};
    static const O code[] = {
        {C, 0, 9}, {ST, 0, 0}, {C, 0, 1}, {C, 0, 0}, {DIV, 0, 0}, {ST, 1, 0},        /* 0 divide */
        {C, 0, 9}, {ST, 0, 0}, {C, 0, 1e308}, {C, 0, 10}, {MUL, 0, 0}, {ST, 1, 0},  /* 6 overflow */
        {C, 0, 9}, {ST, 0, 0}, {C, 0, 1}, {C, 0, 3}, {C, 0, 2}, {CL, 0, 0}, {ST, 1, 0}, /* 12 clamp */
        {C, 0, 5}, {ST, 0, 0},                                                         /* 19 ok */
        {C, 0, 0}, {NEG, 0, 0}, {C, 0, 0}, {MIN, 0, 0},                                /* 21 view -0 */
        {C, 0, 0}, {C, 0, 0}, {NEG, 0, 0}, {MAX, 0, 0},                                /* 25 view +0 */
    };
    static const H handlers[] = {{1, 0, 6}, {3, 6, 6}, {5, 12, 7}, {5, 19, 2}};
    static const V views[] = {{"low", 21, 4}, {"high", 25, 4}};
    CHECK(load(SPEC(states, inputs, views, handlers, code), &module_a) == PURE_OK);
    PureApp app;
    pure_app_start(&app, &module_a);
    const PureFault expected[] = {PURE_FAULT_DIVIDE, PURE_FAULT_NON_FINITE, PURE_FAULT_CLAMP};
    for (uint32_t i = 0; i < 3; ++i) {
        CHECK(pure_app_input(&app, i, PURE_EDGE_DOWN) == PURE_INVALID && app.fault == expected[i]);
        CHECK(app.values[0] == 1 && app.values[1] == 2 && app.events == 0);
    }
    /* A later handler of the same event does not run after a fault. */
    CHECK(app.fault_pc == 17);
    double value;
    CHECK(pure_app_view(&app, 0, &value) && value == 0 && signbit(value));
    CHECK(pure_app_view(&app, 1, &value) && value == 0 && !signbit(value));
}

typedef struct { const char *reason; Spec spec; PureResult result; } Rejection;

static void rejections(void) {
    static const S s1[] = {{"a", 0}};
    static const S dup[] = {{"a", 0}, {"a", 1}};
    static const S bad_name[] = {{"1a", 0}};
    static const S long_name[] = {{"abcdefghijklmnopqrstuvwxyz012345", 0}};
    static const S non_finite[] = {{"a", INFINITY}};
    static const I i1[] = {{"k", "Space"}};
    static const I bad_key[] = {{"k", "KeyAA"}};
    static const I dup_key[] = {{"k", "Space"}, {"j", "Space"}};
    static const V clash[] = {{"a", 0, 1}};
    static const V v_store[] = {{"v", 0, 2}};
    static const V v_dt[] = {{"v", 2, 1}};
    static const V v_empty[] = {{"v", 0, 0}};
    static const H none[1];
    static const O load_a[] = {{L, 0, 0}};
    static const O store[] = {{C, 0, 1}, {ST, 0, 0}, {DT, 0, 0}};
    static const H h0[] = {{0, 0, 2}};
    static const H h_bad_event[] = {{3, 0, 2}};
    static const O back[] = {{C, 0, 1}, {JZ, 0, 0}};
    static const O beyond[] = {{C, 0, 1}, {JZ, 3, 0}};
    static const O under[] = {{ADD, 0, 0}};
    static const O join[] = {{C, 0, 1}, {JZ, 4, 0}, {C, 0, 1}, {C, 0, 1}, {ST, 0, 0}};
    static const O leftover[] = {{C, 0, 1}, {C, 0, 2}};
    static const O unknown_state[] = {{L, 1, 0}, {ST, 0, 0}};
    static const O nan_const[] = {{C, 0, NAN}, {ST, 0, 0}};
    static const O noncanon[] = {{C, 0, 1}, {NEG, 1, 0}, {ST, 0, 0}};
    static const O unreachable[] = {{JMP, 2, 0}, {C, 0, 1}, {C, 0, 1}, {ST, 0, 0}};
    static const O bad_op[] = {{99, 0, 0}, {ST, 0, 0}};
    static const H h_all[] = {{0, 0, 1}};
    static const H h_two[] = {{0, 0, 2}};
    static const H h_five[] = {{0, 0, 5}};
    static const H h_four[] = {{0, 0, 4}};
    O overflow[66];
    for (int i = 0; i < 65; ++i) overflow[i] = (O){C, 0, 1};
    overflow[65] = (O){ST, 0, 0};
    const Rejection cases[] = {
        {"duplicate name", {dup, 2, i1, 0, clash, 0, none, 0, load_a, 0}, PURE_INVALID},
        {"state name", {bad_name, 1, i1, 0, clash, 0, none, 0, load_a, 0}, PURE_INVALID},
        {"state name", {long_name, 1, i1, 0, clash, 0, none, 0, load_a, 0}, PURE_INVALID},
        {"non-finite initial value", {non_finite, 1, i1, 0, clash, 0, none, 0, load_a, 0}, PURE_INVALID},
        {"input key", {s1, 1, bad_key, 1, clash, 0, none, 0, load_a, 0}, PURE_INVALID},
        {"duplicate key", {s1, 1, dup_key, 2, clash, 0, none, 0, load_a, 0}, PURE_INVALID},
        {"duplicate name", {s1, 1, i1, 0, clash, 1, none, 0, load_a, 1}, PURE_INVALID},
        {"view with an effect", {s1, 1, i1, 0, v_store, 1, none, 0, store, 3}, PURE_INVALID},
        {"view with an effect", {s1, 1, i1, 0, v_dt, 1, none, 0, store, 3}, PURE_INVALID},
        {"empty view", {s1, 1, i1, 0, v_empty, 1, none, 0, store, 3}, PURE_INVALID},
        {"handler event", {s1, 1, i1, 1, clash, 0, h_bad_event, 1, store, 3}, PURE_INVALID},
        {"branch not forward", {s1, 1, i1, 0, clash, 0, h_two, 1, back, 2}, PURE_INVALID},
        {"branch not forward", {s1, 1, i1, 0, clash, 0, h_two, 1, beyond, 2}, PURE_INVALID},
        {"stack underflow", {s1, 1, i1, 0, clash, 0, h_all, 1, under, 1}, PURE_INVALID},
        {"stack height differs", {s1, 1, i1, 0, clash, 0, h_five, 1, join, 5}, PURE_INVALID},
        {"handler must leave no value", {s1, 1, i1, 0, clash, 0, h_two, 1, leftover, 2}, PURE_INVALID},
        {"unknown state", {s1, 1, i1, 0, clash, 0, h_two, 1, unknown_state, 2}, PURE_INVALID},
        {"non-canonical operand", {s1, 1, i1, 0, clash, 0, h_two, 1, nan_const, 2}, PURE_INVALID},
        {"non-canonical operand", {s1, 1, i1, 0, clash, 0, h0, 1, noncanon, 3}, PURE_INVALID},
        {"unreachable instruction", {s1, 1, i1, 0, clash, 0, h_four, 1, unreachable, 4}, PURE_INVALID},
        {"unknown opcode", {s1, 1, i1, 0, clash, 0, h_two, 1, bad_op, 2}, PURE_INVALID},
        {"stack overflow", {s1, 1, i1, 0, clash, 0, (H[]){{0, 0, 66}}, 1, overflow, 66}, PURE_INVALID},
    };
    for (size_t i = 0; i < LEN(cases); ++i) {
        PureResult result = load(cases[i].spec, &module_a);
        if (result != cases[i].result || !strstr(error, cases[i].reason)) {
            fprintf(stderr, "case %zu: expected '%s', got %d '%s'\n", i, cases[i].reason, (int)result, error);
            abort();
        }
    }
    static S many[33];
    static char many_names[33][8];
    for (unsigned i = 0; i < 33; ++i) { snprintf(many_names[i], 8, "s%u", i % 100u); many[i] = (S){many_names[i], 0}; }
    CHECK(load((Spec){many, 33, i1, 0, clash, 0, none, 0, load_a, 0}, &module_a) == PURE_LIMIT);
    static unsigned char bytes[65536];
    size_t size = build(bytes, (Spec){s1, 1, i1, 0, clash, 0, none, 0, load_a, 0});
    CHECK(pure_module_load(bytes, size - 1, &module_a, error, sizeof(error)) == PURE_INVALID);
    CHECK(pure_module_load(bytes, size + 1, &module_a, error, sizeof(error)) == PURE_INVALID);
    bytes[0] = 'X';
    CHECK(pure_module_load(bytes, size, &module_a, error, sizeof(error)) == PURE_INVALID);
}

static uint32_t next_random(uint32_t *state) {
    *state ^= *state << 13;
    *state ^= *state >> 17;
    *state ^= *state << 5;
    return *state;
}

/* Arbitrary damage is either rejected or yields a module that runs safely. */
static void mutations(void) {
    static const V views[] = {{"label", 0, 1}, {"width", 26, 7}};
    static unsigned char valid[65536], bytes[65536];
    size_t size = build(valid, SPEC(counter_states, counter_inputs, views, counter_handlers, counter_code));
    uint32_t state = 0x2545f491u;
    unsigned accepted = 0;
    for (unsigned round = 0; round < 20000; ++round) {
        memcpy(bytes, valid, size);
        unsigned flips = 1 + next_random(&state) % 4;
        for (unsigned f = 0; f < flips; ++f)
            bytes[next_random(&state) % size] ^= (unsigned char)(1u << (next_random(&state) % 8));
        if (pure_module_load(bytes, size, &module_a, error, sizeof(error)) != PURE_OK) continue;
        ++accepted;
        PureApp app;
        pure_app_start(&app, &module_a);
        for (unsigned e = 0; e < 16; ++e) {
            if (e % 3 == 0) pure_app_frame(&app);
            else pure_app_input(&app, e % 4, e & 1 ? PURE_EDGE_UP : PURE_EDGE_DOWN);
            for (uint32_t v = 0; v < module_a.views; ++v) { double value; pure_app_view(&app, v, &value); }
            for (uint32_t s = 0; s < module_a.states; ++s) CHECK(isfinite(app.values[s]));
        }
    }
    printf("{\"module_mutations\":20000,\"accepted_after_damage\":%u}\n", accepted);
}

int main(void) {
    behaviour();
    faults();
    rejections();
    mutations();
    printf("{\"sizeof_module\":%zu,\"sizeof_app\":%zu}\n", sizeof(PureModule), sizeof(PureApp));
    puts("{\"module_tests\":\"verified-load-atomic-events-faults-signed-zero-if-else-replace-by-name-rejections-mutations\"}");
    return 0;
}
