#include "module.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); abort(); } } while (0)

typedef struct { const char *name; double initial; } S;
typedef struct { const char *name; uint32_t type; } F;
/* kind 0 is a key input; otherwise PURE_KIND_MESSAGE with fields. */
typedef struct { const char *name, *key; int kind; bool keyed; const F *fields; size_t nfields; } I;
#define KEY(name, key) {name, key, 0, false, NULL, 0}
typedef struct { const char *name; uint32_t start, length; } V;
typedef struct { uint32_t event, start, length; } H;
typedef struct { uint8_t op; uint32_t arg; double value; } O;
typedef struct {
    const S *s; size_t ns; const I *in; size_t ni; const V *v; size_t nv;
    const H *h; size_t nh; const O *o; size_t no; const char *const *c; size_t nc;
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

/* Test-side encoder for format v2; the toolchain has its own. */
static size_t build(unsigned char *out, Spec spec) {
    unsigned char strings[4096];
    size_t used = 0;
    const char *text[512];
    uint32_t offset[512];
    size_t names = 0;
    for (size_t i = 0; i < spec.ns; ++i) text[names++] = spec.s[i].name;
    for (size_t i = 0; i < spec.nc; ++i) text[names++] = spec.c[i];
    for (size_t i = 0; i < spec.ni; ++i) {
        text[names++] = spec.in[i].name;
        if (!spec.in[i].kind) text[names++] = spec.in[i].key;
        for (size_t f = 0; f < spec.in[i].nfields; ++f) text[names++] = spec.in[i].fields[f].name;
    }
    for (size_t i = 0; i < spec.nv; ++i) text[names++] = spec.v[i].name;
    for (size_t i = 0; i < names; ++i) {
        size_t length = strlen(text[i]);
        offset[i] = (uint32_t)used;
        strings[used++] = (unsigned char)length;
        memcpy(strings + used, text[i], length);
        used += length;
    }
    size_t fields = 0;
    for (size_t i = 0; i < spec.ni; ++i) fields += spec.in[i].nfields;
    unsigned char *p = out;
    memset(p, 0, 32);
    memcpy(p, "PUREMOD2", 8);
    p[8] = (unsigned char)spec.ns; p[9] = (unsigned char)(spec.ns >> 8);
    p[10] = (unsigned char)spec.ni; p[12] = (unsigned char)spec.nv; p[14] = (unsigned char)spec.nh;
    put32(p + 16, (uint32_t)spec.no);
    put32(p + 20, (uint32_t)used);
    p[24] = (unsigned char)fields; p[26] = (unsigned char)spec.nc;
    p += 32;
    size_t n = 0;
    for (size_t i = 0; i < spec.ns; ++i) { p = put32(p, offset[n++]); p = put32(p, 0); p = put_double(p, spec.s[i].initial); }
    for (size_t i = 0; i < spec.nc; ++i) { p = put32(p, offset[n++]); p = put32(p, 1); p = put32(p, 1); }
    uint32_t field_offsets[64], field_start = 0;
    size_t nf = 0;
    for (size_t i = 0; i < spec.ni; ++i) {
        const I *in = &spec.in[i];
        p = put32(p, offset[n++]);
        *p++ = (unsigned char)(in->kind ? in->kind : 1); *p++ = in->keyed; *p++ = 0; *p++ = 0;
        p = put32(p, in->kind ? 0 : offset[n++]);
        uint32_t first = in->kind ? field_start : 0; /* key inputs have no fields */
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
#define SPEC(s, in, v, h, o) ((Spec){s, LEN(s), in, LEN(in), v, LEN(v), h, LEN(h), o, LEN(o), NULL, 0})

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
    CHECK(strstr(error, "view must leave one value"));
    static const V views[] = {{"label", 0, 1}, {"width", 26, 7}};
    CHECK(load(SPEC(counter_states, counter_inputs, views, counter_handlers, counter_code), &module_a) == PURE_OK);
    PureApp app;
    pure_app_start(&app, &module_a, (PureData){0});
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
    CHECK(load((Spec){next_states, 2, no_inputs, 0, no_views, 0, no_handlers, 0, no_code, 0, NULL, 0}, &module_b) == PURE_OK);
    pure_app_replace(&app, &module_b);
    CHECK(app.module == &module_b && app.values[0] == 7 && app.values[1] == 25);
    pure_app_replace(&app, &module_a);
    CHECK(app.values[0] == 25 && app.values[1] == 3);
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
    pure_app_start(&app, &module_a, (PureData){0});
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
        {"duplicate name", {dup, 2, i1, 0, clash, 0, none, 0, load_a, 0, NULL, 0}, PURE_INVALID},
        {"state name", {bad_name, 1, i1, 0, clash, 0, none, 0, load_a, 0, NULL, 0}, PURE_INVALID},
        {"state name", {long_name, 1, i1, 0, clash, 0, none, 0, load_a, 0, NULL, 0}, PURE_INVALID},
        {"non-finite initial value", {non_finite, 1, i1, 0, clash, 0, none, 0, load_a, 0, NULL, 0}, PURE_INVALID},
        {"input key", {s1, 1, bad_key, 1, clash, 0, none, 0, load_a, 0, NULL, 0}, PURE_INVALID},
        {"duplicate key", {s1, 1, dup_key, 2, clash, 0, none, 0, load_a, 0, NULL, 0}, PURE_INVALID},
        {"duplicate name", {s1, 1, i1, 0, clash, 1, none, 0, load_a, 1, NULL, 0}, PURE_INVALID},
        {"view with an effect", {s1, 1, i1, 0, v_store, 1, none, 0, store, 3, NULL, 0}, PURE_INVALID},
        {"view with an effect", {s1, 1, i1, 0, v_dt, 1, none, 0, store, 3, NULL, 0}, PURE_INVALID},
        {"empty view", {s1, 1, i1, 0, v_empty, 1, none, 0, store, 3, NULL, 0}, PURE_INVALID},
        {"handler event", {s1, 1, i1, 1, clash, 0, h_bad_event, 1, store, 3, NULL, 0}, PURE_INVALID},
        {"branch not forward", {s1, 1, i1, 0, clash, 0, h_two, 1, back, 2, NULL, 0}, PURE_INVALID},
        {"branch not forward", {s1, 1, i1, 0, clash, 0, h_two, 1, beyond, 2, NULL, 0}, PURE_INVALID},
        {"stack underflow", {s1, 1, i1, 0, clash, 0, h_all, 1, under, 1, NULL, 0}, PURE_INVALID},
        {"join with a non-empty stack", {s1, 1, i1, 0, clash, 0, h_five, 1, join, 5, NULL, 0}, PURE_INVALID},
        {"handler must leave no value", {s1, 1, i1, 0, clash, 0, h_two, 1, leftover, 2, NULL, 0}, PURE_INVALID},
        {"unknown state", {s1, 1, i1, 0, clash, 0, h_two, 1, unknown_state, 2, NULL, 0}, PURE_INVALID},
        {"non-canonical operand", {s1, 1, i1, 0, clash, 0, h_two, 1, nan_const, 2, NULL, 0}, PURE_INVALID},
        {"non-canonical operand", {s1, 1, i1, 0, clash, 0, h0, 1, noncanon, 3, NULL, 0}, PURE_INVALID},
        {"unreachable instruction", {s1, 1, i1, 0, clash, 0, h_four, 1, unreachable, 4, NULL, 0}, PURE_INVALID},
        {"unknown opcode", {s1, 1, i1, 0, clash, 0, h_two, 1, bad_op, 2, NULL, 0}, PURE_INVALID},
        {"stack overflow", {s1, 1, i1, 0, clash, 0, (H[]){{0, 0, 66}}, 1, overflow, 66, NULL, 0}, PURE_INVALID},
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
    CHECK(load((Spec){many, 33, i1, 0, clash, 0, none, 0, load_a, 0, NULL, 0}, &module_a) == PURE_LIMIT);
    static unsigned char bytes[65536];
    size_t size = build(bytes, (Spec){s1, 1, i1, 0, clash, 0, none, 0, load_a, 0, NULL, 0});
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
        pure_app_start(&app, &module_a, (PureData){0});
        for (unsigned e = 0; e < 16; ++e) {
            if (e % 3 == 0) pure_app_frame(&app);
            else pure_app_input(&app, e % 4, e & 1 ? PURE_EDGE_UP : PURE_EDGE_DOWN);
            for (uint32_t v = 0; v < module_a.views; ++v) { double value; pure_app_view(&app, v, &value); }
            for (uint32_t s = 0; s < module_a.states; ++s) CHECK(isfinite(app.values[s]));
        }
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
    return (PureData){m, fake_open, fake_version, fake_read, fake_splice, fake_commit, fake_close};
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
    Spec spec = {states, 1, inputs, 5, views, 0, handlers, 4, code, LEN(code), collections, 1};
    editor_spec = spec;
    PureResult loaded = load(spec, &module_a);
    if (loaded != PURE_OK) { fprintf(stderr, "%s\n", error); abort(); }
    Memory memory = {.limit = 100};
    PureApp app;
    pure_app_start(&app, &module_a, fake_data(&memory));
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
    pure_app_finish(&app);
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
    CHECK(load(bad, &module_b) == PURE_INVALID && strstr(error, "unknown field"));
    static const I unkeyed[] = {{"note", NULL, PURE_KIND_MESSAGE, false, NULL, 0}};
    static const O doc_read[] = {{DOC, 0, 0}, {REPLY, 0, 0}};
    static const H note_handler[] = {{1, 0, 2}};
    bad = (Spec){states, 1, unkeyed, 1, views, 0, note_handler, 1, doc_read, 2, collections, 1};
    CHECK(load(bad, &module_b) == PURE_INVALID && strstr(error, "document operation outside"));
    static const F tail_first[] = {{"insert", 2}, {"start", 1}};
    static const I misplaced[] = {{"edit", NULL, PURE_KIND_MESSAGE, true, tail_first, 2}};
    bad = (Spec){states, 1, misplaced, 1, views, 0, NULL, 0, NULL, 0, collections, 1};
    CHECK(load(bad, &module_b) == PURE_INVALID && strstr(error, "message field"));
    static const H even[] = {{2, 0, 0}};
    bad = (Spec){states, 1, inputs, 1, views, 0, even, 1, NULL, 0, collections, 1};
    CHECK(load(bad, &module_b) == PURE_INVALID && strstr(error, "handler event"));
    static const V field_view[] = {{"v", 0, 1}};
    static const O field_only[] = {{FIELD, 0, 0}};
    bad = (Spec){states, 1, inputs, 2, field_view, 1, NULL, 0, field_only, 1, collections, 1};
    CHECK(load(bad, &module_b) == PURE_INVALID && strstr(error, "view with an effect"));
    static const O branch_value[] = {{C, 0, 1}, {C, 0, 1}, {JZ, 3, 0}, {ST, 0, 0}};
    static const H frame_handler[] = {{0, 0, 4}};
    bad = (Spec){states, 1, inputs, 0, views, 0, frame_handler, 1, branch_value, 4, NULL, 0};
    CHECK(load(bad, &module_b) == PURE_INVALID && strstr(error, "branch with a non-empty stack"));
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
        pure_app_start(&app, &module_b, fake_data(&memory));
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
        pure_app_finish(&app);
        CHECK(memory.opened == memory.closed);
        for (size_t i = 0; i < memory.count; ++i) free(memory.docs[i].bytes);
    }
    printf("{\"message_module_mutations\":20000,\"accepted_after_damage\":%u}\n", accepted);
}

int main(void) {
    behaviour();
    faults();
    rejections();
    mutations();
    messages();
    message_mutations();
    printf("{\"sizeof_module\":%zu,\"sizeof_app\":%zu}\n", sizeof(PureModule), sizeof(PureApp));
    puts("{\"module_tests\":\"verified-load-atomic-events-faults-signed-zero-if-else-replace-by-name-rejections-mutations-messages-documents-typed-verification\"}");
    return 0;
}
