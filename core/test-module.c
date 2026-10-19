#define _POSIX_C_SOURCE 200809L
#ifdef __APPLE__
#define _DARWIN_C_SOURCE /* mkdtemp, which _POSIX_C_SOURCE hides there */
#endif
#include "module.h"
#include "module-run.h"
#include "module-save.h"
#include "module-store.h"
#include "module-text.h"
#include "store.h"
#include <dirent.h>
#include <fcntl.h>
#include <inttypes.h>
#ifdef __GLIBC__
#include <malloc.h>
#endif
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
/* kind 0 is a key input, TEXT_INPUT a text input, TIMER_INPUT a timer; otherwise PURE_KIND_MESSAGE with fields.
 * Arguments name their view by its number in key. A module has messages bound so (027's 4d): expand writes them. */
typedef struct { const char *name, *key; int kind; const F *fields; size_t nfields; } I;
/* POINT_INPUT: a pointing message (one u32 field), its table the view numbered key. DT and TI (250, 251): dt and a
 * text input's text, which a module reads as fields. */
enum { TEXT_INPUT = 100, TIMER_INPUT = 101, POINT_INPUT = 102, TEST_DT = 250, TEST_TEXT_IN = 251 };
#define KEY(name, key) {name, key, 0, NULL, 0}
/* A timer's period (milliseconds) rides in nfields. */
#define TIMER(name, period) {name, NULL, TIMER_INPUT, NULL, period}
/* A number view, or a bool (i64) view when its length carries BOOL_VIEW (I64_VIEW). */
typedef struct { const char *name; uint32_t start, length; } V;
#define BOOL_VIEW (1u << 31)
#define I64_VIEW (1u << 30)
typedef struct { uint32_t event, start, length; } H;
typedef struct { uint8_t op; uint32_t arg; double value; } O;
/* Text (type 1, ref = constant), cursor (type 2, ref = text state), list
 * (type 3, ref = record), index (type 4, ref = list state), bool (type 5,
 * ref = its initial value) and i64 (type 6, ref = its initial value, a
 * 32-bit two's complement) states follow the number states; text views
 * follow the number views. */
typedef struct { const char *name; uint32_t type, ref; } T; /* type: the state type, its flags from bit 8 */
#define TR (1u << 24) /* in a T's type: a transient state */
/* A record field: a number and its default, text (text = 1) whose default
 * is the constant numbered initial, a bool (text = 2) or an i64 (text = 3)
 * and its default, a list (4, initial its record) or bytes (5, its default
 * a constant as a text's). */
typedef struct { const char *name; uint32_t text; double initial; } RF;
typedef struct { const char *name; const RF *fields; size_t nfields; } R;
/* A further view: a table (type 2, start = list state, or bits 8-15 its
 * list + 1 with a filter's block) or a column of the view numbered table - 1
 * (0 a number, 1 a text, 3 a bool, 4 an i64, as views were numbered before
 * 027's 4e; 5 bytes); 6 a list view of a record (bits 16-23 its record + 1,
 * bits 8-15 its list + 1 or none: its block leaves its list), 7 a list
 * column (bits 16-23 its record + 1). Types from 8 on are written as they
 * are. */
typedef struct { const char *name; uint32_t type, table, start, length; } W;
enum { W_BYTES = 5, W_OF = 6, W_LIST = 7 };
/* A function (027's 2c): its block, and its parameters' then its results' types (a record field's: 0 a number, 1 a
 * text, 2 a bool, 3 an i64, 4 a list whose record is beside it; 5 bytes). */
typedef struct { uint32_t start, length; uint8_t params, results, types[16]; uint16_t records[16]; } FN;
typedef struct {
    const S *s; size_t ns; const I *in; size_t ni; const V *v; size_t nv;
    const H *h; size_t nh; const O *o; size_t no; const char *const *c; size_t nc;
    const T *t; size_t nt; const char *const *k; size_t nk; const V *tv; size_t ntv;
    const R *r; size_t nr; const W *w; size_t nw;
} Spec;
/* The functions the next build writes (load_functions sets them a moment). */
static const FN *spec_fn;
static size_t spec_nfn;

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

/* Test-side encoder for format v6; the toolchain has its own. */
/* The tests write states and code as the runtime holds them (a cursor, an index, the steps of each: module-run.h); a
 * module has their anchor (027's 4c), its type 2, and its instructions, which name the text or list (bits 0-15 of a
 * MOVE's and a COUNT's argument: IN), so a test gives that where a step does not name it. */
#define IN(target, unit) ((uint32_t)(target) | (uint32_t)(unit) << PURE_ANCHOR_UNIT)
static uint32_t module_type(uint32_t type) {
    static const uint8_t types[] = {[PURE_STATE_NUM] = PURE_MODULE_NUM, [PURE_STATE_TEXT] = PURE_MODULE_TEXT,
                                    [PURE_STATE_CURSOR] = PURE_MODULE_ANCHOR, [PURE_STATE_LIST] = PURE_MODULE_LIST,
                                    [PURE_STATE_INDEX] = PURE_MODULE_ANCHOR, [PURE_STATE_BOOL] = PURE_MODULE_BOOL,
                                    [PURE_STATE_I64] = PURE_MODULE_I64};
    return type < sizeof(types) ? types[type] : type;
}
static uint8_t module_op(uint8_t op) {
    switch (op) {
    case PURE_OP_CUR_LOAD: case PURE_OP_INDEX_LOAD: return PURE_OP_ANCHOR_LOAD;
    case PURE_OP_CUR_STORE: case PURE_OP_INDEX_STORE: return PURE_OP_ANCHOR_STORE;
    case PURE_OP_CUR_START: case PURE_OP_INDEX_FIRST: return PURE_OP_ANCHOR_START;
    case PURE_OP_CUR_END: case PURE_OP_INDEX_LAST: return PURE_OP_ANCHOR_END;
    case PURE_OP_CUR_MOVE: case PURE_OP_INDEX_MOVE: return PURE_OP_ANCHOR_MOVE;
    case PURE_OP_CUR_COUNT: case PURE_OP_INDEX_NUM: return PURE_OP_ANCHOR_COUNT;
    case PURE_OP_CUR_AT: case PURE_OP_INDEX_AT: return PURE_OP_ANCHOR_AT;
    default: return op;
    }
}

/* A test's inputs, handlers and code as a module has them (027's 4d): a key input a message its press makes, one more
 * its release makes for a key up's handler (event 2 + 2i), a frame's message (one number field) for a frame's handler
 * (event 0), a text input a message of one text field, a timer a message its ticks make; a handler's event its
 * message's number; dt (DT, 250) the frame's field in its handler and 0 elsewhere, a text input's text (TI, 251) its
 * field. */
static I expanded_in[64];
static struct { uint8_t binding; uint32_t period; } expanded_bound[64];
static H expanded_h[64];
static O expanded_o[8192];
static Spec expand(Spec spec) {
    static const F text_field[] = {{"text", PURE_FIELD_TEXT}}, dt_field[] = {{"dt", PURE_FIELD_NUMBER}};
    static char release_names[64][PURE_MODULE_NAME];
    static const F position_field[] = {{"position", PURE_FIELD_U32}};
    size_t ni = 0;
    for (size_t i = 0; i < spec.ni; ++i) {
        I in = spec.in[i];
        uint8_t binding = 0;
        uint32_t period = 0;
        if (!in.kind) in = (I){in.name, in.key, PURE_KIND_MESSAGE, NULL, 0}, binding = PURE_BIND_PRESS;
        else if (in.kind == TEXT_INPUT) in = (I){in.name, NULL, PURE_KIND_MESSAGE, text_field, 1}, binding = PURE_BIND_TEXT;
        else if (in.kind == TIMER_INPUT)
            period = (uint32_t)in.nfields, in = (I){in.name, NULL, PURE_KIND_MESSAGE, NULL, 0}, binding = PURE_BIND_TIMER;
        else if (in.kind == POINT_INPUT) in = (I){in.name, in.key, PURE_KIND_MESSAGE, position_field, 1}, binding = PURE_BIND_POINT;
        expanded_bound[ni].binding = binding, expanded_bound[ni].period = period;
        expanded_in[ni++] = in;
    }
    int release[64], frame = -1;
    for (size_t i = 0; i < spec.ni; ++i) release[i] = -1;
    for (size_t h = 0; h < spec.nh; ++h) {
        uint32_t e = spec.h[h].event;
        if (!e && frame < 0) {
            frame = (int)ni;
            expanded_bound[ni].binding = PURE_BIND_FRAME, expanded_bound[ni].period = 0;
            expanded_in[ni++] = (I){"frame", NULL, PURE_KIND_MESSAGE, dt_field, 1};
        } else if (e && !(e & 1) && (e - 2) / 2 < spec.ni && release[(e - 2) / 2] < 0 &&
                   expanded_bound[(e - 2) / 2].binding == PURE_BIND_PRESS) {
            uint32_t of = (e - 2) / 2;
            snprintf(release_names[of], PURE_MODULE_NAME, "%s.up", spec.in[of].name);
            release[of] = (int)ni;
            expanded_bound[ni].binding = PURE_BIND_RELEASE, expanded_bound[ni].period = 0;
            expanded_in[ni++] = (I){release_names[of], spec.in[of].key, PURE_KIND_MESSAGE, NULL, 0};
        }
    }
    CHECK(ni <= 64 && spec.nh <= 64 && spec.no <= 8192);
    for (size_t h = 0; h < spec.nh; ++h) {
        uint32_t e = spec.h[h].event;
        expanded_h[h] = spec.h[h];
        /* an event past the inputs, or a release of what is not a key, past them all (a test of a refusal) */
        int up = !(e & 1) && e && (e - 2) / 2 < spec.ni ? release[(e - 2) / 2] : -1;
        expanded_h[h].event = !e ? (uint32_t)frame : e & 1 ? (e - 1) / 2 : up >= 0 ? (uint32_t)up : 1000 + e;
    }
    for (size_t k = 0; k < spec.no; ++k) {
        expanded_o[k] = spec.o[k];
        if (spec.o[k].op == TEST_TEXT_IN) expanded_o[k] = (O){PURE_OP_FIELD, 0, 0};
        if (spec.o[k].op != TEST_DT) continue;
        bool in_frame = false;
        for (size_t h = 0; h < spec.nh; ++h)
            in_frame = in_frame || (!spec.h[h].event && k >= spec.h[h].start && k < spec.h[h].start + spec.h[h].length);
        expanded_o[k] = in_frame ? (O){PURE_OP_FIELD, 0, 0} : (O){PURE_OP_CONST, 0, 0};
    }
    spec.in = expanded_in, spec.ni = ni;
    spec.h = expanded_h;
    spec.o = expanded_o;
    return spec;
}

static size_t build(unsigned char *out, Spec spec) {
    spec = expand(spec);
    static unsigned char strings[16384], blob[4096];
    size_t used = 0, blob_used = 0;
    static const char *text[2048];
    static uint32_t offset[2048];
    size_t names = 0;
    for (size_t i = 0; i < spec.ns; ++i) text[names++] = spec.s[i].name;
    for (size_t i = 0; i < spec.nt; ++i) text[names++] = spec.t[i].name;
    for (size_t i = 0; i < spec.ni; ++i) {
        text[names++] = spec.in[i].name;
        if (expanded_bound[i].binding == PURE_BIND_PRESS || expanded_bound[i].binding == PURE_BIND_RELEASE) text[names++] = spec.in[i].key;
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
    CHECK(!spec.nc); /* (collections are gone: 027's 4b) */
    memset(p, 0, 38);
    memcpy(p, "PUREMOD7", 8);
    size_t states = spec.ns + spec.nt, views = spec.nv + spec.ntv + spec.nw;
    p[8] = (unsigned char)states; p[9] = (unsigned char)(states >> 8);
    p[10] = (unsigned char)spec.ni; p[12] = (unsigned char)views; p[14] = (unsigned char)spec.nh;
    put32(p + 16, (uint32_t)spec.no);
    put32(p + 20, (uint32_t)used);
    p[24] = (unsigned char)fields; p[26] = (unsigned char)spec.nk; p[28] = (unsigned char)spec.nr;
    p[34] = (unsigned char)record_fields; p[36] = (unsigned char)spec_nfn;
    put32(p + 30, (uint32_t)blob_used);
    p += 38;
    size_t n = 0;
    for (size_t i = 0; i < spec.ns; ++i) { p = put32(p, offset[n++]); p = put32(p, 0); p = put_double(p, spec.s[i].initial); }
    for (size_t i = 0; i < spec.nt; ++i) {
        p = put32(p, offset[n++]);
        p = put32(p, module_type((spec.t[i].type & 0xFF)) | (spec.t[i].type & TR ? PURE_STATE_TRANSIENT : 0));
        if ((spec.t[i].type & 0xFF) == PURE_STATE_BOOL) { p = put_double(p, spec.t[i].ref); continue; }
        if ((spec.t[i].type & 0xFF) == PURE_STATE_I64) {
            int64_t initial = (int32_t)spec.t[i].ref;
            p = put32(p, (uint32_t)(uint64_t)initial);
            p = put32(p, (uint32_t)((uint64_t)initial >> 32));
            continue;
        }
        p = put32(p, spec.t[i].ref);
        p = put32(p, (spec.t[i].type & ~TR) >> 8);
    }
    uint32_t field_offsets[64], field_start = 0;
    size_t nf = 0;
    for (size_t i = 0; i < spec.ni; ++i) {
        const I *in = &spec.in[i];
        uint8_t binding = expanded_bound[i].binding;
        bool keyed = binding == PURE_BIND_PRESS || binding == PURE_BIND_RELEASE;
        p = put32(p, offset[n++]);
        *p++ = (unsigned char)in->kind; *p++ = binding; *p++ = 0; *p++ = 0;
        p = put32(p, binding == PURE_BIND_TIMER ? expanded_bound[i].period :
                     in->kind == PURE_KIND_ARGUMENTS || binding == PURE_BIND_POINT ? (uint32_t)strtoul(in->key, NULL, 10) :
                     keyed ? offset[n++] : 0);
        *p++ = (unsigned char)field_start; *p++ = (unsigned char)(field_start >> 8);
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
            if (field->text == 1 || field->text == 5) { p = put32(p, (uint32_t)field->initial); p = put32(p, 0); }
            else if (field->text == 3 || field->text == 4) { /* an i64's default, a list's record */
                int64_t initial = (int64_t)field->initial;
                p = put32(p, (uint32_t)(uint64_t)initial);
                p = put32(p, (uint32_t)((uint64_t)initial >> 32));
            }
            else p = put_double(p, field->initial);
        }
    for (size_t i = 0; i < spec.nv; ++i) {
        p = put32(p, offset[n++]);
        p = put32(p, spec.v[i].length & BOOL_VIEW ? PURE_VIEW_BOOL : spec.v[i].length & I64_VIEW ? PURE_VIEW_I64 : 0);
        p = put32(p, 0);
        p = put32(p, spec.v[i].start);
        p = put32(p, spec.v[i].length & ~(BOOL_VIEW | I64_VIEW));
    }
    for (size_t i = 0; i < spec.ntv; ++i) {
        p = put32(p, offset[n++]); p = put32(p, PURE_VIEW_TEXT); p = put32(p, 0);
        p = put32(p, spec.tv[i].start); p = put32(p, spec.tv[i].length);
    }
    static const uint8_t w_types[] = {PURE_VIEW_NUM, PURE_VIEW_TEXT, PURE_VIEW_LIST, PURE_VIEW_BOOL, PURE_VIEW_I64,
                                      PURE_VIEW_BYTES, PURE_VIEW_LIST, PURE_VIEW_LIST};
    for (size_t i = 0; i < spec.nw; ++i) { /* 20 bytes (027's 4e): type, 0, table + 1, record + 1, list + 1, block */
        const W *w = &spec.w[i];
        uint32_t code = w->type & 0xFF, list = w->type >> 8 & 0xFF, record = w->type >> 16 & 0xFF, start = w->start;
        if (code == 2 && !list) list = start + 1, start = 0; /* a table without a filter named its list in start */
        p = put32(p, offset[n++]);
        *p++ = code < sizeof(w_types) ? w_types[code] : (unsigned char)code; *p++ = 0;
        *p++ = (unsigned char)w->table; *p++ = (unsigned char)(w->table >> 8);
        *p++ = (unsigned char)record; *p++ = 0;
        *p++ = (unsigned char)list; *p++ = 0;
        p = put32(p, start);
        p = put32(p, w->length);
    }
    for (size_t i = 0; i < spec.nh; ++i) { p = put32(p, spec.h[i].event); p = put32(p, spec.h[i].start); p = put32(p, spec.h[i].length); }
    for (size_t i = 0; i < spec_nfn; ++i) {
        p = put32(p, spec_fn[i].start);
        p = put32(p, spec_fn[i].length);
        *p++ = spec_fn[i].params; *p++ = spec_fn[i].results; *p++ = 0; *p++ = 0;
    }
    for (size_t i = 0; i < spec_nfn; ++i)
        for (unsigned k = 0; k < (unsigned)spec_fn[i].params + spec_fn[i].results; ++k) {
            *p++ = spec_fn[i].types[k]; *p++ = 0;
            *p++ = (unsigned char)spec_fn[i].records[k]; *p++ = (unsigned char)(spec_fn[i].records[k] >> 8);
        }
    for (size_t i = 0; i < spec.no; ++i) {
        *p++ = module_op(spec.o[i].op); *p++ = 0; *p++ = 0; *p++ = 0;
        p = put32(p, spec.o[i].arg);
        p = put_double(p, spec.o[i].value);
    }
    memcpy(p, strings, used);
    memcpy(p + used, blob, blob_used);
    return (size_t)(p + used + blob_used - out);
}

static PureModule module_a, module_b;

/* A device's events as a module takes them (027's 4d): a key's press and release, a frame, a tick, text, each the
 * message bound so (a release or a frame no message takes is no event); one for an input not bound so is INVALID. */
static PureResult press(PureApp *app, uint32_t input) {
    if (input >= app->module->inputs || app->module->input[input].binding != PURE_BIND_PRESS) return PURE_INVALID;
    return pure_app_message(app, input, (PureSpan){0});
}
static PureResult release(PureApp *app, uint32_t input) {
    const PureModule *m = app->module;
    if (input >= m->inputs || m->input[input].binding != PURE_BIND_PRESS) return PURE_INVALID;
    for (uint32_t i = 0; i < m->inputs; ++i)
        if (m->input[i].binding == PURE_BIND_RELEASE && !strcmp(m->input_keys[i], m->input_keys[input]))
            return pure_app_message(app, i, (PureSpan){0});
    return PURE_OK;
}
static PureResult frame(PureApp *app) {
    static const double dt = 1.0 / 60.0;
    for (uint32_t i = 0; i < app->module->inputs; ++i)
        if (app->module->input[i].binding == PURE_BIND_FRAME) return pure_app_message(app, i, (PureSpan){(const unsigned char *)&dt, 8});
    return PURE_OK;
}
static PureResult tick(PureApp *app, uint32_t input) {
    if (input >= app->module->inputs || app->module->input[input].binding != PURE_BIND_TIMER) return PURE_INVALID;
    return pure_app_message(app, input, (PureSpan){0});
}
/* A test's index put on an item, as a host's pointing did before 027's 4d (now a message an app takes: pointing), and
 * saved unless deferred. */
static PureResult place(PureApp *app, uint32_t state, size_t position) {
    const PureModule *m = app->module;
    if (state >= m->states || m->state_type[state] != PURE_STATE_INDEX) return PURE_INVALID;
    if (position >= pure_list_count(app->lists[m->state_ref[state]])) return PURE_NOT_FOUND;
    app->values[state].num = (double)position;
    ++app->changes;
    return app->deferred ? PURE_OK : pure_app_save(app);
}
static PureResult type_in(PureApp *app, uint32_t input, PureSpan text) {
    if (input >= app->module->inputs || app->module->input[input].binding != PURE_BIND_TEXT) return PURE_INVALID;
    return pure_app_message(app, input, text);
}
static char error[128];

static PureResult load(Spec spec, PureModule *out) {
    static unsigned char bytes[65536];
    size_t size = build(bytes, spec);
    return pure_module_load(bytes, size, out, error, sizeof(error));
}

static PureResult load_functions(Spec spec, const FN *fns, size_t count, PureModule *out) {
    spec_fn = fns, spec_nfn = count;
    PureResult result = load(spec, out);
    spec_fn = NULL, spec_nfn = 0;
    return result;
}

#define LEN(a) (sizeof(a) / sizeof(*(a)))
#define SPEC(s, in, v, h, o) ((Spec){s, LEN(s), in, LEN(in), v, LEN(v), h, LEN(h), o, LEN(o), NULL, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0})

enum { C = PURE_OP_CONST, L = PURE_OP_LOAD, DT = TEST_DT, ADD = PURE_OP_ADD,
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
    for (int i = 0; i < 25; ++i) CHECK(press(&app, 0) == PURE_OK);
    CHECK(app.values[0].num == 25 && pure_app_view(&app, 1, &value) && value == 200);
    CHECK(release(&app, 0) == PURE_OK && app.events == 25);
    /* if / else with forward JZ and JMP */
    CHECK(press(&app, 1) == PURE_OK && app.values[1].num == 4);
    CHECK(press(&app, 1) == PURE_OK && app.values[1].num == 5);
    CHECK(press(&app, 1) == PURE_OK && app.values[1].num == 6);
    CHECK(press(&app, 1) == PURE_OK && app.values[1].num == 0);
    CHECK(frame(&app) == PURE_OK && app.values[1].num == 1.0 / 60.0);
    CHECK(press(&app, 2) == PURE_INVALID);
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
    CHECK(app.module == &module_b && app.values[0].num == 7 && app.values[1].num == 25);
    CHECK(pure_app_replace(&app, &module_a) == PURE_OK);
    CHECK(app.values[0].num == 25 && app.values[1].num == 3);
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
        CHECK(press(&app, i) == PURE_INVALID && app.fault == expected[i]);
        CHECK(app.values[0].num == 1 && app.values[1].num == 2 && app.events == 0);
    }
    /* A later handler of the same event does not run after a fault. */
    CHECK(app.fault_pc == 17);
    double value;
    CHECK(pure_app_view(&app, 0, &value) && value == 0 && signbit(value));
    CHECK(pure_app_view(&app, 1, &value) && value == 0 && !signbit(value));
    pure_app_stop(&app);
}

typedef struct { const char *reason; Spec spec; PureResult result; } Rejection;

static char *new_store(void);
static void remove_store(const char *path);
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
    static const V v_field[] = {{"v", 2, 1}};
    static const V v_empty[] = {{"v", 0, 0}};
    static const H none[1];
    static const O load_a[] = {{L, 0, 0}};
    static const O store[] = {{C, 0, 1}, {ST, 0, 0}, {PURE_OP_FIELD, 0, 0}};
    static const H h0[] = {{0, 0, 2}};
    static const H h_bad_event[] = {{3, 0, 2}};
    static const O back[] = {{C, 1, 1}, {JZ, 0, 0}};
    static const O beyond[] = {{C, 1, 1}, {JZ, 3, 0}};
    static const O under[] = {{ADD, 0, 0}};
    static const O join[] = {{C, 1, 1}, {JZ, 4, 0}, {C, 0, 1}, {C, 0, 1}, {ST, 0, 0}};
    static const O leftover[] = {{C, 0, 1}, {C, 0, 2}};
    static const O unknown_state[] = {{L, 1, 0}, {ST, 0, 0}};
    static const O nan_const[] = {{C, 0, NAN}, {ST, 0, 0}};
    static const O noncanon[] = {{C, 0, 1}, {NEG, 1, 0}, {ST, 0, 0}};
    static const O unreachable[] = {{JMP, 2, 0}, {C, 0, 1}, {C, 0, 1}, {ST, 0, 0}};
    static const O bad_op[] = {{PURE_OP_KEPT_GET, 0, 0}, {ST, 0, 0}}; /* past a module's opcodes: the runtime's own */
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
        {"view with an effect", {s1, 1, i1, 0, v_field, 1, none, 0, store, 3, NULL, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0}, PURE_INVALID},
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
    /* No fixed count of states (027's 4f-2): 300 of them (sets of them past a word), the last one counted by a key; a
     * module of as many runs in the executor alone (the JIT's sets are a word). */
    enum { MANY = 300 };
    static S many[MANY];
    static char many_names[MANY][8];
    for (unsigned i = 0; i < MANY; ++i) { snprintf(many_names[i], 8, "s%u", i); many[i] = (S){many_names[i], 0}; }
    static const O count_last[] = {{L, MANY - 1, 0}, {C, 0, 1}, {ADD, 0, 0}, {ST, MANY - 1, 0}};
    static const I key_in[] = {KEY("go", "KeyG")};
    static const H go[] = {{1, 0, 4}};
    CHECK(load((Spec){many, MANY, key_in, 1, clash, 0, go, 1, count_last, 4, NULL, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0},
               &module_a) == PURE_OK && module_a.state_words == 5 && !module_a.jit);
    PureApp wide;
    CHECK(pure_app_start(&wide, &module_a, (PureData){0}) == PURE_OK && press(&wide, 0) == PURE_OK && press(&wide, 0) == PURE_OK &&
          wide.values[MANY - 1].num == 2 && wide.values[0].num == 0);
    pure_app_stop(&wide);
    /* Nor of inputs, views or handlers (027's 4f-3a): 40 messages (the last counted), 70 views (the last its count). */
    enum { INPUTS = 40, VIEWS = 70 };
    static I messages[INPUTS];
    static V views[VIEWS];
    static char message_names[INPUTS][8], view_names[VIEWS][8];
    for (unsigned i = 0; i < INPUTS; ++i) {
        snprintf(message_names[i], 8, "m%u", i);
        messages[i] = (I){message_names[i], NULL, PURE_KIND_MESSAGE, NULL, 0};
    }
    for (unsigned i = 0; i < VIEWS; ++i) snprintf(view_names[i], 8, "v%u", i), views[i] = (V){view_names[i], 4, 1};
    static const O counted[] = {{L, 0, 0}, {C, 0, 1}, {ADD, 0, 0}, {ST, 0, 0}, {L, 0, 0}};
    static const H last[] = {{1 + 2 * (INPUTS - 1), 0, 4}};
    CHECK(load((Spec){s1, 1, messages, INPUTS, views, VIEWS, last, 1, counted, 5, NULL, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0,
                      NULL, 0}, &module_a) == PURE_OK && module_a.inputs == INPUTS && module_a.views == VIEWS);
    CHECK(pure_app_start(&wide, &module_a, (PureData){0}) == PURE_OK);
    CHECK(pure_app_message(&wide, INPUTS - 1, (PureSpan){0}) == PURE_OK && pure_app_message(&wide, 0, (PureSpan){0}) == PURE_OK &&
          pure_app_view(&wide, VIEWS - 1, &(double){0}));
    double shown;
    CHECK(pure_app_view(&wide, VIEWS - 1, &shown) && shown == 1);
    pure_app_stop(&wide);
    /* Nor of records or their fields in all (027's 4f-3b): 20 records of 10 fields, an item of the last appended. */
    enum { RECORDS = 20, EACH = 10 };
    static RF record_fields[RECORDS][EACH];
    static R records[RECORDS];
    static char record_names[RECORDS][8], field_names[EACH][8];
    for (unsigned f = 0; f < EACH; ++f) snprintf(field_names[f], 8, "f%u", f);
    for (unsigned r = 0; r < RECORDS; ++r) {
        snprintf(record_names[r], 8, "R%u", r);
        for (unsigned f = 0; f < EACH; ++f) record_fields[r][f] = (RF){field_names[f], 0, 0};
        records[r] = (R){record_names[r], record_fields[r], EACH};
    }
    static const T last_list[] = {{"items", 3, RECORDS - 1}};
    static O appended[EACH + 3];
    for (unsigned f = 0; f < EACH; ++f) appended[f] = (O){C, 0, f};
    appended[EACH] = (O){PURE_OP_LIST_APPEND, 1, 0};
    appended[EACH + 1] = (O){PURE_OP_LIST_COUNT, 1, 0};
    appended[EACH + 2] = (O){ST, 0, 0};
    static const H appending[] = {{1, 0, EACH + 3}};
    CHECK(load((Spec){s1, 1, key_in, 1, NULL, 0, appending, 1, appended, EACH + 3, NULL, 0, last_list, 1, NULL, 0, NULL, 0,
                      records, RECORDS, NULL, 0}, &module_a) == PURE_OK && module_a.records == RECORDS &&
          module_a.record_fields == RECORDS * EACH);
    CHECK(pure_app_start(&wide, &module_a, (PureData){0}) == PURE_OK && press(&wide, 0) == PURE_OK && wide.values[0].num == 1 &&
          pure_list_get(wide.lists[1], 0, EACH - 1).num == EACH - 1);
    pure_app_stop(&wide);
    /* A stored list's record graph past the few a layout holds itself: six records, each a list of the next. */
    enum { DEEP = 6 };
    static RF chain_fields[DEEP][2];
    static R chain[DEEP];
    for (unsigned r = 0; r < DEEP; ++r) {
        chain_fields[r][0] = (RF){"n", 0, 0};
        chain_fields[r][1] = (RF){"next", 4, r + 1 < DEEP ? r + 1 : r}; /* the last a list of itself */
        chain[r] = (R){record_names[r], chain_fields[r], 2};
    }
    static const T chained[] = {{"items", 3, 0}, {"let.none", 3 | (uint32_t)PURE_LIST_ROWS << 8, 1}};
    static const O add_one[] = {{C, 0, 7}, {PURE_OP_LIST_VALUE, 2, 0}, {PURE_OP_LIST_APPEND, 1, 0}, {PURE_OP_LIST_COUNT, 1, 0},
                                {ST, 0, 0}};
    static const H adding[] = {{1, 0, 5}};
    Spec deep = {s1, 1, key_in, 1, NULL, 0, adding, 1, add_one, 5, NULL, 0, chained, 2, NULL, 0, NULL, 0, chain, DEEP, NULL, 0};
    CHECK(load(deep, &module_a) == PURE_OK);
    char *deep_path = new_store();
    CHECK(pure_app_start(&wide, &module_a, pure_tree_data(deep_path)) == PURE_OK && press(&wide, 0) == PURE_OK &&
          press(&wide, 0) == PURE_OK && pure_app_save(&wide) == PURE_OK);
    pure_app_stop(&wide);
    CHECK(pure_app_start(&wide, &module_a, pure_tree_data(deep_path)) == PURE_OK && pure_list_count(wide.lists[1]) == 2 &&
          pure_list_get(wide.lists[1], 1, 0).num == 7);
    pure_app_stop(&wide);
    remove_store(deep_path);
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
            if (e % 3 == 0) frame(&app);
            else (e & 1 ? release(&app, e % 4) : press(&app, e % 4));
            for (uint32_t v = 0; v < module_a.views; ++v) { double value; pure_app_view(&app, v, &value); }
            for (uint32_t s = 0; s < module_a.states; ++s) CHECK(isfinite(app.values[s].num));
        }
        pure_app_stop(&app);
    }
    printf("{\"module_mutations\":20000,\"accepted_after_damage\":%u}\n", accepted);
}

static Spec message_spec;

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

enum { FIELD = PURE_OP_FIELD, LEN_ = PURE_OP_LEN, REPLY = PURE_OP_REPLY, REQ = PURE_OP_REQUIRE, LE = PURE_OP_LE };

/* Messages: echo replies its insert; edit adds start + remove to sum, counts itself and replies its insert; guarded
 * counts itself if its insert holds 8 bytes at most. A payload that does not hold the fields fails as limit, a failed
 * require changes nothing, and a key's press, a message with no fields, is one no handler takes here. */
static void messages(void) {
    static const F edit_fields[] = {{"start", 1}, {"remove", 1}, {"insert", 2}};
    static const I inputs[] = {
        {"echo", NULL, PURE_KIND_MESSAGE, edit_fields + 2, 1},
        {"edit", NULL, PURE_KIND_MESSAGE, edit_fields, 3},
        {"guarded", NULL, PURE_KIND_MESSAGE, edit_fields, 3},
        KEY("tick", "Space"),
    };
    static const S states[] = {{"edits", 0}, {"sum", 0}};
    static const O code[] = {
        {FIELD, 0, 0}, {REPLY, 0, 0},                                                  /* 0 echo */
        {FIELD, 0, 0}, {FIELD, 1, 0}, {ADD, 0, 0}, {L, 1, 0}, {ADD, 0, 0}, {ST, 1, 0}, /* 2 edit */
        {FIELD, 2, 0}, {REPLY, 0, 0}, {L, 0, 0}, {C, 0, 1}, {ADD, 0, 0}, {ST, 0, 0},
        {L, 0, 0}, {C, 0, 1}, {ADD, 0, 0}, {ST, 0, 0},                                 /* 14 guarded */
        {FIELD, 2, 0}, {LEN_, 0, 0}, {C, 0, 8}, {LE, 0, 0}, {REQ, 0, 0},
    };
    static const H handlers[] = {{1, 0, 2}, {3, 2, 12}, {5, 14, 9}};
    static const V views[] = {{"count", 0, 0}};
    Spec spec = {states, 2, inputs, 4, views, 0, handlers, 3, code, LEN(code), NULL, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0};
    message_spec = spec;
    PureResult loaded = load(spec, &module_a);
    if (loaded != PURE_OK) { fprintf(stderr, "%s\n", error); abort(); }
    PureApp app;
    CHECK(pure_app_start(&app, &module_a, (PureData){0}) == PURE_OK);
    unsigned char buffer[64];
    CHECK(pure_app_message(&app, 0, (PureSpan){(const unsigned char *)"hello", 5}) == PURE_OK && replied(&app, "hello"));
    CHECK(pure_app_message(&app, 1, edit_payload(buffer, 2, 3, "xy")) == PURE_OK && replied(&app, "xy"));
    CHECK(app.values[0].num == 1 && app.values[1].num == 5);
    CHECK(pure_app_message(&app, 1, (PureSpan){buffer, 7}) == PURE_LIMIT && app.fault == PURE_FAULT_LIMIT);
    CHECK(pure_app_message(&app, 2, edit_payload(buffer, 0, 0, "123456789")) == PURE_LIMIT);
    CHECK(app.values[0].num == 1 && app.values[1].num == 5);
    CHECK(pure_app_message(&app, 2, edit_payload(buffer, 0, 0, "12345678")) == PURE_OK && app.values[0].num == 2);
    CHECK(pure_app_message(&app, 3, (PureSpan){0}) == PURE_OK && app.values[0].num == 2 && app.events == 4);
    pure_app_stop(&app);

    /* Verifier rules for typed operations. */
    static const O field_in_key[] = {{FIELD, 0, 0}, {ST, 0, 0}};
    static const H key_handler[] = {{7, 0, 2}};
    Spec bad = spec;
    bad.o = field_in_key; bad.no = 2; bad.h = key_handler; bad.nh = 1;
    CHECK(load(bad, &module_b) == PURE_INVALID && strstr(error, "no such state, field")); /* a press holds no fields */
    static const O bytes_state[] = {{FIELD, 2, 0}, {ST, 0, 0}};
    static const H edit_handler[] = {{3, 0, 2}};
    bad.o = bytes_state; bad.no = 2; bad.h = edit_handler;
    CHECK(load(bad, &module_b) == PURE_INVALID && strstr(error, "operand type"));
    static const O far_field[] = {{FIELD, 3, 0}, {REPLY, 0, 0}};
    bad.o = far_field; bad.h = edit_handler;
    CHECK(load(bad, &module_b) == PURE_INVALID && strstr(error, "no such state, field"));
    static const F unknown_type[] = {{"insert", 8}, {"start", 1}};
    static const I misplaced[] = {{"edit", NULL, PURE_KIND_MESSAGE, unknown_type, 2}};
    bad = (Spec){states, 1, misplaced, 1, views, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0};
    CHECK(load(bad, &module_b) == PURE_INVALID && strstr(error, "message field"));
    static const H even[] = {{2, 0, 0}};
    bad = (Spec){states, 1, inputs, 1, views, 0, even, 1, NULL, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0};
    CHECK(load(bad, &module_b) == PURE_INVALID && strstr(error, "handler event"));
    static const V field_view[] = {{"v", 0, 1}};
    static const O field_only[] = {{FIELD, 0, 0}};
    bad = (Spec){states, 1, inputs, 2, field_view, 1, NULL, 0, field_only, 1, NULL, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0};
    CHECK(load(bad, &module_b) == PURE_INVALID && strstr(error, "view with an effect"));
    /* A value may cross a join when every path brings the same stack. */
    static const O branch_value[] = {{C, 0, 1}, {C, 1, 1}, {JZ, 3, 0}, {ST, 0, 0}};
    static const O branch_mismatch[] = {{C, 0, 1}, {C, 1, 1}, {JZ, 4, 0}, {C, 0, 2}, {ST, 0, 0}};
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
    size_t size = build(valid, message_spec);
    uint32_t state = 0x9e3779b9u;
    unsigned accepted = 0;
    for (unsigned round = 0; round < 20000; ++round) {
        memcpy(bytes, valid, size);
        unsigned flips = 1 + next_random(&state) % 3;
        for (unsigned f = 0; f < flips; ++f)
            bytes[next_random(&state) % size] ^= (unsigned char)(1u << (next_random(&state) % 8));
        if (pure_module_load(bytes, size, &module_b, error, sizeof(error)) != PURE_OK) continue;
        ++accepted;
        PureApp app;
        CHECK(pure_app_start(&app, &module_b, (PureData){0}) == PURE_OK);
        unsigned char payload[32];
        for (unsigned e = 0; e < 12; ++e) {
            for (size_t k = 0; k < sizeof(payload); ++k) payload[k] = (unsigned char)next_random(&state);
            put32(payload, next_random(&state) % 12);
            put32(payload + 4, next_random(&state) % 4);
            PureSpan span = {payload, next_random(&state) % sizeof(payload)};
            uint32_t input = next_random(&state) % module_b.inputs;
            if (module_b.input[input].kind == PURE_KIND_MESSAGE) {
                if (pure_app_message(&app, input, span) == PURE_OK) {
                    size_t reply = 0;
                    CHECK(pure_app_reply(&app, count_bytes, &reply) == PURE_OK && reply <= 256);
                }
            } else (e & 1 ? release(&app, input) : press(&app, input));
            frame(&app);
            for (uint32_t s = 0; s < module_b.states; ++s) CHECK(isfinite(app.values[s].num));
        }
        pure_app_stop(&app);
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

/* A stored value's first four bytes (or fewer). */
static const char *read_state(const PureDraft *draft) {
    static Text t;
    t.size = 0;
    size_t size = pure_draft_size(draft) < 4 ? pure_draft_size(draft) : 4;
    CHECK(pure_draft_read(draft, 0, size, text_span, &t) == PURE_OK);
    return t.text;
}

static double number_view(PureApp *app, uint32_t view) {
    double value;
    CHECK(pure_app_view(app, view, &value));
    return value;
}

enum { CUL = PURE_OP_CUR_LOAD, CS = PURE_OP_CUR_STORE, CM = PURE_OP_CUR_MOVE, TI = TEST_TEXT_IN,
       TINS = PURE_OP_TEXT_INSERT, TREM = PURE_OP_TEXT_REMOVE, TL = PURE_OP_TEXT_LOAD, TK = PURE_OP_TEXT_CONST,
       CHR = PURE_OP_CHARS, NT = PURE_OP_NUM_TEXT, CAT = PURE_OP_CONCAT, TLN = PURE_OP_TEXT_LINE,
       CC = PURE_OP_CUR_COUNT, CST = PURE_OP_CUR_START };

/* A small text editor module: states edits (num), note (text), at (cursor in
 * note), title (text). The runtime keeps at on character boundaries and
 * moves it with edits; a failed event restores text and cursor. */
static void texts(void) {
    static const S nums[] = {{"edits", 0}};
    static const T textual[] = {{"note", 1, 0}, {"at", 2, 1}, {"title", 1, 1}};
    static const char *const constants[] = {("\xea\xb0\x80" "a\nbc"), /* apart: \x80a would be one escape */
                                            "\xeb\xa9\x94\xeb\xaa\xa8", "XYZ",
                                            " \xc2\xb7 ", "\xec\x9e\x90"};
    static const I inputs[] = {
        {"typed", NULL, TEXT_INPUT, NULL, 0}, KEY("left", "ArrowLeft"), KEY("right", "ArrowRight"),
        KEY("up", "ArrowUp"), KEY("down", "ArrowDown"), KEY("back", "Backspace"), KEY("home", "Home"), KEY("bad", "KeyB"),
    };
    static const O code[] = {
        {CUL, 2, 0}, {TI, 0, 0}, {TINS, 1, 0}, {L, 0, 0}, {C, 0, 1}, {ADD, 0, 0}, {ST, 0, 0},  /* 0 typed */
        {CUL, 2, 0}, {C, 0, -1}, {CM, IN(1, 0), 0}, {CS, 2, 0},                              /* 7 left */
        {CUL, 2, 0}, {C, 0, 1}, {CM, IN(1, 0), 0}, {CS, 2, 0},                               /* 11 right */
        {CUL, 2, 0}, {C, 0, -1}, {CM, IN(1, 1), 0}, {CS, 2, 0},                              /* 15 up */
        {CUL, 2, 0}, {C, 0, 1}, {CM, IN(1, 1), 0}, {CS, 2, 0},                               /* 19 down */
        {CUL, 2, 0}, {C, 0, -1}, {CM, IN(1, 0), 0}, {CUL, 2, 0}, {TREM, 1, 0},                /* 23 back */
        {CST, 1, 0}, {CS, 2, 0},                                                            /* 28 home */
        {CUL, 2, 0}, {TK, 2, 0}, {TINS, 1, 0}, {C, 0, 1}, {C, 0, 0}, {DIV, 0, 0}, {ST, 0, 0}, /* 30 bad */
        {TL, 1, 0}, {CHR, 0, 0},                                                            /* 37 view chars */
        {CUL, 2, 0}, {CC, IN(1, 0), 0},                                                      /* 39 view offset */
        {CUL, 2, 0}, {CC, IN(1, 1), 0},                                                      /* 41 view line_no */
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
    CHECK(!strcmp(state_text(&app, 1), "\xea\xb0\x80" "a\nbc") && app.values[2].num == 0);
    for (int i = 0; i < 6; ++i) CHECK(press(&app, 2) == PURE_OK);
    CHECK(app.values[2].num == 7);                               /* stops at the end */
    CHECK(number_view(&app, 0) == 5 && number_view(&app, 2) == 1 && !strcmp(view_text(&app, 3), "bc"));
    CHECK(press(&app, 3) == PURE_OK && app.values[2].num == 4);   /* up keeps column 2 */
    const char *han = "\xed\x95\x9c";
    CHECK(type_in(&app, 0, (PureSpan){(const unsigned char *)han, 3}) == PURE_OK);
    CHECK(!strcmp(state_text(&app, 1), "\xea\xb0\x80" "a\xed\x95\x9c\nbc") && app.values[2].num == 7 && app.values[0].num == 1);
    CHECK(press(&app, 5) == PURE_OK && app.values[2].num == 4);   /* back over 3 bytes */
    CHECK(press(&app, 5) == PURE_OK && app.values[2].num == 3);
    CHECK(!strcmp(state_text(&app, 1), "\xea\xb0\x80\nbc"));
    CHECK(press(&app, 1) == PURE_OK && app.values[2].num == 0);
    CHECK(press(&app, 1) == PURE_OK && app.values[2].num == 0);   /* stops at the start */
    CHECK(press(&app, 5) == PURE_OK && app.values[2].num == 0);   /* nothing before */
    CHECK(press(&app, 4) == PURE_OK && app.values[2].num == 4);   /* down to column 0 */
    /* A failing event restores the text, the cursor and the states. */
    CHECK(press(&app, 7) == PURE_INVALID && app.fault == PURE_FAULT_DIVIDE);
    CHECK(!strcmp(state_text(&app, 1), "\xea\xb0\x80\nbc") && app.values[2].num == 4 && app.values[0].num == 1);
    CHECK(!strcmp(view_text(&app, 4), "\xeb\xa9\x94\xeb\xaa\xa8 \xc2\xb7 4\xec\x9e\x90"));
    CHECK(!strcmp(view_text(&app, 5), "0.3333333333333333"));
    CHECK(type_in(&app, 0, (PureSpan){(const unsigned char *)"\xff", 1}) == PURE_INVALID);
    CHECK(type_in(&app, 0, (PureSpan){(const unsigned char *)"\xed\xa0\x80", 3}) == PURE_INVALID); /* surrogate */
    CHECK(type_in(&app, 0, (PureSpan){(const unsigned char *)"\xf0\x9f\x98\x80", 4}) == PURE_OK);
    CHECK(app.values[2].num == 8 && number_view(&app, 1) == 3);   /* offset in characters */
    CHECK(press(&app, 1) == PURE_OK && app.values[2].num == 4);  /* back over the emoji */
    CHECK(press(&app, 6) == PURE_OK && app.values[2].num == 0);
    /* Replacement keeps the text and its cursor by name. */
    static const S fresh[] = {{"fresh", 7}};
    static const T kept[] = {{"note", 1, 0}, {"at", 2, 1}};
    static const char *const other[] = {"ignored"};
    Spec next = {fresh, 1, inputs, 0, number_views, 0, handlers, 0, code, 0, NULL, 0, kept, 2, other, 1, text_views, 0, NULL, 0, NULL, 0};
    CHECK(load(next, &module_b) == PURE_OK);
    CHECK(press(&app, 2) == PURE_OK && app.values[2].num == 3);
    CHECK(pure_app_replace(&app, &module_b) == PURE_OK);
    CHECK(app.values[0].num == 7 && app.values[2].num == 3 && !strcmp(state_text(&app, 1), "\xea\xb0\x80\n\xf0\x9f\x98\x80" "bc"));
    pure_app_stop(&app);

    /* Verification of text operations. */
    static const T two[] = {{"one", 1, 0}, {"two", 1, 0}, {"c", 2, 1}};
    static const O wrong_text[] = {{CUL, 3, 0}, {TK, 0, 0}, {TINS, 2, 0}};
    static const H frame[] = {{0, 0, 3}};
    Spec bad = {nums, 1, inputs, 0, number_views, 0, frame, 1, wrong_text, 3, NULL, 0, two, 3, constants, 1, text_views, 0, NULL, 0, NULL, 0};
    CHECK(load(bad, &module_b) == PURE_INVALID && strstr(error, "cursor of another text"));
    static const O input_in_frame[] = {{CUL, 3, 0}, {TI, 0, 0}, {TINS, 1, 0}};
    bad.o = input_in_frame;
    CHECK(load(bad, &module_b) == PURE_INVALID && strstr(error, "operand type")); /* a frame's field is its dt */
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
            if (module_b.input[input].binding == PURE_BIND_TEXT) {
                const char *word = words[next_random(&state) % LEN(words)];
                type_in(&app, input, (PureSpan){(const unsigned char *)word, strlen(word)});
            } else if (module_b.input[input].binding == PURE_BIND_PRESS) press(&app, input);
            else frame(&app);
            for (uint32_t v = 0; v < module_b.views; ++v) {
                double value;
                if (module_b.view_type[v] == PURE_VIEW_TEXT) pure_app_view_text(&app, v, ignore_span, NULL);
                else pure_app_view(&app, v, &value);
            }
            for (uint32_t s = 0; s < module_b.states; ++s) {
                if (module_b.state_type[s] == PURE_STATE_CURSOR) {
                    PureDraft *text = app.texts[module_b.state_ref[s]];
                    size_t length = pure_draft_size(text), at = (size_t)app.values[s].num;
                    CHECK(app.values[s].num >= 0 && at <= length && app.values[s].num == (double)at);
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
                } else CHECK(isfinite(app.values[s].num));
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
/* ---- Lists as persistent trees (module-list.c): a model array beside a list
 * of thousands of items, changed by events that are committed or undone at
 * random, with a stored copy (a draft) kept byte for byte, shares (a
 * snapshot's copies) that keep what they shared while the list changes on,
 * and a keyed list searched by key. */

typedef struct { double key; char text[12]; size_t size; } ModelItem;
typedef struct { ModelItem *items; size_t count, capacity; } Model;

static void model_insert(Model *m, size_t at, ModelItem item) {
    if (m->count == m->capacity) {
        m->capacity = m->capacity ? 2 * m->capacity : 64;
        m->items = realloc(m->items, m->capacity * sizeof(*m->items));
        CHECK(m->items);
    }
    memmove(m->items + at + 1, m->items + at, (m->count - at) * sizeof(*m->items));
    m->items[at] = item;
    ++m->count;
}

static void model_copy(Model *to, const Model *from) {
    to->count = from->count;
    if (to->capacity < from->count) {
        to->capacity = from->capacity;
        to->items = realloc(to->items, to->capacity * sizeof(*to->items));
        CHECK(to->items);
    }
    if (from->count) memcpy(to->items, from->items, from->count * sizeof(*to->items));
}

static bool same_item(const Model *m, size_t i, double key, PureSpan text) {
    return key == m->items[i].key && text.size == m->items[i].size && (!text.size || !memcmp(text.data, m->items[i].text, text.size));
}

/* The items by position, and run after run from the first and from a middle one. */
static bool same_items(const PureList *list, const Model *m) {
    if (pure_list_count(list) != m->count) return false;
    for (size_t i = 0; i < m->count; ++i) {
        PureCell key = pure_list_get(list, i, 0), text = pure_list_get(list, i, 1);
        if (!same_item(m, i, key.num, (PureSpan){text.text, text.size})) return false;
    }
    PureRun run;
    for (size_t from = 0; from < m->count; from += m->count / 2 + 1) {
        size_t at = 0;
        bool more = pure_list_run(list, from, &run);
        if (!more || from < run.start || from >= run.start + run.count) return false;
        for (at = run.start; more; more = pure_list_next(list, &run)) {
            if (run.start != at || !run.count || run.count > PURE_LIST_RUN) return false;
            for (unsigned i = 0; i < run.count; ++i, ++at)
                if (!same_item(m, at, run.column[0].numbers[i], run.column[1].texts[i])) return false;
        }
        if (at != m->count) return false;
    }
    return !pure_list_run(list, m->count, &run);
}

typedef struct { unsigned char *bytes; size_t size, capacity; } Grown;

static bool collect_bytes(void *context, PureSpan span) {
    Grown *b = context;
    if (b->size + span.size > b->capacity) {
        b->capacity = 2 * (b->size + span.size);
        b->bytes = realloc(b->bytes, b->capacity);
        CHECK(b->bytes);
    }
    if (span.size) memcpy(b->bytes + b->size, span.data, span.size); /* none: a save of changes all undone */
    b->size += span.size;
    return true;
}

/* The stored form of the model: a number's binary64 bits, a text's u32le length and bytes. */
static bool stored_as(const PureList *list, const Model *m) {
    Grown got = {pure_list_size(list) ? malloc(pure_list_size(list)) : NULL, pure_list_size(list), 0};
    CHECK(got.bytes || !got.size);
    pure_list_encode(list, got.bytes);
    size_t at = 0;
    bool same = true;
    for (size_t i = 0; i < m->count && same; ++i) {
        uint64_t bits, size = 0;
        memcpy(&bits, &m->items[i].key, 8);
        for (unsigned k = 0; k < 8; ++k) same = same && at + k < got.size && got.bytes[at + k] == (unsigned char)(bits >> (8 * k));
        at += 8;
        for (unsigned k = 0; k < 4 && at + k < got.size; ++k) size |= (uint64_t)got.bytes[at + k] << (8 * k);
        at += 4;
        same = same && size == m->items[i].size && at + size <= got.size && !memcmp(got.bytes + at, m->items[i].text, size);
        at += size;
    }
    same = same && at == got.size;
    free(got.bytes);
    return same;
}

/* A list read from stored items and given the records stored after them. */
static PureList *stored_list(const Grown *stored, size_t items) {
    static const uint8_t text_fields[2] = {0, 1};
    PureList *list = pure_list_new(2, text_fields);
    CHECK(list && pure_list_read(list, stored->bytes, items) == PURE_OK);
    if (pure_list_replay(list, stored->bytes + items, stored->size - items) != PURE_OK) {
        pure_list_free(list);
        return NULL;
    }
    return list;
}

/* A node no copy reads any more comes back to its list (pure_retired_reuse):
 * the next node the list makes is that one, and a copy still held reads
 * what it shared. */
static void retired_come_back(void) {
    static const uint8_t numbers[1] = {0};
    PureList *list = pure_list_new(1, numbers);
    CHECK(list);
    for (unsigned i = 0; i < 8; ++i) CHECK(pure_list_insert(list, i, &(PureCell){.num = i}, NULL) == PURE_OK);
    PureRetired retired[2] = {{0}};
    PureRun run;
    PureList *older = pure_list_share(list, &retired[0], 1);
    CHECK(older && pure_list_run(list, 0, &run));
    const void *first = run.path[run.depth]; /* the one leaf, which the copy reads */
    CHECK(pure_list_set(list, 0, 0, (PureCell){.num = 100}, NULL) == PURE_OK); /* the list copies it */
    CHECK(pure_list_run(list, 0, &run) && run.path[run.depth] != first);
    PureList *newer = pure_list_share(list, &retired[1], 2);
    CHECK(newer);
    pure_list_free(older); /* no copy reads the first leaf now: it comes back */
    pure_retired_reuse(&retired[0], &list, 1);
    CHECK(pure_list_set(list, 1, 0, (PureCell){.num = 101}, NULL) == PURE_OK); /* a copy of the second, made of it */
    CHECK(pure_list_run(list, 0, &run) && run.path[run.depth] == first);
    CHECK(pure_list_get(list, 0, 0).num == 100 && pure_list_get(list, 1, 0).num == 101);
    CHECK(pure_list_get(newer, 0, 0).num == 100 && pure_list_get(newer, 1, 0).num == 1);
    pure_list_free(newer);
    pure_list_free(list);
    pure_retired_free(&retired[0]);
    pure_retired_free(&retired[1]);
}

/* ---- A keyed list's keys as a set, and fingers (docs/design/020): a list
 * keeping its keys as a set beside its tree answers as a model array does at
 * every step of a random run of adds, removes and key changes (some undone),
 * searches by key, reads by position and clears. Its keys go from bits to a
 * table (a key not a small whole number), leave a table's runs from their
 * middle, and grow bits past what a clear keeps; shares (a snapshot's copies,
 * the same list as it was) and other lists read between, so a finger kept
 * for one is never taken for another. */

static size_t model_lower(const Model *m, double key) {
    size_t low = 0, high = m->count;
    while (low < high) {
        size_t middle = (low + high) / 2;
        if (m->items[middle].key < key) low = middle + 1;
        else high = middle;
    }
    return low;
}

/* A key the set keeps as a bit. */
static bool small_whole(double key) { return key >= 0 && key < 4194304 && floor(key) == key; }

static bool model_has(const Model *m, double key) {
    size_t at = model_lower(m, key);
    return at < m->count && m->items[at].key == key;
}

/* Position at read as the model has it, by get, and the search for its key. */
static bool reads_as(const PureList *list, const Model *m, size_t at) {
    PureCell key = pure_list_get(list, at, 0), text = pure_list_get(list, at, 1);
    bool found;
    return same_item(m, at, key.num, (PureSpan){text.text, text.size}) &&
           pure_list_seek(list, 0, (PureCell){.num = m->items[at].key}, &found) == at && found;
}

static void key_sets(void) {
    static const uint8_t fields[2] = {PURE_CELL_NUMBER, PURE_CELL_TEXT};
    enum { ROUNDS = 120000, OTHERS = 4 };
    uint32_t seed = 0x6b657973u;
    PureList *list = pure_list_new(2, fields), *others[OTHERS], *copy = NULL;
    PureRetired retired = {0};
    PureUndo undo = {0};
    Model model = {0}, shared = {0};
    CHECK(list && pure_list_keyed(list, 0) == PURE_OK);
    /* Items there before it keeps a set: the set starts with them. */
    for (unsigned i = 0; i < 300; ++i) {
        double key = (double)(i * 7);
        PureCell cells[2] = {{.num = key}, {.text = (unsigned char *)"a", .size = 1}};
        CHECK(pure_list_insert(list, i, cells, NULL) == PURE_OK);
        model_insert(&model, i, (ModelItem){key, "a", 1});
    }
    CHECK(pure_list_hashed(list) && pure_list_hashed(list) && pure_list_extra(list) > 0);
    bool found;
    CHECK(pure_list_contains(list, 7, &found) && found && pure_list_contains(list, 8, &found) && !found);
    CHECK(pure_list_contains(list, -0.0, &found) && found); /* -0 is the key 0 */
    for (unsigned o = 0; o < OTHERS; ++o) { /* other lists, read between: key i at i */
        CHECK((others[o] = pure_list_new(2, fields)) && pure_list_keyed(others[o], 0) == PURE_OK);
        for (unsigned i = 0; i < 2000; ++i)
            CHECK(pure_list_insert(others[o], i, (PureCell[]){{.num = i + o}, {.size = 0}}, NULL) == PURE_OK);
    }
    {
        PureList *texts = pure_list_new(2, (const uint8_t[]){PURE_CELL_TEXT, PURE_CELL_NUMBER});
        CHECK(texts && pure_list_keyed(texts, 0) == PURE_OK && !pure_list_hashed(texts) && !pure_list_contains(texts, 1, &found));
        pure_list_free(texts);
    }
    size_t adds = 0, removes = 0, changes = 0, undone = 0, clears = 0, tables = 0, shares = 0;
    bool table = false;
    for (unsigned round = 0; round < ROUNDS; ++round) {
        /* The keys: small whole ones (bits); some past what a clear keeps;
         * then quarters too (a table), dense, so runs are long and removes
         * from their middle many; then whole ones past the bits. */
        unsigned phase = round / (ROUNDS / 4), r = next_random(&seed);
        double key = phase == 0   ? (double)(r % 30000)
                     : phase == 1 ? (r % 997 ? (double)(r % 30000) : (double)(r % 4000000))
                     : phase == 2 ? (double)(r % 24000) / 4
                                  : (r % 5 ? (double)(r % 30000) : 4194304.0 + (double)(r % 100000));
        if (phase == 2 && r % 7 == 0) key = -key; /* negative keys too */
        CHECK(pure_list_contains(list, key, &found) && found == model_has(&model, key));
        unsigned op = next_random(&seed) % 16;
        bool undoing = next_random(&seed) % 9 == 0;
        if (op < 7) { /* add */
            char text[12];
            size_t size = (size_t)snprintf(text, sizeof(text), "%u", round % 100000);
            PureCell cells[2] = {{.num = key}, {.text = (unsigned char *)text, .size = size}};
            size_t at, low = model_lower(&model, key);
            bool exists;
            CHECK(pure_list_add(list, cells, undoing ? &undo : NULL, &at, &exists) == PURE_OK);
            CHECK(at == low && exists == found);
            if (!exists && undoing) {
                pure_undo_rollback(&undo, 0);
                ++undone;
                CHECK(pure_list_contains(list, key, &found) && !found && pure_list_count(list) == model.count);
            } else if (!exists) {
                ModelItem item = {key, {0}, size};
                memcpy(item.text, text, size);
                model_insert(&model, at, item);
                ++adds;
                if (!table && !small_whole(key)) {
                    table = true, ++tables; /* a table: at most half full */
                    CHECK(pure_list_extra(list) >= 2 * model.count * sizeof(uint64_t));
                }
            }
            pure_undo_commit(&undo);
        } else if (op < 10 && model.count) { /* remove */
            size_t at = next_random(&seed) % model.count;
            double gone = model.items[at].key;
            CHECK(pure_list_remove(list, at, undoing ? &undo : NULL) == PURE_OK);
            CHECK(pure_list_contains(list, gone, &found) && !found);
            if (undoing) {
                pure_undo_rollback(&undo, 0);
                ++undone;
                CHECK(pure_list_contains(list, gone, &found) && found && reads_as(list, &model, at));
            } else {
                memmove(model.items + at, model.items + at + 1, (model.count - at - 1) * sizeof(*model.items));
                --model.count;
                ++removes;
            }
            pure_undo_commit(&undo);
        } else if (op < 12 && model.count) { /* a key changed to one between its neighbours */
            size_t at = next_random(&seed) % model.count;
            double was = model.items[at].key, low = at ? model.items[at - 1].key : was - 4,
                   high = at + 1 < model.count ? model.items[at + 1].key : was + 4;
            double to = phase == 2 ? low + (high - low) * (double)(1 + next_random(&seed) % 3) / 4
                                   : floor(low) + 1 + (double)(next_random(&seed) % 3);
            if (to > low && to < high && (phase == 2 || to >= 0)) {
                CHECK(pure_list_set(list, at, 0, (PureCell){.num = to}, undoing ? &undo : NULL) == PURE_OK);
                CHECK(pure_list_contains(list, to, &found) && found);
                CHECK(to == was || (pure_list_contains(list, was, &found) && !found));
                if (undoing) {
                    pure_undo_rollback(&undo, 0);
                    ++undone;
                    CHECK(pure_list_contains(list, was, &found) && found);
                    CHECK(to == was || (pure_list_contains(list, to, &found) && !found));
                } else {
                    model.items[at].key = to;
                    ++changes;
                    if (!table && !small_whole(to)) table = true, ++tables;
                }
                CHECK(reads_as(list, &model, at));
                pure_undo_commit(&undo);
            }
        } else if (op < 14) { /* a search, and the items around what it found */
            size_t at = pure_list_seek(list, 0, (PureCell){.num = key}, &found);
            CHECK(at == model_lower(&model, key) && found == model_has(&model, key));
            for (size_t i = at ? at - 1 : 0; i < at + 2 && i < model.count; ++i) CHECK(reads_as(list, &model, i));
        } else if (model.count) { /* items read in order from one */
            size_t at = next_random(&seed) % model.count;
            for (size_t i = at; i < at + 40 && i < model.count; ++i) {
                PureCell k = pure_list_get(list, i, 0), t = pure_list_get(list, i, 1);
                CHECK(same_item(&model, i, k.num, (PureSpan){t.text, t.size}));
            }
        }
        /* Another list read between: the finger is another's then. */
        unsigned o = next_random(&seed) % OTHERS, i = next_random(&seed) % 2000;
        CHECK(pure_list_get(others[o], i, 0).num == i + o);
        CHECK(pure_list_seek(others[o], 0, (PureCell){.num = i + o + 0.5}, &found) == i + 1 && !found);
        /* A share: the list as it was, read beside it while it changes. */
        if (copy) {
            CHECK(pure_list_count(copy) == shared.count && !pure_list_contains(copy, key, &found));
            if (shared.count) CHECK(reads_as(copy, &shared, next_random(&seed) % shared.count));
            size_t at = pure_list_seek(copy, 0, (PureCell){.num = key}, &found);
            CHECK(at == model_lower(&shared, key) && found == model_has(&shared, key));
        }
        if (round % 6000 == 3000) {
            if (copy) {
                CHECK(same_items(copy, &shared));
                pure_list_free(copy);
            }
            CHECK((copy = pure_list_share(list, &retired, round + 1)) && !pure_list_hashed(copy));
            model_copy(&shared, &model);
            ++shares;
        }
        if (round % 9973 == 9972) { /* cleared: an event's list begins each event empty */
            CHECK(same_items(list, &model));
            for (size_t k = 0; k < model.count; ++k) CHECK(pure_list_contains(list, model.items[k].key, &found) && found);
            pure_list_clear(list);
            model.count = 0, table = false, ++clears;
            CHECK(pure_list_count(list) == 0 && pure_list_contains(list, key, &found) && !found);
        }
    }
    CHECK(same_items(list, &model));
    for (size_t k = 0; k < model.count; ++k) CHECK(pure_list_contains(list, model.items[k].key, &found) && found);
    CHECK(adds > 10000 && removes > 5000 && changes > 1000 && undone > 1000 && tables >= 2 && clears >= 10);
    if (copy) pure_list_free(copy);
    pure_list_free(list);
    for (unsigned o = 0; o < OTHERS; ++o) pure_list_free(others[o]);
    pure_retired_free(&retired);
    pure_undo_free(&undo);
    free(model.items);
    free(shared.items);
    printf("{\"key_sets\":{\"adds\":%zu,\"removes\":%zu,\"changes\":%zu,\"undone\":%zu,\"tables\":%zu,\"clears\":%zu,\"shares\":%zu}}\n",
           adds, removes, changes, undone, tables, clears, shares);
}

/* A stored keyed list adds by key too (an append to it, as an event's):
 * what it journals after the items stored whole makes the list again, with
 * adds of keys it has (no record), adds undone (their records forgotten),
 * removes and key changes among them. */
static void keyed_journal(void) {
    static const uint8_t fields[2] = {PURE_CELL_NUMBER, PURE_CELL_TEXT};
    uint32_t seed = 0x6a6f7572u;
    PureList *list = pure_list_new(2, fields);
    CHECK(list && pure_list_keyed(list, 0) == PURE_OK);
    pure_list_journal(list, true);
    Grown stored = {0};
    size_t stored_items = 0, adds = 0, had = 0, undone = 0, restarts = 0;
    PureUndo undo = {0};
    Model model = {0}, before = {0};
    for (unsigned event = 0; event < 3000; ++event) {
        size_t mark = undo.count, journal = pure_list_changes(list).size;
        model_copy(&before, &model);
        for (unsigned change = 1 + (unsigned)(next_random(&seed) % 12); change--;) {
            unsigned roll = (unsigned)(next_random(&seed) % 100);
            if (roll < 60 || !model.count) {
                ModelItem item = {(double)(next_random(&seed) % 20000) / 2 - 2000, "", next_random(&seed) % 11};
                for (size_t k = 0; k < item.size; ++k) item.text[k] = (char)('a' + next_random(&seed) % 26);
                PureCell cells[2] = {{.num = item.key}, {.text = (unsigned char *)item.text, .size = item.size}};
                size_t at;
                bool exists;
                CHECK(pure_list_add(list, cells, &undo, &at, &exists) == PURE_OK);
                CHECK(at == model_lower(&model, item.key) && exists == model_has(&model, item.key));
                if (exists) ++had;
                else model_insert(&model, at, item), ++adds;
            } else if (roll < 85) {
                size_t at = next_random(&seed) % model.count;
                CHECK(pure_list_remove(list, at, &undo) == PURE_OK);
                memmove(model.items + at, model.items + at + 1, (model.count - at - 1) * sizeof(*model.items));
                --model.count;
            } else { /* a key changed to one between its neighbours */
                size_t at = next_random(&seed) % model.count;
                double low = at ? model.items[at - 1].key : model.items[at].key - 1,
                       high = at + 1 < model.count ? model.items[at + 1].key : model.items[at].key + 1;
                double to = low + (high - low) / 2;
                if (to > low && to < high) {
                    CHECK(pure_list_set(list, at, 0, (PureCell){.num = to}, &undo) == PURE_OK);
                    model.items[at].key = to;
                }
            }
        }
        if (next_random(&seed) % 5 == 0) { /* undone: the list and its journal as before */
            pure_undo_rollback(&undo, mark);
            pure_list_forget(list, journal);
            model_copy(&model, &before);
            ++undone;
        } else
            pure_undo_commit(&undo);
        if (event % 3 == 0) { /* a save: its records after the stored ones, or now and then the items whole */
            if (event % 120 == 0) {
                stored.size = 0;
                Grown whole = {malloc(pure_list_size(list) + 1), 0, 0};
                CHECK(whole.bytes);
                pure_list_encode(list, whole.bytes);
                collect_bytes(&stored, (PureSpan){whole.bytes, pure_list_size(list)});
                free(whole.bytes);
                stored_items = pure_list_size(list);
            } else
                collect_bytes(&stored, pure_list_changes(list));
            pure_list_forget(list, 0);
            if (event % 17 == 0) { /* what a restart finds is the list as it is */
                PureList *restarted = stored_list(&stored, stored_items);
                CHECK(restarted && same_items(restarted, &model));
                pure_list_free(restarted);
                ++restarts;
            }
        }
    }
    CHECK(same_items(list, &model) && adds > 5000 && had > 500 && undone > 400 && restarts > 40);
    pure_list_free(list);
    pure_undo_free(&undo);
    free(stored.bytes);
    free(model.items);
    free(before.items);
    printf("{\"keyed_journal\":{\"adds\":%zu,\"had\":%zu,\"undone\":%zu,\"restarts\":%zu}}\n", adds, had, undone, restarts);
}

/* An event's list in arrays (docs/design/021): beside a list that is a tree
 * alone, the same random run of changes and questions gets the same answers
 * at every step, whatever the arrays are then (FLAT, DENSE, or a tree after
 * all): adds by key (where asked and not), inserts and removes anywhere,
 * changes of a text and of a key, finds, reads by position and run by run,
 * the field found by key, clears between events. Keys run small and whole
 * (DENSE), past what DENSE may hold, and fractional (then the set is a
 * table), so every change of form happens, back and forth. */
static void same_list(const PureList *a, const PureList *t) {
    CHECK(pure_list_count(a) == pure_list_count(t) && pure_list_size(a) == pure_list_size(t));
    size_t n = pure_list_count(t);
    for (size_t i = 0; i < n; i += 1 + i / 64) {
        PureCell x = pure_list_get(a, i, 0), y = pure_list_get(t, i, 0), u = pure_list_get(a, i, 1), v = pure_list_get(t, i, 1),
                 w = pure_list_get(a, i, 2), z = pure_list_get(t, i, 2);
        CHECK(x.i64 == y.i64 && u.size == v.size && (!u.size || !memcmp(u.text, v.text, u.size)) && w.i64 == z.i64);
    }
}

/* A third field's number: in some events small, in some large, in some
 * anything (fractions, -0, below 0), so its column narrows and widens
 * (docs/design/022). */
static double some_number(unsigned event, uint32_t r) {
    switch (event % 4) {
    case 0: case 3: return (double)(r % 250);
    case 1: return (double)(r % 4000000000u);
    default: return r % 5 == 0 ? -0.0 : r % 5 == 1 ? -(double)(r % 77) : r % 5 == 2 ? (double)(r % 1000) / 8 : (double)(r % 300);
    }
}

/* An item into a list keyed by its text field 0, as the executor does: added at once (HASHED; not when where it went is
 * asked, as an index state asks), else found and inserted. */
static PureResult text_add(PureList *list, const PureCell *cells, bool ask, bool *exists) {
    size_t where;
    PureResult result = pure_list_add(list, cells, NULL, ask ? &where : NULL, exists);
    if (result != PURE_INVALID) return result;
    size_t at = pure_list_seek(list, 0, cells[0], exists);
    return *exists ? PURE_OK : pure_list_insert(list, at, cells, NULL);
}

/* An event's list keyed by a text (HASHED by its texts' hashes, docs/design
 * 023) against a tree keyed so: adds, has, a field by key, a set where its
 * key was found, positions, runs, removes, and the event's end. */
static void text_keys_equivalence(void) {
    static const uint8_t fields[3] = {PURE_CELL_TEXT, PURE_CELL_NUMBER, PURE_CELL_TEXT};
    uint32_t seed = 0x74657874u;
    size_t hashed = 0, steps = 0;
    PureList *a = pure_list_new(3, fields), *t = pure_list_new(3, fields);
    CHECK(a && t && pure_list_keyed(a, 0) == PURE_OK && pure_list_keyed(t, 0) == PURE_OK && pure_list_arrays(a));
    for (unsigned event = 0; event < 120; ++event) {
        unsigned changes = 20 + next_random(&seed) % (event % 5 == 0 ? 3000 : 300), words = 1 + next_random(&seed) % 2000;
        for (unsigned c = 0; c < changes; ++c, ++steps) {
            uint32_t r = next_random(&seed);
            char key[24], other[16];
            size_t size = (size_t)snprintf(key, sizeof(key), r % 7 ? "w%u" : "%u-word", (r >> 3) % words);
            size_t other_size = (size_t)snprintf(other, sizeof(other), "o%u", r % 97);
            PureCell cells[3] = {{.text = (unsigned char *)key, .size = size}, {.num = (double)(r % 1000)},
                                 {.text = (unsigned char *)other, .size = r % 5 ? other_size : 0}};
            size_t n = pure_list_count(t);
            unsigned op = next_random(&seed) % (event % 4 == 1 ? 10 : 16); /* some events by key alone: HASHED stays */
            if (op < 5) {
                bool ea, et;
                CHECK(text_add(a, cells, event % 3 == 2 && r % 31 == 0, &ea) == PURE_OK && text_add(t, cells, false, &et) == PURE_OK && ea == et);
            } else if (op < 8) { /* has, and fields by key */
                bool ha, ht, fa, ft;
                PureCell ca, ct;
                if (!pure_list_contains_text(a, cells[0], &ha)) (void)pure_list_seek(a, 0, cells[0], &ha);
                (void)pure_list_seek(t, 0, cells[0], &ht);
                CHECK(ha == ht);
                fa = pure_list_lookup(a, 0, cells[0], 1, &ca);
                ft = pure_list_lookup(t, 0, cells[0], 1, &ct);
                CHECK(fa == ft && fa == ht && (!fa || ca.i64 == ct.i64));
                fa = pure_list_lookup(a, 0, cells[0], 2, &ca);
                ft = pure_list_lookup(t, 0, cells[0], 2, &ct);
                CHECK(fa == ft && (!fa || (ca.size == ct.size && (!ca.size || !memcmp(ca.text, ct.text, ca.size)))));
            } else if (op < 10) { /* set where its key was found (a slot, in a HASHED list) */
                bool fa, ft;
                size_t ra = pure_list_find_row(a, 0, cells[0], &fa), rt = pure_list_seek(t, 0, cells[0], &ft);
                CHECK(fa == ft);
                if (fa) {
                    unsigned field = r % 2 ? 1 : 2;
                    CHECK(pure_list_set_row(a, ra, field, cells[field], NULL) == PURE_OK &&
                          pure_list_set(t, rt, field, cells[field], NULL) == PURE_OK);
                }
            } else if (op < 12 && n) { /* by position: in order now */
                size_t at = next_random(&seed) % n;
                PureCell x = pure_list_get(a, at, 0), y = pure_list_get(t, at, 0), w = pure_list_get(a, at, 1), z = pure_list_get(t, at, 1);
                CHECK(x.size == y.size && !memcmp(x.text, y.text, x.size) && w.i64 == z.i64);
            } else if (op < 14 && n) { /* a run from somewhere */
                size_t at = next_random(&seed) % n;
                PureRun ra, rt;
                CHECK(pure_list_run(a, at, &ra) && pure_list_run(t, at, &rt));
                PureSpan sa = ra.column[0].texts[at - ra.start], st = rt.column[0].texts[at - rt.start];
                CHECK(sa.size == st.size && (!sa.size || !memcmp(sa.data, st.data, sa.size)));
            } else if (op < 16 && n) { /* remove */
                size_t at = next_random(&seed) % n;
                CHECK(pure_list_remove(a, at, NULL) == PURE_OK && pure_list_remove(t, at, NULL) == PURE_OK);
            }
            hashed += pure_list_form(a) == 3;
        }
        CHECK(pure_list_count(a) == pure_list_count(t) && pure_list_size(a) == pure_list_size(t));
        for (size_t i = 0; i < pure_list_count(t); i += 1 + i / 64)
            for (unsigned f = 0; f < 3; ++f) {
                PureCell x = pure_list_get(a, i, f), y = pure_list_get(t, i, f);
                CHECK(f == 1 ? x.i64 == y.i64 : x.size == y.size && (!x.size || !memcmp(x.text, y.text, x.size)));
            }
        pure_list_clear(a);
        pure_list_clear(t);
    }
    pure_list_free(a);
    pure_list_free(t);
    printf("{\"text_keys\":{\"steps\":%zu,\"hashed\":%zu}}\n", steps, hashed);
    CHECK(hashed > 8000);
}

static void arrays_equivalence(void) {
    static const uint8_t fields[3] = {PURE_CELL_NUMBER, PURE_CELL_TEXT, PURE_CELL_NUMBER};
    uint32_t seed = 0x61727261u;
    size_t forms[4] = {0}, conversions = 0, steps = 0;
    for (unsigned keyed = 0; keyed < 2; ++keyed) {
        PureList *a = pure_list_new(3, fields), *t = pure_list_new(3, fields);
        CHECK(a && t);
        if (keyed) {
            CHECK(pure_list_keyed(a, 0) == PURE_OK && pure_list_keyed(t, 0) == PURE_OK);
            CHECK(pure_list_hashed(a) && pure_list_hashed(t));
        }
        CHECK(pure_list_arrays(a) && pure_list_form(a) == 1);
        unsigned form = pure_list_form(a);
        for (unsigned event = 0; event < 400; ++event) {
            /* Each event's keys: small and whole, sparse, or with fractions. */
            unsigned kind = event % 5, changes = 50 + next_random(&seed) % (event % 7 == 0 ? 4000 : 400);
            for (unsigned c = 0; c < changes; ++c, ++steps) {
                uint32_t r = next_random(&seed);
                size_t n = pure_list_count(t);
                double key = kind < 3 ? (double)(r % 20000)
                             : kind == 3 ? (double)(r % 4000000)
                                         : (double)(r % 80000) / 4;
                char text[16];
                size_t size = (size_t)snprintf(text, sizeof(text), "t%u", r % 1000);
                PureCell cells[3] = {{.num = key}, {.text = (unsigned char *)text, .size = r % 11 ? size : 0},
                                     {.num = some_number(event, r >> 7)}};
                unsigned op = next_random(&seed) % 20;
                if (keyed && event % 3 == 1) op = op < 12 ? 1 + op % 8 : 13 + op % 2; /* by key alone, as a search does: DENSE stays */
                if (keyed && op < 9) { /* add by key, where it went asked or not */
                    bool ask = op == 0, ea, et;
                    size_t at_a = 0, at_t = 0;
                    CHECK(pure_list_add(a, cells, NULL, ask ? &at_a : NULL, &ea) == PURE_OK);
                    CHECK(pure_list_add(t, cells, NULL, &at_t, &et) == PURE_OK);
                    CHECK(ea == et && (!ask || at_a == at_t));
                } else if (!keyed && op < 9) { /* insert anywhere: mostly the end */
                    size_t at = op < 6 ? n : n ? next_random(&seed) % (n + 1) : 0;
                    CHECK(pure_list_insert(a, at, cells, NULL) == PURE_OK && pure_list_insert(t, at, cells, NULL) == PURE_OK);
                } else if (op < 11 && n) { /* remove */
                    size_t at = next_random(&seed) % n;
                    CHECK(pure_list_remove(a, at, NULL) == PURE_OK && pure_list_remove(t, at, NULL) == PURE_OK);
                } else if (op < 12 && n) { /* a text changed; in a keyed list now and then its key, between its neighbours */
                    size_t at = next_random(&seed) % n;
                    CHECK(pure_list_set(a, at, 1, cells[1], NULL) == PURE_OK && pure_list_set(t, at, 1, cells[1], NULL) == PURE_OK);
                    if (r % 2) /* the third field: wider now and then */
                        CHECK(pure_list_set(a, at, 2, (PureCell){.num = some_number(event + r % 3, r >> 9)}, NULL) == PURE_OK &&
                              pure_list_set(t, at, 2, (PureCell){.num = some_number(event + r % 3, r >> 9)}, NULL) == PURE_OK);
                    if (keyed && at + 1 < n && r % 3 == 0) {
                        double low = pure_list_get(t, at, 0).num, high = pure_list_get(t, at + 1, 0).num, to = low + (high - low) / 2;
                        if (to > low && to < high) {
                            CHECK(pure_list_set(a, at, 0, (PureCell){.num = to}, NULL) == PURE_OK);
                            CHECK(pure_list_set(t, at, 0, (PureCell){.num = to}, NULL) == PURE_OK);
                        }
                    }
                } else if (keyed && op < 15) { /* found, has, and its field by key */
                    bool fa, ft, ha, ht;
                    PureCell ca, ct;
                    if (op == 12) CHECK(pure_list_seek(a, 0, cells[0], &fa) == pure_list_seek(t, 0, cells[0], &ft) && fa == ft);
                    CHECK(pure_list_contains(a, key, &ha) && pure_list_contains(t, key, &ht) && ha == ht);
                    fa = pure_list_lookup(a, 0, cells[0], 1, &ca);
                    ft = pure_list_lookup(t, 0, cells[0], 1, &ct);
                    CHECK(fa == ft && fa == ht && (!fa || (ca.size == ct.size && (!ca.size || !memcmp(ca.text, ct.text, ca.size)))));
                    fa = pure_list_lookup(a, 0, cells[0], 2, &ca);
                    ft = pure_list_lookup(t, 0, cells[0], 2, &ct);
                    CHECK(fa == ft && (!fa || ca.i64 == ct.i64));
                    fa = pure_list_lookup(a, 0, cells[0], 0, &ca); /* the key itself: a DENSE list's slot */
                    ft = pure_list_lookup(t, 0, cells[0], 0, &ct);
                    CHECK(fa == ft && (!fa || ca.i64 == ct.i64));
                } else if (op < 17 && n) { /* by position */
                    size_t at = next_random(&seed) % n;
                    PureCell x = pure_list_get(a, at, 0), y = pure_list_get(t, at, 0), w = pure_list_get(a, at, 2),
                             z = pure_list_get(t, at, 2);
                    CHECK(x.i64 == y.i64 && w.i64 == z.i64);
                } else if (op < 18 && n) { /* run by run from somewhere */
                    size_t at = next_random(&seed) % n, seen = 0;
                    PureRun ra, rt;
                    CHECK(pure_list_run(a, at, &ra) && pure_list_run(t, at, &rt));
                    size_t ia = at, it = at;
                    for (bool ma = true, mt = true; seen < 200 && ma && mt; ++seen) {
                        while (ma && ia >= ra.start + ra.count) ma = pure_list_next(a, &ra);
                        while (mt && it >= rt.start + rt.count) mt = pure_list_next(t, &rt);
                        if (!ma || !mt) {
                            CHECK(ma == mt);
                            break;
                        }
                        CHECK(ra.column[0].numbers[ia - ra.start] == rt.column[0].numbers[it - rt.start]);
                        CHECK(!memcmp(&ra.column[2].numbers[ia - ra.start], &rt.column[2].numbers[it - rt.start], sizeof(double)));
                        PureSpan sa = ra.column[1].texts[ia - ra.start], st = rt.column[1].texts[it - rt.start];
                        CHECK(sa.size == st.size && (!sa.size || !memcmp(sa.data, st.data, sa.size)));
                        ++ia, ++it;
                    }
                }
                if (pure_list_form(a) != form) ++conversions, form = pure_list_form(a);
                ++forms[form];
            }
            same_list(a, t);
            pure_list_clear(a); /* the event's end */
            pure_list_clear(t);
            CHECK(pure_list_count(a) == 0 && pure_list_form(a) == 1);
            form = 1;
        }
        pure_list_free(a);
        pure_list_free(t);
    }
    CHECK(forms[0] > 10000 && forms[1] > 10000 && forms[2] > 10000 && forms[3] > 10000 && conversions > 100);
    /* A snapshot shares every list: an event's, empty between events, stays in arrays; one with items is a tree for its copy. */
    {
        PureList *list = pure_list_new(3, fields);
        PureRetired retired = {0};
        CHECK(list && pure_list_arrays(list));
        PureList *copy = pure_list_share(list, &retired, 1);
        CHECK(copy && pure_list_count(copy) == 0 && pure_list_form(list) == 1);
        pure_list_free(copy);
        for (unsigned i = 0; i < 100; ++i)
            CHECK(pure_list_insert(list, i, (PureCell[]){{.num = i}, {.size = 0}, {.num = i * 1000.5}}, NULL) == PURE_OK);
        CHECK((copy = pure_list_share(list, &retired, 2)) && pure_list_form(list) == 0 && pure_list_count(copy) == 100);
        CHECK(pure_list_get(copy, 99, 0).num == 99 && pure_list_get(copy, 99, 2).num == 99049.5);
        pure_list_free(copy);
        pure_list_clear(list);
        CHECK(pure_list_form(list) == 1); /* arrays again, the next event */
        pure_list_free(list);
        pure_retired_free(&retired);
    }
    printf("{\"arrays\":{\"steps\":%zu,\"tree\":%zu,\"flat\":%zu,\"dense\":%zu,\"hashed\":%zu,\"conversions\":%zu}}\n", steps,
           forms[0], forms[1], forms[2], forms[3], conversions);
}

/* The first position in a model (in key order) whose key is not below key. */
static size_t model_bound(const Model *m, double key) {
    size_t low = 0, high = m->count;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        if (m->items[middle].key < key) low = middle + 1;
        else high = middle;
    }
    return low;
}

/* Two lists' stored forms, whole and a part at a time, the same. */
static bool stored_numbers_same(const PureList *a, const PureList *b) {
    size_t size = pure_list_size(a);
    unsigned char *x = malloc(size + 1), *y = malloc(size + 1);
    CHECK(x && y && size == pure_list_size(b));
    pure_list_encode(a, x);
    pure_list_encode(b, y);
    bool same = !memcmp(x, y, size);
    free(x), free(y);
    return same;
}

/* The stored form of a list a part at a time, against it whole. */
static bool encoded_in_parts(const PureList *list, size_t capacity) {
    size_t size = pure_list_size(list), at = 0, used = 0, needed;
    unsigned char *whole = malloc(size + 1), *parts = malloc(size + 1), *buffer = malloc(capacity);
    CHECK(whole && parts && buffer);
    pure_list_encode(list, whole);
    for (;;) {
        size_t got = pure_list_encode_part(list, &at, buffer, capacity, &needed);
        if (!got) {
            if (!needed) break;
            unsigned char *grown = realloc(buffer, needed);
            CHECK(grown);
            buffer = grown, capacity = needed;
            continue;
        }
        CHECK(used + got <= size);
        memcpy(parts + used, buffer, got);
        used += got;
    }
    bool same = used == size && at == pure_list_count(list) && (!size || !memcmp(whole, parts, size));
    free(whole), free(parts), free(buffer);
    return same;
}

/* A stored keyed list packed between events once searched often (docs/design
 * 022): at every step its answers are a model's, through searches by key at
 * a bucket's start and between, its changes (a tree again), their undoing
 * (also of changes made before it was packed), its journal and stored form,
 * a restart from them, and its copies, which share its pack and outlive its
 * changes. Then packs of keys at the edges of the index: fractions, near
 * 2^53, negative, and a span too wide for buckets. */
static void packed_lists(void) {
    static const uint8_t fields[2] = {PURE_CELL_NUMBER, PURE_CELL_TEXT};
    uint32_t seed = 0x7061636bu;
    PureList *list = pure_list_new(2, fields);
    CHECK(list && pure_list_keyed(list, 0) == PURE_OK && pure_list_hashed(list));
    pure_list_journal(list, true);
    PureUndo undo = {0};
    Model model = {0}, before = {0};
    Grown stored = {0};
    size_t stored_items = 0;
    enum { SHARES = 4 };
    PureList *copies[SHARES];
    Model shared[SHARES] = {{0}};
    PureRetired retired[SHARES] = {{0}};
    unsigned first = 0, held = 0, open = 0;
    uint64_t epoch = 0;
    size_t packs = 0, unpacked = 0, searches = 0, packed_searches = 0, undone_packed = 0, mark = 0, journal = 0;
    for (unsigned event = 0; event < 3000; ++event) {
        if (!open) mark = undo.count, journal = pure_list_changes(list).size, model_copy(&before, &model);
        bool was_packed = pure_list_packed(list);
        /* Grow to some thousands, searched at times in bursts (packing it), changed at others. */
        unsigned changes = event < 300 ? 60 : next_random(&seed) % 4 ? 0 : 1 + next_random(&seed) % 6;
        for (unsigned c = 0; c < changes; ++c) {
            uint32_t r = next_random(&seed);
            unsigned roll = r % 100;
            if (roll < 60 || model.count < 100) {
                ModelItem item = {(double)(next_random(&seed) % 700) * 1024 + next_random(&seed) % 1000, "", r % 11};
                for (size_t k = 0; k < item.size; ++k) item.text[k] = (char)('a' + next_random(&seed) % 26);
                PureCell cells[2] = {{.num = item.key}, {.text = (unsigned char *)item.text, .size = item.size}};
                size_t at;
                bool exists;
                CHECK(pure_list_add(list, cells, &undo, &at, &exists) == PURE_OK);
                size_t want = model_bound(&model, item.key);
                CHECK(at == want && exists == (want < model.count && model.items[want].key == item.key));
                if (!exists) model_insert(&model, at, item);
            } else if (roll < 80) {
                size_t at = next_random(&seed) % model.count;
                CHECK(pure_list_remove(list, at, &undo) == PURE_OK);
                memmove(model.items + at, model.items + at + 1, (model.count - at - 1) * sizeof(*model.items));
                --model.count;
            } else {
                size_t at = next_random(&seed) % model.count;
                ModelItem *item = &model.items[at];
                if (r % 3 == 0 && at + 1 < model.count && item[1].key - item[0].key > 1) { /* its key, between its neighbours */
                    double key = item[0].key + 1;
                    CHECK(pure_list_set(list, at, 0, (PureCell){.num = key}, &undo) == PURE_OK);
                    item->key = key;
                } else {
                    item->size = next_random(&seed) % 11;
                    for (size_t k = 0; k < item->size; ++k) item->text[k] = (char)('A' + next_random(&seed) % 26);
                    CHECK(pure_list_set(list, at, 1, (PureCell){.text = (unsigned char *)item->text, .size = item->size}, &undo) ==
                          PURE_OK);
                }
            }
        }
        unpacked += was_packed && !pure_list_packed(list);
        /* Searches: a node's slice bounds (at a bucket's start once packed), a key between, by halves of keys, a field by key. */
        unsigned burst = event % 9 == 4 ? 1500 + next_random(&seed) % 1500 : next_random(&seed) % 40;
        for (unsigned k = 0; k < burst && model.count; ++k, ++searches) {
            uint32_t r = next_random(&seed);
            double key = r % 4 == 0 ? (double)(r % 720) * 1024
                         : r % 4 == 1 ? model.items[r % model.count].key
                         : r % 4 == 2 ? (double)(next_random(&seed) % 740000) - 10000
                                      : (double)(next_random(&seed) % 2900000) / 4;
            size_t want = model_bound(&model, key);
            bool found, want_found = want < model.count && model.items[want].key == key;
            packed_searches += pure_list_packed(list);
            CHECK(pure_list_bound(list, 0, (PureCell){.num = key}) == want);
            if (r % 3 == 0) CHECK(pure_list_seek(list, 0, (PureCell){.num = key}, &found) == want && found == want_found);
            if (r % 5 == 0) {
                PureCell text;
                CHECK(pure_list_lookup(list, 0, (PureCell){.num = key}, 1, &text) == want_found);
                if (want_found) CHECK(same_item(&model, want, key, (PureSpan){text.text, text.size}));
            }
        }
        /* The event's end: kept, or undone (now and then with the one before it, packed between them). */
        if (open || next_random(&seed) % 8) {
            if (!open && next_random(&seed) % 12 == 0) {
                ++open; /* left open across one more event */
            } else if (next_random(&seed) % 6 == 0) {
                undone_packed += pure_list_packed(list);
                pure_undo_rollback(&undo, mark);
                pure_list_forget(list, journal);
                model_copy(&model, &before);
                open = 0;
            } else {
                pure_undo_commit(&undo);
                open = 0;
            }
        } else {
            pure_undo_rollback(&undo, mark);
            pure_list_forget(list, journal);
            model_copy(&model, &before);
        }
        if (pure_list_settle(list)) ++packs;
        if (event % 11 == 0) CHECK(same_items(list, &model));
        if (!open && event % 3 == 0) { /* a save: records, or now and then the items whole */
            if (event % 90 == 0) {
                stored.size = 0;
                Grown whole = {malloc(pure_list_size(list) + 1), 0, 0};
                CHECK(whole.bytes);
                pure_list_encode(list, whole.bytes);
                collect_bytes(&stored, (PureSpan){whole.bytes, pure_list_size(list)});
                free(whole.bytes);
                stored_items = pure_list_size(list);
            } else
                collect_bytes(&stored, pure_list_changes(list));
            pure_list_forget(list, 0);
        }
        if (!open && event % 3 == 0 && event % 51 == 0) {
            PureList *restarted = stored_list(&stored, stored_items);
            CHECK(restarted && same_items(restarted, &model));
            pure_list_free(restarted);
            CHECK(stored_as(list, &model) && encoded_in_parts(list, 64 + next_random(&seed) % 4096));
        }
        if (!open && event % 30 == 15) { /* a share, as a snapshot takes one */
            if (held == SHARES) {
                CHECK(same_items(copies[first], &shared[first]) && stored_as(copies[first], &shared[first]));
                CHECK(encoded_in_parts(copies[first], 100));
                pure_list_free(copies[first]);
                pure_retired_reuse(&retired[first], &list, 1);
                first = (first + 1) % SHARES;
                --held;
            }
            unsigned slot = (first + held) % SHARES;
            CHECK((copies[slot] = pure_list_share(list, &retired[slot], ++epoch)));
            CHECK(pure_list_packed(copies[slot]) == pure_list_packed(list));
            model_copy(&shared[slot], &model);
            ++held;
        }
        if (event % 25 == 0)
            for (unsigned i = 0; i < held; ++i) {
                const PureList *copy = copies[(first + i) % SHARES];
                const Model *m = &shared[(first + i) % SHARES];
                CHECK(same_items(copy, m));
                for (unsigned k = 0; k < 50 && m->count; ++k) {
                    double key = (double)(next_random(&seed) % 720) * 1024;
                    CHECK(pure_list_bound(copy, 0, (PureCell){.num = key}) == model_bound(m, key));
                }
            }
    }
    CHECK(same_items(list, &model) && stored_as(list, &model));
    CHECK(packs > 20 && unpacked > 10 && packed_searches > 10000 && undone_packed > 0);
    pure_list_free(list); /* first, as an app's: its copies read on what they share */
    for (; held; --held, first = (first + 1) % SHARES) {
        CHECK(same_items(copies[first], &shared[first]));
        pure_list_free(copies[first]);
        pure_retired_free(&retired[first]);
    }
    pure_undo_free(&undo);
    free(stored.bytes);
    free(model.items);
    free(before.items);
    for (unsigned i = 0; i < SHARES; ++i) free(shared[i].items);

    /* The index's edges: each list packed, then searched as its tree (never packed) is; its second field's
     * cells packed as narrow as they let (docs/design/022) and read back as they were, by position and by runs. */
    static const uint8_t number[2] = {PURE_CELL_NUMBER, PURE_CELL_NUMBER};
    size_t indexed = 0;
    for (unsigned kind = 0; kind < 5; ++kind) {
        PureList *packed = pure_list_new(2, number), *tree = pure_list_new(2, number);
        CHECK(packed && tree && pure_list_keyed(packed, 0) == PURE_OK && pure_list_keyed(tree, 0) == PURE_OK);
        double keys[6000];
        size_t n = 0;
        for (size_t i = 0; i < 6000; ++i) {
            double key = kind == 0 ? (double)i * 0.25 - 700         /* fractions, negative and positive, -0 among them */
                         : kind == 1 ? 9007199254740992.0 - 2 * (double)(6000 - i) * 2 /* near 2^53: the keys' last bits */
                         : kind == 2 ? -1e300 + (double)i * 1e296   /* wide, negative */
                         : kind == 3 ? (i % 2 ? 1e300 : -1e300) * (double)(i + 1) / 6000 /* too wide for buckets */
                                     : (double)(i / 7) * 65536 + (double)(i % 7) * 3; /* slices at multiples of 2^16 */
            if (kind == 0 && i == 2800) key = -0.0;
            bool exists;
            double second = some_number(kind, (uint32_t)(i * 2654435761u)), third = kind == 4 ? (double)(i % 200) : second;
            CHECK(pure_list_add(packed, (PureCell[]){{.num = key}, {.num = third}}, NULL, NULL, &exists) == PURE_OK);
            CHECK(pure_list_add(tree, (PureCell[]){{.num = key}, {.num = third}}, NULL, NULL, &exists) == PURE_OK);
            keys[n++] = key;
        }
        for (size_t i = 0; i < 1000; ++i) { /* searched enough, at the slices' starts for the last kind */
            bool found;
            double key = kind == 4 ? (double)(next_random(&seed) % 900) * 65536 : keys[next_random(&seed) % n];
            (void)pure_list_seek(packed, 0, (PureCell){.num = key}, &found);
        }
        CHECK(pure_list_settle(packed) && pure_list_packed(packed));
        for (size_t i = 0; i < n; ++i) {
            PureCell x = pure_list_get(packed, i, 1), y = pure_list_get(tree, i, 1);
            CHECK(x.i64 == y.i64);
        }
        PureRun rp, rt;
        size_t at = n / 3, read = 0;
        CHECK(pure_list_run(packed, at, &rp) && pure_list_run(tree, at, &rt));
        for (size_t ip = at, it = at; read < 500; ++read, ++ip, ++it) {
            bool mp = true, mt = true;
            while (mp && ip >= rp.start + rp.count) mp = pure_list_next(packed, &rp);
            while (mt && it >= rt.start + rt.count) mt = pure_list_next(tree, &rt);
            CHECK(mp == mt);
            if (!mp) break;
            CHECK(!memcmp(&rp.column[1].numbers[ip - rp.start], &rt.column[1].numbers[it - rt.start], sizeof(double)));
        }
        CHECK(stored_numbers_same(packed, tree));
        for (size_t i = 0; i < 20000; ++i) {
            uint32_t r = next_random(&seed);
            double a = keys[r % n], b = keys[(r / 7) % n];
            double key = r % 5 == 0 ? a : r % 5 == 1 ? a + (b - a) * 0.37 : r % 5 == 2 ? -a : r % 5 == 3 ? a * 1.0000001
                         : kind == 4 ? (double)(r % 900) * 65536 : (r % 2 ? 1 : -1) * 1e308;
            bool fp, ft;
            size_t got = pure_list_seek(packed, 0, (PureCell){.num = key}, &fp), want = pure_list_seek(tree, 0, (PureCell){.num = key}, &ft);
            CHECK(got == want && fp == ft && pure_list_bound(packed, 0, (PureCell){.num = key}) == want);
        }
        indexed += kind != 3; /* the too wide one has no buckets: searched by halves */
        pure_list_free(packed);
        pure_list_free(tree);
    }
    printf("{\"packed\":{\"searches\":%zu,\"packed_searches\":%zu,\"packs\":%zu,\"unpacked\":%zu,\"undone_packed\":%zu,\"edges\":%zu}}\n",
           searches, packed_searches, packs, unpacked, undone_packed, indexed);
}

static void list_trees(void) {
    static const uint8_t text_fields[2] = {0, 1};
    uint32_t seed = 0x5eed1234u;
    PureList *list = pure_list_new(2, text_fields);
    CHECK(list);
    pure_list_journal(list, true);
    /* What a store would hold: the items as last stored whole, then the
     * records of each save since. */
    Grown stored = {0};
    size_t stored_items = 0, saves = 0, rewrites = 0;
    PureUndo undo = {0};
    Model model = {0}, before = {0};
    /* Up to four shares at a time, each with what it shared and the items
     * and nodes retired while it was the newest: when the oldest goes, no
     * copy reads those, and the list makes its next nodes of them. */
    enum { SHARES = 4 };
    PureList *copies[SHARES];
    Model shared[SHARES] = {{0}};
    PureRetired retired[SHARES + 1] = {{0}};
    unsigned first = 0, held = 0;
    uint64_t epoch = 0; /* the copies', one after another */
    size_t largest = 0, events = 0, undone = 0;
    for (unsigned event = 0; event < 6000; ++event) {
        size_t mark = undo.count, journal = pure_list_changes(list).size;
        model_copy(&before, &model);
        /* Mostly growing to a few thousand, then shrinking back down, so the
         * tree gains and loses levels. */
        unsigned grow = event < 2500 ? 70 : event < 4500 ? 30 : 60;
        for (unsigned change = 1 + (unsigned)(next_random(&seed) % 12); change--;) {
            unsigned roll = (unsigned)(next_random(&seed) % 100);
            if (roll < grow || !model.count) {
                size_t at = model.count ? next_random(&seed) % (model.count + 1) : 0;
                ModelItem item = {(double)(next_random(&seed) % 100000) - 50000, "", next_random(&seed) % 11};
                for (size_t k = 0; k < item.size; ++k) item.text[k] = (char)('a' + next_random(&seed) % 26);
                PureCell cells[2] = {{.num = item.key}, {.text = (unsigned char *)item.text, .size = item.size}};
                CHECK(pure_list_insert(list, at, cells, &undo) == PURE_OK);
                model_insert(&model, at, item);
            } else if (roll < grow + 25) {
                size_t at = next_random(&seed) % model.count;
                CHECK(pure_list_remove(list, at, &undo) == PURE_OK);
                memmove(model.items + at, model.items + at + 1, (model.count - at - 1) * sizeof(*model.items));
                --model.count;
            } else {
                size_t at = next_random(&seed) % model.count;
                if (next_random(&seed) % 2) {
                    double key = (double)(next_random(&seed) % 1000);
                    CHECK(pure_list_set(list, at, 0, (PureCell){.num = key}, &undo) == PURE_OK);
                    model.items[at].key = key;
                } else {
                    ModelItem *item = &model.items[at];
                    item->size = next_random(&seed) % 11;
                    for (size_t k = 0; k < item->size; ++k) item->text[k] = (char)('A' + next_random(&seed) % 26);
                    CHECK(pure_list_set(list, at, 1, (PureCell){.text = (unsigned char *)item->text, .size = item->size}, &undo) == PURE_OK);
                }
            }
        }
        if (next_random(&seed) % 5 == 0) { /* undone: the list and its journal as before */
            pure_undo_rollback(&undo, mark);
            pure_list_forget(list, journal);
            model_copy(&model, &before);
            ++undone;
        } else
            pure_undo_commit(&undo);
        ++events;
        largest = model.count > largest ? model.count : largest;
        if (event % 7 == 0) CHECK(same_items(list, &model));
        /* A save every few events: its records after the stored ones, or now
         * and then the items stored whole with none after them. */
        if (event % 3 == 0) {
            PureSpan changes = pure_list_changes(list);
            if (event % 240 == 0) {
                stored.size = 0;
                Grown whole = {malloc(pure_list_size(list) + 1), 0, 0};
                CHECK(whole.bytes);
                pure_list_encode(list, whole.bytes);
                collect_bytes(&stored, (PureSpan){whole.bytes, pure_list_size(list)});
                free(whole.bytes);
                stored_items = pure_list_size(list);
                ++rewrites;
            } else
                collect_bytes(&stored, changes);
            pure_list_forget(list, 0);
            ++saves;
        }
        if (event % 51 == 0) { /* just saved: what a restart finds is the list as it is */
            PureList *restarted = stored_list(&stored, stored_items);
            CHECK(restarted && same_items(restarted, &model));
            pure_list_free(restarted);
            CHECK(stored_as(list, &model));
        }
        /* A share now and then, as a snapshot takes one; the oldest goes with
         * the items and nodes retired while it was the newest. */
        if (event % 40 == 20) {
            if (held == SHARES) {
                CHECK(same_items(copies[first], &shared[first]));
                pure_list_free(copies[first]);
                pure_retired_reuse(&retired[first], &list, 1);
                first = (first + 1) % SHARES;
                --held;
            }
            unsigned slot = (first + held) % SHARES;
            CHECK((copies[slot] = pure_list_share(list, &retired[slot], ++epoch)));
            model_copy(&shared[slot], &model);
            ++held;
        }
        if (event % 25 == 0)
            for (unsigned i = 0; i < held; ++i) CHECK(same_items(copies[(first + i) % SHARES], &shared[(first + i) % SHARES]));
    }
    CHECK(same_items(list, &model) && stored_as(list, &model) && largest > 2000 && undone > 500 && rewrites > 20);
    /* Damaged records are refused: cut short, past the end, of no kind or no field. */
    collect_bytes(&stored, pure_list_changes(list));
    pure_list_forget(list, 0);
    PureList *whole = stored_list(&stored, stored_items);
    CHECK(whole && same_items(whole, &model));
    pure_list_free(whole);
    size_t length = stored.size;
    static const unsigned char damaged[][8] = {
        {1, 0xff, 0xff, 0xff, 0}, {2, 0xff, 0xff, 0xff, 0}, {9, 0, 0, 0, 0}, {3, 0, 0, 0, 0, 7}, {3, 0, 0, 0, 0, 0, 1}};
    static const size_t damaged_size[] = {5, 5, 5, 6, 7};
    for (unsigned d = 0; d < 5; ++d) {
        stored.size = length;
        collect_bytes(&stored, (PureSpan){damaged[d], damaged_size[d]});
        CHECK(!stored_list(&stored, stored_items));
    }
    CHECK(length > stored_items);
    stored.size = length - 1; /* the last record cut short */
    CHECK(!stored_list(&stored, stored_items));
    free(stored.bytes);
    /* Compact records keep every number bit for bit: integers, -0, the edges of 2^53, the rest. */
    static const uint8_t number_field[1] = {0};
    static const double edges[] = {0.0, -0.0, 1, -1, 127, -128, 9007199254740991.0, -9007199254740991.0,
                                   9007199254740992.0, -9007199254740994.0, 1e300, -1e-300, 0.1, 3.5e15 + 0.5, 4.9e-324};
    PureList *numbers = pure_list_new(1, number_field), *read_back = pure_list_new(1, number_field);
    CHECK(numbers && read_back);
    pure_list_journal(numbers, true);
    for (size_t k = 0; k < sizeof(edges) / sizeof(*edges); ++k) {
        CHECK(pure_list_insert(numbers, k, (PureCell[]){{.num = edges[k]}}, NULL) == PURE_OK);
        CHECK(pure_list_set(numbers, k, 0, (PureCell){.num = edges[k]}, NULL) == PURE_OK);
    }
    PureSpan edge_records = pure_list_changes(numbers);
    CHECK(edge_records.size < sizeof(edges) / sizeof(*edges) * 2 * 14 && pure_list_replay(read_back, edge_records.data, edge_records.size) == PURE_OK);
    for (size_t k = 0; k < sizeof(edges) / sizeof(*edges); ++k) {
        double got = pure_list_get(read_back, k, 0).num;
        CHECK(!memcmp(&got, &edges[k], sizeof(got)));
    }
    pure_list_free(numbers);
    pure_list_free(read_back);
    printf("{\"journal_saves\":%zu,\"journal_rewrites\":%zu}\n", saves, rewrites);
    for (; held; --held, first = (first + 1) % SHARES) {
        CHECK(same_items(copies[first], &shared[first]));
        pure_list_free(copies[first]);
        pure_retired_free(&retired[first]);
    }
    pure_list_free(list);
    pure_undo_free(&undo);

    /* A keyed list: items kept in key order, found by key; from halfway on
     * keyed, so found through each node's guide (made for the items there
     * then, beside a share that keeps reading its own). */
    PureList *keyed = pure_list_new(2, text_fields);
    PureRetired keyed_retired = {0};
    Model keys = {0};
    CHECK(keyed);
    for (unsigned round = 0; round < 40000; ++round) {
        if (round == 20000) {
            PureList *copy = pure_list_share(keyed, &keyed_retired, 1);
            CHECK(copy && pure_list_keyed(keyed, 0) == PURE_OK && same_items(copy, &keys) && same_items(keyed, &keys));
            pure_list_free(copy);
        }
        double key = (double)(next_random(&seed) % 20000);
        bool found;
        size_t at = pure_list_seek(keyed, 0, (PureCell){.num = key}, &found);
        size_t low = 0, high = keys.count;
        while (low < high) {
            size_t middle = (low + high) / 2;
            if (keys.items[middle].key < key) low = middle + 1;
            else high = middle;
        }
        CHECK(at == low && found == (low < keys.count && keys.items[low].key == key));
        if (!found) {
            PureCell cells[2] = {{.num = key}, {.text = (unsigned char *)"k", .size = 1}};
            if (round % 7 == 0) { /* an insert undone first: the guides as they were */
                PureUndo undone_insert = {0};
                CHECK(pure_list_insert(keyed, at, cells, &undone_insert) == PURE_OK);
                pure_undo_rollback(&undone_insert, 0);
                pure_undo_free(&undone_insert);
            }
            CHECK(pure_list_insert(keyed, at, cells, NULL) == PURE_OK);
            model_insert(&keys, at, (ModelItem){key, "k", 1});
        } else if (next_random(&seed) % 3 == 0) {
            PureUndo removal = {0};
            CHECK(pure_list_remove(keyed, at, &removal) == PURE_OK);
            pure_undo_commit(&removal);
            pure_undo_free(&removal);
            memmove(keys.items + at, keys.items + at + 1, (keys.count - at - 1) * sizeof(*keys.items));
            --keys.count;
        }
    }
    CHECK(same_items(keyed, &keys) && keys.count > 5000 && pure_list_size(keyed) == keys.count * 13);
    pure_list_free(keyed);
    pure_retired_free(&keyed_retired);
    free(model.items);
    free(before.items);
    free(keys.items);
    for (unsigned i = 0; i < SHARES; ++i) free(shared[i].items);
    for (unsigned i = 0; i <= SHARES; ++i) pure_retired_free(&retired[i]);
    printf("{\"list_tree_events\":%zu,\"undone\":%zu,\"largest\":%zu,\"keyed_items\":%zu}\n", events, undone,
           largest, keys.count);
}

static void lists(void) {
    static const S nums[] = {{"made", 0}};
    static const T states[] = {{"tasks", 3, 0}, {"at", 4, 1}};
    static const RF task_fields[] = {{"title", 1, 0}, {"done", 2, 0}};
    static const R records[] = {{"task", task_fields, 2}};
    static const char *const constants[] = {"", "new", "x"};
    static const I inputs[] = {
        {"typed", NULL, TEXT_INPUT, NULL, 0}, KEY("ins", "KeyI"), KEY("del", "Delete"),
        KEY("up", "ArrowUp"), KEY("down", "ArrowDown"), KEY("toggle", "Space"), KEY("rename", "KeyR"), KEY("bad", "KeyB"),
    };
    static const O code[] = {
        {TI, 0, 0}, {C, 1, 0}, {LA, 1, 0}, {XE, 1, 0}, {XS, 2, 0}, {L, 0, 0}, {C, 0, 1}, {ADD, 0, 0}, {ST, 0, 0}, /* 0 typed */
        {XL, 2, 0}, {TK, 1, 0}, {C, 1, 0}, {LI, 1, 0},                                           /* 9 ins */
        {XL, 2, 0}, {LR, 1, 0},                                                                  /* 13 del */
        {XL, 2, 0}, {C, 0, -1}, {XM, 1, 0}, {XS, 2, 0},                                          /* 15 up */
        {XL, 2, 0}, {C, 0, 1}, {XM, 1, 0}, {XS, 2, 0},                                           /* 19 down */
        {XL, 2, 0}, {XL, 2, 0}, {IG, DONE, 0}, {NOT, 0, 0}, {IS, DONE, 0},                         /* 23 toggle */
        {XL, 2, 0}, {XL, 2, 0}, {IG, TITLE, 0}, {TK, 2, 0}, {CAT, 0, 0}, {IS, TITLE, 0},           /* 28 rename */
        {XL, 2, 0}, {TK, 2, 0}, {IS, TITLE, 0}, {XL, 2, 0}, {LR, 1, 0}, {TK, 1, 0}, {C, 1, 0},     /* 34 bad */
        {LA, 1, 0}, {C, 0, 1}, {C, 0, 0}, {DIV, 0, 0}, {ST, 0, 0},
        {LC, 1, 0},                                                                              /* 46 view count */
        {XL, 2, 0}, {XN, 1, 0},                                                                  /* 47 view position */
        {XL, 2, 0}, {IG, DONE, 0},                                                               /* 49 view done */
        {XL, 2, 0}, {IG, TITLE, 0},                                                              /* 51 view current */
    };
    static const H handlers[] = {{1, 0, 9}, {3, 9, 4}, {5, 13, 2}, {7, 15, 4}, {9, 19, 4}, {11, 23, 5},
                                 {13, 28, 6}, {15, 34, 12}};
    static const V number_views[] = {{"count", 46, 1}, {"position", 47, 2}, {"done", 49, 2 | BOOL_VIEW}};
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
    CHECK(type_in(&app, 0, (PureSpan){(const unsigned char *)"a", 1}) == PURE_OK);
    CHECK(type_in(&app, 0, (PureSpan){(const unsigned char *)"b", 1}) == PURE_OK);
    CHECK(pure_list_count(app.lists[1]) == 2 && app.values[2].num == 1 && app.values[0].num == 2);
    CHECK(press(&app, 3) == PURE_OK && app.values[2].num == 0);
    CHECK(press(&app, 3) == PURE_OK && app.values[2].num == 0);   /* stops at the first */
    /* Inserting before the current item: the index follows it. */
    CHECK(press(&app, 1) == PURE_OK && app.values[2].num == 1);
    CHECK(!strcmp(item_title(&app, 1, 0), "new") && !strcmp(view_text(&app, 3), "a"));
    CHECK(press(&app, 5) == PURE_OK && number_view(&app, 2) == 1);
    CHECK(press(&app, 6) == PURE_OK && !strcmp(view_text(&app, 3), "ax"));
    CHECK(press(&app, 4) == PURE_OK && app.values[2].num == 2);
    CHECK(press(&app, 4) == PURE_OK && app.values[2].num == 2);   /* stops at the last */
    /* Removing the last item: the index moves to the new last. */
    CHECK(press(&app, 2) == PURE_OK && app.values[2].num == 1 && pure_list_count(app.lists[1]) == 2);
    CHECK(press(&app, 3) == PURE_OK && app.values[2].num == 0);
    /* Removing the current item: the index names the next. */
    CHECK(press(&app, 2) == PURE_OK && app.values[2].num == 0 && !strcmp(view_text(&app, 3), "ax"));
    /* A failing event undoes a set, a remove and an append. */
    CHECK(press(&app, 7) == PURE_INVALID && app.fault == PURE_FAULT_DIVIDE);
    CHECK(pure_list_count(app.lists[1]) == 1 && !strcmp(item_title(&app, 1, 0), "ax") &&
          pure_list_get(app.lists[1], 0, 1).num == 1 && app.values[2].num == 0 && app.values[0].num == 2);
    CHECK(press(&app, 2) == PURE_OK && pure_list_count(app.lists[1]) == 0 && app.values[2].num == 0);
    CHECK(press(&app, 2) == PURE_NOT_FOUND && app.fault == PURE_FAULT_NOT_FOUND);
    CHECK(press(&app, 5) == PURE_NOT_FOUND);
    CHECK(press(&app, 1) == PURE_OK && pure_list_count(app.lists[1]) == 1 && app.values[2].num == 0);
    for (int i = 0; i < 3; ++i) CHECK(type_in(&app, 0, (PureSpan){(const unsigned char *)"\xea\xb0\x80", 3}) == PURE_OK);
    CHECK(press(&app, 3) == PURE_OK && app.values[2].num == 2);
    CHECK(press(&app, 5) == PURE_OK);

    /* Replacement keeps the list by name. The same record keeps the list
     * itself; a changed record keeps each field by name and kind and gives new
     * fields their default. */
    static const S no_nums[] = {{"made", 0}};
    Spec same = {no_nums, 1, inputs, 0, number_views, 0, handlers, 0, code, 0, NULL, 0, states, 2, constants, 3,
                 text_views, 0, records, 1, NULL, 0};
    CHECK(load(same, &module_b) == PURE_OK);
    const PureList *before = app.lists[1];
    CHECK(pure_app_replace(&app, &module_b) == PURE_OK && app.lists[1] == before && app.values[2].num == 2);
    static const RF changed_fields[] = {{"done", 0, 0}, {"note", 1, 1}, {"title", 0, 7}, {"rank", 0, 5}};
    static const R changed[] = {{"task", changed_fields, 4}};
    Spec next = same;
    next.r = changed;
    CHECK(load(next, &module_a) == PURE_OK);
    CHECK(pure_app_replace(&app, &module_a) == PURE_OK && pure_list_count(app.lists[1]) == 4 && app.values[2].num == 2);
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
    CHECK(load(bad, &module_b) == PURE_INVALID && strstr(error, "view changes a state")); /* a body changes its event's lists alone */
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
            if (module_b.input[input].binding == PURE_BIND_TEXT) {
                const char *word = words[next_random(&state) % LEN(words)];
                type_in(&app, input, (PureSpan){(const unsigned char *)word, strlen(word)});
            } else if (module_b.input[input].binding == PURE_BIND_PRESS) press(&app, input);
            else frame(&app);
            for (uint32_t v = 0; v < module_b.views; ++v) {
                double value;
                if (module_b.view_type[v] == PURE_VIEW_TEXT) pure_app_view_text(&app, v, ignore_span, NULL);
                else pure_app_view(&app, v, &value);
            }
            for (uint32_t s = 0; s < module_b.states; ++s) {
                if (module_b.state_type[s] == PURE_STATE_INDEX) {
                    size_t count = pure_list_count(app.lists[module_b.state_ref[s]]);
                    CHECK(app.values[s].num == floor(app.values[s].num) && app.values[s].num >= 0 &&
                          app.values[s].num <= (count ? (double)(count - 1) : 0));
                } else if (module_b.state_type[s] == PURE_STATE_LIST) {
                    const PureRecord *record = &module_b.record[module_b.state_ref[s]];
                    for (size_t at = 0; at < pure_list_count(app.lists[s]); ++at)
                        for (unsigned f = 0; f < record->count; ++f) {
                            PureCell cell = pure_list_get(app.lists[s], at, f);
                            if (module_b.record_field_type[record->first + f] == PURE_RECORD_TEXT)
                                CHECK(pure_utf8_valid((PureSpan){cell.text, cell.size}));
                            else CHECK(isfinite(cell.num));
                        }
                } else if (module_b.state_type[s] == PURE_STATE_NUM) CHECK(isfinite(app.values[s].num));
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
    static const RF task_fields[] = {{"title", 1, 0}, {"done", 2, 0}};
    static const R records[] = {{"task", task_fields, 2}};
    static const char *const constants[] = {"", "x", " ", "empty"};
    static const I inputs[] = {{"typed", NULL, TEXT_INPUT, NULL, 0}, KEY("toggle", "Space")};
    static const O code[] = {
        {TI, 0, 0}, {C, 1, 0}, {LA, 1, 0}, {XE, 1, 0}, {XS, 2, 0},                          /* 0 typed */
        {XL, 2, 0}, {XL, 2, 0}, {IG, DONE, 0}, {NOT, 0, 0}, {IS, DONE, 0},                     /* 5 toggle */
        {LC, 1, 0}, {C, 0, 0}, {GT, 0, 0}, {JZ, 17, 0}, {XL, 2, 0}, {IG, TITLE, 0}, {JMP, 18, 0},
        {TK, 3, 0},                                                                          /* 10 current */
        {ROW, 1, 0}, {IG, TITLE, 0},                                                         /* 18 title */
        {ROW, 1, 0}, {IG, DONE, 0}, {JZ, 25, 0}, {TK, 1, 0}, {JMP, 26, 0}, {TK, 2, 0},         /* 20 mark */
        {ROW, 1, 0}, {XN, 1, 0},                                                             /* 26 pos */
        {ROW, 1, 0}, {XN, 1, 0}, {XL, 2, 0}, {XN, 1, 0}, {EQ, 0, 0},                           /* 28 here */
    };
    static const H handlers[] = {{1, 0, 5}, {3, 5, 5}};
    static const V text_views[] = {{"current", 10, 8}};
    static const W table[] = {{"board", 2, 0, 1, 0}, {"title", 1, 2, 18, 2}, {"mark", 1, 2, 20, 6},
                              {"pos", 0, 2, 26, 2}, {"here", 3, 2, 28, 5}};
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
    CHECK(type_in(&app, 0, (PureSpan){(const unsigned char *)"a", 1}) == PURE_OK);
    CHECK(type_in(&app, 0, (PureSpan){(const unsigned char *)"\xeb\xb9\xb5", 3}) == PURE_OK);
    CHECK(press(&app, 1) == PURE_OK);
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
    CHECK(load(bad, &module_b) == PURE_INVALID && strstr(error, "list view's list"));
    static const O other_row[] = {{ROW, 2, 0}, {XN, 2, 0}};
    static const W other_row_column[] = {{"board", 2, 0, 1, 0}, {"pos", 0, 1, 0, 2}};
    bad = spec; bad.o = other_row; bad.no = 2; bad.h = NULL; bad.nh = 0; bad.tv = NULL; bad.ntv = 0;
    bad.w = other_row_column; bad.nw = 2;
    CHECK(load(bad, &module_b) == PURE_INVALID && strstr(error, "no such state"));
    static const O typed_join[] = {{C, 1, 1}, {JZ, 4, 0}, {TK, 0, 0}, {JMP, 5, 0}, {C, 0, 1}};
    static const V mixed[] = {{"current", 0, 5}};
    bad = spec; bad.o = typed_join; bad.no = 5; bad.h = NULL; bad.nh = 0; bad.tv = mixed; bad.w = NULL; bad.nw = 0;
    CHECK(load(bad, &module_b) == PURE_INVALID && strstr(error, "join with different stacks"));
    static const O truth_join[] = {{C, 1, 1}, {JZ, 4, 0}, {C, 1, 1}, {JMP, 5, 0}, {C, 0, 1}}; /* a bool is not a number */
    static const V truth_view[] = {{"v", 0, 5}};
    bad.o = truth_join; bad.tv = NULL; bad.ntv = 0; bad.v = truth_view; bad.nv = 1;
    CHECK(load(bad, &module_b) == PURE_INVALID && strstr(error, "join with different stacks"));
    /* At most 8 joins with a value may be ahead at once. */
    for (unsigned open = 8; open <= 9; ++open) {
        static O nested[1 + 2 * 9 + 9 + 1];
        unsigned n = 0;
        nested[n++] = (O){C, 0, 1};
        for (unsigned k = 0; k < open; ++k) { nested[n++] = (O){C, 1, 0}; nested[n++] = (O){JZ, 1 + 2 * open + k, 0}; }
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
            if (module_b.input[input].binding == PURE_BIND_TEXT) type_in(&app, input, (PureSpan){(const unsigned char *)"t", 1});
            else if (module_b.input[input].binding == PURE_BIND_PRESS) press(&app, input);
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
        } else CHECK(!memcmp(&a->values[i].num, &b->values[i].num, sizeof(double)));
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
        if (module_a.input[input].binding == PURE_BIND_TEXT) {
            const char *word = words[next_random(&seed) % word_count];
            PureSpan text = {(const unsigned char *)word, strlen(word)};
            a = type_in(&memory, input, text);
            b = type_in(&stored, input, text);
        } else {
            a = press(&memory, input);
            b = press(&stored, input);
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
    unsigned n = (unsigned)app->values[0].num;
    CHECK(app->values[0].num == n && pure_draft_size(app->texts[NOTE]) == n && app->values[2].num == n);
    CHECK(pure_list_count(app->lists[TASKS]) == n && app->values[4].num == (n ? n - 1 : 0));
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
    CHECK(press(&app, 0) == PURE_OK && consistent(&app) == 1);
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
            press(&doomed, 0);
            _exit(0);
        }
        int status;
        CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 77);
        if (stage == PURE_BEFORE_DIR_SYNC) ++expected; /* the head was written */
        CHECK(pure_app_start(&app, &module_a, pure_tree_data(path)) == PURE_OK && consistent(&app) == expected);
        pure_app_stop(&app);
    }
    /* The list's change goes to the log before the tree commits: dying before
     * or after writing it there, the save's entry does not count (the tree
     * has not its number), and all three are as before. */
    for (int stage = PURE_LOG_BEFORE_WRITE; stage <= PURE_LOG_BEFORE_SYNC; ++stage) {
        pid_t child = fork();
        CHECK(child >= 0);
        if (!child) {
            PureApp doomed;
            if (pure_app_start(&doomed, &module_a, pure_tree_data(path)) != PURE_OK) _exit(1);
            crash = 1;
            fail_stage = stage;
            press(&doomed, 0);
            _exit(0);
        }
        int status;
        CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 77);
        CHECK(pure_app_start(&app, &module_a, pure_tree_data(path)) == PURE_OK && consistent(&app) == expected);
        pure_app_stop(&app);
    }
    /* A failed commit leaves the app and the store as they were; after an
     * uncertain one the app no longer matches the store and its events fail
     * until it starts again, from what the store has. */
    CHECK(pure_app_start(&app, &module_a, pure_tree_data(path)) == PURE_OK);
    for (fail_stage = PURE_AFTER_HEADER; fail_stage <= PURE_BEFORE_RENAME; ++fail_stage) {
        CHECK(press(&app, 0) == PURE_STORE_ERROR && consistent(&app) == expected);
    }
    fail_stage = PURE_BEFORE_DIR_SYNC;
    CHECK(press(&app, 0) == PURE_UNCERTAIN && consistent(&app) == expected);
    fail_stage = -1;
    CHECK(press(&app, 0) == PURE_CONFLICT && consistent(&app) == expected);
    pure_app_stop(&app);
    CHECK(pure_app_start(&app, &module_a, pure_tree_data(path)) == PURE_OK && consistent(&app) == expected + 1);
    CHECK(press(&app, 0) == PURE_OK && consistent(&app) == expected + 2);
    /* The log refusing its entry fails the save and changes nothing; the log
     * failing its sync leaves it uncertain: no save follows until the app
     * starts again, and then the entry does not count (the tree has not its
     * number). */
    fail_stage = PURE_LOG_BEFORE_WRITE;
    CHECK(press(&app, 0) == PURE_STORE_ERROR && consistent(&app) == expected + 2);
    fail_stage = PURE_LOG_BEFORE_SYNC;
    CHECK(press(&app, 0) == PURE_UNCERTAIN && consistent(&app) == expected + 2);
    fail_stage = -1;
    CHECK(press(&app, 0) == PURE_CONFLICT);
    pure_app_stop(&app);
    CHECK(pure_app_start(&app, &module_a, pure_tree_data(path)) == PURE_OK && consistent(&app) == expected + 2);
    CHECK(press(&app, 0) == PURE_OK && consistent(&app) == expected + 3);
    pure_app_stop(&app);
    CHECK(pure_app_start(&app, &module_a, pure_tree_data(path)) == PURE_OK && consistent(&app) == expected + 3);
    pure_app_stop(&app);
    remove_store(path);
}

/* A save that changes numbers alone (no list, so no log entry) and whose
 * commit may yet be stored leaves the app uncertain all the same: the next
 * event, which changes a list alone, is refused, and starting again finds
 * what the store has (review 004, F2). */
static void uncertain_numbers(void) {
    char *path = new_store();
    static const S nums[] = {{"n", 0}};
    static const T states[] = {{"rows", 3, 0}};
    static const RF fields[] = {{"value", 0, 0}};
    static const R records[] = {{"row", fields, 1}};
    static const I inputs[] = {KEY("inc", "KeyI"), KEY("add", "KeyA")};
    static const O code[] = {
        {L, 0, 0}, {C, 0, 1}, {ADD, 0, 0}, {ST, 0, 0}, /* n = n + 1 */
        {L, 0, 0}, {LA, 1, 0},                         /* rows.append(row { n }) */
    };
    static const H handlers[] = {{1, 0, 4}, {3, 4, 2}};
    Spec spec = {nums, 1, inputs, 2, NULL, 0, handlers, 2, code, LEN(code), NULL, 0, states, 1, NULL, 0, NULL, 0,
                 records, 1, NULL, 0};
    CHECK(load(spec, &module_a) == PURE_OK);
    PureApp app;
    CHECK(pure_app_start(&app, &module_a, pure_tree_data(path)) == PURE_OK);
    fail_stage = PURE_BEFORE_DIR_SYNC;
    CHECK(press(&app, 0) == PURE_UNCERTAIN && app.uncertain && app.values[0].num == 0);
    fail_stage = -1;
    CHECK(press(&app, 1) == PURE_CONFLICT && !pure_list_count(app.lists[1]));
    pure_app_stop(&app);
    CHECK(pure_app_start(&app, &module_a, pure_tree_data(path)) == PURE_OK && app.values[0].num == 1 &&
          !pure_list_count(app.lists[1]));
    CHECK(press(&app, 1) == PURE_OK && pure_list_get(app.lists[1], 0, 0).num == 1);
    pure_app_stop(&app);
    CHECK(pure_app_start(&app, &module_a, pure_tree_data(path)) == PURE_OK && app.values[0].num == 1 &&
          pure_list_count(app.lists[1]) == 1 && pure_list_get(app.lists[1], 0, 0).num == 1);
    pure_app_stop(&app);
    remove_store(path);
}

/* Frames a host defers (027's 4d: a frame is a message, saved as each is unless deferred) are stored by a save or by
 * the next event it does not defer; a stop alone leaves them out. */
static void stored_frames(void) {
    char *path = new_store();
    static const O code[] = {{L, 0, 0}, {C, 0, 1}, {ADD, 0, 0}, {ST, 0, 0}};
    static const H handlers[] = {{0, 0, 4}, {1, 0, 0}};
    Spec spec = {crash_nums, 1, crash_inputs, 1, NULL, 0, handlers, 2, code, 4, NULL, 0, NULL, 0, NULL, 0, NULL, 0,
                 NULL, 0, NULL, 0};
    CHECK(load(spec, &module_a) == PURE_OK);
    PureApp app;
    CHECK(pure_app_start(&app, &module_a, pure_tree_data(path)) == PURE_OK && pure_app_defer(&app, true));
    for (int i = 0; i < 5; ++i) CHECK(frame(&app) == PURE_OK);
    pure_app_stop(&app);
    CHECK(pure_app_start(&app, &module_a, pure_tree_data(path)) == PURE_OK && app.values[0].num == 0);
    CHECK(pure_app_defer(&app, true));
    for (int i = 0; i < 3; ++i) CHECK(frame(&app) == PURE_OK);
    CHECK(pure_app_save(&app) == PURE_OK);
    pure_app_stop(&app);
    CHECK(pure_app_start(&app, &module_a, pure_tree_data(path)) == PURE_OK && app.values[0].num == 3);
    CHECK(pure_app_defer(&app, true));
    for (int i = 0; i < 2; ++i) CHECK(frame(&app) == PURE_OK);
    CHECK(pure_app_defer(&app, false) && press(&app, 0) == PURE_OK);
    pure_app_stop(&app);
    CHECK(pure_app_start(&app, &module_a, pure_tree_data(path)) == PURE_OK && app.values[0].num == 5);
    pure_app_stop(&app);
    remove_store(path);
}

/* A message's changes are stored with its event, and one that fails stores nothing. */
static void stored_messages(void) {
    char *path = new_store();
    CHECK(load(message_spec, &module_a) == PURE_OK);
    PureApp app;
    unsigned char buffer[64];
    CHECK(pure_app_start(&app, &module_a, pure_tree_data(path)) == PURE_OK);
    CHECK(pure_app_message(&app, 1, edit_payload(buffer, 2, 3, "hello")) == PURE_OK && replied(&app, "hello"));
    CHECK(pure_app_message(&app, 2, edit_payload(buffer, 0, 0, "-world, long")) == PURE_LIMIT && app.values[0].num == 1);
    CHECK(pure_app_message(&app, 2, edit_payload(buffer, 0, 0, "!")) == PURE_OK && app.values[0].num == 2);
    pure_app_stop(&app);
    CHECK(pure_app_start(&app, &module_a, pure_tree_data(path)) == PURE_OK && app.values[0].num == 2 && app.values[1].num == 5);
    pure_app_stop(&app);
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
    CHECK(press(&app, 0) == PURE_OK && press(&app, 0) == PURE_OK);
    pure_app_stop(&app);
    for (int round = 0; round < 2; ++round) { /* migrated, then stored as migrated */
        CHECK(pure_app_start(&app, &module_b, pure_tree_data(path)) == PURE_OK && pure_list_count(app.lists[TASKS]) == 2);
        PureCell note = pure_list_get(app.lists[TASKS], 1, 1), title = pure_list_get(app.lists[TASKS], 1, 2);
        CHECK(pure_list_get(app.lists[TASKS], 1, 0).num == 2 && pure_list_get(app.lists[TASKS], 1, 3).num == 5);
        CHECK(note.size == 1 && note.text[0] == 'x' && title.size == 1 && title.text[0] == 'x');
        CHECK(app.values[0].num == 2 && app.values[4].num == 1 && pure_draft_size(app.texts[NOTE]) == 2);
        pure_app_stop(&app);
    }
    /* Back to the first record by replacing the module; the next save stores it. */
    CHECK(pure_app_start(&app, &module_b, pure_tree_data(path)) == PURE_OK);
    CHECK(pure_app_replace(&app, &module_a) == PURE_OK && press(&app, 0) == PURE_OK);
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
    CHECK(app.values[0].num == 9 && app.values[2].num == 0 && app.values[4].num == 0);
    pure_app_stop(&app);
    /* Damaged stored lists (their images) and texts are refused, not replaced. */
    char image[128];
    snprintf(image, sizeof(image), "%s/image-tasks", path);
    int fd = open(image, O_RDWR);
    unsigned char byte;
    CHECK(fd >= 0 && pread(fd, &byte, 1, 20) == 1);
    byte ^= 0x40;
    CHECK(pwrite(fd, &byte, 1, 20) == 1);
    CHECK(pure_app_start(&app, &module_a, pure_tree_data(path)) == PURE_STORE_ERROR);
    byte ^= 0x40;
    CHECK(pwrite(fd, &byte, 1, 20) == 1 && !close(fd));
    PureDraft *draft;
    CHECK(pure_state_open(path, "text-note", &draft) == PURE_OK);
    CHECK(pure_draft_splice(draft, 0, 0, (PureSpan){(const unsigned char *)"\xff", 1}) == PURE_OK);
    CHECK(pure_draft_commit(draft, &revision) == PURE_OK);
    CHECK(pure_app_start(&app, &module_a, pure_tree_data(path)) == PURE_STORE_ERROR);
    CHECK(pure_draft_splice(draft, 0, 1, (PureSpan){0}) == PURE_OK && pure_draft_commit(draft, &revision) == PURE_OK);
    pure_draft_close(draft);
    CHECK(pure_app_start(&app, &module_a, pure_tree_data(path)) == PURE_OK);
    CHECK(pure_list_count(app.lists[TASKS]) == 3 && pure_draft_size(app.texts[NOTE]) == 3 && app.values[0].num == 9);
    pure_app_stop(&app);
    pure_draft_close(values);
    remove_store(path);
}

extern unsigned long pure_save_test_checkpoints;

/* Transient states (018) are the app's while it runs, undone with a failed
 * event, never stored: a start begins them anew. Replacing the module moves
 * a state between stored and transient: one stored from then on is saved,
 * and one no longer stored is to the store as a dropped state. */
static void transient_states(void) {
    enum { N, T_, NOTE_, LOG, I_, KEEP, PICK, REQ = PURE_OP_REQUIRE, TSET_ = PURE_OP_TEXT_SET };
    static const S nums[] = {{"n", 0}};
    static const T a_states[] = {{"t", 0 | TR, 0}, {"note", 1 | TR, 1}, {"log", 3 | TR, 0}, {"i", 4 | TR, LOG},
                                 {"keep", 3, 0}, {"pick", 4 | TR, KEEP}};
    static const T b_states[] = {{"t", 0, 0}, {"note", 1, 1}, {"log", 3, 0}, {"i", 4 | TR, LOG},
                                 {"keep", 3 | TR, 0}, {"pick", 4 | TR, KEEP}};
    static const T stored_index[] = {{"t", 0 | TR, 0}, {"note", 1 | TR, 1}, {"log", 3 | TR, 0}, {"i", 4, LOG},
                                     {"keep", 3, 0}, {"pick", 4 | TR, KEEP}};
    static const RF fields[] = {{"value", 0, 0}};
    static const R records[] = {{"row", fields, 1}};
    static const char *const constants[] = {"", "x"};
    static const I inputs[] = {KEY("inc", "KeyI"), KEY("fail", "KeyF"), KEY("touch", "KeyT")};
    static const O code[] = {
        {L, N, 0}, {C, 0, 1}, {ADD, 0, 0}, {ST, N, 0},                 /* n = n + 1 */
        {L, T_, 0}, {C, 0, 1}, {ADD, 0, 0}, {ST, T_, 0},               /* t = t + 1 */
        {L, T_, 0}, {LA, LOG, 0}, {L, N, 0}, {LA, KEEP, 0},            /* log.append(row { t }), keep.append(row { n }) */
        {XE, LOG, 0}, {XS, I_, 0}, {XE, KEEP, 0}, {XS, PICK, 0},       /* i = log.last, pick = keep.last */
        {TL, NOTE_, 0}, {TK, 1, 0}, {CAT, 0, 0}, {TSET_, NOTE_, 0},     /* note = note + "x" */
        /* fail: the transient changes, then a require that fails */
        {L, T_, 0}, {C, 0, 1}, {ADD, 0, 0}, {ST, T_, 0}, {L, T_, 0}, {LA, LOG, 0},
        {TL, NOTE_, 0}, {TK, 1, 0}, {CAT, 0, 0}, {TSET_, NOTE_, 0}, {C, 1, 0}, {REQ, 0, 0},
        /* touch: only transient states */
        {L, T_, 0}, {C, 0, 1}, {ADD, 0, 0}, {ST, T_, 0}, {L, T_, 0}, {LA, LOG, 0},
    };
    static const H handlers[] = {{1, 0, 20}, {3, 20, 12}, {5, 32, 6}};
    Spec a = {nums, 1, inputs, 3, NULL, 0, handlers, 3, code, LEN(code), NULL, 0, a_states, 6, constants, 2, NULL, 0,
              records, 1, NULL, 0};
    Spec b = a, c = a;
    b.t = b_states;
    c.t = stored_index;
    PureModule refused = {0};
    CHECK(load(c, &refused) != PURE_OK && strstr(error, "stored position in a transient state"));
    CHECK(load(a, &module_a) == PURE_OK && load(b, &module_b) == PURE_OK);
    CHECK(pure_state_transient(&module_a, T_) && !pure_state_transient(&module_a, KEEP) && !pure_state_transient(&module_b, T_));
    char *path = new_store();
    PureApp app;
    CHECK(pure_app_start(&app, &module_a, pure_tree_data(path)) == PURE_OK);
    CHECK(press(&app, 0) == PURE_OK && press(&app, 0) == PURE_OK);
    CHECK(app.values[N].num == 2 && app.values[T_].num == 2 && pure_list_count(app.lists[LOG]) == 2 &&
          pure_list_count(app.lists[KEEP]) == 2 && app.values[I_].num == 1 && app.values[PICK].num == 1 &&
          pure_draft_size(app.texts[NOTE_]) == 3);
    /* A snapshot copies a transient text, and shares its lists as any other. */
    PureSnapshot *snapshot = pure_app_snapshot(&app);
    PureApp reader = {0};
    CHECK(snapshot);
    pure_app_read(&reader, snapshot);
    CHECK(pure_draft_size(reader.texts[NOTE_]) == 3 && pure_list_count(reader.lists[LOG]) == 2 && reader.values[T_].num == 2);
    pure_app_stop(&reader);
    pure_snapshot_release(snapshot);
    /* A failed event undoes transient states too. */
    CHECK(press(&app, 1) != PURE_OK && app.values[T_].num == 2 &&
          pure_list_count(app.lists[LOG]) == 2 && pure_draft_size(app.texts[NOTE_]) == 3);
    /* Changing only transient states stores nothing. */
    uint64_t seq = app.seq;
    for (int k = 0; k < 3; ++k) CHECK(press(&app, 2) == PURE_OK);
    CHECK(app.seq == seq && app.values[T_].num == 5 && pure_list_count(app.lists[LOG]) == 5);
    pure_app_stop(&app);
    CHECK(pure_app_start(&app, &module_a, pure_tree_data(path)) == PURE_OK);
    CHECK(app.values[N].num == 2 && pure_list_count(app.lists[KEEP]) == 2 && app.values[T_].num == 0 &&
          !pure_list_count(app.lists[LOG]) && app.values[I_].num == 0 && app.values[PICK].num == 0 &&
          pure_draft_size(app.texts[NOTE_]) == 1);
    /* Into b: t, note and log are stored from now on, keep is not. */
    CHECK(press(&app, 0) == PURE_OK && press(&app, 0) == PURE_OK);
    CHECK(pure_app_replace(&app, &module_b) == PURE_OK && app.values[T_].num == 2 && pure_list_count(app.lists[LOG]) == 2 &&
          pure_list_count(app.lists[KEEP]) == 4 && pure_draft_size(app.texts[NOTE_]) == 3);
    CHECK(press(&app, 0) == PURE_OK);
    pure_app_stop(&app);
    CHECK(pure_app_start(&app, &module_b, pure_tree_data(path)) == PURE_OK);
    CHECK(app.values[N].num == 5 && app.values[T_].num == 3 && pure_list_count(app.lists[LOG]) == 3 &&
          pure_list_get(app.lists[LOG], 2, 0).num == 3 && pure_draft_size(app.texts[NOTE_]) == 4 &&
          !pure_list_count(app.lists[KEEP]));
    pure_app_stop(&app);
    /* Back in a, keep is stored again: what the store has of it is what it held when it stopped being stored. */
    CHECK(pure_app_start(&app, &module_a, pure_tree_data(path)) == PURE_OK);
    CHECK(app.values[N].num == 5 && pure_list_count(app.lists[KEEP]) == 4 && app.values[T_].num == 0 &&
          !pure_list_count(app.lists[LOG]) && pure_draft_size(app.texts[NOTE_]) == 1);
    pure_app_stop(&app);
    remove_store(path);
}

static void stored_states(void) {
    static const char *const list_words[] = {"a", "\xea\xb0\x80", "", "\n", "\xf0\x9f\x98\x80"};
    static const char *const text_words[] = {"a", "\xea\xb0\x80", "\n", "\xf0\x9f\x98\x80", "xy\nz", ""};
    unsigned long checkpoints = pure_save_test_checkpoints;
    stored_equivalence(list_spec, list_words, LEN(list_words), 0x9e3779b9u);
    CHECK(pure_save_test_checkpoints > checkpoints + 5); /* images written again as the log grew */
    stored_equivalence(text_spec, text_words, LEN(text_words), 0x7f4a7c15u);
    stored_crashes();
    uncertain_numbers();
    stored_frames();
    stored_messages();
    stored_migration();
    transient_states();
}

/* ---- Deferred saving ---------------------------------------------------- */

typedef struct { uint32_t input; const char *word; } Step;

static PureResult apply_step(PureApp *app, Step step) {
    if (step.word) return type_in(app, step.input, (PureSpan){(const unsigned char *)step.word, strlen(step.word)});
    return press(app, step.input);
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
        if (module_a.input[next.input].binding == PURE_BIND_TEXT) next.word = words[next_random(&seed) % word_count];
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
    for (int i = 0; i < 5; ++i) CHECK(press(&app, 0) == PURE_OK);
    CHECK(consistent(&app) == 5 && app.events == 5);
    for (fail_stage = PURE_AFTER_HEADER; fail_stage <= PURE_BEFORE_RENAME; fail_stage += 2) {
        CHECK(pure_app_save(&app) == PURE_STORE_ERROR && consistent(&app) == 0 && app.events == 0);
        for (int i = 0; i < 5; ++i) CHECK(press(&app, 0) == PURE_OK);
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
            for (int i = 0; i < 7; ++i) if (press(&doomed, 0) != PURE_OK) _exit(2);
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

static void deferred(void) {
    static const char *const list_words[] = {"a", "\xea\xb0\x80", "", "\n", "\xf0\x9f\x98\x80"};
    static const char *const text_words[] = {"a", "\xea\xb0\x80", "\n", "\xf0\x9f\x98\x80", "xy\nz", ""};
    deferred_equivalence(list_spec, list_words, LEN(list_words), 0x2545f491u);
    deferred_equivalence(text_spec, text_words, LEN(text_words), 0x1b873593u);
    deferred_bursts();
}

/* Text inserted at a cursor goes before it; at a mark (a cursor that stays),
 * after it. Everywhere else, and when text is removed, both move alike. */
static void marks(void) {
    enum { MARK = 2 | PURE_CURSOR_STAYS << 8 };
    static const T textual[] = {{"note", 1, 0}, {"at", 2, 0}, {"mark", MARK, 0}};
    static const char *const constants[] = {"ab"};
    static const I inputs[] = {{"typed", NULL, TEXT_INPUT, NULL, 0}, KEY("keep", "KeyK"), KEY("back", "Backspace")};
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
    CHECK(type_in(&app, 0, (PureSpan){(const unsigned char *)"x", 1}) == PURE_OK);
    CHECK(!strcmp(state_text(&app, 0), "xab") && app.values[1].num == 1 && app.values[2].num == 0); /* the mark stayed */
    CHECK(press(&app, 1) == PURE_OK && app.values[2].num == 1);
    CHECK(type_in(&app, 0, (PureSpan){(const unsigned char *)"yz", 2}) == PURE_OK);
    CHECK(!strcmp(state_text(&app, 0), "xyzab") && app.values[1].num == 3 && app.values[2].num == 1);
    CHECK(press(&app, 2) == PURE_OK && app.values[1].num == 2 && app.values[2].num == 1);
    CHECK(press(&app, 2) == PURE_OK && app.values[1].num == 1 && app.values[2].num == 1); /* gathered */
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
static const RF fold_fields[] = {{"title", 1, 0}, {"done", 2, 0}, {"points", 0, 0}};
static const R fold_records[] = {{"task", fold_fields, 3}};
static const T fold_states[] = {{"tasks", 3, 0}, {"at", 4, 0}};
static const char *const fold_constants[] = {""};
static const I fold_inputs[] = {{"typed", NULL, TEXT_INPUT, NULL, 0}, KEY("toggle", "KeyT")};
enum { F_TITLE = 0, F_DONE = 1u << 16, F_POINTS = 2u << 16 };
static const O fold_code[] = {
    {TI, 0, 0}, {C, 1, 0}, {TI, 0, 0}, {CHR, 0, 0}, {LA, 0, 0}, {XE, 0, 0}, {XS, 1, 0},     /* 0 typed */
    {XL, 1, 0}, {XL, 1, 0}, {IG, F_DONE, 0}, {NOT, 0, 0}, {IS, F_DONE, 0},                 /* 7 toggle */
    {FOLD, FOLDING(PURE_FOLD_COUNT, 0, 3), 0}, {ROW, 0, 0}, {IG, F_DONE, 0}, {NOT, 0, 0},   /* 12 open */
    {FOLD, FOLDING(PURE_FOLD_SUM, 0, 3), 0}, {ROW, 0, 0}, {IG, F_POINTS, 0}, {C, 1, 1},     /* 16 points */
    {FOLD, FOLDING(PURE_FOLD_MAX, 0, 4), 0}, {ROW, 0, 0}, {IG, F_TITLE, 0}, {CHR, 0, 0},    /* 20 longest */
    {C, 1, 1},
    {FOLD, FOLDING(PURE_FOLD_SUM, 0, 4), 0}, {ROW, 0, 0}, {IG, F_POINTS, 0}, {ROW, 0, 0},   /* 25 done_points */
    {IG, F_DONE, 0},
    {FOLD, FOLDING(PURE_FOLD_MIN, 0, 5), 0}, {ROW, 0, 0}, {IG, F_POINTS, 0}, {ROW, 0, 0},   /* 30 fewest_open */
    {IG, F_DONE, 0}, {NOT, 0, 0},
    {C, 0, 10}, {FOLD, FOLDING(PURE_FOLD_COUNT, 0, 1), 0}, {C, 1, 1}, {ADD, 0, 0},         /* 36 count_plus */
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
        CHECK(type_in(&app, 0, (PureSpan){(const unsigned char *)words[i], strlen(words[i])}) == PURE_OK);
    CHECK(number_view(&app, 0) == 3 && number_view(&app, 1) == 6 && number_view(&app, 2) == 3);
    CHECK(number_view(&app, 3) == 0 && number_view(&app, 4) == 1 && number_view(&app, 5) == 13);
    CHECK(press(&app, 1) == PURE_OK);   /* the last task is done */
    CHECK(number_view(&app, 0) == 2 && number_view(&app, 3) == 2 && number_view(&app, 4) == 1);
    CHECK(number_view(&app, 6) == 0);                                 /* the first open one: an index */
    CHECK(place(&app, 1, 0) == PURE_OK && press(&app, 1) == PURE_OK);
    CHECK(number_view(&app, 6) == 1);
    CHECK(place(&app, 1, 1) == PURE_OK && press(&app, 1) == PURE_OK);
    CHECK(!pure_app_view(&app, 6, &value));
    CHECK(number_view(&app, 0) == 0 && number_view(&app, 3) == 6 && !pure_app_view(&app, 4, &value)); /* none open */
    pure_app_stop(&app);

    /* What a fold may not be. */
    static const O nested[] = {{FOLD, FOLDING(PURE_FOLD_COUNT, 0, 2), 0}, {FOLD, FOLDING(PURE_FOLD_COUNT, 0, 1), 0}, {C, 1, 1}};
    static const O into[] = {{C, 1, 1}, {JZ, 3, 0}, {FOLD, FOLDING(PURE_FOLD_COUNT, 0, 1), 0}, {C, 1, 1}};
    static const O effect[] = {{FOLD, FOLDING(PURE_FOLD_COUNT, 0, 3), 0}, {ROW, 0, 0}, {XS, 1, 0}, {C, 1, 1},
                               {C, 0, 0}, {GT, 0, 0}, {PURE_OP_REQUIRE, 0, 0}};
    static const O short_sum[] = {{FOLD, FOLDING(PURE_FOLD_SUM, 0, 1), 0}, {C, 1, 1}};
    static const O outside[] = {{FOLD, FOLDING(PURE_FOLD_COUNT, 0, 5), 0}, {C, 1, 1}};
    static const O not_list[] = {{FOLD, FOLDING(PURE_FOLD_COUNT, 1, 1), 0}, {C, 1, 1}};
    /* Whether an item counts is a bool, not a number; a sum's value is a number. */
    static const O count_number[] = {{FOLD, FOLDING(PURE_FOLD_COUNT, 0, 1), 0}, {C, 0, 1}};
    static const O sum_numbers[] = {{FOLD, FOLDING(PURE_FOLD_SUM, 0, 2), 0}, {C, 0, 1}, {C, 0, 1}};
    static const O sum_bools[] = {{FOLD, FOLDING(PURE_FOLD_SUM, 0, 2), 0}, {C, 1, 1}, {C, 1, 1}};
    static const O first_number[] = {{FOLD, FOLDING(PURE_FOLD_FIRST, 0, 1), 0}, {C, 0, 0}, {XN, 0, 0}};
    static const struct { const O *code; size_t count, length; bool handler; const char *reason; } refused[] = {
        {nested, LEN(nested), 3, false, "fold inside a fold"},
        {into, LEN(into), 4, false, "branch into a fold"},
        {effect, LEN(effect), 7, true, "fold with an effect"},
        {short_sum, LEN(short_sum), 2, false, "fold block must leave a number and a bool"},
        {outside, LEN(outside), 2, false, "fold block outside its block"},
        {not_list, LEN(not_list), 2, false, "no such state"},
        {count_number, LEN(count_number), 2, false, "fold block must leave one bool"},
        {sum_numbers, LEN(sum_numbers), 3, false, "fold block must leave a number and a bool"},
        {sum_bools, LEN(sum_bools), 3, false, "fold block must leave a number and a bool"},
        {first_number, LEN(first_number), 3, false, "fold block must leave one bool"},
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
    {TI, 0, 0}, {C, 1, 0}, {TI, 0, 0}, {CHR, 0, 0}, {LA, 0, 0}, {XE, 0, 0}, {XS, 1, 0},     /* 0 typed */
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
        CHECK(type_in(&app, 0, (PureSpan){(const unsigned char *)word, 1}) == PURE_OK);
    CHECK(pure_app_rows(&app, 0) == 3 && !strcmp(cell_title(&app, 1, 2), "c") && pure_app_cell(&app, 2, 2, &value) && value == 2);
    CHECK(press(&app, 1) == PURE_OK);   /* c is done: it leaves */
    CHECK(pure_app_rows(&app, 0) == 2 && !pure_app_cell(&app, 2, 2, &value) && !pure_app_item_row(&app, 0, 2, &at));
    CHECK(place(&app, 1, 0) == PURE_OK && press(&app, 1) == PURE_OK); /* a too */
    CHECK(pure_app_rows(&app, 0) == 1 && !strcmp(cell_title(&app, 1, 0), "b"));
    CHECK(pure_app_cell(&app, 2, 0, &value) && value == 1);      /* the item's position, not the row's */
    CHECK(pure_app_row_item(&app, 0, 0, &at) && at == 1 && !pure_app_row_item(&app, 0, 1, &at));
    CHECK(pure_app_item_row(&app, 0, 1, &at) && at == 0 && !pure_app_item_row(&app, 0, 0, &at));
    CHECK(press(&app, 1) == PURE_OK);   /* a comes back, before b */
    CHECK(pure_app_rows(&app, 0) == 2 && !strcmp(cell_title(&app, 1, 0), "a") && pure_app_item_row(&app, 0, 1, &at) && at == 1);
    CHECK(pure_app_replace(&app, &module_a) == PURE_OK && pure_app_rows(&app, 0) == 2);
    pure_app_stop(&app);

    /* What a filter may not be. */
    static const O effect[] = {{ROW, 0, 0}, {XS, 1, 0}, {C, 1, 1}};
    static const O text_filter[] = {{ROW, 0, 0}, {IG, F_TITLE, 0}};
    static const O number_filter[] = {{ROW, 0, 0}, {IG, F_POINTS, 0}};
    static const O folded[] = {{FOLD, FOLDING(PURE_FOLD_COUNT, 0, 1), 0}, {C, 1, 1}};
    static const W filtered_by[] = {{"open", W_OF | 1 << 16, 0, 0, 0}}, open3[] = {{"open", FILTERED, 0, 0, 3}},
                   open2[] = {{"open", FILTERED, 0, 0, 2}};
    static const W not_a_list[] = {{"open", 2 | 2 << 8, 0, 0, 3}};
    static const W flagged_column[] = {{"open", FILTERED, 0, 0, 3}, {"c", 0 | 1 << 8, 1, 0, 3}};
    static const struct { const O *code; size_t count; const W *views; size_t nviews; const char *reason; } refused[] = {
        {effect, LEN(effect), open3, 1, "filter with an effect"},
        {text_filter, LEN(text_filter), open2, 1, "filter must leave one bool"},
        {number_filter, LEN(number_filter), open2, 1, "filter must leave one bool"},
        {folded, LEN(folded), open2, 1, "fold inside a filter"},
        {effect, LEN(effect), filtered_by, 1, "list view's list"}, /* a computed list view needs its block */
        {effect, LEN(effect), not_a_list, 1, "list view's list"},  /* state 1 is an index */
        {effect, LEN(effect), flagged_column, 2, "view type"},     /* only a table has a list byte */
    };
    for (size_t i = 0; i < LEN(refused); ++i)
        if (load(filter_spec(refused[i].code, refused[i].count, refused[i].views, refused[i].nviews), &module_b) != PURE_INVALID ||
            !strstr(error, refused[i].reason)) {
            fprintf(stderr, "filter case %zu: expected '%s', got '%s'\n", i, refused[i].reason, error);
            abort();
        }
}

/* ---- Folds and filters a leaf at a time --------------------------------
 * Random well-typed folds (plain views, a table's columns reading their row,
 * a handler storing them), filters and lists: a module whose folds run a
 * leaf at a time answers bit for bit as the same module run item by item,
 * faults included (their kind and instruction). */
/* tree: no PICK or SLIDE (a shaped fold's block is a tree). A fold_list of
 * -1 makes code that reads no fold's item. */
typedef struct { O code[8192]; size_t count; uint32_t seed; int fold_list, outer; bool tree; } Gen;
enum { LANE_K = 0, LANE_L = 4, LANE_M = 5 };

static void put_op(Gen *g, uint8_t op, uint32_t arg, double value) {
    CHECK(g->count < LEN(g->code));
    g->code[g->count++] = (O){op, arg, value};
}

static unsigned roll(Gen *g, unsigned n) { return next_random(&g->seed) % n; }

static void gen_num(Gen *g, int depth);

/* An index of list: the fold's item, the column's row, a key found, an end, or one moved. */
static void gen_index(Gen *g, int list, int depth) {
    unsigned choice = roll(g, depth <= 0 ? 3 : 7);
    if (choice == 0 && list == g->fold_list) put_op(g, PURE_OP_ROW, (uint32_t)list, 0);
    else if (choice == 1 && list == LANE_L && g->outer >= 0) put_op(g, PURE_OP_OUTER_ROW, LANE_L, 0);
    else if (choice >= 3 && choice <= 4 && list == LANE_L) {
        gen_num(g, depth - 1);
        put_op(g, PURE_OP_LIST_FIND, LANE_L, 0);
    } else if (choice == 5) {
        gen_index(g, list, depth - 1);
        gen_num(g, depth - 1);
        put_op(g, PURE_OP_INDEX_MOVE, (uint32_t)list, 0);
    } else
        put_op(g, roll(g, 2) ? PURE_OP_INDEX_FIRST : PURE_OP_INDEX_LAST, (uint32_t)list, 0);
}

static void gen_bool(Gen *g, int depth);

/* An item's field: a number (L's key, a or b, M's x or y) or a bool (L's c, M's z). */
static void gen_field(Gen *g, int depth, bool flag) {
    int list = g->fold_list >= 0 && (depth <= 0 || roll(g, 3)) ? g->fold_list : roll(g, 2) ? LANE_L : LANE_M;
    gen_index(g, list, depth - 1);
    uint32_t field = flag ? (list == LANE_L ? 3 : 2) : roll(g, list == LANE_L ? 3 : 2);
    put_op(g, PURE_OP_ITEM_GET, (uint32_t)list | field << 16, 0);
}

static void gen_num(Gen *g, int depth) {
    static const double pool[] = {0, -0.0, 1, -1, 2, 0.5, 3, 7, 100, -3.5, 1e308, -1e308, 4, 10};
    static const uint8_t binary[] = {PURE_OP_ADD, PURE_OP_SUB, PURE_OP_MUL, PURE_OP_DIV, PURE_OP_MIN, PURE_OP_MAX, PURE_OP_MOD};
    static const uint8_t unary[] = {PURE_OP_NEG, PURE_OP_FLOOR};
    unsigned choice = roll(g, depth <= 0 ? 4 : 11);
    if (g->tree && (choice == 8 || choice == 9)) choice = 10;
    switch (choice) {
    case 0: put_op(g, C, 0, pool[roll(g, LEN(pool))]); return;
    case 1: put_op(g, L, LANE_K, 0); return;
    case 2: case 3: gen_field(g, depth, false); return;
    case 4: put_op(g, PURE_OP_LIST_COUNT, roll(g, 2) ? LANE_L : LANE_M, 0); return;
    case 5: {
        int list = roll(g, 2) ? LANE_L : LANE_M;
        gen_index(g, list, depth - 1);
        put_op(g, PURE_OP_INDEX_NUM, (uint32_t)list, 0);
        return;
    }
    case 6: gen_num(g, depth - 1); put_op(g, unary[roll(g, LEN(unary))], 0, 0); return;
    case 7: gen_num(g, depth - 1); gen_num(g, depth - 1); gen_num(g, depth - 1); put_op(g, CL, 0, 0); return;
    case 8: gen_num(g, depth - 1); put_op(g, PURE_OP_PICK, 0, 0); put_op(g, binary[roll(g, LEN(binary))], 0, 0); return;
    case 9: /* a value of either type dropped under a number */
        if (roll(g, 2)) gen_bool(g, depth - 1);
        else gen_num(g, depth - 1);
        gen_num(g, depth - 1);
        put_op(g, PURE_OP_SLIDE, 1, 0);
        return;
    default: gen_num(g, depth - 1); gen_num(g, depth - 1); put_op(g, binary[roll(g, LEN(binary))], 0, 0); return;
    }
}

/* A bool: a constant, an item's flag, a key looked up, numbers or bools
 * compared, or bools combined. */
static void gen_bool(Gen *g, int depth) {
    static const uint8_t compare[] = {PURE_OP_LT, PURE_OP_LE, PURE_OP_GT, PURE_OP_GE, PURE_OP_EQ, PURE_OP_NE};
    static const uint8_t logic[] = {PURE_OP_AND, PURE_OP_OR, PURE_OP_EQ, PURE_OP_NE};
    unsigned choice = roll(g, depth <= 0 ? 2 : 10);
    if (g->tree && (choice == 8 || choice == 9)) choice = 7;
    switch (choice) {
    case 0: put_op(g, C, 1, roll(g, 2)); return;
    case 1: gen_field(g, depth, true); return;
    case 2: gen_num(g, depth - 1); put_op(g, PURE_OP_LIST_HAS, LANE_L, 0); return;
    case 3: case 4: gen_num(g, depth - 1); gen_num(g, depth - 1); put_op(g, compare[roll(g, LEN(compare))], 0, 0); return;
    case 5: gen_bool(g, depth - 1); put_op(g, PURE_OP_NOT, 0, 0); return;
    case 8: gen_bool(g, depth - 1); put_op(g, PURE_OP_PICK, 0, 0); put_op(g, logic[roll(g, LEN(logic))], 0, 0); return;
    case 9: /* a value of either type dropped under a bool */
        if (roll(g, 2)) gen_bool(g, depth - 1);
        else gen_num(g, depth - 1);
        gen_bool(g, depth - 1);
        put_op(g, PURE_OP_SLIDE, 1, 0);
        return;
    default: gen_bool(g, depth - 1); gen_bool(g, depth - 1); put_op(g, logic[roll(g, LEN(logic))], 0, 0); return;
    }
}

/* What reads only the fold's list, its item and constants (a shaped fold's
 * own parts): a number, or a bool (flag). */
static void gen_own(Gen *g, int depth, bool flag) {
    static const double pool[] = {0, -0.0, 1, -1, 2, 0.5, 3, 5, 1e308};
    static const uint8_t binary[] = {PURE_OP_ADD, PURE_OP_SUB, PURE_OP_MUL, PURE_OP_DIV, PURE_OP_MIN, PURE_OP_MOD};
    static const uint8_t compare[] = {PURE_OP_LT, PURE_OP_GT, PURE_OP_EQ, PURE_OP_NE};
    static const uint8_t logic[] = {PURE_OP_AND, PURE_OP_OR, PURE_OP_EQ, PURE_OP_NE};
    int list = g->fold_list;
    switch (roll(g, depth <= 0 ? 3 : 7)) {
    case 0:
        if (flag) put_op(g, C, 1, roll(g, 2));
        else put_op(g, C, 0, pool[roll(g, LEN(pool))]);
        return;
    case 1: case 2:
        if (roll(g, 4)) put_op(g, PURE_OP_ROW, (uint32_t)list, 0);
        else put_op(g, roll(g, 2) ? PURE_OP_INDEX_FIRST : PURE_OP_INDEX_LAST, (uint32_t)list, 0);
        put_op(g, PURE_OP_ITEM_GET, (uint32_t)list | (flag ? (list == LANE_L ? 3u : 2u) : (uint32_t)roll(g, list == LANE_L ? 3 : 2)) << 16, 0);
        return;
    case 3:
        if (flag) { gen_own(g, depth - 1, false); gen_own(g, depth - 1, false); put_op(g, compare[roll(g, LEN(compare))], 0, 0); }
        else put_op(g, PURE_OP_LIST_COUNT, (uint32_t)list, 0);
        return;
    case 4: gen_own(g, depth - 1, flag); put_op(g, flag ? PURE_OP_NOT : NEG, 0, 0); return;
    default:
        gen_own(g, depth - 1, flag);
        gen_own(g, depth - 1, flag);
        put_op(g, flag ? logic[roll(g, LEN(logic))] : binary[roll(g, LEN(binary))], 0, 0);
        return;
    }
}

/* A fold in a column of L with a shape's test: the item's field against a
 * key the row gives, ANDed with the item's own tests, and an own value. */
static void gen_shaped(Gen *g, int list) {
    static const uint8_t relations[] = {PURE_OP_EQ, PURE_OP_EQ, PURE_OP_LT, PURE_OP_LE, PURE_OP_GT, PURE_OP_GE};
    unsigned how = roll(g, 5) < 2 ? PURE_FOLD_COUNT : roll(g, 6), before = roll(g, 3) ? 0 : 1 + roll(g, 2),
             after = roll(g, 3) ? 0 : 1;
    size_t at = g->count;
    put_op(g, PURE_OP_FOLD, 0, 0);
    int fold_list = g->fold_list, fold_outer = g->outer;
    g->fold_list = list, g->outer = LANE_L, g->tree = true;
    if (how != PURE_FOLD_COUNT && how != PURE_FOLD_FIRST) gen_own(g, 2, false);
    for (unsigned k = 0; k < before; ++k) {
        gen_own(g, 2, true);
        if (k) put_op(g, PURE_OP_AND, 0, 0);
    }
    bool field_first = roll(g, 2);
    for (unsigned side = 0; side < 2; ++side) {
        if (side == !field_first) { /* the item's field */
            put_op(g, PURE_OP_ROW, (uint32_t)list, 0);
            put_op(g, PURE_OP_ITEM_GET, (uint32_t)list | (uint32_t)roll(g, list == LANE_L ? 3 : 2) << 16, 0);
        } else { /* the key: the column's row's field, mostly */
            g->fold_list = -1;
            if (roll(g, 5)) {
                put_op(g, PURE_OP_OUTER_ROW, LANE_L, 0);
                put_op(g, PURE_OP_ITEM_GET, LANE_L | (uint32_t)roll(g, 3) << 16, 0);
            } else
                gen_num(g, 2);
            g->fold_list = list;
        }
    }
    put_op(g, relations[roll(g, LEN(relations))], 0, 0);
    if (before) put_op(g, PURE_OP_AND, 0, 0);
    for (unsigned k = 0; k < after; ++k) {
        gen_own(g, 2, true);
        put_op(g, PURE_OP_AND, 0, 0);
    }
    g->fold_list = fold_list, g->outer = fold_outer, g->tree = false;
    g->code[at].arg = FOLDING(how, list, g->count - at - 1);
    if (how == PURE_FOLD_FIRST) put_op(g, PURE_OP_INDEX_NUM, (uint32_t)list, 0);
}

/* A fold over list leaving a number (first's index as one), in a column of L (outer) or not. */
static void gen_fold(Gen *g, int list, int outer) {
    unsigned how = roll(g, 6); /* count, sum, max, min, first, distinct */
    size_t at = g->count;
    put_op(g, PURE_OP_FOLD, 0, 0);
    int fold_list = g->fold_list, fold_outer = g->outer;
    g->fold_list = list, g->outer = outer;
    if (how != PURE_FOLD_COUNT && how != PURE_FOLD_FIRST) gen_num(g, 3);
    gen_bool(g, 3);
    g->fold_list = fold_list, g->outer = fold_outer;
    g->code[at].arg = FOLDING(how, list, g->count - at - 1);
    if (how == PURE_FOLD_FIRST) put_op(g, PURE_OP_INDEX_NUM, (uint32_t)list, 0);
}

static bool same_number(bool a_ok, double a, bool b_ok, double b) {
    return a_ok == b_ok && (!a_ok || !memcmp(&a, &b, sizeof(a)));
}

static void lane_folds(void) {
    static const S numbers[] = {{"k", 3}, {"h0", 0}, {"h1", 0}, {"h2", 0}};
    static const T lists[] = {{"l", 3 | 1 << 8, 0}, {"m", 3, 1}};
    static const RF l_fields[] = {{"key", 0, 0}, {"a", 0, 0}, {"b", 0, 0}, {"c", 2, 0}},
                    m_fields[] = {{"x", 0, 0}, {"y", 0, 0}, {"z", 2, 1}};
    static const R records[] = {{"lrec", l_fields, 4}, {"mrec", m_fields, 3}};
    static const I inputs[] = {KEY("go", "KeyG")};
    static const double values[] = {0, -0.0, 1, -1, 2, 3, 0.5, -2.5, 5, 10, 1e308, -7};
    enum { VIEWS = 6, COLUMNS = 3, HANDLED = 3 };
    static Gen g;
    uint32_t seed = 0x1a2b3c4du;
    unsigned planned = 0, folds = 0, faults = 0, cells = 0, shaped[3] = {0}, passes = 0;
    for (unsigned round = 0; round < 300; ++round) {
        g = (Gen){.seed = next_random(&seed), .fold_list = -1, .outer = -1};
        V views[VIEWS];
        char names[VIEWS + 1 + COLUMNS][4];
        W tables[1 + COLUMNS];
        for (unsigned v = 0; v < VIEWS; ++v) {
            snprintf(names[v], sizeof(names[v]), "v%u", v);
            views[v] = (V){names[v], (uint32_t)g.count, 0};
            gen_fold(&g, roll(&g, 3) ? LANE_L : LANE_M, -1);
            views[v].length = (uint32_t)(g.count - views[v].start);
        }
        tables[0] = (W){"t", 2 | (LANE_L + 1) << 8, 0, (uint32_t)g.count, 0};
        g.fold_list = LANE_L;
        gen_bool(&g, 3);
        g.fold_list = -1;
        tables[0].length = (uint32_t)(g.count - tables[0].start);
        for (unsigned c = 0; c < COLUMNS; ++c) {
            snprintf(names[VIEWS + 1 + c], sizeof(names[0]), "c%u", c);
            tables[1 + c] = (W){names[VIEWS + 1 + c], 0, VIEWS + 1, (uint32_t)g.count, 0};
            if (roll(&g, 3)) gen_shaped(&g, roll(&g, 3) ? LANE_L : LANE_M);
            else gen_fold(&g, roll(&g, 3) ? LANE_L : LANE_M, LANE_L);
            tables[1 + c].length = (uint32_t)(g.count - tables[1 + c].start);
        }
        H handler = {1, (uint32_t)g.count, 0};
        for (unsigned h = 0; h < HANDLED; ++h) {
            gen_fold(&g, roll(&g, 2) ? LANE_L : LANE_M, -1);
            put_op(&g, ST, 1 + h, 0);
        }
        handler.length = (uint32_t)(g.count - handler.start);
        Spec spec = {numbers, LEN(numbers), inputs, LEN(inputs), views, VIEWS, &handler, 1, g.code, g.count, NULL, 0,
                     lists, LEN(lists), NULL, 0, NULL, 0, records, LEN(records), tables, LEN(tables)};
        if (load(spec, &module_a) != PURE_OK || load(spec, &module_b) != PURE_OK) {
            fprintf(stderr, "lane round %u: %s\n", round, error);
            abort();
        }
        for (uint32_t i = 0; i < module_b.code_count; ++i) {
            folds += module_a.code[i].op == PURE_OP_FOLD;
            planned += module_a.code[i].plan;
            shaped[module_a.code[i].shape ? module_a.shape[module_a.code[i].shape - 1].form : 0] +=
                module_a.code[i].op == PURE_OP_FOLD;
            module_b.code[i].plan = module_b.code[i].shape = 0;
        }
        planned += module_a.view_plan[VIEWS];
        memset(module_b.view_plan, 0, module_b.views * sizeof(*module_b.view_plan));
        pure_jit_free(module_b.jit); /* its code changed: compiled again (021) */
        module_b.jit = pure_jit_make(&module_b);
        PureApp a, b;
        CHECK(pure_app_start(&a, &module_a, (PureData){0}) == PURE_OK && pure_app_start(&b, &module_b, (PureData){0}) == PURE_OK);
        pure_app_work(&a, UINT64_MAX); /* counted, never past it */
        pure_app_work(&b, UINT64_MAX);
        size_t l_items = roll(&g, 4) ? roll(&g, 200) : 0, m_items = roll(&g, 4) ? roll(&g, 100) : 0;
        for (size_t i = 0; i < l_items + m_items; ++i) {
            bool in_l = i < l_items;
            double flag = roll(&g, 2);
            PureCell cells_[4] = {{.num = in_l ? 2.0 * (double)i : values[roll(&g, LEN(values))]},
                                  {.num = values[roll(&g, LEN(values))]},
                                  {.num = in_l ? values[roll(&g, LEN(values))] : flag}, {.num = flag}};
            PureList *list_a = a.lists[in_l ? LANE_L : LANE_M], *list_b = b.lists[in_l ? LANE_L : LANE_M];
            CHECK(pure_list_insert(list_a, pure_list_count(list_a), cells_, NULL) == PURE_OK &&
                  pure_list_insert(list_b, pure_list_count(list_b), cells_, NULL) == PURE_OK);
        }
        for (unsigned v = 0; v < VIEWS; ++v) {
            double x = 0, y = 0;
            bool ok_a = pure_app_view(&a, v, &x), ok_b = pure_app_view(&b, v, &y);
            faults += !ok_a;
            CHECK(a.work == b.work); /* counted as written, however it ran */
            if (!same_number(ok_a, x, ok_b, y)) {
                fprintf(stderr, "lane round %u view %u: %d %g / %d %g\n", round, v, ok_a, x, ok_b, y);
                abort();
            }
        }
        size_t rows = pure_app_rows(&a, VIEWS);
        CHECK(rows == pure_app_rows(&b, VIEWS) && a.work == b.work);
        for (size_t row = 0; row < rows; ++row) {
            size_t item_a, item_b;
            CHECK(pure_app_row_item(&a, VIEWS, row, &item_a) && pure_app_row_item(&b, VIEWS, row, &item_b) && item_a == item_b);
            for (unsigned c = 0; c < COLUMNS; ++c) {
                double x = 0, y = 0;
                bool ok_a = pure_app_cell(&a, VIEWS + 1 + c, row, &x), ok_b = pure_app_cell(&b, VIEWS + 1 + c, row, &y);
                ++cells;
                CHECK(a.work == b.work);
                if (!same_number(ok_a, x, ok_b, y)) {
                    fprintf(stderr, "lane round %u column %u row %zu: %d %g / %d %g\n", round, c, row, ok_a, x, ok_b, y);
                    abort();
                }
            }
        }
        passes += pure_app_passes(&a);
        PureResult went_a = press(&a, 0), went_b = press(&b, 0);
        if (went_a != went_b || a.fault != b.fault || a.fault_pc != b.fault_pc || a.work != b.work ||
            memcmp(a.values, b.values, 4 * sizeof(double))) {
            fprintf(stderr, "lane round %u handler: %d %d %u / %d %d %u\n", round, went_a, a.fault, a.fault_pc, went_b, b.fault,
                    b.fault_pc);
            abort();
        }
        faults += went_a != PURE_OK;
        pure_app_stop(&a);
        pure_app_stop(&b);
        pure_module_release(&module_a);
        pure_module_release(&module_b);
    }
    CHECK(planned > folds / 2 && faults > 100 && cells > 1000 && shaped[PURE_SHAPE_GROUP] > 80 &&
          shaped[PURE_SHAPE_RANK] > 100 && passes > 60);
    printf("{\"lane_folds\":%u,\"planned\":%u,\"groups\":%u,\"ranks\":%u,\"passes\":%u,\"faults_alike\":%u,"
           "\"cells\":%u}\n", folds, planned, shaped[PURE_SHAPE_GROUP], shaped[PURE_SHAPE_RANK], passes, faults, cells);
}

/* ---- Walks and a message's rows ----------------------------------------
 * A shop: stock is keyed by item; an order's rows (a message's rows field,
 * filling the list order.lines) each take their quantity from the item's
 * stock and are logged, all or nothing (an item not stocked, or not enough of
 * it, fails the order); clean walks the stock, removing an item with none
 * left, removing the first item for one with one left, and adding one to
 * the others. Random events against a model: the walk visits every item
 * still there when it comes, once, in order, and the rows are gone after
 * every event. */
enum { EACH = PURE_OP_EACH, W_N = 0, W_SUM = 1, W_STOCK = 2, W_LINES = 3, W_LOG = 4 };
#define WALKING(list, length) ((uint32_t)(list) << 8 | (uint32_t)(length) << 16)
#define ITEM(list, field) ((uint32_t)(list) | (uint32_t)(field) << 16)
static const S walk_numbers[] = {{"n", 0}, {"sum", 0}};
static const T walk_lists[] = {{"stock", 3 | 1 << 8, 0}, {"order.lines", 3 | (uint32_t)PURE_LIST_ROWS << 8, 1}, {"log", 3, 2}};
static const RF stock_fields[] = {{"item", 0, 0}, {"left", 0, 0}}, line_fields[] = {{"item", 0, 0}, {"qty", 0, 0}},
                entry_fields[] = {{"item", 0, 0}, {"qty", 0, 0}};
static const R walk_records[] = {{"Stock", stock_fields, 2}, {"Line", line_fields, 2}, {"Entry", entry_fields, 2}};
static const F order_fields[] = {{"lines", PURE_FIELD_ROWS | W_LINES << 8}}, restock_fields[] = {{"item", 1}, {"left", 1}};
static const I walk_inputs[] = {{"order", NULL, PURE_KIND_MESSAGE, order_fields, 1}, KEY("clean", "KeyC"),
                                {"restock", NULL, PURE_KIND_MESSAGE, restock_fields, 2}};
static const O walk_code[] = {
    /* 0 order: for l in order.lines { stock[l.item].left -= l.qty (not below 0); log.append(l) } */
    {EACH, WALKING(W_LINES, 20), 0},
    {ROW, W_LINES, 0}, {IG, ITEM(W_LINES, 0), 0}, {PURE_OP_LIST_FIND, W_STOCK, 0},
    {ROW, W_LINES, 0}, {IG, ITEM(W_LINES, 0), 0}, {PURE_OP_LIST_FIND, W_STOCK, 0}, {IG, ITEM(W_STOCK, 1), 0},
    {ROW, W_LINES, 0}, {IG, ITEM(W_LINES, 1), 0}, {PURE_OP_SUB, 0, 0},
    {PICK, 0, 0}, {C, 0, 0}, {PURE_OP_GE, 0, 0}, {REQ, 0, 0},
    {IS, ITEM(W_STOCK, 1), 0},
    {ROW, W_LINES, 0}, {IG, ITEM(W_LINES, 0), 0}, {ROW, W_LINES, 0}, {IG, ITEM(W_LINES, 1), 0}, {LA, W_LOG, 0},
    /* 21: n += 1; sum += sum(l.qty for l in order.lines) */
    {L, W_N, 0}, {C, 0, 1}, {ADD, 0, 0}, {ST, W_N, 0},
    {L, W_SUM, 0}, {FOLD, FOLDING(PURE_FOLD_SUM, W_LINES, 3), 0}, {ROW, W_LINES, 0}, {IG, ITEM(W_LINES, 1), 0},
    {C, 1, 1}, {ADD, 0, 0}, {ST, W_SUM, 0},
    /* 32 clean: for s in stock { if s.left == 0 { stock.remove(s) } else if s.left == 1 { stock.remove(stock.first) }
     *                             else { s.left = s.left + 1 } } */
    {EACH, WALKING(W_STOCK, 22), 0},
    {ROW, W_STOCK, 0}, {IG, ITEM(W_STOCK, 1), 0}, {C, 0, 0}, {EQ, 0, 0}, {JZ, 41, 0},
    {ROW, W_STOCK, 0}, {LR, W_STOCK, 0}, {JMP, 55, 0},
    {ROW, W_STOCK, 0}, {IG, ITEM(W_STOCK, 1), 0}, {C, 0, 1}, {EQ, 0, 0}, {JZ, 49, 0},
    {XF, W_STOCK, 0}, {LR, W_STOCK, 0}, {JMP, 55, 0},
    {ROW, W_STOCK, 0}, {ROW, W_STOCK, 0}, {IG, ITEM(W_STOCK, 1), 0}, {C, 0, 1}, {ADD, 0, 0}, {IS, ITEM(W_STOCK, 1), 0},
    /* 55 restock: stock.add(Stock { item, left }) */
    {FIELD, 0, 0}, {FIELD, 1, 0}, {LA, W_STOCK, 0},
};
static const H walk_handlers[] = {{1, 0, 32}, {3, 32, 23}, {5, 55, 3}};

static Spec walk_spec(const O *code, size_t count, const I *inputs, size_t ninputs, const H *handlers, size_t nhandlers,
                      const T *lists, const V *views, size_t nviews) {
    return (Spec){walk_numbers, LEN(walk_numbers), inputs, ninputs, views, nviews, handlers, nhandlers, code, count, NULL, 0,
                  lists, 3, NULL, 0, NULL, 0, walk_records, LEN(walk_records), NULL, 0};
}

typedef struct { double item, left; } StockItem;
typedef struct { StockItem stock[64]; size_t stocked; double log[4096][2]; size_t logged; double n, sum; } Shop;

static bool same_shop(const PureApp *app, const Shop *shop) {
    const PureList *stock = app->lists[W_STOCK], *log = app->lists[W_LOG];
    if (pure_list_count(stock) != shop->stocked || pure_list_count(log) != shop->logged ||
        pure_list_count(app->lists[W_LINES]) || app->values[W_N].num != shop->n || app->values[W_SUM].num != shop->sum)
        return false;
    for (size_t i = 0; i < shop->stocked; ++i)
        if (pure_list_get(stock, i, 0).num != shop->stock[i].item || pure_list_get(stock, i, 1).num != shop->stock[i].left)
            return false;
    for (size_t i = 0; i < shop->logged; ++i)
        if (pure_list_get(log, i, 0).num != shop->log[i][0] || pure_list_get(log, i, 1).num != shop->log[i][1]) return false;
    return true;
}

static size_t put_row(unsigned char *out, double item, double qty) {
    put_double(out, item);
    put_double(out + 8, qty);
    return 16;
}

static void walks(void) {
    CHECK(load(walk_spec(walk_code, LEN(walk_code), walk_inputs, 3, walk_handlers, 3, walk_lists, NULL, 0), &module_a) == PURE_OK);
    PureApp app;
    CHECK(pure_app_start(&app, &module_a, (PureData){0}) == PURE_OK);
    static Shop shop, before;
    shop = (Shop){.stocked = 0};
    uint32_t seed = 0x5a1e5eedu;
    unsigned orders = 0, failed = 0, cleaned = 0, removed = 0, earlier = 0;
    for (unsigned round = 0; round < 3000; ++round) {
        before = shop;
        unsigned roll_ = next_random(&seed) % 10;
        PureResult result;
        bool ok = true;
        if (roll_ < 3) { /* restock an item, maybe one there already */
            double item = (double)(next_random(&seed) % 12), left = (double)(next_random(&seed) % 4);
            unsigned char payload[8];
            put32(payload, (uint32_t)item);
            put32(payload + 4, (uint32_t)left);
            result = pure_app_message(&app, 2, (PureSpan){payload, 8});
            size_t at = 0;
            while (at < shop.stocked && shop.stock[at].item < item) ++at;
            if (at < shop.stocked && shop.stock[at].item == item) ok = false;
            else {
                memmove(&shop.stock[at + 1], &shop.stock[at], (shop.stocked - at) * sizeof(*shop.stock));
                shop.stock[at] = (StockItem){item, left};
                ++shop.stocked;
            }
        } else if (roll_ < 8) { /* an order of up to 6 lines */
            unsigned char payload[6 * 16];
            size_t size = 0, lines = next_random(&seed) % 7;
            double qty_total = 0;
            for (size_t k = 0; k < lines && ok; ++k) {
                double item = (double)(next_random(&seed) % 13), qty = (double)(next_random(&seed) % 3);
                size += put_row(payload + size, item, qty);
                size_t at = 0;
                while (at < shop.stocked && shop.stock[at].item != item) ++at;
                if (at == shop.stocked || shop.stock[at].left - qty < 0) { ok = false; break; }
                shop.stock[at].left -= qty;
                shop.log[shop.logged][0] = item;
                shop.log[shop.logged++][1] = qty;
                qty_total += qty;
            }
            for (size_t k = 0; !ok && k < lines; ++k) /* the rest of the rows, for the payload */
                if (size < (k + 1) * 16) size += put_row(payload + size, (double)(next_random(&seed) % 13), 1);
            result = pure_app_message(&app, 0, (PureSpan){size ? payload : NULL, size});
            if (ok) {
                shop.n += 1;
                shop.sum += qty_total;
                ++orders;
            }
        } else { /* clean */
            result = press(&app, 1);
            size_t at = 0;
            while (at < shop.stocked) {
                bool gone = false;
                if (shop.stock[at].left == 0) {
                    memmove(&shop.stock[at], &shop.stock[at + 1], (shop.stocked - at - 1) * sizeof(*shop.stock));
                    --shop.stocked;
                    gone = true;
                    ++removed;
                } else if (shop.stock[at].left == 1) {
                    memmove(&shop.stock[0], &shop.stock[1], (shop.stocked - 1) * sizeof(*shop.stock));
                    --shop.stocked;
                    if (at == 0) gone = true;
                    else --at, ++earlier;
                    ++removed;
                } else
                    shop.stock[at].left += 1;
                if (!gone) ++at;
            }
            ++cleaned;
            release(&app, 1);
        }
        if (!ok) {
            shop = before;
            ++failed;
        }
        if ((result == PURE_OK) != ok || !same_shop(&app, &shop)) {
            fprintf(stderr, "walk round %u: result %d, expected %s\n", round, result, ok ? "ok" : "a fault");
            abort();
        }
        if (shop.logged > 4000) { /* start over before the model's log fills */
            pure_app_stop(&app);
            CHECK(pure_app_start(&app, &module_a, (PureData){0}) == PURE_OK);
            shop = (Shop){.stocked = 0};
        }
    }
    /* Rows that are not whole fail the message and change nothing. */
    before = shop;
    unsigned char short_row[12] = {0}, not_finite[16];
    put_double(not_finite, 1);
    put_double(not_finite + 8, NAN);
    CHECK(pure_app_message(&app, 0, (PureSpan){short_row, 12}) == PURE_LIMIT && same_shop(&app, &before));
    CHECK(pure_app_message(&app, 0, (PureSpan){not_finite, 16}) == PURE_LIMIT && same_shop(&app, &before));
    pure_app_stop(&app);

    /* What a walk and a rows field may not be. */
    static const F note_fields[] = {{"lines", PURE_FIELD_ROWS | W_LINES << 8}, {"text", 4}};
    static const I note_inputs[] = {{"order", NULL, PURE_KIND_MESSAGE, note_fields, 2}};
    static const O into_walked[] = {{EACH, WALKING(W_STOCK, 3), 0}, {C, 0, 1}, {C, 0, 2}, {LA, W_STOCK, 0}};
    static const O nested[] = {{EACH, WALKING(W_STOCK, 4), 0}, {EACH, WALKING(W_LOG, 3), 0}, {C, 0, 1}, {C, 0, 2},
                               {LA, W_STOCK, 0}};
    static const O nested_remove[] = {{EACH, WALKING(W_STOCK, 3), 0}, {EACH, WALKING(W_LOG, 2), 0}, {XF, W_STOCK, 0},
                                      {LR, W_STOCK, 0}};
    static const O replied[] = {{EACH, WALKING(W_STOCK, 2), 0}, {FIELD, 1, 0}, {REPLY, 0, 0}};
    static const O rows_read[] = {{FIELD, 0, 0}, {PURE_OP_BYTES_FROM, 0, 0}, {REPLY, 0, 0}};
    static const O into_each[] = {{C, 1, 1}, {JZ, 3, 0}, {EACH, WALKING(W_STOCK, 2), 0}, {C, 0, 1}, {ST, W_N, 0}};
    static const O view_each[] = {{EACH, WALKING(W_STOCK, 2), 0}, {C, 0, 1}, {ST, W_N, 0}, {C, 0, 1}};
    static const H first3[] = {{1, 0, 4}}, first3b[] = {{1, 0, 3}}, first4[] = {{1, 0, 4}}, first5[] = {{1, 0, 5}};
    static const V walk_view[] = {{"v", 0, 4}};
    static const T unnamed[] = {{"stock", 3 | 1 << 8, 0}, {"lines", 3 | (uint32_t)PURE_LIST_ROWS << 8, 1}, {"log", 3, 2}};
    static const F logged_rows[] = {{"lines", PURE_FIELD_ROWS | W_LOG << 8}};
    static const I logged_inputs[] = {{"order", NULL, PURE_KIND_MESSAGE, logged_rows, 1}};
    static const I sent_rows[] = {{"order", NULL, PURE_KIND_OUTPUT, order_fields, 1}};
    const struct { Spec spec; const char *reason; } refused[] = {
        {walk_spec(into_walked, LEN(into_walked), note_inputs, 1, first3, 1, walk_lists, NULL, 0), "insert into the list walked"},
        {walk_spec(nested, LEN(nested), note_inputs, 1, first5, 1, walk_lists, NULL, 0), "insert into the list walked"},
        {walk_spec(nested_remove, LEN(nested_remove), note_inputs, 1, first4, 1, walk_lists, NULL, 0),
         "remove from a list an outer walk walks"},
        {walk_spec(replied, LEN(replied), note_inputs, 1, first3b, 1, walk_lists, NULL, 0), "reply inside a loop"},
        {walk_spec(rows_read, LEN(rows_read), note_inputs, 1, first3b, 1, walk_lists, NULL, 0), "no such state, field"},
        {walk_spec(into_each, LEN(into_each), note_inputs, 1, first5, 1, walk_lists, NULL, 0), "branch into an each"},
        {walk_spec(view_each, LEN(view_each), note_inputs, 1, NULL, 0, walk_lists, walk_view, 1), "view with an effect"},
        {walk_spec(NULL, 0, note_inputs, 1, NULL, 0, unnamed, NULL, 0), "state name"},
        {walk_spec(NULL, 0, logged_inputs, 1, NULL, 0, walk_lists, NULL, 0), "rows field"},
        {walk_spec(NULL, 0, sent_rows, 1, NULL, 0, walk_lists, NULL, 0), "rows field"},
    };
    for (size_t i = 0; i < LEN(refused); ++i)
        if (load(refused[i].spec, &module_b) != PURE_INVALID || !strstr(error, refused[i].reason)) {
            fprintf(stderr, "walk case %zu: expected '%s', got '%s'\n", i, refused[i].reason, error);
            abort();
        }
    CHECK(orders > 300 && failed > 300 && cleaned > 200 && removed > 50 && earlier > 10);
    printf("{\"walks\":{\"orders\":%u,\"failed\":%u,\"cleaned\":%u,\"removed_while_walking\":%u,\"removed_before\":%u}}\n",
           orders, failed, cleaned, removed, earlier);
}

/* Damaged walk modules: whatever loads runs safely, rows and all. */
static void walk_mutations(void) {
    static unsigned char valid[65536], bytes[65536];
    size_t size = build(valid, walk_spec(walk_code, LEN(walk_code), walk_inputs, 3, walk_handlers, 3, walk_lists, NULL, 0));
    uint32_t state = 0x2f6b1c3du;
    unsigned accepted = 0;
    for (unsigned round = 0; round < 20000; ++round) {
        memcpy(bytes, valid, size);
        for (unsigned f = 0, flips = 1 + next_random(&state) % 3; f < flips; ++f)
            bytes[next_random(&state) % size] ^= (unsigned char)(1u << (next_random(&state) % 8));
        if (pure_module_load(bytes, size, &module_b, error, sizeof(error)) != PURE_OK) continue;
        ++accepted;
        PureApp app;
        if (pure_app_start(&app, &module_b, (PureData){0}) != PURE_OK) continue;
        unsigned char payload[96];
        for (unsigned e = 0; e < 24; ++e) {
            uint32_t input = next_random(&state) % module_b.inputs;
            size_t length = next_random(&state) % sizeof(payload);
            for (size_t k = 0; k < length; ++k) payload[k] = (unsigned char)next_random(&state);
            for (size_t k = 0; k + 16 <= length && next_random(&state) % 2; k += 16)
                put_row(payload + k, (double)(next_random(&state) % 8), (double)(next_random(&state) % 3));
            if (module_b.input[input].kind == PURE_KIND_MESSAGE)
                pure_app_message(&app, input, (PureSpan){payload, length});
            else if (module_b.input[input].binding == PURE_BIND_PRESS)
                press(&app, input);
            for (uint32_t s_ = 0; s_ < module_b.states; ++s_)
                if (module_b.state_type[s_] == PURE_STATE_LIST && module_b.state_flags[s_] & PURE_LIST_ROWS >> 8)
                    CHECK(!pure_list_count(app.lists[s_])); /* rows go with their event */
        }
        pure_app_stop(&app);
    }
    printf("{\"walk_module_mutations\":20000,\"accepted_after_damage\":%u}\n", accepted);
}

/* ---- Views that take arguments -----------------------------------------
 * balance(aid) finds an account by key, rich(min) counts accounts above an
 * argument (a leaf at a time), echo(name) shows a text argument as it is,
 * branch(b) is a table filtered by an argument whose column folds with it;
 * a handler's fold reads its message. Given again, arguments are read
 * anew: a filter's rows are kept for the arguments they were made for. */
enum { A_N = 0, A_ACCOUNTS = 1 };
static const S argued_numbers[] = {{"n", 0}};
static const T argued_lists[] = {{"accounts", 3 | 1 << 8, 0}};
static const RF account_fields[] = {{"aid", 0, 0}, {"bid", 0, 0}, {"balance", 0, 0}};
static const R argued_records[] = {{"Account", account_fields, 3}};
static const F pay_fields[] = {{"b", 1}}, add_fields[] = {{"aid", 1}, {"bid", 1}, {"balance", 3}},
               aid_fields[] = {{"aid", 1}}, min_fields[] = {{"min", 3}}, name_fields[] = {{"name", 4}}, b_fields[] = {{"b", 1}};
static const I argued_inputs[] = {
    {"pay", NULL, PURE_KIND_MESSAGE, pay_fields, 1}, {"add", NULL, PURE_KIND_MESSAGE, add_fields, 3},
    {"balance.args", "0", PURE_KIND_ARGUMENTS, aid_fields, 1},
    {"rich.args", "1", PURE_KIND_ARGUMENTS, min_fields, 1},
    {"echo.args", "2", PURE_KIND_ARGUMENTS, name_fields, 1},
    {"branch.args", "3", PURE_KIND_ARGUMENTS, b_fields, 1}};
static const O argued_code[] = {
    /* 0 balance(aid) = accounts[aid].balance */
    {FIELD, 0, 0}, {PURE_OP_LIST_FIND, A_ACCOUNTS, 0}, {IG, ITEM(A_ACCOUNTS, 2), 0},
    /* 3 rich(min) = count(a in accounts where a.balance > min) */
    {FOLD, FOLDING(PURE_FOLD_COUNT, A_ACCOUNTS, 4), 0}, {ROW, A_ACCOUNTS, 0}, {IG, ITEM(A_ACCOUNTS, 2), 0}, {FIELD, 0, 0},
    {GT, 0, 0},
    /* 8 echo(name) = name */
    {FIELD, 0, 0},
    /* 9 branch(b): each a in accounts where a.bid == b */
    {ROW, A_ACCOUNTS, 0}, {IG, ITEM(A_ACCOUNTS, 1), 0}, {FIELD, 0, 0}, {EQ, 0, 0},
    /* 13 aid = a.aid */
    {ROW, A_ACCOUNTS, 0}, {IG, ITEM(A_ACCOUNTS, 0), 0},
    /* 15 share = a.balance / sum(x.balance for x in accounts where x.bid == b) */
    {ROW, A_ACCOUNTS, 0}, {IG, ITEM(A_ACCOUNTS, 2), 0}, {FOLD, FOLDING(PURE_FOLD_SUM, A_ACCOUNTS, 6), 0}, {ROW, A_ACCOUNTS, 0},
    {IG, ITEM(A_ACCOUNTS, 2), 0}, {ROW, A_ACCOUNTS, 0}, {IG, ITEM(A_ACCOUNTS, 1), 0}, {FIELD, 0, 0}, {EQ, 0, 0},
    {DIV, 0, 0},
    /* 25 on pay: n = count(a in accounts where a.bid == pay.b) */
    {FOLD, FOLDING(PURE_FOLD_COUNT, A_ACCOUNTS, 4), 0}, {ROW, A_ACCOUNTS, 0}, {IG, ITEM(A_ACCOUNTS, 1), 0}, {FIELD, 0, 0},
    {EQ, 0, 0}, {ST, A_N, 0},
    /* 31 on add: accounts.add(Account { aid, bid, balance }) */
    {FIELD, 0, 0}, {FIELD, 1, 0}, {FIELD, 2, 0}, {LA, A_ACCOUNTS, 0},
};
static const V argued_views[] = {{"balance", 0, 3}, {"rich", 3, 5}};
static const V argued_texts[] = {{"echo", 8, 1}};
static const W argued_tables[] = {{"branch", 2 | (A_ACCOUNTS + 1) << 8, 0, 9, 4}, {"aid", 0, 4, 13, 2}, {"share", 0, 4, 15, 10}};
static const H argued_handlers[] = {{1, 25, 6}, {3, 31, 4}};

static Spec argued_spec(const I *inputs, size_t ninputs, const H *handlers, size_t nhandlers) {
    return (Spec){argued_numbers, 1, inputs, ninputs, argued_views, 2, handlers, nhandlers, argued_code, LEN(argued_code), NULL, 0,
                  argued_lists, 1, NULL, 0, argued_texts, 1, argued_records, 1, argued_tables, 3};
}

static PureResult give(PureApp *app, uint32_t view, const unsigned char *payload, size_t size) {
    return pure_app_arguments(app, view, (PureSpan){payload, size}, NULL);
}

static void view_arguments(void) {
    CHECK(load(argued_spec(argued_inputs, LEN(argued_inputs), argued_handlers, 2), &module_a) == PURE_OK);
    CHECK(pure_module_arguments(&module_a, 0) == 2 && pure_module_arguments(&module_a, 5) == 5 &&
          pure_module_arguments(&module_a, 3) == 5 && module_a.code[3].plan && module_a.code[25].plan);
    PureApp app;
    CHECK(pure_app_start(&app, &module_a, (PureData){0}) == PURE_OK);
    for (uint32_t aid = 1; aid <= 30; ++aid) {
        unsigned char payload[16];
        put32(payload, aid);
        put32(payload + 4, aid % 3);
        put_double(payload + 8, aid * 10.0);
        CHECK(pure_app_message(&app, 1, (PureSpan){payload, 16}) == PURE_OK);
    }
    double value;
    unsigned char four[8];
    CHECK(!pure_app_view(&app, 0, &value)); /* no arguments given */
    put32(four, 7);
    CHECK(give(&app, 0, four, 4) == PURE_OK && pure_app_view(&app, 0, &value) && value == 70);
    put32(four, 99);
    CHECK(give(&app, 0, four, 4) == PURE_OK && !pure_app_view(&app, 0, &value)); /* no such account */
    CHECK(give(&app, 0, four, 3) == PURE_LIMIT && !pure_app_view(&app, 0, &value));
    for (double min = -1; min <= 310; min += 7.5) {
        put_double(four, min);
        size_t above = 0;
        for (uint32_t aid = 1; aid <= 30; ++aid) above += aid * 10.0 > min;
        CHECK(give(&app, 1, four, 8) == PURE_OK && pure_app_view(&app, 1, &value) && value == (double)above);
    }
    put_double(four, NAN);
    CHECK(give(&app, 1, four, 8) == PURE_INVALID);
    const char *name = "argument: 한글 ✓";
    CHECK(give(&app, 2, (const unsigned char *)name, strlen(name)) == PURE_OK);
    Text shown = {{0}, 0};
    CHECK(pure_app_view_text(&app, 2, text_span, &shown) == PURE_OK && shown.size == strlen(name) &&
          !memcmp(shown.text, name, shown.size));
    CHECK(give(&app, 2, (const unsigned char *)"\xff", 1) == PURE_INVALID);
    for (uint32_t b = 0; b < 3; ++b) {
        put32(four, b);
        CHECK(give(&app, 3, four, 4) == PURE_OK && pure_app_rows(&app, 3) == 10);
        double total = 0;
        for (uint32_t aid = 1; aid <= 30; ++aid) if (aid % 3 == b) total += aid * 10.0;
        for (size_t row = 0; row < 10; ++row) {
            double aid, share;
            CHECK(pure_app_cell(&app, 4, row, &aid) && (uint32_t)aid % 3 == b && pure_app_cell(&app, 5, row, &share) &&
                  share == aid * 10.0 / total);
        }
    }
    put32(four, 2);
    CHECK(pure_app_message(&app, 0, (PureSpan){four, 4}) == PURE_OK && app.values[A_N].num == 10);
    pure_app_stop(&app);

    /* What arguments may not be. */
    static const I on_column[] = {{"pay", NULL, PURE_KIND_MESSAGE, pay_fields, 1},
                                  {"aid.args", "4", PURE_KIND_ARGUMENTS, aid_fields, 1}};
    static const I misnamed[] = {{"pay", NULL, PURE_KIND_MESSAGE, pay_fields, 1},
                                 {"balance.argz", "0", PURE_KIND_ARGUMENTS, aid_fields, 1}};
    static const I twice[] = {{"pay", NULL, PURE_KIND_MESSAGE, pay_fields, 1},
                              {"balance.args", "0", PURE_KIND_ARGUMENTS, aid_fields, 1},
                              {"balance.args", "0", PURE_KIND_ARGUMENTS, aid_fields, 1}};
    static const H handled[] = {{5, 31, 4}};
    const struct { Spec spec; const char *reason; } refused[] = {
        {argued_spec(on_column, 2, NULL, 0), "arguments"},
        {argued_spec(misnamed, 2, NULL, 0), "arguments"},
        {argued_spec(twice, 3, NULL, 0), "arguments"},
        {argued_spec(argued_inputs, LEN(argued_inputs), handled, 1), "handler event"},
        {argued_spec(argued_inputs, 2, NULL, 0), "view with an effect"}, /* FIELD without arguments */
    };
    for (size_t i = 0; i < LEN(refused); ++i)
        if (load(refused[i].spec, &module_b) != PURE_INVALID || !strstr(error, refused[i].reason)) {
            fprintf(stderr, "arguments case %zu: expected '%s', got '%s'\n", i, refused[i].reason, error);
            abort();
        }
    printf("{\"view_arguments\":{\"accounts\":30,\"branches\":3}}\n");
}

/* ---- Locals ----------------------------------------------------------
 * A handler's locals start at 0 each event, are shared by its walks and the
 * folds in them, and are never saved: total sums the items in a walk, a fold
 * counts the items below it, an index local finds the last item, and every
 * event begins again. */
enum { LOCAL_L = PURE_OP_LOCAL_LOAD, LOCAL_S = PURE_OP_LOCAL_STORE, L_N = 0, L_M = 1, L_XS = 2 };
#define LOCAL(slot, list) ((uint32_t)(slot) | ((list) >= 0 ? 1u << 8 | (uint32_t)(list) << 16 : 0))
static const S local_numbers[] = {{"n", 0}, {"m", 0}};
static const T local_lists[] = {{"xs", 3, 0}};
static const RF local_fields[] = {{"a", 0, 0}};
static const R local_records[] = {{"X", local_fields, 1}};
static const F local_add_fields[] = {{"a", 3}};
static const I local_inputs[] = {KEY("sum", "KeyS"), {"add", NULL, PURE_KIND_MESSAGE, local_add_fields, 1}};
static const O local_code[] = {
    /* 0 on sum: total = total + x.a for each x (total starts at 0); n = count(x in xs where x.a < total);
     *   last = xs.last; m = total + last.a (every event: total is 0 again) */
    {LOCAL_L, LOCAL(0, -1), 0}, {C, 0, 1}, {ADD, 0, 0}, {LOCAL_S, LOCAL(0, -1), 0}, /* starts at 0 each event: 1 */
    {EACH, WALKING(L_XS, 5), 0}, {LOCAL_L, LOCAL(0, -1), 0}, {ROW, L_XS, 0}, {IG, ITEM(L_XS, 0), 0}, {ADD, 0, 0},
    {LOCAL_S, LOCAL(0, -1), 0},
    {FOLD, FOLDING(PURE_FOLD_COUNT, L_XS, 4), 0}, {ROW, L_XS, 0}, {IG, ITEM(L_XS, 0), 0}, {LOCAL_L, LOCAL(0, -1), 0},
    {PURE_OP_LT, 0, 0}, {ST, L_N, 0},
    {XE, L_XS, 0}, {LOCAL_S, LOCAL(1, L_XS), 0},
    {LOCAL_L, LOCAL(0, -1), 0}, {LOCAL_L, LOCAL(1, L_XS), 0}, {IG, ITEM(L_XS, 0), 0}, {ADD, 0, 0}, {ST, L_M, 0},
    /* 23 on add: xs.append(X { a }) */
    {FIELD, 0, 0}, {LA, L_XS, 0},
};
static const H local_handlers[] = {{1, 0, 23}, {3, 23, 2}};

static Spec local_spec(const O *code, size_t count, const H *handlers, size_t nhandlers, const V *views, size_t nviews) {
    return (Spec){local_numbers, 2, local_inputs, 2, views, nviews, handlers, nhandlers, code, count, NULL, 0, local_lists, 1,
                  NULL, 0, NULL, 0, local_records, 1, NULL, 0};
}

static void locals(void) {
    CHECK(load(local_spec(local_code, LEN(local_code), local_handlers, 2, NULL, 0), &module_a) == PURE_OK && module_a.code[10].plan);
    PureApp app;
    CHECK(pure_app_start(&app, &module_a, (PureData){0}) == PURE_OK);
    double items[64];
    size_t count = 0;
    uint32_t seed = 0x10ca15u;
    for (unsigned round = 0; round < 40; ++round) {
        unsigned char payload[8];
        items[count] = (double)(next_random(&seed) % 20) - 5;
        put_double(payload, items[count++]);
        CHECK(pure_app_message(&app, 1, (PureSpan){payload, 8}) == PURE_OK);
        CHECK(press(&app, 0) == PURE_OK);
        double total = 1, below = 0;
        for (size_t i = 0; i < count; ++i) total += items[i];
        for (size_t i = 0; i < count; ++i) below += items[i] < total;
        CHECK(app.values[L_N].num == below && app.values[L_M].num == total + items[count - 1]);
    }
    pure_app_stop(&app);

    /* What a local may not be. */
    static const O view_local[] = {{LOCAL_L, LOCAL(0, -1), 0}};
    static const V local_view[] = {{"v", 0, 1}};
    static const O fold_store[] = {{FOLD, FOLDING(PURE_FOLD_COUNT, L_XS, 3), 0}, {C, 0, 1}, {LOCAL_S, LOCAL(0, -1), 0}, {C, 0, 1},
                                   {ST, L_N, 0}};
    static const O two_things[] = {{C, 0, 1}, {LOCAL_S, LOCAL(0, -1), 0}, {XF, L_XS, 0}, {LOCAL_S, LOCAL(0, L_XS), 0}};
    static const O other_list[] = {{XF, L_XS, 0}, {LOCAL_S, LOCAL(0, 3), 0}};
    static const O far_slot[] = {{C, 0, 1}, {LOCAL_S, LOCAL(64, -1), 0}};
    static const H first2[] = {{1, 0, 2}}, first4[] = {{1, 0, 4}}, first5[] = {{1, 0, 5}};
    /* A view outside a table computes (018): its locals begin at 0. */
    CHECK(load(local_spec(view_local, 1, NULL, 0, local_view, 1), &module_b) == PURE_OK && module_b.view_body[0]);
    pure_module_release(&module_b);
    const struct { Spec spec; const char *reason; } refused[] = {
        {local_spec(fold_store, LEN(fold_store), first5, 1, NULL, 0), "fold with an effect"},
        {local_spec(two_things, LEN(two_things), first4, 1, NULL, 0), "local used as two things"},
        {local_spec(other_list, LEN(other_list), first2, 1, NULL, 0), "no such state"},
        {local_spec(far_slot, LEN(far_slot), first2, 1, NULL, 0), "no such state"},
    };
    for (size_t i = 0; i < LEN(refused); ++i)
        if (load(refused[i].spec, &module_b) != PURE_INVALID || !strstr(error, refused[i].reason)) {
            fprintf(stderr, "local case %zu: expected '%s', got '%s'\n", i, refused[i].reason, error);
            abort();
        }
    printf("{\"locals\":{\"events\":80}}\n");
}

/* ---- Key ranges -----------------------------------------------------------
 * Folds, a filter and walks whose tests bound a keyed list's key read only
 * the items in bounds; each answers as the same module reading every item:
 * two bounds, one, an equality, a bound on the right, a test that may fault
 * (no range), a walk that changes and removes items in its range, and a
 * walk whose body moves its own bound (no range). */
enum { R_LO = 0, R_HI = 1, R_N = 2, R_L = 3 };
static const S range_numbers[] = {{"lo", 0}, {"hi", 0}, {"n", 0}};
static const T range_lists[] = {{"l", 3 | 1 << 8, 0}};
static const RF range_fields[] = {{"key", 0, 0}, {"a", 0, 0}, {"b", 0, 0}};
static const R range_records[] = {{"K", range_fields, 3}};
static const F bounds_fields[] = {{"lo", 3}, {"hi", 3}};
static const I range_inputs[] = {{"bounds", NULL, PURE_KIND_MESSAGE, bounds_fields, 2}, KEY("walk", "KeyW"),
                                 KEY("drift", "KeyD"), KEY("slice", "KeyS")};
#define KEYED(field) {IG, ITEM(R_L, field), 0}
static const O range_code[] = {
    /* 0 v0 = count(x in l where x.key >= lo && x.key < hi) */
    {FOLD, FOLDING(PURE_FOLD_COUNT, R_L, 9), 0}, {ROW, R_L, 0}, KEYED(0), {L, R_LO, 0}, {PURE_OP_GE, 0, 0},
    {ROW, R_L, 0}, KEYED(0), {L, R_HI, 0}, {PURE_OP_LT, 0, 0}, {PURE_OP_AND, 0, 0},
    /* 10 v1 = sum(x.a for x in l where x.key > lo && x.key <= hi && x.b > 0) */
    {FOLD, FOLDING(PURE_FOLD_SUM, R_L, 16), 0}, {ROW, R_L, 0}, KEYED(1),
    {ROW, R_L, 0}, KEYED(0), {L, R_LO, 0}, {GT, 0, 0}, {ROW, R_L, 0}, KEYED(0), {L, R_HI, 0}, {PURE_OP_LE, 0, 0},
    {PURE_OP_AND, 0, 0}, {ROW, R_L, 0}, KEYED(2), {C, 0, 0}, {GT, 0, 0}, {PURE_OP_AND, 0, 0},
    /* 27 v2 = first(x in l where x.key == lo).position */
    {FOLD, FOLDING(PURE_FOLD_FIRST, R_L, 4), 0}, {ROW, R_L, 0}, KEYED(0), {L, R_LO, 0}, {EQ, 0, 0}, {XN, R_L, 0},
    /* 33 v3 = min(x.b for x in l where hi > x.key && x.key >= lo) */
    {FOLD, FOLDING(PURE_FOLD_MIN, R_L, 11), 0}, {ROW, R_L, 0}, KEYED(2),
    {L, R_HI, 0}, {ROW, R_L, 0}, KEYED(0), {GT, 0, 0}, {ROW, R_L, 0}, KEYED(0), {L, R_LO, 0}, {PURE_OP_GE, 0, 0},
    {PURE_OP_AND, 0, 0},
    /* 45 v4 = count(x in l where x.key >= lo && x.a / x.b > 0): a test that may fault */
    {FOLD, FOLDING(PURE_FOLD_COUNT, R_L, 12), 0}, {ROW, R_L, 0}, KEYED(0), {L, R_LO, 0}, {PURE_OP_GE, 0, 0},
    {ROW, R_L, 0}, KEYED(1), {ROW, R_L, 0}, KEYED(2), {DIV, 0, 0}, {C, 0, 0}, {GT, 0, 0}, {PURE_OP_AND, 0, 0},
    /* 58 table t: each x in l where x.key >= lo && x.key < hi && x.a > 1 */
    {ROW, R_L, 0}, KEYED(0), {L, R_LO, 0}, {PURE_OP_GE, 0, 0}, {ROW, R_L, 0}, KEYED(0), {L, R_HI, 0}, {PURE_OP_LT, 0, 0},
    {PURE_OP_AND, 0, 0}, {ROW, R_L, 0}, KEYED(1), {C, 0, 1}, {GT, 0, 0}, {PURE_OP_AND, 0, 0},
    /* 72 column c = x.b */
    {ROW, R_L, 0}, KEYED(2),
    /* 74 on bounds: lo = bounds.lo; hi = bounds.hi */
    {FIELD, 0, 0}, {ST, R_LO, 0}, {FIELD, 1, 0}, {ST, R_HI, 0},
    /* 78 on walk: for x in l where x.key >= lo && x.key < hi { x.a = x.a + 1; if x.b < 0 { l.remove(x) } } */
    {EACH, WALKING(R_L, 23), 0},
    {ROW, R_L, 0}, KEYED(0), {L, R_LO, 0}, {PURE_OP_GE, 0, 0}, {ROW, R_L, 0}, KEYED(0), {L, R_HI, 0}, {PURE_OP_LT, 0, 0},
    {PURE_OP_AND, 0, 0}, {JZ, 102, 0},
    {ROW, R_L, 0}, {ROW, R_L, 0}, KEYED(1), {C, 0, 1}, {ADD, 0, 0}, {IS, ITEM(R_L, 1), 0},
    {ROW, R_L, 0}, KEYED(2), {C, 0, 0}, {PURE_OP_LT, 0, 0}, {JZ, 102, 0}, {ROW, R_L, 0}, {LR, R_L, 0},
    /* 102 on drift: for x in l where x.key < hi { hi = hi - 1; n = n + 1 }: its bound moves */
    {EACH, WALKING(R_L, 13), 0},
    {ROW, R_L, 0}, KEYED(0), {L, R_HI, 0}, {PURE_OP_LT, 0, 0}, {JZ, 116, 0},
    {L, R_HI, 0}, {C, 0, 1}, {PURE_OP_SUB, 0, 0}, {ST, R_HI, 0}, {L, R_N, 0}, {C, 0, 1}, {ADD, 0, 0}, {ST, R_N, 0},
    /* 116 v5 = count(x in l[lo .. hi] where x.a / x.b > 0): a slice, whose items alone may fault */
    {L, R_LO, 0}, {L, R_HI, 0}, {FOLD, FOLDING(PURE_FOLD_COUNT | PURE_FOLD_SLICED, R_L, 7), 0}, {ROW, R_L, 0}, KEYED(1),
    {ROW, R_L, 0}, KEYED(2), {DIV, 0, 0}, {C, 0, 0}, {GT, 0, 0},
    /* 126 on slice: for x in l[lo .. hi] { x.b = x.b * 2 } */
    {L, R_LO, 0}, {L, R_HI, 0}, {EACH, WALKING(R_L, 6) | PURE_EACH_SLICED, 0}, {ROW, R_L, 0}, {ROW, R_L, 0}, KEYED(2),
    {C, 0, 2}, {MUL, 0, 0}, {IS, ITEM(R_L, 2), 0},
};
#undef KEYED
static const V range_views[] = {{"v0", 0, 10}, {"v1", 10, 17}, {"v2", 27, 6}, {"v3", 33, 12}, {"v4", 45, 13},
                                 {"v5", 116, 10}};
static const W range_tables[] = {{"t", 2 | (R_L + 1) << 8, 0, 58, 14}, {"c", 0, 7, 72, 2}};
static const H range_handlers[] = {{1, 74, 4}, {3, 78, 24}, {5, 102, 14}, {7, 126, 9}};

static bool same_lists(const PureApp *a, const PureApp *b) {
    const PureList *x = a->lists[R_L], *y = b->lists[R_L];
    if (pure_list_count(x) != pure_list_count(y)) return false;
    for (size_t i = 0; i < pure_list_count(x); ++i)
        for (unsigned f = 0; f < 3; ++f)
            if (pure_list_get(x, i, f).num != pure_list_get(y, i, f).num) return false;
    return true;
}

static void ranges(void) {
    Spec spec = {range_numbers, 3, range_inputs, 4, range_views, 6, range_handlers, 4, range_code, LEN(range_code), NULL, 0,
                 range_lists, 1, NULL, 0, NULL, 0, range_records, 1, range_tables, 2};
    CHECK(load(spec, &module_a) == PURE_OK && load(spec, &module_b) == PURE_OK);
    CHECK(module_a.code[0].range && module_a.code[10].range && module_a.code[27].range && module_a.code[33].range &&
          !module_a.code[45].range && module_a.view_range[6] && module_a.code[78].range && !module_a.code[102].range &&
          !module_a.code[118].range && module_a.code[118].plan);
    for (uint32_t i = 0; i < module_b.code_count; ++i) module_b.code[i].range = 0;
    memset(module_b.view_range, 0, module_b.views * sizeof(*module_b.view_range));
    pure_jit_free(module_b.jit); /* its code changed: compiled again (021) */
    module_b.jit = pure_jit_make(&module_b);
    uint32_t seed = 0x7a45e11u;
    unsigned compared = 0, walked = 0;
    for (unsigned round = 0; round < 60; ++round) {
        PureApp a, b;
        CHECK(pure_app_start(&a, &module_a, (PureData){0}) == PURE_OK && pure_app_start(&b, &module_b, (PureData){0}) == PURE_OK);
        double key = (double)(next_random(&seed) % 5);
        for (size_t i = 0, items = next_random(&seed) % 400; i < items; ++i) {
            key += 1 + next_random(&seed) % 3;
            PureCell cells[3] = {{.num = key}, {.num = (double)(next_random(&seed) % 9) - 3},
                                 {.num = (double)(next_random(&seed) % 7) - 3}};
            CHECK(pure_list_insert(a.lists[R_L], i, cells, NULL) == PURE_OK && pure_list_insert(b.lists[R_L], i, cells, NULL) == PURE_OK);
        }
        for (unsigned step = 0; step < 12; ++step) {
            unsigned char payload[16];
            double lo = (double)(next_random(&seed) % 900) - 50, hi = lo + (double)(next_random(&seed) % 400) - 20;
            put_double(payload, lo);
            put_double(payload + 8, hi);
            CHECK(pure_app_message(&a, 0, (PureSpan){payload, 16}) == PURE_OK &&
                  pure_app_message(&b, 0, (PureSpan){payload, 16}) == PURE_OK);
            /* The slice: its items alone, a division by zero among them failing it. */
            size_t sliced = 0;
            bool divided = true;
            for (size_t i = 0; i < pure_list_count(a.lists[R_L]); ++i) {
                double k = pure_list_get(a.lists[R_L], i, 0).num, x = pure_list_get(a.lists[R_L], i, 1).num,
                       y = pure_list_get(a.lists[R_L], i, 2).num;
                if (k < lo || k >= hi) continue;
                if (y == 0) divided = false;
                else sliced += x / y > 0;
            }
            double shown;
            CHECK(pure_app_view(&a, 5, &shown) == divided && (!divided || shown == (double)sliced));
            for (uint32_t v = 0; v < 6; ++v) {
                double x = 0, y = 0;
                bool ok_a = pure_app_view(&a, v, &x), ok_b = pure_app_view(&b, v, &y);
                if (!same_number(ok_a, x, ok_b, y)) {
                    fprintf(stderr, "range round %u view %u: %d %g / %d %g\n", round, v, ok_a, x, ok_b, y);
                    abort();
                }
                ++compared;
            }
            size_t rows = pure_app_rows(&a, 6);
            CHECK(rows == pure_app_rows(&b, 6));
            for (size_t row = 0; row < rows; ++row) {
                double x, y;
                CHECK(pure_app_cell(&a, 7, row, &x) && pure_app_cell(&b, 7, row, &y) && x == y);
            }
            unsigned input = 1 + next_random(&seed) % 3;
            PureResult went_a = press(&a, input), went_b = press(&b, input);
            CHECK(went_a == went_b && same_lists(&a, &b) && !memcmp(a.values, b.values, 3 * sizeof(double)));
            walked += input == 1;
        }
        pure_app_stop(&a);
        pure_app_stop(&b);
    }
    printf("{\"ranges\":{\"compared\":%u,\"walks\":%u}}\n", compared, walked);
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
            if (module_b.input[input].binding == PURE_BIND_TEXT) type_in(&app, input, (PureSpan){(const unsigned char *)"ab", 2});
            else if (module_b.input[input].binding == PURE_BIND_PRESS) press(&app, input);
            for (uint32_t v = 0; v < module_b.views; ++v) {
                double value;
                size_t item;
                if (module_b.view_type[v] == PURE_VIEW_LIST) {
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

/* A host points at a row (027's 4d): a message bound to the table (one u32 field, the row's item), whose handler moves
 * the index there; saved like any other event, and deferred, it joins the burst. A pointing names a table. */
static void pointing(void) {
    enum { XAT_ = PURE_OP_INDEX_AT };
    static const S nums[] = {{"made", 0}};
    static const T states[] = {{"tasks", 3, 0}, {"at", 4, 1}};
    static const RF task_fields[] = {{"title", 1, 0}};
    static const R records[] = {{"task", task_fields, 1}};
    static const char *const constants[] = {""};
    static const I inputs[] = {{"typed", NULL, TEXT_INPUT, NULL, 0}, {"rows.point", "1", POINT_INPUT, NULL, 0}};
    static const O code[] = {
        {TI, 0, 0}, {LA, 1, 0}, {XE, 1, 0}, {XS, 2, 0},     /* 0 typed: a task, at it */
        {FIELD, 0, 0}, {XAT_, 1, 0}, {XS, 2, 0},            /* 4 rows.point: at the row's item */
        {XL, 2, 0}, {XN, 1, 0},                             /* 7 view position */
        {ROW, 1, 0}, {IG, 1, 0},                            /* 9 column title */
    };
    static const H handlers[] = {{1, 0, 4}, {3, 4, 3}};
    static const V views[] = {{"position", 7, 2}};
    static const W tables[] = {{"rows", 2, 0, 1, 0}, {"title", 1, 2, 9, 2}};
    Spec spec = {nums, 1, inputs, LEN(inputs), views, 1, handlers, LEN(handlers), code, LEN(code), NULL, 0, states, 2,
                 constants, 1, NULL, 0, records, 1, tables, 2};
    if (load(spec, &module_a) != PURE_OK) { fprintf(stderr, "pointing: %s\n", error); abort(); }
    char path[] = "/tmp/pure-point-XXXXXX";
    CHECK(mkdtemp(path) && pure_store_init(path) == PURE_OK);
    PureApp app;
    unsigned char row[4];
    CHECK(pure_app_start(&app, &module_a, pure_tree_data(path)) == PURE_OK);
    CHECK(pure_app_message(&app, 1, (PureSpan){put32(row, 0) - 4, 4}) == PURE_NOT_FOUND); /* nothing to point at */
    for (int i = 0; i < 3; ++i) CHECK(type_in(&app, 0, (PureSpan){(const unsigned char *)"a", 1}) == PURE_OK);
    uint64_t events = app.events;
    CHECK(app.values[2].num == 2 && pure_app_message(&app, 1, (PureSpan){put32(row, 0) - 4, 4}) == PURE_OK);
    CHECK(app.values[2].num == 0 && app.events == events + 1);
    CHECK(pure_app_message(&app, 1, (PureSpan){put32(row, 3) - 4, 4}) == PURE_NOT_FOUND && app.values[2].num == 0);
    CHECK(pure_app_message(&app, 1, (PureSpan){put32(row, 1) - 4, 4}) == PURE_OK);
    pure_app_stop(&app);
    CHECK(pure_app_start(&app, &module_a, pure_tree_data(path)) == PURE_OK && app.values[2].num == 1);
    CHECK(pure_app_defer(&app, true) && pure_app_message(&app, 1, (PureSpan){put32(row, 2) - 4, 4}) == PURE_OK && app.open &&
          app.values[2].num == 2);
    CHECK(pure_app_save(&app) == PURE_OK);
    pure_app_stop(&app);
    CHECK(pure_app_start(&app, &module_a, pure_tree_data(path)) == PURE_OK && app.values[2].num == 2);
    pure_app_stop(&app);
    remove_store(path);
    static const W columns_only[] = {{"rows", 1, 0, 9, 2}}; /* no table: a pointing has none to point at */
    Spec bad = spec;
    bad.w = columns_only, bad.nw = 1;
    CHECK(load(bad, &module_b) != PURE_OK && strstr(error, "pointing's table"));
}

/* Keyed lists: items in key order (texts by bytes, numbers by value), each
 * key once; found by key; a list that becomes keyed is put in order with
 * its indices following their items, or refused when two items share a key. */
enum { FIND = PURE_OP_LIST_FIND, HAS = PURE_OP_LIST_HAS, TCMP = PURE_OP_TEXT_COMPARE,
       PEOPLE = 1, K_AT = 2, SCORES = 3, K_FOUND = 4, K_NAME = 0, K_VISITS = 1u << 16 };
static const S keyed_nums[] = {{"n", 0}};
static const T keyed_states[] = {{"people", 3 | 1 << 8, 0}, {"at", 4, 1}, {"scores", 3 | 1 << 8, 1}, {"found", 5, 0}};
static const T unkeyed_states[] = {{"people", 3, 0}, {"at", 4, 1}, {"scores", 3, 1}, {"found", 5, 0}};
static const RF person_fields[] = {{"name", 1, 0}, {"visits", 0, 0}};
static const RF score_fields[] = {{"score", 0, 0}}, flag_fields[] = {{"on", 2, 0}};
static const R keyed_records[] = {{"person", person_fields, 2}, {"score", score_fields, 1}, {"flag", flag_fields, 1}};
static const char *const keyed_constants[] = {"", "bob", "m"};
static const I keyed_inputs[] = {
    {"typed", NULL, TEXT_INPUT, NULL, 0}, {"seek", NULL, TEXT_INPUT, NULL, 0},
    {"has", NULL, TEXT_INPUT, NULL, 0}, KEY("drop", "Delete"), KEY("bump", "KeyB"),
    KEY("two", "KeyN"), KEY("zero", "KeyO"), KEY("minus_zero", "KeyM"), {"twice", NULL, TEXT_INPUT, NULL, 0},
};
static const O keyed_code[] = {
    {TI, 0, 0}, {C, 0, 0}, {LA, PEOPLE, 0}, {TI, 0, 0}, {FIND, PEOPLE, 0}, {XS, K_AT, 0},        /* 0 typed: add, point at it */
    {TI, 0, 0}, {FIND, PEOPLE, 0}, {XS, K_AT, 0},                                               /* 6 seek */
    {TI, 0, 0}, {HAS, PEOPLE, 0}, {ST, K_FOUND, 0},                                             /* 9 has */
    {XL, K_AT, 0}, {LR, PEOPLE, 0},                                                             /* 12 drop */
    {XL, K_AT, 0}, {XL, K_AT, 0}, {IG, PEOPLE | K_VISITS, 0}, {C, 0, 1}, {ADD, 0, 0},            /* 14 bump */
    {IS, PEOPLE | K_VISITS, 0},
    {C, 0, 2}, {LA, SCORES, 0}, {C, 0, 0}, {LA, SCORES, 0}, {C, 0, -0.0}, {LA, SCORES, 0},       /* 20 two, zero, minus_zero */
    {TI, 0, 0}, {C, 0, 0}, {LA, PEOPLE, 0}, {TI, 0, 0}, {C, 0, 0}, {LA, PEOPLE, 0},              /* 26 twice: always exists */
    {LC, PEOPLE, 0},                                                                            /* 32 view count */
    {XL, K_AT, 0}, {XN, PEOPLE, 0},                                                             /* 33 view position */
    {XL, K_AT, 0}, {IG, PEOPLE | K_NAME, 0}, {TK, 1, 0}, {TCMP, PURE_EQUAL, 0},                  /* 35 view is_bob */
    {XL, K_AT, 0}, {IG, PEOPLE | K_NAME, 0}, {TK, 2, 0}, {TCMP, PURE_LESS, 0},                   /* 39 view before_m */
    {LC, SCORES, 0},                                                                            /* 43 view scores */
    {XL, K_AT, 0}, {IG, PEOPLE | K_NAME, 0},                                                    /* 44 view current */
};
static const H keyed_handlers[] = {{1, 0, 6}, {3, 6, 3}, {5, 9, 3}, {7, 12, 2}, {9, 14, 6}, {11, 20, 2}, {13, 22, 2},
                                   {15, 24, 2}, {17, 26, 6}};
static const V keyed_views[] = {{"count", 32, 1}, {"position", 33, 2}, {"is_bob", 35, 4 | BOOL_VIEW}, {"before_m", 39, 4 | BOOL_VIEW},
                                {"scores_count", 43, 1}};
static const V keyed_text_views[] = {{"current", 44, 2}};
/* Unkeyed, the same states: typed appends and points at the new last item. */
static const O unkeyed_code[] = {{TI, 0, 0}, {C, 0, 0}, {LA, PEOPLE, 0}, {XE, PEOPLE, 0}, {XS, K_AT, 0}};
static const H unkeyed_handlers[] = {{1, 0, 5}};

static Spec keyed_spec(const T *states, const O *code, size_t count, const H *handlers, size_t nhandlers) {
    bool views = code == keyed_code;
    return (Spec){keyed_nums, 1, keyed_inputs, LEN(keyed_inputs), keyed_views, views ? LEN(keyed_views) : 0, handlers,
                  nhandlers, code, count, NULL, 0, states, LEN(keyed_states), keyed_constants, LEN(keyed_constants),
                  keyed_text_views, views ? 1 : 0, keyed_records, LEN(keyed_records), NULL, 0};
}

static PureResult type_text(PureApp *app, uint32_t input, const char *text) {
    return type_in(app, input, (PureSpan){(const unsigned char *)text, strlen(text)});
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
    CHECK(!strcmp(names_of(&app, PEOPLE), "a,b,m,z") && app.values[K_AT].num == 0 && !strcmp(view_text(&app, 5), "a"));
    /* A key once: the add fails, and so does an event whose second add repeats its first. */
    CHECK(type_text(&app, 0, "b") == PURE_CONFLICT && app.fault == PURE_FAULT_EXISTS);
    CHECK(type_text(&app, 8, "c") == PURE_CONFLICT && !strcmp(names_of(&app, PEOPLE), "a,b,m,z") && app.values[K_AT].num == 0);
    CHECK(type_text(&app, 1, "z") == PURE_OK && app.values[K_AT].num == 3 && !strcmp(view_text(&app, 5), "z"));
    CHECK(type_text(&app, 1, "q") == PURE_NOT_FOUND && app.fault == PURE_FAULT_NOT_FOUND && app.values[K_AT].num == 3);
    CHECK(type_text(&app, 2, "m") == PURE_OK && app.values[K_FOUND].num == 1);
    CHECK(type_text(&app, 2, "c") == PURE_OK && app.values[K_FOUND].num == 0);
    CHECK(press(&app, 4) == PURE_OK && pure_list_get(app.lists[PEOPLE], 3, 1).num == 1);
    CHECK(press(&app, 3) == PURE_OK && !strcmp(names_of(&app, PEOPLE), "a,b,m"));
    CHECK(app.values[K_AT].num == 2 && number_view(&app, 2) == 0 && number_view(&app, 3) == 0);  /* "m": not bob, not before m */
    CHECK(type_text(&app, 1, "b") == PURE_OK && number_view(&app, 2) == 0 && number_view(&app, 3) == 1);
    CHECK(type_text(&app, 0, "bob") == PURE_OK && number_view(&app, 2) == 1 && number_view(&app, 3) == 1);
    /* Bytes order texts: a text before the longer ones it begins, UTF-8 after ASCII, the empty text first. */
    CHECK(type_text(&app, 0, "\xea\xb0\x80") == PURE_OK && type_text(&app, 0, "ab") == PURE_OK);
    CHECK(type_text(&app, 0, "") == PURE_OK && type_text(&app, 0, "") == PURE_CONFLICT);
    CHECK(!strcmp(names_of(&app, PEOPLE), ",a,ab,b,bob,m,\xea\xb0\x80") && app.values[K_AT].num == 0);
    /* Numbers order by value, and -0 is the key 0. */
    CHECK(press(&app, 5) == PURE_OK && press(&app, 6) == PURE_OK);
    CHECK(press(&app, 7) == PURE_CONFLICT && number_view(&app, 4) == 2);
    CHECK(pure_list_get(app.lists[SCORES], 0, 0).num == 0 && !signbit(pure_list_get(app.lists[SCORES], 0, 0).num) &&
          pure_list_get(app.lists[SCORES], 1, 0).num == 2);

    /* Replacing: unkeyed, the list stays as it is and takes appends at its end. */
    CHECK(load(keyed_spec(unkeyed_states, unkeyed_code, LEN(unkeyed_code), unkeyed_handlers, 1), &module_b) == PURE_OK);
    CHECK(pure_app_replace(&app, &module_b) == PURE_OK && type_text(&app, 0, "c") == PURE_OK);
    CHECK(!strcmp(names_of(&app, PEOPLE), ",a,ab,b,bob,m,\xea\xb0\x80,c") && app.values[K_AT].num == 7);
    CHECK(place(&app, K_AT, 6) == PURE_OK);                     /* at the Korean name */
    /* Keyed again: put in order, the index following its item. */
    CHECK(pure_app_replace(&app, &module_a) == PURE_OK && !strcmp(names_of(&app, PEOPLE), ",a,ab,b,bob,c,m,\xea\xb0\x80"));
    CHECK(app.values[K_AT].num == 7 && !strcmp(view_text(&app, 5), "\xea\xb0\x80"));
    /* Two items with one key cannot become keyed: the app keeps its module and items. */
    CHECK(pure_app_replace(&app, &module_b) == PURE_OK && type_text(&app, 0, "a") == PURE_OK);
    CHECK(pure_app_replace(&app, &module_a) == PURE_CONFLICT && app.module == &module_b && pure_list_count(app.lists[PEOPLE]) == 9);
    pure_app_stop(&app);

    /* Stored: a list that becomes keyed is stored in order, once. */
    char *path = new_store();
    CHECK(pure_app_start(&app, &module_b, pure_tree_data(path)) == PURE_OK);
    for (const char *const *word = (const char *const[]){"m", "b", "z", NULL}; *word; ++word)
        CHECK(type_text(&app, 0, *word) == PURE_OK);
    CHECK(place(&app, K_AT, 1) == PURE_OK);
    pure_app_stop(&app);
    for (int round = 0; round < 2; ++round) {
        CHECK(pure_app_start(&app, &module_a, pure_tree_data(path)) == PURE_OK);
        CHECK(!strcmp(names_of(&app, PEOPLE), "b,m,z") && app.values[K_AT].num == 0);
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
    static const O relation[] = {{TI, 0, 0}, {TI, 0, 0}, {TCMP, PURE_NOT_EQUAL + 1, 0}, {ST, K_FOUND, 0}};
    static const O compare_numbers[] = {{C, 0, 1}, {C, 0, 1}, {TCMP, PURE_EQUAL, 0}, {ST, K_FOUND, 0}};
    static const O has_number[] = {{TI, 0, 0}, {HAS, PEOPLE, 0}, {ST, 0, 0}}; /* a bool is not a number */
    static const T far_key[] = {{"people", 3 | 3 << 8, 0}, {"at", 4, 1}, {"scores", 3, 1}, {"found", 5, 0}};
    static const T keyed_index[] = {{"people", 3, 0}, {"at", 4 | 1 << 8, 1}, {"scores", 3, 1}, {"found", 5, 0}};
    static const T bool_key[] = {{"people", 3, 0}, {"at", 4, 1}, {"scores", 3 | 1 << 8, 2}, {"found", 5, 0}};
    static const struct { const T *states; const O *code; size_t count; const char *reason; } refused[] = {
        {keyed_states, insert, LEN(insert), "insert by position into a keyed list"},
        {keyed_states, set_key, LEN(set_key), "change of a keyed list's key"},
        {keyed_states, find_number, LEN(find_number), "operand type"},
        {unkeyed_states, find, LEN(find), "no such state"},
        {keyed_states, relation, LEN(relation), "no such state"},
        {keyed_states, compare_numbers, LEN(compare_numbers), "operand type"},
        {keyed_states, has_number, LEN(has_number), "operand type"},
        {bool_key, unkeyed_code, LEN(unkeyed_code), "state reference"},  /* a bool is no key */
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
            if (module_b.input[input].binding == PURE_BIND_TEXT) type_text(&app, input, words[next_random(&state) % LEN(words)]);
            else if (module_b.input[input].binding == PURE_BIND_PRESS) press(&app, input);
            for (uint32_t v = 0; v < module_b.views; ++v) {
                double value;
                if (module_b.view_type[v] == PURE_VIEW_TEXT) pure_app_view_text(&app, v, ignore_span, NULL);
                else pure_app_view(&app, v, &value);
            }
            for (uint32_t s = 0; s < module_b.states; ++s) {
                if (module_b.state_type[s] == PURE_STATE_INDEX) {
                    size_t count = pure_list_count(app.lists[module_b.state_ref[s]]);
                    CHECK(app.values[s].num == floor(app.values[s].num) && app.values[s].num >= 0 &&
                          app.values[s].num <= (count ? (double)(count - 1) : 0));
                }
                if (module_b.state_type[s] != PURE_STATE_LIST || !module_b.state_key[s]) continue;
                unsigned key = module_b.state_key[s] - 1u;
                bool text = module_b.record_field_type[module_b.record[module_b.state_ref[s]].first + key] == PURE_RECORD_TEXT;
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
                    if (m->record_field_type[r->first + f] == PURE_RECORD_TEXT) show_span(s, (PureSpan){cell.text, cell.size});
                    else show(s, "%g", cell.num);
                    show(s, f + 1 < r->count ? "," : ";", 0);
                }
        } else show(s, "%g", app->values[i].num);
        show(s, "|", 0);
    }
    for (uint32_t v = 0; v < m->views; ++v) {
        double value;
        if (m->view_table[v] || m->view_type[v] == PURE_VIEW_LIST) continue;
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
    if (app->module->input[input].binding == PURE_BIND_TEXT) type_text(app, input, names[next_random(seed) % LEN(names)]);
    else press(app, input);
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
        /* Its lists' changes fail before the log has them, the rest before the tree does. */
        fail_stage = tries % 2 ? PURE_AFTER_HEADER : PURE_LOG_BEFORE_WRITE;
        saved = pure_app_save(&app); /* OK when the event changed nothing the stage stores */
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

/* A snapshot held while the app goes on for long keeps the states as they
 * were then, and nothing the app makes and lets go after it: holding one
 * costs the states of its time, however many events and snapshots follow
 * (it held them all before, each snapshot holding the next). */
static size_t snapshot_held_growth;

static void snapshot_held(Spec spec, void (*event)(PureApp *, uint32_t *)) {
    static char was[1 << 16], shown_text[1 << 16];
    char *path = new_store();
    PureStore *store;
    CHECK(load(spec, &module_a) == PURE_OK && pure_store_open(path, &store) == PURE_OK);
    PureApp app, reader = {0};
    CHECK(pure_app_start(&app, &module_a, pure_store_data(store)) == PURE_OK);
    uint32_t seed = 0x401du;
    for (unsigned step = 0; step < 200; ++step) event(&app, &seed);
    PureSnapshot *held = pure_app_snapshot(&app);
    CHECK(held);
    Shown before = {was, 0, sizeof(was)}, shown = {shown_text, 0, sizeof(shown_text)};
    describe(&app, &before);
#ifdef __GLIBC__
    size_t used[3] = {0, 0, 0};
#endif
    for (unsigned half = 0; half < 2; ++half) {
#ifdef __GLIBC__
        used[half] = mallinfo2().uordblks;
#endif
        for (unsigned step = 0; step < 20000; ++step) {
            event(&app, &seed);
            pure_snapshot_release(pure_app_snapshot(&app));
        }
    }
#ifdef __GLIBC__
    used[2] = mallinfo2().uordblks;
    /* The first half may make the app's own copies of what the held one shares; the second makes none to keep. */
    snapshot_held_growth = used[2] > used[1] ? used[2] - used[1] : 0;
    CHECK(snapshot_held_growth < 256 * 1024);
#endif
    pure_app_read(&reader, held);
    describe(&reader, &shown);
    CHECK(!strcmp(before.text, shown.text));
    pure_snapshot_release(held);
    pure_app_stop(&reader);
    pure_app_stop(&app);
    pure_store_close(store);
    remove_store(path);
}

static void crash_event(PureApp *app, uint32_t *seed) {
    (void)seed;
    CHECK(press(app, 0) == PURE_OK);
}

static void snapshots(void) {
    Spec keyed = keyed_spec(keyed_states, keyed_code, LEN(keyed_code), keyed_handlers, LEN(keyed_handlers));
    Spec crashing = {crash_nums, 1, crash_inputs, 1, NULL, 0, crash_handlers, 1, crash_code, LEN(crash_code), NULL, 0,
                     crash_states, 4, crash_constants, 2, NULL, 0, crash_records, 1, NULL, 0};
    Spec unkeyed = keyed_spec(unkeyed_states, unkeyed_code, LEN(unkeyed_code), unkeyed_handlers, 1);
    snapshots_kept(keyed, &unkeyed, keyed_event, 3000);
    snapshots_kept(crashing, NULL, crash_event, 300);
    snapshots_read(keyed, keyed_event, 3000);
    snapshot_held(keyed, keyed_event);
    snapshots_read(crashing, crash_event, 300);
    /* Without stored states (texts only in memory) there is no snapshot. */
    CHECK(load(crashing, &module_a) == PURE_OK);
    PureApp app;
    CHECK(pure_app_start(&app, &module_a, (PureData){0}) == PURE_OK && !pure_app_snapshot(&app));
    pure_app_stop(&app);
    CHECK(snapshot_retired > 100);
    printf("{\"snapshots\":6600,\"items_retired\":%zu,\"reads_on_threads\":%zu,\"held_growth_bytes\":%zu}\n",
           snapshot_retired, snapshot_reads, snapshot_held_growth);
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
    static const I inputs[] = {{"post", NULL, PURE_KIND_MESSAGE, fields, 4}};
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
    CHECK(pure_app_message(&app, 0, (PureSpan){payload, size}) == PURE_OK);
    CHECK(app.values[0].num == 5.5 && !strcmp(state_text(&app, 1), "\xea\xb0\x80\xeb\x82\x98") && replied(&app, "xyz"));
    pure_app_finish(&app);
    /* Empty variable fields. */
    title_size = 0;
    memcpy(payload, &title_size, 4);
    CHECK(pure_app_message(&app, 0, (PureSpan){payload, 16}) == PURE_OK && !*state_text(&app, 1) && replied(&app, ""));
    pure_app_finish(&app);
    /* A title longer than what is there, a title that is not UTF-8, a number that is not finite, a short prefix. */
    title_size = 10;
    memcpy(payload, &title_size, 4);
    CHECK(pure_app_message(&app, 0, (PureSpan){payload, 25}) == PURE_LIMIT && app.fault == PURE_FAULT_LIMIT);
    title_size = 2;
    memcpy(payload, &title_size, 4);
    memcpy(payload + 16, "\xff\xfe", 2);
    CHECK(pure_app_message(&app, 0, (PureSpan){payload, 18}) == PURE_INVALID && app.fault == PURE_FAULT_ENCODING);
    title_size = 0;
    points = NAN;
    memcpy(payload, &title_size, 4);
    memcpy(payload + 4, &points, 8);
    CHECK(pure_app_message(&app, 0, (PureSpan){payload, 16}) == PURE_INVALID && app.fault == PURE_FAULT_NON_FINITE);
    CHECK(pure_app_message(&app, 0, (PureSpan){payload, 15}) == PURE_LIMIT);
    CHECK(app.values[0].num == 5.5 && !*state_text(&app, 1)); /* failed messages changed nothing */
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
    CHECK(pure_app_message_fields(&app, 0, (PureSpan){payload, size}, placed) == PURE_OK);
    CHECK(app.values[0].num == 5.5 && !strcmp(state_text(&app, 1), "\xea\xb0\x80\xeb\x82\x98") && replied(&app, "xyz"));
    pure_app_finish(&app);
    /* A field that starts or ends past the payload, or a title that is not UTF-8 there. */
    placed[0] = (uint32_t)size + 1;
    CHECK(pure_app_message_fields(&app, 0, (PureSpan){payload, size}, placed) == PURE_LIMIT);
    placed[0] = (uint32_t)size - 2;
    placed[1] = 3;
    CHECK(pure_app_message_fields(&app, 0, (PureSpan){payload, size}, placed) == PURE_LIMIT);
    placed[0] = 16 + 24;
    placed[1] = 2;
    CHECK(pure_app_message_fields(&app, 0, (PureSpan){payload, size}, placed) == PURE_INVALID &&
          app.fault == PURE_FAULT_ENCODING);
    CHECK(app.values[0].num == 5.5);
    pure_app_stop(&app);

    /* Fields in groups: a name is a path of identifiers (user.name), and
     * never both a field and a group. */
    static const F grouped[] = {{"user.name", PURE_FIELD_TEXT}, {"user.age", PURE_FIELD_U32}, {"plan", PURE_FIELD_TEXT}};
    static const I grouped_inputs[] = {{"post", NULL, PURE_KIND_MESSAGE, grouped, 3}};
    static const O grouped_code[] = {{FIELD, 1, 0}, {ST, 0, 0}, {FIELD, 0, 0}, {TSET, 1, 0}};
    static const H grouped_handlers[] = {{1, 0, 4}};
    Spec groups = spec;
    groups.in = grouped_inputs; groups.o = grouped_code; groups.no = LEN(grouped_code); groups.h = grouped_handlers;
    CHECK(load(groups, &module_b) == PURE_OK && !strcmp(module_b.field[0].name, "user.name"));
    static const char *const refused_paths[] = {"user..name", ".user", "user.", "user.1st", "user name"};
    for (size_t i = 0; i < LEN(refused_paths); ++i) {
        F renamed[3] = {grouped[0], grouped[1], grouped[2]};
        renamed[0].name = refused_paths[i];
        I renamed_inputs[1] = {{"post", NULL, PURE_KIND_MESSAGE, renamed, 3}};
        groups.in = renamed_inputs;
        CHECK(load(groups, &module_b) == PURE_INVALID && strstr(error, "message field"));
    }
    F both[3] = {{"user", PURE_FIELD_TEXT}, grouped[1], grouped[2]};
    I both_inputs[1] = {{"post", NULL, PURE_KIND_MESSAGE, both, 3}};
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
typedef struct { size_t count; uint64_t first, last; } Tally;

static bool tally_sent(void *context, uint32_t output, uint64_t seq, PureSpan payload) {
    (void)output, (void)payload;
    Tally *t = context;
    if (!t->count++) t->first = seq;
    t->last = seq;
    return true;
}

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
    PureResult result = pure_app_message(app, input, (PureSpan){payload, 4 + length});
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
    PureResult result = pure_app_deliver(app, 0, source, seq, (PureSpan){payload, 12 + length}, NULL);
    pure_app_finish(app);
    return result;
}

static void links(void) {
    static const S sender_nums[] = {{"n", 0}};
    static const F order_fields[] = {{"item", PURE_FIELD_TEXT}, {"count", PURE_FIELD_U32}};
    static const F ordered_fields[] = {{"item", PURE_FIELD_TEXT}, {"count", PURE_FIELD_U32}, {"total", PURE_FIELD_NUMBER}};
    static const I sender_inputs[] = {{"buy", NULL, PURE_KIND_MESSAGE, order_fields, 2},
                                      {"fail", NULL, PURE_KIND_MESSAGE, order_fields, 2},
                                      {"ordered", NULL, PURE_KIND_OUTPUT, ordered_fields, 3}};
    static const O sender_code[] = {{L, 0, 0}, {C, 0, 1}, {ADD, 0, 0}, {ST, 0, 0}, {FIELD, 0, 0}, {FIELD, 1, 0},
                                    {L, 0, 0}, {SEND, 2, 0},                                           /* buy */
                                    {FIELD, 0, 0}, {FIELD, 1, 0}, {C, 0, 0}, {SEND, 2, 0}, {C, 1, 0}, {REQ, 0, 0}}; /* fail */
    static const H sender_handlers[] = {{1, 0, 8}, {3, 8, 6}};
    Spec sender = {sender_nums, 1, sender_inputs, 3, NULL, 0, sender_handlers, 2, sender_code, LEN(sender_code),
                   NULL, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0};
    static const S receiver_nums[] = {{"stock", 0}};
    static const T receiver_texts[] = {{"last", 1, 0}};
    static const char *const receiver_constants[] = {""};
    static const I receiver_inputs[] = {{"order", NULL, PURE_KIND_MESSAGE, ordered_fields, 3}};
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
    CHECK(o.count == 1 && o.items[0].output == 2 && o.items[0].seq == 1 && o.items[0].size == 15 && a.values[0].num == 1);
    PureSpan sent = {o.items[0].payload, o.items[0].size};
    CHECK(pure_app_deliver(&b, 0, "orders.ordered", 1, sent, NULL) == PURE_OK && !b.duplicate && b.values[0].num == 2 &&
          !strcmp(state_text(&b, 1), "pen"));
    pure_app_finish(&b);
    CHECK(pure_app_deliver(&b, 0, "orders.ordered", 1, sent, NULL) == PURE_OK && b.duplicate && b.values[0].num == 2);
    CHECK(deliver(&b, "other", 1, "cup", 5) == PURE_OK && !b.duplicate && b.values[0].num == 7);
    CHECK(deliver(&b, "orders.ordered", 3, "hat", 1) == PURE_OK && b.values[0].num == 8); /* numbers may skip */
    CHECK(deliver(&b, "orders.ordered", 2, "old", 1) == PURE_OK && b.duplicate && b.values[0].num == 8);
    /* Refused (count 100): delivered all the same, never applied again. */
    CHECK(deliver(&b, "other", 2, "big", 100) == PURE_LIMIT && b.values[0].num == 7 + 1);
    CHECK(deliver(&b, "other", 2, "big", 1) == PURE_OK && b.duplicate && b.values[0].num == 8);
    /* Bad deliveries change nothing. */
    CHECK(deliver(&b, "", 4, "x", 1) == PURE_INVALID && deliver(&b, "a b", 4, "x", 1) == PURE_INVALID);
    CHECK(deliver(&b, "other", 0, "x", 1) == PURE_INVALID && pure_app_deliver(&a, 2, "s", 1, sent, NULL) == PURE_INVALID);
    CHECK(pure_app_message(&a, 2, sent) == PURE_INVALID);
    /* Trimmed, a send is gone at once for the host and from the store at the next save. */
    pure_app_trim(&a, 2, 1);
    CHECK(outbox_of(&a).count == 0 && pure_app_save(&a) == PURE_OK && pure_list_count(a.outbox) == 0);
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
    CHECK(o.count == 2 && o.items[0].seq == 1 && o.items[1].seq == 2 && a.values[0].num == 2);
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
    CHECK(o.count == 5 && o.items[0].seq == 1 && a.values[0].num == 5);         /* the trim and the send undone */
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
    CHECK(pure_app_start(&b, &module_b, pure_tree_data(path_b)) == PURE_OK && b.values[0].num == 2);
    CHECK(deliver(&b, "orders.ordered", 1, "pen", 2) == PURE_OK && b.duplicate && b.values[0].num == 2);
    CHECK(deliver(&b, "x", 1, "big", 1) == PURE_OK && b.duplicate);
    /* A save that fails forgets what it heard. */
    CHECK(pure_app_defer(&b, true) && deliver(&b, "orders.ordered", 2, "ink", 3) == PURE_OK && b.values[0].num == 5);
    fail_stage = PURE_AFTER_HEADER;
    CHECK(pure_app_save(&b) == PURE_STORE_ERROR && b.values[0].num == 2);
    fail_stage = -1;
    CHECK(deliver(&b, "orders.ordered", 2, "ink", 3) == PURE_OK && !b.duplicate && b.values[0].num == 5);
    CHECK(pure_app_save(&b) == PURE_OK);
    pure_app_stop(&b);
    /* Links stored before the heard list kept its numbers: a start moves them
     * there (the larger number of a source wins) and stores the links without. */
    PureDraft *stored_links;
    int64_t revision;
    unsigned char old_links[64] = "PLK1";
    size_t at = 5;
    old_links[4] = 0; /* no outputs */
    old_links[at++] = 2, old_links[at++] = 0;
    const char *old_sources[] = {"legacy", "orders.ordered"};
    uint64_t old_numbers[] = {5, 1};
    for (unsigned k = 0; k < 2; ++k) {
        old_links[at++] = (unsigned char)strlen(old_sources[k]);
        memcpy(old_links + at, old_sources[k], strlen(old_sources[k]));
        at += strlen(old_sources[k]);
        for (unsigned i = 0; i < 8; ++i) old_links[at++] = (unsigned char)(old_numbers[k] >> (8 * i));
    }
    CHECK(pure_state_open(path_b, "links", &stored_links) == PURE_OK);
    CHECK(pure_draft_splice(stored_links, 0, pure_draft_size(stored_links), (PureSpan){old_links, at}) == PURE_OK);
    CHECK(pure_draft_commit(stored_links, &revision) == PURE_OK);
    pure_draft_close(stored_links);
    CHECK(pure_app_start(&b, &module_b, pure_tree_data(path_b)) == PURE_OK);
    CHECK(pure_app_heard(&b, "legacy") == 5 && pure_app_heard(&b, "orders.ordered") == 2 && pure_app_heard(&b, "x") == 1);
    CHECK(deliver(&b, "legacy", 5, "old", 1) == PURE_OK && b.duplicate && deliver(&b, "legacy", 6, "new", 1) == PURE_OK && !b.duplicate);
    pure_app_stop(&b);
    CHECK(pure_state_open(path_b, "links", &stored_links) == PURE_OK && !memcmp(read_state(stored_links), "PLK4", 4));
    pure_draft_close(stored_links);
    /* Links stored before the outbox list kept the sends: a start moves them
     * to it; numbering goes on from the output's last number. */
    char path_c[64];
    PureApp c;
    snprintf(path_c, sizeof(path_c), "%s", new_store());
    unsigned char sends[128] = "PLK2";
    at = 5;
    sends[4] = 1; /* one output: ordered {item, count, total} */
    sends[at++] = 7;
    memcpy(sends + at, "ordered", 7);
    at += 7;
    sends[at++] = 3, sends[at++] = PURE_FIELD_TEXT, sends[at++] = PURE_FIELD_U32, sends[at++] = PURE_FIELD_NUMBER;
    for (unsigned i = 0; i < 8; ++i) sends[at++] = i ? 0 : 2; /* its last number */
    for (unsigned k = 1; k <= 2; ++k) {
        sends[at++] = 0;
        for (unsigned i = 0; i < 8; ++i) sends[at++] = i ? 0 : (unsigned char)k;
        for (unsigned i = 0; i < 4; ++i) sends[at++] = i ? 0 : 4;
        memcpy(sends + at, "abcd", 4);
        at += 4;
    }
    CHECK(pure_state_open(path_c, "links", &stored_links) == PURE_OK);
    CHECK(pure_draft_splice(stored_links, 0, pure_draft_size(stored_links), (PureSpan){sends, at}) == PURE_OK);
    CHECK(pure_draft_commit(stored_links, &revision) == PURE_OK);
    pure_draft_close(stored_links);
    CHECK(pure_app_start(&c, &module_a, pure_tree_data(path_c)) == PURE_OK);
    o = outbox_of(&c);
    CHECK(o.count == 2 && o.items[0].output == 2 && o.items[0].seq == 1 && o.items[1].seq == 2 && o.items[1].size == 4 &&
          !memcmp(o.items[1].payload, "abcd", 4));
    CHECK(buy(&c, 0, "next", 1) == PURE_OK && outbox_of(&c).items[2].seq == 3);
    pure_app_stop(&c);
    CHECK(pure_state_open(path_c, "links", &stored_links) == PURE_OK && !memcmp(read_state(stored_links), "PLK4", 4));
    pure_draft_close(stored_links);
    /* A large outbox costs a save only its changes: the links stay small, the
     * sends and trims come back after a start. */
    CHECK(pure_app_start(&c, &module_a, pure_tree_data(path_c)) == PURE_OK && pure_list_count(c.outbox) == 3);
    CHECK(pure_app_defer(&c, true));
    for (unsigned k = 0; k < 20000; ++k) {
        CHECK(buy(&c, 0, "many", 1) == PURE_OK);
        if (k % 1000 == 999) CHECK(pure_app_save(&c) == PURE_OK);
    }
    CHECK(pure_state_open(path_c, "links", &stored_links) == PURE_OK && pure_draft_size(stored_links) < 256);
    pure_draft_close(stored_links);
    pure_app_trim(&c, 2, 15000);
    CHECK(pure_list_count(c.outbox) == 20003 - 15000 && pure_app_save(&c) == PURE_OK);
    pure_app_stop(&c);
    CHECK(pure_app_start(&c, &module_a, pure_tree_data(path_c)) == PURE_OK);
    Tally tally = {0};
    pure_app_outbox(&c, tally_sent, &tally);
    CHECK(tally.count == 5003 && tally.first == 15001 && tally.last == 20003);
    pure_app_stop(&c);
    remove_store(path_c);
    /* Numbers heard from more sources than two bytes count stay heard across
     * a start; a save logs only the numbers it changed, and the heard list's
     * image is written again once they outgrow it (from a snapshot: through a
     * store handle). */
    PureStore *store;
    CHECK(pure_store_open(path_b, &store) == PURE_OK);
    CHECK(pure_app_start(&b, &module_b, pure_store_data(store)) == PURE_OK && pure_app_heard(&b, "legacy") == 6);
    unsigned long checkpoints = pure_save_test_checkpoints;
    for (unsigned k = 0; k < 400; ++k) CHECK(deliver(&b, "s1", 1 + k, "more", 1) == PURE_OK);
    CHECK(pure_save_test_checkpoints > checkpoints && pure_app_heard(&b, "s1") == 400);
    CHECK(pure_app_defer(&b, true));
    char many[16];
    for (unsigned k = 0; k < 70000; ++k) {
        snprintf(many, sizeof(many), "s%u", k);
        CHECK(deliver(&b, many, 400 + k % 3, "one", 1) == PURE_OK && b.duplicate == (k == 1 && k % 3 == 0));
        if (k % 5000 == 4999) CHECK(pure_app_save(&b) == PURE_OK);
    }
    CHECK(pure_app_save(&b) == PURE_OK);
    pure_app_stop(&b);
    CHECK(pure_app_start(&b, &module_b, pure_store_data(store)) == PURE_OK && pure_list_count(b.heard) == 70003);
    CHECK(pure_app_heard(&b, "s0") == 400 && pure_app_heard(&b, "s1") == 401 && pure_app_heard(&b, "s65537") == 402);
    CHECK(deliver(&b, "s65537", 402, "again", 1) == PURE_OK && b.duplicate);
    pure_app_stop(&b);
    pure_store_close(store);

    /* Replacing: a module without the output is refused while sends are left
     * on it; once they are trimmed it replaces, and the output's number is
     * kept for a module that has it again. */
    static const I plain_inputs[] = {{"buy", NULL, PURE_KIND_MESSAGE, order_fields, 2}};
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
        CHECK(o.count == (size_t)a.values[0].num && (!o.count || o.items[o.count - 1].seq == o.count));
        heard = pure_app_heard(&b, "orders.ordered");
        CHECK(b.values[0].num == (double)heard);
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

/* The app's memory budget (docs/design/013-memory-budget.md): an event that
 * would leave the states over it and holding more is refused and changes
 * nothing; one that frees them goes on; sends waiting for their receivers
 * count, so a full outbox refuses more until some are trimmed; a delivery
 * refused so is not heard, and comes again; an answer has its send's room. */
/* ---- Work budgets (docs/design/017) ------------------------------------
 * An event, or one evaluation of a view, runs at most the app's work
 * budget, counted as written as it begins: a block's length, and a fold's or
 * a walk's block length for every item of its list. Past it an event fails
 * with work and changes nothing; a view, a cell or a table's rows have no
 * value, and pure_app_view_fault says why. (lane_folds checks that a planned
 * fold counts what the same fold run item by item counts.) */
static void work_budget(void) {
    CHECK(!strcmp(pure_fault_name(PURE_FAULT_WORK), "work"));
    /* The shop: restock runs 3 instructions; clean 23, and its walk's 22 for each item stocked. */
    CHECK(load(walk_spec(walk_code, LEN(walk_code), walk_inputs, 3, walk_handlers, 3, walk_lists, NULL, 0), &module_a) == PURE_OK);
    PureApp app;
    CHECK(pure_app_start(&app, &module_a, (PureData){0}) == PURE_OK);
    pure_app_work(&app, UINT64_MAX); /* counted, never past it */
    unsigned char payload[8];
    for (uint32_t item = 0; item < 5; ++item) {
        put32(put32(payload, item), 2);
        CHECK(pure_app_message(&app, 2, (PureSpan){payload, 8}) == PURE_OK && app.work == 3);
    }
    pure_app_work(&app, 23 + 22 * 5 - 1);
    CHECK(press(&app, 1) == PURE_LIMIT && app.fault == PURE_FAULT_WORK);
    for (size_t i = 0; i < 5; ++i) CHECK(pure_list_get(app.lists[W_STOCK], i, 1).num == 2); /* nothing changed */
    pure_app_work(&app, 23 + 22 * 5);
    CHECK(press(&app, 1) == PURE_OK && app.work == 23 + 22 * 5);
    for (size_t i = 0; i < 5; ++i) CHECK(pure_list_get(app.lists[W_STOCK], i, 1).num == 3);
    pure_app_work(&app, 2);
    put32(put32(payload, 9), 1);
    CHECK(pure_app_message(&app, 2, (PureSpan){payload, 8}) == PURE_LIMIT && app.fault == PURE_FAULT_WORK);
    CHECK(pure_list_count(app.lists[W_STOCK]) == 5);
    pure_app_work(&app, 0); /* the default */
    CHECK(pure_app_message(&app, 2, (PureSpan){payload, 8}) == PURE_OK && pure_list_count(app.lists[W_STOCK]) == 6);
    pure_app_stop(&app);
    pure_module_release(&module_a);

    /* Views: open is 4 instructions and its fold's 3 for each task. */
    CHECK(load(fold_spec(fold_code, LEN(fold_code), fold_views, LEN(fold_views), fold_handlers, 2), &module_a) == PURE_OK);
    CHECK(pure_app_start(&app, &module_a, (PureData){0}) == PURE_OK);
    pure_app_work(&app, UINT64_MAX);
    for (const char *word = "abc"; *word; ++word)
        CHECK(type_in(&app, 0, (PureSpan){(const unsigned char *)word, 1}) == PURE_OK && app.work == 7);
    double value;
    CHECK(number_view(&app, 0) == 3 && app.work == 4 + 3 * 3);
    pure_app_work(&app, 4 + 3 * 3 - 1);
    CHECK(!pure_app_view(&app, 0, &value) && pure_app_view_fault(&app) == PURE_FAULT_WORK);
    CHECK(app.fault == PURE_FAULT_NONE); /* a view leaves the app's own fault alone */
    pure_app_work(&app, 4 + 3 * 3);
    CHECK(number_view(&app, 0) == 3);
    pure_app_work(&app, 0);
    CHECK(press(&app, 1) == PURE_OK && place(&app, 1, 0) == PURE_OK &&
          press(&app, 1) == PURE_OK && place(&app, 1, 1) == PURE_OK &&
          press(&app, 1) == PURE_OK); /* all done */
    CHECK(!pure_app_view(&app, 4, &value) && pure_app_view_fault(&app) == PURE_FAULT_NOT_FOUND); /* fewest open: none */
    pure_app_stop(&app);
    pure_module_release(&module_a);

    /* A table's filter is a unit of its own: 3 instructions for each task; a cell's column is 2. */
    CHECK(load(filter_spec(filter_code, LEN(filter_code), filter_views, 3), &module_a) == PURE_OK);
    CHECK(pure_app_start(&app, &module_a, (PureData){0}) == PURE_OK);
    for (const char *word = "abcd"; *word; ++word)
        CHECK(type_in(&app, 0, (PureSpan){(const unsigned char *)word, 1}) == PURE_OK);
    pure_app_work(&app, 3 * 4 - 1);
    size_t at;
    CHECK(pure_app_rows(&app, 0) == 0 && pure_app_view_fault(&app) == PURE_FAULT_WORK && !pure_app_row_item(&app, 0, 0, &at));
    CHECK(!pure_app_cell(&app, 2, 0, &value) && pure_app_view_fault(&app) == PURE_FAULT_WORK);
    pure_app_work(&app, 3 * 4); /* the rows refused before are asked again */
    CHECK(pure_app_rows(&app, 0) == 4 && app.work == 3 * 4 && pure_app_view_fault(&app) == PURE_FAULT_NONE);
    CHECK(pure_app_cell(&app, 2, 3, &value) && value == 3 && app.work == 2);
    pure_app_stop(&app);
    pure_module_release(&module_a);
    puts("{\"work_budget\":\"events-views-cells-rows-rolled-back\"}");
}

static void memory_budget(void) {
    enum { ADD, DROP, SEND_KEY, OUT, TAKE, ANSWER };
    static const S nums[] = {{"n", 0}};
    static const T states[] = {{"rows", 3, 0}};
    static const RF fields[] = {{"value", 0, 0}};
    static const R records[] = {{"row", fields, 1}};
    static const F value[] = {{"value", PURE_FIELD_NUMBER}};
    static const I inputs[] = {KEY("add", "KeyA"), KEY("drop", "KeyD"), KEY("send", "KeyS"),
                               {"out", NULL, PURE_KIND_OUTPUT, value, 1},
                               {"take", NULL, PURE_KIND_MESSAGE, value, 1},
                               {"answer", NULL, PURE_KIND_MESSAGE, value, 1}};
    static const O code[] = {
        {L, 0, 0}, {LA, 1, 0},     /* add: rows.append(row { n }) */
        {XF, 1, 0}, {LR, 1, 0},    /* drop: rows.remove(rows.first) */
        {C, 0, 1}, {SEND, 3, 0},   /* send: send out { value: 1 } */
        {FIELD, 0, 0}, {LA, 1, 0}, /* take: rows.append(row { take.value }) */
        {FIELD, 0, 0}, {LA, 1, 0}, {FIELD, 0, 0}, {LA, 1, 0}, {FIELD, 0, 0}, {LA, 1, 0}, /* answer: six rows */
        {FIELD, 0, 0}, {LA, 1, 0}, {FIELD, 0, 0}, {LA, 1, 0}, {FIELD, 0, 0}, {LA, 1, 0},
    };
    static const H handlers[] = {{1 + 2 * ADD, 0, 2}, {1 + 2 * DROP, 2, 2}, {1 + 2 * SEND_KEY, 4, 2}, {1 + 2 * TAKE, 6, 2},
                                 {1 + 2 * ANSWER, 8, 12}};
    Spec spec = {nums, 1, inputs, 6, NULL, 0, handlers, 5, code, LEN(code), NULL, 0, states, 1, NULL, 0, NULL, 0,
                 records, 1, NULL, 0};
    CHECK(load(spec, &module_a) == PURE_OK);
    char *path = new_store();
    PureApp app;
    CHECK(pure_app_start(&app, &module_a, pure_tree_data(path)) == PURE_OK);
    for (int k = 0; k < 3; ++k) CHECK(press(&app, ADD) == PURE_OK);
    size_t three = pure_app_state_bytes(&app);
    CHECK(three == 3 * 8); /* a row's number as it is stored */
    /* Room for one row more: the next is refused, and nothing of it stays. */
    pure_app_memory(&app, three + 8);
    CHECK(press(&app, ADD) == PURE_OK && pure_list_count(app.lists[1]) == 4);
    CHECK(press(&app, ADD) == PURE_BUDGET && app.fault == PURE_FAULT_MEMORY &&
          pure_list_count(app.lists[1]) == 4 && pure_app_state_bytes(&app) == three + 8);
    /* Over the budget (made smaller), what frees goes on and what grows does not. */
    pure_app_memory(&app, 8);
    CHECK(press(&app, ADD) == PURE_BUDGET);
    CHECK(press(&app, DROP) == PURE_OK && pure_list_count(app.lists[1]) == 3);
    /* Sends count while they wait: a full outbox refuses the next until some are trimmed. */
    size_t before = pure_app_state_bytes(&app);
    pure_app_memory(&app, before);
    CHECK(press(&app, SEND_KEY) == PURE_BUDGET && !pure_list_count(app.outbox));
    pure_app_memory(&app, 0);
    CHECK(press(&app, SEND_KEY) == PURE_OK && pure_list_count(app.outbox) == 1);
    size_t one = pure_app_state_bytes(&app) - before;
    pure_app_memory(&app, before + 2 * one);
    CHECK(press(&app, SEND_KEY) == PURE_OK);
    CHECK(press(&app, SEND_KEY) == PURE_BUDGET && pure_list_count(app.outbox) == 2);
    pure_app_trim(&app, OUT, 1);
    CHECK(pure_app_save(&app) == PURE_OK && pure_list_count(app.outbox) == 1);
    CHECK(press(&app, SEND_KEY) == PURE_OK && outbox_of(&app).items[1].seq == 3);
    /* A delivery refused for memory is not heard: given again with room, it is applied once. */
    double taken = 7;
    pure_app_memory(&app, pure_app_state_bytes(&app));
    CHECK(pure_app_deliver(&app, TAKE, "far.out", 1, (PureSpan){(const unsigned char *)&taken, 8}, NULL) == PURE_BUDGET &&
          app.fault == PURE_FAULT_MEMORY && pure_app_heard(&app, "far.out") == 0 && pure_list_count(app.lists[1]) == 3);
    pure_app_finish(&app);
    pure_app_memory(&app, 0);
    CHECK(pure_app_deliver(&app, TAKE, "far.out", 1, (PureSpan){(const unsigned char *)&taken, 8}, NULL) == PURE_OK &&
          !app.duplicate && pure_app_heard(&app, "far.out") == 1 && pure_list_count(app.lists[1]) == 4);
    pure_app_finish(&app);
    CHECK(pure_list_get(app.lists[1], 3, 0).num == 7);
    /* An answer takes its send out of the outbox in its own event: one that
     * keeps more than the send held is refused whole at the budget (the send
     * stays), and goes on with room; one that keeps less goes on at it. */
    size_t full = pure_app_state_bytes(&app);
    CHECK(one < 6 * 8 && one > 8);
    double paid = 9;
    PureSpan nine = {(const unsigned char *)&paid, 8};
    pure_app_memory(&app, full);
    CHECK(pure_app_answer(&app, ANSWER, OUT, "wire.out", 2, nine, NULL) == PURE_BUDGET &&
          app.fault == PURE_FAULT_MEMORY && pure_list_count(app.outbox) == 2 && pure_list_count(app.lists[1]) == 4 &&
          pure_app_heard(&app, "wire.out") == 0 && pure_app_state_bytes(&app) == full);
    pure_app_finish(&app);
    size_t heard = 4 + strlen("wire.out") + 8; /* its source among the numbers heard */
    pure_app_memory(&app, full + 6 * 8 + heard - one);
    CHECK(pure_app_answer(&app, ANSWER, OUT, "wire.out", 2, nine, NULL) == PURE_OK && pure_list_count(app.outbox) == 1 &&
          outbox_of(&app).items[0].seq == 3 && pure_list_count(app.lists[1]) == 10 &&
          pure_app_heard(&app, "wire.out") == 2 && pure_app_state_bytes(&app) == full + 6 * 8 + heard - one);
    pure_app_finish(&app);
    pure_app_memory(&app, pure_app_state_bytes(&app));
    CHECK(pure_app_answer(&app, TAKE, OUT, "wire.out", 3, nine, NULL) == PURE_OK && !pure_list_count(app.outbox) &&
          pure_list_count(app.lists[1]) == 11);
    pure_app_finish(&app);
    CHECK(pure_app_answer(&app, TAKE, TAKE, "wire.out", 4, nine, NULL) == PURE_INVALID); /* not an output */
    pure_app_stop(&app);
    CHECK(pure_app_start(&app, &module_a, pure_tree_data(path)) == PURE_OK && pure_list_count(app.lists[1]) == 11 &&
          !pure_list_count(app.outbox) && pure_app_heard(&app, "far.out") == 1 && pure_app_heard(&app, "wire.out") == 3);
    pure_app_stop(&app);
    remove_store(path);
}

/* A send laid out for some fields, made anew for others by name: values
 * the new types can hold go over (a u32 widened, a number narrowed when it
 * is one; text to bytes, and bytes that are UTF-8 to text), the rest start
 * at 0 or empty. There and back again, nothing changes. */
static void send_recast(void) {
    PureField ordered[3] = {{"item", PURE_FIELD_TEXT, 0, 0}, {"count", PURE_FIELD_U32, 0, 0},
                            {"total", PURE_FIELD_NUMBER, 0, 0}};
    PureField kept[5] = {{"total", PURE_FIELD_NUMBER, 0, 0}, {"item", PURE_FIELD_TEXT, 0, 0},
                         {"count", PURE_FIELD_NUMBER, 0, 0}, {"memo", PURE_FIELD_BYTES, 0, 0},
                         {"extra", PURE_FIELD_U32, 0, 0}};
    CHECK(pure_fields_lay_out(ordered, 3) == 12 && ordered[0].offset == PURE_FIELD_REST);
    CHECK(pure_fields_lay_out(kept, 5) == 24 && kept[1].offset == 8 && kept[3].offset == PURE_FIELD_REST);
    unsigned char payload[15] = {2, 0, 0, 0}; /* count 2, total 7.5, then the item */
    double total = 7.5;
    memcpy(payload + 4, &total, 8);
    memcpy(payload + 12, "pen", 3);
    unsigned char *made, *back;
    size_t size, back_size;
    CHECK(pure_send_recast(ordered, 3, kept, 5, (PureSpan){payload, 15}, &made, &size) == PURE_OK && size == 27);
    double count, first;
    memcpy(&first, made, 8);
    memcpy(&count, made + 12, 8);
    CHECK(first == 7.5 && made[8] == 3 && count == 2 && !memcmp(made + 20, "\0\0\0\0", 4) && !memcmp(made + 24, "pen", 3));
    CHECK(pure_send_recast(kept, 5, ordered, 3, (PureSpan){made, size}, &back, &back_size) == PURE_OK &&
          back_size == 15 && !memcmp(back, payload, 15));
    free(back);
    /* A number that is not a u32, and bytes that are not UTF-8, start empty. */
    PureField narrow[2] = {{"count", PURE_FIELD_U32, 0, 0}, {"memo", PURE_FIELD_TEXT, 0, 0}};
    pure_fields_lay_out(narrow, 2);
    count = 2.5;
    memcpy(made + 12, &count, 8);
    memcpy(made + 24, "p\xffn", 3); /* the memo (the rest) is those three bytes now, the item none */
    made[8] = 0;
    CHECK(pure_send_recast(kept, 5, narrow, 2, (PureSpan){made, 24 + 3}, &back, &back_size) == PURE_OK &&
          back_size == 4 && !memcmp(back, "\0\0\0\0", 4));
    free(back);
    PureField bytes_memo[1] = {{"memo", PURE_FIELD_BYTES, 0, 0}};
    pure_fields_lay_out(bytes_memo, 1);
    CHECK(pure_send_recast(kept, 5, bytes_memo, 1, (PureSpan){made, 24 + 3}, &back, &back_size) == PURE_OK &&
          back_size == 3 && !memcmp(back, "p\xffn", 3));
    free(back);
    /* A payload not laid out by its fields is the store's error. */
    CHECK(pure_send_recast(kept, 5, ordered, 3, (PureSpan){made, 10}, &back, &back_size) == PURE_STORE_ERROR && !back);
    free(made);
}

/* Two numbers of a waiting send, as laid out (pay's fields in the app's module). */
static bool pay_reads(const PureApp *app, size_t k, double first, double second) {
    Outbox o = outbox_of(app);
    double a, b;
    if (k >= o.count || o.items[k].size != 16) return false;
    memcpy(&a, o.items[k].payload, 8);
    memcpy(&b, o.items[k].payload + 8, 8);
    return a == first && b == second;
}

/* An output whose fields are named otherwise (here two numbers swapped)
 * would mean something else of the bytes its sends wait with: replacing or
 * starting the module recasts them by field name, as a list's items move.
 * The outbox's image names the fields it holds its sends in, so a start
 * after a replace whose save never came (the links still naming the old
 * fields) reads them right. A module without the output is refused while
 * sends wait on it, and says so. */
static void output_meaning(void) {
    static const S nums[] = {{"n", 0}};
    static const F was_fields[] = {{"debit", PURE_FIELD_NUMBER}, {"credit", PURE_FIELD_NUMBER}};
    static const F now_fields[] = {{"credit", PURE_FIELD_NUMBER}, {"debit", PURE_FIELD_NUMBER}};
    static const I was_inputs[] = {KEY("tap", "KeyT"), {"pay", NULL, PURE_KIND_OUTPUT, was_fields, 2}};
    static const I now_inputs[] = {KEY("tap", "KeyT"), {"pay", NULL, PURE_KIND_OUTPUT, now_fields, 2}};
    static const I none_inputs[] = {KEY("tap", "KeyT")};
    static const O was_code[] = {{C, 0, 10}, {C, 0, 20}, {SEND, 1, 0}}; /* pay { debit: 10, credit: 20 } */
    static const O now_code[] = {{C, 0, 20}, {C, 0, 10}, {SEND, 1, 0}}; /* the same, fields in the new order */
    static const O none_code[] = {{C, 0, 1}, {ST, 0, 0}};
    static const H handlers[] = {{1, 0, 3}}, none_handlers[] = {{1, 0, 2}};
    Spec was = {nums, 1, was_inputs, 2, NULL, 0, handlers, 1, was_code, 3, NULL, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0,
                NULL, 0};
    Spec now = {nums, 1, now_inputs, 2, NULL, 0, handlers, 1, now_code, 3, NULL, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0,
                NULL, 0};
    Spec none = {nums, 1, none_inputs, 1, NULL, 0, none_handlers, 1, none_code, 2, NULL, 0, NULL, 0, NULL, 0, NULL, 0,
                 NULL, 0, NULL, 0};
    PureModule without = {0};
    CHECK(load(was, &module_a) == PURE_OK && load(now, &module_b) == PURE_OK && load(none, &without) == PURE_OK);
    CHECK(pure_module_shape(&module_a, 1) != pure_module_shape(&module_b, 1));
    char *path = new_store();
    PureApp app;
    CHECK(pure_app_start(&app, &module_a, pure_tree_data(path)) == PURE_OK);
    CHECK(press(&app, 0) == PURE_OK && pay_reads(&app, 0, 10, 20));
    CHECK(pure_app_replace(&app, &module_b) == PURE_OK && app.module == &module_b && pay_reads(&app, 0, 20, 10));
    pure_app_stop(&app); /* no save since: the links name a's fields, the outbox's image b's */
    CHECK(pure_app_start(&app, &module_b, pure_tree_data(path)) == PURE_OK && pay_reads(&app, 0, 20, 10));
    pure_app_stop(&app);
    CHECK(pure_app_start(&app, &module_a, pure_tree_data(path)) == PURE_OK && pay_reads(&app, 0, 10, 20));
    CHECK(press(&app, 0) == PURE_OK && pay_reads(&app, 1, 10, 20));
    CHECK(outbox_of(&app).items[1].seq == 2);
    pure_app_stop(&app);
    CHECK(pure_app_start(&app, &module_b, pure_tree_data(path)) == PURE_OK && pay_reads(&app, 0, 20, 10) &&
          pay_reads(&app, 1, 20, 10) && outbox_of(&app).items[1].seq == 2);
    /* Sent with b's fields, logged after the image named them. */
    CHECK(press(&app, 0) == PURE_OK && pay_reads(&app, 2, 20, 10));
    pure_app_stop(&app);
    CHECK(pure_app_start(&app, &module_a, pure_tree_data(path)) == PURE_OK && pay_reads(&app, 2, 10, 20));
    pure_app_stop(&app);
    CHECK(pure_app_start(&app, &without, pure_tree_data(path)) == PURE_CONFLICT && strstr(app.why, "'pay'"));
    CHECK(pure_app_start(&app, &module_a, pure_tree_data(path)) == PURE_OK && !app.why[0]);
    CHECK(pure_app_replace(&app, &without) == PURE_CONFLICT && app.module == &module_a && strstr(app.why, "'pay'"));
    pure_app_trim(&app, 1, 3);
    CHECK(pure_app_save(&app) == PURE_OK && pure_app_replace(&app, &without) == PURE_OK && !app.why[0]);
    pure_app_stop(&app);
    pure_module_release(&without);
    remove_store(path);
}

/* ---- Bools (docs/design/016-bool-and-i64.md) -----------------------------
 * A bool is its own type: comparisons, has and logic give one; branches,
 * require, filters and folds' tests take one; a number is never one, nor a
 * bool a number. It runs as 0 or 1 in a number's place, a message holds it
 * in a byte, and it is written true or false. A number that becomes a bool
 * (a module replaced, a stored state or item) keeps 0 and 1, of either sign,
 * and takes the default for any other; a bool that becomes a number is 0 or 1. */
enum { B_N = 0, B_ON = 1, B_OFF = 2, B_TOLD = 5 };
#define TRUTH_LOCAL(slot) ((uint32_t)(slot) | 2u << 8)
static const S bool_nums[] = {{"n", 0}};
static const T bool_states[] = {{"on", 5, 1}, {"off", 5, 0}};
static const F set_fields[] = {{"on", PURE_FIELD_BOOL}, {"n", PURE_FIELD_NUMBER}};
static const F told_fields[] = {{"on", PURE_FIELD_BOOL}, {"big", PURE_FIELD_BOOL}, {"n", PURE_FIELD_NUMBER}};
static const I bool_inputs[] = {KEY("flip", "KeyF"), KEY("count", "KeyC"), KEY("check", "KeyK"), KEY("tell", "KeyT"),
                                {"set", NULL, PURE_KIND_MESSAGE, set_fields, 2},
                                {"told", NULL, PURE_KIND_OUTPUT, told_fields, 3}};
static const O bool_code[] = {
    {L, B_ON, 0}, {NOT, 0, 0}, {ST, B_ON, 0},                                           /* 0 flip: on = !on */
    {L, B_ON, 0}, {JZ, 9, 0}, {L, B_N, 0}, {C, 0, 1}, {ADD, 0, 0}, {ST, B_N, 0},          /* 3 count: if on { n += 1 } */
    {C, 1, 1}, {ST, B_OFF, 0}, {L, B_N, 0}, {C, 0, 3}, {LT, 0, 0}, {REQ, 0, 0},         /* 9 check: off = true; require n < 3 */
    {L, B_ON, 0}, {L, B_N, 0}, {C, 0, 2}, {GT, 0, 0}, {L, B_N, 0}, {SEND, B_TOLD, 0},   /* 15 tell: told { on, big: n > 2, n } */
    {FIELD, 0, 0}, {ST, B_ON, 0}, {FIELD, 1, 0}, {ST, B_N, 0},                          /* 21 set */
    {L, B_ON, 0},                                                                       /* 25 view lit */
    {L, B_N, 0}, {C, 0, 2}, {GT, 0, 0},                                                 /* 26 view big */
    {L, B_ON, 0}, {L, B_OFF, 0}, {EQ, 0, 0},                                            /* 29 view same */
    {L, B_ON, 0}, {L, B_OFF, 0}, {PURE_OP_OR, 0, 0}, {L, B_N, 0}, {C, 0, 0}, {PURE_OP_NE, 0, 0},
    {PURE_OP_AND, 0, 0},                                                                /* 32 view either */
    {L, B_ON, 0}, {NT, 1, 0}, {C, 0, 1}, {NT, 0, 0}, {CAT, 0, 0},                       /* 39 view said */
};
static const H bool_handlers[] = {{1, 0, 3}, {3, 3, 6}, {5, 9, 6}, {7, 15, 6}, {9, 21, 4}};
static const V bool_views[] = {{"lit", 25, 1 | BOOL_VIEW}, {"big", 26, 3 | BOOL_VIEW}, {"same", 29, 3 | BOOL_VIEW},
                               {"either", 32, 7 | BOOL_VIEW}};
static const V bool_texts[] = {{"said", 39, 5}};

static void bools(void) {
    Spec spec = {bool_nums, 1, bool_inputs, LEN(bool_inputs), bool_views, LEN(bool_views), bool_handlers,
                 LEN(bool_handlers), bool_code, LEN(bool_code), NULL, 0, bool_states, LEN(bool_states), NULL, 0,
                 bool_texts, 1, NULL, 0, NULL, 0};
    if (load(spec, &module_a) != PURE_OK) { fprintf(stderr, "%s\n", error); abort(); }
    CHECK(module_a.state_type[B_ON] == PURE_STATE_BOOL && module_a.view_type[0] == PURE_VIEW_BOOL &&
          module_a.input[4].prefix == 9 && module_a.input[B_TOLD].prefix == 10);
    PureApp app;
    CHECK(pure_app_start(&app, &module_a, (PureData){0}) == PURE_OK && app.values[B_ON].num == 1 && app.values[B_OFF].num == 0);
    CHECK(number_view(&app, 0) == 1 && number_view(&app, 1) == 0 && number_view(&app, 2) == 0 && number_view(&app, 3) == 0);
    CHECK(!strcmp(view_text(&app, 4), "true1"));
    for (int k = 0; k < 3; ++k) CHECK(press(&app, 1) == PURE_OK);
    CHECK(app.values[B_N].num == 3 && number_view(&app, 1) == 1 && number_view(&app, 3) == 1);
    CHECK(press(&app, 2) == PURE_LIMIT && app.values[B_OFF].num == 0); /* require failed: undone */
    CHECK(press(&app, 0) == PURE_OK && app.values[B_ON].num == 0 && !strcmp(view_text(&app, 4), "false1"));
    CHECK(press(&app, 1) == PURE_OK && app.values[B_N].num == 3 && number_view(&app, 2) == 1);
    /* A message's bool is a byte, 0 or 1; any other byte fails the message. */
    unsigned char payload[9] = {1};
    double five = 5;
    memcpy(payload + 1, &five, 8);
    CHECK(pure_app_message(&app, 4, (PureSpan){payload, 9}) == PURE_OK && app.values[B_ON].num == 1 && app.values[B_N].num == 5);
    pure_app_finish(&app);
    payload[0] = 2;
    five = 6;
    memcpy(payload + 1, &five, 8);
    CHECK(pure_app_message(&app, 4, (PureSpan){payload, 9}) == PURE_LIMIT && app.values[B_N].num == 5);
    pure_app_finish(&app);
    /* A send holds its bools as bytes. */
    CHECK(press(&app, 3) == PURE_OK);
    Outbox o = outbox_of(&app);
    double n;
    memcpy(&n, o.items[0].payload + 2, 8);
    CHECK(o.count == 1 && o.items[0].size == 10 && o.items[0].payload[0] == 1 && o.items[0].payload[1] == 1 && n == 5);
    /* Stored, a waiting send keeps its bools. */
    char *sends = new_store();
    PureApp kept;
    CHECK(pure_app_start(&kept, &module_a, pure_tree_data(sends)) == PURE_OK && press(&kept, 1) == PURE_OK);
    CHECK(press(&kept, 3) == PURE_OK && pure_app_save(&kept) == PURE_OK);
    pure_app_stop(&kept);
    CHECK(pure_app_start(&kept, &module_a, pure_tree_data(sends)) == PURE_OK);
    Outbox stored_sends = outbox_of(&kept);
    CHECK(stored_sends.count == 1 && stored_sends.items[0].size == 10 && stored_sends.items[0].payload[0] == 1 &&
          stored_sends.items[0].payload[1] == 0);
    pure_app_stop(&kept);
    remove_store(sends);
    /* A link delivers a bool to a bool only; a module's outputs replaced
     * recast a waiting send's values as its states are kept. */
    static const unsigned char same[] = {2, PURE_FIELD_BOOL, 2, 'o', 'n', PURE_FIELD_NUMBER, 1, 'n'};
    static const unsigned char number_on[] = {1, PURE_FIELD_NUMBER, 2, 'o', 'n'}, bool_n[] = {1, PURE_FIELD_BOOL, 1, 'n'};
    CHECK(pure_module_takes(&module_a, 4, same, sizeof(same)) && !pure_module_takes(&module_a, 4, number_on, sizeof(number_on)) &&
          !pure_module_takes(&module_a, 4, bool_n, sizeof(bool_n)));
    PureField told[3] = {{"on", PURE_FIELD_BOOL, 0, 0}, {"big", PURE_FIELD_BOOL, 0, 0}, {"n", PURE_FIELD_NUMBER, 0, 0}};
    PureField other[3] = {{"n", PURE_FIELD_BOOL, 0, 0}, {"on", PURE_FIELD_NUMBER, 0, 0}, {"big", PURE_FIELD_U32, 0, 0}};
    CHECK(pure_fields_lay_out(told, 3) == 10 && pure_fields_lay_out(other, 3) == 13);
    unsigned char *made, *back;
    size_t size, back_size;
    CHECK(pure_send_recast(told, 3, other, 3, (PureSpan){o.items[0].payload, 10}, &made, &size) == PURE_OK && size == 13);
    double on;
    uint32_t big;
    memcpy(&on, made + 1, 8);
    memcpy(&big, made + 9, 4);
    CHECK(made[0] == 0 && on == 1 && big == 1); /* n = 5 is no bool */
    CHECK(pure_send_recast(other, 3, told, 3, (PureSpan){made, size}, &back, &back_size) == PURE_OK && back_size == 10);
    memcpy(&n, back + 2, 8);
    CHECK(back[0] == 1 && back[1] == 1 && n == 0 && !signbit(n));
    free(made);
    free(back);
    /* Replaced: n (5) becomes a bool (its default, false), on a number (1); back again, 0 and 1. */
    static const S swapped_nums[] = {{"on", 7}};
    static const T swapped_states[] = {{"n", 5, 0}, {"off", 5, 1}};
    Spec swapped = {swapped_nums, 1, bool_inputs, LEN(bool_inputs), NULL, 0, NULL, 0, NULL, 0, NULL, 0, swapped_states, 2,
                    NULL, 0, NULL, 0, NULL, 0, NULL, 0};
    CHECK(load(swapped, &module_b) == PURE_OK);
    CHECK(pure_app_replace(&app, &module_b) == PURE_OK && app.values[0].num == 1 && app.values[1].num == 0 && app.values[2].num == 0);
    app.values[0].num = -0.0; /* on, a number: -0 is 0 */
    CHECK(pure_app_replace(&app, &module_a) == PURE_OK && app.values[B_N].num == 0 && app.values[B_ON].num == 0 &&
          !signbit(app.values[B_ON].num) && app.values[B_OFF].num == 0);
    pure_app_stop(&app);

    /* What mixes bools and numbers is refused. */
    static const O add_bools[] = {{C, 1, 1}, {C, 1, 0}, {ADD, 0, 0}, {ST, B_N, 0}};
    static const O branch_number[] = {{C, 0, 1}, {JZ, 2, 0}};
    static const O bool_in_number[] = {{C, 1, 1}, {ST, B_N, 0}};
    static const O number_in_bool[] = {{C, 0, 1}, {ST, B_ON, 0}};
    static const O order_bools[] = {{C, 1, 1}, {C, 1, 0}, {LT, 0, 0}, {ST, B_ON, 0}};
    static const O bool_is_number[] = {{C, 1, 1}, {C, 0, 0}, {EQ, 0, 0}, {ST, B_ON, 0}};
    static const O not_number[] = {{C, 0, 1}, {NOT, 0, 0}, {ST, B_ON, 0}};
    static const O require_number[] = {{C, 0, 1}, {REQ, 0, 0}};
    static const O and_number[] = {{C, 1, 1}, {C, 0, 1}, {PURE_OP_AND, 0, 0}, {ST, B_ON, 0}};
    static const O min_bools[] = {{C, 1, 1}, {C, 1, 1}, {MIN, 0, 0}, {ST, B_N, 0}};
    static const O neg_bool[] = {{C, 1, 1}, {NEG, 0, 0}, {ST, B_N, 0}};
    static const O send_number[] = {{C, 1, 1}, {C, 0, 1}, {C, 0, 2}, {SEND, B_TOLD, 0}};
    static const O local_number[] = {{C, 0, 1}, {LOCAL_S, TRUTH_LOCAL(0), 0}};
    static const O local_bool[] = {{C, 1, 1}, {LOCAL_S, LOCAL(0, -1), 0}};
    static const O field_number[] = {{FIELD, 0, 0}, {ST, B_N, 0}};
    static const O two[] = {{C, 1, 2}, {ST, B_ON, 0}};
    static const O minus_zero[] = {{C, 1, -0.0}, {ST, B_ON, 0}};
    static const O half[] = {{C, 1, 0.5}, {ST, B_ON, 0}};
    static const O kind_three[] = {{C, 3, 1}, {ST, B_ON, 0}};
    static const struct { const O *code; size_t count; uint32_t event; const char *reason; } refused[] = {
        {add_bools, LEN(add_bools), 1, "operand type"},
        {branch_number, LEN(branch_number), 1, "operand type"},
        {bool_in_number, LEN(bool_in_number), 1, "operand type"},
        {number_in_bool, LEN(number_in_bool), 1, "operand type"},
        {order_bools, LEN(order_bools), 1, "operand type"},
        {bool_is_number, LEN(bool_is_number), 1, "operand type"},
        {not_number, LEN(not_number), 1, "operand type"},
        {require_number, LEN(require_number), 1, "operand type"},
        {and_number, LEN(and_number), 1, "operand type"},
        {min_bools, LEN(min_bools), 1, "operand type"},
        {neg_bool, LEN(neg_bool), 1, "operand type"},
        {send_number, LEN(send_number), 1, "operand type"},
        {local_number, LEN(local_number), 1, "operand type"},
        {local_bool, LEN(local_bool), 1, "operand type"},
        {field_number, LEN(field_number), 9, "operand type"},   /* set's on is a bool */
        {two, LEN(two), 1, "non-canonical operand"},
        {minus_zero, LEN(minus_zero), 1, "non-canonical operand"},
        {half, LEN(half), 1, "non-canonical operand"},
        {kind_three, LEN(kind_three), 1, "operand names no such"},
    };
    for (size_t i = 0; i < LEN(refused); ++i) {
        H handler = {refused[i].event, 0, (uint32_t)refused[i].count};
        Spec bad = spec;
        bad.o = refused[i].code, bad.no = refused[i].count, bad.h = &handler, bad.nh = 1;
        bad.v = NULL, bad.nv = 0, bad.tv = NULL, bad.ntv = 0;
        if (load(bad, &module_b) != PURE_INVALID || !strstr(error, refused[i].reason)) {
            fprintf(stderr, "bool case %zu: expected '%s', got '%s'\n", i, refused[i].reason, error);
            abort();
        }
    }
    static const O one_number[] = {{C, 0, 1}}, one_bool[] = {{C, 1, 1}};
    static const O text_of_number[] = {{C, 0, 1}, {NT, 1, 0}}, text_of_bool[] = {{C, 1, 1}, {NT, 0, 0}},
                   text_kind[] = {{C, 1, 1}, {NT, 3, 0}};
    V as_bool = {"v", 0, 1 | BOOL_VIEW}, as_number = {"v", 0, 1}, as_text = {"v", 0, 2};
    Spec bad = spec;
    bad.h = NULL, bad.nh = 0, bad.tv = NULL, bad.ntv = 0, bad.no = 1, bad.nv = 1;
    bad.o = one_number, bad.v = &as_bool;
    CHECK(load(bad, &module_b) == PURE_INVALID && strstr(error, "view must leave one value of its type"));
    bad.o = one_bool, bad.v = &as_number;
    CHECK(load(bad, &module_b) == PURE_INVALID && strstr(error, "view must leave one value of its type"));
    bad.v = NULL, bad.nv = 0, bad.tv = &as_text, bad.ntv = 1, bad.no = 2;
    bad.o = text_of_number;
    CHECK(load(bad, &module_b) == PURE_INVALID && strstr(error, "operand type"));
    bad.o = text_of_bool;
    CHECK(load(bad, &module_b) == PURE_INVALID && strstr(error, "operand type"));
    bad.o = text_kind;
    CHECK(load(bad, &module_b) == PURE_INVALID && strstr(error, "operand names no such"));
    /* A bool's initial value and default are 0 or 1; types past bool are none. */
    static const T two_initial[] = {{"on", 5, 2}, {"off", 5, 0}}, seventh[] = {{"on", 7, 0}, {"off", 5, 0}};
    static const RF two_default[] = {{"done", 2, 2}};
    static const R two_record[] = {{"r", two_default, 1}};
    static const T with_list[] = {{"on", 5, 1}, {"off", 5, 0}, {"rs", 3, 0}};
    static const W fifth_view[] = {{"x", 8, 0, 25, 1}};
    bad = spec;
    bad.t = two_initial;
    CHECK(load(bad, &module_b) == PURE_INVALID && strstr(error, "bool initial value"));
    bad.t = seventh;
    CHECK(load(bad, &module_b) == PURE_INVALID && strstr(error, "state type"));
    bad.t = with_list, bad.nt = 3, bad.r = two_record, bad.nr = 1;
    CHECK(load(bad, &module_b) == PURE_INVALID && strstr(error, "record field default"));
    bad = spec;
    bad.w = fifth_view, bad.nw = 1;
    CHECK(load(bad, &module_b) == PURE_INVALID && strstr(error, "view type"));

    /* Stored: n and the items' done, numbers, become bools (1 kept, 2 and 3
     * the default, false) and done a number again. */
    char *path = new_store();
    Spec numbers = {crash_nums, 1, crash_inputs, 1, NULL, 0, crash_handlers, 1, crash_code, LEN(crash_code), NULL, 0,
                    crash_states, 4, crash_constants, 2, NULL, 0, crash_records, 1, NULL, 0};
    static const T flagged_states[] = {{"n", 5, 0}, {"note", 1, 0}, {"at", 2, 1}, {"tasks", 3, 0}, {"i", 4, 3}};
    static const RF flagged_fields[] = {{"title", 1, 0}, {"done", 2, 0}};
    static const R flagged_records[] = {{"task", flagged_fields, 2}};
    Spec flagged = {NULL, 0, crash_inputs, 1, NULL, 0, NULL, 0, NULL, 0, NULL, 0, flagged_states, 5, crash_constants, 2,
                    NULL, 0, flagged_records, 1, NULL, 0};
    CHECK(load(numbers, &module_a) == PURE_OK && load(flagged, &module_b) == PURE_OK);
    CHECK(pure_app_start(&app, &module_a, pure_tree_data(path)) == PURE_OK);
    for (int k = 0; k < 3; ++k) CHECK(press(&app, 0) == PURE_OK);
    pure_app_stop(&app);
    for (int round = 0; round < 2; ++round) { /* migrated, then stored as migrated */
        CHECK(pure_app_start(&app, &module_b, pure_tree_data(path)) == PURE_OK && app.values[0].num == 0);
        CHECK(pure_list_count(app.lists[TASKS]) == 3 && pure_list_get(app.lists[TASKS], 0, 1).num == 1 &&
              pure_list_get(app.lists[TASKS], 1, 1).num == 0 && pure_list_get(app.lists[TASKS], 2, 1).num == 0);
        pure_app_stop(&app);
    }
    CHECK(pure_app_start(&app, &module_a, pure_tree_data(path)) == PURE_OK && pure_list_count(app.lists[TASKS]) == 3);
    CHECK(pure_list_get(app.lists[TASKS], 0, 1).num == 1 && pure_list_get(app.lists[TASKS], 2, 1).num == 0);
    pure_app_stop(&app);
    remove_store(path);
    printf("{\"bools\":{\"refused\":%zu}}\n", LEN(refused) + 9);
}

/* ---- i64s (docs/design/016-bool-and-i64.md) ------------------------------
 * An i64 is its own type, exact over its range: arithmetic past it fails
 * as overflow, division and remainder floor, conversions to and from
 * numbers happen only where the value fits. Keyed lists order i64 keys as
 * integers; folds sum, keep the most or least, count the different ones;
 * stores, logs, messages, sends and replacements keep every bit. */
enum { IADD = PURE_OP_I64_ADD, ISUB = PURE_OP_I64_SUB, IMUL = PURE_OP_I64_MUL, IDIV = PURE_OP_I64_DIV,
       IMOD = PURE_OP_I64_MOD, INEG = PURE_OP_I64_NEG, ICMP = PURE_OP_I64_COMPARE, IMIN = PURE_OP_I64_MIN,
       IMAX = PURE_OP_I64_MAX, IFROM = PURE_OP_I64_FROM, NFROM = PURE_OP_NUM_FROM };
static const int64_t STEP = INT64_MAX / 7; /* seven of them make the largest i64 */

/* An i64 constant's 8 bytes where an instruction keeps its value. */
static double integer_bits(int64_t value) {
    double bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}
#define IC(v) ((O){C, 2, integer_bits(v)})

/* Code run as a key's handler over the states n (a number), i (an i64) and
 * b (a bool): the fault it ends with, and the states after. */
typedef struct { PureFault fault; PureValue values[3]; } Ran;
static Ran run_code(const O *code, uint32_t count) {
    static const S nums[] = {{"n", 0}};
    static const T others[] = {{"i", PURE_STATE_I64, 0}, {"b", PURE_STATE_BOOL, 0}};
    static const I inputs[] = {KEY("go", "KeyG")};
    H handler = {1, 0, count};
    Spec spec = {nums, 1, inputs, 1, NULL, 0, &handler, 1, code, count, NULL, 0, others, 2, NULL, 0, NULL, 0, NULL, 0, NULL, 0};
    if (load(spec, &module_b) != PURE_OK) { fprintf(stderr, "%s\n", error); abort(); }
    PureApp app;
    CHECK(pure_app_start(&app, &module_b, (PureData){0}) == PURE_OK);
    Ran ran = {press(&app, 0) == PURE_OK ? PURE_FAULT_NONE : app.fault, {{0}}};
    memcpy(ran.values, app.values, sizeof(ran.values));
    pure_app_stop(&app);
    return ran;
}

/* The keyed list app: items keyed by an i64 id, next the id to add. */
enum { KI_N = 0, KI_NEXT = 1, KI_ITEMS = 2, KI_FOUND = 3 };
static const S keyed_i64_nums[] = {{"n", 0}};
static const T keyed_i64_states[] = {{"next", PURE_STATE_I64, 0}, {"items", 3 | 1 << 8, 0}, {"found", PURE_STATE_BOOL, 0}};
static const RF item_i64_fields[] = {{"id", 3, 0}, {"qty", 0, 0}};
static const R item_i64_records[] = {{"Item", item_i64_fields, 2}};
static const F put_i64_fields[] = {{"id", PURE_FIELD_I64}, {"qty", PURE_FIELD_NUMBER}};
static const F echo_i64_fields[] = {{"id", PURE_FIELD_I64}};
static const I keyed_i64_inputs[] = {KEY("add", "KeyA"), KEY("low", "KeyL"), KEY("drop", "KeyD"), KEY("ask", "KeyK"),
                                     {"put", NULL, PURE_KIND_MESSAGE, put_i64_fields, 2},
                                     {"echo", NULL, PURE_KIND_OUTPUT, echo_i64_fields, 1}};

static Spec keyed_i64_spec(O *code) {
    O made[] = {
        /* 0 add: items.add(Item { id: next, qty: n }); next = next + STEP; n = n + 1 */
        {L, KI_NEXT, 0}, {L, KI_N, 0}, {LA, KI_ITEMS, 0}, {L, KI_NEXT, 0}, IC(STEP), {IADD, 0, 0}, {ST, KI_NEXT, 0},
        {L, KI_N, 0}, {C, 0, 1}, {ADD, 0, 0}, {ST, KI_N, 0},
        /* 11 low: items.add(Item { id: -next, qty: -1 }) */
        {L, KI_NEXT, 0}, {INEG, 0, 0}, {C, 0, -1}, {LA, KI_ITEMS, 0},
        /* 15 drop: items.remove(items.first) */
        {XF, KI_ITEMS, 0}, {LR, KI_ITEMS, 0},
        /* 17 ask: found = items.has(next - STEP) */
        {L, KI_NEXT, 0}, IC(STEP), {ISUB, 0, 0}, {HAS, KI_ITEMS, 0}, {ST, KI_FOUND, 0},
        /* 22 view total = sum(i.id for i in items), 26 most, 30 least, 34 kinds (distinct) */
        {FOLD, FOLDING(PURE_FOLD_SUM | PURE_FOLD_INTEGER, KI_ITEMS, 3), 0}, {ROW, KI_ITEMS, 0}, {IG, ITEM(KI_ITEMS, 0), 0}, {C, 1, 1},
        {FOLD, FOLDING(PURE_FOLD_MAX | PURE_FOLD_INTEGER, KI_ITEMS, 3), 0}, {ROW, KI_ITEMS, 0}, {IG, ITEM(KI_ITEMS, 0), 0}, {C, 1, 1},
        {FOLD, FOLDING(PURE_FOLD_MIN | PURE_FOLD_INTEGER, KI_ITEMS, 3), 0}, {ROW, KI_ITEMS, 0}, {IG, ITEM(KI_ITEMS, 0), 0}, {C, 1, 1},
        {FOLD, FOLDING(PURE_FOLD_DISTINCT | PURE_FOLD_INTEGER, KI_ITEMS, 3), 0}, {ROW, KI_ITEMS, 0}, {IG, ITEM(KI_ITEMS, 0), 0},
        {C, 1, 1},
        /* 38 view first_id = items.first.id, 40 view said = "{next}" */
        {XF, KI_ITEMS, 0}, {IG, ITEM(KI_ITEMS, 0), 0}, {L, KI_NEXT, 0}, {NT, 2, 0},
        /* 42 put: items.add(Item { id: put.id, qty: put.qty }); send echo { id: put.id } */
        {FIELD, 0, 0}, {FIELD, 1, 0}, {LA, KI_ITEMS, 0}, {FIELD, 0, 0}, {SEND, 5, 0},
    };
    memcpy(code, made, sizeof(made));
    static const H handlers[] = {{1, 0, 11}, {3, 11, 4}, {5, 15, 2}, {7, 17, 5}, {9, 42, 5}};
    static const V views[] = {{"total", 22, 4 | I64_VIEW}, {"most", 26, 4 | I64_VIEW}, {"least", 30, 4 | I64_VIEW},
                              {"kinds", 34, 4}, {"first_id", 38, 2 | I64_VIEW}};
    static const V texts[] = {{"said", 40, 2}};
    return (Spec){keyed_i64_nums, 1, keyed_i64_inputs, LEN(keyed_i64_inputs), views, LEN(views), handlers, LEN(handlers),
                  code, LEN(made), NULL, 0, keyed_i64_states, LEN(keyed_i64_states), NULL, 0, texts, 1, item_i64_records, 1,
                  NULL, 0};
}

static int64_t i64_view(PureApp *app, uint32_t view) {
    int64_t value = 0;
    CHECK(pure_app_view_i64(app, view, &value));
    return value;
}

static void integers(void) {
    /* Arithmetic at the ends of the range, floored division and remainders. */
    static const struct { uint8_t op; int64_t a, b; PureFault fault; int64_t result; } cases[] = {
        {IADD, INT64_MAX - 1, 1, PURE_FAULT_NONE, INT64_MAX}, {IADD, INT64_MAX, 1, PURE_FAULT_OVERFLOW, 0},
        {IADD, INT64_MIN, -1, PURE_FAULT_OVERFLOW, 0}, {ISUB, INT64_MIN + 1, 1, PURE_FAULT_NONE, INT64_MIN},
        {ISUB, INT64_MIN, 1, PURE_FAULT_OVERFLOW, 0}, {ISUB, 0, INT64_MIN, PURE_FAULT_OVERFLOW, 0},
        {IMUL, 3037000499, 3037000499, PURE_FAULT_NONE, INT64_C(9223372030926249001)},
        {IMUL, INT64_MAX, 2, PURE_FAULT_OVERFLOW, 0}, {IMUL, INT64_MIN, -1, PURE_FAULT_OVERFLOW, 0},
        {IMUL, INT64_MIN, 1, PURE_FAULT_NONE, INT64_MIN},
        {IDIV, 7, 2, PURE_FAULT_NONE, 3}, {IDIV, -7, 2, PURE_FAULT_NONE, -4}, {IDIV, 7, -2, PURE_FAULT_NONE, -4},
        {IDIV, -7, -2, PURE_FAULT_NONE, 3}, {IDIV, INT64_MIN, -1, PURE_FAULT_OVERFLOW, 0},
        {IDIV, INT64_MIN, 1, PURE_FAULT_NONE, INT64_MIN}, {IDIV, INT64_MAX, -1, PURE_FAULT_NONE, -INT64_MAX},
        {IDIV, 5, 0, PURE_FAULT_DIVIDE, 0},
        {IMOD, 7, 2, PURE_FAULT_NONE, 1}, {IMOD, -7, 2, PURE_FAULT_NONE, 1}, {IMOD, 7, -2, PURE_FAULT_NONE, -1},
        {IMOD, -7, -2, PURE_FAULT_NONE, -1}, {IMOD, INT64_MIN, -1, PURE_FAULT_NONE, 0}, {IMOD, 6, -3, PURE_FAULT_NONE, 0},
        {IMOD, INT64_MIN, INT64_MAX, PURE_FAULT_NONE, INT64_MAX - 1}, {IMOD, 5, 0, PURE_FAULT_DIVIDE, 0},
        {IMIN, -1, INT64_MIN, PURE_FAULT_NONE, INT64_MIN}, {IMAX, -1, INT64_MAX, PURE_FAULT_NONE, INT64_MAX},
    };
    for (size_t k = 0; k < LEN(cases); ++k) {
        O code[] = {IC(cases[k].a), IC(cases[k].b), {cases[k].op, 0, 0}, {ST, 1, 0}};
        Ran ran = run_code(code, LEN(code));
        if (ran.fault != cases[k].fault || ran.values[1].i64 != (cases[k].fault ? 0 : cases[k].result)) {
            fprintf(stderr, "i64 case %zu: fault %d, %" PRId64 "\n", k, (int)ran.fault, ran.values[1].i64);
            abort();
        }
    }
    O neg_most[] = {IC(INT64_MIN), {INEG, 0, 0}, {ST, 1, 0}}, neg_max[] = {IC(INT64_MAX), {INEG, 0, 0}, {ST, 1, 0}};
    CHECK(run_code(neg_most, 3).fault == PURE_FAULT_OVERFLOW);
    CHECK(run_code(neg_max, 3).values[1].i64 == -INT64_MAX);
    /* Comparisons give bools, by each relation. */
    static const struct { int64_t a, b; uint8_t truths; } pairs[] = {{-1, 0, 0x23}, {INT64_MAX, INT64_MIN, 0x2C}, {5, 5, 0x1A}};
    for (size_t k = 0; k < LEN(pairs); ++k)
        for (uint32_t relation = 0; relation <= PURE_NOT_EQUAL; ++relation) {
            O code[] = {IC(pairs[k].a), IC(pairs[k].b), {ICMP, relation, 0}, {ST, 2, 0}};
            CHECK(run_code(code, LEN(code)).values[2].num == (pairs[k].truths >> relation & 1));
        }
    /* Conversions only where the value fits. */
    static const struct { double from; PureFault fault; int64_t to; } wholes[] = {
        {9007199254740994.0, PURE_FAULT_NONE, INT64_C(9007199254740994)}, {-0.0, PURE_FAULT_NONE, 0},
        {-9223372036854775808.0, PURE_FAULT_NONE, INT64_MIN}, {9223372036854775808.0, PURE_FAULT_LIMIT, 0},
        {0.5, PURE_FAULT_LIMIT, 0}, {-1e300, PURE_FAULT_LIMIT, 0}};
    for (size_t k = 0; k < LEN(wholes); ++k) {
        O code[] = {{C, 0, wholes[k].from}, {IFROM, 0, 0}, {ST, 1, 0}};
        Ran ran = run_code(code, LEN(code));
        CHECK(ran.fault == wholes[k].fault && ran.values[1].i64 == wholes[k].to);
    }
    static const struct { int64_t from; PureFault fault; double to; } numbers_[] = {
        {INT64_C(9007199254740992), PURE_FAULT_NONE, 9007199254740992.0}, {-INT64_C(9007199254740992), PURE_FAULT_NONE, -9007199254740992.0},
        {INT64_C(9007199254740993), PURE_FAULT_LIMIT, 0}, {INT64_MIN, PURE_FAULT_LIMIT, 0}};
    for (size_t k = 0; k < LEN(numbers_); ++k) {
        O code[] = {IC(numbers_[k].from), {NFROM, 0, 0}, {ST, 0, 0}};
        Ran ran = run_code(code, LEN(code));
        CHECK(ran.fault == numbers_[k].fault && ran.values[0].num == numbers_[k].to);
    }

    /* The keyed list app: ids ordered as integers, a sum that overflows, every digit shown. */
    O code[64];
    Spec spec = keyed_i64_spec(code);
    if (load(spec, &module_a) != PURE_OK) { fprintf(stderr, "%s\n", error); abort(); }
    CHECK(module_a.state_type[KI_NEXT] == PURE_STATE_I64 && module_a.view_type[0] == PURE_VIEW_I64 &&
          module_a.input[4].prefix == 16 && module_a.input[5].prefix == 8);
    PureApp app;
    CHECK(pure_app_start(&app, &module_a, (PureData){0}) == PURE_OK);
    int64_t value;
    CHECK(i64_view(&app, 0) == 0 && !pure_app_view_i64(&app, 1, &value) && !pure_app_view_i64(&app, 4, &value));
    double number;
    CHECK(!pure_app_view(&app, 0, &number) && pure_app_view(&app, 3, &number) && number == 0);
    for (int k = 0; k < 3; ++k) CHECK(press(&app, 0) == PURE_OK);
    CHECK(press(&app, 1) == PURE_OK); /* -3 steps */
    CHECK(pure_list_count(app.lists[KI_ITEMS]) == 4 && pure_list_get(app.lists[KI_ITEMS], 0, 0).i64 == -3 * STEP &&
          pure_list_get(app.lists[KI_ITEMS], 1, 0).i64 == 0 && pure_list_get(app.lists[KI_ITEMS], 3, 0).i64 == 2 * STEP);
    CHECK(i64_view(&app, 0) == 0 && i64_view(&app, 1) == 2 * STEP && i64_view(&app, 2) == -3 * STEP &&
          pure_app_view(&app, 3, &number) && number == 4 && i64_view(&app, 4) == -3 * STEP);
    CHECK(press(&app, 1) == PURE_CONFLICT && app.fault == PURE_FAULT_EXISTS); /* -3 steps again */
    for (int k = 0; k < 4; ++k) CHECK(press(&app, 0) == PURE_OK);
    CHECK(app.values[KI_NEXT].i64 == INT64_MAX && !pure_app_view_i64(&app, 0, &value)); /* the sum overflows */
    CHECK(!strcmp(view_text(&app, 5), "9223372036854775807"));
    CHECK(press(&app, 0) == PURE_INVALID && app.fault == PURE_FAULT_OVERFLOW);
    CHECK(pure_list_count(app.lists[KI_ITEMS]) == 8 && app.values[KI_NEXT].i64 == INT64_MAX && app.values[KI_N].num == 7);
    CHECK(press(&app, 3) == PURE_OK && app.values[KI_FOUND].num == 1);
    CHECK(press(&app, 2) == PURE_OK && i64_view(&app, 4) == 0);
    /* A message's i64 is 8 bytes, a send's too. */
    unsigned char payload[16];
    int64_t least = INT64_MIN;
    double qty = 2.5;
    memcpy(payload, &least, 8);
    memcpy(payload + 8, &qty, 8);
    CHECK(pure_app_message(&app, 4, (PureSpan){payload, 16}) == PURE_OK && i64_view(&app, 2) == INT64_MIN);
    pure_app_finish(&app);
    Outbox o = outbox_of(&app);
    CHECK(o.count == 1 && o.items[0].size == 8 && !memcmp(o.items[0].payload, &least, 8));
    pure_app_stop(&app);

    /* Recast by name: a u32 widens to an i64; to and from numbers only where the value fits. */
    PureField wide[2] = {{"id", PURE_FIELD_I64, 0, 0}, {"n", PURE_FIELD_NUMBER, 0, 0}};
    PureField narrow[2] = {{"id", PURE_FIELD_NUMBER, 0, 0}, {"n", PURE_FIELD_I64, 0, 0}};
    PureField small[1] = {{"id", PURE_FIELD_U32, 0, 0}};
    CHECK(pure_fields_lay_out(wide, 2) == 16 && pure_fields_lay_out(narrow, 2) == 16 && pure_fields_lay_out(small, 1) == 4);
    unsigned char given[16], *made;
    size_t size;
    int64_t id = INT64_C(9007199254740993);
    double n = 2.5;
    memcpy(given, &id, 8);
    memcpy(given + 8, &n, 8);
    CHECK(pure_send_recast(wide, 2, narrow, 2, (PureSpan){given, 16}, &made, &size) == PURE_OK && size == 16);
    double read_id;
    int64_t read_n;
    memcpy(&read_id, made, 8);
    memcpy(&read_n, made + 8, 8);
    CHECK(read_id == 0 && read_n == 0); /* 2^53 + 1 is no number, 2.5 no i64 */
    free(made);
    uint32_t u = UINT32_MAX;
    CHECK(pure_send_recast(small, 1, wide, 2, (PureSpan){(unsigned char *)&u, 4}, &made, &size) == PURE_OK && size == 16);
    memcpy(&id, made, 8);
    CHECK(id == UINT32_MAX);
    free(made);
    static const unsigned char u32_id[] = {1, PURE_FIELD_U32, 2, 'i', 'd'}, number_id[] = {1, PURE_FIELD_NUMBER, 2, 'i', 'd'};
    CHECK(pure_module_takes(&module_a, 4, u32_id, sizeof(u32_id)) && !pure_module_takes(&module_a, 4, number_id, sizeof(number_id)));

    /* What mixes i64s and numbers is refused. */
    const struct { O code[4]; uint32_t count; const char *reason; } refused[] = {
        {{{C, 0, 1}, {C, 0, 2}, {IADD, 0, 0}, {ST, 1, 0}}, 4, "operand type"},
        {{IC(1), IC(2), {ADD, 0, 0}, {ST, 1, 0}}, 4, "operand type"},
        {{IC(1), {ST, 0, 0}}, 2, "operand type"},
        {{{C, 0, 1}, {ST, 1, 0}}, 2, "operand type"},
        {{IC(1), IC(2), {ICMP, PURE_NOT_EQUAL + 1, 0}, {ST, 2, 0}}, 4, "operand names no such"},
        {{IC(1), IC(2), {LT, 0, 0}, {ST, 2, 0}}, 4, "operand type"},
        {{IC(1), {JZ, 2, 0}}, 2, "operand type"},
        {{{C, 0, 1}, {NFROM, 0, 0}, {ST, 0, 0}}, 3, "operand type"},
        {{IC(1), {IFROM, 0, 0}, {ST, 1, 0}}, 3, "operand type"},
        {{{C, 0, 1}, {LOCAL_S, 3u << 8, 0}}, 2, "operand type"},
        {{IC(1), {LOCAL_S, LOCAL(0, -1), 0}}, 2, "operand type"},
    };
    for (size_t k = 0; k < LEN(refused); ++k) {
        static const S nums[] = {{"n", 0}};
        static const T others[] = {{"i", PURE_STATE_I64, 0}, {"b", PURE_STATE_BOOL, 0}};
        static const I inputs[] = {KEY("go", "KeyG")};
        H handler = {1, 0, refused[k].count};
        Spec bad = {nums, 1, inputs, 1, NULL, 0, &handler, 1, refused[k].code, refused[k].count, NULL, 0, others, 2, NULL, 0,
                    NULL, 0, NULL, 0, NULL, 0};
        if (load(bad, &module_b) != PURE_INVALID || !strstr(error, refused[k].reason)) {
            fprintf(stderr, "i64 refusal %zu: expected '%s', got '%s'\n", k, refused[k].reason, error);
            abort();
        }
    }
    /* A fold's value is a number or an i64 as its kind says; count and first take no i64. */
    O sum_number[] = {{FOLD, FOLDING(PURE_FOLD_SUM | PURE_FOLD_INTEGER, KI_ITEMS, 3), 0}, {ROW, KI_ITEMS, 0},
                      {IG, ITEM(KI_ITEMS, 1), 0}, {C, 1, 1}};
    O sum_i64[] = {{FOLD, FOLDING(PURE_FOLD_SUM, KI_ITEMS, 3), 0}, {ROW, KI_ITEMS, 0}, {IG, ITEM(KI_ITEMS, 0), 0}, {C, 1, 1}};
    O count_i64[] = {{FOLD, FOLDING(PURE_FOLD_COUNT | PURE_FOLD_INTEGER, KI_ITEMS, 1), 0}, {C, 1, 1}};
    V as_i64 = {"v", 0, 4 | I64_VIEW}, as_number = {"v", 0, 4}, as_count = {"v", 0, 2};
    Spec bad = spec;
    bad.h = NULL, bad.nh = 0, bad.tv = NULL, bad.ntv = 0, bad.nv = 1;
    bad.o = sum_number, bad.no = 4, bad.v = &as_i64;
    CHECK(load(bad, &module_b) == PURE_INVALID && strstr(error, "fold block must leave an i64 and a bool"));
    bad.o = sum_i64, bad.v = &as_number;
    CHECK(load(bad, &module_b) == PURE_INVALID && strstr(error, "fold block must leave a number and a bool"));
    bad.o = count_i64, bad.no = 2, bad.v = &as_count;
    CHECK(load(bad, &module_b) == PURE_INVALID && strstr(error, "operand names no such"));

    /* Replaced: a number, a bool and an i64 carry over each other where the value fits, else take the default. */
    static const S was_nums[] = {{"x", 5}, {"y", 2.5}};
    static const T was_others[] = {{"w", PURE_STATE_I64, 7}, {"v", PURE_STATE_I64, 0}, {"rows", 3, 0}};
    static const RF was_fields[] = {{"a", 0, 0}};
    static const R was_records[] = {{"Row", was_fields, 1}};
    static const S now_nums[] = {{"w", 3}};
    static const T now_others[] = {{"x", PURE_STATE_I64, (uint32_t)-1}, {"y", PURE_STATE_I64, (uint32_t)-2},
                                   {"v", PURE_STATE_BOOL, 0}, {"rows", 3, 0}};
    static const RF now_fields[] = {{"a", 3, 9}};
    static const R now_records[] = {{"Row", now_fields, 1}};
    Spec was = {was_nums, 2, NULL, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0, was_others, 3, NULL, 0, NULL, 0, was_records, 1, NULL, 0};
    Spec now = {now_nums, 1, NULL, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0, now_others, 4, NULL, 0, NULL, 0, now_records, 1, NULL, 0};
    CHECK(load(was, &module_a) == PURE_OK && load(now, &module_b) == PURE_OK);
    CHECK(pure_app_start(&app, &module_a, (PureData){0}) == PURE_OK && app.values[2].i64 == 7);
    app.values[3].i64 = INT64_C(9007199254740993);
    static const double rows[] = {3, 2.5, -0.0, 1e300};
    for (size_t k = 0; k < LEN(rows); ++k) CHECK(pure_list_insert(app.lists[4], k, (PureCell[]){{.num = rows[k]}}, NULL) == PURE_OK);
    CHECK(pure_app_replace(&app, &module_b) == PURE_OK);
    /* now: w (a number), x, y (i64s), v (a bool), rows of i64s */
    CHECK(app.values[0].num == 7 && app.values[1].i64 == 5 && app.values[2].i64 == -2 && app.values[3].num == 0);
    static const int64_t migrated[] = {3, 9, 0, 9};
    for (size_t k = 0; k < LEN(migrated); ++k) CHECK(pure_list_get(app.lists[4], k, 0).i64 == migrated[k]);
    app.values[3].num = 1; /* v true: an i64 1 when it goes back */
    CHECK(pure_app_replace(&app, &module_a) == PURE_OK);
    CHECK(app.values[0].num == 5 && app.values[1].num == -2 && app.values[2].i64 == 7 && app.values[3].i64 == 1);
    for (size_t k = 0; k < LEN(migrated); ++k) CHECK(pure_list_get(app.lists[4], k, 0).num == (double)migrated[k]);
    pure_app_stop(&app);

    /* Stored: random adds, drops and asks, restarting now and then, as in memory; extremes logged and imaged. */
    static const char *const none[] = {""};
    unsigned long checkpoints = pure_save_test_checkpoints;
    for (uint32_t seed = 1; seed <= 3; ++seed) stored_equivalence(keyed_i64_spec(code), none, 1, seed * 0x2545f491u);
    printf("{\"integers\":{\"cases\":%zu,\"refused\":%zu,\"checkpoints\":%lu}}\n", LEN(cases), LEN(refused) + 3,
           pure_save_test_checkpoints - checkpoints);
}

/* ---- Bits and bytes (docs/design/027, stage 1) ---------------------------
 * I64_BITS works on integers in two's complement without end: a left shift
 * past the range fails as overflow, a right shift rounds down, a shift
 * outside 0-63 fails as limit. BYTE_AT reads the byte at a whole position
 * of any bytes value (here a constant's and a text state's), BYTES_OF makes
 * the bytes of a list of one i64, INDEX_AT finds a list's item by position,
 * failing where there is none. */
enum { BITS = PURE_OP_I64_BITS, BAT = PURE_OP_BYTE_AT, BOF = PURE_OP_BYTES_OF, XAT = PURE_OP_INDEX_AT };

/* Code run as a key's handler over n (a number), note (text "Az\xc3\xa9"),
 * bytes (a list of Byte { v: i64 }), i (an i64) and said (text): the fault,
 * and n, i, said and the list's count after it. */
typedef struct { PureFault fault; double n; int64_t i; char said[64]; size_t count; } Byted;
static Byted run_bytes(const O *code, uint32_t count) {
    static const S nums[] = {{"n", 0}};
    static const T others[] = {{"note", 1, 0}, {"bytes", 3, 0}, {"i", PURE_STATE_I64, 0}, {"said", 1, 1}};
    static const RF byte_fields[] = {{"v", 3, 0}};
    static const R records[] = {{"Byte", byte_fields, 1}};
    static const char *const constants[] = {"Az\xc3\xa9", ""};
    static const I inputs[] = {KEY("go", "KeyG")};
    H handler = {1, 0, count};
    Spec spec = {nums, 1, inputs, 1, NULL, 0, &handler, 1, code, count, NULL, 0, others, LEN(others), constants,
                 LEN(constants), NULL, 0, records, 1, NULL, 0};
    if (load(spec, &module_b) != PURE_OK) { fprintf(stderr, "%s\n", error); abort(); }
    PureApp app;
    CHECK(pure_app_start(&app, &module_b, (PureData){0}) == PURE_OK);
    Byted ran = {press(&app, 0) == PURE_OK ? PURE_FAULT_NONE : app.fault, app.values[0].num,
                 app.values[3].i64, {0}, pure_list_count(app.lists[2])};
    snprintf(ran.said, sizeof(ran.said), "%s", state_text(&app, 4));
    pure_app_stop(&app);
    return ran;
}

static void bits_and_bytes(void) {
    enum { AND = PURE_BITS_AND, OR = PURE_BITS_OR, XOR = PURE_BITS_XOR, LEFT = PURE_BITS_LEFT, RIGHT = PURE_BITS_RIGHT };
    static const struct { uint32_t how; int64_t a, b; PureFault fault; int64_t result; } cases[] = {
        {AND, 0xF0F0, 0x0FF0, PURE_FAULT_NONE, 0x00F0}, {AND, -1, 5, PURE_FAULT_NONE, 5},
        {AND, INT64_MIN, -1, PURE_FAULT_NONE, INT64_MIN}, {AND, -8, 0xFF, PURE_FAULT_NONE, 0xF8},
        {OR, 0xF000, 0x000F, PURE_FAULT_NONE, 0xF00F}, {OR, -8, 3, PURE_FAULT_NONE, -5},
        {XOR, 5, -1, PURE_FAULT_NONE, -6}, {XOR, 0x55, 0xFF, PURE_FAULT_NONE, 0xAA}, {XOR, INT64_MAX, -1, PURE_FAULT_NONE, INT64_MIN},
        {LEFT, 1, 62, PURE_FAULT_NONE, INT64_C(1) << 62}, {LEFT, 1, 63, PURE_FAULT_OVERFLOW, 0},
        {LEFT, -1, 63, PURE_FAULT_NONE, INT64_MIN}, {LEFT, 3, 62, PURE_FAULT_OVERFLOW, 0}, {LEFT, 0, 63, PURE_FAULT_NONE, 0},
        {LEFT, 0x80000000, 32, PURE_FAULT_OVERFLOW, 0}, {LEFT, 0xFFFFFFFF, 7, PURE_FAULT_NONE, INT64_C(0x7FFFFFFF80)},
        {LEFT, -3, 2, PURE_FAULT_NONE, -12}, {LEFT, 5, -1, PURE_FAULT_LIMIT, 0}, {LEFT, 5, 64, PURE_FAULT_LIMIT, 0},
        {RIGHT, 7, 1, PURE_FAULT_NONE, 3}, {RIGHT, -7, 1, PURE_FAULT_NONE, -4}, {RIGHT, INT64_MIN, 63, PURE_FAULT_NONE, -1},
        {RIGHT, -1, 63, PURE_FAULT_NONE, -1}, {RIGHT, 1, 63, PURE_FAULT_NONE, 0}, {RIGHT, INT64_MAX, 0, PURE_FAULT_NONE, INT64_MAX},
        {RIGHT, 5, 64, PURE_FAULT_LIMIT, 0}, {RIGHT, 5, -64, PURE_FAULT_LIMIT, 0},
    };
    for (size_t k = 0; k < LEN(cases); ++k) {
        O code[] = {IC(cases[k].a), IC(cases[k].b), {BITS, cases[k].how, 0}, {ST, 1, 0}};
        Ran ran = run_code(code, LEN(code));
        if (ran.fault != cases[k].fault || ran.values[1].i64 != (cases[k].fault ? 0 : cases[k].result)) {
            fprintf(stderr, "bits case %zu: fault %d, %" PRId64 "\n", k, (int)ran.fault, ran.values[1].i64);
            abort();
        }
    }
    /* A byte at a position: of a constant's bytes, of a text state's, by a number or an i64. */
    static const struct { uint32_t text_state; uint32_t as_i64; double at; PureFault fault; int64_t byte; } reads[] = {
        {0, 0, 0, PURE_FAULT_NONE, 'A'}, {0, 0, 3, PURE_FAULT_NONE, 0xA9}, {0, 1, 2, PURE_FAULT_NONE, 0xC3},
        {0, 0, 4, PURE_FAULT_LIMIT, 0}, {0, 0, -1, PURE_FAULT_LIMIT, 0}, {0, 0, 1.5, PURE_FAULT_LIMIT, 0},
        {0, 1, 4, PURE_FAULT_LIMIT, 0}, {0, 1, -1, PURE_FAULT_LIMIT, 0}, {1, 0, 1, PURE_FAULT_NONE, 'z'},
        {1, 1, 3, PURE_FAULT_NONE, 0xA9}, {1, 0, 4, PURE_FAULT_LIMIT, 0},
    };
    for (size_t k = 0; k < LEN(reads); ++k) {
        O position = reads[k].as_i64 ? IC((int64_t)reads[k].at) : (O){C, 0, reads[k].at};
        O code[] = {reads[k].text_state ? (O){TL, 1, 0} : (O){TK, 0, 0}, {PURE_OP_BYTES_FROM, 0, 0}, position,
                    {BAT, reads[k].as_i64, 0}, {ST, 3, 0}};
        Byted ran = run_bytes(code, LEN(code));
        if (ran.fault != reads[k].fault || ran.i != reads[k].byte) {
            fprintf(stderr, "byte read %zu: fault %d, %" PRId64 "\n", k, (int)ran.fault, ran.i);
            abort();
        }
    }
    /* Bytes of a list of one i64: said = text(bytes(list)); a value past a byte fails and leaves nothing. */
    O made[] = {IC('H'), {LA, 2, 0}, IC('i'), {LA, 2, 0}, {BOF, 2, 0}, {PURE_OP_TEXT_FROM, 0, 0}, {PURE_OP_TEXT_SET, 4, 0}};
    Byted ran = run_bytes(made, LEN(made));
    CHECK(ran.fault == PURE_FAULT_NONE && !strcmp(ran.said, "Hi") && ran.count == 2);
    O none[] = {{BOF, 2, 0}, {PURE_OP_TEXT_FROM, 0, 0}, {PURE_OP_TEXT_SET, 4, 0}};
    ran = run_bytes(none, LEN(none));
    CHECK(ran.fault == PURE_FAULT_NONE && !strcmp(ran.said, "") && ran.count == 0);
    O past[] = {IC('H'), {LA, 2, 0}, IC(256), {LA, 2, 0}, {BOF, 2, 0}, {PURE_OP_TEXT_FROM, 0, 0}, {PURE_OP_TEXT_SET, 4, 0}};
    ran = run_bytes(past, LEN(past));
    CHECK(ran.fault == PURE_FAULT_LIMIT && !strcmp(ran.said, "") && ran.count == 0);
    O negative[] = {IC(-1), {LA, 2, 0}, {BOF, 2, 0}, {PURE_OP_TEXT_FROM, 0, 0}, {PURE_OP_TEXT_SET, 4, 0}};
    CHECK(run_bytes(negative, LEN(negative)).fault == PURE_FAULT_LIMIT);
    /* An item by position: i = bytes[at].v, where there is one. */
    static const struct { double at; PureFault fault; int64_t v; } items[] = {
        {0, PURE_FAULT_NONE, 7}, {1, PURE_FAULT_NONE, 9}, {2, PURE_FAULT_NOT_FOUND, 0}, {-1, PURE_FAULT_NOT_FOUND, 0},
        {0.5, PURE_FAULT_LIMIT, 0}};
    for (size_t k = 0; k < LEN(items); ++k) {
        O code[] = {IC(7), {LA, 2, 0}, IC(9), {LA, 2, 0}, {C, 0, items[k].at}, {XAT, 2, 0}, {IG, ITEM(2, 0), 0}, {ST, 3, 0}};
        Byted at = run_bytes(code, LEN(code));
        CHECK(at.fault == items[k].fault && at.i == items[k].v && at.count == (items[k].fault ? 0 : 2));
    }
    /* Refused: bits of numbers or of no such kind, a byte of a text, by a position of the other type or of no such kind,
     * bytes of a number state, an item by an i64's position. */
    const O refused[][4] = {
        {{C, 0, 1}, {C, 0, 2}, {BITS, AND, 0}, {ST, 0, 0}}, {IC(1), IC(2), {BITS, RIGHT + 1, 0}, {ST, 3, 0}},
        {{TK, 0, 0}, {C, 0, 0}, {BAT, 0, 0}, {ST, 3, 0}}, {{TK, 0, 0}, {PURE_OP_BYTES_FROM, 0, 0}, IC(0), {BAT, 0, 0}},
        {{TK, 0, 0}, {PURE_OP_BYTES_FROM, 0, 0}, {C, 0, 0}, {BAT, 2, 0}}, {{BOF, 0, 0}, {PURE_OP_TEXT_FROM, 0, 0}, {PURE_OP_TEXT_SET, 4, 0}},
        {IC(0), {XAT, 2, 0}, {IG, ITEM(2, 0), 0}, {ST, 3, 0}},
    };
    for (size_t k = 0; k < LEN(refused); ++k) {
        static const S nums[] = {{"n", 0}};
        static const T others[] = {{"note", 1, 0}, {"bytes", 3, 0}, {"i", PURE_STATE_I64, 0}, {"said", 1, 1}};
        static const RF byte_fields[] = {{"v", 3, 0}};
        static const R records[] = {{"Byte", byte_fields, 1}};
        static const char *const constants[] = {"Az\xc3\xa9", ""};
        static const I inputs[] = {KEY("go", "KeyG")};
        uint32_t length = k == 5 ? 3 : 4;
        H handler = {1, 0, length};
        Spec spec = {nums, 1, inputs, 1, NULL, 0, &handler, 1, refused[k], length, NULL, 0, others, LEN(others), constants,
                     LEN(constants), NULL, 0, records, 1, NULL, 0};
        if (load(spec, &module_b) == PURE_OK) { fprintf(stderr, "bits and bytes: refused %zu loaded\n", k); abort(); }
    }
    printf("{\"bits_and_bytes\":{\"bits\":%zu,\"reads\":%zu,\"refused\":%zu}}\n", LEN(cases), LEN(reads), LEN(refused));
}

/* ---- Formats, slices and values held by name (docs/design/027, stage 2a) --
 * NUM_TEXT writes a number, a bool or an i64 in a width, with zeros or
 * spaces, in a base or with fixed digits; an integer's form fails as limit
 * on a number that is not whole. BYTES_SLICE takes whole positions inside
 * the value; BYTE makes one byte. BYTES_LOAD and BYTES_STORE keep bytes
 * and texts by name for the event (a text's slots hold text alone): set,
 * appended, set to a part of itself, counted in the room, empty at the next
 * event; a reply of one is kept as it was. */
enum { NTX = PURE_OP_NUM_TEXT, SLC = PURE_OP_BYTES_SLICE, BYT = PURE_OP_BYTE, HLD = PURE_OP_BYTES_LOAD, HST = PURE_OP_BYTES_STORE };
#define FMT(type, width, zeros, form, digits) ((uint32_t)(type) | (uint32_t)(width) << PURE_FORMAT_WIDTH | \
    ((zeros) ? (uint32_t)PURE_FORMAT_ZEROS : 0) | (uint32_t)(form) << PURE_FORMAT_FORM | (uint32_t)(digits) << PURE_FORMAT_DIGITS)

static void formats_and_held(void) {
    static const struct { int i64; double value; uint32_t format; PureFault fault; const char *text; } shown[] = {
        {1, 255, FMT(2, 0, 0, PURE_FORMAT_HEX, 0), PURE_FAULT_NONE, "ff"},
        {1, 255, FMT(2, 8, 1, PURE_FORMAT_HEX, 0), PURE_FAULT_NONE, "000000ff"},
        {1, -255, FMT(2, 6, 1, PURE_FORMAT_HEX_UPPER, 0), PURE_FAULT_NONE, "-000FF"},
        {1, -5, FMT(2, 4, 0, PURE_FORMAT_DECIMAL, 0), PURE_FAULT_NONE, "  -5"},
        {1, 5, FMT(2, 0, 0, PURE_FORMAT_BINARY, 0), PURE_FAULT_NONE, "101"},
        {1, 8, FMT(2, 0, 0, PURE_FORMAT_OCTAL, 0), PURE_FAULT_NONE, "10"},
        {1, 0, FMT(2, 3, 1, PURE_FORMAT_PLAIN, 0), PURE_FAULT_NONE, "000"},
        {0, 2.5, FMT(0, 0, 0, PURE_FORMAT_FIXED, 3), PURE_FAULT_NONE, "2.50"},
        {0, 1.005, FMT(0, 0, 0, PURE_FORMAT_FIXED, 3), PURE_FAULT_NONE, "1.00"}, /* the binary value is below 1.005 */
        {0, -0.0, FMT(0, 0, 0, PURE_FORMAT_FIXED, 2), PURE_FAULT_NONE, "0.0"},
        {0, -1.25, FMT(0, 7, 1, PURE_FORMAT_FIXED, 2), PURE_FAULT_NONE, "-0001.2"},
        {0, 3, FMT(0, 0, 0, PURE_FORMAT_FIXED, 0), PURE_FAULT_NONE, "3.000000"},
        {0, 0.1, FMT(0, 6, 0, PURE_FORMAT_PLAIN, 0), PURE_FAULT_NONE, "   0.1"},
        {0, 42, FMT(0, 0, 0, PURE_FORMAT_HEX, 0), PURE_FAULT_NONE, "2a"},
        {0, 2.5, FMT(0, 0, 0, PURE_FORMAT_HEX, 0), PURE_FAULT_LIMIT, ""},
        {0, 1, FMT(1, 6, 0, PURE_FORMAT_PLAIN, 0), PURE_FAULT_NONE, "  true"},
    };
    for (size_t k = 0; k < LEN(shown); ++k) {
        O value = shown[k].i64 ? IC((int64_t)shown[k].value) : (O){C, shown[k].format & 3, shown[k].value};
        O code[] = {value, {NTX, shown[k].format, 0}, {PURE_OP_TEXT_SET, 4, 0}};
        Byted ran = run_bytes(code, LEN(code));
        if (ran.fault != shown[k].fault || strcmp(ran.said, shown[k].text)) {
            fprintf(stderr, "format %zu: fault %d, '%s'\n", k, (int)ran.fault, ran.said);
            abort();
        }
    }
    /* Slices of a constant's bytes ("Az\xc3\xa9") and of a text state's, by numbers or i64s; one byte. */
    static const struct { uint32_t text_state, as_i64; double from, to; PureFault fault; const char *text; } slices[] = {
        {0, 0, 0, 2, PURE_FAULT_NONE, "Az"}, {0, 1, 2, 4, PURE_FAULT_NONE, "\xc3\xa9"}, {1, 0, 1, 1, PURE_FAULT_NONE, ""},
        {1, 1, 0, 4, PURE_FAULT_NONE, "Az\xc3\xa9"}, {0, 0, 2, 1, PURE_FAULT_LIMIT, ""}, {0, 0, 0, 5, PURE_FAULT_LIMIT, ""},
        {0, 0, 0.5, 2, PURE_FAULT_LIMIT, ""}, {0, 1, -1, 2, PURE_FAULT_LIMIT, ""}, {0, 0, 2, 3, PURE_FAULT_ENCODING, ""},
    };
    for (size_t k = 0; k < LEN(slices); ++k) {
        O from = slices[k].as_i64 ? IC((int64_t)slices[k].from) : (O){C, 0, slices[k].from};
        O to = slices[k].as_i64 ? IC((int64_t)slices[k].to) : (O){C, 0, slices[k].to};
        O code[] = {slices[k].text_state ? (O){TL, 1, 0} : (O){TK, 0, 0}, {PURE_OP_BYTES_FROM, 0, 0}, from, to,
                    {SLC, slices[k].as_i64, 0}, {PURE_OP_TEXT_FROM, 0, 0}, {PURE_OP_TEXT_SET, 4, 0}};
        Byted ran = run_bytes(code, LEN(code));
        CHECK(ran.fault == slices[k].fault && !strcmp(ran.said, slices[k].text));
    }
    O bytes_joined[] = {IC('O'), {BYT, 0, 0}, IC('k'), {BYT, 0, 0}, {CAT, 1, 0}, {PURE_OP_TEXT_FROM, 0, 0}, {PURE_OP_TEXT_SET, 4, 0}};
    CHECK(!strcmp(run_bytes(bytes_joined, LEN(bytes_joined)).said, "Ok"));
    O past[] = {IC(256), {BYT, 0, 0}, {PURE_OP_TEXT_FROM, 0, 0}, {PURE_OP_TEXT_SET, 4, 0}};
    CHECK(run_bytes(past, LEN(past)).fault == PURE_FAULT_LIMIT);
    /* Held: bytes slot 3 set to "Az", appended "Az\xc3\xa9", set to a part of itself, then shown; a text slot. */
    O held[] = {{TK, 0, 0}, {PURE_OP_BYTES_FROM, 0, 0}, {C, 0, 0}, {C, 0, 2}, {SLC, 0, 0}, {HST, 3, 0},
                {TK, 0, 0}, {PURE_OP_BYTES_FROM, 0, 0}, {HST, 3 | PURE_HELD_APPEND, 0},
                {HLD, 3, 0}, {HLD, 3, 0}, {CAT, 1, 0}, {HST, 3, 0},                              /* "AzAz\xc3\xa9" twice */
                {HLD, 3, 0}, {C, 0, 1}, {C, 0, 6}, {SLC, 0, 0}, {HST, 3 | PURE_HELD_APPEND, 0},   /* itself, appended */
                {HLD, 3, 0}, {C, 0, 2}, {C, 0, 8}, {SLC, 0, 0}, {HST, 3, 0},                      /* a part of itself */
                {HLD, 3, 0}, {PURE_OP_TEXT_FROM, 0, 0}, {HST, 20, 0}, {HLD, 20, 0}, {PURE_OP_TEXT_SET, 4, 0}};
    Byted ran = run_bytes(held, LEN(held));
    CHECK(ran.fault == PURE_FAULT_NONE && !strcmp(ran.said, "Az\xc3\xa9" "Az"));
    /* Held values start empty each event; doubling past the room fails as memory and leaves nothing. */
    static const S nums[] = {{"n", 0}};
    static const T others[] = {{"note", 1, 0}, {"bytes", 3, 0}, {"i", PURE_STATE_I64, 0}, {"said", 1, 1}};
    static const RF byte_fields[] = {{"v", 3, 0}};
    static const R records[] = {{"Byte", byte_fields, 1}};
    static const char *const constants[] = {"Az\xc3\xa9", ""};
    static const I inputs[] = {KEY("go", "KeyG"), KEY("grow", "KeyH"), {"ask", NULL, PURE_KIND_MESSAGE, NULL, 0}};
    static const O code[] = {
        /* 0 go: held5 = held5 + bytes(note); said = text(held5) */
        {HLD, 5, 0}, {TK, 0, 0}, {PURE_OP_BYTES_FROM, 0, 0}, {CAT, 1, 0}, {HST, 5, 0}, {HLD, 5, 0}, {PURE_OP_TEXT_FROM, 0, 0},
        {PURE_OP_TEXT_SET, 4, 0},
        /* 8 grow: held6 = bytes(note); for 40 rounds held6 = held6 + held6; said = "x" */
        {TK, 0, 0}, {PURE_OP_BYTES_FROM, 0, 0}, {HST, 6, 0}, {C, 0, 0}, {C, 0, 40}, {PURE_OP_LOOP, 0 | 2u << 16, 0},
        {HLD, 6, 0}, {HST, 6 | PURE_HELD_APPEND, 0}, {TK, 0, 0}, {PURE_OP_TEXT_SET, 4, 0},
        /* 18 ask: held7 = bytes(note); reply held7; held7 = "" */
        {TK, 0, 0}, {PURE_OP_BYTES_FROM, 0, 0}, {HST, 7, 0}, {HLD, 7, 0}, {PURE_OP_REPLY, 0, 0}, {TK, 1, 0},
        {PURE_OP_BYTES_FROM, 0, 0}, {HST, 7, 0},
    };
    static const H handlers[] = {{1, 0, 8}, {3, 8, 10}, {5, 18, 8}};
    Spec spec = {nums, 1, inputs, LEN(inputs), NULL, 0, handlers, LEN(handlers), code, LEN(code), NULL, 0, others, LEN(others),
                 constants, LEN(constants), NULL, 0, records, 1, NULL, 0};
    if (load(spec, &module_b) != PURE_OK) { fprintf(stderr, "%s\n", error); abort(); }
    PureApp app;
    CHECK(pure_app_start(&app, &module_b, (PureData){0}) == PURE_OK);
    CHECK(press(&app, 0) == PURE_OK && press(&app, 0) == PURE_OK);
    CHECK(!strcmp(state_text(&app, 4), "Az\xc3\xa9")); /* held5 began empty again */
    CHECK(press(&app, 1) == PURE_BUDGET && app.fault == PURE_FAULT_MEMORY);
    CHECK(!strcmp(state_text(&app, 4), "Az\xc3\xa9")); /* nothing changed */
    CHECK(pure_app_message(&app, 2, (PureSpan){NULL, 0}) == PURE_OK);
    Text replied = {{0}, 0};
    CHECK(pure_app_reply(&app, text_span, &replied) == PURE_OK && !strcmp(replied.text, "Az\xc3\xa9"));
    pure_app_stop(&app);
    /* Refused: bytes into a text's slot, a slot past the last, a format that does not fit its type. */
    const O refused[][3] = {
        {{TK, 0, 0}, {PURE_OP_BYTES_FROM, 0, 0}, {HST, PURE_HELD_TEXT, 0}}, {{TK, 0, 0}, {HST, 3, 0}, {C, 0, 0}},
        {{TK, 0, 0}, {HST, PURE_MODULE_HELD, 0}, {C, 0, 0}}, {{C, 1, 1}, {NTX, FMT(1, 0, 0, PURE_FORMAT_HEX, 0), 0}, {PURE_OP_TEXT_SET, 4, 0}},
        {IC(1), {NTX, FMT(2, 0, 0, PURE_FORMAT_FIXED, 2), 0}, {PURE_OP_TEXT_SET, 4, 0}},
        {IC(1), {NTX, FMT(2, 0, 0, PURE_FORMAT_HEX, 2), 0}, {PURE_OP_TEXT_SET, 4, 0}},
        {{TK, 0, 0}, {TK, 0, 0}, {CAT, 1, 0}},
    };
    for (size_t k = 0; k < LEN(refused); ++k) {
        H one = {1, 0, 3};
        Spec bad = {nums, 1, inputs, 1, NULL, 0, &one, 1, refused[k], 3, NULL, 0, others, LEN(others), constants,
                    LEN(constants), NULL, 0, records, 1, NULL, 0};
        if (load(bad, &module_b) == PURE_OK) { fprintf(stderr, "formats and held: refused %zu loaded\n", k); abort(); }
    }
    printf("{\"formats_and_held\":{\"formats\":%zu,\"slices\":%zu,\"refused\":%zu}}\n", LEN(shown), LEN(slices), LEN(refused));
}

/* ---- Lists in records (docs/design/027, stage 2b) ------------------------
 * A Node's kids are a list of Leaf. A list made a value (LIST_VALUE) is a
 * copy; a value stored in a second cell shares the list, and an append into
 * either (APPEND_IN) makes that cell's own first; a register (LIST_BIND)
 * reads the list as it was bound, appends after it go to a copy. */
enum { LV = PURE_OP_LIST_VALUE, BIND = PURE_OP_LIST_BIND, AIN = PURE_OP_APPEND_IN };
#define NEXT_DIGIT(list) {L, 0, 0}, {C, 0, 10}, {MUL, 0, 0}, {LC, list, 0}, {ADD, 0, 0}, {ST, 0, 0}

static void lists_in_records(void) {
    static const S nums[] = {{"n", 0}};
    static const RF leaf_fields[] = {{"v", 3, 0}};
    static const RF node_fields[] = {{"name", 3, 0}, {"kids", 4, 0}}; /* kids: a list of record 0, Leaf */
    static const R records[] = {{"Leaf", leaf_fields, 1}, {"Node", node_fields, 2}};
    static const T others[] = {{"let.nodes", 3 | (uint32_t)PURE_LIST_ROWS << 8, 1}, {"let.leaves", 3 | (uint32_t)PURE_LIST_ROWS << 8, 0},
                               {"let.view", 3 | (uint32_t)(PURE_LIST_ROWS | PURE_LIST_BOUND) << 8, 0}, {"i", PURE_STATE_I64, 0}};
    static const I inputs[] = {KEY("go", "KeyG")};
    const O code[] = {
        {C, 0, 0}, {ST, 0, 0},                                                  /* 0 n = 0 */
        IC(7), {LA, 2, 0}, IC(9), {LA, 2, 0},                                   /* 2 leaves = [7, 9] */
        IC(1), {LV, 2, 0}, {LA, 1, 0},                                          /* 6 nodes.append(Node { 1, leaves }) */
        IC(11), {LA, 2, 0},                                                     /* 9 leaves.append(11): kids keep 2 */
        {XF, 1, 0}, IC(5), {AIN, ITEM(1, 1), 0},                                /* 11 nodes[0].kids += 5 */
        IC(2), {XF, 1, 0}, {IG, ITEM(1, 1), 0}, {LA, 1, 0},                     /* 14 nodes.append(Node { 2, nodes[0].kids }) */
        {C, 0, 1}, {XAT, 1, 0}, IC(6), {AIN, ITEM(1, 1), 0},                    /* 18 nodes[1].kids += 6: its own first */
        {XF, 1, 0}, {IG, ITEM(1, 1), 0}, {BIND, 3, 0},                          /* 22 view = nodes[0].kids (3) */
        NEXT_DIGIT(3),                                                          /* 25 */
        {XF, 1, 0}, IC(8), {AIN, ITEM(1, 1), 0},                                /* 31 nodes[0].kids += 8: bound, so a copy */
        NEXT_DIGIT(3),                                                          /* 34 the view still 3 */
        {XF, 1, 0}, {IG, ITEM(1, 1), 0}, {BIND, 3, 0}, NEXT_DIGIT(3),           /* 40 nodes[0].kids now 4 */
        {C, 0, 1}, {XAT, 1, 0}, {IG, ITEM(1, 1), 0}, {BIND, 3, 0}, NEXT_DIGIT(3), /* 49 nodes[1].kids: 7 9 5 6 */
        {FOLD, FOLDING(PURE_FOLD_SUM | PURE_FOLD_INTEGER, 3, 3), 0}, {ROW, 3, 0}, {IG, ITEM(3, 0), 0}, {C, 1, 1}, {ST, 4, 0},
        NEXT_DIGIT(2),                                                          /* 64 leaves: 3 */
    };
    H handler = {1, 0, LEN(code)};
    Spec spec = {nums, 1, inputs, 1, NULL, 0, &handler, 1, code, LEN(code), NULL, 0, others, LEN(others), NULL, 0, NULL, 0,
                 records, 2, NULL, 0};
    if (load(spec, &module_b) != PURE_OK) { fprintf(stderr, "%s\n", error); abort(); }
    PureApp app;
    CHECK(pure_app_start(&app, &module_b, (PureData){0}) == PURE_OK);
    for (int round = 0; round < 3; ++round) { /* each event begins with its lists and theirs empty */
        CHECK(press(&app, 0) == PURE_OK);
        if (app.values[0].num != 33443 || app.values[4].i64 != 27) {
            fprintf(stderr, "lists in records: n %g, i %" PRId64 "\n", app.values[0].num, app.values[4].i64);
            abort();
        }
        CHECK(pure_list_count(app.lists[1]) == 0 && app.lists[3] && pure_list_count(app.lists[3]) == 0);
    }
    pure_app_stop(&app);
    /* Refused: a change of a register, a bind to a list not bound, an append into a field not a list, a list of another
     * record; a stored list of records holding lists. */
    const O refused[][4] = {
        {IC(1), {LA, 3, 0}, {C, 0, 0}, {ST, 0, 0}},
        {{XF, 1, 0}, {IG, ITEM(1, 1), 0}, {BIND, 2, 0}, {C, 0, 0}},
        {{XF, 1, 0}, IC(1), {AIN, ITEM(1, 0), 0}, {C, 0, 0}},
        {IC(1), {LV, 1, 0}, {LA, 1, 0}, {C, 0, 0}},
        {{LV, 4, 0}, {C, 0, 0}, {C, 0, 0}, {C, 0, 0}},
    };
    for (size_t k = 0; k < LEN(refused); ++k) {
        H one = {1, 0, 3};
        Spec bad = {nums, 1, inputs, 1, NULL, 0, &one, 1, refused[k], 3, NULL, 0, others, LEN(others), NULL, 0, NULL, 0,
                    records, 2, NULL, 0};
        if (load(bad, &module_b) == PURE_OK) { fprintf(stderr, "lists in records: refused %zu loaded\n", k); abort(); }
    }
    static const T stored[] = {{"nodes", 3, 1}};
    static const O nothing[] = {{C, 0, 0}, {ST, 0, 0}};
    H two = {1, 0, 2};
    Spec kept = {nums, 1, inputs, 1, NULL, 0, &two, 1, nothing, 2, NULL, 0, stored, 1, NULL, 0, NULL, 0, records, 2, NULL, 0};
    CHECK(load(kept, &module_b) == PURE_OK); /* a stored list's records hold lists too since 027's stage 3 */
    printf("{\"lists_in_records\":{\"rounds\":3,\"refused\":%zu}}\n", LEN(refused));
}

/* Functions and characters (027's 2c): a function calls itself; each call has its own locals, lists, registers and
 * held values (a caller's as they were when it returns); a walk goes on around a call; calls count as work and memory;
 * the verifier keeps to a function's types and to what a walk around a call keeps to. */
enum { CALL = PURE_OP_CALL, RET = PURE_OP_RETURN, CHAT = PURE_OP_CHAR_AT, CHAR_ = PURE_OP_CHAR, FN_TEXT = 1, FN_I64 = 3,
       FN_LIST = 4 };
#define I64_LOCAL(slot) ((uint32_t)(slot) | 3u << 8)
#define WALK_OF(list, length) ((uint32_t)(list) << 8 | (uint32_t)(length) << 16)
#define LIST_LOCAL(slot, record) ((uint32_t)(slot) | 4u << 8 | (uint32_t)(record) << 16)
static void functions_and_characters(void) {
    static const S nums[] = {{"n", 0}};
    static const RF leaf_fields[] = {{"v", 3, 0}};
    static const RF node_fields[] = {{"v", 3, 0}, {"kids", 4, 1}}; /* kids: a list of Node */
    static const R records[] = {{"Leaf", leaf_fields, 1}, {"Node", node_fields, 2}};
    enum { IV = 1, SAID, OWN, NODES, KIDS, R_, STORED, NONE };
    static const T others[] = {{"i", PURE_STATE_I64, 0}, {"said", 1, 0}, {"let.own", 3 | (uint32_t)PURE_LIST_ROWS << 8, 0},
                               {"let.nodes", 3 | (uint32_t)PURE_LIST_ROWS << 8, 1}, {"let.kids", 3 | (uint32_t)PURE_LIST_ROWS << 8, 1},
                               {"let.r", 3 | (uint32_t)(PURE_LIST_ROWS | PURE_LIST_BOUND) << 8, 1}, {"stored", 3, 0},
                               {"let.none", 3 | (uint32_t)PURE_LIST_ROWS << 8, 1}};
    static const char *const constants[] = {"", "a\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80"}; /* a é € 😀 */
    static const I inputs[] = {KEY("fib", "KeyF"), KEY("tree", "KeyT"), KEY("own", "KeyO"), KEY("held", "KeyH"),
                               KEY("chars", "KeyC"), KEY("deep", "KeyD")};
    enum { LS_ = PURE_OP_LOCAL_STORE, LL_ = PURE_OP_LOCAL_LOAD };
    const O code[] = {
        /* 0 fib(n): n < 2 ? n : fib(n - 1) + fib(n - 2) */
        {LS_, I64_LOCAL(0), 0}, {LL_, I64_LOCAL(0), 0}, IC(2), {ICMP, PURE_LESS, 0}, {JZ, 7, 0},
        {LL_, I64_LOCAL(0), 0}, {RET, 0, 0},
        {LL_, I64_LOCAL(0), 0}, IC(1), {ISUB, 0, 0}, {CALL, 0, 0}, {LL_, I64_LOCAL(0), 0}, IC(2), {ISUB, 0, 0}, {CALL, 0, 0},
        {IADD, 0, 0}, {RET, 0, 0},
        /* 17 sum(xs): the v of every node in xs and under it, by a walk of its register that calls itself */
        {BIND, R_, 0}, IC(0), {LS_, I64_LOCAL(0), 0}, {EACH, WALK_OF(R_, 9), 0},
        {LL_, I64_LOCAL(0), 0}, {ROW, R_, 0}, {IG, ITEM(R_, 0), 0}, {IADD, 0, 0}, {ROW, R_, 0}, {IG, ITEM(R_, 1), 0}, {CALL, 1, 0},
        {IADD, 0, 0}, {LS_, I64_LOCAL(0), 0},
        {LL_, I64_LOCAL(0), 0}, {RET, 1, 0},
        /* 32 own(n): own.append(n); n == 0 ? own.count : own(n - 1) * 10 + own.count (each call's own list holds one) */
        {LS_, I64_LOCAL(0), 0}, {LL_, I64_LOCAL(0), 0}, {LA, OWN, 0}, {LL_, I64_LOCAL(0), 0}, IC(0), {ICMP, PURE_EQUAL, 0},
        {JZ, 42, 0}, {LC, OWN, 0}, {IFROM, 0, 0}, {RET, 2, 0},
        {LL_, I64_LOCAL(0), 0}, IC(1), {ISUB, 0, 0}, {CALL, 2, 0}, IC(10), {IMUL, 0, 0}, {LC, OWN, 0}, {IFROM, 0, 0}, {IADD, 0, 0},
        {RET, 2, 0},
        /* 52 held(t, k): s = t + "k"; k == 0 ? s : held(s, k - 1) + s (each call's s its own) */
        {LS_, I64_LOCAL(0), 0}, {LL_, I64_LOCAL(0), 0}, {NTX, 2, 0}, {CAT, 0, 0}, {HST, 16, 0},
        {LL_, I64_LOCAL(0), 0}, IC(0), {ICMP, PURE_EQUAL, 0}, {JZ, 63, 0}, {HLD, 16, 0}, {RET, 3, 0},
        {HLD, 16, 0}, {LL_, I64_LOCAL(0), 0}, IC(1), {ISUB, 0, 0}, {CALL, 3, 0}, {HLD, 16, 0}, {CAT, 0, 0}, {RET, 3, 0},
        /* 71 deep(n): deep(n + 1), never returning */
        IC(1), {IADD, 0, 0}, {CALL, 4, 0}, {RET, 4, 0},
        /* 75 handler fib: i = fib(20) */
        IC(20), {CALL, 0, 0}, {ST, IV, 0},
        /* 78 handler tree: kids = [4, 1 [2 [4], 3]]; i = sum(kids) + r[0].v, r bound by the handler around the calls */
        IC(4), {LV, NONE, 0}, {LA, KIDS, 0}, IC(2), {LV, KIDS, 0}, {LA, NODES, 0}, IC(3), {LV, NONE, 0}, {LA, NODES, 0},
        {LV, NONE, 0}, {CALL, 1, 0}, {ST, IV, 0},                                           /* sum of nothing: 0 */
        IC(1), {LV, NODES, 0}, {LA, KIDS, 0}, {LV, KIDS, 0}, {BIND, R_, 0},
        {LV, KIDS, 0}, {LS_, LIST_LOCAL(0, 1), 0}, {LL_, LIST_LOCAL(0, 1), 0}, {CALL, 1, 0}, /* by way of a local */
        {XF, R_, 0}, {IG, ITEM(R_, 0), 0}, {IADD, 0, 0}, {ST, IV, 0},                    /* 14 + 4 */
        /* 103 handler own: own = [7]; i = own(3); n = own.count */
        IC(7), {LA, OWN, 0}, IC(3), {CALL, 2, 0}, {ST, IV, 0}, {LC, OWN, 0}, {ST, 0, 0},
        /* 110 handler held: said = held("", 2) */
        {TK, 0, 0}, IC(2), {CALL, 3, 0}, {PURE_OP_TEXT_SET, SAID, 0},
        /* 114 handler chars: i = the code points at 0, 1, 3 and 6, packed; said = char(...) joined, the constant again */
        {TK, 1, 0}, IC(0), {CHAT, 0, 0}, {TK, 1, 0}, IC(1), {CHAT, 0, 0}, {IADD, 0, 0}, {TK, 1, 0}, IC(3), {CHAT, 0, 0},
        {IADD, 0, 0}, {TK, 1, 0}, IC(6), {CHAT, 0, 0}, {IADD, 0, 0}, {ST, IV, 0},
        IC(0x61), {CHAR_, 0, 0}, IC(0xE9), {CHAR_, 0, 0}, {CAT, 0, 0}, IC(0x20AC), {CHAR_, 0, 0}, {CAT, 0, 0}, IC(0x1F600), {CHAR_, 0, 0},
        {CAT, 0, 0}, {PURE_OP_TEXT_SET, SAID, 0},
        /* 142 handler deep: own = [5]; deep(0) */
        IC(5), {LA, OWN, 0}, IC(0), {CALL, 4, 0}, {ST, IV, 0},
    };
    static const FN fns[] = {{0, 17, 1, 1, {FN_I64, FN_I64}, {0}}, {17, 15, 1, 1, {FN_LIST, FN_I64}, {1, 0}},
                             {32, 20, 1, 1, {FN_I64, FN_I64}, {0}}, {52, 19, 2, 1, {FN_TEXT, FN_I64, FN_TEXT}, {0}},
                             {71, 4, 1, 1, {FN_I64, FN_I64}, {0}}};
    static const H handlers[] = {{1, 75, 3}, {3, 78, 25}, {5, 103, 7}, {7, 110, 4}, {9, 114, 28}, {11, 142, 5}};
    CHECK(LEN(code) == 147);
    Spec spec = {nums, 1, inputs, LEN(inputs), NULL, 0, handlers, LEN(handlers), code, LEN(code), NULL, 0,
                 others, LEN(others), constants, LEN(constants), NULL, 0, records, 2, NULL, 0};
    if (load_functions(spec, fns, LEN(fns), &module_b) != PURE_OK) { fprintf(stderr, "functions: %s\n", error); abort(); }
    CHECK(module_b.state_words == 1 && module_b.function_lists[2] == UINT64_C(1) << OWN &&
          module_b.function_registers[1] == UINT64_C(1) << R_ && module_b.function[3].held == UINT32_C(1) << 16 &&
          module_b.runner == 2);
    PureApp app;
    CHECK(pure_app_start(&app, &module_b, (PureData){0}) == PURE_OK);
    for (int round = 0; round < 3; ++round) { /* the frames' room used again, each event alike */
        CHECK(press(&app, 0) == PURE_OK && app.values[IV].i64 == 6765);
        CHECK(press(&app, 1) == PURE_OK && app.values[IV].i64 == 18);
        CHECK(press(&app, 2) == PURE_OK && app.values[IV].i64 == 1111 && app.values[0].num == 1);
        CHECK(press(&app, 3) == PURE_OK && !strcmp(state_text(&app, SAID), "210212"));
        CHECK(press(&app, 4) == PURE_OK && app.values[IV].i64 == 0x61 + 0xE9 + 0x20AC + 0x1F600 &&
              !strcmp(state_text(&app, SAID), constants[1]));
    }
    /* A call without end: the room runs out (MEMORY) where the work does not; with a smaller work budget, WORK. Either
     * way nothing changed, and the lists are the app's own again. */
    uint64_t before = (uint64_t)app.values[IV].i64;
    CHECK(press(&app, 5) == PURE_BUDGET && app.fault == PURE_FAULT_MEMORY);
    CHECK((uint64_t)app.values[IV].i64 == before && pure_list_count(app.lists[OWN]) == 0 && app.calls.at == 0 && !app.calls.used);
    pure_app_work(&app, 100000);
    CHECK(press(&app, 5) == PURE_LIMIT && app.fault == PURE_FAULT_WORK);
    CHECK(press(&app, 2) == PURE_OK && app.values[IV].i64 == 1111 && app.values[0].num == 1);
    pure_app_stop(&app);
    /* Characters that fail: within one (é at 2), past the end, a surrogate, past U+10FFFF, below 0. */
    const O chars_bad[][3] = {{{TK, 1, 0}, IC(2), {CHAT, 0, 0}}, {{TK, 1, 0}, IC(10), {CHAT, 0, 0}},
                              {IC(0xD800), {CHAR_, 0, 0}, {PURE_OP_TEXT_SET, SAID, 0}}, {IC(0x110000), {CHAR_, 0, 0}, {PURE_OP_TEXT_SET, SAID, 0}},
                              {IC(-1), {CHAR_, 0, 0}, {PURE_OP_TEXT_SET, SAID, 0}}};
    static const PureFault chars_fault[] = {PURE_FAULT_LIMIT, PURE_FAULT_LIMIT, PURE_FAULT_ENCODING, PURE_FAULT_LIMIT, PURE_FAULT_LIMIT};
    for (size_t k = 0; k < LEN(chars_bad); ++k) {
        O one[4] = {chars_bad[k][0], chars_bad[k][1], chars_bad[k][2], {ST, IV, 0}};
        H go = {1, 0, chars_bad[k][2].op == CHAT ? 4 : 3};
        Spec bad = {nums, 1, inputs, 1, NULL, 0, &go, 1, one, LEN(one), NULL, 0, others, LEN(others), constants, LEN(constants),
                    NULL, 0, records, 2, NULL, 0};
        if (load(bad, &module_a) != PURE_OK) { fprintf(stderr, "chars %zu: %s\n", k, error); abort(); }
        CHECK(pure_app_start(&app, &module_a, (PureData){0}) == PURE_OK);
        CHECK(press(&app, 0) != PURE_OK && app.fault == chars_fault[k]);
        pure_app_stop(&app);
    }
    /* Refused: no return; a result of another type; code after a return; RETURN outside a function; a call with an
     * argument of another type, in a view, or changing a list a walk around it walks; a message's field in a function;
     * a loop above values; a signature's type or record. */
    const struct { O code[6]; uint32_t count; FN fn; const char *why; } refused[] = {
        {{IC(1), {ST, IV, 0}}, 2, {0, 2, 0, 0, {0}, {0}}, "function ends without return"},
        {{{C, 0, 1}, {RET, 0, 0}}, 2, {0, 2, 0, 1, {FN_I64}, {0}}, "operand type"},
        {{{RET, 0, 0}, {C, 0, 0}}, 2, {0, 2, 0, 0, {0}, {0}}, "unreachable instruction"},
        {{{C, 0, 1}, {CALL, 0, 0}, {RET, 0, 0}}, 3, {0, 3, 1, 0, {FN_I64}, {0}}, "operand type"},
        {{IC(1), {LA, STORED, 0}, {RET, 0, 0}}, 3, {0, 3, 0, 0, {0}, {0}}, NULL},
        {{{PURE_OP_FIELD, 0, 0}, {RET, 0, 0}}, 2, {0, 2, 0, 0, {0}, {0}}, "message operation outside a message handler"},
        {{IC(0), IC(1), IC(0), IC(1), {PURE_OP_LOOP, PURE_LOOP_INTEGER, 0}, {RET, 0, 0}}, 6,
         {0, 6, 0, 0, {0}, {0}}, "loop above values"},
        {{{RET, 0, 0}}, 1, {0, 1, 0, 0, {6}, {0}}, "function signature"},
    };
    for (size_t k = 0; k < LEN(refused); ++k) {
        FN fn = refused[k].fn;
        if (k == 7) fn.params = 1;
        O all[12];
        uint32_t n = refused[k].count;
        memcpy(all, refused[k].code, n * sizeof(O));
        H go = {1, n, 1};
        all[n] = (O){C, 0, 0}; /* a handler that stores nothing: C, ST */
        all[n + 1] = (O){ST, 0, 0};
        go.length = 2;
        if (k == 4) { /* the call in a walk of the list it appends to */
            all[n] = (O){EACH, WALK_OF(STORED, 1), 0}, all[n + 1] = (O){CALL, 0, 0};
        }
        Spec bad = {nums, 1, inputs, 1, NULL, 0, &go, 1, all, n + 2, NULL, 0, others, LEN(others), constants, LEN(constants),
                    NULL, 0, records, 2, NULL, 0};
        const char *why = refused[k].why ? refused[k].why : "call that changes a list walked";
        if (load_functions(bad, &fn, 1, &module_a) == PURE_OK || !strstr(error, why)) {
            fprintf(stderr, "functions: refused %zu: %s\n", k, error);
            abort();
        }
    }
    static const O in_view[] = {{CALL, 0, 0}, {RET, 0, 0}, {RET, 0, 0}};
    static const V call_view[] = {{"v", 0, 1}};
    static const FN nothing = {1, 1, 0, 0, {0}, {0}};
    Spec viewed = {nums, 1, inputs, 1, call_view, 1, NULL, 0, in_view, 2, NULL, 0, others, LEN(others), constants, LEN(constants),
                   NULL, 0, records, 2, NULL, 0};
    CHECK(load_functions(viewed, &nothing, 1, &module_a) != PURE_OK && strstr(error, "call outside a handler or a function"));
    static const H returning = {1, 2, 1};
    Spec handler_return = {nums, 1, inputs, 1, NULL, 0, &returning, 1, in_view, 3, NULL, 0, others,
                           LEN(others), constants, LEN(constants), NULL, 0, records, 2, NULL, 0};
    CHECK(load_functions(handler_return, &nothing, 1, &module_a) != PURE_OK && strstr(error, "return outside a function"));
    /* A handler's loop above values: its block would begin above them (once past the run's stack). */
    const O above[] = {IC(0), IC(0), IC(1), {PURE_OP_LOOP, PURE_LOOP_INTEGER, 0}, {ST, IV, 0}};
    static const H whole = {1, 0, 5};
    Spec looped = {nums, 1, inputs, 1, NULL, 0, &whole, 1, above, LEN(above), NULL, 0, others, LEN(others), constants,
                   LEN(constants), NULL, 0, records, 2, NULL, 0};
    CHECK(load(looped, &module_a) != PURE_OK && strstr(error, "loop above values"));
    printf("{\"functions_and_characters\":{\"rounds\":3,\"refused\":%zu}}\n", LEN(refused) + 3);
}

/* 027's stage 3: an event changes a number and the list in a stored list's item's field (APPEND_IN: read in, appended,
 * written back); the process dies at each commit stage, or the commit fails. Starting again finds both as before or
 * both as after. */
enum { NESTED_BOARDS = 1, NESTED_NONE = 2 };
static unsigned nested_consistent(const PureApp *app) {
    unsigned n = (unsigned)app->values[0].num;
    CHECK(app->values[0].num == n && pure_list_count(app->lists[NESTED_BOARDS]) == (n > 0));
    if (!n) return 0;
    PureCell items = pure_list_get(app->lists[NESTED_BOARDS], 0, 1); /* the leaves' stored form: an i64 each */
    CHECK(items.size == 8u * n);
    for (unsigned k = 0; k < n; ++k) {
        int64_t v;
        memcpy(&v, items.text + 8 * k, 8);
        CHECK(v == (int64_t)k + 1);
    }
    return n;
}

static void nested_crashes(void) {
    enum { LC_ = PURE_OP_LIST_COUNT, LV_ = PURE_OP_LIST_VALUE, AIN_ = PURE_OP_APPEND_IN, EQ_ = PURE_OP_EQ };
    static const S nums[] = {{"n", 0}};
    static const T states[] = {{"boards", 3, 1}, {"let.none", 3 | (uint32_t)PURE_LIST_ROWS << 8, 0}};
    static const RF leaf_fields[] = {{"v", 3, 0}};
    static const RF board_fields[] = {{"id", 3, 0}, {"items", 4, 0}};
    static const R records[] = {{"Leaf", leaf_fields, 1}, {"Board", board_fields, 2}};
    static const I inputs[] = {KEY("go", "KeyG")};
    const O code[] = {
        {LC_, NESTED_BOARDS, 0}, {C, 0, 0}, {EQ_, 0, 0}, {JZ, 7, 0}, IC(0), {LV_, NESTED_NONE, 0}, {LA, NESTED_BOARDS, 0},
        {L, 0, 0}, {C, 0, 1}, {ADD, 0, 0}, {ST, 0, 0},                                      /* n = n + 1 */
        {XF, NESTED_BOARDS, 0}, {L, 0, 0}, {PURE_OP_I64_FROM, 0, 0}, {AIN_, ITEM(NESTED_BOARDS, 1), 0}, /* items += n */
    };
    H handler = {1, 0, LEN(code)};
    Spec spec = {nums, 1, inputs, 1, NULL, 0, &handler, 1, code, LEN(code), NULL, 0, states, LEN(states), NULL, 0, NULL, 0,
                 records, 2, NULL, 0};
    if (load(spec, &module_a) != PURE_OK) { fprintf(stderr, "nested crashes: %s\n", error); abort(); }
    char *path = new_store();
    PureApp app;
    CHECK(pure_app_start(&app, &module_a, pure_tree_data(path)) == PURE_OK);
    CHECK(press(&app, 0) == PURE_OK && press(&app, 0) == PURE_OK);
    CHECK(nested_consistent(&app) == 2);
    pure_app_stop(&app);
    unsigned expected = 2;
    for (int stage = PURE_AFTER_HEADER; stage <= PURE_BEFORE_DIR_SYNC + 2; ++stage) { /* the tree's stages, then the log's */
        int at = stage <= PURE_BEFORE_DIR_SYNC ? stage : PURE_LOG_BEFORE_WRITE + (stage - PURE_BEFORE_DIR_SYNC - 1);
        pid_t child = fork();
        CHECK(child >= 0);
        if (!child) {
            PureApp doomed;
            if (pure_app_start(&doomed, &module_a, pure_tree_data(path)) != PURE_OK) _exit(1);
            crash = 1;
            fail_stage = at;
            press(&doomed, 0);
            _exit(0);
        }
        int status;
        CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 77);
        if (at == PURE_BEFORE_DIR_SYNC) ++expected; /* the head was written */
        CHECK(pure_app_start(&app, &module_a, pure_tree_data(path)) == PURE_OK && nested_consistent(&app) == expected);
        pure_app_stop(&app);
    }
    crash = 0;
    CHECK(pure_app_start(&app, &module_a, pure_tree_data(path)) == PURE_OK);
    for (fail_stage = PURE_AFTER_HEADER; fail_stage <= PURE_BEFORE_RENAME; ++fail_stage) /* a failed commit: nothing */
        CHECK(press(&app, 0) == PURE_STORE_ERROR && nested_consistent(&app) == expected);
    fail_stage = PURE_LOG_BEFORE_WRITE;
    CHECK(press(&app, 0) == PURE_STORE_ERROR && nested_consistent(&app) == expected);
    fail_stage = -1;
    CHECK(press(&app, 0) == PURE_OK && nested_consistent(&app) == expected + 1);
    pure_app_stop(&app);
    CHECK(pure_app_start(&app, &module_a, pure_tree_data(path)) == PURE_OK && nested_consistent(&app) == expected + 1);
    pure_app_stop(&app);
    remove_store(path);
    printf("{\"nested_crashes\":{\"stages\":%d,\"kept\":%u}}\n", PURE_BEFORE_DIR_SYNC + 3 - PURE_AFTER_HEADER, expected + 1);
}

/* 027's stage 3: a module replaced while the app runs moves a stored and a transient list's items, and the items of the
 * lists in their fields, by field name: a field the items' record gains takes its default, a field it moves keeps its
 * values. */
static void nested_replace(void) {
    enum { LC_ = PURE_OP_LIST_COUNT, LV_ = PURE_OP_LIST_VALUE, AIN_ = PURE_OP_APPEND_IN, EQ_ = PURE_OP_EQ };
    static const S nums[] = {{"n", 0}};
    static const T states[] = {{"boards", 3, 1}, {"let.none", 3 | (uint32_t)PURE_LIST_ROWS << 8, 0}, {"cache", 3 | TR, 1}};
    static const RF leaf_fields[] = {{"v", 3, 0}};
    static const RF board_fields[] = {{"id", 3, 0}, {"items", 4, 0}};
    static const R records[] = {{"Leaf", leaf_fields, 1}, {"Board", board_fields, 2}};
    static const I inputs[] = {KEY("go", "KeyG")};
    const O code[] = {
        {LC_, 1, 0}, {C, 0, 0}, {EQ_, 0, 0}, {JZ, 10, 0}, IC(9), {LV_, 2, 0}, {LA, 1, 0}, IC(8), {LV_, 2, 0}, {LA, 3, 0},
        {L, 0, 0}, {C, 0, 1}, {ADD, 0, 0}, {ST, 0, 0},
        {XF, 1, 0}, {L, 0, 0}, {PURE_OP_I64_FROM, 0, 0}, {AIN_, ITEM(1, 1), 0}, /* boards.first.items += n */
        {XF, 3, 0}, {L, 0, 0}, {C, 0, 10}, {MUL, 0, 0}, {PURE_OP_I64_FROM, 0, 0}, {AIN_, ITEM(3, 1), 0}, /* cache's += 10n */
    };
    H handler = {1, 0, LEN(code)};
    Spec spec = {nums, 1, inputs, 1, NULL, 0, &handler, 1, code, LEN(code), NULL, 0, states, LEN(states), NULL, 0, NULL, 0,
                 records, 2, NULL, 0};
    if (load(spec, &module_a) != PURE_OK) { fprintf(stderr, "nested replace: %s\n", error); abort(); }
    PureApp app;
    CHECK(pure_app_start(&app, &module_a, (PureData){0}) == PURE_OK);
    for (int k = 0; k < 3; ++k) CHECK(press(&app, 0) == PURE_OK);
    /* The leaves gain w (7 by default) and the boards' fields swap places. */
    static const RF leaf_wider[] = {{"v", 3, 0}, {"w", 0, 7}};
    static const RF board_swapped[] = {{"items", 4, 0}, {"id", 3, 0}};
    static const R wider[] = {{"Leaf", leaf_wider, 2}, {"Board", board_swapped, 2}};
    static const O nothing[] = {{C, 0, 0}, {ST, 0, 0}};
    H two = {1, 0, 2};
    Spec next = {nums, 1, inputs, 1, NULL, 0, &two, 1, nothing, 2, NULL, 0, states, LEN(states), NULL, 0, NULL, 0, wider, 2,
                 NULL, 0};
    CHECK(load(next, &module_b) == PURE_OK);
    CHECK(pure_app_replace(&app, &module_b) == PURE_OK);
    for (unsigned list = 1; list <= 3; list += 2) { /* boards, then cache */
        CHECK(pure_list_count(app.lists[list]) == 1 && pure_list_get(app.lists[list], 0, 1).i64 == (list == 1 ? 9 : 8));
        PureCell items = pure_list_get(app.lists[list], 0, 0);
        CHECK(items.size == 3 * 16);
        for (unsigned k = 0; k < 3; ++k) {
            int64_t v;
            double w;
            memcpy(&v, items.text + 16 * k, 8);
            memcpy(&w, items.text + 16 * k + 8, 8);
            CHECK(v == (int64_t)(k + 1) * (list == 1 ? 1 : 10) && w == 7);
        }
    }
    pure_app_stop(&app);
    printf("{\"nested_replace\":{\"lists\":2,\"items\":3}}\n");
}

/* 027's 4a: a message's rows whose records hold lists. Each node's kids come in as one of the event's lists, read
 * through a register; the rows and their lists go as the event ends, and rows that are not whole fail it with nothing
 * changed. */
static size_t put_node(unsigned char *out, int64_t name, const int64_t *kids, uint32_t count) {
    memcpy(out, &name, 8);
    put32(out + 8, 8 * count);
    for (uint32_t k = 0; k < count; ++k) memcpy(out + 12 + 8 * k, &kids[k], 8);
    return 12 + 8 * (size_t)count;
}

static void nested_rows(void) {
    enum { LC_ = PURE_OP_LIST_COUNT, BIND_ = PURE_OP_LIST_BIND, NODES = 1, KIDS = 2, TOTAL = 3 };
    static const S nums[] = {{"n", 0}};
    static const RF leaf_fields[] = {{"v", 3, 0}};
    static const RF node_fields[] = {{"name", 3, 0}, {"kids", 4, 0}};
    static const R records[] = {{"Leaf", leaf_fields, 1}, {"Node", node_fields, 2}};
    static const T others[] = {{"order.nodes", 3 | (uint32_t)PURE_LIST_ROWS << 8, 1},
                               {"let.kids", 3 | (uint32_t)(PURE_LIST_ROWS | PURE_LIST_BOUND) << 8, 0}, {"total", PURE_STATE_I64, 0}};
    static const F fields[] = {{"nodes", PURE_FIELD_ROWS | NODES << 8}};
    static const I inputs[] = {{"order", NULL, PURE_KIND_MESSAGE, fields, 1}};
    static const O code[] = {
        /* 0: for x in order.nodes { total += sum(k.v for k in x.kids) } */
        {EACH, WALKING(NODES, 10), 0}, {ROW, NODES, 0}, {IG, ITEM(NODES, 1), 0}, {BIND_, KIDS, 0},
        {L, TOTAL, 0}, {FOLD, FOLDING(PURE_FOLD_SUM | PURE_FOLD_INTEGER, KIDS, 3), 0}, {ROW, KIDS, 0}, {IG, ITEM(KIDS, 0), 0},
        {C, 1, 1}, {IADD, 0, 0}, {ST, TOTAL, 0},
        /* 11: n = n * 10 + order.nodes.count */
        {L, 0, 0}, {C, 0, 10}, {MUL, 0, 0}, {LC_, NODES, 0}, {ADD, 0, 0}, {ST, 0, 0},
    };
    H handler = {1, 0, LEN(code)};
    Spec spec = {nums, 1, inputs, 1, NULL, 0, &handler, 1, code, LEN(code), NULL, 0, others, LEN(others), NULL, 0, NULL, 0,
                 records, 2, NULL, 0};
    if (load(spec, &module_a) != PURE_OK) { fprintf(stderr, "nested rows: %s\n", error); abort(); }
    PureApp app;
    CHECK(pure_app_start(&app, &module_a, (PureData){0}) == PURE_OK);
    static const int64_t first[] = {7, 9}, second[] = {5};
    unsigned char payload[64];
    size_t size = put_node(payload, 1, first, 2);
    size += put_node(payload + size, 2, second, 1);
    size += put_node(payload + size, 3, NULL, 0);
    for (int round = 1; round <= 3; ++round) { /* each event begins with its lists and theirs empty */
        CHECK(pure_app_message(&app, 0, (PureSpan){payload, size}) == PURE_OK);
        CHECK(app.values[TOTAL].i64 == 21 * round && app.values[0].num == (round == 1 ? 3 : round == 2 ? 33 : 333));
        CHECK(pure_list_count(app.lists[NODES]) == 0 && app.nested.count == 2); /* the third node's kids are none */
    }
    /* Not whole: a node's kids past the end, a kid cut short, a node cut short. */
    const size_t cuts[] = {size - 1, size - 12 - 4, 4};
    unsigned char broken[64];
    memcpy(broken, payload, size);
    put32(broken + 8, 1000);
    CHECK(pure_app_message(&app, 0, (PureSpan){broken, size}) == PURE_LIMIT && app.fault == PURE_FAULT_LIMIT);
    for (size_t k = 0; k < LEN(cuts); ++k)
        CHECK(pure_app_message(&app, 0, (PureSpan){payload, cuts[k]}) == PURE_LIMIT && app.fault == PURE_FAULT_LIMIT);
    CHECK(app.values[TOTAL].i64 == 63 && app.values[0].num == 333 && pure_list_count(app.lists[NODES]) == 0 && !app.nested.count);
    CHECK(pure_app_message(&app, 0, (PureSpan){payload, size}) == PURE_OK && app.values[TOTAL].i64 == 84);
    pure_app_stop(&app);
    printf("{\"nested_rows\":{\"rounds\":3,\"refused\":%zu}}\n", LEN(cuts) + 1);
}

/* 027's 4b: a record field holding bytes. A message's bytes (any, UTF-8 or not) go into a list's item and come back
 * as they were; a text goes where bytes go only as bytes (BYTES_FROM), and bytes never where a text goes. Replaced, a
 * text field becomes a bytes field keeping its value, and a new bytes field takes its constant. */
static void bytes_fields(void) {
    enum { BF = PURE_OP_BYTES_FROM, TC = PURE_OP_TEXT_CONST, XL_ = PURE_OP_INDEX_LAST, ITEMS = 1, BLOB = 2 };
    static const S nums[] = {{"n", 0}};
    static const char *const constants[] = {"", "zz"};
    static const RF text_fields[] = {{"t", 1, 0}};
    static const RF bytes_fields_[] = {{"t", 5, 0}, {"b", 5, 1}};
    static const R before[] = {{"Item", text_fields, 1}}, after[] = {{"Item", bytes_fields_, 2}};
    static const T others[] = {{"items", 3, 0}, {"blob", PURE_STATE_TEXT, 0}};
    static const F text_in[] = {{"t", PURE_FIELD_TEXT}}, bytes_in[] = {{"b", PURE_FIELD_BYTES}};
    static const I inputs_a[] = {{"put", NULL, PURE_KIND_MESSAGE, text_in, 1}};
    static const I inputs_b[] = {{"put", NULL, PURE_KIND_MESSAGE, bytes_in, 1}};
    /* A: items.append(Item { t: put.t }); n = items.count */
    static const O code_a[] = {{FIELD, 0, 0}, {LA, ITEMS, 0}, {LC, ITEMS, 0}, {ST, 0, 0}};
    /* B: items.append(Item { t: put.b, b: bytes("zz") + put.b }); n = items.last.b's length */
    static const O code_b[] = {{FIELD, 0, 0}, {TC, 1, 0}, {BF, 0, 0}, {FIELD, 0, 0}, {CAT, 1, 0}, {LA, ITEMS, 0},
                               {XL_, ITEMS, 0}, {IG, ITEM(ITEMS, 1), 0}, {PURE_OP_LEN, 0, 0}, {ST, 0, 0}};
    H ha = {1, 0, LEN(code_a)}, hb = {1, 0, LEN(code_b)};
    Spec a = {nums, 1, inputs_a, 1, NULL, 0, &ha, 1, code_a, LEN(code_a), NULL, 0, others, LEN(others), constants, LEN(constants),
              NULL, 0, before, 1, NULL, 0};
    Spec b = {nums, 1, inputs_b, 1, NULL, 0, &hb, 1, code_b, LEN(code_b), NULL, 0, others, LEN(others), constants, LEN(constants),
              NULL, 0, after, 1, NULL, 0};
    if (load(a, &module_a) != PURE_OK || load(b, &module_b) != PURE_OK) { fprintf(stderr, "bytes fields: %s\n", error); abort(); }
    PureApp app;
    CHECK(pure_app_start(&app, &module_a, (PureData){0}) == PURE_OK);
    CHECK(pure_app_message(&app, 0, (PureSpan){(const unsigned char *)"héllo", 6}) == PURE_OK);
    CHECK(pure_app_replace(&app, &module_b) == PURE_OK);
    PureCell t = pure_list_get(app.lists[ITEMS], 0, 0), zz = pure_list_get(app.lists[ITEMS], 0, 1);
    CHECK(t.size == 6 && !memcmp(t.text, "héllo", 6) && zz.size == 2 && !memcmp(zz.text, "zz", 2));
    static const unsigned char raw[] = {0xFF, 0x00, 0xC3, 0x28, 0x80}; /* no UTF-8 */
    CHECK(pure_app_message(&app, 0, (PureSpan){raw, sizeof(raw)}) == PURE_OK && app.values[0].num == 2 + sizeof(raw));
    PureCell back = pure_list_get(app.lists[ITEMS], 1, 0), joined = pure_list_get(app.lists[ITEMS], 1, 1);
    CHECK(back.size == sizeof(raw) && !memcmp(back.text, raw, sizeof(raw)) && joined.size == 2 + sizeof(raw) &&
          !memcmp(joined.text, "zz", 2) && !memcmp(joined.text + 2, raw, sizeof(raw)));
    pure_app_stop(&app);
    /* Refused: a text where bytes go, bytes where a text goes (the field read into a text state). */
    static const O text_in_bytes[] = {{TC, 0, 0}, {TC, 1, 0}, {LA, ITEMS, 0}, {C, 0, 0}, {ST, 0, 0}};
    static const O bytes_as_text[] = {{XL_, ITEMS, 0}, {IG, ITEM(ITEMS, 0), 0}, {PURE_OP_TEXT_SET, BLOB, 0}, {C, 0, 0}, {ST, 0, 0}};
    const O *refused[] = {text_in_bytes, bytes_as_text};
    for (size_t k = 0; k < LEN(refused); ++k) {
        H one = {1, 0, 5};
        Spec bad = {nums, 1, inputs_b, 1, NULL, 0, &one, 1, refused[k], 5, NULL, 0, others, LEN(others), constants, LEN(constants),
                    NULL, 0, after, 1, NULL, 0};
        CHECK(load(bad, &module_a) != PURE_OK && strstr(error, "operand type"));
    }
    static const T keyed[] = {{"items", 3 | 1u << 8, 0}, {"blob", PURE_STATE_TEXT, 0}}; /* keyed by t, bytes */
    H one = {1, 0, LEN(code_b)};
    Spec by_bytes = {nums, 1, inputs_b, 1, NULL, 0, &one, 1, code_b, LEN(code_b), NULL, 0, keyed, LEN(keyed), constants,
                     LEN(constants), NULL, 0, after, 1, NULL, 0};
    CHECK(load(by_bytes, &module_a) != PURE_OK && strstr(error, "bytes as a key"));
    printf("{\"bytes_fields\":{\"items\":2,\"refused\":%zu}}\n", LEN(refused) + 1);
}

/* 027's 4c: an anchor's instructions name what it is in. AT finds the place so many characters or lines from a text's
 * start, or the item so many from a list's (past the end: NOT_FOUND, not whole: LIMIT); COUNT gives it back. Refused:
 * an instruction naming a list on a cursor and a text on an index, a list's unit other than an item, a mark in a list. */
static void anchors(void) {
    enum { CAT_ = PURE_OP_CUR_AT, XAT_ = PURE_OP_INDEX_AT, NOTE = 0, TASKS = 1, AT = 2, CARET = 3 };
    static const char *const constants[] = {"ab\ncd\nef"};
    static const T states[] = {{"note", PURE_STATE_TEXT, 0}, {"tasks", PURE_STATE_LIST, 0}, {"at", PURE_STATE_INDEX, 1},
                               {"caret", PURE_STATE_CURSOR, 0}};
    static const RF fields[] = {{"v", 0, 0}};
    static const R records[] = {{"Task", fields, 1}};
    static const I inputs[] = {KEY("add", "KeyA")};
    static const O code[] = {
        {C, 0, 7}, {LA, TASKS, 0},                                                       /* 0 add: a task */
        {C, 0, 4}, {CAT_, IN(NOTE, 0), 0}, {CC, IN(NOTE, 0), 0},                        /* 2 view chars: 4 */
        {C, 0, 2}, {CAT_, IN(NOTE, 1), 0}, {CC, IN(NOTE, 0), 0},                        /* 5 view line_at: 6 */
        {C, 0, 2}, {CAT_, IN(NOTE, 1), 0}, {CC, IN(NOTE, 1), 0},                        /* 8 view line: 2 */
        {C, 0, 9}, {CAT_, IN(NOTE, 0), 0}, {CC, IN(NOTE, 0), 0},                        /* 11 view past: none */
        {C, 0, 1.5}, {CAT_, IN(NOTE, 0), 0}, {CC, IN(NOTE, 0), 0},                      /* 14 view half: none */
        {C, 0, 1}, {XAT_, TASKS, 0}, {XN, TASKS, 0},                                    /* 17 view second: 1 */
    };
    static const H handlers[] = {{1, 0, 2}};
    static const V views[] = {{"chars", 2, 3}, {"line_at", 5, 3}, {"line", 8, 3}, {"past", 11, 3}, {"half", 14, 3},
                              {"second", 17, 3}};
    Spec spec = {NULL, 0, inputs, 1, views, LEN(views), handlers, 1, code, LEN(code), NULL, 0, states, LEN(states), constants,
                 LEN(constants), NULL, 0, records, 1, NULL, 0};
    if (load(spec, &module_a) != PURE_OK) { fprintf(stderr, "anchors: %s\n", error); abort(); }
    PureApp app;
    CHECK(pure_app_start(&app, &module_a, (PureData){0}) == PURE_OK);
    double value;
    CHECK(number_view(&app, 0) == 4 && number_view(&app, 1) == 6 && number_view(&app, 2) == 2);
    CHECK(!pure_app_view(&app, 3, &value) && pure_app_view_fault(&app) == PURE_FAULT_NOT_FOUND);
    CHECK(!pure_app_view(&app, 4, &value) && pure_app_view_fault(&app) == PURE_FAULT_LIMIT);
    CHECK(!pure_app_view(&app, 5, &value) && pure_app_view_fault(&app) == PURE_FAULT_NOT_FOUND);
    for (int k = 0; k < 2; ++k) CHECK(press(&app, 0) == PURE_OK);
    CHECK(number_view(&app, 5) == 1);
    pure_app_stop(&app);
    const struct { O code[4]; uint32_t count; const char *why; } refused[] = {
        {{{CUL, CARET, 0}, {C, 0, 1}, {XM, TASKS, 0}, {XN, TASKS, 0}}, 4, "operand type"}, /* a list's move of a cursor */
        {{{XL, AT, 0}, {CC, IN(NOTE, 0), 0}}, 2, "operand type"},                          /* a text's count of an index */
        {{{C, 0, 0}, {XAT_, IN(TASKS, 1), 0}, {XN, TASKS, 0}}, 3, "no such state"},        /* a list's unit not an item */
    };
    for (size_t k = 0; k < LEN(refused); ++k) {
        V one[] = {{"v", 0, refused[k].count}};
        Spec bad = {NULL, 0, inputs, 1, one, 1, NULL, 0, refused[k].code, refused[k].count, NULL, 0, states, LEN(states),
                    constants, LEN(constants), NULL, 0, records, 1, NULL, 0};
        if (load(bad, &module_b) == PURE_OK || !strstr(error, refused[k].why)) {
            fprintf(stderr, "anchors: refused %zu: %s\n", k, error);
            abort();
        }
    }
    static const T marked[] = {{"note", PURE_STATE_TEXT, 0}, {"tasks", PURE_STATE_LIST, 0},
                               {"at", PURE_STATE_INDEX | PURE_CURSOR_STAYS << 8, 1}, {"caret", PURE_STATE_CURSOR, 0}};
    Spec mark = spec;
    mark.t = marked;
    CHECK(load(mark, &module_b) != PURE_OK && strstr(error, "state reference"));
    printf("{\"anchors\":{\"views\":6,\"refused\":%zu}}\n", LEN(refused) + 1);
}

/* List views (027's 4e): a list state as it is and filtered (its record's fields its columns, made at load, a list
 * field's cell lent in its stored form), a list a view's body builds (kept for its rows and cells until a change), a
 * map's list and bytes columns, a bytes view; refused: a computed view without its block, a record not its list's, a
 * column of a view of a record, a list column without its record, a body leaving a list of another record, a pointing
 * at a computed list. */
static bool tags_are(PureApp *app, uint32_t column, size_t row, const int64_t *want, size_t count) {
    PureSpan items;
    PureCell cells[PURE_RECORD_FIELDS];
    if (pure_app_cell_items(app, column, row, &items) != PURE_OK) return false;
    size_t at = 0, seen = 0;
    while (pure_items_next(app->module, 0, items, &at, cells))
        if (seen >= count || cells[0].i64 != want[seen++]) return false;
    return seen == count && at == items.size;
}

static void list_views(void) {
    enum { BF = PURE_OP_BYTES_FROM, TB = PURE_OP_TEXT_BYTES, ITEMS = 1, TAGS = 2, MADE = 3 };
    enum { ALL, LONG, COMPUTED, MAP, MAP_TAGS, MAP_RAW, BLOB };
    static const S nums[] = {{"n", 0}};
    static const char *const constants[] = {"", "a", "zz", "bb"};
    static const RF tag_fields[] = {{"v", 3, 0}};
    static const RF item_fields[] = {{"name", 1, 0}, {"raw", 5, 0}, {"tags", 4, 0}};
    static const R records[] = {{"Tag", tag_fields, 1}, {"Item", item_fields, 3}};
    static const T others[] = {{"items", 3, 1}, {"let.tags", 3 | (uint32_t)PURE_LIST_ROWS << 8, 0},
                               {"let.made", 3 | (uint32_t)PURE_LIST_ROWS << 8, 1}};
    static const I inputs[] = {KEY("go", "KeyG")};
    const O code[] = {
        IC(7), {LA, TAGS, 0}, IC(9), {LA, TAGS, 0},                                /* 0 tags = [7, 9] */
        {TK, 1, 0}, {TK, 2, 0}, {BF, 0, 0}, {LV, TAGS, 0}, {LA, ITEMS, 0},          /* 4 items += { a, zz, tags } */
        IC(11), {LA, TAGS, 0},                                                     /* 9 tags += 11 */
        {TK, 3, 0}, {TK, 0, 0}, {BF, 0, 0}, {LV, TAGS, 0}, {LA, ITEMS, 0},          /* 11 items += { bb, "", tags } */
        {LC, ITEMS, 0}, {ST, 0, 0},                                                /* 16 n = items.count */
        {ROW, ITEMS, 0}, {IG, ITEM(ITEMS, 0), 0}, {TB, 0, 0}, {C, 0, 1}, {GT, 0, 0}, /* 18 long: a name past a byte */
        IC(5), {LA, TAGS, 0}, {TK, 1, 0}, {TK, 2, 0}, {BF, 0, 0}, {LV, TAGS, 0},    /* 23 computed: [{ a, zz, [5] }] */
        {LA, MADE, 0}, {LV, MADE, 0},
        {ROW, ITEMS, 0}, {IG, ITEM(ITEMS, 2), 0},                                  /* 31 map's tags */
        {ROW, ITEMS, 0}, {IG, ITEM(ITEMS, 1), 0},                                  /* 33 map's raw */
        {TK, 2, 0}, {BF, 0, 0},                                                    /* 35 blob */
    };
    static const W views[] = {{"all", W_OF | 2 << 16 | (ITEMS + 1) << 8, 0, 0, 0}, {"long", W_OF | 2 << 16 | (ITEMS + 1) << 8, 0, 18, 5},
                              {"computed", W_OF | 2 << 16, 0, 23, 8}, {"map", 2, 0, ITEMS, 0},
                              {"tags", W_LIST | 1 << 16, MAP + 1, 31, 2}, {"raw", W_BYTES, MAP + 1, 33, 2},
                              {"blob", W_BYTES, 0, 35, 2}};
    H handler = {1, 0, 18};
    Spec spec = {nums, 1, inputs, 1, NULL, 0, &handler, 1, code, LEN(code), NULL, 0, others, LEN(others), constants,
                 LEN(constants), NULL, 0, records, 2, views, LEN(views)};
    if (load(spec, &module_a) != PURE_OK) { fprintf(stderr, "list views: %s\n", error); abort(); }
    const PureModule *m = &module_a;
    int name = pure_module_column(m, ALL, "name"), raw = pure_module_column(m, ALL, "raw"), tags = pure_module_column(m, ALL, "tags");
    int long_name = pure_module_column(m, LONG, "name"), long_tags = pure_module_column(m, LONG, "tags");
    int made_name = pure_module_column(m, COMPUTED, "name"), made_tags = pure_module_column(m, COMPUTED, "tags");
    CHECK(name >= (int)LEN(views) && raw > name && tags > raw && long_tags > long_name && made_tags > made_name);
    CHECK(m->view_type[name] == PURE_VIEW_TEXT && m->view_type[raw] == PURE_VIEW_BYTES && m->view_type[tags] == PURE_VIEW_LIST &&
          m->view_record[tags] == 1 && m->view_type[COMPUTED] == PURE_VIEW_LIST && m->view_record[COMPUTED] == 2);
    PureApp app;
    CHECK(pure_app_start(&app, m, (PureData){0}) == PURE_OK);
    CHECK(pure_app_rows(&app, ALL) == 0 && pure_app_rows(&app, COMPUTED) == 1);
    CHECK(press(&app, 0) == PURE_OK && app.values[0].num == 2);
    static const int64_t two[] = {7, 9}, three[] = {7, 9, 11}, five[] = {5};
    CHECK(pure_app_rows(&app, ALL) == 2 && !strcmp(cell_text(&app, (uint32_t)name, 1), "bb") &&
          !strcmp(cell_text(&app, (uint32_t)raw, 0), "zz") && tags_are(&app, (uint32_t)tags, 0, two, 2) &&
          tags_are(&app, (uint32_t)tags, 1, three, 3));
    CHECK(pure_app_rows(&app, LONG) == 1 && !strcmp(cell_text(&app, (uint32_t)long_name, 0), "bb") &&
          tags_are(&app, (uint32_t)long_tags, 0, three, 3));
    CHECK(pure_app_rows(&app, COMPUTED) == 1 && !strcmp(view_text(&app, BLOB), "zz") && /* another evaluation between */
          !strcmp(cell_text(&app, (uint32_t)made_name, 0), "a") && tags_are(&app, (uint32_t)made_tags, 0, five, 1));
    CHECK(pure_app_rows(&app, MAP) == 2 && tags_are(&app, MAP_TAGS, 0, two, 2) && tags_are(&app, MAP_TAGS, 1, three, 3) &&
          !strcmp(cell_text(&app, MAP_RAW, 0), "zz") && !strcmp(cell_text(&app, MAP_RAW, 1), ""));
    double number;
    CHECK(!pure_app_cell(&app, (uint32_t)name, 0, &number) && !pure_app_cell(&app, (uint32_t)name, 2, &number));
    CHECK(press(&app, 0) == PURE_OK && pure_app_rows(&app, ALL) == 4 && pure_app_rows(&app, LONG) == 2 &&
          pure_app_rows(&app, COMPUTED) == 1);
    size_t item;
    CHECK(pure_app_row_item(&app, LONG, 1, &item) && item == 3 && pure_app_row_item(&app, COMPUTED, 0, &item) && item == 0 &&
          !pure_app_row_item(&app, COMPUTED, 1, &item));
    pure_app_stop(&app);
    static const W no_block[] = {{"x", W_OF | 2 << 16, 0, 0, 0}}, not_its_record[] = {{"x", W_OF | 1 << 16 | (ITEMS + 1) << 8, 0, 0, 0}},
                   made_column[] = {{"all", W_OF | 2 << 16 | (ITEMS + 1) << 8, 0, 0, 0}, {"c", 0, ALL + 1, 18, 2}},
                   unrecorded[] = {{"map", 2, 0, ITEMS, 0}, {"tags", W_LIST, 1, 31, 2}},
                   another_record[] = {{"x", W_OF | 1 << 16, 0, 23, 8}},
                   pointed[] = {{"computed", W_OF | 2 << 16, 0, 23, 8}};
    static const I pointing_at[] = {KEY("go", "KeyG"), {"computed.point", "0", POINT_INPUT, NULL, 0}};
    const struct { const W *w; size_t nw; const I *in; size_t ni; const char *why; } refused[] = {
        {no_block, 1, inputs, 1, "list view's list"},
        {not_its_record, 1, inputs, 1, "list view's list"},
        {made_column, 2, inputs, 1, "view type"},
        {unrecorded, 2, inputs, 1, "view type"},
        {another_record, 1, inputs, 1, "view must leave one value of its type"},
        {pointed, 1, pointing_at, 2, "pointing's table"},
    };
    for (size_t k = 0; k < LEN(refused); ++k) {
        Spec bad = spec;
        bad.w = refused[k].w, bad.nw = refused[k].nw, bad.in = refused[k].in, bad.ni = refused[k].ni;
        if (load(bad, &module_b) == PURE_OK || !strstr(error, refused[k].why)) {
            fprintf(stderr, "list views: refused %zu: %s\n", k, error);
            abort();
        }
    }
    printf("{\"list_views\":{\"views\":%zu,\"refused\":%zu}}\n", LEN(views), LEN(refused));
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
    CHECK(module_a.input[0].binding == PURE_BIND_TIMER && module_a.input[0].period == 1000);
    PureApp app;
    CHECK(pure_app_start(&app, &module_a, (PureData){0}) == PURE_OK);
    app.now = 1700000123456.0;
    CHECK(tick(&app, 0) == PURE_OK && app.values[0].num == 1700000123456.0 &&
          app.values[1].num == (double)(1700000123 % 60) && app.values[2].num == 1);
    CHECK(tick(&app, 1) == PURE_INVALID && press(&app, 0) == PURE_INVALID);
    CHECK(press(&app, 1) == PURE_OK && app.values[3].num == 2 - 20);
    /* A zero takes the divisor's sign too. */
    static const O zeros[] = {{C, 0, -6}, {C, 0, 3}, {MOD, 0, 0}, {ST, 0, 0}, {C, 0, 6}, {C, 0, -3}, {MOD, 0, 0}, {ST, 1, 0}};
    static const H zero_handler[] = {{3, 0, 8}};
    Spec signs = spec;
    signs.o = zeros; signs.no = 8; signs.h = zero_handler; signs.nh = 1;
    CHECK(load(signs, &module_b) == PURE_OK);
    PureApp zero;
    CHECK(pure_app_start(&zero, &module_b, (PureData){0}) == PURE_OK && press(&zero, 1) == PURE_OK);
    CHECK(zero.values[0].num == 0 && !signbit(zero.values[0].num) && zero.values[1].num == 0 && signbit(zero.values[1].num));
    pure_app_stop(&zero);
    CHECK(press(&app, 2) == PURE_INVALID && app.fault == PURE_FAULT_DIVIDE);
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

/* Floor division of numbers (027's stage 5): as Python's //, the quotient MOD's remainder leaves, exactly (1 // 0.1 is 9:
 * 0.1 is a little more than a tenth), its zero signed as a / b; by zero it fails as divide. */
static void floor_division(void) {
    enum { FDIV = PURE_OP_FLOORDIV };
    static const S nums[] = {{"a", 0}, {"b", 0}, {"c", 0}, {"d", 0}, {"e", 0}, {"f", 0}};
    static const I inputs[] = {KEY("go", "KeyG"), KEY("zero", "KeyZ")};
    static const O code[] = {
        {C, 0, 7}, {C, 0, 2}, {FDIV, 0, 0}, {ST, 0, 0},       {C, 0, -7}, {C, 0, 2}, {FDIV, 0, 0}, {ST, 1, 0},
        {C, 0, 7}, {C, 0, -2}, {FDIV, 0, 0}, {ST, 2, 0},      {C, 0, 1}, {C, 0, 0.1}, {FDIV, 0, 0}, {ST, 3, 0},
        {C, 0, 0}, {C, 0, -3}, {FDIV, 0, 0}, {ST, 4, 0},      {C, 0, 1e300}, {C, 0, 7}, {FDIV, 0, 0}, {ST, 5, 0},
        {C, 0, 1}, {C, 0, 0}, {FDIV, 0, 0}, {ST, 0, 0},
    };
    static const H handlers[] = {{1, 0, 24}, {3, 24, 4}};
    static const V none[] = {{"v", 0, 1}};
    Spec spec = SPEC(nums, inputs, none, handlers, code);
    spec.nv = 0;
    if (load(spec, &module_a) != PURE_OK) { fprintf(stderr, "floor division: %s\n", error); abort(); }
    PureApp app;
    CHECK(pure_app_start(&app, &module_a, (PureData){0}) == PURE_OK && press(&app, 0) == PURE_OK);
    double huge = 1e300, quotient = (huge - fmod(huge, 7)) / 7;
    CHECK(app.values[0].num == 3 && app.values[1].num == -4 && app.values[2].num == -4 && app.values[3].num == 9 &&
          app.values[4].num == 0 && signbit(app.values[4].num) && app.values[5].num == floor(quotient));
    CHECK(press(&app, 1) == PURE_INVALID && app.fault == PURE_FAULT_DIVIDE && app.values[0].num == 3);
    pure_app_stop(&app);
    printf("{\"floor_division\":6}\n");
}

/* A number is written with the fewest digits that read back as it, the
 * digits %g gives (with its exponent's leading zeros dropped): the same
 * text as that search over precisions, for values of every kind. */
static void number_texts(void) {
    uint32_t seed = 0x6e756d73u;
    size_t checked = 0;
    for (unsigned i = 0; i < 300000; ++i) {
        double value;
        uint64_t bits = (uint64_t)next_random(&seed) << 32 | next_random(&seed);
        switch (i % 5) {
        case 0: value = (double)((int64_t)(bits % 2000001) - 1000000) / 100.0; break;         /* money */
        case 1: value = (double)(bits % 20000001) / pow(10, (double)(bits >> 40 & 7)); break; /* decimals */
        case 2: memcpy(&value, &bits, sizeof(value)); if (!isfinite(value)) value = 0.75; break;
        case 3: value = (double)(bits % 100000) / 7.0; break;
        default: value = ldexp((double)(bits >> 11), (int)(bits % 80) - 60); break;
        }
        char got[32], want[32];
        pure_format_number(value, got);
        if (value == 0) strcpy(want, "0");
        else if (value == floor(value) && fabs(value) < 1e15) snprintf(want, sizeof(want), "%.0f", value);
        else {
            for (int precision = 1; precision <= 17; ++precision) {
                snprintf(want, sizeof(want), "%.*g", precision, value);
                if (strtod(want, NULL) == value) break;
            }
            char *exponent = strchr(want, 'e');
            if (exponent)
                while (exponent[2] == '0' && exponent[3]) memmove(exponent + 2, exponent + 3, strlen(exponent + 2));
        }
        CHECK(!strcmp(got, want) && strtod(got, NULL) == value);
        ++checked;
    }
    printf("{\"number_texts\":%zu}\n", checked);
}

/* Loops (docs/design/018): LOOP counts from low up to before high with a
 * local holding the value, BREAK ends the innermost loop, walks nest, and
 * an event's list (let.name) is the event's alone. Work counts as a loop
 * goes round, and what an event adds counts as it adds it: a loop that
 * would not end, or would fill the memory, fails and changes nothing. */
static void loops(void) {
    enum { N, S_, XS, SEEN, PAGES, LOOP = PURE_OP_LOOP, BREAK = PURE_OP_BREAK, LL = PURE_OP_LOCAL_LOAD,
           EQ_ = PURE_OP_EQ, MOD_ = PURE_OP_MOD, HAS_ = PURE_OP_LIST_HAS, EACH_ = PURE_OP_EACH, REQ = PURE_OP_REQUIRE };
#define BODY(slot, length) ((uint32_t)(slot) | (uint32_t)(length) << 16)
    static const S nums[] = {{"n", 0}, {"s", 0}};
    static const T states[] = {{"xs", 3, 0}, {"let.seen", 3 | (uint32_t)(PURE_LIST_ROWS | 1) << 8, 0},
                               {"let.pages", 3 | (uint32_t)PURE_LIST_ROWS << 8, 1}};
    static const RF row_fields[] = {{"value", 0, 0}}, page_fields[] = {{"body", 1, 0}};
    static const R records[] = {{"row", row_fields, 1}, {"page", page_fields, 1}};
    static char page[4001];
    memset(page, 'p', 4000);
    static const char *constants[2];
    constants[0] = "";
    constants[1] = page;
    static const I inputs[] = {KEY("sum", "KeyS"), KEY("brk", "KeyB"), KEY("nest", "KeyN"), KEY("seen", "KeyE"),
                               KEY("huge", "KeyH"), KEY("pages", "KeyP"), KEY("wide", "KeyI"), KEY("frac", "KeyF"),
                               KEY("idle", "KeyZ"), KEY("add", "KeyA")};
    O code[] = {
        /* sum: s = 0; for i in 0 .. n { s = s + i } (9) */
        {C, 0, 0}, {ST, S_, 0}, {C, 0, 0}, {L, N, 0}, {LOOP, BODY(0, 4), 0}, {L, S_, 0}, {LL, 0, 0}, {ADD, 0, 0}, {ST, S_, 0},
        /* brk at 9: s = 0; for i in 0 .. 100 { if i == 5 { break }; s = s + 1 } (14) */
        {C, 0, 0}, {ST, S_, 0}, {C, 0, 0}, {C, 0, 100}, {LOOP, BODY(0, 9), 0},
        {LL, 0, 0}, {C, 0, 5}, {EQ_, 0, 0}, {JZ, 19, 0}, {BREAK, 0, 0}, {L, S_, 0}, {C, 0, 1}, {ADD, 0, 0}, {ST, S_, 0},
        /* nest at 23: s = 0; for a in xs { for b in xs { s = s + 1 } };
         * for i in 0 .. 3 { for j in 0 .. 10 { if j == 2 { break }; s = s + 100 } } (23) */
        {C, 0, 0}, {ST, S_, 0}, {EACH_, WALKING(XS, 5), 0}, {EACH_, WALKING(XS, 4), 0}, {L, S_, 0}, {C, 0, 1}, {ADD, 0, 0},
        {ST, S_, 0},
        {C, 0, 0}, {C, 0, 3}, {LOOP, BODY(0, 12), 0}, {C, 0, 0}, {C, 0, 10}, {LOOP, BODY(1, 9), 0},
        {LL, 1, 0}, {C, 0, 2}, {EQ_, 0, 0}, {JZ, 42, 0}, {BREAK, 0, 0}, {L, S_, 0}, {C, 0, 100}, {ADD, 0, 0}, {ST, S_, 0},
        /* seen at 46: for i in 0 .. 10 { if !seen.has(i mod 3) { seen.add(row { i mod 3 }) } };
         * s = seen.count (15) */
        {C, 0, 0}, {C, 0, 10}, {LOOP, BODY(0, 10), 0}, {LL, 0, 0}, {C, 0, 3}, {MOD_, 0, 0}, {HAS_, SEEN, 0}, {NOT, 0, 0},
        {JZ, 59, 0}, {LL, 0, 0}, {C, 0, 3}, {MOD_, 0, 0}, {LA, SEEN, 0}, {LC, SEEN, 0}, {ST, S_, 0},
        /* huge at 61: for i in 0 .. 1e15 { s = s + 1 } (7) */
        {C, 0, 0}, {C, 0, 1e15}, {LOOP, BODY(0, 4), 0}, {L, S_, 0}, {C, 0, 1}, {ADD, 0, 0}, {ST, S_, 0},
        /* pages at 68: s = 1; for i in 0 .. 1e6 { pages.append(page { big }) } (7) */
        {C, 0, 1}, {ST, S_, 0}, {C, 0, 0}, {C, 0, 1e6}, {LOOP, BODY(0, 2), 0}, {TK, 1, 0}, {LA, PAGES, 0},
        /* wide at 75: s = 0; for i in max - 3 .. max (i64s) { s = s + 1 }; require the last i was max - 1 (12) */
        {C, 0, 0}, {ST, S_, 0}, {C, 2, 0}, {C, 2, 0}, {LOOP, BODY(2, 4) | PURE_LOOP_INTEGER, 0}, {L, S_, 0}, {C, 0, 1},
        {ADD, 0, 0}, {ST, S_, 0}, {LL, 2 | 3 << 8, 0}, {C, 2, 0}, {PURE_OP_I64_COMPARE, PURE_EQUAL, 0},
        /* frac at 88: for i in 0 .. 2.5 { } (3) */
        {REQ, 0, 0}, {C, 0, 0}, {C, 0, 2.5}, {LOOP, BODY(0, 0), 0},
        /* idle at 91: for i in 0 .. 1e15 { } (3); add at 94: xs.append(row { n }); n = n + 1 (6) */
        {C, 0, 0}, {C, 0, 1e15}, {LOOP, BODY(0, 0), 0},
        {L, N, 0}, {LA, XS, 0}, {L, N, 0}, {C, 0, 1}, {ADD, 0, 0}, {ST, N, 0},
    };
    code[77] = IC(INT64_MAX - 3);
    code[78] = IC(INT64_MAX);
    code[85] = IC(INT64_MAX - 1);
    static const H handlers[] = {{1, 0, 9}, {3, 9, 14}, {5, 23, 23}, {7, 46, 15}, {9, 61, 7}, {11, 68, 7},
                                 {13, 75, 13}, {15, 88, 3}, {17, 91, 3}, {19, 94, 6}};
    Spec spec = {nums, 2, inputs, LEN(inputs), NULL, 0, handlers, LEN(handlers), code, LEN(code), NULL, 0, states, 3,
                 constants, 2, NULL, 0, records, 2, NULL, 0};
    CHECK(load(spec, &module_a) == PURE_OK);
    CHECK(module_a.state_key[SEEN] == 1 && module_a.state_flags[SEEN] == PURE_LIST_ROWS >> 8);
    PureApp app;
    CHECK(pure_app_start(&app, &module_a, (PureData){0}) == PURE_OK);
    for (int k = 0; k < 10; ++k) CHECK(press(&app, 9) == PURE_OK);
    CHECK(app.values[N].num == 10 && pure_list_count(app.lists[XS]) == 10);
    CHECK(press(&app, 0) == PURE_OK && app.values[S_].num == 45);
    CHECK(app.work == 9 + 10 * (4 + 1)); /* the LOOP counts its block and itself each time round */
    CHECK(press(&app, 1) == PURE_OK && app.values[S_].num == 5);
    CHECK(press(&app, 2) == PURE_OK && app.values[S_].num == 100 + 3 * 2 * 100);
    CHECK(press(&app, 3) == PURE_OK && app.values[S_].num == 3 && !pure_list_count(app.lists[SEEN]));
    CHECK(press(&app, 3) == PURE_OK && app.values[S_].num == 3); /* each event's own */
    CHECK(press(&app, 6) == PURE_OK && app.values[S_].num == 3);
    /* A loop that would not end runs out of work, by default too, and changes nothing. */
    CHECK(press(&app, 4) == PURE_LIMIT && app.fault == PURE_FAULT_WORK && app.values[S_].num == 3);
    CHECK(app.work <= PURE_WORK_DEFAULT && app.work > PURE_WORK_DEFAULT - 5);
    CHECK(press(&app, 8) == PURE_LIMIT && app.fault == PURE_FAULT_WORK);
    CHECK(press(&app, 7) == PURE_LIMIT && app.fault == PURE_FAULT_LIMIT); /* not a whole number */
    /* A list that would fill the memory fails as it passes the default room, or the budget. */
    CHECK(press(&app, 5) == PURE_BUDGET && app.fault == PURE_FAULT_MEMORY &&
          app.values[S_].num == 3 && !pure_list_count(app.lists[PAGES]));
    CHECK(app.grown > (int64_t)PURE_GROWTH_DEFAULT && app.grown < (int64_t)PURE_GROWTH_DEFAULT + 8192);
    pure_app_memory(&app, 1 << 20);
    CHECK(press(&app, 5) == PURE_BUDGET && app.fault == PURE_FAULT_MEMORY &&
          app.grown < (1 << 20) + 8192 && !pure_list_count(app.lists[PAGES]));
    pure_app_memory(&app, 0);
    pure_app_work(&app, 10); /* a budget of its own: the sum's 9, and not one time round */
    CHECK(press(&app, 0) == PURE_LIMIT && app.values[S_].num == 3);
    pure_app_stop(&app);

    /* What the verifier refuses. */
    static const O outside[] = {{BREAK, 0, 0}};
    static const O valued[] = {{C, 0, 0}, {C, 0, 1}, {LOOP, BODY(0, 3), 0}, {C, 0, 1}, {BREAK, 0, 0}, {ST, S_, 0}};
    static const O into[] = {{C, 1, 1}, {JZ, 5, 0}, {C, 0, 0}, {C, 0, 1}, {LOOP, BODY(0, 2), 0}, {C, 0, 1}, {ST, S_, 0}};
    static const O typed[] = {{C, 1, 1}, {C, 0, 1}, {LOOP, BODY(0, 0), 0}};
    static const O flagged[] = {{C, 0, 0}, {C, 0, 1}, {LOOP, BODY(0, 0) | 1u << 9, 0}};
    static const O two_ways[] = {{C, 0, 0}, {C, 0, 1}, {LOOP, BODY(0, 0), 0}, {XF, XS, 0}, {PURE_OP_LOCAL_STORE, 0 | 1 << 8 | XS << 16, 0}};
    static const O into_outer[] = {{EACH_, WALKING(XS, 5), 0}, {C, 0, 0}, {C, 0, 1}, {LOOP, BODY(0, 2), 0}, {C, 0, 1}, {LA, XS, 0}};
    O deep[8 * 3 + 2];
    for (unsigned d = 0; d < 9; ++d) {
        if (d == 8) { deep[24] = (O){C, 0, 1}; deep[25] = (O){ST, S_, 0}; break; }
        deep[3 * d] = (O){C, 0, 0};
        deep[3 * d + 1] = (O){C, 0, 1};
        deep[3 * d + 2] = (O){LOOP, BODY(0, 26 - 3 * d - 3), 0};
    }
    enum { DEEPER = 40 }; /* nested past what a run keeps on its stack (027's 4f-4): its app's room */
    O deeper[DEEPER * 3 + 4];
    for (unsigned d = 0; d < DEEPER; ++d) {
        deeper[3 * d] = (O){C, 0, 0};
        deeper[3 * d + 1] = (O){C, 0, 2}; /* each twice: the innermost 2^40 times past any budget, so its first fault */
        deeper[3 * d + 2] = (O){LOOP, BODY(0, DEEPER * 3 + 4 - 3 * d - 3), 0};
    }
    deeper[3 * DEEPER] = (O){PURE_OP_LOAD, S_, 0};
    deeper[3 * DEEPER + 1] = (O){C, 0, 1};
    deeper[3 * DEEPER + 2] = (O){PURE_OP_ADD, 0, 0};
    deeper[3 * DEEPER + 3] = (O){ST, S_, 0};
    const struct { const O *code; size_t count; const char *why; } refused[] = {
        {outside, LEN(outside), "break outside a loop"},
        {valued, LEN(valued), "break with values on the stack"},
        {into, LEN(into), "branch into a loop"},
        {typed, LEN(typed), "operand type"},
        {flagged, LEN(flagged), "no such state"},
        {two_ways, LEN(two_ways), "local used as two things"},
        {into_outer, LEN(into_outer), "insert into the list walked"},
    };
    for (size_t i = 0; i < LEN(refused); ++i) {
        H one[] = {{1, 0, (uint32_t)refused[i].count}};
        Spec bad = spec;
        bad.o = refused[i].code;
        bad.no = refused[i].count;
        bad.h = one;
        bad.nh = 1;
        if (load(bad, &module_b) == PURE_OK || !strstr(error, refused[i].why)) {
            fprintf(stderr, "loop case %zu: expected '%s', got '%s'\n", i, refused[i].why, error);
            abort();
        }
    }
    H eight[] = {{1, 0, LEN(deep)}};
    Spec nested = spec;
    nested.o = deep;
    nested.no = LEN(deep);
    nested.h = eight;
    nested.nh = 1;
    CHECK(load(nested, &module_b) == PURE_OK && module_b.nesting == 8 && module_b.runner != 2); /* on the stack */
    H forty[] = {{1, 0, LEN(deeper)}};
    nested.o = deeper, nested.no = LEN(deeper), nested.h = forty;
    CHECK(load(nested, &module_b) == PURE_OK && module_b.nesting == DEEPER && module_b.runner == 2 && !module_b.jit);
    PureApp deep_app;
    CHECK(pure_app_start(&deep_app, &module_b, (PureData){0}) == PURE_OK);
    pure_app_work(&deep_app, 100000); /* (2^40 rounds would never end) */
    CHECK(press(&deep_app, 0) == PURE_LIMIT && deep_app.fault == PURE_FAULT_WORK && deep_app.values[S_].num == 0);
    pure_app_work(&deep_app, 0);
    pure_app_stop(&deep_app);
    for (unsigned d = 0; d < DEEPER; ++d) deeper[3 * d + 1] = (O){C, 0, d % 2 ? 1 : 2}; /* 2^20 rounds */
    CHECK(load(nested, &module_b) == PURE_OK && pure_app_start(&deep_app, &module_b, (PureData){0}) == PURE_OK);
    pure_app_work(&deep_app, UINT64_C(1) << 40);
    CHECK(press(&deep_app, 0) == PURE_OK && deep_app.values[S_].num == 1 << 20 && press(&deep_app, 0) == PURE_OK &&
          deep_app.values[S_].num == 2 << 20);
    pure_app_stop(&deep_app);
    pure_module_release(&module_b);
    pure_module_release(&module_a);
#undef BODY
    puts("{\"loops\":\"count-break-nested-walks-event-lists-work-memory-refusals\"}");
}

/* Views that compute (docs/design/018): a body's locals begin at 0 and
 * the event's lists it fills begin empty at each evaluation, on the app and
 * on readers of one snapshot at once (each its own lists); its loops count
 * as a view's work, its lists as an event's growth. */
typedef struct { PureSnapshot *snapshot; double total, distinct; unsigned rounds; } BodyReads;

static void *read_bodies(void *context) {
    BodyReads *r = context;
    PureApp reader = {0};
    for (unsigned k = 0; k < r->rounds; ++k) {
        pure_app_read(&reader, r->snapshot);
        double total, distinct;
        CHECK(pure_app_view(&reader, 1, &distinct) && distinct == r->distinct);
        CHECK(pure_app_view(&reader, 0, &total) && total == r->total);
    }
    CHECK(reader.own[3] && !pure_list_count(reader.own[3]));
    pure_app_stop(&reader);
    return NULL;
}

static void view_bodies(void) {
    enum { N, XS = 2, SEEN, LOOP = PURE_OP_LOOP, LL = PURE_OP_LOCAL_LOAD, LS = PURE_OP_LOCAL_STORE, HAS_ = PURE_OP_LIST_HAS,
           EACH_ = PURE_OP_EACH, MOD_ = PURE_OP_MOD };
    static const S nums[] = {{"n", 0}, {"m", 0}};
    static const T states[] = {{"xs", 3, 0}, {"let.seen", 3 | (uint32_t)(PURE_LIST_ROWS | 1) << 8, 0}};
    static const RF fields[] = {{"value", 0, 0}};
    static const R records[] = {{"row", fields, 1}};
    static const I inputs[] = {KEY("add", "KeyA")};
    static const O code[] = {
        /* add: xs.append(row { n mod 7 }); n = n + 1 (8) */
        {L, N, 0}, {C, 0, 7}, {MOD_, 0, 0}, {LA, XS, 0}, {L, N, 0}, {C, 0, 1}, {ADD, 0, 0}, {ST, N, 0},
        /* total at 8: s = 0; for i in 0 .. xs.count { s = s + (xs.first + i).value }; s (13) */
        {C, 0, 0}, {LS, 1, 0}, {C, 0, 0}, {LC, XS, 0}, {LOOP, 0 | 7u << 16, 0},
        {LL, 1, 0}, {XF, XS, 0}, {LL, 0, 0}, {XM, XS, 0}, {IG, XS, 0}, {ADD, 0, 0}, {LS, 1, 0}, {LL, 1, 0},
        /* distinct at 21: for x in xs { if !seen.has(x.value) { seen.add(row { x.value }) } }; seen.count (10) */
        {EACH_, WALKING(XS, 8), 0}, {ROW, XS, 0}, {IG, XS, 0}, {HAS_, SEEN, 0}, {NOT, 0, 0}, {JZ, 30, 0}, {ROW, XS, 0},
        {IG, XS, 0}, {LA, SEEN, 0}, {LC, SEEN, 0},
        /* spin at 31: for i in 0 .. 1e15 { }; 1 (4) */
        {C, 0, 0}, {C, 0, 1e15}, {LOOP, 0, 0}, {C, 0, 1},
        /* fill at 35: for i in 0 .. 1e6 { seen.add(row { i }) }; seen.count (6) */
        {C, 0, 0}, {C, 0, 1e6}, {LOOP, 0 | 2u << 16, 0}, {LL, 0, 0}, {LA, SEEN, 0}, {LC, SEEN, 0},
    };
    static const H handlers[] = {{1, 0, 8}};
    static const V views[] = {{"total", 8, 13}, {"distinct", 21, 10}, {"spin", 31, 4}, {"fill", 35, 6}};
    Spec spec = {nums, 2, inputs, 1, views, 4, handlers, 1, code, LEN(code), NULL, 0, states, 2, NULL, 0, NULL, 0,
                 records, 1, NULL, 0};
    CHECK(load(spec, &module_a) == PURE_OK);
    CHECK(module_a.view_body[0] && module_a.view_body[1] && module_a.state_words == 1 && !module_a.view_lists[0] &&
          module_a.view_lists[1] == UINT64_C(1) << SEEN);
    char *path = new_store();
    PureApp app;
    CHECK(pure_app_start(&app, &module_a, pure_tree_data(path)) == PURE_OK);
    double total = 0, value;
    for (int k = 0; k < 20; ++k) {
        CHECK(press(&app, 0) == PURE_OK);
        total += k % 7;
    }
    for (int round = 0; round < 2; ++round) /* each evaluation begins anew */
        CHECK(number_view(&app, 0) == total && number_view(&app, 1) == 7 && !pure_list_count(app.lists[SEEN]));
    CHECK(!pure_app_view(&app, 2, &value) && pure_app_view_fault(&app) == PURE_FAULT_WORK);
    CHECK(number_view(&app, 3) == 1e6 && !pure_list_count(app.lists[SEEN]));
    pure_app_memory(&app, 4096);
    CHECK(!pure_app_view(&app, 3, &value) && pure_app_view_fault(&app) == PURE_FAULT_MEMORY && !pure_list_count(app.lists[SEEN]));
    pure_app_memory(&app, 0);
    CHECK(press(&app, 0) == PURE_OK); /* the app goes on */
    total += 20 % 7;
    /* Readers of one snapshot compute at once, each in its own lists. */
    BodyReads reads = {pure_app_snapshot(&app), total, 7, 2000};
    CHECK(reads.snapshot);
    pthread_t threads[3];
    for (int t = 0; t < 3; ++t) CHECK(!pthread_create(&threads[t], NULL, read_bodies, &reads));
    for (int t = 0; t < 3; ++t) pthread_join(threads[t], NULL);
    pure_snapshot_release(reads.snapshot);
    pure_app_stop(&app);
    remove_store(path);

    /* A body changes no state. */
    static const O stores[] = {{C, 0, 0}, {C, 0, 2}, {LOOP, 0 | 2u << 16, 0}, {C, 0, 1}, {ST, N, 0}, {C, 0, 1}};
    static const O appends[] = {{C, 0, 1}, {LA, XS, 0}, {C, 0, 1}};
    static const O breaks[] = {{PURE_OP_BREAK, 0, 0}, {C, 0, 1}};
    const struct { const O *code; size_t count; const char *why; } refused[] = {
        {stores, LEN(stores), "view with an effect"},
        {appends, LEN(appends), "view changes a state"},
        {breaks, LEN(breaks), "break outside a loop"},
    };
    for (size_t i = 0; i < LEN(refused); ++i) {
        V one[] = {{"v", 0, (uint32_t)refused[i].count}};
        Spec bad = spec;
        bad.o = refused[i].code;
        bad.no = refused[i].count;
        bad.h = NULL;
        bad.nh = 0;
        bad.v = one;
        bad.nv = 1;
        if (load(bad, &module_b) == PURE_OK || !strstr(error, refused[i].why)) {
            fprintf(stderr, "body case %zu: expected '%s', got '%s'\n", i, refused[i].why, error);
            abort();
        }
    }
    pure_module_release(&module_a);
    puts("{\"view_bodies\":\"locals-loops-own-lists-readers-work-memory-refusals\"}");
}

int main(void) {
    number_texts();
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
    retired_come_back();
    list_trees();
    key_sets();
    keyed_journal();
    text_keys_equivalence();
    arrays_equivalence();
    packed_lists();
    folds();
    filters();
    lane_folds();
    walks();
    walk_mutations();
    view_arguments();
    locals();
    loops();
    view_bodies();
    ranges();
    keyed();
    keyed_mutations();
    snapshots();
    links();
    send_recast();
    output_meaning();
    bools();
    integers();
    bits_and_bytes();
    formats_and_held();
    lists_in_records();
    functions_and_characters();
    nested_crashes();
    nested_replace();
    nested_rows();
    bytes_fields();
    anchors();
    list_views();
    floor_division();
    memory_budget();
    work_budget();
    fold_mutations();
    pointing();
    list_mutations();
    tables();
    table_mutations();
    stored_states();
    deferred();
    printf("{\"sizeof_module\":%zu,\"sizeof_app\":%zu}\n", sizeof(PureModule), sizeof(PureApp));
    puts("{\"module_tests\":\"verified-load-atomic-events-faults-signed-zero-if-else-bools-replace-by-name-rejections-mutations-messages-typed-verification-text-cursors-utf8-views-rollback-replace-lists-indices-undo-migration-joins-tables-columns-pick-slide-stored-equivalence-crashes-frames-messages-migration-deferred-bursts-pointing-marks-folds-filters-keyed-lists-key-sets-arrays-text-order-message-fields-timers-now-snapshots-links\"}");
    return 0;
}
