#include "module.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

enum { STATE_BYTES = 16, TABLE_BYTES = 12, OP_BYTES = 16, KEY_INPUT = 1 };
static const unsigned char magic[8] = {'P','U','R','E','M','O','D','1'};
/* Operand stack effect per opcode, in PureOpcode order. */
static const unsigned char pops[PURE_OP_COUNT] =
    {0,0,0,2,2,2,2,1,2,2,2,2,2,2,2,2,1,2,2,3,1,1,0};
static const unsigned char pushes[PURE_OP_COUNT] =
    {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0};

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

static bool merge(int16_t *height, uint32_t at, int value) {
    if (height[at] < 0) height[at] = (int16_t)value;
    return height[at] == value;
}

/* Every instruction is reachable, every path meets the same stack height at a
 * join, and the block ends with 0 (handler) or 1 (view) values. Branches only
 * move forward, so one pass in order sees all incoming edges first. */
static bool verify_block(const PureModule *m, PureBlock block, bool view,
                         char *error, size_t size, unsigned index) {
    if (block.start > m->code_count || block.length > m->code_count - block.start)
        return reject(error, size, "code range outside module", index);
    if (view && !block.length) return reject(error, size, "empty view", index);
    int16_t height[PURE_MODULE_CODE + 1];
    for (uint32_t i = 0; i <= block.length; ++i) height[i] = -1;
    height[0] = 0;
    for (uint32_t i = 0; i < block.length; ++i) {
        const PureOp *op = &m->code[block.start + i];
        int h = height[i];
        if (h < 0) return reject(error, size, "unreachable instruction", block.start + i);
        if (op->op >= PURE_OP_COUNT) return reject(error, size, "unknown opcode", block.start + i);
        if (view && (op->op == PURE_OP_DT || op->op >= PURE_OP_STORE))
            return reject(error, size, "view with an effect or branch", block.start + i);
        if (h < pops[op->op]) return reject(error, size, "stack underflow", block.start + i);
        int next = h - pops[op->op] + pushes[op->op];
        if (next > PURE_MODULE_STACK) return reject(error, size, "stack overflow", block.start + i);
        bool uses_arg = op->op == PURE_OP_LOAD || op->op == PURE_OP_STORE ||
                        op->op == PURE_OP_JZ || op->op == PURE_OP_JMP;
        if ((!uses_arg && op->arg) || (op->op != PURE_OP_CONST && op->value != 0) ||
            (op->op == PURE_OP_CONST && !isfinite(op->value)))
            return reject(error, size, "non-canonical operand", block.start + i);
        if ((op->op == PURE_OP_LOAD || op->op == PURE_OP_STORE) && op->arg >= m->states)
            return reject(error, size, "unknown state", block.start + i);
        if (op->op == PURE_OP_JZ || op->op == PURE_OP_JMP) {
            if (op->arg <= block.start + i || op->arg > block.start + block.length)
                return reject(error, size, "branch not forward within its block", block.start + i);
            if (!merge(height, op->arg - block.start, next))
                return reject(error, size, "stack height differs at branch target", block.start + i);
        }
        if (op->op != PURE_OP_JMP && !merge(height, i + 1, next))
            return reject(error, size, "stack height differs at join", block.start + i);
    }
    if (height[block.length] != (view ? 1 : 0))
        return reject(error, size, view ? "view must leave one value" : "handler must leave no value", index);
    return true;
}

PureResult pure_module_load(const void *bytes, size_t size, PureModule *out,
                            char *error, size_t error_size) {
    const unsigned char *p = bytes;
    if (error_size) error[0] = 0;
    memset(out, 0, sizeof(*out));
    if (!p || size < PURE_MODULE_HEADER || memcmp(p, magic, sizeof(magic)))
        return reject(error, error_size, "not a Pure module v1", 0), PURE_INVALID;
    out->states = (uint16_t)(p[8] | p[9] << 8);
    out->inputs = (uint16_t)(p[10] | p[11] << 8);
    out->views = (uint16_t)(p[12] | p[13] << 8);
    out->handlers = (uint16_t)(p[14] | p[15] << 8);
    out->code_count = read32(p + 16);
    uint32_t strings = read32(p + 20);
    if (read32(p + 24) || read32(p + 28))
        return reject(error, error_size, "reserved header bytes", 24), PURE_INVALID;
    if (out->states > PURE_MODULE_STATES || out->inputs > PURE_MODULE_INPUTS ||
        out->views > PURE_MODULE_VIEWS || out->handlers > PURE_MODULE_HANDLERS ||
        out->code_count > PURE_MODULE_CODE || strings > PURE_MODULE_STRINGS)
        return reject(error, error_size, "module exceeds a limit", 8), PURE_LIMIT;
    size_t expected = PURE_MODULE_HEADER + (size_t)out->states * STATE_BYTES +
        ((size_t)out->inputs + out->views + out->handlers) * TABLE_BYTES +
        (size_t)out->code_count * OP_BYTES + strings;
    if (size != expected) return reject(error, error_size, "module size", (unsigned)expected), PURE_INVALID;
    const unsigned char *at = p + PURE_MODULE_HEADER;
    const unsigned char *table = at + (size_t)out->states * STATE_BYTES +
        ((size_t)out->inputs + out->views + out->handlers) * TABLE_BYTES;
    const unsigned char *text = table + (size_t)out->code_count * OP_BYTES;
    for (uint32_t i = 0; i < out->code_count; ++i, table += OP_BYTES) {
        if (table[1] || table[2] || table[3])
            return reject(error, error_size, "instruction padding", i), PURE_INVALID;
        out->code[i] = (PureOp){.op = table[0], .arg = read32(table + 4), .value = read_double(table + 8)};
    }
    for (uint32_t i = 0; i < out->states; ++i, at += STATE_BYTES) {
        out->initial[i] = read_double(at + 8);
        if (!name_at(text, strings, read32(at), true, out->state_names[i]) || read32(at + 4))
            return reject(error, error_size, "state name", i), PURE_INVALID;
        if (!isfinite(out->initial[i])) return reject(error, error_size, "non-finite initial value", i), PURE_INVALID;
    }
    for (uint32_t i = 0; i < out->inputs; ++i, at += TABLE_BYTES) {
        if (!name_at(text, strings, read32(at), true, out->input_names[i]))
            return reject(error, error_size, "input name", i), PURE_INVALID;
        if (read32(at + 4) != KEY_INPUT || !name_at(text, strings, read32(at + 8), false, out->input_keys[i]) ||
            !key_name(out->input_keys[i])) return reject(error, error_size, "input key", i), PURE_INVALID;
        for (uint32_t k = 0; k < i; ++k)
            if (!strcmp(out->input_keys[k], out->input_keys[i]))
                return reject(error, error_size, "duplicate key", i), PURE_INVALID;
    }
    for (uint32_t i = 0; i < out->views; ++i, at += TABLE_BYTES) {
        if (!name_at(text, strings, read32(at), true, out->view_names[i]))
            return reject(error, error_size, "view name", i), PURE_INVALID;
        out->view_code[i] = (PureBlock){read32(at + 4), read32(at + 8)};
    }
    for (uint32_t i = 0; i < out->handlers; ++i, at += TABLE_BYTES) {
        out->handler[i] = (PureHandler){read32(at), {read32(at + 4), read32(at + 8)}};
        if (out->handler[i].event > 2u * out->inputs)
            return reject(error, error_size, "handler event", i), PURE_INVALID;
    }
    /* States, inputs and views share one namespace. */
    const char *names[PURE_MODULE_STATES + PURE_MODULE_INPUTS + PURE_MODULE_VIEWS];
    size_t count = 0;
    for (uint32_t i = 0; i < out->states; ++i) names[count++] = out->state_names[i];
    for (uint32_t i = 0; i < out->inputs; ++i) names[count++] = out->input_names[i];
    for (uint32_t i = 0; i < out->views; ++i) names[count++] = out->view_names[i];
    for (size_t i = 0; i < count; ++i)
        for (size_t k = 0; k < i; ++k)
            if (!strcmp(names[i], names[k])) return reject(error, error_size, "duplicate name", (unsigned)i), PURE_INVALID;
    for (uint32_t i = 0; i < out->views; ++i)
        if (!verify_block(out, out->view_code[i], true, error, error_size, i)) return PURE_INVALID;
    for (uint32_t i = 0; i < out->handlers; ++i)
        if (!verify_block(out, out->handler[i].code, false, error, error_size, i)) return PURE_INVALID;
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

/* Run a verified block. Stack bounds were proven at load. */
static PureFault run(const PureModule *m, double *values, PureBlock block, double dt,
                     double *result, uint32_t *fault_pc) {
    double stack[PURE_MODULE_STACK];
    size_t sp = 0;
    uint32_t pc = block.start, end = block.start + block.length;
    while (pc < end) {
        const PureOp *op = &m->code[pc];
        double a, b, value;
        switch (op->op) {
        case PURE_OP_CONST: stack[sp++] = op->value; ++pc; continue;
        case PURE_OP_LOAD: stack[sp++] = values[op->arg]; ++pc; continue;
        case PURE_OP_DT: stack[sp++] = dt; ++pc; continue;
        case PURE_OP_STORE: values[op->arg] = stack[--sp]; ++pc; continue;
        case PURE_OP_JZ: pc = stack[--sp] == 0 ? op->arg : pc + 1; continue;
        case PURE_OP_JMP: pc = op->arg; continue;
        case PURE_OP_NEG: stack[sp - 1] = -stack[sp - 1]; ++pc; continue;
        case PURE_OP_NOT: stack[sp - 1] = stack[sp - 1] == 0; ++pc; continue;
        case PURE_OP_CLAMP: {
            double high = stack[--sp], low = stack[--sp];
            if (low > high) { *fault_pc = pc; return PURE_FAULT_CLAMP; }
            stack[sp - 1] = maximum(low, minimum(stack[sp - 1], high));
            ++pc;
            continue;
        }
        default: break;
        }
        b = stack[--sp];
        a = stack[sp - 1];
        switch (op->op) {
        case PURE_OP_ADD: value = a + b; break;
        case PURE_OP_SUB: value = a - b; break;
        case PURE_OP_MUL: value = a * b; break;
        case PURE_OP_DIV:
            if (b == 0) { *fault_pc = pc; return PURE_FAULT_DIVIDE; }
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
        if (!isfinite(value)) { *fault_pc = pc; return PURE_FAULT_NON_FINITE; }
        stack[sp - 1] = value;
        ++pc;
    }
    if (result) *result = stack[0];
    return PURE_FAULT_NONE;
}

void pure_app_start(PureApp *app, const PureModule *module) {
    *app = (PureApp){.module = module, .fault_pc = UINT32_MAX};
    memcpy(app->values, module->initial, sizeof(double) * module->states);
}

/* All handlers of one event run in module order against a pending copy. */
static PureResult dispatch(PureApp *app, uint32_t event, double dt) {
    const PureModule *m = app->module;
    double pending[PURE_MODULE_STATES];
    memcpy(pending, app->values, sizeof(pending));
    app->fault = PURE_FAULT_NONE;
    app->fault_pc = UINT32_MAX;
    for (uint32_t i = 0; i < m->handlers; ++i) {
        if (m->handler[i].event != event) continue;
        app->fault = run(m, pending, m->handler[i].code, dt, NULL, &app->fault_pc);
        if (app->fault) return PURE_INVALID;
    }
    memcpy(app->values, pending, sizeof(pending));
    ++app->events;
    return PURE_OK;
}

PureResult pure_app_frame(PureApp *app) {
    PureResult result = dispatch(app, 0, 1.0 / 60.0);
    if (result == PURE_OK) ++app->ticks;
    return result;
}

PureResult pure_app_input(PureApp *app, uint32_t input, PureEdge edge) {
    if (input >= app->module->inputs || (edge != PURE_EDGE_DOWN && edge != PURE_EDGE_UP)) return PURE_INVALID;
    return dispatch(app, 1 + 2 * input + (edge == PURE_EDGE_UP), 0);
}

bool pure_app_view(const PureApp *app, uint32_t view, double *out) {
    if (view >= app->module->views) return false;
    double values[PURE_MODULE_STATES];
    uint32_t pc;
    memcpy(values, app->values, sizeof(values));
    return run(app->module, values, app->module->view_code[view], 0, out, &pc) == PURE_FAULT_NONE;
}

void pure_app_replace(PureApp *app, const PureModule *module) {
    double values[PURE_MODULE_STATES];
    for (uint32_t i = 0; i < module->states; ++i) {
        int old = pure_module_state(app->module, module->state_names[i]);
        values[i] = old >= 0 ? app->values[old] : module->initial[i];
    }
    memcpy(app->values, values, sizeof(double) * module->states);
    app->module = module;
}
