#define _POSIX_C_SOURCE 200809L
#include "module.h"
#include "module-store.h"
#include "module-text.h"
#include "store.h"
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); abort(); } } while (0)

/* Store faults: a stage fails (or the process dies there when crash is set). */
static int fail_stage = -1;
static int crash;
int pure_store_test_fault(PureStoreStage stage) {
    if ((int)stage != fail_stage) return 0;
    if (crash) _exit(77);
    return 1;
}

typedef struct { const char *name; double initial; } S;
typedef struct { const char *name; uint32_t type; } F;
/* kind 0 is a key input; otherwise PURE_KIND_MESSAGE with fields. */
typedef struct { const char *name, *key; int kind; bool keyed; const F *fields; size_t nfields; } I;
#define KEY(name, key) {name, key, 0, false, NULL, 0}
typedef struct { const char *name; uint32_t start, length; } V;
typedef struct { uint32_t event, start, length; } H;
typedef struct { uint8_t op; uint32_t arg; double value; } O;
/* Text (type 1, ref = constant), cursor (type 2, ref = text state), list
 * (type 3, ref = record) and index (type 4, ref = list state) states follow
 * the number states; text views follow the number views. */
typedef struct { const char *name; uint32_t type, ref; } T;
/* A record field: a number and its default, or text (text = 1) whose
 * default is the constant numbered initial. */
typedef struct { const char *name; uint32_t text; double initial; } RF;
typedef struct { const char *name; const RF *fields; size_t nfields; } R;
/* A further view: a table (type 2, start = list state) or a column of the
 * view numbered table - 1. */
typedef struct { const char *name; uint32_t type, table, start, length; } W;
typedef struct {
    const S *s; size_t ns; const I *in; size_t ni; const V *v; size_t nv;
    const H *h; size_t nh; const O *o; size_t no; const char *const *c; size_t nc;
    const T *t; size_t nt; const char *const *k; size_t nk; const V *tv; size_t ntv;
    const R *r; size_t nr; const W *w; size_t nw;
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

/* Test-side encoder for format v5; the toolchain has its own. */
static size_t build(unsigned char *out, Spec spec) {
    unsigned char strings[4096], blob[4096];
    size_t used = 0, blob_used = 0;
    const char *text[512];
    uint32_t offset[512];
    size_t names = 0;
    for (size_t i = 0; i < spec.ns; ++i) text[names++] = spec.s[i].name;
    for (size_t i = 0; i < spec.nt; ++i) text[names++] = spec.t[i].name;
    for (size_t i = 0; i < spec.nc; ++i) text[names++] = spec.c[i];
    for (size_t i = 0; i < spec.ni; ++i) {
        text[names++] = spec.in[i].name;
        if (!spec.in[i].kind) text[names++] = spec.in[i].key;
        for (size_t f = 0; f < spec.in[i].nfields; ++f) text[names++] = spec.in[i].fields[f].name;
    }
    for (size_t i = 0; i < spec.nv; ++i) text[names++] = spec.v[i].name;
    for (size_t i = 0; i < spec.ntv; ++i) text[names++] = spec.tv[i].name;
    for (size_t i = 0; i < spec.nw; ++i) text[names++] = spec.w[i].name;
    size_t record_names = names, record_fields = 0;
    for (size_t i = 0; i < spec.nr; ++i) {
        text[names++] = spec.r[i].name;
        for (size_t f = 0; f < spec.r[i].nfields; ++f) text[names++] = spec.r[i].fields[f].name;
        record_fields += spec.r[i].nfields;
    }
    for (size_t i = 0; i < names; ++i) {
        size_t length = strlen(text[i]);
        offset[i] = (uint32_t)used;
        strings[used++] = (unsigned char)length;
        memcpy(strings + used, text[i], length);
        used += length;
    }
    size_t fields = 0;
    for (size_t i = 0; i < spec.ni; ++i) fields += spec.in[i].nfields;
    for (size_t i = 0; i < spec.nk; ++i) { memcpy(blob + blob_used, spec.k[i], strlen(spec.k[i])); blob_used += strlen(spec.k[i]); }
    unsigned char *p = out;
    memset(p, 0, 40);
    memcpy(p, "PUREMOD5", 8);
    size_t states = spec.ns + spec.nt, views = spec.nv + spec.ntv + spec.nw;
    p[8] = (unsigned char)states; p[9] = (unsigned char)(states >> 8);
    p[10] = (unsigned char)spec.ni; p[12] = (unsigned char)views; p[14] = (unsigned char)spec.nh;
    put32(p + 16, (uint32_t)spec.no);
    put32(p + 20, (uint32_t)used);
    p[24] = (unsigned char)fields; p[26] = (unsigned char)spec.nc; p[28] = (unsigned char)spec.nk;
    p[30] = (unsigned char)spec.nr; p[36] = (unsigned char)record_fields;
    put32(p + 32, (uint32_t)blob_used);
    p += 40;
    size_t n = 0;
    for (size_t i = 0; i < spec.ns; ++i) { p = put32(p, offset[n++]); p = put32(p, 0); p = put_double(p, spec.s[i].initial); }
    for (size_t i = 0; i < spec.nt; ++i) { p = put32(p, offset[n++]); p = put32(p, spec.t[i].type); p = put32(p, spec.t[i].ref); p = put32(p, 0); }
    for (size_t i = 0; i < spec.nc; ++i) { p = put32(p, offset[n++]); p = put32(p, 1); p = put32(p, 1); }
    uint32_t field_offsets[64], field_start = 0;
    size_t nf = 0;
    for (size_t i = 0; i < spec.ni; ++i) {
        const I *in = &spec.in[i];
        p = put32(p, offset[n++]);
        *p++ = (unsigned char)(in->kind ? in->kind : 1); *p++ = in->keyed; *p++ = 0; *p++ = 0;
        p = put32(p, in->kind ? 0 : offset[n++]);
        uint32_t first = in->kind == PURE_KIND_MESSAGE ? field_start : 0; /* only messages have fields */
        *p++ = (unsigned char)first; *p++ = (unsigned char)(first >> 8);
        *p++ = (unsigned char)in->nfields; *p++ = 0;
        for (size_t f = 0; f < in->nfields; ++f) field_offsets[nf++] = offset[n++];
        field_start += (uint32_t)in->nfields;
    }
    nf = 0;
    for (size_t i = 0; i < spec.ni; ++i)
        for (size_t f = 0; f < spec.in[i].nfields; ++f) {
            p = put32(p, field_offsets[nf++]);
            p = put32(p, spec.in[i].fields[f].type);
        }
    for (size_t i = 0, at = 0; i < spec.nk; ++i) { p = put32(p, (uint32_t)at); p = put32(p, (uint32_t)strlen(spec.k[i])); at += strlen(spec.k[i]); }
    size_t record_field = record_names;
    for (size_t i = 0, first = 0; i < spec.nr; ++i) {
        p = put32(p, offset[record_field]);
        record_field += 1 + spec.r[i].nfields;
        *p++ = (unsigned char)first; *p++ = 0; *p++ = (unsigned char)spec.r[i].nfields; *p++ = 0;
        first += spec.r[i].nfields;
    }
    for (size_t i = 0, n = record_names; i < spec.nr; ++i, ++n)
        for (size_t f = 0; f < spec.r[i].nfields; ++f) {
            const RF *field = &spec.r[i].fields[f];
            p = put32(p, offset[++n]);
            p = put32(p, field->text);
            if (field->text) { p = put32(p, (uint32_t)field->initial); p = put32(p, 0); }
            else p = put_double(p, field->initial);
        }
    for (size_t i = 0; i < spec.nv; ++i) { p = put32(p, offset[n++]); p = put32(p, 0); p = put32(p, spec.v[i].start); p = put32(p, spec.v[i].length); }
    for (size_t i = 0; i < spec.ntv; ++i) { p = put32(p, offset[n++]); p = put32(p, 1); p = put32(p, spec.tv[i].start); p = put32(p, spec.tv[i].length); }
    for (size_t i = 0; i < spec.nw; ++i) {
        const W *w = &spec.w[i];
        p = put32(p, offset[n++]);
        *p++ = (unsigned char)w->type; *p++ = 0; *p++ = (unsigned char)w->table; *p++ = (unsigned char)(w->table >> 8);
        p = put32(p, w->start);
        p = put32(p, w->length);
    }
    for (size_t i = 0; i < spec.nh; ++i) { p = put32(p, spec.h[i].event); p = put32(p, spec.h[i].start); p = put32(p, spec.h[i].length); }
    for (size_t i = 0; i < spec.no; ++i) {
        *p++ = spec.o[i].op; *p++ = 0; *p++ = 0; *p++ = 0;
        p = put32(p, spec.o[i].arg);
        p = put_double(p, spec.o[i].value);
    }
    memcpy(p, strings, used);
    memcpy(p + used, blob, blob_used);
    return (size_t)(p + used + blob_used - out);
}

static PureModule module_a, module_b;
static char error[128];

static PureResult load(Spec spec, PureModule *out) {
    static unsigned char bytes[65536];
    size_t size = build(bytes, spec);
    return pure_module_load(bytes, size, out, error, sizeof(error));
}

#define LEN(a) (sizeof(a) / sizeof(*(a)))
#define SPEC(s, in, v, h, o) ((Spec){s, LEN(s), in, LEN(in), v, LEN(v), h, LEN(h), o, LEN(o), NULL, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0})

enum { C = PURE_OP_CONST, L = PURE_OP_LOAD, DT = PURE_OP_DT, ADD = PURE_OP_ADD,
       MUL = PURE_OP_MUL, DIV = PURE_OP_DIV, GT = PURE_OP_GT, MIN = PURE_OP_MIN,
       MAX = PURE_OP_MAX, CL = PURE_OP_CLAMP, ST = PURE_OP_STORE, JZ = PURE_OP_JZ,
       JMP = PURE_OP_JMP, NEG = PURE_OP_NEG };

static const S counter_states[] = {{"count", 0}, {"x", 3}};
static const I counter_inputs[] = {KEY("tap", "Space"), KEY("move", "ArrowRight")};
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
    CHECK(strstr(error, "view must leave one value of its type"));
    static const V views[] = {{"label", 0, 1}, {"width", 26, 7}};
    CHECK(load(SPEC(counter_states, counter_inputs, views, counter_handlers, counter_code), &module_a) == PURE_OK);
    PureApp app;
    CHECK(pure_app_start(&app, &module_a, (PureData){0}) == PURE_OK);
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
    CHECK(load((Spec){next_states, 2, no_inputs, 0, no_views, 0, no_handlers, 0, no_code, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0}, &module_b) == PURE_OK);
    CHECK(pure_app_replace(&app, &module_b) == PURE_OK);
    CHECK(app.module == &module_b && app.values[0] == 7 && app.values[1] == 25);
    CHECK(pure_app_replace(&app, &module_a) == PURE_OK);
    CHECK(app.values[0] == 25 && app.values[1] == 3);
    pure_app_stop(&app);
}

/* A fault leaves every state exactly as before the event. */
static void faults(void) {
    static const S states[] = {{"a", 1}, {"b", 2}};
    static const I inputs[] = {KEY("one", "KeyA"), KEY("two", "KeyB"), KEY("three", "Digit3")};
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
    CHECK(pure_app_start(&app, &module_a, (PureData){0}) == PURE_OK);
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
    pure_app_stop(&app);
}

typedef struct { const char *reason; Spec spec; PureResult result; } Rejection;

static void rejections(void) {
    static const S s1[] = {{"a", 0}};
    static const S dup[] = {{"a", 0}, {"a", 1}};
    static const S bad_name[] = {{"1a", 0}};
    static const S long_name[] = {{"abcdefghijklmnopqrstuvwxyz012345", 0}};
    static const S non_finite[] = {{"a", INFINITY}};
    static const I i1[] = {KEY("k", "Space")};
    static const I bad_key[] = {KEY("k", "KeyAA")};
    static const I dup_key[] = {KEY("k", "Space"), KEY("j", "Space")};
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
        {"duplicate name", {dup, 2, i1, 0, clash, 0, none, 0, load_a, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0}, PURE_INVALID},
        {"state name", {bad_name, 1, i1, 0, clash, 0, none, 0, load_a, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0}, PURE_INVALID},
        {"state name", {long_name, 1, i1, 0, clash, 0, none, 0, load_a, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0}, PURE_INVALID},
        {"non-finite initial value", {non_finite, 1, i1, 0, clash, 0, none, 0, load_a, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0}, PURE_INVALID},
        {"input key", {s1, 1, bad_key, 1, clash, 0, none, 0, load_a, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0}, PURE_INVALID},
        {"duplicate key", {s1, 1, dup_key, 2, clash, 0, none, 0, load_a, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0}, PURE_INVALID},
        {"duplicate name", {s1, 1, i1, 0, clash, 1, none, 0, load_a, 1, NULL, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0}, PURE_INVALID},
        {"view with an effect", {s1, 1, i1, 0, v_store, 1, none, 0, store, 3, NULL, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0}, PURE_INVALID},
        {"view with an effect", {s1, 1, i1, 0, v_dt, 1, none, 0, store, 3, NULL, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0}, PURE_INVALID},
        {"empty view", {s1, 1, i1, 0, v_empty, 1, none, 0, store, 3, NULL, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0}, PURE_INVALID},
        {"handler event", {s1, 1, i1, 1, clash, 0, h_bad_event, 1, store, 3, NULL, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0}, PURE_INVALID},
        {"branch not forward", {s1, 1, i1, 0, clash, 0, h_two, 1, back, 2, NULL, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0}, PURE_INVALID},
        {"branch not forward", {s1, 1, i1, 0, clash, 0, h_two, 1, beyond, 2, NULL, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0}, PURE_INVALID},
        {"stack underflow", {s1, 1, i1, 0, clash, 0, h_all, 1, under, 1, NULL, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0}, PURE_INVALID},
        {"join with different stacks", {s1, 1, i1, 0, clash, 0, h_five, 1, join, 5, NULL, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0}, PURE_INVALID},
        {"handler must leave no value", {s1, 1, i1, 0, clash, 0, h_two, 1, leftover, 2, NULL, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0}, PURE_INVALID},
        {"no such state", {s1, 1, i1, 0, clash, 0, h_two, 1, unknown_state, 2, NULL, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0}, PURE_INVALID},
        {"non-canonical operand", {s1, 1, i1, 0, clash, 0, h_two, 1, nan_const, 2, NULL, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0}, PURE_INVALID},
        {"non-canonical operand", {s1, 1, i1, 0, clash, 0, h0, 1, noncanon, 3, NULL, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0}, PURE_INVALID},
        {"unreachable instruction", {s1, 1, i1, 0, clash, 0, h_four, 1, unreachable, 4, NULL, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0}, PURE_INVALID},
        {"unknown opcode", {s1, 1, i1, 0, clash, 0, h_two, 1, bad_op, 2, NULL, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0}, PURE_INVALID},
        {"stack overflow", {s1, 1, i1, 0, clash, 0, (H[]){{0, 0, 66}}, 1, overflow, 66, NULL, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0}, PURE_INVALID},
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
    CHECK(load((Spec){many, 33, i1, 0, clash, 0, none, 0, load_a, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0}, &module_a) == PURE_LIMIT);
    static unsigned char bytes[65536];
    size_t size = build(bytes, (Spec){s1, 1, i1, 0, clash, 0, none, 0, load_a, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0});
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
        CHECK(pure_app_start(&app, &module_a, (PureData){0}) == PURE_OK);
        for (unsigned e = 0; e < 16; ++e) {
            if (e % 3 == 0) pure_app_frame(&app);
            else pure_app_input(&app, e % 4, e & 1 ? PURE_EDGE_UP : PURE_EDGE_DOWN);
            for (uint32_t v = 0; v < module_a.views; ++v) { double value; pure_app_view(&app, v, &value); }
            for (uint32_t s = 0; s < module_a.states; ++s) CHECK(isfinite(app.values[s]));
        }
        pure_app_stop(&app);
    }
    printf("{\"module_mutations\":20000,\"accepted_after_damage\":%u}\n", accepted);
}

static Spec editor_spec;

/* An in-memory data host: every splice makes a new immutable version. */
typedef struct { char key[65]; unsigned char *bytes; size_t size; int64_t revision; } Stored;
typedef struct {
    Stored docs[8];
    size_t count, limit;
    bool busy;
    unsigned opened, closed;
} Memory;
typedef struct {
    Memory *memory;
    int index; /* -1 for an absent key */
    int64_t base;
    unsigned char *versions[64];
    size_t sizes[64], count;
} Doc;

static Stored *stored(Memory *m, const char *key) {
    for (size_t i = 0; i < m->count; ++i) if (!strcmp(m->docs[i].key, key)) return &m->docs[i];
    return NULL;
}

static PureResult fake_open(void *context, uint32_t collection, const char *key, void **out, int64_t *revision) {
    Memory *m = context;
    CHECK(collection == 0);
    Doc *d = calloc(1, sizeof(*d));
    Stored *s = stored(m, key);
    d->memory = m;
    d->index = s ? (int)(s - m->docs) : -1;
    if (!s) {
        CHECK(m->count < 8);
        d->index = (int)m->count;
        snprintf(m->docs[m->count++].key, 65, "%s", key);
        s = &m->docs[d->index];
    }
    d->base = s->revision;
    d->versions[0] = malloc(s->size + 1);
    memcpy(d->versions[0], s->bytes ? s->bytes : (unsigned char *)"", s->size);
    d->sizes[0] = s->size;
    d->count = 1;
    ++m->opened;
    *out = d;
    *revision = s->revision;
    return PURE_OK;
}

static PureVersion fake_version(void *context, void *document) {
    (void)context;
    Doc *d = document;
    return (PureVersion){d->count - 1, d->sizes[d->count - 1]};
}

/* Two spans per read, so replies arrive in pieces. */
static PureResult fake_read(void *context, void *document, PureVersion at, size_t offset, size_t length,
                            bool (*visit)(void *, PureSpan), void *visit_context) {
    (void)context;
    Doc *d = document;
    CHECK(at.ref < d->count && at.bytes == d->sizes[at.ref] && offset + length <= at.bytes);
    size_t half = length / 2;
    if (half && !visit(visit_context, (PureSpan){d->versions[at.ref] + offset, half})) return PURE_OK;
    if (length - half) visit(visit_context, (PureSpan){d->versions[at.ref] + offset + half, length - half});
    return PURE_OK;
}

static PureResult fake_splice(void *context, void *document, size_t offset, size_t remove, PureSpan insert) {
    Memory *m = context;
    Doc *d = document;
    size_t size = d->sizes[d->count - 1], next = size - remove + insert.size;
    if (next > m->limit) return PURE_LIMIT;
    CHECK(d->count < 64);
    unsigned char *bytes = malloc(next + 1);
    memcpy(bytes, d->versions[d->count - 1], offset);
    if (insert.size) memcpy(bytes + offset, insert.data, insert.size);
    memcpy(bytes + offset + insert.size, d->versions[d->count - 1] + offset + remove, size - offset - remove);
    d->versions[d->count] = bytes;
    d->sizes[d->count++] = next;
    return PURE_OK;
}

static PureResult fake_commit(void *context, void *document, int64_t *revision) {
    Memory *m = context;
    Doc *d = document;
    Stored *s = &m->docs[d->index];
    *revision = s->revision;
    if (m->busy) return PURE_BUSY;
    if (s->revision != d->base) return PURE_CONFLICT;
    free(s->bytes);
    s->size = d->sizes[d->count - 1];
    s->bytes = malloc(s->size + 1);
    memcpy(s->bytes, d->versions[d->count - 1], s->size);
    *revision = ++s->revision;
    /* Like a tree draft, a commit rebases: earlier versions are gone. */
    for (size_t i = 0; i + 1 < d->count; ++i) { free(d->versions[i]); d->versions[i] = NULL; d->sizes[i] = 0; }
    d->versions[0] = d->versions[d->count - 1];
    d->sizes[0] = d->sizes[d->count - 1];
    d->count = 1;
    d->base = s->revision;
    return PURE_OK;
}

static void fake_close(void *context, void *document) {
    Memory *m = context;
    Doc *d = document;
    for (size_t i = 0; i < d->count; ++i) free(d->versions[i]);
    free(d);
    ++m->closed;
}

static PureData fake_data(Memory *m) {
    return (PureData){m, fake_open, fake_version, fake_read, fake_splice, fake_commit, fake_close, NULL, NULL};
}

typedef struct { unsigned char bytes[256]; size_t size, spans; } Collected;

static bool count_bytes(void *context, PureSpan span) {
    *(size_t *)context += span.size;
    return true;
}

static bool collect(void *context, PureSpan span) {
    Collected *c = context;
    CHECK(c->size + span.size <= sizeof(c->bytes));
    memcpy(c->bytes + c->size, span.data, span.size);
    c->size += span.size;
    ++c->spans;
    return true;
}

static bool replied(const PureApp *app, const char *text) {
    Collected c = {{0}, 0, 0};
    CHECK(pure_app_reply(app, collect, &c) == PURE_OK);
    return c.size == strlen(text) && !memcmp(c.bytes, text, c.size);
}

static PureSpan edit_payload(unsigned char *buffer, uint32_t start, uint32_t remove, const char *insert) {
    put32(buffer, start);
    put32(buffer + 4, remove);
    memcpy(buffer + 8, insert, strlen(insert));
    return (PureSpan){buffer, 8 + strlen(insert)};
}

enum { FIELD = PURE_OP_FIELD, DOC = PURE_OP_DOC, SIZE = PURE_OP_DOC_SIZE, LEN_ = PURE_OP_LEN,
       REPL = PURE_OP_REPLACE, REPLY = PURE_OP_REPLY, REQ = PURE_OP_REQUIRE, LE = PURE_OP_LE };

/* The document editor as a module: show replies the document, edit splices
 * start .. start + remove with insert, replies insert and counts edits. A
 * keep message replies the document read before its own edit; guarded edits
 * require the final size to stay within 8 bytes. */
static void messages(void) {
    static const F edit_fields[] = {{"start", 1}, {"remove", 1}, {"insert", 2}};
    static const I inputs[] = {
        {"show", NULL, PURE_KIND_MESSAGE, true, NULL, 0},
        {"edit", NULL, PURE_KIND_MESSAGE, true, edit_fields, 3},
        {"keep", NULL, PURE_KIND_MESSAGE, true, edit_fields + 2, 1},
        {"guarded", NULL, PURE_KIND_MESSAGE, true, edit_fields, 3},
        KEY("tick", "Space"),
    };
    static const S states[] = {{"edits", 0}};
    static const char *const collections[] = {"documents"};
    static const O code[] = {
        {DOC, 0, 0}, {REPLY, 0, 0},                                                      /* 0 show */
        {FIELD, 0, 0}, {FIELD, 0, 0}, {FIELD, 1, 0}, {ADD, 0, 0}, {FIELD, 2, 0},       /* 2 edit */
        {REPL, 0, 0}, {FIELD, 2, 0}, {REPLY, 0, 0}, {L, 0, 0}, {C, 0, 1}, {ADD, 0, 0}, {ST, 0, 0},
        {DOC, 0, 0}, {REPLY, 0, 0}, {C, 0, 0}, {C, 0, 0}, {FIELD, 0, 0}, {REPL, 0, 0},  /* 14 keep: prepend */
        {FIELD, 0, 0}, {FIELD, 0, 0}, {FIELD, 1, 0}, {ADD, 0, 0}, {FIELD, 2, 0},       /* 20 guarded */
        {REPL, 0, 0}, {L, 0, 0}, {C, 0, 1}, {ADD, 0, 0}, {ST, 0, 0},
        {SIZE, 0, 0}, {C, 0, 8}, {LE, 0, 0}, {REQ, 0, 0},
    };
    static const H handlers[] = {{1, 0, 2}, {3, 2, 12}, {5, 14, 6}, {7, 20, 14}};
    static const V views[] = {{"count", 0, 0}};
    Spec spec = {states, 1, inputs, 5, views, 0, handlers, 4, code, LEN(code), collections, 1, NULL, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0};
    editor_spec = spec;
    PureResult loaded = load(spec, &module_a);
    if (loaded != PURE_OK) { fprintf(stderr, "%s\n", error); abort(); }
    Memory memory = {.limit = 100};
    PureApp app;
    CHECK(pure_app_start(&app, &module_a, fake_data(&memory)) == PURE_OK);
    unsigned char buffer[64];

    CHECK(pure_app_message(&app, 0, "doc", -1, (PureSpan){0}) == PURE_NOT_FOUND && app.revision == 0);
    CHECK(pure_app_message(&app, 1, "doc", 0, edit_payload(buffer, 0, 0, "hello")) == PURE_OK);
    CHECK(app.revision == 1 && replied(&app, "hello") && app.values[0] == 1);
    CHECK(pure_app_message(&app, 0, "doc", -1, (PureSpan){0}) == PURE_OK && app.revision == 1 && replied(&app, "hello"));
    /* stale base conflicts before bounds; bounds fail as limit; nothing changes */
    CHECK(pure_app_message(&app, 1, "doc", 0, edit_payload(buffer, 99, 0, "x")) == PURE_CONFLICT && app.revision == 1);
    CHECK(pure_app_message(&app, 1, "doc", 1, edit_payload(buffer, 99, 0, "x")) == PURE_LIMIT && app.revision == 1);
    CHECK(pure_app_message(&app, 1, "doc", 1, edit_payload(buffer, 2, 9, "x")) == PURE_LIMIT);
    CHECK(pure_app_message(&app, 1, "doc", 1, (PureSpan){buffer, 7}) == PURE_LIMIT && app.revision == 0);
    CHECK(pure_app_message(&app, 1, "doc", -1, edit_payload(buffer, 0, 0, "x")) == PURE_INVALID);
    CHECK(pure_app_message(&app, 1, "bad/key", 1, edit_payload(buffer, 0, 0, "x")) == PURE_INVALID);
    CHECK(pure_app_message(&app, 4, "doc", 1, (PureSpan){0}) == PURE_INVALID);
    CHECK(memory.docs[0].revision == 1 && memory.docs[0].size == 5 && app.values[0] == 1);
    /* A busy commit changes neither the document nor the states. */
    memory.busy = true;
    CHECK(pure_app_message(&app, 1, "doc", 1, edit_payload(buffer, 5, 0, "!")) == PURE_BUSY);
    memory.busy = false;
    CHECK(memory.docs[0].size == 5 && app.values[0] == 1 && app.events == 2);
    /* The size limit of the data host fails as limit. */
    memory.limit = 6;
    CHECK(pure_app_message(&app, 1, "doc", 1, edit_payload(buffer, 5, 0, "!!")) == PURE_LIMIT);
    memory.limit = 100;
    /* A reply read before the event's own edit keeps its content after commit. */
    CHECK(pure_app_message(&app, 2, "doc", 1, (PureSpan){(const unsigned char *)">> ", 3}) == PURE_OK);
    CHECK(app.revision == 2 && replied(&app, "hello"));
    CHECK(pure_app_message(&app, 0, "doc", -1, (PureSpan){0}) == PURE_OK && replied(&app, ">> hello"));
    /* require: an edit that would exceed 8 bytes changes nothing */
    CHECK(pure_app_message(&app, 3, "doc", 2, edit_payload(buffer, 8, 0, "!")) == PURE_LIMIT);
    CHECK(memory.docs[0].size == 8 && app.values[0] == 1);
    CHECK(pure_app_message(&app, 3, "doc", 2, edit_payload(buffer, 0, 3, "")) == PURE_OK && app.revision == 3);
    CHECK(app.values[0] == 2);
    /* Another key is its own document. */
    CHECK(pure_app_message(&app, 1, "other", 0, edit_payload(buffer, 0, 0, "abc")) == PURE_OK && app.revision == 1);
    pure_app_stop(&app);
    CHECK(memory.opened == memory.closed);
    for (size_t i = 0; i < memory.count; ++i) free(memory.docs[i].bytes);

    /* Verifier rules for typed operations. */
    static const O field_in_key[] = {{FIELD, 0, 0}, {ST, 0, 0}};
    static const H key_handler[] = {{9, 0, 2}};
    Spec bad = spec;
    bad.o = field_in_key; bad.no = 2; bad.h = key_handler; bad.nh = 1;
    CHECK(load(bad, &module_b) == PURE_INVALID && strstr(error, "message operation outside"));
    static const O num_insert[] = {{C, 0, 0}, {C, 0, 0}, {C, 0, 1}, {REPL, 0, 0}};
    static const H edit_handler[] = {{3, 0, 4}};
    bad.o = num_insert; bad.no = 4; bad.h = edit_handler;
    CHECK(load(bad, &module_b) == PURE_INVALID && strstr(error, "operand type"));
    static const O bytes_state[] = {{FIELD, 2, 0}, {ST, 0, 0}};
    static const H edit_handler2[] = {{3, 0, 2}};
    bad.o = bytes_state; bad.no = 2; bad.h = edit_handler2;
    CHECK(load(bad, &module_b) == PURE_INVALID && strstr(error, "operand type"));
    static const O far_field[] = {{FIELD, 3, 0}, {REPLY, 0, 0}};
    bad.o = far_field; bad.h = edit_handler2;
    CHECK(load(bad, &module_b) == PURE_INVALID && strstr(error, "no such state, field"));
    static const I unkeyed[] = {{"note", NULL, PURE_KIND_MESSAGE, false, NULL, 0}};
    static const O doc_read[] = {{DOC, 0, 0}, {REPLY, 0, 0}};
    static const H note_handler[] = {{1, 0, 2}};
    bad = (Spec){states, 1, unkeyed, 1, views, 0, note_handler, 1, doc_read, 2, collections, 1, NULL, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0};
    CHECK(load(bad, &module_b) == PURE_INVALID && strstr(error, "document operation outside"));
    static const F tail_first[] = {{"insert", 2}, {"start", 1}};
    static const I misplaced[] = {{"edit", NULL, PURE_KIND_MESSAGE, true, tail_first, 2}};
    bad = (Spec){states, 1, misplaced, 1, views, 0, NULL, 0, NULL, 0, collections, 1, NULL, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0};
    CHECK(load(bad, &module_b) == PURE_INVALID && strstr(error, "message field"));
    static const H even[] = {{2, 0, 0}};
    bad = (Spec){states, 1, inputs, 1, views, 0, even, 1, NULL, 0, collections, 1, NULL, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0};
    CHECK(load(bad, &module_b) == PURE_INVALID && strstr(error, "handler event"));
    static const V field_view[] = {{"v", 0, 1}};
    static const O field_only[] = {{FIELD, 0, 0}};
    bad = (Spec){states, 1, inputs, 2, field_view, 1, NULL, 0, field_only, 1, collections, 1, NULL, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0};
    CHECK(load(bad, &module_b) == PURE_INVALID && strstr(error, "view with an effect"));
    /* A value may cross a join when every path brings the same stack. */
    static const O branch_value[] = {{C, 0, 1}, {C, 0, 1}, {JZ, 3, 0}, {ST, 0, 0}};
    static const O branch_mismatch[] = {{C, 0, 1}, {C, 0, 1}, {JZ, 4, 0}, {C, 0, 2}, {ST, 0, 0}};
    static const H frame_handler[] = {{0, 0, 4}};
    bad = (Spec){states, 1, inputs, 0, views, 0, frame_handler, 1, branch_value, 4, NULL, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0};
    CHECK(load(bad, &module_b) == PURE_OK);
    static const H frame_five[] = {{0, 0, 5}};
    bad.o = branch_mismatch; bad.no = 5; bad.h = frame_five;
    CHECK(load(bad, &module_b) == PURE_INVALID && strstr(error, "join with different stacks"));
}

/* Damaged message modules either fail to load or run messages safely. */
static void message_mutations(void) {
    static unsigned char valid[65536], bytes[65536];
    size_t size = build(valid, editor_spec);
    uint32_t state = 0x9e3779b9u;
    unsigned accepted = 0;
    for (unsigned round = 0; round < 20000; ++round) {
        memcpy(bytes, valid, size);
        unsigned flips = 1 + next_random(&state) % 3;
        for (unsigned f = 0; f < flips; ++f)
            bytes[next_random(&state) % size] ^= (unsigned char)(1u << (next_random(&state) % 8));
        if (pure_module_load(bytes, size, &module_b, error, sizeof(error)) != PURE_OK) continue;
        ++accepted;
        Memory memory = {.limit = 64};
        PureApp app;
        CHECK(pure_app_start(&app, &module_b, fake_data(&memory)) == PURE_OK);
        unsigned char payload[32];
        for (unsigned e = 0; e < 12; ++e) {
            for (size_t k = 0; k < sizeof(payload); ++k) payload[k] = (unsigned char)next_random(&state);
            put32(payload, next_random(&state) % 12);
            put32(payload + 4, next_random(&state) % 4);
            PureSpan span = {payload, next_random(&state) % sizeof(payload)};
            uint32_t input = next_random(&state) % 5;
            if (module_b.input[input].kind == PURE_KIND_MESSAGE) {
                int64_t base = (int64_t)(next_random(&state) % 4) - 1;
                if (pure_app_message(&app, input, "doc", base, span) == PURE_OK) {
                    size_t reply = 0;
                    CHECK(pure_app_reply(&app, count_bytes, &reply) == PURE_OK && reply <= 256);
                }
            } else pure_app_input(&app, input, e & 1 ? PURE_EDGE_UP : PURE_EDGE_DOWN);
            pure_app_frame(&app);
            for (uint32_t s = 0; s < module_b.states; ++s) CHECK(isfinite(app.values[s]));
        }
        pure_app_stop(&app);
        CHECK(memory.opened == memory.closed);
        for (size_t i = 0; i < memory.count; ++i) free(memory.docs[i].bytes);
    }
    printf("{\"message_module_mutations\":20000,\"accepted_after_damage\":%u}\n", accepted);
}

static Spec text_spec;

typedef struct { char text[256]; size_t size; } Text;

static bool text_span(void *context, PureSpan span) {
    Text *t = context;
    CHECK(t->size + span.size < sizeof(t->text));
    memcpy(t->text + t->size, span.data, span.size);
    t->size += span.size;
    t->text[t->size] = 0;
    return true;
}

static const char *view_text(PureApp *app, uint32_t view) {
    static Text t;
    t.size = 0;
    t.text[0] = 0;
    CHECK(pure_app_view_text(app, view, text_span, &t) == PURE_OK);
    return t.text;
}

static const char *state_text(const PureApp *app, uint32_t state) {
    static Text t;
    t.size = 0;
    t.text[0] = 0;
    CHECK(pure_draft_read(app->texts[state], 0, pure_draft_size(app->texts[state]), text_span, &t) == PURE_OK);
    return t.text;
}

static double number_view(PureApp *app, uint32_t view) {
    double value;
    CHECK(pure_app_view(app, view, &value));
    return value;
}

enum { CUL = PURE_OP_CUR_LOAD, CS = PURE_OP_CUR_STORE, CM = PURE_OP_CUR_MOVE, TI = PURE_OP_TEXT_INPUT,
       TINS = PURE_OP_TEXT_INSERT, TREM = PURE_OP_TEXT_REMOVE, TL = PURE_OP_TEXT_LOAD, TK = PURE_OP_TEXT_CONST,
       CHR = PURE_OP_CHARS, NT = PURE_OP_NUM_TEXT, CAT = PURE_OP_CONCAT, TLN = PURE_OP_TEXT_LINE,
       CC = PURE_OP_CUR_COUNT, CST = PURE_OP_CUR_START };

/* A small text editor module: states edits (num), note (text), at (cursor in
 * note), title (text). The runtime keeps at on character boundaries and
 * moves it with edits; a failed event restores text and cursor. */
static void texts(void) {
    static const S nums[] = {{"edits", 0}};
    static const T textual[] = {{"note", 1, 0}, {"at", 2, 1}, {"title", 1, 1}};
    static const char *const constants[] = {"\xea\xb0\x80" "a\nbc", "\xeb\xa9\x94\xeb\xaa\xa8", "XYZ",
                                            " \xc2\xb7 ", "\xec\x9e\x90"};
    static const I inputs[] = {
        {"typed", NULL, PURE_KIND_TEXT, false, NULL, 0}, KEY("left", "ArrowLeft"), KEY("right", "ArrowRight"),
        KEY("up", "ArrowUp"), KEY("down", "ArrowDown"), KEY("back", "Backspace"), KEY("home", "Home"), KEY("bad", "KeyB"),
    };
    static const O code[] = {
        {CUL, 2, 0}, {TI, 0, 0}, {TINS, 1, 0}, {L, 0, 0}, {C, 0, 1}, {ADD, 0, 0}, {ST, 0, 0},  /* 0 typed */
        {CUL, 2, 0}, {C, 0, -1}, {CM, 0, 0}, {CS, 2, 0},                                     /* 7 left */
        {CUL, 2, 0}, {C, 0, 1}, {CM, 0, 0}, {CS, 2, 0},                                      /* 11 right */
        {CUL, 2, 0}, {C, 0, -1}, {CM, 1, 0}, {CS, 2, 0},                                     /* 15 up */
        {CUL, 2, 0}, {C, 0, 1}, {CM, 1, 0}, {CS, 2, 0},                                      /* 19 down */
        {CUL, 2, 0}, {C, 0, -1}, {CM, 0, 0}, {CUL, 2, 0}, {TREM, 1, 0},                       /* 23 back */
        {CST, 1, 0}, {CS, 2, 0},                                                            /* 28 home */
        {CUL, 2, 0}, {TK, 2, 0}, {TINS, 1, 0}, {C, 0, 1}, {C, 0, 0}, {DIV, 0, 0}, {ST, 0, 0}, /* 30 bad */
        {TL, 1, 0}, {CHR, 0, 0},                                                            /* 37 view chars */
        {CUL, 2, 0}, {CC, 0, 0},                                                             /* 39 view offset */
        {CUL, 2, 0}, {CC, 1, 0},                                                             /* 41 view line_no */
        {CUL, 2, 0}, {TLN, 1, 0},                                                            /* 43 view line */
        {TL, 3, 0}, {TK, 3, 0}, {CAT, 0, 0}, {TL, 1, 0}, {CHR, 0, 0}, {NT, 0, 0}, {CAT, 0, 0},
        {TK, 4, 0}, {CAT, 0, 0},                                                            /* 45 view header */
        {C, 0, 1}, {C, 0, 3}, {DIV, 0, 0}, {NT, 0, 0},                                      /* 54 view ratio */
    };
    static const H handlers[] = {{1, 0, 7}, {3, 7, 4}, {5, 11, 4}, {7, 15, 4}, {9, 19, 4}, {11, 23, 5},
                                 {13, 28, 2}, {15, 30, 7}};
    static const V number_views[] = {{"chars", 37, 2}, {"offset", 39, 2}, {"line_no", 41, 2}};
    static const V text_views[] = {{"line", 43, 2}, {"header", 45, 9}, {"ratio", 54, 4}};
    Spec spec = {nums, 1, inputs, LEN(inputs), number_views, 3, handlers, LEN(handlers), code, LEN(code),
                 NULL, 0, textual, 3, constants, LEN(constants), text_views, 3, NULL, 0, NULL, 0};
    text_spec = spec;
    if (load(spec, &module_a) != PURE_OK) { fprintf(stderr, "%s\n", error); abort(); }
    PureApp app;
    CHECK(pure_app_start(&app, &module_a, (PureData){0}) == PURE_OK);
    CHECK(!strcmp(state_text(&app, 1), "\xea\xb0\x80" "a\nbc") && app.values[2] == 0);
    for (int i = 0; i < 6; ++i) CHECK(pure_app_input(&app, 2, PURE_EDGE_DOWN) == PURE_OK);
    CHECK(app.values[2] == 7);                               /* stops at the end */
    CHECK(number_view(&app, 0) == 5 && number_view(&app, 2) == 2 && !strcmp(view_text(&app, 3), "bc"));
    CHECK(pure_app_input(&app, 3, PURE_EDGE_DOWN) == PURE_OK && app.values[2] == 4);   /* up keeps column 2 */
    const char *han = "\xed\x95\x9c";
    CHECK(pure_app_text(&app, 0, (PureSpan){(const unsigned char *)han, 3}) == PURE_OK);
    CHECK(!strcmp(state_text(&app, 1), "\xea\xb0\x80" "a\xed\x95\x9c\nbc") && app.values[2] == 7 && app.values[0] == 1);
    CHECK(pure_app_input(&app, 5, PURE_EDGE_DOWN) == PURE_OK && app.values[2] == 4);   /* back over 3 bytes */
    CHECK(pure_app_input(&app, 5, PURE_EDGE_DOWN) == PURE_OK && app.values[2] == 3);
    CHECK(!strcmp(state_text(&app, 1), "\xea\xb0\x80\nbc"));
    CHECK(pure_app_input(&app, 1, PURE_EDGE_DOWN) == PURE_OK && app.values[2] == 0);
    CHECK(pure_app_input(&app, 1, PURE_EDGE_DOWN) == PURE_OK && app.values[2] == 0);   /* stops at the start */
    CHECK(pure_app_input(&app, 5, PURE_EDGE_DOWN) == PURE_OK && app.values[2] == 0);   /* nothing before */
    CHECK(pure_app_input(&app, 4, PURE_EDGE_DOWN) == PURE_OK && app.values[2] == 4);   /* down to column 0 */
    /* A failing event restores the text, the cursor and the states. */
    CHECK(pure_app_input(&app, 7, PURE_EDGE_DOWN) == PURE_INVALID && app.fault == PURE_FAULT_DIVIDE);
    CHECK(!strcmp(state_text(&app, 1), "\xea\xb0\x80\nbc") && app.values[2] == 4 && app.values[0] == 1);
    CHECK(!strcmp(view_text(&app, 4), "\xeb\xa9\x94\xeb\xaa\xa8 \xc2\xb7 4\xec\x9e\x90"));
    CHECK(!strcmp(view_text(&app, 5), "0.3333333333333333"));
    CHECK(pure_app_text(&app, 0, (PureSpan){(const unsigned char *)"\xff", 1}) == PURE_INVALID);
    CHECK(pure_app_text(&app, 0, (PureSpan){(const unsigned char *)"\xed\xa0\x80", 3}) == PURE_INVALID); /* surrogate */
    CHECK(pure_app_text(&app, 0, (PureSpan){(const unsigned char *)"\xf0\x9f\x98\x80", 4}) == PURE_OK);
    CHECK(app.values[2] == 8 && number_view(&app, 1) == 3);   /* offset in characters */
    CHECK(pure_app_input(&app, 1, PURE_EDGE_DOWN) == PURE_OK && app.values[2] == 4);  /* back over the emoji */
    CHECK(pure_app_input(&app, 6, PURE_EDGE_DOWN) == PURE_OK && app.values[2] == 0);
    /* Replacement keeps the text and its cursor by name. */
    static const S fresh[] = {{"fresh", 7}};
    static const T kept[] = {{"note", 1, 0}, {"at", 2, 1}};
    static const char *const other[] = {"ignored"};
    Spec next = {fresh, 1, inputs, 0, number_views, 0, handlers, 0, code, 0, NULL, 0, kept, 2, other, 1, text_views, 0, NULL, 0, NULL, 0};
    CHECK(load(next, &module_b) == PURE_OK);
    CHECK(pure_app_input(&app, 2, PURE_EDGE_DOWN) == PURE_OK && app.values[2] == 3);
    CHECK(pure_app_replace(&app, &module_b) == PURE_OK);
    CHECK(app.values[0] == 7 && app.values[2] == 3 && !strcmp(state_text(&app, 1), "\xea\xb0\x80\n\xf0\x9f\x98\x80" "bc"));
    pure_app_stop(&app);

    /* Verification of text operations. */
    static const T two[] = {{"one", 1, 0}, {"two", 1, 0}, {"c", 2, 1}};
    static const O wrong_text[] = {{CUL, 3, 0}, {TK, 0, 0}, {TINS, 2, 0}};
    static const H frame[] = {{0, 0, 3}};
    Spec bad = {nums, 1, inputs, 0, number_views, 0, frame, 1, wrong_text, 3, NULL, 0, two, 3, constants, 1, text_views, 0, NULL, 0, NULL, 0};
    CHECK(load(bad, &module_b) == PURE_INVALID && strstr(error, "cursor of another text"));
    static const O input_in_frame[] = {{CUL, 3, 0}, {TI, 0, 0}, {TINS, 1, 0}};
    bad.o = input_in_frame;
    CHECK(load(bad, &module_b) == PURE_INVALID && strstr(error, "text input outside"));
    static const O store_text[] = {{TK, 0, 0}, {ST, 0, 0}};
    static const H frame2[] = {{0, 0, 2}};
    bad.o = store_text; bad.no = 2; bad.h = frame2;
    CHECK(load(bad, &module_b) == PURE_INVALID && strstr(error, "operand type"));
    static const O load_text[] = {{L, 1, 0}, {ST, 0, 0}};
    bad.o = load_text;
    CHECK(load(bad, &module_b) == PURE_INVALID && strstr(error, "no such state"));
    static const V text_as_number[] = {{"v", 0, 1}};
    static const O constant_only[] = {{TK, 0, 0}};
    bad = (Spec){nums, 1, inputs, 0, text_as_number, 1, NULL, 0, constant_only, 1, NULL, 0, two, 3, constants, 1, NULL, 0, NULL, 0, NULL, 0};
    CHECK(load(bad, &module_b) == PURE_INVALID && strstr(error, "view must leave one value of its type"));
    static const T cursor_on_number[] = {{"c", 2, 0}};
    bad = (Spec){nums, 1, inputs, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0, cursor_on_number, 1, constants, 1, NULL, 0, NULL, 0, NULL, 0};
    CHECK(load(bad, &module_b) == PURE_INVALID && strstr(error, "state reference"));
    static const char *const broken[] = {"\xc0\xaf"};
    bad = (Spec){nums, 1, inputs, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0, broken, 1, NULL, 0, NULL, 0, NULL, 0};
    CHECK(load(bad, &module_b) == PURE_INVALID && strstr(error, "text constant"));
}

static bool ignore_span(void *context, PureSpan span) {
    (void)context;
    CHECK(pure_utf8_valid(span) || span.size);
    return true;
}

/* Damaged text modules either fail to load or edit text safely: every cursor
 * stays within its text and every text stays valid UTF-8. */
static void text_mutations(void) {
    static unsigned char valid[65536], bytes[65536];
    size_t size = build(valid, text_spec);
    static const char *const words[] = {"a", "\xea\xb0\x80", "\n", "\xf0\x9f\x98\x80", "xy\nz", ""};
    uint32_t state = 0x1b873593u;
    unsigned accepted = 0;
    for (unsigned round = 0; round < 20000; ++round) {
        memcpy(bytes, valid, size);
        unsigned flips = 1 + next_random(&state) % 3;
        for (unsigned f = 0; f < flips; ++f)
            bytes[next_random(&state) % size] ^= (unsigned char)(1u << (next_random(&state) % 8));
        if (pure_module_load(bytes, size, &module_b, error, sizeof(error)) != PURE_OK) continue;
        ++accepted;
        PureApp app;
        if (pure_app_start(&app, &module_b, (PureData){0}) != PURE_OK) continue;
        for (unsigned e = 0; e < 16; ++e) {
            uint32_t input = next_random(&state) % 8;
            if (module_b.input[input].kind == PURE_KIND_TEXT) {
                const char *word = words[next_random(&state) % LEN(words)];
                pure_app_text(&app, input, (PureSpan){(const unsigned char *)word, strlen(word)});
            } else if (module_b.input[input].kind == PURE_KIND_KEY) pure_app_input(&app, input, PURE_EDGE_DOWN);
            else pure_app_frame(&app);
            for (uint32_t v = 0; v < module_b.views; ++v) {
                double value;
                if (module_b.view_type[v] == PURE_VIEW_TEXT) pure_app_view_text(&app, v, ignore_span, NULL);
                else pure_app_view(&app, v, &value);
            }
            for (uint32_t s = 0; s < module_b.states; ++s) {
                if (module_b.state_type[s] == PURE_STATE_CURSOR) {
                    PureDraft *text = app.texts[module_b.state_ref[s]];
                    size_t length = pure_draft_size(text), at = (size_t)app.values[s];
                    CHECK(app.values[s] >= 0 && at <= length && app.values[s] == (double)at);
                    if (at < length) { /* never inside a multi-byte character */
                        Text t = {{0}, 0};
                        pure_draft_read(text, at, 1, text_span, &t);
                        CHECK(((unsigned char)t.text[0] & 0xC0) != 0x80);
                    }
                } else if (module_b.state_type[s] == PURE_STATE_TEXT) {
                    unsigned utf8 = 0;
                    bool ok = true;
                    Text t = {{0}, 0};
                    if (pure_draft_size(app.texts[s]) < 200) {
                        pure_draft_read(app.texts[s], 0, pure_draft_size(app.texts[s]), text_span, &t);
                        ok = pure_utf8_step(&utf8, (PureSpan){(const unsigned char *)t.text, t.size}) &&
                             pure_utf8_complete(utf8);
                    }
                    CHECK(ok);
                } else CHECK(isfinite(app.values[s]));
            }
        }
        pure_app_stop(&app);
    }
    printf("{\"text_module_mutations\":20000,\"accepted_after_damage\":%u}\n", accepted);
}

static Spec list_spec;

enum { LC = PURE_OP_LIST_COUNT, LA = PURE_OP_LIST_APPEND, LI = PURE_OP_LIST_INSERT, LR = PURE_OP_LIST_REMOVE,
       IG = PURE_OP_ITEM_GET, IS = PURE_OP_ITEM_SET, XL = PURE_OP_INDEX_LOAD, XS = PURE_OP_INDEX_STORE,
       XM = PURE_OP_INDEX_MOVE, XF = PURE_OP_INDEX_FIRST, XE = PURE_OP_INDEX_LAST, XN = PURE_OP_INDEX_NUM,
       NOT = PURE_OP_NOT, TITLE = 1, DONE = 1 | 1u << 16 };

static const char *item_title(const PureApp *app, uint32_t list, size_t at) {
    static char title[64];
    PureCell cell = pure_list_get(app->lists[list], at, 0);
    CHECK(cell.size < sizeof(title));
    if (cell.size) memcpy(title, cell.text, cell.size);
    title[cell.size] = 0;
    return title;
}

/* A task list: made (num), tasks (list of task {title, done}), at (index of
 * tasks). Indices follow their item across inserts and removes; a failed
 * event undoes every list change. */
static void lists(void) {
    static const S nums[] = {{"made", 0}};
    static const T states[] = {{"tasks", 3, 0}, {"at", 4, 1}};
    static const RF task_fields[] = {{"title", 1, 0}, {"done", 0, 0}};
    static const R records[] = {{"task", task_fields, 2}};
    static const char *const constants[] = {"", "new", "x"};
    static const I inputs[] = {
        {"typed", NULL, PURE_KIND_TEXT, false, NULL, 0}, KEY("ins", "KeyI"), KEY("del", "Delete"),
        KEY("up", "ArrowUp"), KEY("down", "ArrowDown"), KEY("toggle", "Space"), KEY("rename", "KeyR"), KEY("bad", "KeyB"),
    };
    static const O code[] = {
        {TI, 0, 0}, {C, 0, 0}, {LA, 1, 0}, {XE, 1, 0}, {XS, 2, 0}, {L, 0, 0}, {C, 0, 1}, {ADD, 0, 0}, {ST, 0, 0}, /* 0 typed */
        {XL, 2, 0}, {TK, 1, 0}, {C, 0, 0}, {LI, 1, 0},                                           /* 9 ins */
        {XL, 2, 0}, {LR, 1, 0},                                                                  /* 13 del */
        {XL, 2, 0}, {C, 0, -1}, {XM, 0, 0}, {XS, 2, 0},                                          /* 15 up */
        {XL, 2, 0}, {C, 0, 1}, {XM, 0, 0}, {XS, 2, 0},                                           /* 19 down */
        {XL, 2, 0}, {XL, 2, 0}, {IG, DONE, 0}, {NOT, 0, 0}, {IS, DONE, 0},                         /* 23 toggle */
        {XL, 2, 0}, {XL, 2, 0}, {IG, TITLE, 0}, {TK, 2, 0}, {CAT, 0, 0}, {IS, TITLE, 0},           /* 28 rename */
        {XL, 2, 0}, {TK, 2, 0}, {IS, TITLE, 0}, {XL, 2, 0}, {LR, 1, 0}, {TK, 1, 0}, {C, 0, 0},     /* 34 bad */
        {LA, 1, 0}, {C, 0, 1}, {C, 0, 0}, {DIV, 0, 0}, {ST, 0, 0},
        {LC, 1, 0},                                                                              /* 46 view count */
        {XL, 2, 0}, {XN, 0, 0},                                                                  /* 47 view position */
        {XL, 2, 0}, {IG, DONE, 0},                                                               /* 49 view done */
        {XL, 2, 0}, {IG, TITLE, 0},                                                              /* 51 view current */
    };
    static const H handlers[] = {{1, 0, 9}, {3, 9, 4}, {5, 13, 2}, {7, 15, 4}, {9, 19, 4}, {11, 23, 5},
                                 {13, 28, 6}, {15, 34, 12}};
    static const V number_views[] = {{"count", 46, 1}, {"position", 47, 2}, {"done", 49, 2}};
    static const V text_views[] = {{"current", 51, 2}};
    Spec spec = {nums, 1, inputs, LEN(inputs), number_views, 3, handlers, LEN(handlers), code, LEN(code),
                 NULL, 0, states, 2, constants, 3, text_views, 1, records, 1, NULL, 0};
    list_spec = spec;
    if (load(spec, &module_a) != PURE_OK) { fprintf(stderr, "%s\n", error); abort(); }
    PureApp app;
    CHECK(pure_app_start(&app, &module_a, (PureData){0}) == PURE_OK);
    double value;
    CHECK(number_view(&app, 0) == 0 && number_view(&app, 1) == 0 && !pure_app_view(&app, 2, &value));
    CHECK(pure_app_view_text(&app, 3, text_span, &(Text){{0}, 0}) == PURE_NOT_FOUND);  /* no item yet */
    CHECK(app.fault == PURE_FAULT_NONE);                                                /* views leave faults alone */
    CHECK(pure_app_text(&app, 0, (PureSpan){(const unsigned char *)"a", 1}) == PURE_OK);
    CHECK(pure_app_text(&app, 0, (PureSpan){(const unsigned char *)"b", 1}) == PURE_OK);
    CHECK(pure_list_count(app.lists[1]) == 2 && app.values[2] == 1 && app.values[0] == 2);
    CHECK(pure_app_input(&app, 3, PURE_EDGE_DOWN) == PURE_OK && app.values[2] == 0);
    CHECK(pure_app_input(&app, 3, PURE_EDGE_DOWN) == PURE_OK && app.values[2] == 0);   /* stops at the first */
    /* Inserting before the current item: the index follows it. */
    CHECK(pure_app_input(&app, 1, PURE_EDGE_DOWN) == PURE_OK && app.values[2] == 1);
    CHECK(!strcmp(item_title(&app, 1, 0), "new") && !strcmp(view_text(&app, 3), "a"));
    CHECK(pure_app_input(&app, 5, PURE_EDGE_DOWN) == PURE_OK && number_view(&app, 2) == 1);
    CHECK(pure_app_input(&app, 6, PURE_EDGE_DOWN) == PURE_OK && !strcmp(view_text(&app, 3), "ax"));
    CHECK(pure_app_input(&app, 4, PURE_EDGE_DOWN) == PURE_OK && app.values[2] == 2);
    CHECK(pure_app_input(&app, 4, PURE_EDGE_DOWN) == PURE_OK && app.values[2] == 2);   /* stops at the last */
    /* Removing the last item: the index moves to the new last. */
    CHECK(pure_app_input(&app, 2, PURE_EDGE_DOWN) == PURE_OK && app.values[2] == 1 && pure_list_count(app.lists[1]) == 2);
    CHECK(pure_app_input(&app, 3, PURE_EDGE_DOWN) == PURE_OK && app.values[2] == 0);
    /* Removing the current item: the index names the next. */
    CHECK(pure_app_input(&app, 2, PURE_EDGE_DOWN) == PURE_OK && app.values[2] == 0 && !strcmp(view_text(&app, 3), "ax"));
    /* A failing event undoes a set, a remove and an append. */
    CHECK(pure_app_input(&app, 7, PURE_EDGE_DOWN) == PURE_INVALID && app.fault == PURE_FAULT_DIVIDE);
    CHECK(pure_list_count(app.lists[1]) == 1 && !strcmp(item_title(&app, 1, 0), "ax") &&
          pure_list_get(app.lists[1], 0, 1).num == 1 && app.values[2] == 0 && app.values[0] == 2);
    CHECK(pure_app_input(&app, 2, PURE_EDGE_DOWN) == PURE_OK && pure_list_count(app.lists[1]) == 0 && app.values[2] == 0);
    CHECK(pure_app_input(&app, 2, PURE_EDGE_DOWN) == PURE_NOT_FOUND && app.fault == PURE_FAULT_NOT_FOUND);
    CHECK(pure_app_input(&app, 5, PURE_EDGE_DOWN) == PURE_NOT_FOUND);
    CHECK(pure_app_input(&app, 1, PURE_EDGE_DOWN) == PURE_OK && pure_list_count(app.lists[1]) == 1 && app.values[2] == 0);
    for (int i = 0; i < 3; ++i) CHECK(pure_app_text(&app, 0, (PureSpan){(const unsigned char *)"\xea\xb0\x80", 3}) == PURE_OK);
    CHECK(pure_app_input(&app, 3, PURE_EDGE_DOWN) == PURE_OK && app.values[2] == 2);
    CHECK(pure_app_input(&app, 5, PURE_EDGE_DOWN) == PURE_OK);

    /* Replacement keeps the list by name. The same record keeps the list
     * itself; a changed record keeps each field by name and kind and gives new
     * fields their default. */
    static const S no_nums[] = {{"made", 0}};
    Spec same = {no_nums, 1, inputs, 0, number_views, 0, handlers, 0, code, 0, NULL, 0, states, 2, constants, 3,
                 text_views, 0, records, 1, NULL, 0};
    CHECK(load(same, &module_b) == PURE_OK);
    const PureList *before = app.lists[1];
    CHECK(pure_app_replace(&app, &module_b) == PURE_OK && app.lists[1] == before && app.values[2] == 2);
    static const RF changed_fields[] = {{"done", 0, 0}, {"note", 1, 1}, {"title", 0, 7}, {"rank", 0, 5}};
    static const R changed[] = {{"task", changed_fields, 4}};
    Spec next = same;
    next.r = changed;
    CHECK(load(next, &module_a) == PURE_OK);
    CHECK(pure_app_replace(&app, &module_a) == PURE_OK && pure_list_count(app.lists[1]) == 4 && app.values[2] == 2);
    PureCell cell = pure_list_get(app.lists[1], 2, 0);
    CHECK(cell.num == 1);                                           /* done kept by name */
    cell = pure_list_get(app.lists[1], 2, 1);
    CHECK(cell.size == 3 && !memcmp(cell.text, "new", 3));          /* note: its default */
    CHECK(pure_list_get(app.lists[1], 2, 2).num == 7);              /* title is now a number: default */
    CHECK(pure_list_get(app.lists[1], 0, 3).num == 5);
    /* A state whose kind changed starts again. */
    static const T swapped_kinds[] = {{"tasks", 1, 0}, {"at", 3, 0}};
    next.t = swapped_kinds;
    CHECK(load(next, &module_b) == PURE_OK);
    CHECK(pure_app_replace(&app, &module_b) == PURE_OK && !app.lists[1] && pure_draft_size(app.texts[1]) == 0 &&
          pure_list_count(app.lists[2]) == 0);
    pure_app_stop(&app);

    /* Verification of list operations. */
    static const T two_lists[] = {{"a", 3, 0}, {"b", 3, 0}, {"ia", 4, 1}};
    static const O other_index[] = {{XL, 3, 0}, {LR, 2, 0}};
    static const H frame[] = {{0, 0, 2}};
    Spec bad = {nums, 1, inputs, 0, number_views, 0, frame, 1, other_index, 2, NULL, 0, two_lists, 3, constants, 3,
                text_views, 0, records, 1, NULL, 0};
    CHECK(load(bad, &module_b) == PURE_INVALID && strstr(error, "index of another list"));
    static const O swapped[] = {{C, 0, 0}, {TK, 0, 0}, {LA, 1, 0}};
    static const H frame3[] = {{0, 0, 3}};
    bad.o = swapped; bad.no = 3; bad.h = frame3;
    CHECK(load(bad, &module_b) == PURE_INVALID && strstr(error, "operand type"));
    static const O short_record[] = {{TK, 0, 0}, {LA, 1, 0}};
    bad.o = short_record; bad.no = 2; bad.h = frame;
    CHECK(load(bad, &module_b) == PURE_INVALID && strstr(error, "stack underflow"));
    static const O no_field[] = {{XL, 3, 0}, {IG, 1 | 2u << 16, 0}, {ST, 0, 0}};
    bad.o = no_field; bad.no = 3; bad.h = frame3;
    CHECK(load(bad, &module_b) == PURE_INVALID && strstr(error, "no such state"));
    static const O index_as_number[] = {{XL, 3, 0}, {ST, 0, 0}};
    bad.o = index_as_number; bad.no = 2; bad.h = frame;
    CHECK(load(bad, &module_b) == PURE_INVALID && strstr(error, "operand type"));
    static const V append_view[] = {{"v", 0, 3}};
    bad = (Spec){nums, 1, inputs, 0, append_view, 1, NULL, 0, (O[]){{TK, 0, 0}, {C, 0, 0}, {LA, 1, 0}}, 3, NULL, 0,
                 two_lists, 3, constants, 3, NULL, 0, records, 1, NULL, 0};
    CHECK(load(bad, &module_b) == PURE_INVALID && strstr(error, "view with an effect"));
    static const T index_of_number[] = {{"ia", 4, 0}};
    bad = (Spec){nums, 1, inputs, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0, index_of_number, 1, constants, 3, NULL, 0,
                 records, 1, NULL, 0};
    CHECK(load(bad, &module_b) == PURE_INVALID && strstr(error, "state reference"));
    static const T list_of_nothing[] = {{"a", 3, 1}};
    bad.t = list_of_nothing;
    CHECK(load(bad, &module_b) == PURE_INVALID && strstr(error, "state reference"));
    bad.t = NULL; bad.nt = 0;
    static const RF repeated[] = {{"x", 0, 0}, {"x", 1, 0}};
    bad.r = (R[]){{"task", repeated, 2}};
    CHECK(load(bad, &module_b) == PURE_INVALID && strstr(error, "duplicate record field"));
    static const RF far_default[] = {{"x", 1, 3}};
    bad.r = (R[]){{"task", far_default, 1}};
    CHECK(load(bad, &module_b) == PURE_INVALID && strstr(error, "record field default"));
    static const RF infinite[] = {{"x", 0, INFINITY}};
    bad.r = (R[]){{"task", infinite, 1}};
    CHECK(load(bad, &module_b) == PURE_INVALID && strstr(error, "record field default"));
    bad.r = (R[]){{"task", NULL, 0}};
    CHECK(load(bad, &module_b) == PURE_INVALID && strstr(error, "record"));
    bad.r = (R[]){{"made", task_fields, 2}};
    CHECK(load(bad, &module_b) == PURE_INVALID && strstr(error, "duplicate name"));
}

/* Damaged list modules either fail to load or keep every index on an item
 * of its list and every text field valid UTF-8. */
static void list_mutations(void) {
    static unsigned char valid[65536], bytes[65536];
    size_t size = build(valid, list_spec);
    static const char *const words[] = {"a", "\xea\xb0\x80", "\n", ""};
    uint32_t state = 0x85ebca6bu;
    unsigned accepted = 0;
    for (unsigned round = 0; round < 20000; ++round) {
        memcpy(bytes, valid, size);
        unsigned flips = 1 + next_random(&state) % 3;
        for (unsigned f = 0; f < flips; ++f)
            bytes[next_random(&state) % size] ^= (unsigned char)(1u << (next_random(&state) % 8));
        if (pure_module_load(bytes, size, &module_b, error, sizeof(error)) != PURE_OK) continue;
        ++accepted;
        PureApp app;
        if (pure_app_start(&app, &module_b, (PureData){0}) != PURE_OK) continue;
        for (unsigned e = 0; e < 24; ++e) {
            uint32_t input = next_random(&state) % 8;
            if (module_b.input[input].kind == PURE_KIND_TEXT) {
                const char *word = words[next_random(&state) % LEN(words)];
                pure_app_text(&app, input, (PureSpan){(const unsigned char *)word, strlen(word)});
            } else if (module_b.input[input].kind == PURE_KIND_KEY) pure_app_input(&app, input, PURE_EDGE_DOWN);
            else pure_app_frame(&app);
            for (uint32_t v = 0; v < module_b.views; ++v) {
                double value;
                if (module_b.view_type[v] == PURE_VIEW_TEXT) pure_app_view_text(&app, v, ignore_span, NULL);
                else pure_app_view(&app, v, &value);
            }
            for (uint32_t s = 0; s < module_b.states; ++s) {
                if (module_b.state_type[s] == PURE_STATE_INDEX) {
                    size_t count = pure_list_count(app.lists[module_b.state_ref[s]]);
                    CHECK(app.values[s] == floor(app.values[s]) && app.values[s] >= 0 &&
                          app.values[s] <= (count ? (double)(count - 1) : 0));
                } else if (module_b.state_type[s] == PURE_STATE_LIST) {
                    const PureRecord *record = &module_b.record[module_b.state_ref[s]];
                    for (size_t at = 0; at < pure_list_count(app.lists[s]); ++at)
                        for (unsigned f = 0; f < record->count; ++f) {
                            PureCell cell = pure_list_get(app.lists[s], at, f);
                            if (module_b.record_field_text[record->first + f])
                                CHECK(pure_utf8_valid((PureSpan){cell.text, cell.size}));
                            else CHECK(isfinite(cell.num));
                        }
                } else if (module_b.state_type[s] == PURE_STATE_NUM) CHECK(isfinite(app.values[s]));
            }
        }
        pure_app_stop(&app);
    }
    printf("{\"list_module_mutations\":20000,\"accepted_after_damage\":%u}\n", accepted);
}

static Spec table_spec;

static const char *cell_text(PureApp *app, uint32_t column, size_t row) {
    static Text t;
    t.size = 0;
    t.text[0] = 0;
    CHECK(pure_app_cell_text(app, column, row, text_span, &t) == PURE_OK);
    return t.text;
}

enum { ROW = PURE_OP_ROW, EQ = PURE_OP_EQ, LT = PURE_OP_LT, PICK = PURE_OP_PICK, SLIDE = PURE_OP_SLIDE };

/* A table view has a row per item; its columns read the row and may branch.
 * A value crosses a join when every path brings the same stack. */
static void tables(void) {
    static const S nums[] = {{"n", 0}};
    static const T states[] = {{"tasks", 3, 0}, {"at", 4, 1}};
    static const RF task_fields[] = {{"title", 1, 0}, {"done", 0, 0}};
    static const R records[] = {{"task", task_fields, 2}};
    static const char *const constants[] = {"", "x", " ", "empty"};
    static const I inputs[] = {{"typed", NULL, PURE_KIND_TEXT, false, NULL, 0}, KEY("toggle", "Space")};
    static const O code[] = {
        {TI, 0, 0}, {C, 0, 0}, {LA, 1, 0}, {XE, 1, 0}, {XS, 2, 0},                          /* 0 typed */
        {XL, 2, 0}, {XL, 2, 0}, {IG, DONE, 0}, {NOT, 0, 0}, {IS, DONE, 0},                     /* 5 toggle */
        {LC, 1, 0}, {C, 0, 0}, {GT, 0, 0}, {JZ, 17, 0}, {XL, 2, 0}, {IG, TITLE, 0}, {JMP, 18, 0},
        {TK, 3, 0},                                                                          /* 10 current */
        {ROW, 1, 0}, {IG, TITLE, 0},                                                         /* 18 title */
        {ROW, 1, 0}, {IG, DONE, 0}, {JZ, 25, 0}, {TK, 1, 0}, {JMP, 26, 0}, {TK, 2, 0},         /* 20 mark */
        {ROW, 1, 0}, {XN, 0, 0},                                                             /* 26 pos */
        {ROW, 1, 0}, {XN, 0, 0}, {XL, 2, 0}, {XN, 0, 0}, {EQ, 0, 0},                           /* 28 here */
    };
    static const H handlers[] = {{1, 0, 5}, {3, 5, 5}};
    static const V text_views[] = {{"current", 10, 8}};
    static const W table[] = {{"board", 2, 0, 1, 0}, {"title", 1, 2, 18, 2}, {"mark", 1, 2, 20, 6},
                              {"pos", 0, 2, 26, 2}, {"here", 0, 2, 28, 5}};
    Spec spec = {nums, 1, inputs, 2, NULL, 0, handlers, 2, code, LEN(code), NULL, 0, states, 2, constants, 4,
                 text_views, 1, records, 1, table, 5};
    table_spec = spec;
    if (load(spec, &module_a) != PURE_OK) { fprintf(stderr, "%s\n", error); abort(); }
    CHECK(pure_module_view(&module_a, "board") == 1 && pure_module_view(&module_a, "title") == -1 &&
          pure_module_column(&module_a, 1, "title") == 2 && pure_module_column(&module_a, 1, "current") == -1);
    PureApp app;
    CHECK(pure_app_start(&app, &module_a, (PureData){0}) == PURE_OK);
    double value;
    CHECK(pure_app_rows(&app, 1) == 0 && !strcmp(view_text(&app, 0), "empty"));   /* the else branch */
    CHECK(pure_app_cell_text(&app, 2, 0, text_span, &(Text){{0}, 0}) == PURE_NOT_FOUND);
    CHECK(pure_app_text(&app, 0, (PureSpan){(const unsigned char *)"a", 1}) == PURE_OK);
    CHECK(pure_app_text(&app, 0, (PureSpan){(const unsigned char *)"\xeb\xb9\xb5", 3}) == PURE_OK);
    CHECK(pure_app_input(&app, 1, PURE_EDGE_DOWN) == PURE_OK);
    CHECK(pure_app_rows(&app, 1) == 2 && !strcmp(view_text(&app, 0), "\xeb\xb9\xb5"));
    CHECK(!strcmp(cell_text(&app, 2, 0), "a") && !strcmp(cell_text(&app, 2, 1), "\xeb\xb9\xb5"));
    CHECK(!strcmp(cell_text(&app, 3, 0), " ") && !strcmp(cell_text(&app, 3, 1), "x"));
    CHECK(pure_app_cell(&app, 4, 1, &value) && value == 1 && pure_app_cell(&app, 5, 0, &value) && value == 0 &&
          pure_app_cell(&app, 5, 1, &value) && value == 1);
    CHECK(!pure_app_cell(&app, 4, 2, &value) && pure_app_cell_text(&app, 2, 2, text_span, &(Text){{0}, 0}) == PURE_NOT_FOUND);
    /* Columns are read by row, other views without one. */
    CHECK(!pure_app_view(&app, 4, &value) && pure_app_view_text(&app, 2, text_span, &(Text){{0}, 0}) == PURE_INVALID);
    CHECK(pure_app_cell_text(&app, 0, 0, text_span, &(Text){{0}, 0}) == PURE_INVALID && pure_app_rows(&app, 0) == 0);
    CHECK(pure_app_view_text(&app, 1, text_span, &(Text){{0}, 0}) == PURE_INVALID);
    pure_app_stop(&app);

    /* Verification of rows, tables and joins. */
    Spec bad = spec;
    static const V row_view[] = {{"current", 18, 2}};
    bad.tv = row_view;
    CHECK(load(bad, &module_b) == PURE_INVALID && strstr(error, "row outside a column"));
    static const H row_handler[] = {{3, 26, 2}};
    bad = spec; bad.h = row_handler; bad.nh = 1;
    CHECK(load(bad, &module_b) == PURE_INVALID && strstr(error, "row outside a column"));
    static const W twice[] = {{"board", 2, 0, 1, 0}, {"title", 1, 2, 18, 2}, {"title", 1, 2, 18, 2}};
    bad = spec; bad.w = twice; bad.nw = 3;
    CHECK(load(bad, &module_b) == PURE_INVALID && strstr(error, "duplicate column"));
    static const W two_tables[] = {{"board", 2, 0, 1, 0}, {"title", 1, 2, 18, 2}, {"other", 2, 0, 1, 0},
                                   {"title", 1, 4, 18, 2}, {"tasks", 0, 4, 26, 2}};
    bad = spec; bad.w = two_tables; bad.nw = 5;
    CHECK(load(bad, &module_b) == PURE_OK);   /* column names belong to their table */
    static const W not_a_table[] = {{"title", 1, 1, 18, 2}};
    bad = spec; bad.w = not_a_table; bad.nw = 1;
    CHECK(load(bad, &module_b) == PURE_INVALID && strstr(error, "view type"));
    static const W over_number[] = {{"board", 2, 0, 0, 0}};
    bad = spec; bad.w = over_number; bad.nw = 1;
    CHECK(load(bad, &module_b) == PURE_INVALID && strstr(error, "table list"));
    static const O other_row[] = {{ROW, 2, 0}, {XN, 0, 0}};
    static const W other_row_column[] = {{"board", 2, 0, 1, 0}, {"pos", 0, 1, 0, 2}};
    bad = spec; bad.o = other_row; bad.no = 2; bad.h = NULL; bad.nh = 0; bad.tv = NULL; bad.ntv = 0;
    bad.w = other_row_column; bad.nw = 2;
    CHECK(load(bad, &module_b) == PURE_INVALID && strstr(error, "no such state"));
    static const O typed_join[] = {{L, 0, 0}, {JZ, 4, 0}, {TK, 0, 0}, {JMP, 5, 0}, {C, 0, 1}};
    static const V mixed[] = {{"current", 0, 5}};
    bad = spec; bad.o = typed_join; bad.no = 5; bad.h = NULL; bad.nh = 0; bad.tv = mixed; bad.w = NULL; bad.nw = 0;
    CHECK(load(bad, &module_b) == PURE_INVALID && strstr(error, "join with different stacks"));
    /* At most 8 joins with a value may be ahead at once. */
    for (unsigned open = 8; open <= 9; ++open) {
        static O nested[1 + 2 * 9 + 9 + 1];
        unsigned n = 0;
        nested[n++] = (O){C, 0, 1};
        for (unsigned k = 0; k < open; ++k) { nested[n++] = (O){C, 0, 0}; nested[n++] = (O){JZ, 1 + 2 * open + k, 0}; }
        for (unsigned k = 0; k < open; ++k) nested[n++] = (O){NEG, 0, 0};
        nested[n++] = (O){ST, 0, 0};
        H frame = {0, 0, n};
        bad = spec; bad.o = nested; bad.no = n; bad.h = &frame; bad.nh = 1; bad.tv = NULL; bad.ntv = 0; bad.w = NULL; bad.nw = 0;
        CHECK(load(bad, &module_b) == (open == 8 ? PURE_OK : PURE_INVALID));
    }
    CHECK(strstr(error, "more than 8 joins ahead"));

    /* PICK copies a value from below, SLIDE drops values under the top: a
     * value used twice is computed once. (3 + 4) * (3 + 4) with a text between. */
    static const O picked[] = {{C, 0, 3}, {C, 0, 4}, {ADD, 0, 0}, {TK, 1, 0}, {PICK, 1, 0}, {PICK, 2, 0}, {MUL, 0, 0},
                               {SLIDE, 2, 0}};
    static const V squared[] = {{"sq", 0, 8}};
    bad = (Spec){nums, 1, inputs, 0, squared, 1, NULL, 0, picked, 8, NULL, 0, NULL, 0, constants, 2, NULL, 0, NULL, 0, NULL, 0};
    CHECK(load(bad, &module_b) == PURE_OK);
    CHECK(pure_app_start(&app, &module_b, (PureData){0}) == PURE_OK && number_view(&app, 0) == 49);
    pure_app_stop(&app);
    static const O pick_text[] = {{TK, 1, 0}, {C, 0, 1}, {PICK, 1, 0}, {SLIDE, 2, 0}};
    static const V text_left[] = {{"t", 0, 4}};
    bad.v = NULL; bad.nv = 0; bad.tv = text_left; bad.ntv = 1; bad.o = pick_text; bad.no = 4;
    CHECK(load(bad, &module_b) == PURE_OK);
    CHECK(pure_app_start(&app, &module_b, (PureData){0}) == PURE_OK && !strcmp(view_text(&app, 0), "x"));
    pure_app_stop(&app);
    static const O pick_type[] = {{TK, 1, 0}, {PICK, 0, 0}, {C, 0, 1}, {ADD, 0, 0}, {SLIDE, 1, 0}};
    static const V five[] = {{"sq", 0, 5}}, three[] = {{"sq", 0, 3}};
    bad.o = pick_type; bad.no = 5; bad.tv = NULL; bad.ntv = 0; bad.v = five; bad.nv = 1;
    CHECK(load(bad, &module_b) == PURE_INVALID && strstr(error, "operand type"));   /* a copy keeps its type */
    static const O pick_far[] = {{C, 0, 1}, {PICK, 1, 0}, {SLIDE, 1, 0}};
    bad.o = pick_far; bad.no = 3; bad.v = three;
    CHECK(load(bad, &module_b) == PURE_INVALID && strstr(error, "stack underflow"));
    static const O slide_all[] = {{C, 0, 1}, {C, 0, 2}, {SLIDE, 2, 0}};
    bad.o = slide_all;
    CHECK(load(bad, &module_b) == PURE_INVALID && strstr(error, "stack underflow"));
    static const O slide_none[] = {{C, 0, 1}, {C, 0, 2}, {SLIDE, 0, 0}};
    bad.o = slide_none;
    CHECK(load(bad, &module_b) == PURE_INVALID && strstr(error, "stack underflow"));
}

/* Damaged table modules either fail to load or evaluate every cell safely. */
static void table_mutations(void) {
    static unsigned char valid[65536], bytes[65536];
    size_t size = build(valid, table_spec);
    uint32_t state = 0xc2b2ae35u;
    unsigned accepted = 0;
    for (unsigned round = 0; round < 20000; ++round) {
        memcpy(bytes, valid, size);
        unsigned flips = 1 + next_random(&state) % 3;
        for (unsigned f = 0; f < flips; ++f)
            bytes[next_random(&state) % size] ^= (unsigned char)(1u << (next_random(&state) % 8));
        if (pure_module_load(bytes, size, &module_b, error, sizeof(error)) != PURE_OK) continue;
        ++accepted;
        PureApp app;
        if (pure_app_start(&app, &module_b, (PureData){0}) != PURE_OK) continue;
        for (unsigned e = 0; e < 12; ++e) {
            uint32_t input = next_random(&state) % 2;
            if (module_b.input[input].kind == PURE_KIND_TEXT) pure_app_text(&app, input, (PureSpan){(const unsigned char *)"t", 1});
            else if (module_b.input[input].kind == PURE_KIND_KEY) pure_app_input(&app, input, PURE_EDGE_DOWN);
            for (uint32_t v = 0; v < module_b.views; ++v) {
                double value;
                if (!module_b.view_table[v]) {
                    if (module_b.view_type[v] == PURE_VIEW_TEXT) pure_app_view_text(&app, v, ignore_span, NULL);
                    else pure_app_view(&app, v, &value);
                    continue;
                }
                size_t rows = pure_app_rows(&app, module_b.view_table[v] - 1u);
                for (size_t row = 0; row <= rows; ++row) {
                    if (module_b.view_type[v] == PURE_VIEW_TEXT) pure_app_cell_text(&app, v, row, ignore_span, NULL);
                    else if (pure_app_cell(&app, v, row, &value)) CHECK(isfinite(value));
                }
            }
        }
        pure_app_stop(&app);
    }
    printf("{\"table_module_mutations\":20000,\"accepted_after_damage\":%u}\n", accepted);
}

/* ---- Stored states ---------------------------------------------------- */

static char *new_store(void) {
    static char path[64];
    snprintf(path, sizeof(path), "/tmp/pure-module-store-XXXXXX");
    CHECK(mkdtemp(path) && pure_store_init(path) == PURE_OK);
    return path;
}

static void remove_store(const char *path) {
    DIR *entries = opendir(path);
    CHECK(entries);
    struct dirent *entry;
    while ((entry = readdir(entries)))
        if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..")) CHECK(unlinkat(dirfd(entries), entry->d_name, 0) == 0);
    CHECK(closedir(entries) == 0 && rmdir(path) == 0);
}

typedef struct { unsigned char bytes[4096]; size_t size; } Bytes;

static bool bytes_span(void *context, PureSpan span) {
    Bytes *b = context;
    CHECK(b->size + span.size <= sizeof(b->bytes));
    memcpy(b->bytes + b->size, span.data, span.size);
    b->size += span.size;
    return true;
}

/* Two apps of one module hold equal states: numbers bit for bit, texts and
 * list items byte for byte. */
static void same_states(const PureApp *a, const PureApp *b) {
    const PureModule *m = a->module;
    for (uint32_t i = 0; i < m->states; ++i) {
        if (m->state_type[i] == PURE_STATE_TEXT) {
            static Bytes x, y;
            x.size = y.size = 0;
            CHECK(pure_draft_read(a->texts[i], 0, pure_draft_size(a->texts[i]), bytes_span, &x) == PURE_OK);
            CHECK(pure_draft_read(b->texts[i], 0, pure_draft_size(b->texts[i]), bytes_span, &y) == PURE_OK);
            CHECK(x.size == y.size && !memcmp(x.bytes, y.bytes, x.size));
        } else if (m->state_type[i] == PURE_STATE_LIST) {
            const PureRecord *r = &m->record[m->state_ref[i]];
            CHECK(pure_list_count(a->lists[i]) == pure_list_count(b->lists[i]));
            for (size_t at = 0; at < pure_list_count(a->lists[i]); ++at)
                for (unsigned f = 0; f < r->count; ++f) {
                    PureCell x = pure_list_get(a->lists[i], at, f), y = pure_list_get(b->lists[i], at, f);
                    CHECK(!memcmp(&x.num, &y.num, sizeof(x.num)) && x.size == y.size && (!x.size || !memcmp(x.text, y.text, x.size)));
                }
        } else CHECK(!memcmp(&a->values[i], &b->values[i], sizeof(double)));
    }
}

/* The same random events on an app in memory and on one whose states are
 * stored and which restarts from them every few events: equal after every
 * step, failed events included. */
static void stored_equivalence(Spec spec, const char *const *words, size_t word_count, uint32_t seed) {
    char *path = new_store();
    CHECK(load(spec, &module_a) == PURE_OK);
    PureApp memory, stored;
    CHECK(pure_app_start(&memory, &module_a, (PureData){0}) == PURE_OK);
    CHECK(pure_app_start(&stored, &module_a, pure_tree_data(path)) == PURE_OK);
    unsigned restarts = 0;
    for (unsigned step = 0; step < 400; ++step) {
        uint32_t input = next_random(&seed) % module_a.inputs;
        PureResult a, b;
        if (module_a.input[input].kind == PURE_KIND_TEXT) {
            const char *word = words[next_random(&seed) % word_count];
            PureSpan text = {(const unsigned char *)word, strlen(word)};
            a = pure_app_text(&memory, input, text);
            b = pure_app_text(&stored, input, text);
        } else {
            a = pure_app_input(&memory, input, PURE_EDGE_DOWN);
            b = pure_app_input(&stored, input, PURE_EDGE_DOWN);
        }
        CHECK(a == b);
        same_states(&memory, &stored);
        if (next_random(&seed) % 7 == 0) {
            pure_app_stop(&stored);
            CHECK(pure_app_start(&stored, &module_a, pure_tree_data(path)) == PURE_OK);
            same_states(&memory, &stored);
            ++restarts;
        }
    }
    CHECK(restarts > 20);
    pure_app_stop(&memory);
    pure_app_stop(&stored);
    remove_store(path);
}

/* One event changes a number, a text and a list; the process dies at each
 * commit stage. Starting again finds all three as before or all as after. */
enum { NOTE = 1, TASKS = 3 };
static const S crash_nums[] = {{"n", 0}};
static const T crash_states[] = {{"note", 1, 0}, {"at", 2, 1}, {"tasks", 3, 0}, {"i", 4, 3}};
static const RF crash_fields[] = {{"title", 1, 0}, {"done", 0, 0}};
static const R crash_records[] = {{"task", crash_fields, 2}};
static const char *const crash_constants[] = {"", "x"};
static const I crash_inputs[] = {KEY("go", "KeyG")};
static const O crash_code[] = {
    {L, 0, 0}, {C, 0, 1}, {ADD, 0, 0}, {ST, 0, 0},              /* n = n + 1 */
    {CUL, 2, 0}, {TK, 1, 0}, {TINS, 1, 0},                      /* note.insert(at, "x") */
    {TK, 1, 0}, {L, 0, 0}, {LA, 3, 0},                          /* tasks.append(task { "x", n }) */
    {XE, 3, 0}, {XS, 4, 0},                                     /* i = tasks.last */
};
static const H crash_handlers[] = {{1, 0, 12}};

static unsigned consistent(const PureApp *app) {
    unsigned n = (unsigned)app->values[0];
    CHECK(app->values[0] == n && pure_draft_size(app->texts[NOTE]) == n && app->values[2] == n);
    CHECK(pure_list_count(app->lists[TASKS]) == n && app->values[4] == (n ? n - 1 : 0));
    for (unsigned k = 0; k < n; ++k) CHECK(pure_list_get(app->lists[TASKS], k, 1).num == k + 1);
    return n;
}

static void stored_crashes(void) {
    char *path = new_store();
    Spec spec = {crash_nums, 1, crash_inputs, 1, NULL, 0, crash_handlers, 1, crash_code, LEN(crash_code), NULL, 0,
                 crash_states, 4, crash_constants, 2, NULL, 0, crash_records, 1, NULL, 0};
    CHECK(load(spec, &module_a) == PURE_OK);
    PureApp app;
    CHECK(pure_app_start(&app, &module_a, pure_tree_data(path)) == PURE_OK);
    CHECK(pure_app_input(&app, 0, PURE_EDGE_DOWN) == PURE_OK && consistent(&app) == 1);
    pure_app_stop(&app);
    unsigned expected = 1;
    for (int stage = PURE_AFTER_HEADER; stage <= PURE_BEFORE_DIR_SYNC; ++stage) {
        pid_t child = fork();
        CHECK(child >= 0);
        if (!child) {
            PureApp doomed;
            if (pure_app_start(&doomed, &module_a, pure_tree_data(path)) != PURE_OK) _exit(1);
            crash = 1;
            fail_stage = stage;
            pure_app_input(&doomed, 0, PURE_EDGE_DOWN);
            _exit(0);
        }
        int status;
        CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 77);
        if (stage == PURE_BEFORE_DIR_SYNC) ++expected; /* the head was written */
        CHECK(pure_app_start(&app, &module_a, pure_tree_data(path)) == PURE_OK && consistent(&app) == expected);
        pure_app_stop(&app);
    }
    /* A failed commit leaves the app and the store as they were; after an
     * uncertain one the app no longer matches the store and its events fail
     * until it starts again, from what the store has. */
    CHECK(pure_app_start(&app, &module_a, pure_tree_data(path)) == PURE_OK);
    for (fail_stage = PURE_AFTER_HEADER; fail_stage <= PURE_BEFORE_RENAME; ++fail_stage) {
        CHECK(pure_app_input(&app, 0, PURE_EDGE_DOWN) == PURE_STORE_ERROR && consistent(&app) == expected);
    }
    fail_stage = PURE_BEFORE_DIR_SYNC;
    CHECK(pure_app_input(&app, 0, PURE_EDGE_DOWN) == PURE_UNCERTAIN && consistent(&app) == expected);
    fail_stage = -1;
    CHECK(pure_app_input(&app, 0, PURE_EDGE_DOWN) == PURE_CONFLICT && consistent(&app) == expected);
    pure_app_stop(&app);
    CHECK(pure_app_start(&app, &module_a, pure_tree_data(path)) == PURE_OK && consistent(&app) == expected + 1);
    CHECK(pure_app_input(&app, 0, PURE_EDGE_DOWN) == PURE_OK && consistent(&app) == expected + 2);
    pure_app_stop(&app);
    remove_store(path);
}

/* Frame changes are stored by the next event or by a save; a stop alone
 * leaves them out. */
static void stored_frames(void) {
    char *path = new_store();
    static const O code[] = {{L, 0, 0}, {C, 0, 1}, {ADD, 0, 0}, {ST, 0, 0}};
    static const H handlers[] = {{0, 0, 4}, {1, 0, 0}};
    Spec spec = {crash_nums, 1, crash_inputs, 1, NULL, 0, handlers, 2, code, 4, NULL, 0, NULL, 0, NULL, 0, NULL, 0,
                 NULL, 0, NULL, 0};
    CHECK(load(spec, &module_a) == PURE_OK);
    PureApp app;
    CHECK(pure_app_start(&app, &module_a, pure_tree_data(path)) == PURE_OK);
    for (int i = 0; i < 5; ++i) CHECK(pure_app_frame(&app) == PURE_OK);
    pure_app_stop(&app);
    CHECK(pure_app_start(&app, &module_a, pure_tree_data(path)) == PURE_OK && app.values[0] == 0);
    for (int i = 0; i < 3; ++i) CHECK(pure_app_frame(&app) == PURE_OK);
    CHECK(pure_app_save(&app) == PURE_OK);
    pure_app_stop(&app);
    CHECK(pure_app_start(&app, &module_a, pure_tree_data(path)) == PURE_OK && app.values[0] == 3);
    for (int i = 0; i < 2; ++i) CHECK(pure_app_frame(&app) == PURE_OK);
    CHECK(pure_app_input(&app, 0, PURE_EDGE_DOWN) == PURE_OK);
    pure_app_stop(&app);
    CHECK(pure_app_start(&app, &module_a, pure_tree_data(path)) == PURE_OK && app.values[0] == 5);
    pure_app_stop(&app);
    remove_store(path);
}

static bool list_keys(void *context, const char *key) {
    strcat(context, key);
    strcat(context, ",");
    return true;
}

/* A message's document and the states its handlers change commit together. */
static void stored_messages(void) {
    char *path = new_store();
    CHECK(load(editor_spec, &module_a) == PURE_OK);
    PureApp app;
    unsigned char buffer[64];
    CHECK(pure_app_start(&app, &module_a, pure_tree_data(path)) == PURE_OK);
    CHECK(pure_app_message(&app, 1, "doc", 0, edit_payload(buffer, 0, 0, "hello")) == PURE_OK && app.revision == 1);
    CHECK(app.values[0] == 1);
    CHECK(pure_app_message(&app, 1, "doc", 0, edit_payload(buffer, 0, 0, "x")) == PURE_CONFLICT && app.values[0] == 1);
    /* An edit past the size guard fails: neither the document nor the count moves. */
    CHECK(pure_app_message(&app, 3, "doc", 1, edit_payload(buffer, 5, 0, "-world")) == PURE_LIMIT && app.values[0] == 1);
    CHECK(pure_app_message(&app, 3, "doc", 1, edit_payload(buffer, 5, 0, "!")) == PURE_OK && app.values[0] == 2);
    pure_app_stop(&app);
    CHECK(pure_app_start(&app, &module_a, pure_tree_data(path)) == PURE_OK && app.values[0] == 2);
    CHECK(pure_app_message(&app, 0, "doc", -1, (PureSpan){0}) == PURE_OK && app.revision == 2 && replied(&app, "hello!"));
    pure_app_stop(&app);
    /* Documents stay documents: the app's states are not among the keys. */
    char seen[256] = "";
    CHECK(pure_store_keys(path, list_keys, seen) == PURE_OK && !strcmp(seen, "doc,"));
    remove_store(path);
}

/* A stored list whose record changed keeps each field by name, when the app
 * starts from it and when its module is replaced; stored values that no
 * longer fit fall back, and damaged stored values stop the start. */
static void stored_migration(void) {
    char *path = new_store();
    static const RF changed_fields[] = {{"done", 0, 0}, {"note", 1, 1}, {"title", 1, 0}, {"rank", 0, 5}};
    static const R changed[] = {{"task", changed_fields, 4}};
    Spec before = {crash_nums, 1, crash_inputs, 1, NULL, 0, crash_handlers, 1, crash_code, LEN(crash_code), NULL, 0,
                   crash_states, 4, crash_constants, 2, NULL, 0, crash_records, 1, NULL, 0};
    Spec after = before;
    after.r = changed;
    after.h = NULL;
    after.nh = 0;
    CHECK(load(before, &module_a) == PURE_OK && load(after, &module_b) == PURE_OK);
    PureApp app;
    CHECK(pure_app_start(&app, &module_a, pure_tree_data(path)) == PURE_OK);
    CHECK(pure_app_input(&app, 0, PURE_EDGE_DOWN) == PURE_OK && pure_app_input(&app, 0, PURE_EDGE_DOWN) == PURE_OK);
    pure_app_stop(&app);
    for (int round = 0; round < 2; ++round) { /* migrated, then stored as migrated */
        CHECK(pure_app_start(&app, &module_b, pure_tree_data(path)) == PURE_OK && pure_list_count(app.lists[TASKS]) == 2);
        PureCell note = pure_list_get(app.lists[TASKS], 1, 1), title = pure_list_get(app.lists[TASKS], 1, 2);
        CHECK(pure_list_get(app.lists[TASKS], 1, 0).num == 2 && pure_list_get(app.lists[TASKS], 1, 3).num == 5);
        CHECK(note.size == 1 && note.text[0] == 'x' && title.size == 1 && title.text[0] == 'x');
        CHECK(app.values[0] == 2 && app.values[4] == 1 && pure_draft_size(app.texts[NOTE]) == 2);
        pure_app_stop(&app);
    }
    /* Back to the first record by replacing the module; the next save stores it. */
    CHECK(pure_app_start(&app, &module_b, pure_tree_data(path)) == PURE_OK);
    CHECK(pure_app_replace(&app, &module_a) == PURE_OK && pure_app_input(&app, 0, PURE_EDGE_DOWN) == PURE_OK);
    pure_app_stop(&app);
    CHECK(pure_app_start(&app, &module_a, pure_tree_data(path)) == PURE_OK && consistent(&app) == 3);
    pure_app_stop(&app);
    /* Stored numbers that no longer fit their state start again. */
    PureDraft *values;
    int64_t revision;
    CHECK(pure_state_open(path, "values", &values) == PURE_OK);
    unsigned char record[64] = "PVS1";
    size_t at = 4;
    const char *names[] = {"at", "i", "n"};
    double stored[] = {100, 7.5, 9};
    uint8_t types[] = {PURE_STATE_CURSOR, PURE_STATE_INDEX, PURE_STATE_NUM};
    for (unsigned k = 0; k < 3; ++k) {
        record[at++] = types[k];
        record[at++] = (unsigned char)strlen(names[k]);
        memcpy(record + at, names[k], strlen(names[k]));
        at += strlen(names[k]);
        uint64_t bits;
        memcpy(&bits, &stored[k], sizeof(bits));
        for (unsigned b = 0; b < 8; ++b) record[at++] = (unsigned char)(bits >> (8 * b));
    }
    CHECK(pure_draft_splice(values, 0, pure_draft_size(values), (PureSpan){record, at}) == PURE_OK);
    CHECK(pure_draft_commit(values, &revision) == PURE_OK);
    CHECK(pure_app_start(&app, &module_a, pure_tree_data(path)) == PURE_OK);
    CHECK(app.values[0] == 9 && app.values[2] == 0 && app.values[4] == 0);
    pure_app_stop(&app);
    /* Damaged stored lists and texts are refused, not replaced. */
    const char *damaged[] = {"list-tasks", "text-note"};
    for (unsigned k = 0; k < 2; ++k) {
        PureDraft *draft;
        CHECK(pure_state_open(path, damaged[k], &draft) == PURE_OK);
        CHECK(pure_draft_splice(draft, 0, 0, (PureSpan){(const unsigned char *)"\xff", 1}) == PURE_OK);
        CHECK(pure_draft_commit(draft, &revision) == PURE_OK);
        CHECK(pure_app_start(&app, &module_a, pure_tree_data(path)) == PURE_STORE_ERROR);
        CHECK(pure_draft_splice(draft, 0, 1, (PureSpan){0}) == PURE_OK && pure_draft_commit(draft, &revision) == PURE_OK);
        pure_draft_close(draft);
    }
    CHECK(pure_app_start(&app, &module_a, pure_tree_data(path)) == PURE_OK);
    CHECK(pure_list_count(app.lists[TASKS]) == 3 && pure_draft_size(app.texts[NOTE]) == 3 && app.values[0] == 9);
    pure_app_stop(&app);
    pure_draft_close(values);
    remove_store(path);
}

static void stored_states(void) {
    static const char *const list_words[] = {"a", "\xea\xb0\x80", "", "\n", "\xf0\x9f\x98\x80"};
    static const char *const text_words[] = {"a", "\xea\xb0\x80", "\n", "\xf0\x9f\x98\x80", "xy\nz", ""};
    stored_equivalence(list_spec, list_words, LEN(list_words), 0x9e3779b9u);
    stored_equivalence(text_spec, text_words, LEN(text_words), 0x7f4a7c15u);
    stored_crashes();
    stored_frames();
    stored_messages();
    stored_migration();
}

/* ---- Deferred saving ---------------------------------------------------- */

typedef struct { uint32_t input; const char *word; } Step;

static PureResult apply_step(PureApp *app, Step step) {
    if (step.word) return pure_app_text(app, step.input, (PureSpan){(const unsigned char *)step.word, strlen(step.word)});
    return pure_app_input(app, step.input, PURE_EDGE_DOWN);
}

/* Deferred events apply at once and last only through a save: an app that
 * stops without saving comes back as it was at the last save. The oracle is
 * an app in memory that replays exactly the saved events. */
static void deferred_equivalence(Spec spec, const char *const *words, size_t word_count, uint32_t seed) {
    char *path = new_store();
    CHECK(load(spec, &module_a) == PURE_OK);
    static Step history[2000];
    size_t saved = 0, applied = 0;
    PureApp memory, stored;
    CHECK(pure_app_start(&memory, &module_a, (PureData){0}) == PURE_OK);
    CHECK(pure_app_start(&stored, &module_a, pure_tree_data(path)) == PURE_OK);
    CHECK(pure_app_defer(&stored, true) && !pure_app_defer(&memory, true));
    unsigned lost = 0, saves = 0;
    for (unsigned step = 0; step < 600; ++step) {
        Step next = {next_random(&seed) % module_a.inputs, NULL};
        if (module_a.input[next.input].kind == PURE_KIND_TEXT) next.word = words[next_random(&seed) % word_count];
        PureResult a = apply_step(&memory, next), b = apply_step(&stored, next);
        CHECK(a == b);
        if (a == PURE_OK) history[applied++] = next;
        same_states(&memory, &stored);
        uint32_t roll = next_random(&seed) % 16;
        if (roll < 3) {
            CHECK(pure_app_save(&stored) == PURE_OK);
            saved = applied;
            ++saves;
        } else if (roll == 3) {
            /* Stop without saving: only saved events come back. */
            pure_app_stop(&stored);
            pure_app_stop(&memory);
            CHECK(pure_app_start(&stored, &module_a, pure_tree_data(path)) == PURE_OK && pure_app_defer(&stored, true));
            CHECK(pure_app_start(&memory, &module_a, (PureData){0}) == PURE_OK);
            for (size_t k = 0; k < saved; ++k) CHECK(apply_step(&memory, history[k]) == PURE_OK);
            lost += (unsigned)(applied - saved);
            applied = saved;
            same_states(&memory, &stored);
        }
    }
    CHECK(saves > 50 && lost > 0);
    pure_app_stop(&memory);
    pure_app_stop(&stored);
    remove_store(path);
}

/* A burst of events is saved with one commit: when that commit fails every
 * event of the burst is undone, and a process that dies while saving comes
 * back with all of the burst or none of it. */
static void deferred_bursts(void) {
    char *path = new_store();
    Spec spec = {crash_nums, 1, crash_inputs, 1, NULL, 0, crash_handlers, 1, crash_code, LEN(crash_code), NULL, 0,
                 crash_states, 4, crash_constants, 2, NULL, 0, crash_records, 1, NULL, 0};
    CHECK(load(spec, &module_a) == PURE_OK);
    PureApp app;
    CHECK(pure_app_start(&app, &module_a, pure_tree_data(path)) == PURE_OK && pure_app_defer(&app, true));
    for (int i = 0; i < 5; ++i) CHECK(pure_app_input(&app, 0, PURE_EDGE_DOWN) == PURE_OK);
    CHECK(consistent(&app) == 5 && app.events == 5);
    for (fail_stage = PURE_AFTER_HEADER; fail_stage <= PURE_BEFORE_RENAME; fail_stage += 2) {
        CHECK(pure_app_save(&app) == PURE_STORE_ERROR && consistent(&app) == 0 && app.events == 0);
        for (int i = 0; i < 5; ++i) CHECK(pure_app_input(&app, 0, PURE_EDGE_DOWN) == PURE_OK);
    }
    fail_stage = -1;
    CHECK(pure_app_save(&app) == PURE_OK && consistent(&app) == 5);
    pure_app_stop(&app);
    CHECK(pure_app_start(&app, &module_a, pure_tree_data(path)) == PURE_OK && consistent(&app) == 5);
    pure_app_stop(&app);
    unsigned expected = 5;
    for (int stage = PURE_AFTER_HEADER; stage <= PURE_BEFORE_DIR_SYNC; ++stage) {
        pid_t child = fork();
        CHECK(child >= 0);
        if (!child) {
            PureApp doomed;
            if (pure_app_start(&doomed, &module_a, pure_tree_data(path)) != PURE_OK || !pure_app_defer(&doomed, true)) _exit(1);
            for (int i = 0; i < 7; ++i) if (pure_app_input(&doomed, 0, PURE_EDGE_DOWN) != PURE_OK) _exit(2);
            crash = 1;
            fail_stage = stage;
            pure_app_save(&doomed);
            _exit(0);
        }
        int status;
        CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 77);
        if (stage == PURE_BEFORE_DIR_SYNC) expected += 7;
        CHECK(pure_app_start(&app, &module_a, pure_tree_data(path)) == PURE_OK && consistent(&app) == expected);
        pure_app_stop(&app);
    }
    remove_store(path);
}

/* Messages in a burst: documents of different keys commit together with the
 * states, each moving one revision; a second message for a key saves the
 * burst first, and so does a full burst. */
static void deferred_messages(void) {
    char *path = new_store();
    CHECK(load(editor_spec, &module_a) == PURE_OK);
    PureApp app;
    unsigned char buffer[64];
    char key[8];
    CHECK(pure_app_start(&app, &module_a, pure_tree_data(path)) == PURE_OK && pure_app_defer(&app, true));
    for (int i = 0; i < 3; ++i) {
        snprintf(key, sizeof(key), "k%d", i);
        CHECK(pure_app_message(&app, 1, key, 0, edit_payload(buffer, 0, 0, "ab")) == PURE_OK && app.revision == 1);
    }
    CHECK(app.values[0] == 3 && app.pending_count == 3 && app.open);
    /* The same key again: the burst is saved first, then this edit joins a new one. */
    CHECK(pure_app_message(&app, 1, "k0", 1, edit_payload(buffer, 2, 0, "c")) == PURE_OK && app.revision == 2);
    CHECK(app.pending_count == 1 && app.values[0] == 4);
    CHECK(pure_app_message(&app, 1, "k1", 0, edit_payload(buffer, 0, 0, "x")) == PURE_CONFLICT);
    CHECK(pure_app_save(&app) == PURE_OK && !app.open && !app.pending_count);
    for (int i = 0; i < PURE_APP_PENDING + 1; ++i) {
        snprintf(key, sizeof(key), "m%d", i);
        CHECK(pure_app_message(&app, 1, key, 0, edit_payload(buffer, 0, 0, "y")) == PURE_OK);
        CHECK(app.pending_count == (i < PURE_APP_PENDING ? (unsigned)i + 1 : 1)); /* full: saved on the way */
    }
    pure_app_stop(&app);           /* the last edit was never saved */
    CHECK(pure_app_start(&app, &module_a, pure_tree_data(path)) == PURE_OK && app.values[0] == 4 + PURE_APP_PENDING);
    CHECK(pure_app_message(&app, 0, "k0", -1, (PureSpan){0}) == PURE_OK && app.revision == 2 && replied(&app, "abc"));
    snprintf(key, sizeof(key), "m%d", PURE_APP_PENDING);
    CHECK(pure_app_message(&app, 0, key, -1, (PureSpan){0}) == PURE_NOT_FOUND);
    snprintf(key, sizeof(key), "m%d", PURE_APP_PENDING - 1);
    CHECK(pure_app_message(&app, 0, key, -1, (PureSpan){0}) == PURE_OK && replied(&app, "y"));
    pure_app_stop(&app);
    remove_store(path);
}

static void deferred(void) {
    static const char *const list_words[] = {"a", "\xea\xb0\x80", "", "\n", "\xf0\x9f\x98\x80"};
    static const char *const text_words[] = {"a", "\xea\xb0\x80", "\n", "\xf0\x9f\x98\x80", "xy\nz", ""};
    deferred_equivalence(list_spec, list_words, LEN(list_words), 0x2545f491u);
    deferred_equivalence(text_spec, text_words, LEN(text_words), 0x1b873593u);
    deferred_bursts();
    deferred_messages();
}

/* A host points at an item: the index a view shows as its position moves
 * there, as an event with no handler, saved like a key event; deferred, it
 * joins the burst and a failed save undoes it with the rest. */
static void pointing(void) {
    CHECK(load(list_spec, &module_a) == PURE_OK);
    CHECK(pure_module_position(&module_a, 1) == 2); /* position = at.position */
    CHECK(pure_module_position(&module_a, 0) == -1 && pure_module_position(&module_a, 2) == -1 &&
          pure_module_position(&module_a, 3) == -1 && pure_module_position(&module_a, 99) == -1);
    char path[] = "/tmp/pure-point-XXXXXX";
    CHECK(mkdtemp(path) && pure_store_init(path) == PURE_OK);
    PureApp app;
    CHECK(pure_app_start(&app, &module_a, pure_tree_data(path)) == PURE_OK);
    CHECK(pure_app_point(&app, 2, 0) == PURE_NOT_FOUND); /* nothing to point at */
    for (int i = 0; i < 3; ++i) CHECK(pure_app_text(&app, 0, (PureSpan){(const unsigned char *)"a", 1}) == PURE_OK);
    uint64_t events = app.events;
    CHECK(app.values[2] == 2 && pure_app_point(&app, 2, 0) == PURE_OK && app.values[2] == 0 && app.events == events + 1);
    CHECK(pure_app_point(&app, 2, 3) == PURE_NOT_FOUND && app.values[2] == 0);
    CHECK(pure_app_point(&app, 0, 1) == PURE_INVALID && pure_app_point(&app, 1, 1) == PURE_INVALID); /* a number, a list */
    CHECK(pure_app_point(&app, 2, 1) == PURE_OK);
    pure_app_stop(&app);
    CHECK(pure_app_start(&app, &module_a, pure_tree_data(path)) == PURE_OK && app.values[2] == 1);
    CHECK(pure_app_defer(&app, true) && pure_app_point(&app, 2, 2) == PURE_OK && app.open && app.values[2] == 2);
    CHECK(pure_app_save(&app) == PURE_OK);
    pure_app_stop(&app);
    CHECK(pure_app_start(&app, &module_a, pure_tree_data(path)) == PURE_OK && app.values[2] == 2);
    pure_app_stop(&app);
    remove_store(path);
}

int main(void) {
    behaviour();
    faults();
    rejections();
    mutations();
    messages();
    message_mutations();
    texts();
    text_mutations();
    lists();
    pointing();
    list_mutations();
    tables();
    table_mutations();
    stored_states();
    deferred();
    printf("{\"sizeof_module\":%zu,\"sizeof_app\":%zu}\n", sizeof(PureModule), sizeof(PureApp));
    puts("{\"module_tests\":\"verified-load-atomic-events-faults-signed-zero-if-else-replace-by-name-rejections-mutations-messages-documents-typed-verification-text-cursors-utf8-views-rollback-replace-lists-indices-undo-migration-joins-tables-columns-pick-slide-stored-equivalence-crashes-frames-messages-migration-deferred-bursts-pointing\"}");
    return 0;
}
