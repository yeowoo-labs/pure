#include "module.h"
#include "module-text.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { STATE_BYTES = 16, COLLECTION_BYTES = 12, INPUT_BYTES = 16, FIELD_BYTES = 8,
       CONSTANT_BYTES = 8, VIEW_BYTES = 16, HANDLER_BYTES = 12, OP_BYTES = 16 };
enum { T_NUM = 1, T_BYTES = 2, T_TEXT = 3, T_CURSOR = 4, T_FIELD = 5 /* type of the named field */ };
enum { FROM_PAYLOAD, FROM_DOCUMENT, FROM_COPY, FROM_CONSTANT, FROM_TEXT, FROM_ARENA };
/* Where an opcode may appear. */
enum { IN_VIEW = 1, IN_HANDLER = 2, IN_MESSAGE = 4, IN_KEYED = 8, IN_TEXT = 16 };
enum { READ = IN_VIEW | IN_HANDLER };
static const unsigned char magic[8] = {'P','U','R','E','M','O','D','3'};

/* Operand types popped (bottom to top), the type pushed, and where allowed. */
static const struct { unsigned char pops, in[3], out, where; } signature[PURE_OP_COUNT] = {
    [PURE_OP_CONST] = {0, {0}, T_NUM, READ},
    [PURE_OP_LOAD] = {0, {0}, T_NUM, READ},
    [PURE_OP_DT] = {0, {0}, T_NUM, IN_HANDLER},
    [PURE_OP_ADD] = {2, {T_NUM, T_NUM}, T_NUM, READ},
    [PURE_OP_SUB] = {2, {T_NUM, T_NUM}, T_NUM, READ},
    [PURE_OP_MUL] = {2, {T_NUM, T_NUM}, T_NUM, READ},
    [PURE_OP_DIV] = {2, {T_NUM, T_NUM}, T_NUM, READ},
    [PURE_OP_NEG] = {1, {T_NUM}, T_NUM, READ},
    [PURE_OP_LT] = {2, {T_NUM, T_NUM}, T_NUM, READ},
    [PURE_OP_LE] = {2, {T_NUM, T_NUM}, T_NUM, READ},
    [PURE_OP_GT] = {2, {T_NUM, T_NUM}, T_NUM, READ},
    [PURE_OP_GE] = {2, {T_NUM, T_NUM}, T_NUM, READ},
    [PURE_OP_EQ] = {2, {T_NUM, T_NUM}, T_NUM, READ},
    [PURE_OP_NE] = {2, {T_NUM, T_NUM}, T_NUM, READ},
    [PURE_OP_AND] = {2, {T_NUM, T_NUM}, T_NUM, READ},
    [PURE_OP_OR] = {2, {T_NUM, T_NUM}, T_NUM, READ},
    [PURE_OP_NOT] = {1, {T_NUM}, T_NUM, READ},
    [PURE_OP_MIN] = {2, {T_NUM, T_NUM}, T_NUM, READ},
    [PURE_OP_MAX] = {2, {T_NUM, T_NUM}, T_NUM, READ},
    [PURE_OP_CLAMP] = {3, {T_NUM, T_NUM, T_NUM}, T_NUM, READ},
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
    [PURE_OP_TEXT_CONST] = {0, {0}, T_TEXT, READ},
    [PURE_OP_TEXT_LOAD] = {0, {0}, T_TEXT, READ},
    [PURE_OP_TEXT_INPUT] = {0, {0}, T_TEXT, IN_TEXT},
    [PURE_OP_NUM_TEXT] = {1, {T_NUM}, T_TEXT, READ},
    [PURE_OP_CONCAT] = {2, {T_TEXT, T_TEXT}, T_TEXT, READ},
    [PURE_OP_CHARS] = {1, {T_TEXT}, T_NUM, READ},
    [PURE_OP_LINES] = {1, {T_TEXT}, T_NUM, READ},
    [PURE_OP_TEXT_BYTES] = {1, {T_TEXT}, T_NUM, READ},
    [PURE_OP_CUR_LOAD] = {0, {0}, T_CURSOR, READ},
    [PURE_OP_CUR_START] = {0, {0}, T_CURSOR, READ},
    [PURE_OP_CUR_END] = {0, {0}, T_CURSOR, READ},
    [PURE_OP_CUR_MOVE] = {2, {T_CURSOR, T_NUM}, T_CURSOR, READ},
    [PURE_OP_CUR_STORE] = {1, {T_CURSOR}, 0, IN_HANDLER},
    [PURE_OP_TEXT_INSERT] = {2, {T_CURSOR, T_TEXT}, 0, IN_HANDLER},
    [PURE_OP_TEXT_REMOVE] = {2, {T_CURSOR, T_CURSOR}, 0, IN_HANDLER},
    [PURE_OP_TEXT_LINE] = {1, {T_CURSOR}, T_TEXT, READ},
    [PURE_OP_TEXT_SET] = {1, {T_TEXT}, 0, IN_HANDLER},
    [PURE_OP_TEXT_FROM] = {1, {T_BYTES}, T_TEXT, READ},
    [PURE_OP_BYTES_FROM] = {1, {T_TEXT}, T_BYTES, READ},
    [PURE_OP_CUR_COUNT] = {1, {T_CURSOR}, T_NUM, READ},
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
    static const char *const named[] = {"Space", "Enter", "ArrowLeft", "ArrowRight", "ArrowUp", "ArrowDown",
                                        "Backspace", "Delete", "Home", "End", "Tab", "Escape", "PageUp", "PageDown"};
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

static bool uses_arg(uint8_t op) {
    switch (op) {
    case PURE_OP_LOAD: case PURE_OP_STORE: case PURE_OP_JZ: case PURE_OP_JMP: case PURE_OP_FIELD:
    case PURE_OP_DOC: case PURE_OP_DOC_SIZE: case PURE_OP_REPLACE: case PURE_OP_TEXT_CONST:
    case PURE_OP_TEXT_LOAD: case PURE_OP_CUR_LOAD: case PURE_OP_CUR_START: case PURE_OP_CUR_END:
    case PURE_OP_CUR_MOVE: case PURE_OP_CUR_STORE: case PURE_OP_TEXT_INSERT: case PURE_OP_TEXT_REMOVE:
    case PURE_OP_TEXT_LINE: case PURE_OP_TEXT_SET: case PURE_OP_CUR_COUNT:
        return true;
    default: return false;
    }
}

/* The state an argument names must have this type. */
static bool state_of(const PureModule *m, uint32_t state, uint8_t type) {
    return state < m->states && m->state_type[state] == type;
}

/* Types and heights are checked per instruction; a cursor's type records
 * which text it belongs to. Branches go forward within the block and every
 * join happens with an empty operand stack, so control flow never merges
 * typed values. Handlers end empty, views with one value of their type. */
static bool verify_block(const PureModule *m, PureBlock block, int view, uint32_t event,
                         char *error, size_t size, unsigned index) {
    if (block.start > m->code_count || block.length > m->code_count - block.start)
        return reject(error, size, "code range outside module", index);
    if (view >= 0 && !block.length) return reject(error, size, "empty view", index);
    int input = view >= 0 ? -1 : event_input(event);
    const PureInputShape *shape = input >= 0 ? &m->input[input] : NULL;
    unsigned where = view >= 0 ? IN_VIEW : IN_HANDLER;
    if (shape && shape->kind == PURE_KIND_MESSAGE) where |= IN_MESSAGE | (shape->keyed ? IN_KEYED : 0);
    if (shape && shape->kind == PURE_KIND_TEXT) where |= IN_TEXT;
    unsigned char types[PURE_MODULE_STACK], owners[PURE_MODULE_STACK], target[PURE_MODULE_CODE + 1] = {0};
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
            if (view >= 0) return reject(error, size, "view with an effect or branch", pc);
            if (allowed & IN_KEYED) return reject(error, size, "document operation outside a keyed message handler", pc);
            if (allowed & IN_TEXT) return reject(error, size, "text input outside a text handler", pc);
            return reject(error, size, "message operation outside a message handler", pc);
        }
        if ((!uses_arg(op->op) && op->arg) || (op->op != PURE_OP_CONST && op->value != 0) ||
            (op->op == PURE_OP_CONST && !isfinite(op->value)))
            return reject(error, size, "non-canonical operand", pc);
        uint32_t arg = op->arg;
        bool known = true;
        switch (op->op) {
        case PURE_OP_LOAD: case PURE_OP_STORE: known = state_of(m, arg, PURE_STATE_NUM); break;
        case PURE_OP_FIELD: known = arg < shape->field_count; break;
        case PURE_OP_DOC: case PURE_OP_DOC_SIZE: case PURE_OP_REPLACE: known = arg < m->collections; break;
        case PURE_OP_TEXT_CONST: known = arg < m->constants; break;
        case PURE_OP_TEXT_LOAD: case PURE_OP_TEXT_SET: case PURE_OP_CUR_START: case PURE_OP_CUR_END:
        case PURE_OP_TEXT_INSERT: case PURE_OP_TEXT_REMOVE: case PURE_OP_TEXT_LINE:
            known = state_of(m, arg, PURE_STATE_TEXT);
            break;
        case PURE_OP_CUR_LOAD: case PURE_OP_CUR_STORE: known = state_of(m, arg, PURE_STATE_CURSOR); break;
        case PURE_OP_CUR_MOVE: case PURE_OP_CUR_COUNT: known = arg <= PURE_UNIT_LINE; break;
        default: break;
        }
        if (!known) return reject(error, size, "operand names no such state, field, constant or unit", pc);
        unsigned pops = signature[op->op].pops;
        if (height < (int)pops) return reject(error, size, "stack underflow", pc);
        for (unsigned k = 0; k < pops; ++k)
            if (types[height - (int)pops + (int)k] != signature[op->op].in[k])
                return reject(error, size, "operand type", pc);
        /* The text a cursor operand belongs to. */
        int owner = pops && types[height - 1] == T_CURSOR ? owners[height - 1] :
                    pops > 1 && types[height - 2] == T_CURSOR ? owners[height - 2] : -1;
        bool same_text = true;
        switch (op->op) {
        case PURE_OP_CUR_STORE: same_text = owner == m->state_ref[arg]; break;
        case PURE_OP_TEXT_INSERT: case PURE_OP_TEXT_LINE: same_text = owner == (int)arg; break;
        case PURE_OP_TEXT_REMOVE: same_text = owners[height - 1] == arg && owners[height - 2] == arg; break;
        default: break;
        }
        if (!same_text) return reject(error, size, "cursor of another text", pc);
        height -= (int)pops;
        unsigned char out = signature[op->op].out;
        if (out == T_FIELD) out = m->field[shape->field_start + arg].type == PURE_FIELD_U32 ? T_NUM : T_BYTES;
        if (out) {
            if (height == PURE_MODULE_STACK) return reject(error, size, "stack overflow", pc);
            owners[height] = (unsigned char)(op->op == PURE_OP_CUR_LOAD ? m->state_ref[arg] :
                              op->op == PURE_OP_CUR_START || op->op == PURE_OP_CUR_END ? arg :
                              owner >= 0 ? (unsigned)owner : 0);
            types[height++] = out;
        }
        if (op->op == PURE_OP_JZ || op->op == PURE_OP_JMP) {
            if (arg <= pc || arg > block.start + block.length)
                return reject(error, size, "branch not forward within its block", pc);
            if (height) return reject(error, size, "branch with a non-empty stack", pc);
            target[arg - block.start] = 1;
            if (op->op == PURE_OP_JMP) reachable = false;
        }
    }
    if (!reachable) return reject(error, size, "block end is unreachable", index);
    unsigned char result = view == PURE_VIEW_TEXT ? T_TEXT : T_NUM;
    if (view >= 0 ? height != 1 || types[0] != result : height != 0)
        return reject(error, size, view >= 0 ? "view must leave one value of its type" : "handler must leave no value", index);
    return true;
}

PureResult pure_module_load(const void *bytes, size_t size, PureModule *out,
                            char *error, size_t error_size) {
    const unsigned char *p = bytes;
    if (error_size) error[0] = 0;
    memset(out, 0, sizeof(*out));
    if (!p || size < PURE_MODULE_HEADER || memcmp(p, magic, sizeof(magic)))
        return reject(error, error_size, "not a Pure module v3", 0), PURE_INVALID;
    out->states = (uint16_t)read16(p + 8);
    out->inputs = (uint16_t)read16(p + 10);
    out->views = (uint16_t)read16(p + 12);
    out->handlers = (uint16_t)read16(p + 14);
    out->code_count = read32(p + 16);
    uint32_t strings = read32(p + 20);
    out->fields = (uint16_t)read16(p + 24);
    out->collections = (uint16_t)read16(p + 26);
    out->constants = (uint16_t)read16(p + 28);
    uint32_t constant_bytes = read32(p + 32);
    if (read16(p + 30) || read32(p + 36)) return reject(error, error_size, "reserved header bytes", 30), PURE_INVALID;
    if (out->states > PURE_MODULE_STATES || out->inputs > PURE_MODULE_INPUTS ||
        out->views > PURE_MODULE_VIEWS || out->handlers > PURE_MODULE_HANDLERS ||
        out->code_count > PURE_MODULE_CODE || strings > PURE_MODULE_STRINGS ||
        out->fields > PURE_MODULE_FIELDS || out->collections > PURE_MODULE_COLLECTIONS ||
        out->constants > PURE_MODULE_CONSTANTS || constant_bytes > PURE_MODULE_CONSTANT_BYTES)
        return reject(error, error_size, "module exceeds a limit", 8), PURE_LIMIT;
    size_t expected = PURE_MODULE_HEADER + (size_t)out->states * STATE_BYTES +
        (size_t)out->collections * COLLECTION_BYTES + (size_t)out->inputs * INPUT_BYTES +
        (size_t)out->fields * FIELD_BYTES + (size_t)out->constants * CONSTANT_BYTES +
        (size_t)out->views * VIEW_BYTES + (size_t)out->handlers * HANDLER_BYTES +
        (size_t)out->code_count * OP_BYTES + strings + constant_bytes;
    if (size != expected) return reject(error, error_size, "module size", (unsigned)expected), PURE_INVALID;
    const unsigned char *at = p + PURE_MODULE_HEADER;
    const unsigned char *text = p + size - constant_bytes - strings;
    memcpy(out->constant_data, p + size - constant_bytes, constant_bytes);
    for (uint32_t i = 0; i < out->states; ++i, at += STATE_BYTES) {
        out->state_type[i] = (uint8_t)read32(at + 4);
        if (!name_at(text, strings, read32(at), true, out->state_names[i]))
            return reject(error, error_size, "state name", i), PURE_INVALID;
        if (read32(at + 4) > PURE_STATE_CURSOR) return reject(error, error_size, "state type", i), PURE_INVALID;
        if (out->state_type[i] == PURE_STATE_NUM) {
            out->initial[i] = read_double(at + 8);
            if (!isfinite(out->initial[i])) return reject(error, error_size, "non-finite initial value", i), PURE_INVALID;
        } else {
            if (read32(at + 12) || read32(at + 8) > UINT16_MAX)
                return reject(error, error_size, "state reference", i), PURE_INVALID;
            out->state_ref[i] = (uint16_t)read32(at + 8);
        }
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
        if (shape->kind == PURE_KIND_TEXT) {
            if (shape->keyed || shape->field_start || shape->field_count || read32(at + 8))
                return reject(error, error_size, "text input shape", i), PURE_INVALID;
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
    for (uint32_t i = 0; i < out->constants; ++i, at += CONSTANT_BYTES) {
        out->constant_offset[i] = read32(at);
        out->constant_length[i] = read32(at + 4);
        if (out->constant_offset[i] > constant_bytes || out->constant_length[i] > constant_bytes - out->constant_offset[i] ||
            !pure_utf8_valid((PureSpan){out->constant_data + out->constant_offset[i], out->constant_length[i]}))
            return reject(error, error_size, "text constant", i), PURE_INVALID;
    }
    for (uint32_t i = 0; i < out->states; ++i)
        if ((out->state_type[i] == PURE_STATE_TEXT && out->state_ref[i] >= out->constants) ||
            (out->state_type[i] == PURE_STATE_CURSOR && !state_of(out, out->state_ref[i], PURE_STATE_TEXT)))
            return reject(error, error_size, "state reference", i), PURE_INVALID;
    for (uint32_t i = 0; i < out->views; ++i, at += VIEW_BYTES) {
        if (!name_at(text, strings, read32(at), true, out->view_names[i]))
            return reject(error, error_size, "view name", i), PURE_INVALID;
        if (read32(at + 4) > PURE_VIEW_TEXT) return reject(error, error_size, "view type", i), PURE_INVALID;
        out->view_type[i] = (uint8_t)read32(at + 4);
        out->view_code[i] = (PureBlock){read32(at + 8), read32(at + 12)};
    }
    for (uint32_t i = 0; i < out->handlers; ++i, at += HANDLER_BYTES) {
        uint32_t event = read32(at);
        out->handler[i] = (PureHandler){event, {read32(at + 4), read32(at + 8)}};
        int input = event_input(event);
        if (event > 2u * out->inputs || (input >= 0 && out->input[input].kind != PURE_KIND_KEY && !(event & 1)))
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
        if (!verify_block(out, out->view_code[i], out->view_type[i], 0, error, error_size, i)) return PURE_INVALID;
    for (uint32_t i = 0; i < out->handlers; ++i)
        if (!verify_block(out, out->handler[i].code, -1, out->handler[i].event, error, error_size, i))
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

/* ---- Execution ------------------------------------------------------ */

/* IEEE 754-2019 minimum/maximum on finite values: -0 orders below +0. */
static double minimum(double a, double b) { return a < b || (a == b && signbit(a)) ? a : b; }
static double maximum(double a, double b) { return a > b || (a == b && !signbit(a)) ? a : b; }

typedef struct { double num; PureBytes bytes; } Slot;
typedef struct {
    PureApp *app;
    const PureInputShape *shape; /* message, or NULL */
    PureArena *arena;
    double dt;
    bool modified, exists;
} Event;

static PureResult fault(PureApp *app, PureFault kind, uint32_t pc) {
    app->fault = kind;
    app->fault_pc = pc;
    return kind == PURE_FAULT_LIMIT ? PURE_LIMIT : kind == PURE_FAULT_NOT_FOUND ? PURE_NOT_FOUND : PURE_INVALID;
}

static bool reserve(PureArena *arena, size_t more) {
    if (more <= arena->capacity - arena->used) return true;
    size_t capacity = arena->capacity ? arena->capacity : 256;
    while (capacity - arena->used < more) {
        if (capacity > SIZE_MAX / 2) return false;
        capacity *= 2;
    }
    unsigned char *bytes = realloc(arena->bytes, capacity);
    if (!bytes) return false;
    arena->bytes = bytes;
    arena->capacity = capacity;
    return true;
}

/* Visit a value's bytes in order, from wherever they live. */
static PureResult visit_value(const PureApp *app, const PureArena *arena, PureBytes v,
                              bool (*visit)(void *, PureSpan), void *context) {
    if (!v.length) return PURE_OK;
    const unsigned char *direct = NULL;
    switch (v.source) {
    case FROM_PAYLOAD: direct = app->payload.data + v.offset; break;
    case FROM_COPY: direct = app->copied + v.offset; break;
    case FROM_CONSTANT: direct = app->module->constant_data + v.offset; break;
    case FROM_ARENA: direct = arena->bytes + v.offset; break;
    case FROM_TEXT:
        return pure_draft_read_version(app->texts[v.owner], v.version, (size_t)v.offset, (size_t)v.length,
                                       visit, context);
    default:
        return app->data.read(app->data.context, app->document, v.version, (size_t)v.offset,
                              (size_t)v.length, visit, context);
    }
    visit(context, (PureSpan){direct, (size_t)v.length});
    return PURE_OK;
}

static bool append_span(void *context, PureSpan span) {
    PureArena *arena = context;
    memmove(arena->bytes + arena->used, span.data, span.size);
    arena->used += span.size;
    return true;
}

/* Copy a value to the end of the arena. Room is reserved first, so reading
 * an arena value while appending never follows a moved buffer. */
static PureResult append_value(const PureApp *app, PureArena *arena, PureBytes v) {
    if (!reserve(arena, (size_t)v.length)) return PURE_ALLOCATION;
    return visit_value(app, arena, v, append_span, arena);
}

/* A contiguous span of a value, copying into the arena when it is spread out. */
static PureResult contiguous(const PureApp *app, PureArena *arena, PureBytes v, PureSpan *out) {
    if (v.source == FROM_PAYLOAD || v.source == FROM_COPY || v.source == FROM_CONSTANT) {
        const unsigned char *base = v.source == FROM_PAYLOAD ? app->payload.data :
                                    v.source == FROM_COPY ? app->copied : app->module->constant_data;
        *out = (PureSpan){v.length ? base + v.offset : NULL, (size_t)v.length};
        return PURE_OK;
    }
    if (v.source == FROM_ARENA) {
        *out = (PureSpan){arena->bytes + v.offset, (size_t)v.length};
        return PURE_OK;
    }
    size_t start = arena->used;
    PureResult result = append_value(app, arena, v);
    *out = (PureSpan){arena->bytes + start, (size_t)v.length};
    return result;
}

typedef struct { size_t count; unsigned unit; unsigned utf8; bool valid; } Measure;

static bool measure(void *context, PureSpan span) {
    Measure *m = context;
    if (m->unit == 2) m->valid = m->valid && pure_utf8_step(&m->utf8, span);
    else m->count += m->unit ? pure_utf8_newlines(span) : pure_utf8_scalars(span);
    return true;
}

static PureBytes text_of(const PureApp *app, uint32_t state) {
    PureVersion version = pure_draft_version(app->texts[state]);
    return (PureBytes){FROM_TEXT, (uint16_t)state, version, 0, version.bytes};
}

static void move_cursors(const PureModule *m, double *values, uint32_t text, size_t at,
                         size_t removed, size_t inserted) {
    for (uint32_t c = 0; c < m->states; ++c) {
        if (m->state_type[c] != PURE_STATE_CURSOR || m->state_ref[c] != text) continue;
        double position = values[c];
        if (removed) {
            if (position >= at + removed) position -= (double)removed;
            else if (position > at) position = (double)at;
        } else if (position >= at) position += (double)inserted;
        values[c] = position;
    }
}

/* Run a verified block. Types and stack bounds were proven at load. */
static PureResult run(const PureModule *m, double *values, PureBlock block, Event *ev, Slot *result) {
    Slot stack[PURE_MODULE_STACK];
    size_t sp = 0;
    PureApp *app = ev->app;
    uint32_t pc = block.start, end = block.start + block.length;
    while (pc < end) {
        const PureOp *op = &m->code[pc];
        double a, b, value;
        PureResult status;
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
            if (field->type == PURE_FIELD_U32) stack[sp++].num = (double)(
                (uint32_t)app->payload.data[field->offset] | (uint32_t)app->payload.data[field->offset + 1] << 8 |
                (uint32_t)app->payload.data[field->offset + 2] << 16 | (uint32_t)app->payload.data[field->offset + 3] << 24);
            else stack[sp++].bytes = (PureBytes){FROM_PAYLOAD, 0, {0, 0}, field->offset,
                                                 app->payload.size - field->offset};
            ++pc;
            continue;
        }
        case PURE_OP_DOC: {
            if (!ev->exists) return fault(app, PURE_FAULT_NOT_FOUND, pc);
            PureVersion version = app->data.version(app->data.context, app->document);
            stack[sp++].bytes = (PureBytes){FROM_DOCUMENT, 0, version, 0, version.bytes};
            ++pc;
            continue;
        }
        case PURE_OP_DOC_SIZE:
            stack[sp++].num = (double)app->data.version(app->data.context, app->document).bytes;
            ++pc;
            continue;
        case PURE_OP_LEN: case PURE_OP_TEXT_BYTES:
            stack[sp - 1].num = (double)stack[sp - 1].bytes.length;
            ++pc;
            continue;
        case PURE_OP_REPLACE: {
            PureBytes insert = stack[--sp].bytes;
            double last = stack[--sp].num, first = stack[--sp].num;
            double size = (double)app->data.version(app->data.context, app->document).bytes;
            if (first != floor(first) || last != floor(last) || first < 0 || last < first || last > size)
                return fault(app, PURE_FAULT_LIMIT, pc);
            PureSpan span;
            size_t mark = ev->arena->used;
            if ((status = contiguous(app, ev->arena, insert, &span)) != PURE_OK) return status;
            status = app->data.splice(app->data.context, app->document, (size_t)first, (size_t)(last - first), span);
            ev->arena->used = mark;
            if (status == PURE_LIMIT) return fault(app, PURE_FAULT_LIMIT, pc);
            if (status != PURE_OK) return status;
            ev->modified = ev->exists = true;
            ++pc;
            continue;
        }
        case PURE_OP_REPLY: {
            /* A reply outlives the arena mark of later statements: keep it by value. */
            app->reply = stack[--sp].bytes;
            app->has_reply = true;
            ++pc;
            continue;
        }
        case PURE_OP_REQUIRE:
            if (stack[--sp].num == 0) return fault(app, PURE_FAULT_LIMIT, pc);
            ++pc;
            continue;
        case PURE_OP_TEXT_CONST:
            stack[sp++].bytes = (PureBytes){FROM_CONSTANT, 0, {0, 0}, m->constant_offset[op->arg],
                                            m->constant_length[op->arg]};
            ++pc;
            continue;
        case PURE_OP_TEXT_LOAD: stack[sp++].bytes = text_of(app, op->arg); ++pc; continue;
        case PURE_OP_TEXT_INPUT:
            stack[sp++].bytes = (PureBytes){FROM_PAYLOAD, 0, {0, 0}, 0, app->payload.size};
            ++pc;
            continue;
        case PURE_OP_NUM_TEXT: {
            char digits[32];
            size_t length = pure_format_number(stack[sp - 1].num, digits);
            if (!reserve(ev->arena, length)) return PURE_ALLOCATION;
            memcpy(ev->arena->bytes + ev->arena->used, digits, length);
            stack[sp - 1].bytes = (PureBytes){FROM_ARENA, 0, {0, 0}, ev->arena->used, length};
            ev->arena->used += length;
            ++pc;
            continue;
        }
        case PURE_OP_CONCAT: {
            PureBytes right = stack[--sp].bytes, left = stack[sp - 1].bytes;
            size_t start = ev->arena->used;
            if ((status = append_value(app, ev->arena, left)) != PURE_OK ||
                (status = append_value(app, ev->arena, right)) != PURE_OK) return status;
            stack[sp - 1].bytes = (PureBytes){FROM_ARENA, 0, {0, 0}, start, left.length + right.length};
            ++pc;
            continue;
        }
        case PURE_OP_CHARS: case PURE_OP_LINES: {
            Measure count = {0, op->op == PURE_OP_LINES, 0, true};
            if ((status = visit_value(app, ev->arena, stack[sp - 1].bytes, measure, &count)) != PURE_OK) return status;
            stack[sp - 1].num = (double)(count.count + (op->op == PURE_OP_LINES));
            ++pc;
            continue;
        }
        case PURE_OP_CUR_LOAD:
            stack[sp].num = values[op->arg];
            stack[sp++].bytes.owner = m->state_ref[op->arg];
            ++pc;
            continue;
        case PURE_OP_CUR_START: case PURE_OP_CUR_END:
            stack[sp].num = op->op == PURE_OP_CUR_START ? 0 : (double)pure_draft_size(app->texts[op->arg]);
            stack[sp++].bytes.owner = (uint16_t)op->arg;
            ++pc;
            continue;
        case PURE_OP_CUR_MOVE: {
            double count = stack[--sp].num;
            if (count != floor(count)) return fault(app, PURE_FAULT_LIMIT, pc);
            PureDraft *text = app->texts[stack[sp - 1].bytes.owner];
            PureVersion version = pure_draft_version(text);
            size_t position = (size_t)stack[sp - 1].num;
            stack[sp - 1].num = (double)(op->arg == PURE_UNIT_CHAR ? pure_text_step(text, version, position, count)
                                                                   : pure_text_line_step(text, version, position, count));
            ++pc;
            continue;
        }
        case PURE_OP_CUR_STORE: values[op->arg] = stack[--sp].num; ++pc; continue;
        case PURE_OP_CUR_COUNT: {
            PureDraft *text = app->texts[stack[sp - 1].bytes.owner];
            stack[sp - 1].num = (double)pure_text_count(text, pure_draft_version(text),
                                                        (size_t)stack[sp - 1].num, op->arg);
            ++pc;
            continue;
        }
        case PURE_OP_TEXT_INSERT: case PURE_OP_TEXT_SET: {
            PureBytes insert = stack[--sp].bytes;
            size_t at = op->op == PURE_OP_TEXT_INSERT ? (size_t)stack[--sp].num : 0;
            size_t removed = op->op == PURE_OP_TEXT_SET ? pure_draft_size(app->texts[op->arg]) : 0;
            PureSpan span;
            size_t mark = ev->arena->used;
            if ((status = contiguous(app, ev->arena, insert, &span)) != PURE_OK) return status;
            status = pure_draft_splice(app->texts[op->arg], at, removed, span);
            ev->arena->used = mark;
            if (status == PURE_LIMIT) return fault(app, PURE_FAULT_LIMIT, pc);
            if (status != PURE_OK) return status;
            if (op->op == PURE_OP_TEXT_SET) {
                for (uint32_t c = 0; c < m->states; ++c)
                    if (m->state_type[c] == PURE_STATE_CURSOR && m->state_ref[c] == op->arg) values[c] = 0;
            } else move_cursors(m, values, op->arg, at, 0, span.size);
            ++pc;
            continue;
        }
        case PURE_OP_TEXT_REMOVE: {
            double y = stack[--sp].num, x = stack[--sp].num;
            size_t first = (size_t)minimum(x, y), last = (size_t)maximum(x, y);
            if ((status = pure_draft_splice(app->texts[op->arg], first, last - first, (PureSpan){0})) != PURE_OK)
                return status;
            if (last > first) move_cursors(m, values, op->arg, first, last - first, 0);
            ++pc;
            continue;
        }
        case PURE_OP_TEXT_LINE: {
            PureDraft *text = app->texts[op->arg];
            PureVersion version = pure_draft_version(text);
            size_t position = (size_t)stack[sp - 1].num;
            size_t first = pure_text_line_start(text, version, position);
            size_t last = pure_text_line_end(text, version, position);
            stack[sp - 1].bytes = (PureBytes){FROM_TEXT, (uint16_t)op->arg, version, first, last - first};
            ++pc;
            continue;
        }
        case PURE_OP_TEXT_FROM: {
            Measure check = {0, 2, 0, true};
            if ((status = visit_value(app, ev->arena, stack[sp - 1].bytes, measure, &check)) != PURE_OK) return status;
            if (!check.valid || !pure_utf8_complete(check.utf8)) return fault(app, PURE_FAULT_ENCODING, pc);
            ++pc;
            continue;
        }
        case PURE_OP_BYTES_FROM: ++pc; continue; /* same bytes, now untyped */
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
    if (result) *result = stack[0];
    return PURE_OK;
}

static PureResult new_text(PureDraft **out, const PureModule *m, uint32_t constant) {
    *out = pure_draft_new();
    if (!*out) return PURE_ALLOCATION;
    PureSpan initial = {m->constant_data + m->constant_offset[constant], m->constant_length[constant]};
    PureResult result = pure_draft_splice(*out, 0, 0, initial);
    if (result != PURE_OK) { pure_draft_close(*out); *out = NULL; }
    return result;
}

PureResult pure_app_start(PureApp *app, const PureModule *module, PureData data) {
    *app = (PureApp){.module = module, .data = data, .fault_pc = UINT32_MAX};
    memcpy(app->values, module->initial, sizeof(double) * module->states);
    for (uint32_t i = 0; i < module->states; ++i) {
        if (module->state_type[i] == PURE_STATE_CURSOR) app->values[i] = 0;
        if (module->state_type[i] != PURE_STATE_TEXT) continue;
        PureResult result = new_text(&app->texts[i], module, module->state_ref[i]);
        if (result != PURE_OK) { pure_app_stop(app); return result; }
    }
    return PURE_OK;
}

void pure_app_finish(PureApp *app) {
    if (app->document) app->data.close(app->data.context, app->document);
    free(app->copied);
    app->document = NULL;
    app->copied = NULL;
    app->has_reply = false;
    app->payload = (PureSpan){0};
    app->arena.used = 0;
}

void pure_app_stop(PureApp *app) {
    pure_app_finish(app);
    for (uint32_t i = 0; i < PURE_MODULE_STATES; ++i) pure_draft_close(app->texts[i]);
    free(app->arena.bytes);
    free(app->view_arena.bytes);
    memset(app, 0, sizeof(*app));
}

/* Every text draft is held for the event; on failure each returns to the
 * version it had when the event began. */
typedef struct { PureVersion before[PURE_MODULE_STATES]; } Texts;

static void hold_texts(PureApp *app, Texts *texts) {
    for (uint32_t i = 0; i < app->module->states; ++i)
        if (app->texts[i]) {
            pure_draft_hold(app->texts[i], true);
            texts->before[i] = pure_draft_version(app->texts[i]);
        }
}

static void release_texts(PureApp *app, const Texts *texts, bool keep) {
    for (uint32_t i = 0; i < app->module->states; ++i)
        if (app->texts[i]) {
            if (!keep) pure_draft_restore(app->texts[i], texts->before[i]);
            pure_draft_hold(app->texts[i], false);
        }
}

/* All handlers of one event run in module order against pending states. */
static PureResult run_handlers(PureApp *app, uint32_t event, Event *ev, double *pending) {
    const PureModule *m = app->module;
    memcpy(pending, app->values, sizeof(app->values));
    app->fault = PURE_FAULT_NONE;
    app->fault_pc = UINT32_MAX;
    for (uint32_t i = 0; i < m->handlers; ++i) {
        if (m->handler[i].event != event) continue;
        PureResult result = run(m, pending, m->handler[i].code, ev, NULL);
        if (result != PURE_OK) return result;
    }
    return PURE_OK;
}

static PureResult dispatch(PureApp *app, uint32_t event, Event *ev) {
    double pending[PURE_MODULE_STATES];
    Texts texts;
    hold_texts(app, &texts);
    PureResult result = run_handlers(app, event, ev, pending);
    release_texts(app, &texts, result == PURE_OK);
    if (result == PURE_OK) {
        memcpy(app->values, pending, sizeof(pending));
        ++app->events;
    }
    return result;
}

PureResult pure_app_frame(PureApp *app) {
    pure_app_finish(app);
    Event ev = {.app = app, .arena = &app->arena, .dt = 1.0 / 60.0};
    PureResult result = dispatch(app, 0, &ev);
    if (result == PURE_OK) ++app->ticks;
    return result;
}

PureResult pure_app_input(PureApp *app, uint32_t input, PureEdge edge) {
    if (input >= app->module->inputs || app->module->input[input].kind != PURE_KIND_KEY ||
        (edge != PURE_EDGE_DOWN && edge != PURE_EDGE_UP)) return PURE_INVALID;
    pure_app_finish(app);
    Event ev = {.app = app, .arena = &app->arena};
    return dispatch(app, 1 + 2 * input + (edge == PURE_EDGE_UP), &ev);
}

PureResult pure_app_text(PureApp *app, uint32_t input, PureSpan text) {
    pure_app_finish(app);
    if (input >= app->module->inputs || app->module->input[input].kind != PURE_KIND_TEXT ||
        (text.size && !text.data) || !pure_utf8_valid(text)) return PURE_INVALID;
    app->payload = text;
    Event ev = {.app = app, .arena = &app->arena};
    PureResult result = dispatch(app, 1 + 2 * input, &ev);
    app->payload = (PureSpan){0};
    return result;
}

static PureResult keep_reply(PureApp *app) {
    size_t length = (size_t)app->reply.length, start = app->arena.used;
    unsigned char *copy = malloc(length ? length : 1);
    if (!copy) return PURE_ALLOCATION;
    PureResult result = append_value(app, &app->arena, app->reply);
    if (result != PURE_OK) { free(copy); return result; }
    if (length) memcpy(copy, app->arena.bytes + start, length);
    app->arena.used = start;
    app->copied = copy;
    app->reply = (PureBytes){FROM_COPY, 0, {0, 0}, 0, length};
    return PURE_OK;
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
    Event ev = {.app = app, .shape = shape, .arena = &app->arena, .exists = app->revision > 0};
    double pending[PURE_MODULE_STATES];
    Texts texts;
    hold_texts(app, &texts);
    PureResult result = run_handlers(app, 1 + 2 * input, &ev, pending);
    if (result == PURE_OK && ev.modified && base < 0) result = PURE_INVALID;
    /* Commit rebases the working version and releasing texts may reclaim
     * earlier text versions: keep such a reply by copying it first. */
    if (result == PURE_OK && app->has_reply &&
        ((app->reply.source == FROM_DOCUMENT && ev.modified) || app->reply.source == FROM_TEXT))
        result = keep_reply(app);
    if (result == PURE_OK && ev.modified)
        result = app->data.commit(app->data.context, app->document, &app->revision);
    /* States and texts change only with a successful commit (or no document edit). */
    release_texts(app, &texts, result == PURE_OK);
    if (result == PURE_OK) {
        memcpy(app->values, pending, sizeof(pending));
        ++app->events;
    } else if (result != PURE_UNCERTAIN) pure_app_finish(app);
    return result;
}

PureResult pure_app_reply(const PureApp *app, bool (*visit)(void *, PureSpan), void *context) {
    if (!app->has_reply) return PURE_OK;
    return visit_value(app, &app->arena, app->reply, visit, context);
}

static PureResult evaluate(PureApp *app, uint32_t view, Slot *out) {
    double values[PURE_MODULE_STATES];
    memcpy(values, app->values, sizeof(values));
    app->view_arena.used = 0;
    Event ev = {.app = app, .arena = &app->view_arena};
    return run(app->module, values, app->module->view_code[view], &ev, out);
}

bool pure_app_view(PureApp *app, uint32_t view, double *out) {
    if (view >= app->module->views || app->module->view_type[view] != PURE_VIEW_NUM) return false;
    Slot slot;
    if (evaluate(app, view, &slot) != PURE_OK) return false;
    *out = slot.num;
    return true;
}

PureResult pure_app_view_text(PureApp *app, uint32_t view, bool (*visit)(void *, PureSpan), void *context) {
    if (view >= app->module->views || app->module->view_type[view] != PURE_VIEW_TEXT) return PURE_INVALID;
    Slot slot;
    PureResult result = evaluate(app, view, &slot);
    return result == PURE_OK ? visit_value(app, &app->view_arena, slot.bytes, visit, context) : result;
}

PureResult pure_app_replace(PureApp *app, const PureModule *module) {
    pure_app_finish(app);
    const PureModule *old = app->module;
    double values[PURE_MODULE_STATES] = {0};
    PureDraft *texts[PURE_MODULE_STATES] = {0};
    int kept[PURE_MODULE_STATES];
    PureResult result = PURE_OK;
    for (uint32_t i = 0; i < module->states; ++i) {
        int previous = pure_module_state(old, module->state_names[i]);
        kept[i] = previous >= 0 && old->state_type[previous] == module->state_type[i] ? previous : -1;
        if (module->state_type[i] == PURE_STATE_TEXT) {
            if (kept[i] >= 0) texts[i] = app->texts[kept[i]];
            else if (result == PURE_OK) result = new_text(&texts[i], module, module->state_ref[i]);
        } else values[i] = kept[i] >= 0 ? app->values[kept[i]] : module->initial[i];
    }
    /* A cursor keeps its position only while its text is the same kept text. */
    for (uint32_t i = 0; i < module->states; ++i) {
        if (module->state_type[i] != PURE_STATE_CURSOR) continue;
        uint32_t text = module->state_ref[i];
        bool same = kept[i] >= 0 && kept[text] >= 0 && old->state_ref[kept[i]] == (unsigned)kept[text];
        values[i] = same ? app->values[kept[i]] : 0;
    }
    if (result != PURE_OK) {
        for (uint32_t i = 0; i < module->states; ++i)
            if (texts[i] && kept[i] < 0) pure_draft_close(texts[i]);
        return result;
    }
    for (uint32_t i = 0; i < old->states; ++i) {
        bool reused = false;
        for (uint32_t k = 0; k < module->states; ++k) reused = reused || (texts[k] && texts[k] == app->texts[i]);
        if (app->texts[i] && !reused) pure_draft_close(app->texts[i]);
    }
    memcpy(app->values, values, sizeof(values));
    memcpy(app->texts, texts, sizeof(texts));
    app->module = module;
    return PURE_OK;
}
