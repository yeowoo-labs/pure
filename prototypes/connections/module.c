#include "module.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { STATE_BYTES = 16, COLLECTION_BYTES = 12, INPUT_BYTES = 16, FIELD_BYTES = 8,
       TABLE_BYTES = 12, OP_BYTES = 16 };
enum { T_NUM = 1, T_BYTES = 2, T_FIELD = 3 /* type of the named field */ };
enum { FROM_PAYLOAD, FROM_DOCUMENT, FROM_COPY };
/* Where an opcode may appear. */
enum { IN_VIEW = 1, IN_HANDLER = 2, IN_MESSAGE = 4, IN_KEYED = 8 };
static const unsigned char magic[8] = {'P','U','R','E','M','O','D','2'};

/* Operand types popped (bottom to top), the type pushed, and where allowed. */
static const struct { unsigned char pops, in[3], out, where; } signature[PURE_OP_COUNT] = {
    [PURE_OP_CONST] = {0, {0}, T_NUM, IN_VIEW | IN_HANDLER},
    [PURE_OP_LOAD] = {0, {0}, T_NUM, IN_VIEW | IN_HANDLER},
    [PURE_OP_DT] = {0, {0}, T_NUM, IN_HANDLER},
    [PURE_OP_ADD] = {2, {T_NUM, T_NUM}, T_NUM, IN_VIEW | IN_HANDLER},
    [PURE_OP_SUB] = {2, {T_NUM, T_NUM}, T_NUM, IN_VIEW | IN_HANDLER},
    [PURE_OP_MUL] = {2, {T_NUM, T_NUM}, T_NUM, IN_VIEW | IN_HANDLER},
    [PURE_OP_DIV] = {2, {T_NUM, T_NUM}, T_NUM, IN_VIEW | IN_HANDLER},
    [PURE_OP_NEG] = {1, {T_NUM}, T_NUM, IN_VIEW | IN_HANDLER},
    [PURE_OP_LT] = {2, {T_NUM, T_NUM}, T_NUM, IN_VIEW | IN_HANDLER},
    [PURE_OP_LE] = {2, {T_NUM, T_NUM}, T_NUM, IN_VIEW | IN_HANDLER},
    [PURE_OP_GT] = {2, {T_NUM, T_NUM}, T_NUM, IN_VIEW | IN_HANDLER},
    [PURE_OP_GE] = {2, {T_NUM, T_NUM}, T_NUM, IN_VIEW | IN_HANDLER},
    [PURE_OP_EQ] = {2, {T_NUM, T_NUM}, T_NUM, IN_VIEW | IN_HANDLER},
    [PURE_OP_NE] = {2, {T_NUM, T_NUM}, T_NUM, IN_VIEW | IN_HANDLER},
    [PURE_OP_AND] = {2, {T_NUM, T_NUM}, T_NUM, IN_VIEW | IN_HANDLER},
    [PURE_OP_OR] = {2, {T_NUM, T_NUM}, T_NUM, IN_VIEW | IN_HANDLER},
    [PURE_OP_NOT] = {1, {T_NUM}, T_NUM, IN_VIEW | IN_HANDLER},
    [PURE_OP_MIN] = {2, {T_NUM, T_NUM}, T_NUM, IN_VIEW | IN_HANDLER},
    [PURE_OP_MAX] = {2, {T_NUM, T_NUM}, T_NUM, IN_VIEW | IN_HANDLER},
    [PURE_OP_CLAMP] = {3, {T_NUM, T_NUM, T_NUM}, T_NUM, IN_VIEW | IN_HANDLER},
    [PURE_OP_STORE] = {1, {T_NUM}, 0, IN_HANDLER},
    [PURE_OP_JZ] = {1, {T_NUM}, 0, IN_HANDLER},
    [PURE_OP_JMP] = {0, {0}, 0, IN_HANDLER},
    [PURE_OP_FIELD] = {0, {0}, T_FIELD, IN_MESSAGE},
    [PURE_OP_DOC] = {0, {0}, T_BYTES, IN_KEYED},
    [PURE_OP_DOC_SIZE] = {0, {0}, T_NUM, IN_KEYED},
    [PURE_OP_LEN] = {1, {T_BYTES}, T_NUM, IN_HANDLER},
    [PURE_OP_REPLACE] = {3, {T_NUM, T_NUM, T_BYTES}, 0, IN_KEYED},
    [PURE_OP_REPLY] = {1, {T_BYTES}, 0, IN_MESSAGE},
    [PURE_OP_REQUIRE] = {1, {T_NUM}, 0, IN_HANDLER},
};

static uint32_t read16(const unsigned char *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }

static uint32_t read32(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static double read_double(const unsigned char *p) {
    uint64_t bits = read32(p) | (uint64_t)read32(p + 4) << 32;
    double value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static bool reject(char *error, size_t size, const char *message, unsigned index) {
    if (size) snprintf(error, size, "%s (%u)", message, index);
    return false;
}

static bool key_name(const char *key) {
    static const char *const named[] = {"Space", "Enter", "ArrowLeft", "ArrowRight", "ArrowUp", "ArrowDown"};
    for (size_t i = 0; i < sizeof(named) / sizeof(*named); ++i)
        if (!strcmp(key, named[i])) return true;
    if (!strncmp(key, "Key", 3)) return key[3] >= 'A' && key[3] <= 'Z' && !key[4];
    if (!strncmp(key, "Digit", 5)) return key[5] >= '0' && key[5] <= '9' && !key[6];
    return false;
}

/* A document key: 1..64 bytes of [A-Za-z0-9_-], as every Pure store accepts. */
static bool document_key(const char *key) {
    size_t length = key ? strlen(key) : 0;
    if (!length || length > 64) return false;
    for (size_t i = 0; i < length; ++i) {
        char c = key[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '-' || c == '_')) return false;
    }
    return true;
}

/* Strings are [u8 length][bytes]; a name is an ASCII identifier of 1..31 bytes. */
static bool name_at(const unsigned char *strings, uint32_t size, uint32_t offset,
                    bool identifier, char out[PURE_MODULE_NAME]) {
    if (offset >= size) return false;
    uint32_t length = strings[offset];
    if (!length || length >= PURE_MODULE_NAME || length > size - offset - 1) return false;
    for (uint32_t i = 0; i < length; ++i) {
        unsigned char c = strings[offset + 1 + i];
        bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
        if (!letter && !(i && c >= '0' && c <= '9') && (identifier || c < 32 || c > 126)) return false;
    }
    memcpy(out, strings + offset + 1, length);
    out[length] = 0;
    return true;
}

/* The input whose handler this is, or -1 for frame. */
static int event_input(uint32_t event) { return event ? (int)((event - 1) / 2) : -1; }

/* Types and heights are checked per instruction. Branches go forward within
 * the block and every join happens with an empty operand stack, so control
 * flow never merges typed values. Handlers end empty, views with one number. */
static bool verify_block(const PureModule *m, PureBlock block, bool view, uint32_t event,
                         char *error, size_t size, unsigned index) {
    if (block.start > m->code_count || block.length > m->code_count - block.start)
        return reject(error, size, "code range outside module", index);
    if (view && !block.length) return reject(error, size, "empty view", index);
    int input = view ? -1 : event_input(event);
    const PureInputShape *shape = input >= 0 ? &m->input[input] : NULL;
    unsigned where = view ? IN_VIEW : IN_HANDLER;
    if (shape && shape->kind == PURE_KIND_MESSAGE) where |= IN_MESSAGE | (shape->keyed ? IN_KEYED : 0);
    unsigned char types[PURE_MODULE_STACK], target[PURE_MODULE_CODE + 1] = {0};
    int height = 0;
    bool reachable = true;
    for (uint32_t i = 0; i <= block.length; ++i) {
        uint32_t pc = block.start + i;
        if (!reachable && target[i]) { reachable = true; height = 0; }
        else if (target[i] && height) return reject(error, size, "join with a non-empty stack", pc);
        if (i == block.length) break;
        if (!reachable) return reject(error, size, "unreachable instruction", pc);
        const PureOp *op = &m->code[pc];
        if (op->op >= PURE_OP_COUNT) return reject(error, size, "unknown opcode", pc);
        unsigned allowed = signature[op->op].where;
        if (!(allowed & where)) {
            if (view) return reject(error, size, "view with an effect or branch", pc);
            if (allowed & IN_KEYED) return reject(error, size, "document operation outside a keyed message handler", pc);
            return reject(error, size, "message operation outside a message handler", pc);
        }
        bool uses_arg = op->op == PURE_OP_LOAD || op->op == PURE_OP_STORE || op->op == PURE_OP_JZ ||
                        op->op == PURE_OP_JMP || op->op == PURE_OP_FIELD || op->op == PURE_OP_DOC ||
                        op->op == PURE_OP_DOC_SIZE || op->op == PURE_OP_REPLACE;
        if ((!uses_arg && op->arg) || (op->op != PURE_OP_CONST && op->value != 0) ||
            (op->op == PURE_OP_CONST && !isfinite(op->value)))
            return reject(error, size, "non-canonical operand", pc);
        if ((op->op == PURE_OP_LOAD || op->op == PURE_OP_STORE) && op->arg >= m->states)
            return reject(error, size, "unknown state", pc);
        if (op->op == PURE_OP_FIELD && op->arg >= shape->field_count)
            return reject(error, size, "unknown field", pc);
        if ((op->op == PURE_OP_DOC || op->op == PURE_OP_DOC_SIZE || op->op == PURE_OP_REPLACE) &&
            op->arg >= m->collections) return reject(error, size, "unknown collection", pc);
        unsigned pops = signature[op->op].pops;
        if (height < (int)pops) return reject(error, size, "stack underflow", pc);
        for (unsigned k = 0; k < pops; ++k)
            if (types[height - (int)pops + (int)k] != signature[op->op].in[k])
                return reject(error, size, "operand type", pc);
        height -= (int)pops;
        unsigned char out = signature[op->op].out;
        if (out == T_FIELD) out = m->field[shape->field_start + op->arg].type == PURE_FIELD_U32 ? T_NUM : T_BYTES;
        if (out) {
            if (height == PURE_MODULE_STACK) return reject(error, size, "stack overflow", pc);
            types[height++] = out;
        }
        if (op->op == PURE_OP_JZ || op->op == PURE_OP_JMP) {
            if (op->arg <= pc || op->arg > block.start + block.length)
                return reject(error, size, "branch not forward within its block", pc);
            if (height) return reject(error, size, "branch with a non-empty stack", pc);
            target[op->arg - block.start] = 1;
            if (op->op == PURE_OP_JMP) reachable = false;
        }
    }
    if (!reachable) return reject(error, size, "block end is unreachable", index);
    if (view ? height != 1 || types[0] != T_NUM : height != 0)
        return reject(error, size, view ? "view must leave one value" : "handler must leave no value", index);
    return true;
}

PureResult pure_module_load(const void *bytes, size_t size, PureModule *out,
                            char *error, size_t error_size) {
    const unsigned char *p = bytes;
    if (error_size) error[0] = 0;
    memset(out, 0, sizeof(*out));
    if (!p || size < PURE_MODULE_HEADER || memcmp(p, magic, sizeof(magic)))
        return reject(error, error_size, "not a Pure module v2", 0), PURE_INVALID;
    out->states = (uint16_t)read16(p + 8);
    out->inputs = (uint16_t)read16(p + 10);
    out->views = (uint16_t)read16(p + 12);
    out->handlers = (uint16_t)read16(p + 14);
    out->code_count = read32(p + 16);
    uint32_t strings = read32(p + 20);
    out->fields = (uint16_t)read16(p + 24);
    out->collections = (uint16_t)read16(p + 26);
    if (read32(p + 28)) return reject(error, error_size, "reserved header bytes", 28), PURE_INVALID;
    if (out->states > PURE_MODULE_STATES || out->inputs > PURE_MODULE_INPUTS ||
        out->views > PURE_MODULE_VIEWS || out->handlers > PURE_MODULE_HANDLERS ||
        out->code_count > PURE_MODULE_CODE || strings > PURE_MODULE_STRINGS ||
        out->fields > PURE_MODULE_FIELDS || out->collections > PURE_MODULE_COLLECTIONS)
        return reject(error, error_size, "module exceeds a limit", 8), PURE_LIMIT;
    size_t expected = PURE_MODULE_HEADER + (size_t)out->states * STATE_BYTES +
        (size_t)out->collections * COLLECTION_BYTES + (size_t)out->inputs * INPUT_BYTES +
        (size_t)out->fields * FIELD_BYTES + ((size_t)out->views + out->handlers) * TABLE_BYTES +
        (size_t)out->code_count * OP_BYTES + strings;
    if (size != expected) return reject(error, error_size, "module size", (unsigned)expected), PURE_INVALID;
    const unsigned char *at = p + PURE_MODULE_HEADER;
    const unsigned char *text = p + size - strings;
    for (uint32_t i = 0; i < out->states; ++i, at += STATE_BYTES) {
        out->initial[i] = read_double(at + 8);
        if (!name_at(text, strings, read32(at), true, out->state_names[i]))
            return reject(error, error_size, "state name", i), PURE_INVALID;
        if (read32(at + 4)) return reject(error, error_size, "state type", i), PURE_INVALID;
        if (!isfinite(out->initial[i])) return reject(error, error_size, "non-finite initial value", i), PURE_INVALID;
    }
    for (uint32_t i = 0; i < out->collections; ++i, at += COLLECTION_BYTES) {
        if (!name_at(text, strings, read32(at), true, out->collection_names[i]))
            return reject(error, error_size, "collection name", i), PURE_INVALID;
        if (read32(at + 4) != 1 || read32(at + 8) != 1)
            return reject(error, error_size, "collection must hold bytes by key", i), PURE_INVALID;
    }
    uint32_t next_field = 0;
    const unsigned char *fields = at + (size_t)out->inputs * INPUT_BYTES;
    for (uint32_t i = 0; i < out->inputs; ++i, at += INPUT_BYTES) {
        PureInputShape *shape = &out->input[i];
        shape->kind = at[4];
        shape->keyed = at[5];
        shape->field_start = (uint16_t)read16(at + 12);
        shape->field_count = (uint16_t)read16(at + 14);
        if (!name_at(text, strings, read32(at), true, out->input_names[i]))
            return reject(error, error_size, "input name", i), PURE_INVALID;
        if (read16(at + 6) || at[5] > 1) return reject(error, error_size, "input flags", i), PURE_INVALID;
        if (shape->kind == PURE_KIND_KEY) {
            if (shape->keyed || shape->field_start || shape->field_count ||
                !name_at(text, strings, read32(at + 8), false, out->input_keys[i]) ||
                !key_name(out->input_keys[i])) return reject(error, error_size, "input key", i), PURE_INVALID;
            for (uint32_t k = 0; k < i; ++k)
                if (out->input[k].kind == PURE_KIND_KEY && !strcmp(out->input_keys[k], out->input_keys[i]))
                    return reject(error, error_size, "duplicate key", i), PURE_INVALID;
            continue;
        }
        if (shape->kind != PURE_KIND_MESSAGE || read32(at + 8) || shape->field_start != next_field ||
            shape->field_count > out->fields - next_field)
            return reject(error, error_size, "message shape", i), PURE_INVALID;
        /* Fixed u32le fields in order, then at most one bytes tail. */
        for (uint32_t f = 0; f < shape->field_count; ++f) {
            const unsigned char *entry = fields + (size_t)(next_field + f) * FIELD_BYTES;
            PureField *field = &out->field[next_field + f];
            field->type = (uint8_t)read32(entry + 4);
            if (!name_at(text, strings, read32(entry), true, field->name) ||
                (field->type != PURE_FIELD_U32 && field->type != PURE_FIELD_BYTES) || read32(entry + 4) > 2 ||
                (field->type == PURE_FIELD_BYTES && f + 1 != shape->field_count))
                return reject(error, error_size, "message field", next_field + f), PURE_INVALID;
            for (uint32_t k = 0; k < f; ++k)
                if (!strcmp(out->field[next_field + k].name, field->name))
                    return reject(error, error_size, "duplicate field", next_field + f), PURE_INVALID;
            field->offset = shape->prefix;
            if (field->type == PURE_FIELD_U32) shape->prefix += 4;
        }
        next_field += shape->field_count;
    }
    if (next_field != out->fields) return reject(error, error_size, "unused message fields", next_field), PURE_INVALID;
    at = fields + (size_t)out->fields * FIELD_BYTES;
    for (uint32_t i = 0; i < out->views; ++i, at += TABLE_BYTES) {
        if (!name_at(text, strings, read32(at), true, out->view_names[i]))
            return reject(error, error_size, "view name", i), PURE_INVALID;
        out->view_code[i] = (PureBlock){read32(at + 4), read32(at + 8)};
    }
    for (uint32_t i = 0; i < out->handlers; ++i, at += TABLE_BYTES) {
        uint32_t event = read32(at);
        out->handler[i] = (PureHandler){event, {read32(at + 4), read32(at + 8)}};
        int input = event_input(event);
        if (event > 2u * out->inputs || (input >= 0 && out->input[input].kind == PURE_KIND_MESSAGE && !(event & 1)))
            return reject(error, error_size, "handler event", i), PURE_INVALID;
    }
    for (uint32_t i = 0; i < out->code_count; ++i, at += OP_BYTES) {
        if (at[1] || at[2] || at[3]) return reject(error, error_size, "instruction padding", i), PURE_INVALID;
        out->code[i] = (PureOp){.op = at[0], .arg = read32(at + 4), .value = read_double(at + 8)};
    }
    /* States, collections, inputs and views share one namespace. */
    const char *names[PURE_MODULE_STATES + PURE_MODULE_COLLECTIONS + PURE_MODULE_INPUTS + PURE_MODULE_VIEWS];
    size_t count = 0;
    for (uint32_t i = 0; i < out->states; ++i) names[count++] = out->state_names[i];
    for (uint32_t i = 0; i < out->collections; ++i) names[count++] = out->collection_names[i];
    for (uint32_t i = 0; i < out->inputs; ++i) names[count++] = out->input_names[i];
    for (uint32_t i = 0; i < out->views; ++i) names[count++] = out->view_names[i];
    for (size_t i = 0; i < count; ++i)
        for (size_t k = 0; k < i; ++k)
            if (!strcmp(names[i], names[k])) return reject(error, error_size, "duplicate name", (unsigned)i), PURE_INVALID;
    for (uint32_t i = 0; i < out->views; ++i)
        if (!verify_block(out, out->view_code[i], true, 0, error, error_size, i)) return PURE_INVALID;
    for (uint32_t i = 0; i < out->handlers; ++i)
        if (!verify_block(out, out->handler[i].code, false, out->handler[i].event, error, error_size, i))
            return PURE_INVALID;
    return PURE_OK;
}

static int find(const char (*names)[PURE_MODULE_NAME], unsigned count, const char *name) {
    for (unsigned i = 0; i < count; ++i)
        if (!strcmp(names[i], name)) return (int)i;
    return -1;
}

int pure_module_state(const PureModule *m, const char *name) { return find(m->state_names, m->states, name); }
int pure_module_input(const PureModule *m, const char *name) { return find(m->input_names, m->inputs, name); }
int pure_module_view(const PureModule *m, const char *name) { return find(m->view_names, m->views, name); }

/* IEEE 754-2019 minimum/maximum on finite values: -0 orders below +0. */
static double minimum(double a, double b) { return a < b || (a == b && signbit(a)) ? a : b; }
static double maximum(double a, double b) { return a > b || (a == b && !signbit(a)) ? a : b; }

typedef struct { double num; PureBytes bytes; } Slot;
typedef struct {
    PureApp *app;
    const PureInputShape *shape; /* message, or NULL */
    double dt;
    bool modified, exists;
} Event;

static PureResult fault(PureApp *app, PureFault kind, uint32_t pc) {
    if (app) { app->fault = kind; app->fault_pc = pc; }
    return kind == PURE_FAULT_LIMIT ? PURE_LIMIT : kind == PURE_FAULT_NOT_FOUND ? PURE_NOT_FOUND : PURE_INVALID;
}

static bool gather(void *context, PureSpan span) {
    unsigned char **out = context;
    memcpy(*out, span.data, span.size);
    *out += span.size;
    return true;
}

/* A contiguous copy of a document range, for inserting a document value. */
static PureResult materialize(PureApp *app, PureBytes value, unsigned char **out) {
    *out = malloc(value.length ? (size_t)value.length : 1);
    if (!*out) return PURE_ALLOCATION;
    unsigned char *cursor = *out;
    PureResult result = app->data.read(app->data.context, app->document, value.version,
                                       (size_t)value.offset, (size_t)value.length, gather, &cursor);
    if (result != PURE_OK) { free(*out); *out = NULL; }
    return result;
}

/* Run a verified block. Types and stack bounds were proven at load. */
static PureResult run(const PureModule *m, double *values, PureBlock block, Event *ev, double *result) {
    Slot stack[PURE_MODULE_STACK];
    size_t sp = 0;
    PureApp *app = ev ? ev->app : NULL;
    uint32_t pc = block.start, end = block.start + block.length;
    while (pc < end) {
        const PureOp *op = &m->code[pc];
        double a, b, value;
        switch (op->op) {
        case PURE_OP_CONST: stack[sp++].num = op->value; ++pc; continue;
        case PURE_OP_LOAD: stack[sp++].num = values[op->arg]; ++pc; continue;
        case PURE_OP_DT: stack[sp++].num = ev->dt; ++pc; continue;
        case PURE_OP_STORE: values[op->arg] = stack[--sp].num; ++pc; continue;
        case PURE_OP_JZ: pc = stack[--sp].num == 0 ? op->arg : pc + 1; continue;
        case PURE_OP_JMP: pc = op->arg; continue;
        case PURE_OP_NEG: stack[sp - 1].num = -stack[sp - 1].num; ++pc; continue;
        case PURE_OP_NOT: stack[sp - 1].num = stack[sp - 1].num == 0; ++pc; continue;
        case PURE_OP_CLAMP: {
            double high = stack[--sp].num, low = stack[--sp].num;
            if (low > high) return fault(app, PURE_FAULT_CLAMP, pc);
            stack[sp - 1].num = maximum(low, minimum(stack[sp - 1].num, high));
            ++pc;
            continue;
        }
        case PURE_OP_FIELD: {
            const PureField *field = &m->field[ev->shape->field_start + op->arg];
            if (field->type == PURE_FIELD_U32) {
                const unsigned char *p = app->payload.data + field->offset;
                stack[sp++].num = (double)read32(p);
            } else {
                stack[sp++].bytes = (PureBytes){FROM_PAYLOAD, {0, 0}, field->offset,
                                                app->payload.size - field->offset};
            }
            ++pc;
            continue;
        }
        case PURE_OP_DOC: {
            if (!ev->exists) return fault(app, PURE_FAULT_NOT_FOUND, pc);
            PureVersion version = app->data.version(app->data.context, app->document);
            stack[sp++].bytes = (PureBytes){FROM_DOCUMENT, version, 0, version.bytes};
            ++pc;
            continue;
        }
        case PURE_OP_DOC_SIZE:
            stack[sp++].num = (double)app->data.version(app->data.context, app->document).bytes;
            ++pc;
            continue;
        case PURE_OP_LEN: stack[sp - 1].num = (double)stack[sp - 1].bytes.length; ++pc; continue;
        case PURE_OP_REPLACE: {
            PureBytes insert = stack[--sp].bytes;
            double last = stack[--sp].num, first = stack[--sp].num;
            double size = (double)app->data.version(app->data.context, app->document).bytes;
            if (first != floor(first) || last != floor(last) || first < 0 || last < first || last > size)
                return fault(app, PURE_FAULT_LIMIT, pc);
            unsigned char *copy = NULL;
            PureSpan span = {app->payload.data + insert.offset, (size_t)insert.length};
            if (insert.source == FROM_DOCUMENT) {
                PureResult copied = materialize(app, insert, &copy);
                if (copied != PURE_OK) return copied;
                span.data = copy;
            }
            PureResult spliced = app->data.splice(app->data.context, app->document, (size_t)first,
                                                  (size_t)(last - first), span);
            free(copy);
            if (spliced == PURE_LIMIT) return fault(app, PURE_FAULT_LIMIT, pc);
            if (spliced != PURE_OK) return spliced;
            ev->modified = ev->exists = true;
            ++pc;
            continue;
        }
        case PURE_OP_REPLY: app->reply = stack[--sp].bytes; app->has_reply = true; ++pc; continue;
        case PURE_OP_REQUIRE:
            if (stack[--sp].num == 0) return fault(app, PURE_FAULT_LIMIT, pc);
            ++pc;
            continue;
        default: break;
        }
        b = stack[--sp].num;
        a = stack[sp - 1].num;
        switch (op->op) {
        case PURE_OP_ADD: value = a + b; break;
        case PURE_OP_SUB: value = a - b; break;
        case PURE_OP_MUL: value = a * b; break;
        case PURE_OP_DIV:
            if (b == 0) return fault(app, PURE_FAULT_DIVIDE, pc);
            value = a / b;
            break;
        case PURE_OP_LT: value = a < b; break;
        case PURE_OP_LE: value = a <= b; break;
        case PURE_OP_GT: value = a > b; break;
        case PURE_OP_GE: value = a >= b; break;
        case PURE_OP_EQ: value = a == b; break;
        case PURE_OP_NE: value = a != b; break;
        case PURE_OP_AND: value = a != 0 && b != 0; break;
        case PURE_OP_OR: value = a != 0 || b != 0; break;
        case PURE_OP_MIN: value = minimum(a, b); break;
        default: value = maximum(a, b); break;
        }
        if (!isfinite(value)) return fault(app, PURE_FAULT_NON_FINITE, pc);
        stack[sp - 1].num = value;
        ++pc;
    }
    if (result) *result = stack[0].num;
    return PURE_OK;
}

void pure_app_start(PureApp *app, const PureModule *module, PureData data) {
    *app = (PureApp){.module = module, .data = data, .fault_pc = UINT32_MAX};
    memcpy(app->values, module->initial, sizeof(double) * module->states);
}

void pure_app_finish(PureApp *app) {
    if (app->document) app->data.close(app->data.context, app->document);
    free(app->copied);
    app->document = NULL;
    app->copied = NULL;
    app->has_reply = false;
    app->payload = (PureSpan){0};
}

/* All handlers of one event run in module order against a pending copy. */
static PureResult dispatch(PureApp *app, uint32_t event, Event *ev) {
    const PureModule *m = app->module;
    double pending[PURE_MODULE_STATES];
    memcpy(pending, app->values, sizeof(pending));
    app->fault = PURE_FAULT_NONE;
    app->fault_pc = UINT32_MAX;
    for (uint32_t i = 0; i < m->handlers; ++i) {
        if (m->handler[i].event != event) continue;
        PureResult result = run(m, pending, m->handler[i].code, ev, NULL);
        if (result != PURE_OK) return result;
    }
    memcpy(app->values, pending, sizeof(pending));
    ++app->events;
    return PURE_OK;
}

PureResult pure_app_frame(PureApp *app) {
    Event ev = {.app = app, .dt = 1.0 / 60.0};
    PureResult result = dispatch(app, 0, &ev);
    if (result == PURE_OK) ++app->ticks;
    return result;
}

PureResult pure_app_input(PureApp *app, uint32_t input, PureEdge edge) {
    if (input >= app->module->inputs || app->module->input[input].kind != PURE_KIND_KEY ||
        (edge != PURE_EDGE_DOWN && edge != PURE_EDGE_UP)) return PURE_INVALID;
    Event ev = {.app = app};
    return dispatch(app, 1 + 2 * input + (edge == PURE_EDGE_UP), &ev);
}

PureResult pure_app_message(PureApp *app, uint32_t input, const char *key, int64_t base, PureSpan payload) {
    pure_app_finish(app);
    const PureModule *m = app->module;
    app->fault = PURE_FAULT_NONE;
    app->fault_pc = UINT32_MAX;
    app->revision = 0;
    if (input >= m->inputs || m->input[input].kind != PURE_KIND_MESSAGE || base < -1 ||
        (payload.size && !payload.data) || (m->input[input].keyed ? !document_key(key) : key != NULL))
        return PURE_INVALID;
    const PureInputShape *shape = &m->input[input];
    if (payload.size < shape->prefix) return fault(app, PURE_FAULT_LIMIT, UINT32_MAX);
    app->payload = payload;
    if (shape->keyed && m->collections) {
        if (!app->data.open) return PURE_INVALID;
        PureResult opened = app->data.open(app->data.context, 0, key, &app->document, &app->revision);
        if (opened != PURE_OK) { app->document = NULL; pure_app_finish(app); return opened; }
        if (base >= 0 && base != app->revision) { pure_app_finish(app); return PURE_CONFLICT; }
    }
    Event ev = {.app = app, .shape = shape, .exists = app->revision > 0};
    double before[PURE_MODULE_STATES];
    memcpy(before, app->values, sizeof(before));
    uint64_t events = app->events;
    PureResult result = dispatch(app, 1 + 2 * input, &ev);
    if (result != PURE_OK) { pure_app_finish(app); return result; }
    if (ev.modified) {
        if (base < 0) result = PURE_INVALID;
        /* Commit rebases the working version; keep a document reply by copying it. */
        if (result == PURE_OK && app->has_reply && app->reply.source == FROM_DOCUMENT) {
            unsigned char *copy;
            result = materialize(app, app->reply, &copy);
            if (result == PURE_OK) {
                app->copied = copy;
                app->reply = (PureBytes){FROM_COPY, {0, 0}, 0, app->reply.length};
            }
        }
        if (result == PURE_OK) result = app->data.commit(app->data.context, app->document, &app->revision);
        if (result != PURE_OK) {
            /* A failed commit leaves the stored document; states stay as before. */
            memcpy(app->values, before, sizeof(before));
            app->events = events;
            if (result != PURE_UNCERTAIN) pure_app_finish(app);
        }
    }
    return result;
}

PureResult pure_app_reply(const PureApp *app, bool (*visit)(void *, PureSpan), void *context) {
    if (!app->has_reply || !app->reply.length) return PURE_OK;
    const PureBytes *r = &app->reply;
    if (r->source == FROM_PAYLOAD)
        return visit(context, (PureSpan){app->payload.data + r->offset, (size_t)r->length}), PURE_OK;
    if (r->source == FROM_COPY) return visit(context, (PureSpan){app->copied, (size_t)r->length}), PURE_OK;
    return app->data.read(app->data.context, app->document, r->version, (size_t)r->offset,
                          (size_t)r->length, visit, context);
}

bool pure_app_view(const PureApp *app, uint32_t view, double *out) {
    if (view >= app->module->views) return false;
    double values[PURE_MODULE_STATES];
    memcpy(values, app->values, sizeof(values));
    return run(app->module, values, app->module->view_code[view], NULL, out) == PURE_OK;
}

void pure_app_replace(PureApp *app, const PureModule *module) {
    pure_app_finish(app);
    double values[PURE_MODULE_STATES];
    for (uint32_t i = 0; i < module->states; ++i) {
        int old = pure_module_state(app->module, module->state_names[i]);
        values[i] = old >= 0 ? app->values[old] : module->initial[i];
    }
    memcpy(app->values, values, sizeof(double) * module->states);
    app->module = module;
}
