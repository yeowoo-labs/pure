#define _POSIX_C_SOURCE 200809L
#include "module.h"
#include "module-store.h"
#include "module-text.h"
#include "store.h"
#include <dirent.h>
#include <math.h>
#include <pthread.h>
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
/* A timer's period (milliseconds) rides in nfields. */
#define TIMER(name, period) {name, NULL, PURE_KIND_TIMER, false, NULL, period}
typedef struct { const char *name; uint32_t start, length; } V;
typedef struct { uint32_t event, start, length; } H;
typedef struct { uint8_t op; uint32_t arg; double value; } O;
/* Text (type 1, ref = constant), cursor (type 2, ref = text state), list
 * (type 3, ref = record) and index (type 4, ref = list state) states follow
 * the number states; text views follow the number views. */
typedef struct { const char *name; uint32_t type, ref; } T; /* type: the state type, its flags from bit 8 */
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
        for (size_t f = 0; spec.in[i].kind != PURE_KIND_TIMER && f < spec.in[i].nfields; ++f)
            text[names++] = spec.in[i].fields[f].name;
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
    for (size_t i = 0; i < spec.ni; ++i) fields += spec.in[i].kind == PURE_KIND_TIMER ? 0 : spec.in[i].nfields;
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
    for (size_t i = 0; i < spec.nt; ++i) {
        p = put32(p, offset[n++]);
        p = put32(p, spec.t[i].type & 0xFF);
        p = put32(p, spec.t[i].ref);
        p = put32(p, spec.t[i].type >> 8);
    }
    for (size_t i = 0; i < spec.nc; ++i) { p = put32(p, offset[n++]); p = put32(p, 1); p = put32(p, 1); }
    uint32_t field_offsets[64], field_start = 0;
    size_t nf = 0;
    for (size_t i = 0; i < spec.ni; ++i) {
        const I *in = &spec.in[i];
        p = put32(p, offset[n++]);
        *p++ = (unsigned char)(in->kind ? in->kind : 1); *p++ = in->keyed; *p++ = 0; *p++ = 0;
        bool timer = in->kind == PURE_KIND_TIMER;
        p = put32(p, timer ? (uint32_t)in->nfields : in->kind ? 0 : offset[n++]);
        uint32_t first = in->kind == PURE_KIND_MESSAGE || in->kind == PURE_KIND_OUTPUT ? field_start : 0; /* only messages and outputs have fields */
        *p++ = (unsigned char)first; *p++ = (unsigned char)(first >> 8);
        *p++ = (unsigned char)(timer ? 0 : in->nfields); *p++ = 0;
        for (size_t f = 0; !timer && f < in->nfields; ++f) field_offsets[nf++] = offset[n++];
        field_start += timer ? 0 : (uint32_t)in->nfields;
    }
    nf = 0;
    for (size_t i = 0; i < spec.ni; ++i)
        for (size_t f = 0; spec.in[i].kind != PURE_KIND_TIMER && f < spec.in[i].nfields; ++f) {
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
        *p++ = (unsigned char)w->type; *p++ = (unsigned char)(w->type >> 8); /* a filtered table: its list + 1 */
        *p++ = (unsigned char)w->table; *p++ = (unsigned char)(w->table >> 8);
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
    static const F unknown_type[] = {{"insert", 5}, {"start", 1}};
    static const I misplaced[] = {{"edit", NULL, PURE_KIND_MESSAGE, true, unknown_type, 2}};
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

/* Text inserted at a cursor goes before it; at a mark (a cursor that stays),
 * after it. Everywhere else, and when text is removed, both move alike. */
static void marks(void) {
    enum { MARK = 2 | PURE_CURSOR_STAYS << 8 };
    static const T textual[] = {{"note", 1, 0}, {"at", 2, 0}, {"mark", MARK, 0}};
    static const char *const constants[] = {"ab"};
    static const I inputs[] = {{"typed", NULL, PURE_KIND_TEXT, false, NULL, 0}, KEY("keep", "KeyK"), KEY("back", "Backspace")};
    static const O code[] = {
        {CUL, 1, 0}, {TI, 0, 0}, {TINS, 0, 0},                                  /* 0 typed: at the cursor */
        {CUL, 1, 0}, {CS, 2, 0},                                                /* 3 keep: mark = at */
        {CUL, 1, 0}, {C, 0, -1}, {CM, 0, 0}, {CUL, 1, 0}, {TREM, 0, 0},          /* 5 back */
    };
    static const H handlers[] = {{1, 0, 3}, {3, 3, 2}, {5, 5, 5}};
    Spec spec = {NULL, 0, inputs, LEN(inputs), NULL, 0, handlers, LEN(handlers), code, LEN(code),
                 NULL, 0, textual, 3, constants, 1, NULL, 0, NULL, 0, NULL, 0};
    if (load(spec, &module_a) != PURE_OK) { fprintf(stderr, "%s\n", error); abort(); }
    CHECK(module_a.state_flags[2] == PURE_CURSOR_STAYS && !module_a.state_flags[1]);
    PureApp app;
    CHECK(pure_app_start(&app, &module_a, (PureData){0}) == PURE_OK);
    CHECK(pure_app_text(&app, 0, (PureSpan){(const unsigned char *)"x", 1}) == PURE_OK);
    CHECK(!strcmp(state_text(&app, 0), "xab") && app.values[1] == 1 && app.values[2] == 0); /* the mark stayed */
    CHECK(pure_app_input(&app, 1, PURE_EDGE_DOWN) == PURE_OK && app.values[2] == 1);
    CHECK(pure_app_text(&app, 0, (PureSpan){(const unsigned char *)"yz", 2}) == PURE_OK);
    CHECK(!strcmp(state_text(&app, 0), "xyzab") && app.values[1] == 3 && app.values[2] == 1);
    CHECK(pure_app_input(&app, 2, PURE_EDGE_DOWN) == PURE_OK && app.values[1] == 2 && app.values[2] == 1);
    CHECK(pure_app_input(&app, 2, PURE_EDGE_DOWN) == PURE_OK && app.values[1] == 1 && app.values[2] == 1); /* gathered */
    pure_app_stop(&app);
    /* A flag only on a cursor, and only this one. */
    static const T wrong_flag[] = {{"note", 1, 0}, {"at", 2 | 2 << 8, 0}, {"mark", MARK, 0}};
    static const T flagged_text[] = {{"note", 1 | PURE_CURSOR_STAYS << 8, 0}, {"at", 2, 0}, {"mark", 2, 0}};
    Spec bad = spec;
    bad.t = wrong_flag;
    CHECK(load(bad, &module_b) == PURE_INVALID && strstr(error, "state reference"));
    bad.t = flagged_text;
    CHECK(load(bad, &module_b) == PURE_INVALID && strstr(error, "state reference"));
}

/* Folds: a block run once per item of a list, as a column of it, gives a
 * count, a sum, a maximum or a minimum; an empty maximum has no value. */
enum { FOLD = PURE_OP_FOLD };
#define FOLDING(kind, list, length) ((uint32_t)(kind) | (uint32_t)(list) << 8 | (uint32_t)(length) << 16)
static const RF fold_fields[] = {{"title", 1, 0}, {"done", 0, 0}, {"points", 0, 0}};
static const R fold_records[] = {{"task", fold_fields, 3}};
static const T fold_states[] = {{"tasks", 3, 0}, {"at", 4, 0}};
static const char *const fold_constants[] = {""};
static const I fold_inputs[] = {{"typed", NULL, PURE_KIND_TEXT, false, NULL, 0}, KEY("toggle", "KeyT")};
enum { F_TITLE = 0, F_DONE = 1u << 16, F_POINTS = 2u << 16 };
static const O fold_code[] = {
    {TI, 0, 0}, {C, 0, 0}, {TI, 0, 0}, {CHR, 0, 0}, {LA, 0, 0}, {XE, 0, 0}, {XS, 1, 0},     /* 0 typed */
    {XL, 1, 0}, {XL, 1, 0}, {IG, F_DONE, 0}, {NOT, 0, 0}, {IS, F_DONE, 0},                 /* 7 toggle */
    {FOLD, FOLDING(PURE_FOLD_COUNT, 0, 3), 0}, {ROW, 0, 0}, {IG, F_DONE, 0}, {NOT, 0, 0},   /* 12 open */
    {FOLD, FOLDING(PURE_FOLD_SUM, 0, 3), 0}, {ROW, 0, 0}, {IG, F_POINTS, 0}, {C, 0, 1},     /* 16 points */
    {FOLD, FOLDING(PURE_FOLD_MAX, 0, 4), 0}, {ROW, 0, 0}, {IG, F_TITLE, 0}, {CHR, 0, 0},    /* 20 longest */
    {C, 0, 1},
    {FOLD, FOLDING(PURE_FOLD_SUM, 0, 4), 0}, {ROW, 0, 0}, {IG, F_POINTS, 0}, {ROW, 0, 0},   /* 25 done_points */
    {IG, F_DONE, 0},
    {FOLD, FOLDING(PURE_FOLD_MIN, 0, 5), 0}, {ROW, 0, 0}, {IG, F_POINTS, 0}, {ROW, 0, 0},   /* 30 fewest_open */
    {IG, F_DONE, 0}, {NOT, 0, 0},
    {C, 0, 10}, {FOLD, FOLDING(PURE_FOLD_COUNT, 0, 1), 0}, {C, 0, 1}, {ADD, 0, 0},         /* 36 count_plus */
    {FOLD, FOLDING(PURE_FOLD_FIRST, 0, 3), 0}, {ROW, 0, 0}, {IG, F_DONE, 0}, {NOT, 0, 0},   /* 40 first_open */
    {XN, 0, 0},
};
static const H fold_handlers[] = {{1, 0, 7}, {3, 7, 5}};
static const V fold_views[] = {{"open", 12, 4}, {"points", 16, 4}, {"longest", 20, 5}, {"done_points", 25, 5},
                               {"fewest_open", 30, 6}, {"count_plus", 36, 4}, {"first_open", 40, 5}};
static Spec fold_spec(const O *code, size_t count, const V *views, size_t nviews, const H *handlers, size_t nhandlers) {
    return (Spec){NULL, 0, fold_inputs, LEN(fold_inputs), views, nviews, handlers, nhandlers, code, count, NULL, 0,
                  fold_states, 2, fold_constants, 1, NULL, 0, fold_records, 1, NULL, 0};
}

static void folds(void) {
    CHECK(load(fold_spec(fold_code, LEN(fold_code), fold_views, LEN(fold_views), fold_handlers, 2), &module_a) == PURE_OK);
    PureApp app;
    double value;
    CHECK(pure_app_start(&app, &module_a, (PureData){0}) == PURE_OK);
    CHECK(number_view(&app, 0) == 0 && number_view(&app, 1) == 0 && !pure_app_view(&app, 2, &value));
    CHECK(number_view(&app, 3) == 0 && !pure_app_view(&app, 4, &value) && number_view(&app, 5) == 10);
    CHECK(!pure_app_view(&app, 6, &value));                           /* no first open task yet */
    const char *const words[] = {"a", "bbb", "\xea\xb0\x80\xea\xb0\x80"}; /* one, three and two characters */
    for (size_t i = 0; i < 3; ++i)
        CHECK(pure_app_text(&app, 0, (PureSpan){(const unsigned char *)words[i], strlen(words[i])}) == PURE_OK);
    CHECK(number_view(&app, 0) == 3 && number_view(&app, 1) == 6 && number_view(&app, 2) == 3);
    CHECK(number_view(&app, 3) == 0 && number_view(&app, 4) == 1 && number_view(&app, 5) == 13);
    CHECK(pure_app_input(&app, 1, PURE_EDGE_DOWN) == PURE_OK);   /* the last task is done */
    CHECK(number_view(&app, 0) == 2 && number_view(&app, 3) == 2 && number_view(&app, 4) == 1);
    CHECK(number_view(&app, 6) == 0);                                 /* the first open one: an index */
    CHECK(pure_app_point(&app, 1, 0) == PURE_OK && pure_app_input(&app, 1, PURE_EDGE_DOWN) == PURE_OK);
    CHECK(number_view(&app, 6) == 1);
    CHECK(pure_app_point(&app, 1, 1) == PURE_OK && pure_app_input(&app, 1, PURE_EDGE_DOWN) == PURE_OK);
    CHECK(!pure_app_view(&app, 6, &value));
    CHECK(number_view(&app, 0) == 0 && number_view(&app, 3) == 6 && !pure_app_view(&app, 4, &value)); /* none open */
    pure_app_stop(&app);

    /* What a fold may not be. */
    static const O nested[] = {{FOLD, FOLDING(PURE_FOLD_COUNT, 0, 2), 0}, {FOLD, FOLDING(PURE_FOLD_COUNT, 0, 1), 0}, {C, 0, 1}};
    static const O into[] = {{C, 0, 1}, {JZ, 3, 0}, {FOLD, FOLDING(PURE_FOLD_COUNT, 0, 1), 0}, {C, 0, 1}};
    static const O effect[] = {{FOLD, FOLDING(PURE_FOLD_COUNT, 0, 3), 0}, {ROW, 0, 0}, {XS, 1, 0}, {C, 0, 1},
                               {PURE_OP_REQUIRE, 0, 0}};
    static const O short_sum[] = {{FOLD, FOLDING(PURE_FOLD_SUM, 0, 1), 0}, {C, 0, 1}};
    static const O outside[] = {{FOLD, FOLDING(PURE_FOLD_COUNT, 0, 5), 0}, {C, 0, 1}};
    static const O not_list[] = {{FOLD, FOLDING(PURE_FOLD_COUNT, 1, 1), 0}, {C, 0, 1}};
    static const struct { const O *code; size_t count, length; bool handler; const char *reason; } refused[] = {
        {nested, LEN(nested), 3, false, "fold inside a fold"},
        {into, LEN(into), 4, false, "branch into a fold"},
        {effect, LEN(effect), 5, true, "fold with an effect"},
        {short_sum, LEN(short_sum), 2, false, "fold block must leave its numbers"},
        {outside, LEN(outside), 2, false, "fold block outside its block"},
        {not_list, LEN(not_list), 2, false, "no such state"},
    };
    for (size_t i = 0; i < LEN(refused); ++i) {
        V view = {"v", 0, (uint32_t)refused[i].length};
        H handler = {3, 0, (uint32_t)refused[i].length};
        Spec spec = refused[i].handler ? fold_spec(refused[i].code, refused[i].count, NULL, 0, &handler, 1)
                                       : fold_spec(refused[i].code, refused[i].count, &view, 1, NULL, 0);
        if (load(spec, &module_b) != PURE_INVALID || !strstr(error, refused[i].reason)) {
            fprintf(stderr, "fold case %zu: expected '%s', got '%s'\n", i, refused[i].reason, error);
            abort();
        }
    }
}

/* A filtered table: one row per item its filter keeps; its columns read the
 * item (ROW is the item, not the row), and hosts turn rows into items and
 * back. The rows follow every change. */
static const O filter_code[] = {
    {TI, 0, 0}, {C, 0, 0}, {TI, 0, 0}, {CHR, 0, 0}, {LA, 0, 0}, {XE, 0, 0}, {XS, 1, 0},     /* 0 typed */
    {XL, 1, 0}, {XL, 1, 0}, {IG, F_DONE, 0}, {NOT, 0, 0}, {IS, F_DONE, 0},                 /* 7 toggle */
    {ROW, 0, 0}, {IG, F_DONE, 0}, {NOT, 0, 0},                                            /* 12 filter: open */
    {ROW, 0, 0}, {IG, F_TITLE, 0},                                                        /* 15 title */
    {ROW, 0, 0}, {XN, 0, 0},                                                              /* 17 pos */
};
enum { FILTERED = 2 | 1 << 8 }; /* a table over state 0, filtered */
static const W filter_views[] = {{"open", FILTERED, 0, 12, 3}, {"title", 1, 1, 15, 2}, {"pos", 0, 1, 17, 2}};

static Spec filter_spec(const O *code, size_t count, const W *views, size_t nviews) {
    return (Spec){NULL, 0, fold_inputs, LEN(fold_inputs), NULL, 0, fold_handlers, 2, code, count, NULL, 0,
                  fold_states, 2, fold_constants, 1, NULL, 0, fold_records, 1, views, nviews};
}

static const char *cell_title(PureApp *app, uint32_t column, size_t row) {
    static Text text;
    text = (Text){{0}, 0};
    CHECK(pure_app_cell_text(app, column, row, text_span, &text) == PURE_OK && text.size < sizeof(text.text));
    text.text[text.size] = 0;
    return text.text;
}

static void filters(void) {
    CHECK(load(filter_spec(filter_code, LEN(filter_code), filter_views, 3), &module_a) == PURE_OK);
    PureApp app;
    double value;
    size_t at;
    CHECK(pure_app_start(&app, &module_a, (PureData){0}) == PURE_OK && pure_app_rows(&app, 0) == 0);
    for (const char *word = "abc"; *word; ++word)
        CHECK(pure_app_text(&app, 0, (PureSpan){(const unsigned char *)word, 1}) == PURE_OK);
    CHECK(pure_app_rows(&app, 0) == 3 && !strcmp(cell_title(&app, 1, 2), "c") && pure_app_cell(&app, 2, 2, &value) && value == 2);
    CHECK(pure_app_input(&app, 1, PURE_EDGE_DOWN) == PURE_OK);   /* c is done: it leaves */
    CHECK(pure_app_rows(&app, 0) == 2 && !pure_app_cell(&app, 2, 2, &value) && !pure_app_item_row(&app, 0, 2, &at));
    CHECK(pure_app_point(&app, 1, 0) == PURE_OK && pure_app_input(&app, 1, PURE_EDGE_DOWN) == PURE_OK); /* a too */
    CHECK(pure_app_rows(&app, 0) == 1 && !strcmp(cell_title(&app, 1, 0), "b"));
    CHECK(pure_app_cell(&app, 2, 0, &value) && value == 1);      /* the item's position, not the row's */
    CHECK(pure_app_row_item(&app, 0, 0, &at) && at == 1 && !pure_app_row_item(&app, 0, 1, &at));
    CHECK(pure_app_item_row(&app, 0, 1, &at) && at == 0 && !pure_app_item_row(&app, 0, 0, &at));
    CHECK(pure_app_input(&app, 1, PURE_EDGE_DOWN) == PURE_OK);   /* a comes back, before b */
    CHECK(pure_app_rows(&app, 0) == 2 && !strcmp(cell_title(&app, 1, 0), "a") && pure_app_item_row(&app, 0, 1, &at) && at == 1);
    CHECK(pure_app_replace(&app, &module_a) == PURE_OK && pure_app_rows(&app, 0) == 2);
    pure_app_stop(&app);

    /* What a filter may not be. */
    static const O effect[] = {{ROW, 0, 0}, {XS, 1, 0}, {C, 0, 1}};
    static const O text_filter[] = {{ROW, 0, 0}, {IG, F_TITLE, 0}};
    static const O folded[] = {{FOLD, FOLDING(PURE_FOLD_COUNT, 0, 1), 0}, {C, 0, 1}};
    static const W filtered_by[] = {{"open", FILTERED, 0, 0, 0}}, open3[] = {{"open", FILTERED, 0, 0, 3}},
                   open2[] = {{"open", FILTERED, 0, 0, 2}};
    static const W not_a_list[] = {{"open", 2 | 2 << 8, 0, 0, 3}};
    static const W flagged_column[] = {{"open", FILTERED, 0, 0, 3}, {"c", 0 | 1 << 8, 1, 0, 3}};
    static const struct { const O *code; size_t count; const W *views; size_t nviews; const char *reason; } refused[] = {
        {effect, LEN(effect), open3, 1, "filter with an effect"},
        {text_filter, LEN(text_filter), open2, 1, "filter must leave one number"},
        {folded, LEN(folded), open2, 1, "fold inside a filter"},
        {effect, LEN(effect), filtered_by, 1, "table list"},       /* a filtered table needs its block */
        {effect, LEN(effect), not_a_list, 1, "table list"},        /* state 1 is an index */
        {effect, LEN(effect), flagged_column, 2, "view type"},     /* only a table has a list byte */
    };
    for (size_t i = 0; i < LEN(refused); ++i)
        if (load(filter_spec(refused[i].code, refused[i].count, refused[i].views, refused[i].nviews), &module_b) != PURE_INVALID ||
            !strstr(error, refused[i].reason)) {
            fprintf(stderr, "filter case %zu: expected '%s', got '%s'\n", i, refused[i].reason, error);
            abort();
        }
}

/* Damaged fold and filter modules: whatever loads runs safely. */
static void fold_mutations(void) {
    static unsigned char valid[65536], bytes[65536];
    size_t size = build(valid, fold_spec(fold_code, LEN(fold_code), fold_views, LEN(fold_views), fold_handlers, 2));
    for (unsigned kind = 0; kind < 2; ++kind) {
    if (kind) size = build(valid, filter_spec(filter_code, LEN(filter_code), filter_views, 3));
    uint32_t state = kind ? 0x165667b1u : 0x27d4eb2fu;
    unsigned accepted = 0;
    for (unsigned round = 0; round < 20000; ++round) {
        memcpy(bytes, valid, size);
        for (unsigned f = 0, flips = 1 + next_random(&state) % 3; f < flips; ++f)
            bytes[next_random(&state) % size] ^= (unsigned char)(1u << (next_random(&state) % 8));
        if (pure_module_load(bytes, size, &module_b, error, sizeof(error)) != PURE_OK) continue;
        ++accepted;
        PureApp app;
        if (pure_app_start(&app, &module_b, (PureData){0}) != PURE_OK) continue;
        for (unsigned e = 0; e < 16; ++e) {
            uint32_t input = next_random(&state) % 2;
            if (module_b.input[input].kind == PURE_KIND_TEXT) pure_app_text(&app, input, (PureSpan){(const unsigned char *)"ab", 2});
            else if (module_b.input[input].kind == PURE_KIND_KEY) pure_app_input(&app, input, PURE_EDGE_DOWN);
            for (uint32_t v = 0; v < module_b.views; ++v) {
                double value;
                size_t item;
                if (module_b.view_type[v] == PURE_VIEW_TABLE) {
                    size_t rows = pure_app_rows(&app, v);
                    for (size_t row = 0; row < rows + 1; ++row) pure_app_row_item(&app, v, row, &item);
                } else if (module_b.view_table[v]) {
                    for (size_t row = 0; row < 4; ++row)
                        if (module_b.view_type[v] == PURE_VIEW_TEXT) pure_app_cell_text(&app, v, row, ignore_span, NULL);
                        else pure_app_cell(&app, v, row, &value);
                } else if (module_b.view_type[v] == PURE_VIEW_TEXT) pure_app_view_text(&app, v, ignore_span, NULL);
                else pure_app_view(&app, v, &value);
            }
        }
        pure_app_stop(&app);
    }
    printf("{\"%s_module_mutations\":20000,\"accepted_after_damage\":%u}\n", kind ? "filter" : "fold", accepted);
    }
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

/* Keyed lists: items in key order (texts by bytes, numbers by value), each
 * key once; found by key; a list that becomes keyed is put in order with
 * its indices following their items, or refused when two items share a key. */
enum { FIND = PURE_OP_LIST_FIND, HAS = PURE_OP_LIST_HAS, TCMP = PURE_OP_TEXT_COMPARE,
       PEOPLE = 1, K_AT = 2, SCORES = 3, K_NAME = 0, K_VISITS = 1u << 16 };
static const S keyed_nums[] = {{"n", 0}};
static const T keyed_states[] = {{"people", 3 | 1 << 8, 0}, {"at", 4, 1}, {"scores", 3 | 1 << 8, 1}};
static const T unkeyed_states[] = {{"people", 3, 0}, {"at", 4, 1}, {"scores", 3, 1}};
static const RF person_fields[] = {{"name", 1, 0}, {"visits", 0, 0}};
static const RF score_fields[] = {{"score", 0, 0}};
static const R keyed_records[] = {{"person", person_fields, 2}, {"score", score_fields, 1}};
static const char *const keyed_constants[] = {"", "bob", "m"};
static const I keyed_inputs[] = {
    {"typed", NULL, PURE_KIND_TEXT, false, NULL, 0}, {"seek", NULL, PURE_KIND_TEXT, false, NULL, 0},
    {"has", NULL, PURE_KIND_TEXT, false, NULL, 0}, KEY("drop", "Delete"), KEY("bump", "KeyB"),
    KEY("two", "KeyN"), KEY("zero", "KeyO"), KEY("minus_zero", "KeyM"), {"twice", NULL, PURE_KIND_TEXT, false, NULL, 0},
};
static const O keyed_code[] = {
    {TI, 0, 0}, {C, 0, 0}, {LA, PEOPLE, 0}, {TI, 0, 0}, {FIND, PEOPLE, 0}, {XS, K_AT, 0},        /* 0 typed: add, point at it */
    {TI, 0, 0}, {FIND, PEOPLE, 0}, {XS, K_AT, 0},                                               /* 6 seek */
    {TI, 0, 0}, {HAS, PEOPLE, 0}, {ST, 0, 0},                                                   /* 9 has */
    {XL, K_AT, 0}, {LR, PEOPLE, 0},                                                             /* 12 drop */
    {XL, K_AT, 0}, {XL, K_AT, 0}, {IG, PEOPLE | K_VISITS, 0}, {C, 0, 1}, {ADD, 0, 0},            /* 14 bump */
    {IS, PEOPLE | K_VISITS, 0},
    {C, 0, 2}, {LA, SCORES, 0}, {C, 0, 0}, {LA, SCORES, 0}, {C, 0, -0.0}, {LA, SCORES, 0},       /* 20 two, zero, minus_zero */
    {TI, 0, 0}, {C, 0, 0}, {LA, PEOPLE, 0}, {TI, 0, 0}, {C, 0, 0}, {LA, PEOPLE, 0},              /* 26 twice: always exists */
    {LC, PEOPLE, 0},                                                                            /* 32 view count */
    {XL, K_AT, 0}, {XN, 0, 0},                                                                  /* 33 view position */
    {XL, K_AT, 0}, {IG, PEOPLE | K_NAME, 0}, {TK, 1, 0}, {TCMP, PURE_EQUAL, 0},                  /* 35 view is_bob */
    {XL, K_AT, 0}, {IG, PEOPLE | K_NAME, 0}, {TK, 2, 0}, {TCMP, PURE_LESS, 0},                   /* 39 view before_m */
    {LC, SCORES, 0},                                                                            /* 43 view scores */
    {XL, K_AT, 0}, {IG, PEOPLE | K_NAME, 0},                                                    /* 44 view current */
};
static const H keyed_handlers[] = {{1, 0, 6}, {3, 6, 3}, {5, 9, 3}, {7, 12, 2}, {9, 14, 6}, {11, 20, 2}, {13, 22, 2},
                                   {15, 24, 2}, {17, 26, 6}};
static const V keyed_views[] = {{"count", 32, 1}, {"position", 33, 2}, {"is_bob", 35, 4}, {"before_m", 39, 4},
                                {"scores_count", 43, 1}};
static const V keyed_text_views[] = {{"current", 44, 2}};
/* Unkeyed, the same states: typed appends and points at the new last item. */
static const O unkeyed_code[] = {{TI, 0, 0}, {C, 0, 0}, {LA, PEOPLE, 0}, {XE, PEOPLE, 0}, {XS, K_AT, 0}};
static const H unkeyed_handlers[] = {{1, 0, 5}};

static Spec keyed_spec(const T *states, const O *code, size_t count, const H *handlers, size_t nhandlers) {
    bool views = code == keyed_code;
    return (Spec){keyed_nums, 1, keyed_inputs, LEN(keyed_inputs), keyed_views, views ? LEN(keyed_views) : 0, handlers,
                  nhandlers, code, count, NULL, 0, states, LEN(keyed_states), keyed_constants, LEN(keyed_constants),
                  keyed_text_views, views ? 1 : 0, keyed_records, 2, NULL, 0};
}

static PureResult type_text(PureApp *app, uint32_t input, const char *text) {
    return pure_app_text(app, input, (PureSpan){(const unsigned char *)text, strlen(text)});
}

/* A list's first field (text) of every item, comma separated. */
static const char *names_of(const PureApp *app, uint32_t list) {
    static char out[512];
    size_t used = 0;
    for (size_t at = 0; at < pure_list_count(app->lists[list]); ++at) {
        PureCell cell = pure_list_get(app->lists[list], at, 0);
        CHECK(used + cell.size + 2 < sizeof(out));
        if (at) out[used++] = ',';
        if (cell.size) memcpy(out + used, cell.text, cell.size);
        used += cell.size;
    }
    out[used] = 0;
    return out;
}

static void keyed(void) {
    CHECK(load(keyed_spec(keyed_states, keyed_code, LEN(keyed_code), keyed_handlers, LEN(keyed_handlers)), &module_a) == PURE_OK);
    PureApp app;
    CHECK(pure_app_start(&app, &module_a, (PureData){0}) == PURE_OK);
    for (const char *const *word = (const char *const[]){"m", "b", "z", "a", NULL}; *word; ++word)
        CHECK(type_text(&app, 0, *word) == PURE_OK);
    CHECK(!strcmp(names_of(&app, PEOPLE), "a,b,m,z") && app.values[K_AT] == 0 && !strcmp(view_text(&app, 5), "a"));
    /* A key once: the add fails, and so does an event whose second add repeats its first. */
    CHECK(type_text(&app, 0, "b") == PURE_CONFLICT && app.fault == PURE_FAULT_EXISTS);
    CHECK(type_text(&app, 8, "c") == PURE_CONFLICT && !strcmp(names_of(&app, PEOPLE), "a,b,m,z") && app.values[K_AT] == 0);
    CHECK(type_text(&app, 1, "z") == PURE_OK && app.values[K_AT] == 3 && !strcmp(view_text(&app, 5), "z"));
    CHECK(type_text(&app, 1, "q") == PURE_NOT_FOUND && app.fault == PURE_FAULT_NOT_FOUND && app.values[K_AT] == 3);
    CHECK(type_text(&app, 2, "m") == PURE_OK && app.values[0] == 1);
    CHECK(type_text(&app, 2, "c") == PURE_OK && app.values[0] == 0);
    CHECK(pure_app_input(&app, 4, PURE_EDGE_DOWN) == PURE_OK && pure_list_get(app.lists[PEOPLE], 3, 1).num == 1);
    CHECK(pure_app_input(&app, 3, PURE_EDGE_DOWN) == PURE_OK && !strcmp(names_of(&app, PEOPLE), "a,b,m"));
    CHECK(app.values[K_AT] == 2 && number_view(&app, 2) == 0 && number_view(&app, 3) == 0);  /* "m": not bob, not before m */
    CHECK(type_text(&app, 1, "b") == PURE_OK && number_view(&app, 2) == 0 && number_view(&app, 3) == 1);
    CHECK(type_text(&app, 0, "bob") == PURE_OK && number_view(&app, 2) == 1 && number_view(&app, 3) == 1);
    /* Bytes order texts: a text before the longer ones it begins, UTF-8 after ASCII, the empty text first. */
    CHECK(type_text(&app, 0, "\xea\xb0\x80") == PURE_OK && type_text(&app, 0, "ab") == PURE_OK);
    CHECK(type_text(&app, 0, "") == PURE_OK && type_text(&app, 0, "") == PURE_CONFLICT);
    CHECK(!strcmp(names_of(&app, PEOPLE), ",a,ab,b,bob,m,\xea\xb0\x80") && app.values[K_AT] == 0);
    /* Numbers order by value, and -0 is the key 0. */
    CHECK(pure_app_input(&app, 5, PURE_EDGE_DOWN) == PURE_OK && pure_app_input(&app, 6, PURE_EDGE_DOWN) == PURE_OK);
    CHECK(pure_app_input(&app, 7, PURE_EDGE_DOWN) == PURE_CONFLICT && number_view(&app, 4) == 2);
    CHECK(pure_list_get(app.lists[SCORES], 0, 0).num == 0 && !signbit(pure_list_get(app.lists[SCORES], 0, 0).num) &&
          pure_list_get(app.lists[SCORES], 1, 0).num == 2);

    /* Replacing: unkeyed, the list stays as it is and takes appends at its end. */
    CHECK(load(keyed_spec(unkeyed_states, unkeyed_code, LEN(unkeyed_code), unkeyed_handlers, 1), &module_b) == PURE_OK);
    CHECK(pure_app_replace(&app, &module_b) == PURE_OK && type_text(&app, 0, "c") == PURE_OK);
    CHECK(!strcmp(names_of(&app, PEOPLE), ",a,ab,b,bob,m,\xea\xb0\x80,c") && app.values[K_AT] == 7);
    CHECK(pure_app_point(&app, K_AT, 6) == PURE_OK);                     /* at the Korean name */
    /* Keyed again: put in order, the index following its item. */
    CHECK(pure_app_replace(&app, &module_a) == PURE_OK && !strcmp(names_of(&app, PEOPLE), ",a,ab,b,bob,c,m,\xea\xb0\x80"));
    CHECK(app.values[K_AT] == 7 && !strcmp(view_text(&app, 5), "\xea\xb0\x80"));
    /* Two items with one key cannot become keyed: the app keeps its module and items. */
    CHECK(pure_app_replace(&app, &module_b) == PURE_OK && type_text(&app, 0, "a") == PURE_OK);
    CHECK(pure_app_replace(&app, &module_a) == PURE_CONFLICT && app.module == &module_b && pure_list_count(app.lists[PEOPLE]) == 9);
    pure_app_stop(&app);

    /* Stored: a list that becomes keyed is stored in order, once. */
    char *path = new_store();
    CHECK(pure_app_start(&app, &module_b, pure_tree_data(path)) == PURE_OK);
    for (const char *const *word = (const char *const[]){"m", "b", "z", NULL}; *word; ++word)
        CHECK(type_text(&app, 0, *word) == PURE_OK);
    CHECK(pure_app_point(&app, K_AT, 1) == PURE_OK);
    pure_app_stop(&app);
    for (int round = 0; round < 2; ++round) {
        CHECK(pure_app_start(&app, &module_a, pure_tree_data(path)) == PURE_OK);
        CHECK(!strcmp(names_of(&app, PEOPLE), "b,m,z") && app.values[K_AT] == 0);
        pure_app_stop(&app);
    }
    CHECK(pure_app_start(&app, &module_b, pure_tree_data(path)) == PURE_OK && type_text(&app, 0, "m") == PURE_OK);
    pure_app_stop(&app);
    CHECK(pure_app_start(&app, &module_a, pure_tree_data(path)) == PURE_CONFLICT);
    CHECK(pure_app_start(&app, &module_b, pure_tree_data(path)) == PURE_OK && !strcmp(names_of(&app, PEOPLE), "b,m,z,m"));
    pure_app_stop(&app);
    remove_store(path);
    static const char *const words[] = {"a", "b", "ab", "", "\xea\xb0\x80", "zz"};
    for (uint32_t seed = 1; seed <= 3; ++seed)
        stored_equivalence(keyed_spec(keyed_states, keyed_code, LEN(keyed_code), keyed_handlers, LEN(keyed_handlers)),
                           words, LEN(words), seed * 0x9e3779b9u);

    /* What a keyed list and a text comparison may not be. */
    static const O insert[] = {{XF, PEOPLE, 0}, {TK, 0, 0}, {C, 0, 0}, {LI, PEOPLE, 0}};
    static const O set_key[] = {{XL, K_AT, 0}, {TK, 0, 0}, {IS, PEOPLE | K_NAME, 0}};
    static const O find_number[] = {{C, 0, 1}, {FIND, PEOPLE, 0}, {XS, K_AT, 0}};
    static const O find[] = {{TI, 0, 0}, {FIND, PEOPLE, 0}, {XS, K_AT, 0}};
    static const O relation[] = {{TI, 0, 0}, {TI, 0, 0}, {TCMP, PURE_NOT_EQUAL + 1, 0}, {ST, 0, 0}};
    static const O compare_numbers[] = {{C, 0, 1}, {C, 0, 1}, {TCMP, PURE_EQUAL, 0}, {ST, 0, 0}};
    static const T far_key[] = {{"people", 3 | 3 << 8, 0}, {"at", 4, 1}, {"scores", 3, 1}};
    static const T keyed_index[] = {{"people", 3, 0}, {"at", 4 | 1 << 8, 1}, {"scores", 3, 1}};
    static const struct { const T *states; const O *code; size_t count; const char *reason; } refused[] = {
        {keyed_states, insert, LEN(insert), "insert by position into a keyed list"},
        {keyed_states, set_key, LEN(set_key), "change of a keyed list's key"},
        {keyed_states, find_number, LEN(find_number), "operand type"},
        {unkeyed_states, find, LEN(find), "no such state"},
        {keyed_states, relation, LEN(relation), "no such state"},
        {keyed_states, compare_numbers, LEN(compare_numbers), "operand type"},
        {far_key, unkeyed_code, LEN(unkeyed_code), "state reference"},
        {keyed_index, unkeyed_code, LEN(unkeyed_code), "state reference"},
    };
    for (size_t i = 0; i < LEN(refused); ++i) {
        H handler = {1, 0, (uint32_t)refused[i].count};
        if (load(keyed_spec(refused[i].states, refused[i].code, refused[i].count, &handler, 1), &module_b) != PURE_INVALID ||
            !strstr(error, refused[i].reason)) {
            fprintf(stderr, "keyed case %zu: expected '%s', got '%s'\n", i, refused[i].reason, error);
            abort();
        }
    }
}

/* Damaged keyed modules: whatever loads keeps every keyed list in strict key
 * order and every index on an item. */
static void keyed_mutations(void) {
    static unsigned char valid[65536], bytes[65536];
    size_t size = build(valid, keyed_spec(keyed_states, keyed_code, LEN(keyed_code), keyed_handlers, LEN(keyed_handlers)));
    static const char *const words[] = {"a", "b", "ab", "", "\xea\xb0\x80"};
    uint32_t state = 0x6c8e9cf5u;
    unsigned accepted = 0;
    for (unsigned round = 0; round < 20000; ++round) {
        memcpy(bytes, valid, size);
        for (unsigned f = 0, flips = 1 + next_random(&state) % 3; f < flips; ++f)
            bytes[next_random(&state) % size] ^= (unsigned char)(1u << (next_random(&state) % 8));
        if (pure_module_load(bytes, size, &module_b, error, sizeof(error)) != PURE_OK) continue;
        ++accepted;
        PureApp app;
        if (pure_app_start(&app, &module_b, (PureData){0}) != PURE_OK) continue;
        for (unsigned e = 0; e < 24; ++e) {
            uint32_t input = next_random(&state) % module_b.inputs;
            if (module_b.input[input].kind == PURE_KIND_TEXT) type_text(&app, input, words[next_random(&state) % LEN(words)]);
            else if (module_b.input[input].kind == PURE_KIND_KEY) pure_app_input(&app, input, PURE_EDGE_DOWN);
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
                }
                if (module_b.state_type[s] != PURE_STATE_LIST || !module_b.state_key[s]) continue;
                unsigned key = module_b.state_key[s] - 1u;
                bool text = module_b.record_field_text[module_b.record[module_b.state_ref[s]].first + key];
                for (size_t at = 1; at < pure_list_count(app.lists[s]); ++at) {
                    PureCell a = pure_list_get(app.lists[s], at - 1, key), b = pure_list_get(app.lists[s], at, key);
                    if (!text) { CHECK(a.num < b.num); continue; }
                    size_t shorter = a.size < b.size ? a.size : b.size;
                    int order = shorter ? memcmp(a.text, b.text, shorter) : 0;
                    CHECK(order < 0 || (!order && a.size < b.size));
                }
            }
        }
        pure_app_stop(&app);
    }
    printf("{\"keyed_module_mutations\":20000,\"accepted_after_damage\":%u}\n", accepted);
}

/* ---- Snapshots. Everything an app's states and views show, as text (safe
 * on any thread): a reader of a snapshot must show what the app showed. */
typedef struct { char *text; size_t used, size; } Shown;

static bool show_span(void *context, PureSpan span) {
    Shown *s = context;
    CHECK(s->used + span.size < s->size);
    if (span.size) memcpy(s->text + s->used, span.data, span.size);
    s->used += span.size;
    return true;
}

static void show(Shown *s, const char *format, double value) {
    int n = snprintf(s->text + s->used, s->size - s->used, format, value);
    CHECK(n >= 0 && (size_t)n < s->size - s->used);
    s->used += (size_t)n;
}

static void describe(PureApp *app, Shown *s) {
    const PureModule *m = app->module;
    s->used = 0;
    for (uint32_t i = 0; i < m->states; ++i) {
        if (m->state_type[i] == PURE_STATE_TEXT)
            CHECK(pure_draft_read(app->texts[i], 0, pure_draft_size(app->texts[i]), show_span, s) == PURE_OK);
        else if (m->state_type[i] == PURE_STATE_LIST) {
            const PureRecord *r = &m->record[m->state_ref[i]];
            for (size_t at = 0; at < pure_list_count(app->lists[i]); ++at)
                for (unsigned f = 0; f < r->count; ++f) {
                    PureCell cell = pure_list_get(app->lists[i], at, f);
                    if (m->record_field_text[r->first + f]) show_span(s, (PureSpan){cell.text, cell.size});
                    else show(s, "%g", cell.num);
                    show(s, f + 1 < r->count ? "," : ";", 0);
                }
        } else show(s, "%g", app->values[i]);
        show(s, "|", 0);
    }
    for (uint32_t v = 0; v < m->views; ++v) {
        double value;
        if (m->view_table[v] || m->view_type[v] == PURE_VIEW_TABLE) continue;
        if (m->view_type[v] == PURE_VIEW_TEXT) {
            if (pure_app_view_text(app, v, show_span, s) != PURE_OK) show(s, "!", 0);
        } else if (pure_app_view(app, v, &value)) show(s, "%g", value);
        else show(s, "!", 0);
        show(s, "|", 0);
    }
    s->text[s->used] = 0;
}

/* One random event of the keyed app (see keyed), saved as it returns. */
static void keyed_event(PureApp *app, uint32_t *seed) {
    static const char *const names[] = {"a", "b", "ab", "", "\xea\xb0\x80", "zz", "bob", "m", "q", "c"};
    uint32_t input = next_random(seed) % app->module->inputs;
    if (app->module->input[input].kind == PURE_KIND_TEXT) type_text(app, input, names[next_random(seed) % LEN(names)]);
    else pure_app_input(app, input, PURE_EDGE_DOWN);
}

/* A snapshot shows the states of the save before it, as they were, however
 * the app goes on (items it changes or drops, a module it switches to, even
 * after it stops and its store closes); a change not saved yet refuses one,
 * and a save that fails is never shown. */
static size_t snapshot_retired, snapshot_reads;

static void snapshots_kept(Spec spec, const Spec *other, void (*event)(PureApp *, uint32_t *), unsigned steps) {
    enum { KEPT = 8, SIZE = 1 << 16 };
    static char texts[KEPT + 2][SIZE];
    char *path = new_store();
    PureStore *store;
    CHECK(load(spec, &module_a) == PURE_OK && pure_store_open(path, &store) == PURE_OK);
    CHECK(!other || load(*other, &module_b) == PURE_OK);
    PureApp app, reader = {0};
    CHECK(pure_app_start(&app, &module_a, pure_store_data(store)) == PURE_OK);
    PureSnapshot *kept[KEPT] = {NULL};
    Shown seen[KEPT], now = {texts[KEPT], 0, SIZE}, shown = {texts[KEPT + 1], 0, SIZE};
    for (unsigned k = 0; k < KEPT; ++k) seen[k] = (Shown){texts[k], 0, SIZE};
    uint32_t seed = 0x51a95u;
    for (unsigned step = 0; step < steps; ++step) {
        /* Late on, another module (the keyed app unkeyed), and back if it can. */
        if (other && (step == steps - 400 || step == steps - 300))
            CHECK(pure_app_replace(&app, app.module == &module_a ? &module_b : &module_a) != PURE_INVALID);
        event(&app, &seed);
        snapshot_retired += app.retired.count;
        PureSnapshot *made = pure_app_snapshot(&app);
        CHECK(made);
        describe(&app, &now);
        pure_app_read(&reader, made);
        describe(&reader, &shown);
        CHECK(!strcmp(now.text, shown.text));
        unsigned slot = next_random(&seed) % KEPT;
        if (kept[slot]) {
            pure_app_read(&reader, kept[slot]);
            describe(&reader, &shown);
            CHECK(!strcmp(seen[slot].text, shown.text));
            pure_snapshot_release(kept[slot]);
        }
        kept[slot] = made;
        memcpy(seen[slot].text, now.text, now.used + 1);
    }
    /* Deferred: an unsaved change refuses a snapshot, and a failed save
     * leaves the one before it true. */
    CHECK(pure_app_defer(&app, true));
    describe(&app, &now);
    PureResult saved = PURE_OK;
    for (unsigned tries = 0; tries < 64 && saved == PURE_OK; ++tries) {
        event(&app, &seed);
        if (!app.open) continue;
        CHECK(!pure_app_snapshot(&app));
        fail_stage = PURE_AFTER_HEADER;
        saved = pure_app_save(&app); /* OK when the event changed nothing */
        fail_stage = -1;
    }
    CHECK(saved == PURE_STORE_ERROR);
    PureSnapshot *made = pure_app_snapshot(&app);
    CHECK(made);
    pure_app_read(&reader, made);
    describe(&reader, &shown);
    CHECK(!strcmp(now.text, shown.text));
    pure_snapshot_release(made);
    /* The kept ones outlive the app and its store. */
    pure_app_stop(&app);
    pure_store_close(store);
    for (unsigned k = 0; k < KEPT; ++k) {
        pure_app_read(&reader, kept[k]);
        describe(&reader, &shown);
        CHECK(!strcmp(seen[k].text, shown.text));
        pure_snapshot_release(kept[k]);
    }
    pure_app_stop(&reader);
    remove_store(path);
}

/* Readers on two threads take the newest snapshot while the app applies
 * events and makes one after each save: each shows what the app showed. */
typedef struct {
    pthread_mutex_t lock;
    PureSnapshot *newest;
    char *seen;
    bool done;
    unsigned reads;
} Published;

static void *read_newest(void *context) {
    Published *p = context;
    PureApp reader = {0};
    static _Thread_local char a[1 << 16], b[1 << 16];
    Shown expected = {a, 0, sizeof(a)}, shown = {b, 0, sizeof(b)};
    for (bool done = false; !done;) {
        pthread_mutex_lock(&p->lock);
        PureSnapshot *s = p->newest;
        if (s) {
            pure_snapshot_hold(s);
            strcpy(expected.text, p->seen);
        }
        done = p->done;
        pthread_mutex_unlock(&p->lock);
        if (!s) continue;
        pure_app_read(&reader, s);
        describe(&reader, &shown);
        CHECK(!strcmp(expected.text, shown.text));
        pure_snapshot_release(s);
        __atomic_add_fetch(&p->reads, 1, __ATOMIC_RELAXED);
    }
    pure_app_stop(&reader);
    return NULL;
}

static void snapshots_read(Spec spec, void (*event)(PureApp *, uint32_t *), unsigned steps) {
    static char seen[1 << 16], now[1 << 16];
    char *path = new_store();
    PureStore *store;
    CHECK(load(spec, &module_a) == PURE_OK && pure_store_open(path, &store) == PURE_OK);
    PureApp app;
    CHECK(pure_app_start(&app, &module_a, pure_store_data(store)) == PURE_OK);
    Published p = {.lock = PTHREAD_MUTEX_INITIALIZER, .seen = seen};
    pthread_t readers[2];
    for (unsigned r = 0; r < 2; ++r) CHECK(pthread_create(&readers[r], NULL, read_newest, &p) == 0);
    Shown shown = {now, 0, sizeof(now)};
    uint32_t seed = 0x7eadu;
    for (unsigned step = 0; step < steps; ++step) {
        event(&app, &seed);
        PureSnapshot *made = pure_app_snapshot(&app);
        CHECK(made);
        describe(&app, &shown);
        pthread_mutex_lock(&p.lock);
        PureSnapshot *old = p.newest;
        p.newest = made;
        memcpy(seen, shown.text, shown.used + 1);
        pthread_mutex_unlock(&p.lock);
        pure_snapshot_release(old);
    }
    pthread_mutex_lock(&p.lock);
    p.done = true;
    pthread_mutex_unlock(&p.lock);
    for (unsigned r = 0; r < 2; ++r) CHECK(pthread_join(readers[r], NULL) == 0);
    CHECK(p.reads > steps / 4);
    snapshot_reads += p.reads;
    pure_snapshot_release(p.newest);
    pure_app_stop(&app);
    pure_store_close(store);
    remove_store(path);
}

static void crash_event(PureApp *app, uint32_t *seed) {
    (void)seed;
    CHECK(pure_app_input(app, 0, PURE_EDGE_DOWN) == PURE_OK);
}

static void snapshots(void) {
    Spec keyed = keyed_spec(keyed_states, keyed_code, LEN(keyed_code), keyed_handlers, LEN(keyed_handlers));
    Spec crashing = {crash_nums, 1, crash_inputs, 1, NULL, 0, crash_handlers, 1, crash_code, LEN(crash_code), NULL, 0,
                     crash_states, 4, crash_constants, 2, NULL, 0, crash_records, 1, NULL, 0};
    Spec unkeyed = keyed_spec(unkeyed_states, unkeyed_code, LEN(unkeyed_code), unkeyed_handlers, 1);
    snapshots_kept(keyed, &unkeyed, keyed_event, 3000);
    snapshots_kept(crashing, NULL, crash_event, 300);
    snapshots_read(keyed, keyed_event, 3000);
    snapshots_read(crashing, crash_event, 300);
    /* Without stored states (texts only in memory) there is no snapshot. */
    CHECK(load(crashing, &module_a) == PURE_OK);
    PureApp app;
    CHECK(pure_app_start(&app, &module_a, (PureData){0}) == PURE_OK && !pure_app_snapshot(&app));
    pure_app_stop(&app);
    CHECK(snapshot_retired > 100);
    printf("{\"snapshots\":6600,\"items_retired\":%zu,\"reads_on_threads\":%zu}\n", snapshot_retired, snapshot_reads);
}

/* A message of a text, a number, bytes and a u32, in that order: the prefix
 * holds the text's length, the number and the u32; the text's bytes come
 * first after it and the bytes (the last variable field) take the rest. A
 * length past the payload, a text that is not UTF-8 or a number that is not
 * finite fails the message. */
enum { TSET = PURE_OP_TEXT_SET };
static void message_fields(void) {
    static const S nums[] = {{"n", 0}};
    static const T texts[] = {{"title", 1, 0}};
    static const char *const constants[] = {""};
    static const F fields[] = {{"title", PURE_FIELD_TEXT}, {"points", PURE_FIELD_NUMBER}, {"note", PURE_FIELD_BYTES},
                               {"count", PURE_FIELD_U32}};
    static const I inputs[] = {{"post", NULL, PURE_KIND_MESSAGE, false, fields, 4}};
    static const O code[] = {{FIELD, 1, 0}, {FIELD, 3, 0}, {ADD, 0, 0}, {ST, 0, 0}, {FIELD, 0, 0}, {TSET, 1, 0},
                             {FIELD, 2, 0}, {REPLY, 0, 0}};
    static const H handlers[] = {{1, 0, 8}};
    Spec spec = {nums, 1, inputs, 1, NULL, 0, handlers, 1, code, LEN(code), NULL, 0, texts, 1, constants, 1, NULL, 0,
                 NULL, 0, NULL, 0};
    if (load(spec, &module_a) != PURE_OK) { fprintf(stderr, "%s\n", error); abort(); }
    CHECK(module_a.input[0].prefix == 16 && module_a.field[0].offset == 0 && module_a.field[1].offset == 4 &&
          module_a.field[2].offset == PURE_FIELD_REST && module_a.field[3].offset == 12);
    PureApp app;
    CHECK(pure_app_start(&app, &module_a, (PureData){0}) == PURE_OK);
    unsigned char payload[64];
    /* title "가나" (6 bytes), points 2.5, count 3, then the title's bytes and note "xyz". */
    size_t size = 0;
    uint32_t title_size = 6, count = 3;
    double points = 2.5;
    memcpy(payload, &title_size, 4);
    memcpy(payload + 4, &points, 8);
    memcpy(payload + 12, &count, 4);
    memcpy(payload + 16, "\xea\xb0\x80\xeb\x82\x98" "xyz", 9);
    size = 25;
    CHECK(pure_app_message(&app, 0, NULL, -1, (PureSpan){payload, size}) == PURE_OK);
    CHECK(app.values[0] == 5.5 && !strcmp(state_text(&app, 1), "\xea\xb0\x80\xeb\x82\x98") && replied(&app, "xyz"));
    pure_app_finish(&app);
    /* Empty variable fields. */
    title_size = 0;
    memcpy(payload, &title_size, 4);
    CHECK(pure_app_message(&app, 0, NULL, -1, (PureSpan){payload, 16}) == PURE_OK && !*state_text(&app, 1) && replied(&app, ""));
    pure_app_finish(&app);
    /* A title longer than what is there, a title that is not UTF-8, a number that is not finite, a short prefix. */
    title_size = 10;
    memcpy(payload, &title_size, 4);
    CHECK(pure_app_message(&app, 0, NULL, -1, (PureSpan){payload, 25}) == PURE_LIMIT && app.fault == PURE_FAULT_LIMIT);
    title_size = 2;
    memcpy(payload, &title_size, 4);
    memcpy(payload + 16, "\xff\xfe", 2);
    CHECK(pure_app_message(&app, 0, NULL, -1, (PureSpan){payload, 18}) == PURE_INVALID && app.fault == PURE_FAULT_ENCODING);
    title_size = 0;
    points = NAN;
    memcpy(payload, &title_size, 4);
    memcpy(payload + 4, &points, 8);
    CHECK(pure_app_message(&app, 0, NULL, -1, (PureSpan){payload, 16}) == PURE_INVALID && app.fault == PURE_FAULT_NON_FINITE);
    CHECK(pure_app_message(&app, 0, NULL, -1, (PureSpan){payload, 15}) == PURE_LIMIT);
    CHECK(app.values[0] == 5.5 && !*state_text(&app, 1)); /* failed messages changed nothing */
    pure_app_stop(&app);
    /* A text field is text and a bytes field bytes. */
    static const O text_as_bytes[] = {{FIELD, 0, 0}, {LEN_, 0, 0}, {ST, 0, 0}};
    static const O bytes_as_text[] = {{FIELD, 2, 0}, {TSET, 1, 0}};
    static const H three[] = {{1, 0, 3}}, two[] = {{1, 0, 2}};
    Spec bad = spec;
    bad.o = text_as_bytes; bad.no = 3; bad.h = three;
    CHECK(load(bad, &module_b) == PURE_INVALID && strstr(error, "operand type"));
    bad.o = bytes_as_text; bad.no = 2; bad.h = two;
    CHECK(load(bad, &module_b) == PURE_INVALID && strstr(error, "operand type"));

    /* Placed by the host: the variable fields wherever they are in the
     * payload (here a JSON body after the prefix), in any order; the
     * prefix's lengths are not read. */
    CHECK(pure_app_start(&app, &module_a, (PureData){0}) == PURE_OK);
    static const char body[] = "{\"note\":\"xyz\",\"title\":\"\xea\xb0\x80\xeb\x82\x98\"}";
    points = 2.5;
    title_size = 999;
    memcpy(payload, &title_size, 4);
    memcpy(payload + 4, &points, 8);
    memcpy(payload + 12, &count, 4);
    memcpy(payload + 16, body, sizeof(body) - 1);
    size = 16 + sizeof(body) - 1;
    uint32_t placed[8] = {16 + 23, 6, 0, 0, 16 + 9, 3, 0, 0};
    CHECK(pure_app_message_fields(&app, 0, NULL, -1, (PureSpan){payload, size}, placed) == PURE_OK);
    CHECK(app.values[0] == 5.5 && !strcmp(state_text(&app, 1), "\xea\xb0\x80\xeb\x82\x98") && replied(&app, "xyz"));
    pure_app_finish(&app);
    /* A field that starts or ends past the payload, or a title that is not UTF-8 there. */
    placed[0] = (uint32_t)size + 1;
    CHECK(pure_app_message_fields(&app, 0, NULL, -1, (PureSpan){payload, size}, placed) == PURE_LIMIT);
    placed[0] = (uint32_t)size - 2;
    placed[1] = 3;
    CHECK(pure_app_message_fields(&app, 0, NULL, -1, (PureSpan){payload, size}, placed) == PURE_LIMIT);
    placed[0] = 16 + 24;
    placed[1] = 2;
    CHECK(pure_app_message_fields(&app, 0, NULL, -1, (PureSpan){payload, size}, placed) == PURE_INVALID &&
          app.fault == PURE_FAULT_ENCODING);
    CHECK(app.values[0] == 5.5);
    pure_app_stop(&app);

    /* Fields in groups: a name is a path of identifiers (user.name), and
     * never both a field and a group. */
    static const F grouped[] = {{"user.name", PURE_FIELD_TEXT}, {"user.age", PURE_FIELD_U32}, {"plan", PURE_FIELD_TEXT}};
    static const I grouped_inputs[] = {{"post", NULL, PURE_KIND_MESSAGE, false, grouped, 3}};
    static const O grouped_code[] = {{FIELD, 1, 0}, {ST, 0, 0}, {FIELD, 0, 0}, {TSET, 1, 0}};
    static const H grouped_handlers[] = {{1, 0, 4}};
    Spec groups = spec;
    groups.in = grouped_inputs; groups.o = grouped_code; groups.no = LEN(grouped_code); groups.h = grouped_handlers;
    CHECK(load(groups, &module_b) == PURE_OK && !strcmp(module_b.field[0].name, "user.name"));
    static const char *const refused_paths[] = {"user..name", ".user", "user.", "user.1st", "user name"};
    for (size_t i = 0; i < LEN(refused_paths); ++i) {
        F renamed[3] = {grouped[0], grouped[1], grouped[2]};
        renamed[0].name = refused_paths[i];
        I renamed_inputs[1] = {{"post", NULL, PURE_KIND_MESSAGE, false, renamed, 3}};
        groups.in = renamed_inputs;
        CHECK(load(groups, &module_b) == PURE_INVALID && strstr(error, "message field"));
    }
    F both[3] = {{"user", PURE_FIELD_TEXT}, grouped[1], grouped[2]};
    I both_inputs[1] = {{"post", NULL, PURE_KIND_MESSAGE, false, both, 3}};
    groups.in = both_inputs;
    CHECK(load(groups, &module_b) == PURE_INVALID && strstr(error, "field and group"));
}

/* ---- Connections between apps (docs/research/078-app-connections.md): a
 * send is part of its event and is seen once saved; a receiver applies each
 * numbered message once, counts a refused one as heard, and forgets what a
 * failed save undid. */
enum { SEND = PURE_OP_SEND };
typedef struct { uint32_t output; uint64_t seq; unsigned char payload[64]; size_t size; } Sent;
typedef struct { Sent items[16]; size_t count; } Outbox;

static bool collect_sent(void *context, uint32_t output, uint64_t seq, PureSpan payload) {
    Outbox *o = context;
    CHECK(o->count < 16 && payload.size <= 64);
    Sent *s = &o->items[o->count++];
    *s = (Sent){output, seq, {0}, payload.size};
    memcpy(s->payload, payload.data, payload.size);
    return true;
}

static Outbox outbox_of(const PureApp *app) {
    Outbox o = {.count = 0};
    pure_app_outbox(app, collect_sent, &o);
    return o;
}

/* buy/fail {item: text, count: u32}: the count at 0, then the item. */
static PureResult buy(PureApp *app, uint32_t input, const char *item, uint32_t count) {
    unsigned char payload[64];
    size_t length = strlen(item);
    memcpy(payload, &count, 4);
    memcpy(payload + 4, item, length);
    PureResult result = pure_app_message(app, input, NULL, -1, (PureSpan){payload, 4 + length});
    pure_app_finish(app);
    return result;
}

/* order {item: text, count: u32, total: number}: count, total, then item. */
static PureResult deliver(PureApp *app, const char *source, uint64_t seq, const char *item, uint32_t count) {
    unsigned char payload[64];
    size_t length = strlen(item);
    double total = 0;
    memcpy(payload, &count, 4);
    memcpy(payload + 4, &total, 8);
    memcpy(payload + 12, item, length);
    PureResult result = pure_app_deliver(app, 0, source, seq, (PureSpan){payload, 12 + length});
    pure_app_finish(app);
    return result;
}

static void links(void) {
    static const S sender_nums[] = {{"n", 0}};
    static const F order_fields[] = {{"item", PURE_FIELD_TEXT}, {"count", PURE_FIELD_U32}};
    static const F ordered_fields[] = {{"item", PURE_FIELD_TEXT}, {"count", PURE_FIELD_U32}, {"total", PURE_FIELD_NUMBER}};
    static const I sender_inputs[] = {{"buy", NULL, PURE_KIND_MESSAGE, false, order_fields, 2},
                                      {"fail", NULL, PURE_KIND_MESSAGE, false, order_fields, 2},
                                      {"ordered", NULL, PURE_KIND_OUTPUT, false, ordered_fields, 3}};
    static const O sender_code[] = {{L, 0, 0}, {C, 0, 1}, {ADD, 0, 0}, {ST, 0, 0}, {FIELD, 0, 0}, {FIELD, 1, 0},
                                    {L, 0, 0}, {SEND, 2, 0},                                           /* buy */
                                    {FIELD, 0, 0}, {FIELD, 1, 0}, {C, 0, 0}, {SEND, 2, 0}, {C, 0, 0}, {REQ, 0, 0}}; /* fail */
    static const H sender_handlers[] = {{1, 0, 8}, {3, 8, 6}};
    Spec sender = {sender_nums, 1, sender_inputs, 3, NULL, 0, sender_handlers, 2, sender_code, LEN(sender_code),
                   NULL, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0};
    static const S receiver_nums[] = {{"stock", 0}};
    static const T receiver_texts[] = {{"last", 1, 0}};
    static const char *const receiver_constants[] = {""};
    static const I receiver_inputs[] = {{"order", NULL, PURE_KIND_MESSAGE, false, ordered_fields, 3}};
    static const O receiver_code[] = {{FIELD, 1, 0}, {L, 0, 0}, {ADD, 0, 0}, {ST, 0, 0}, {FIELD, 0, 0}, {TSET, 1, 0},
                                      {FIELD, 1, 0}, {C, 0, 100}, {LT, 0, 0}, {REQ, 0, 0}}; /* refuses 100 or more */
    static const H receiver_handlers[] = {{1, 0, 10}};
    Spec receiver = {receiver_nums, 1, receiver_inputs, 1, NULL, 0, receiver_handlers, 1, receiver_code,
                     LEN(receiver_code), NULL, 0, receiver_texts, 1, receiver_constants, 1, NULL, 0, NULL, 0, NULL, 0};
    if (load(sender, &module_a) != PURE_OK || load(receiver, &module_b) != PURE_OK) { fprintf(stderr, "%s\n", error); abort(); }
    CHECK(module_a.input[2].prefix == 12);

    /* In memory: a send is its event's; the receiver applies it once per source. */
    PureApp a, b;
    CHECK(pure_app_start(&a, &module_a, (PureData){0}) == PURE_OK && pure_app_start(&b, &module_b, (PureData){0}) == PURE_OK);
    CHECK(buy(&a, 0, "pen", 2) == PURE_OK && buy(&a, 1, "ink", 3) == PURE_LIMIT);
    Outbox o = outbox_of(&a);
    CHECK(o.count == 1 && o.items[0].output == 2 && o.items[0].seq == 1 && o.items[0].size == 15 && a.values[0] == 1);
    PureSpan sent = {o.items[0].payload, o.items[0].size};
    CHECK(pure_app_deliver(&b, 0, "orders.ordered", 1, sent) == PURE_OK && !b.duplicate && b.values[0] == 2 &&
          !strcmp(state_text(&b, 1), "pen"));
    pure_app_finish(&b);
    CHECK(pure_app_deliver(&b, 0, "orders.ordered", 1, sent) == PURE_OK && b.duplicate && b.values[0] == 2);
    CHECK(deliver(&b, "other", 1, "cup", 5) == PURE_OK && !b.duplicate && b.values[0] == 7);
    CHECK(deliver(&b, "orders.ordered", 3, "hat", 1) == PURE_OK && b.values[0] == 8); /* numbers may skip */
    CHECK(deliver(&b, "orders.ordered", 2, "old", 1) == PURE_OK && b.duplicate && b.values[0] == 8);
    /* Refused (count 100): delivered all the same, never applied again. */
    CHECK(deliver(&b, "other", 2, "big", 100) == PURE_LIMIT && b.values[0] == 7 + 1);
    CHECK(deliver(&b, "other", 2, "big", 1) == PURE_OK && b.duplicate && b.values[0] == 8);
    /* Bad deliveries change nothing. */
    CHECK(deliver(&b, "", 4, "x", 1) == PURE_INVALID && deliver(&b, "a b", 4, "x", 1) == PURE_INVALID);
    CHECK(deliver(&b, "other", 0, "x", 1) == PURE_INVALID && pure_app_deliver(&a, 2, "s", 1, sent) == PURE_INVALID);
    CHECK(pure_app_message(&a, 2, NULL, -1, sent) == PURE_INVALID);
    /* Trimmed, a send is gone at once for the host and from the store at the next save. */
    pure_app_trim(&a, 2, 1);
    CHECK(outbox_of(&a).count == 0 && pure_app_save(&a) == PURE_OK && a.sent_count == 0);
    CHECK(buy(&a, 0, "cap", 1) == PURE_OK && outbox_of(&a).items[0].seq == 2);
    pure_app_stop(&a);
    pure_app_stop(&b);

    /* Stored: sends and heard numbers survive restarts; a deferred burst's sends
     * appear with its save, and a failed save takes them and their numbers back. */
    char path_a[64], path_b[64];
    snprintf(path_a, sizeof(path_a), "%s", new_store());
    snprintf(path_b, sizeof(path_b), "%s", new_store());
    CHECK(pure_app_start(&a, &module_a, pure_tree_data(path_a)) == PURE_OK);
    CHECK(pure_app_start(&b, &module_b, pure_tree_data(path_b)) == PURE_OK);
    CHECK(buy(&a, 0, "pen", 2) == PURE_OK && buy(&a, 0, "ink", 3) == PURE_OK);
    pure_app_stop(&a);
    CHECK(pure_app_start(&a, &module_a, pure_tree_data(path_a)) == PURE_OK);
    o = outbox_of(&a);
    CHECK(o.count == 2 && o.items[0].seq == 1 && o.items[1].seq == 2 && a.values[0] == 2);
    CHECK(pure_app_defer(&a, true));
    for (int i = 0; i < 3; ++i) CHECK(buy(&a, 0, "box", 1) == PURE_OK);
    CHECK(outbox_of(&a).count == 2 && pure_app_save(&a) == PURE_OK);
    o = outbox_of(&a);
    CHECK(o.count == 5 && o.items[4].seq == 5);
    CHECK(buy(&a, 0, "lost", 1) == PURE_OK);
    pure_app_trim(&a, 2, 3);
    fail_stage = PURE_AFTER_HEADER;
    CHECK(pure_app_save(&a) == PURE_STORE_ERROR);
    fail_stage = -1;
    o = outbox_of(&a);
    CHECK(o.count == 5 && o.items[0].seq == 1 && a.values[0] == 5);         /* the trim and the send undone */
    CHECK(buy(&a, 0, "next", 1) == PURE_OK && pure_app_save(&a) == PURE_OK);
    o = outbox_of(&a);
    CHECK(o.count == 6 && o.items[5].seq == 6);                              /* numbered on from 5 */
    pure_app_trim(&a, 2, 4);
    CHECK(pure_app_save(&a) == PURE_OK);
    pure_app_stop(&a);
    CHECK(pure_app_start(&a, &module_a, pure_tree_data(path_a)) == PURE_OK);
    o = outbox_of(&a);
    CHECK(o.count == 2 && o.items[0].seq == 5 && o.items[1].seq == 6);
    /* The receiver: heard numbers stay heard after a restart; a refused one too. */
    CHECK(deliver(&b, "orders.ordered", 1, "pen", 2) == PURE_OK && deliver(&b, "x", 1, "big", 100) == PURE_LIMIT);
    pure_app_stop(&b);
    CHECK(pure_app_start(&b, &module_b, pure_tree_data(path_b)) == PURE_OK && b.values[0] == 2);
    CHECK(deliver(&b, "orders.ordered", 1, "pen", 2) == PURE_OK && b.duplicate && b.values[0] == 2);
    CHECK(deliver(&b, "x", 1, "big", 1) == PURE_OK && b.duplicate);
    /* A save that fails forgets what it heard. */
    CHECK(pure_app_defer(&b, true) && deliver(&b, "orders.ordered", 2, "ink", 3) == PURE_OK && b.values[0] == 5);
    fail_stage = PURE_AFTER_HEADER;
    CHECK(pure_app_save(&b) == PURE_STORE_ERROR && b.values[0] == 2);
    fail_stage = -1;
    CHECK(deliver(&b, "orders.ordered", 2, "ink", 3) == PURE_OK && !b.duplicate && b.values[0] == 5);
    CHECK(pure_app_save(&b) == PURE_OK);
    pure_app_stop(&b);

    /* Replacing: a module without the output is refused while sends are left
     * on it; once they are trimmed it replaces, and the output's number is
     * kept for a module that has it again. */
    static const I plain_inputs[] = {{"buy", NULL, PURE_KIND_MESSAGE, false, order_fields, 2}};
    static const O plain_code[] = {{L, 0, 0}, {C, 0, 1}, {ADD, 0, 0}, {ST, 0, 0}};
    static const H plain_handlers[] = {{1, 0, 4}};
    Spec plain = {sender_nums, 1, plain_inputs, 1, NULL, 0, plain_handlers, 1, plain_code, LEN(plain_code),
                  NULL, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0};
    CHECK(load(plain, &module_b) == PURE_OK);
    CHECK(pure_app_replace(&a, &module_b) == PURE_CONFLICT && a.module == &module_a);
    pure_app_trim(&a, 2, 6);
    CHECK(pure_app_save(&a) == PURE_OK && pure_app_replace(&a, &module_b) == PURE_OK);
    CHECK(buy(&a, 0, "plain", 1) == PURE_OK && pure_app_save(&a) == PURE_OK);
    pure_app_stop(&a);
    CHECK(pure_app_start(&a, &module_b, pure_tree_data(path_a)) == PURE_OK); /* the number kept in the store */
    CHECK(pure_app_replace(&a, &module_a) == PURE_OK && buy(&a, 0, "back", 1) == PURE_OK && pure_app_save(&a) == PURE_OK);
    o = outbox_of(&a);
    CHECK(o.count == 1 && o.items[0].seq == 7);
    /* A stored send whose output the module lacks refuses the start. */
    pure_app_stop(&a);
    CHECK(pure_app_start(&a, &module_b, pure_tree_data(path_a)) == PURE_CONFLICT);
    remove_store(path_a);
    remove_store(path_b);

    /* A process that dies at each commit stage leaves an event and its send
     * together or neither, and a delivery and its effect together or neither. */
    snprintf(path_a, sizeof(path_a), "%s", new_store());
    snprintf(path_b, sizeof(path_b), "%s", new_store());
    CHECK(load(receiver, &module_b) == PURE_OK);
    uint64_t heard = 0;
    size_t kept[2] = {0, 0}; /* sends and deliveries a death left in place */
    for (int round = 0; round < 2; ++round)
    for (int stage = PURE_AFTER_HEADER; stage <= PURE_BEFORE_DIR_SYNC; ++stage) {
        pid_t child = fork();
        CHECK(child >= 0);
        if (!child) {
            PureApp doomed, taker;
            if (pure_app_start(&doomed, &module_a, pure_tree_data(path_a)) != PURE_OK ||
                pure_app_start(&taker, &module_b, pure_tree_data(path_b)) != PURE_OK) _exit(1);
            crash = 1;
            fail_stage = stage;
            if (round) buy(&doomed, 0, "crash", 1);
            else deliver(&taker, "orders.ordered", heard + 1, "crash", 1);
            _exit(0);
        }
        int status;
        CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 77);
        CHECK(pure_app_start(&a, &module_a, pure_tree_data(path_a)) == PURE_OK);
        CHECK(pure_app_start(&b, &module_b, pure_tree_data(path_b)) == PURE_OK);
        o = outbox_of(&a);
        CHECK(o.count == (size_t)a.values[0] && (!o.count || o.items[o.count - 1].seq == o.count));
        heard = b.heard_count ? b.heard[0].seq : 0;
        CHECK(b.values[0] == (double)heard);
        kept[0] = (size_t)heard;
        kept[1] = o.count;
        pure_app_stop(&a);
        pure_app_stop(&b);
    }
    CHECK(kept[0] == 1 && kept[1] == 1); /* only a death after the head was written keeps it */
    remove_store(path_a);
    remove_store(path_b);

    /* What an output and SEND may not be. */
    const struct { const O *code; size_t count; const char *reason; } refused[] = {
        {(const O[]){{FIELD, 1, 0}, {FIELD, 0, 0}, {L, 0, 0}, {SEND, 2, 0}}, 4, "operand type"},
        {(const O[]){{FIELD, 0, 0}, {FIELD, 1, 0}, {L, 0, 0}, {SEND, 1, 0}}, 4, "no such state"},
        {(const O[]){{FIELD, 0, 0}, {FIELD, 1, 0}, {SEND, 2, 0}}, 3, "stack underflow"},
    };
    for (size_t i = 0; i < LEN(refused); ++i) {
        Spec bad = sender;
        H handler = {1, 0, (uint32_t)refused[i].count};
        bad.o = refused[i].code; bad.no = refused[i].count; bad.h = &handler; bad.nh = 1;
        if (load(bad, &module_b) != PURE_INVALID || !strstr(error, refused[i].reason)) {
            fprintf(stderr, "links case %zu: expected '%s', got '%s'\n", i, refused[i].reason, error);
            abort();
        }
    }
    H on_output[] = {{1, 0, 8}, {5, 8, 6}};
    Spec bad = sender;
    bad.h = on_output;
    CHECK(load(bad, &module_b) == PURE_INVALID && strstr(error, "handler event"));
}

/* A timer's tick sees the time the host set; floor and a floored modulo
 * take it apart. now is for handlers only. */
enum { NOW = PURE_OP_NOW, FLOOR = PURE_OP_FLOOR, MOD = PURE_OP_MOD };
static void timers(void) {
    static const S nums[] = {{"clock", 0}, {"seconds", 0}, {"ticks", 0}, {"rest", 0}};
    static const I inputs[] = {TIMER("tick", 1000), KEY("left", "KeyL"), KEY("zero", "KeyZ")};
    static const O code[] = {
        {NOW, 0, 0}, {ST, 0, 0}, {NOW, 0, 0}, {C, 0, 1000}, {DIV, 0, 0}, {FLOOR, 0, 0}, {C, 0, 60}, {MOD, 0, 0},
        {ST, 1, 0}, {L, 2, 0}, {C, 0, 1}, {ADD, 0, 0}, {ST, 2, 0},                      /* 0 tick */
        {C, 0, -7}, {C, 0, 3}, {MOD, 0, 0}, {C, 0, 7}, {C, 0, -3}, {MOD, 0, 0}, {C, 0, 10}, {MUL, 0, 0},
        {ADD, 0, 0}, {ST, 3, 0},                                                         /* 13 left: 2 + -20 */
        {C, 0, 1}, {C, 0, 0}, {MOD, 0, 0}, {ST, 3, 0},                                   /* 23 zero */
    };
    static const H handlers[] = {{1, 0, 13}, {3, 13, 10}, {5, 23, 4}};
    static const V one_view[] = {{"v", 0, 1}};
    Spec spec = SPEC(nums, inputs, one_view, handlers, code);
    spec.nv = 0;
    if (load(spec, &module_a) != PURE_OK) { fprintf(stderr, "%s\n", error); abort(); }
    CHECK(module_a.input[0].kind == PURE_KIND_TIMER && module_a.input[0].period == 1000);
    PureApp app;
    CHECK(pure_app_start(&app, &module_a, (PureData){0}) == PURE_OK);
    app.now = 1700000123456.0;
    CHECK(pure_app_tick(&app, 0) == PURE_OK && app.values[0] == 1700000123456.0 &&
          app.values[1] == (double)(1700000123 % 60) && app.values[2] == 1);
    CHECK(pure_app_tick(&app, 1) == PURE_INVALID && pure_app_input(&app, 0, PURE_EDGE_DOWN) == PURE_INVALID);
    CHECK(pure_app_input(&app, 1, PURE_EDGE_DOWN) == PURE_OK && app.values[3] == 2 - 20);
    /* A zero takes the divisor's sign too. */
    static const O zeros[] = {{C, 0, -6}, {C, 0, 3}, {MOD, 0, 0}, {ST, 0, 0}, {C, 0, 6}, {C, 0, -3}, {MOD, 0, 0}, {ST, 1, 0}};
    static const H zero_handler[] = {{3, 0, 8}};
    Spec signs = spec;
    signs.o = zeros; signs.no = 8; signs.h = zero_handler; signs.nh = 1;
    CHECK(load(signs, &module_b) == PURE_OK);
    PureApp zero;
    CHECK(pure_app_start(&zero, &module_b, (PureData){0}) == PURE_OK && pure_app_input(&zero, 1, PURE_EDGE_DOWN) == PURE_OK);
    CHECK(zero.values[0] == 0 && !signbit(zero.values[0]) && zero.values[1] == 0 && signbit(zero.values[1]));
    pure_app_stop(&zero);
    CHECK(pure_app_input(&app, 2, PURE_EDGE_DOWN) == PURE_INVALID && app.fault == PURE_FAULT_DIVIDE);
    pure_app_stop(&app);
    /* A timer's period is 1 ms to a day, and it has nothing else; now is for handlers. */
    static const I no_period[] = {TIMER("tick", 0)}, too_long[] = {TIMER("tick", PURE_TIMER_LONGEST + 1)};
    static const O now_view[] = {{NOW, 0, 0}};
    Spec bad = spec;
    bad.in = no_period; bad.ni = 1; bad.nh = 1; bad.o = code; bad.no = 13;
    CHECK(load(bad, &module_b) == PURE_INVALID && strstr(error, "timer input"));
    bad.in = too_long;
    CHECK(load(bad, &module_b) == PURE_INVALID && strstr(error, "timer input"));
    bad = SPEC(nums, inputs, one_view, handlers, now_view);
    bad.nh = 0;
    CHECK(load(bad, &module_b) == PURE_INVALID && strstr(error, "view with an effect"));
}

int main(void) {
    behaviour();
    faults();
    rejections();
    mutations();
    messages();
    message_fields();
    timers();
    message_mutations();
    texts();
    marks();
    text_mutations();
    lists();
    folds();
    filters();
    keyed();
    keyed_mutations();
    snapshots();
    links();
    fold_mutations();
    pointing();
    list_mutations();
    tables();
    table_mutations();
    stored_states();
    deferred();
    printf("{\"sizeof_module\":%zu,\"sizeof_app\":%zu}\n", sizeof(PureModule), sizeof(PureApp));
    puts("{\"module_tests\":\"verified-load-atomic-events-faults-signed-zero-if-else-replace-by-name-rejections-mutations-messages-documents-typed-verification-text-cursors-utf8-views-rollback-replace-lists-indices-undo-migration-joins-tables-columns-pick-slide-stored-equivalence-crashes-frames-messages-migration-deferred-bursts-pointing-marks-folds-filters-keyed-lists-text-order-message-fields-timers-now-snapshots-links\"}");
    return 0;
}
