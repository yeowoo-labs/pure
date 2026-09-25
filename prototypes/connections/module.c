#include "module.h"
#include "module-save.h"
#include "module-text.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { STATE_BYTES = 16, COLLECTION_BYTES = 12, INPUT_BYTES = 16, FIELD_BYTES = 8,
       CONSTANT_BYTES = 8, RECORD_BYTES = 8, RECORD_FIELD_BYTES = 16, VIEW_BYTES = 16,
       HANDLER_BYTES = 12, OP_BYTES = 16 };
/* T_FIELD and T_ITEM stand for the type of the message or record field named. */
enum { T_NUM = 1, T_BYTES = 2, T_TEXT = 3, T_CURSOR = 4, T_FIELD = 5, T_INDEX = 6, T_ITEM = 7 };
enum { FROM_PAYLOAD, FROM_DOCUMENT, FROM_COPY, FROM_CONSTANT, FROM_TEXT, FROM_ARENA };
/* Where an opcode may appear. */
enum { IN_VIEW = 1, IN_HANDLER = 2, IN_MESSAGE = 4, IN_KEYED = 8, IN_TEXT = 16, IN_ROW = 32 };
enum { READ = IN_VIEW | IN_HANDLER };
static const unsigned char magic[8] = {'P','U','R','E','M','O','D','5'};

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
    [PURE_OP_JZ] = {1, {T_NUM}, 0, READ},
    [PURE_OP_JMP] = {0, {0}, 0, READ},
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
    /* Record fields pop as many values as the list's record has fields. */
    [PURE_OP_LIST_COUNT] = {0, {0}, T_NUM, READ},
    [PURE_OP_LIST_APPEND] = {0, {0}, 0, IN_HANDLER},
    [PURE_OP_LIST_INSERT] = {1, {T_INDEX}, 0, IN_HANDLER},
    [PURE_OP_LIST_REMOVE] = {1, {T_INDEX}, 0, IN_HANDLER},
    [PURE_OP_ITEM_GET] = {1, {T_INDEX}, T_ITEM, READ},
    [PURE_OP_ITEM_SET] = {1, {T_INDEX}, 0, IN_HANDLER},
    [PURE_OP_INDEX_LOAD] = {0, {0}, T_INDEX, READ},
    [PURE_OP_INDEX_STORE] = {1, {T_INDEX}, 0, IN_HANDLER},
    [PURE_OP_INDEX_MOVE] = {2, {T_INDEX, T_NUM}, T_INDEX, READ},
    [PURE_OP_INDEX_FIRST] = {0, {0}, T_INDEX, READ},
    [PURE_OP_INDEX_LAST] = {0, {0}, T_INDEX, READ},
    [PURE_OP_INDEX_NUM] = {1, {T_INDEX}, T_NUM, READ},
    [PURE_OP_ROW] = {0, {0}, T_INDEX, IN_ROW},
    /* Stack copies: their types come from the values they move. */
    [PURE_OP_PICK] = {0, {0}, 0, READ},
    [PURE_OP_SLIDE] = {0, {0}, 0, READ},
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
    case PURE_OP_TEXT_LINE: case PURE_OP_TEXT_SET: case PURE_OP_CUR_COUNT: case PURE_OP_LIST_COUNT:
    case PURE_OP_LIST_APPEND: case PURE_OP_LIST_INSERT: case PURE_OP_LIST_REMOVE: case PURE_OP_ITEM_GET:
    case PURE_OP_ITEM_SET: case PURE_OP_INDEX_LOAD: case PURE_OP_INDEX_STORE: case PURE_OP_INDEX_FIRST:
    case PURE_OP_INDEX_LAST: case PURE_OP_ROW: case PURE_OP_PICK: case PURE_OP_SLIDE:
        return true;
    default: return false;
    }
}

/* The state an argument names must have this type. */
static bool state_of(const PureModule *m, uint32_t state, uint8_t type) {
    return state < m->states && m->state_type[state] == type;
}

/* The list an item operation names: its low 16 bits (the field is above). */
static uint32_t list_of(uint32_t arg) { return arg & 0xFFFF; }
static uint32_t item_field(uint32_t arg) { return arg >> 16; }

static unsigned char field_type(const PureModule *m, uint32_t list, uint32_t field) {
    return m->record_field_text[m->record[m->state_ref[list]].first + field] ? T_TEXT : T_NUM;
}

/* The stack a branch brings to its target, while the target is ahead. */
typedef struct { uint32_t target; int height; unsigned char types[PURE_MODULE_STACK], owners[PURE_MODULE_STACK]; } Join;

static bool same_stack(const Join *join, int height, const unsigned char *types, const unsigned char *owners) {
    return join->height == height && !memcmp(join->types, types, (size_t)height) &&
           !memcmp(join->owners, owners, (size_t)height);
}

/* Types and heights are checked per instruction; a cursor's type records
 * which text it belongs to and an index's type which list. Branches go
 * forward within the block. Every path into a join brings the same stack
 * (types and owners), so a value may cross a join, as in a conditional
 * expression; at most 8 joins with a non-empty stack are ahead at once.
 * Handlers end empty, views with one value of their type. A column (row
 * is its list) may read the row it is evaluated for. */
static bool verify_block(const PureModule *m, PureBlock block, int view, int row, uint32_t event,
                         char *error, size_t size, unsigned index) {
    if (block.start > m->code_count || block.length > m->code_count - block.start)
        return reject(error, size, "code range outside module", index);
    if (view >= 0 && !block.length) return reject(error, size, "empty view", index);
    int input = view >= 0 ? -1 : event_input(event);
    const PureInputShape *shape = input >= 0 ? &m->input[input] : NULL;
    unsigned where = view >= 0 ? IN_VIEW | (row >= 0 ? IN_ROW : 0) : IN_HANDLER;
    if (shape && shape->kind == PURE_KIND_MESSAGE) where |= IN_MESSAGE | (shape->keyed ? IN_KEYED : 0);
    if (shape && shape->kind == PURE_KIND_TEXT) where |= IN_TEXT;
    unsigned char types[PURE_MODULE_STACK], owners[PURE_MODULE_STACK], target[PURE_MODULE_CODE + 1] = {0};
    Join joins[PURE_MODULE_JOINS];
    unsigned open = 0;
    int height = 0;
    bool reachable = true;
    for (uint32_t i = 0; i <= block.length; ++i) {
        uint32_t pc = block.start + i;
        /* Branches here brought an empty stack (target) or the stack of a join. */
        unsigned join = open;
        for (unsigned k = 0; k < open; ++k) if (joins[k].target == pc) join = k;
        if (join < open) {
            if (target[i] || (reachable && !same_stack(&joins[join], height, types, owners)))
                return reject(error, size, "join with different stacks", pc);
            height = joins[join].height;
            memcpy(types, joins[join].types, (size_t)height);
            memcpy(owners, joins[join].owners, (size_t)height);
            joins[join] = joins[--open];
            reachable = true;
        } else if (target[i]) {
            if (reachable && height) return reject(error, size, "join with different stacks", pc);
            height = 0;
            reachable = true;
        }
        if (i == block.length) break;
        if (!reachable) return reject(error, size, "unreachable instruction", pc);
        const PureOp *op = &m->code[pc];
        if (op->op >= PURE_OP_COUNT) return reject(error, size, "unknown opcode", pc);
        unsigned allowed = signature[op->op].where;
        if (!(allowed & where)) {
            if (allowed & IN_ROW) return reject(error, size, "row outside a column", pc);
            if (view >= 0) return reject(error, size, "view with an effect", pc);
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
        case PURE_OP_LIST_COUNT: case PURE_OP_LIST_APPEND: case PURE_OP_LIST_INSERT: case PURE_OP_LIST_REMOVE:
        case PURE_OP_INDEX_FIRST: case PURE_OP_INDEX_LAST:
            known = state_of(m, arg, PURE_STATE_LIST);
            break;
        case PURE_OP_ITEM_GET: case PURE_OP_ITEM_SET:
            known = state_of(m, list_of(arg), PURE_STATE_LIST) &&
                    item_field(arg) < m->record[m->state_ref[list_of(arg)]].count;
            break;
        case PURE_OP_INDEX_LOAD: case PURE_OP_INDEX_STORE: known = state_of(m, arg, PURE_STATE_INDEX); break;
        case PURE_OP_ROW: known = (int)arg == row; break;
        default: break;
        }
        if (!known) return reject(error, size, "operand names no such state, field, constant or unit", pc);
        /* PICK copies the value arg below the top; SLIDE drops arg values under the top. */
        if (op->op == PURE_OP_PICK || op->op == PURE_OP_SLIDE) {
            bool pick = op->op == PURE_OP_PICK;
            if (pick ? arg >= (uint32_t)height : !arg || arg >= (uint32_t)height)
                return reject(error, size, "stack underflow", pc);
            if (pick && height == PURE_MODULE_STACK) return reject(error, size, "stack overflow", pc);
            int from = height - 1 - (int)arg, to = pick ? height++ : from;
            types[to] = pick ? types[from] : types[height - 1];
            owners[to] = pick ? owners[from] : owners[height - 1];
            if (!pick) height = from + 1;
            continue;
        }
        /* Operand types, bottom to top; record operations take their fields. */
        unsigned char need[PURE_RECORD_FIELDS + 2];
        unsigned pops = signature[op->op].pops;
        memcpy(need, signature[op->op].in, sizeof(signature[op->op].in));
        if (op->op == PURE_OP_LIST_APPEND || op->op == PURE_OP_LIST_INSERT) {
            const PureRecord *record = &m->record[m->state_ref[arg]];
            for (unsigned f = 0; f < record->count; ++f) need[pops + f] = field_type(m, arg, f);
            pops += record->count;
        } else if (op->op == PURE_OP_ITEM_SET) {
            need[pops++] = field_type(m, list_of(arg), item_field(arg));
        }
        if (height < (int)pops) return reject(error, size, "stack underflow", pc);
        for (unsigned k = 0; k < pops; ++k)
            if (types[height - (int)pops + (int)k] != need[k])
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
        /* The list an index operand belongs to; the index is the first operand. */
        unsigned index_owner = need[0] == T_INDEX ? owners[height - (int)pops] : 0;
        bool same_list = true;
        switch (op->op) {
        case PURE_OP_LIST_INSERT: case PURE_OP_LIST_REMOVE: same_list = index_owner == arg; break;
        case PURE_OP_ITEM_GET: case PURE_OP_ITEM_SET: same_list = index_owner == list_of(arg); break;
        case PURE_OP_INDEX_STORE: same_list = index_owner == m->state_ref[arg]; break;
        default: break;
        }
        if (!same_list) return reject(error, size, "index of another list", pc);
        height -= (int)pops;
        unsigned char out = signature[op->op].out;
        if (out == T_FIELD) out = m->field[shape->field_start + arg].type == PURE_FIELD_U32 ? T_NUM : T_BYTES;
        if (out == T_ITEM) out = field_type(m, list_of(arg), item_field(arg));
        if (out) {
            if (height == PURE_MODULE_STACK) return reject(error, size, "stack overflow", pc);
            owners[height] = (unsigned char)(op->op == PURE_OP_CUR_LOAD || op->op == PURE_OP_INDEX_LOAD ? m->state_ref[arg] :
                              op->op == PURE_OP_CUR_START || op->op == PURE_OP_CUR_END ||
                              op->op == PURE_OP_INDEX_FIRST || op->op == PURE_OP_INDEX_LAST || op->op == PURE_OP_ROW ? arg :
                              op->op == PURE_OP_INDEX_MOVE ? index_owner :
                              owner >= 0 ? (unsigned)owner : 0);
            types[height++] = out;
        }
        if (op->op == PURE_OP_JZ || op->op == PURE_OP_JMP) {
            if (arg <= pc || arg > block.start + block.length)
                return reject(error, size, "branch not forward within its block", pc);
            unsigned join = open;
            for (unsigned k = 0; k < open; ++k) if (joins[k].target == arg) join = k;
            if (!height) {
                if (join < open) return reject(error, size, "join with different stacks", pc);
                target[arg - block.start] = 1;
            } else if (join < open) {
                if (!same_stack(&joins[join], height, types, owners)) return reject(error, size, "join with different stacks", pc);
            } else {
                if (target[arg - block.start]) return reject(error, size, "join with different stacks", pc);
                if (open == PURE_MODULE_JOINS) return reject(error, size, "more than 8 joins ahead", pc);
                joins[open] = (Join){.target = arg, .height = height};
                memcpy(joins[open].types, types, (size_t)height);
                memcpy(joins[open++].owners, owners, (size_t)height);
            }
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
        return reject(error, error_size, "not a Pure module v4", 0), PURE_INVALID;
    out->states = (uint16_t)read16(p + 8);
    out->inputs = (uint16_t)read16(p + 10);
    out->views = (uint16_t)read16(p + 12);
    out->handlers = (uint16_t)read16(p + 14);
    out->code_count = read32(p + 16);
    uint32_t strings = read32(p + 20);
    out->fields = (uint16_t)read16(p + 24);
    out->collections = (uint16_t)read16(p + 26);
    out->constants = (uint16_t)read16(p + 28);
    out->records = (uint16_t)read16(p + 30);
    uint32_t constant_bytes = read32(p + 32);
    out->record_fields = (uint16_t)read16(p + 36);
    if (read16(p + 38)) return reject(error, error_size, "reserved header bytes", 38), PURE_INVALID;
    if (out->states > PURE_MODULE_STATES || out->inputs > PURE_MODULE_INPUTS ||
        out->views > PURE_MODULE_VIEWS || out->handlers > PURE_MODULE_HANDLERS ||
        out->code_count > PURE_MODULE_CODE || strings > PURE_MODULE_STRINGS ||
        out->fields > PURE_MODULE_FIELDS || out->collections > PURE_MODULE_COLLECTIONS ||
        out->constants > PURE_MODULE_CONSTANTS || constant_bytes > PURE_MODULE_CONSTANT_BYTES ||
        out->records > PURE_MODULE_RECORDS || out->record_fields > PURE_MODULE_RECORD_FIELDS)
        return reject(error, error_size, "module exceeds a limit", 8), PURE_LIMIT;
    size_t expected = PURE_MODULE_HEADER + (size_t)out->states * STATE_BYTES +
        (size_t)out->collections * COLLECTION_BYTES + (size_t)out->inputs * INPUT_BYTES +
        (size_t)out->fields * FIELD_BYTES + (size_t)out->constants * CONSTANT_BYTES +
        (size_t)out->records * RECORD_BYTES + (size_t)out->record_fields * RECORD_FIELD_BYTES +
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
        if (read32(at + 4) > PURE_STATE_INDEX) return reject(error, error_size, "state type", i), PURE_INVALID;
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
    /* Records: fields in order, numbered from first; a text default is a constant. */
    const unsigned char *fields_at = at + (size_t)out->records * RECORD_BYTES;
    uint32_t next_record_field = 0;
    for (uint32_t i = 0; i < out->records; ++i, at += RECORD_BYTES) {
        PureRecord *record = &out->record[i];
        record->first = (uint16_t)read16(at + 4);
        record->count = (uint16_t)read16(at + 6);
        if (!name_at(text, strings, read32(at), true, out->record_names[i]) || record->first != next_record_field ||
            !record->count || record->count > PURE_RECORD_FIELDS || record->count > out->record_fields - next_record_field)
            return reject(error, error_size, "record", i), PURE_INVALID;
        for (uint32_t f = record->first; f < record->first + record->count; ++f) {
            const unsigned char *entry = fields_at + (size_t)f * RECORD_FIELD_BYTES;
            uint32_t type = read32(entry + 4);
            out->record_field_text[f] = type == 1;
            if (!name_at(text, strings, read32(entry), true, out->record_field_names[f]) || type > 1)
                return reject(error, error_size, "record field", f), PURE_INVALID;
            for (uint32_t k = record->first; k < f; ++k)
                if (!strcmp(out->record_field_names[k], out->record_field_names[f]))
                    return reject(error, error_size, "duplicate record field", f), PURE_INVALID;
            if (type == 1) {
                if (read32(entry + 12) || read32(entry + 8) >= out->constants)
                    return reject(error, error_size, "record field default", f), PURE_INVALID;
                out->record_field_default[f] = read32(entry + 8);
            } else {
                out->record_field_default[f] = read_double(entry + 8);
                if (!isfinite(out->record_field_default[f]))
                    return reject(error, error_size, "record field default", f), PURE_INVALID;
            }
        }
        next_record_field += record->count;
    }
    if (next_record_field != out->record_fields)
        return reject(error, error_size, "unused record fields", next_record_field), PURE_INVALID;
    at = fields_at + (size_t)out->record_fields * RECORD_FIELD_BYTES;
    for (uint32_t i = 0; i < out->states; ++i)
        if ((out->state_type[i] == PURE_STATE_TEXT && out->state_ref[i] >= out->constants) ||
            (out->state_type[i] == PURE_STATE_CURSOR && !state_of(out, out->state_ref[i], PURE_STATE_TEXT)) ||
            (out->state_type[i] == PURE_STATE_LIST && out->state_ref[i] >= out->records) ||
            (out->state_type[i] == PURE_STATE_INDEX && !state_of(out, out->state_ref[i], PURE_STATE_LIST)))
            return reject(error, error_size, "state reference", i), PURE_INVALID;
    /* A table names its list in place of code; its columns follow it. */
    for (uint32_t i = 0; i < out->views; ++i, at += VIEW_BYTES) {
        if (!name_at(text, strings, read32(at), true, out->view_names[i]))
            return reject(error, error_size, "view name", i), PURE_INVALID;
        out->view_type[i] = at[4];
        out->view_table[i] = (uint16_t)read16(at + 6);
        out->view_code[i] = (PureBlock){read32(at + 8), read32(at + 12)};
        uint32_t table = out->view_table[i];
        if (at[4] > PURE_VIEW_TABLE || at[5] || (table && (table > i || out->view_type[table - 1] != PURE_VIEW_TABLE ||
                                                           at[4] == PURE_VIEW_TABLE)))
            return reject(error, error_size, "view type", i), PURE_INVALID;
        if (at[4] == PURE_VIEW_TABLE) {
            if (!state_of(out, out->view_code[i].start, PURE_STATE_LIST) || out->view_code[i].length)
                return reject(error, error_size, "table list", i), PURE_INVALID;
            out->view_list[i] = (uint16_t)out->view_code[i].start;
        } else if (table) out->view_list[i] = out->view_list[table - 1];
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
    /* States, records, collections, inputs and views share one namespace. */
    const char *names[PURE_MODULE_STATES + PURE_MODULE_RECORDS + PURE_MODULE_COLLECTIONS +
                      PURE_MODULE_INPUTS + PURE_MODULE_VIEWS];
    size_t count = 0;
    for (uint32_t i = 0; i < out->states; ++i) names[count++] = out->state_names[i];
    for (uint32_t i = 0; i < out->records; ++i) names[count++] = out->record_names[i];
    for (uint32_t i = 0; i < out->collections; ++i) names[count++] = out->collection_names[i];
    for (uint32_t i = 0; i < out->inputs; ++i) names[count++] = out->input_names[i];
    for (uint32_t i = 0; i < out->views; ++i) if (!out->view_table[i]) names[count++] = out->view_names[i];
    for (size_t i = 0; i < count; ++i)
        for (size_t k = 0; k < i; ++k)
            if (!strcmp(names[i], names[k])) return reject(error, error_size, "duplicate name", (unsigned)i), PURE_INVALID;
    /* Column names are unique within their table. */
    for (uint32_t i = 0; i < out->views; ++i)
        for (uint32_t k = 0; k < i; ++k)
            if (out->view_table[i] && out->view_table[k] == out->view_table[i] &&
                !strcmp(out->view_names[i], out->view_names[k]))
                return reject(error, error_size, "duplicate column", i), PURE_INVALID;
    for (uint32_t i = 0; i < out->views; ++i)
        if (out->view_type[i] != PURE_VIEW_TABLE &&
            !verify_block(out, out->view_code[i], out->view_type[i], out->view_table[i] ? out->view_list[i] : -1, 0,
                          error, error_size, i)) return PURE_INVALID;
    for (uint32_t i = 0; i < out->handlers; ++i)
        if (!verify_block(out, out->handler[i].code, -1, -1, out->handler[i].event, error, error_size, i))
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
int pure_module_view(const PureModule *m, const char *name) {
    for (uint32_t i = 0; i < m->views; ++i)
        if (!m->view_table[i] && !strcmp(m->view_names[i], name)) return (int)i;
    return -1;
}

int pure_module_column(const PureModule *m, uint32_t table, const char *name) {
    for (uint32_t i = 0; i < m->views; ++i)
        if (m->view_table[i] == table + 1 && !strcmp(m->view_names[i], name)) return (int)i;
    return -1;
}

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
    size_t row; /* the row a column is evaluated for */
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

/* Indices follow their item across an insert or remove; an index whose item
 * was removed names the next item (or the last, or 0 in an empty list). */
static void move_indices(const PureModule *m, double *values, uint32_t list, size_t count,
                         size_t at, bool inserted) {
    double last = count ? (double)(count - 1) : 0;
    for (uint32_t i = 0; i < m->states; ++i) {
        if (m->state_type[i] != PURE_STATE_INDEX || m->state_ref[i] != list) continue;
        if (inserted) {
            if (count > 1 && values[i] >= (double)at) ++values[i];
        } else {
            if (values[i] > (double)at) --values[i];
            values[i] = minimum(values[i], last);
        }
    }
}

static bool text_field(const PureModule *m, uint32_t list, uint32_t field) {
    return m->record_field_text[m->record[m->state_ref[list]].first + field];
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
        case PURE_OP_LIST_COUNT: stack[sp++].num = (double)pure_list_count(app->lists[op->arg]); ++pc; continue;
        case PURE_OP_LIST_APPEND: case PURE_OP_LIST_INSERT: {
            PureList *list = app->lists[op->arg];
            const PureRecord *record = &m->record[m->state_ref[op->arg]];
            size_t base = sp - record->count, count = pure_list_count(list), at = count;
            sp = base;
            if (op->op == PURE_OP_LIST_INSERT && (at = (size_t)stack[--sp].num) > count)
                return fault(app, PURE_FAULT_NOT_FOUND, pc);
            /* Room for every text first, so no span moves while the next is copied. */
            size_t mark = ev->arena->used, total = 0;
            for (unsigned f = 0; f < record->count; ++f)
                if (text_field(m, op->arg, f)) total += (size_t)stack[base + f].bytes.length;
            if (!reserve(ev->arena, total)) return PURE_ALLOCATION;
            PureCell cells[PURE_RECORD_FIELDS];
            for (unsigned f = 0; f < record->count; ++f) {
                PureSpan span = {0};
                if (text_field(m, op->arg, f) &&
                    (status = contiguous(app, ev->arena, stack[base + f].bytes, &span)) != PURE_OK) return status;
                cells[f] = (PureCell){stack[base + f].num, (unsigned char *)span.data, span.size};
            }
            status = pure_list_insert(list, at, cells, &app->undo);
            ev->arena->used = mark;
            if (status == PURE_LIMIT) return fault(app, PURE_FAULT_LIMIT, pc);
            if (status != PURE_OK) return status;
            move_indices(m, values, op->arg, pure_list_count(list), at, true);
            ++pc;
            continue;
        }
        case PURE_OP_LIST_REMOVE: {
            size_t at = (size_t)stack[--sp].num;
            status = pure_list_remove(app->lists[op->arg], at, &app->undo);
            if (status == PURE_NOT_FOUND) return fault(app, PURE_FAULT_NOT_FOUND, pc);
            if (status != PURE_OK) return status;
            move_indices(m, values, op->arg, pure_list_count(app->lists[op->arg]), at, false);
            ++pc;
            continue;
        }
        case PURE_OP_ITEM_GET: {
            uint32_t list = list_of(op->arg), field = item_field(op->arg);
            size_t at = (size_t)stack[sp - 1].num;
            if (at >= pure_list_count(app->lists[list])) return fault(app, PURE_FAULT_NOT_FOUND, pc);
            PureCell cell = pure_list_get(app->lists[list], at, field);
            if (!text_field(m, list, field)) stack[sp - 1].num = cell.num;
            else {
                /* A copy: the item may change or go before the value is used. */
                if (!reserve(ev->arena, cell.size)) return PURE_ALLOCATION;
                if (cell.size) memcpy(ev->arena->bytes + ev->arena->used, cell.text, cell.size);
                stack[sp - 1].bytes = (PureBytes){FROM_ARENA, 0, {0, 0}, ev->arena->used, cell.size};
                ev->arena->used += cell.size;
            }
            ++pc;
            continue;
        }
        case PURE_OP_ITEM_SET: {
            uint32_t list = list_of(op->arg), field = item_field(op->arg);
            Slot value = stack[--sp];
            size_t at = (size_t)stack[--sp].num, mark = ev->arena->used;
            PureSpan span = {0};
            if (text_field(m, list, field) && (status = contiguous(app, ev->arena, value.bytes, &span)) != PURE_OK)
                return status;
            status = pure_list_set(app->lists[list], at, field,
                                   (PureCell){value.num, (unsigned char *)span.data, span.size}, &app->undo);
            ev->arena->used = mark;
            if (status == PURE_NOT_FOUND) return fault(app, PURE_FAULT_NOT_FOUND, pc);
            if (status != PURE_OK) return status;
            ++pc;
            continue;
        }
        case PURE_OP_INDEX_LOAD:
            stack[sp].num = values[op->arg];
            stack[sp++].bytes.owner = m->state_ref[op->arg];
            ++pc;
            continue;
        case PURE_OP_INDEX_STORE: {
            /* An index from before a remove may be past the end. */
            double position = stack[--sp].num;
            if (position && position >= (double)pure_list_count(app->lists[m->state_ref[op->arg]]))
                return fault(app, PURE_FAULT_NOT_FOUND, pc);
            values[op->arg] = position;
            ++pc;
            continue;
        }
        case PURE_OP_INDEX_MOVE: {
            double count = stack[--sp].num;
            if (count != floor(count)) return fault(app, PURE_FAULT_LIMIT, pc);
            size_t items = pure_list_count(app->lists[stack[sp - 1].bytes.owner]);
            double last = items ? (double)(items - 1) : 0;
            stack[sp - 1].num = maximum(0, minimum(stack[sp - 1].num + count, last));
            ++pc;
            continue;
        }
        case PURE_OP_INDEX_FIRST: case PURE_OP_INDEX_LAST: {
            size_t items = pure_list_count(app->lists[op->arg]);
            stack[sp].num = op->op == PURE_OP_INDEX_LAST && items ? (double)(items - 1) : 0;
            stack[sp++].bytes.owner = (uint16_t)op->arg;
            ++pc;
            continue;
        }
        case PURE_OP_INDEX_NUM: ++pc; continue; /* the position itself */
        case PURE_OP_ROW:
            stack[sp].num = (double)ev->row;
            stack[sp++].bytes.owner = (uint16_t)op->arg;
            ++pc;
            continue;
        case PURE_OP_PICK: stack[sp] = stack[sp - 1 - op->arg]; ++sp; ++pc; continue;
        case PURE_OP_SLIDE: stack[sp - 1 - op->arg] = stack[sp - 1]; sp -= op->arg; ++pc; continue;
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

static PureList *new_list(const PureModule *m, uint32_t record) {
    return pure_list_new(m->record[record].count, m->record_field_text + m->record[record].first);
}

PureResult pure_app_start(PureApp *app, const PureModule *module, PureData data) {
    *app = (PureApp){.module = module, .data = data, .fault_pc = UINT32_MAX};
    memcpy(app->values, module->initial, sizeof(double) * module->states);
    bool kept = pure_save_enabled(app);
    PureResult result = PURE_OK;
    for (uint32_t i = 0; i < module->states && result == PURE_OK; ++i) {
        if (module->state_type[i] == PURE_STATE_TEXT)
            result = kept ? pure_save_open_text(app, module, i, &app->texts[i], &app->saved[i]) :
                     new_text(&app->texts[i], module, module->state_ref[i]);
        if (module->state_type[i] == PURE_STATE_LIST) {
            if (kept) result = pure_save_open_list(app, module, i, &app->lists[i], &app->list_drafts[i], &app->saved[i]);
            else if (!(app->lists[i] = new_list(module, module->state_ref[i]))) result = PURE_ALLOCATION;
        }
    }
    /* Stored numbers, cursors and indices apply once their texts and lists are in. */
    if (result == PURE_OK && kept) result = pure_save_open_values(app, module, app->values, true);
    if (result == PURE_OK) result = pure_app_save(app);
    if (result != PURE_OK) pure_app_stop(app);
    return result;
}

static bool pending(const PureApp *app, const void *document) {
    for (unsigned i = 0; i < app->pending_count; ++i) if (app->pending[i].document == document) return true;
    return false;
}

void pure_app_finish(PureApp *app) {
    /* A document in the unsaved burst stays open until the save. */
    if (app->document && !pending(app, app->document)) app->data.close(app->data.context, app->document);
    free(app->copied);
    app->document = NULL;
    app->copied = NULL;
    app->has_reply = false;
    app->payload = (PureSpan){0};
    app->arena.used = 0;
}

/* The start of a burst: what a failed save returns to. Text and stored list
 * drafts stay held until the save, so every version since stays readable. */
static void open_burst(PureApp *app) {
    if (app->open) return;
    const PureModule *m = app->module;
    memcpy(app->durable_values, app->values, sizeof(app->values));
    app->durable_ticks = app->ticks;
    app->durable_events = app->events;
    for (uint32_t i = 0; i < m->states; ++i) {
        if (app->texts[i]) {
            pure_draft_hold(app->texts[i], true);
            app->durable_texts[i] = pure_draft_version(app->texts[i]);
        }
        if (app->list_drafts[i]) {
            pure_draft_hold(app->list_drafts[i], true);
            app->durable_lists[i] = pure_draft_version(app->list_drafts[i]);
        }
    }
    app->open = true;
}

/* End a burst, keeping it (saved) or undoing all of it. The documents it held
 * close, except the current event's, which pure_app_finish closes. */
static void close_burst(PureApp *app, bool keep) {
    for (unsigned i = 0; i < app->pending_count; ++i)
        if (app->pending[i].document != app->document) app->data.close(app->data.context, app->pending[i].document);
    app->pending_count = 0;
    if (!app->open) return;
    const PureModule *m = app->module;
    for (uint32_t i = 0; i < m->states; ++i) {
        if (app->texts[i]) {
            if (!keep) pure_draft_restore(app->texts[i], app->durable_texts[i]);
            pure_draft_hold(app->texts[i], false);
        }
        if (app->list_drafts[i]) {
            if (!keep) pure_draft_restore(app->list_drafts[i], app->durable_lists[i]);
            pure_draft_hold(app->list_drafts[i], false);
        }
    }
    if (keep) pure_undo_commit(&app->undo);
    else {
        pure_undo_rollback(&app->undo, 0);
        memcpy(app->values, app->durable_values, sizeof(app->values));
        app->ticks = app->durable_ticks;
        app->events = app->durable_events;
    }
    app->open = false;
}

PureResult pure_app_save(PureApp *app) {
    void *documents[PURE_APP_PENDING];
    int64_t revisions[PURE_APP_PENDING];
    size_t count = 0;
    for (unsigned i = 0; i < app->pending_count; ++i)
        if (app->pending[i].edited) documents[count++] = app->pending[i].document;
    PureResult result = pure_save_commit(app, app->values, documents, count, revisions);
    close_burst(app, result == PURE_OK);
    return result;
}

bool pure_app_defer(PureApp *app, bool deferred) {
    if (deferred && !pure_save_enabled(app)) return false;
    app->deferred = deferred;
    return true;
}

void pure_app_stop(PureApp *app) {
    pure_app_finish(app);
    close_burst(app, true); /* unsaved changes are dropped with the app */
    for (uint32_t i = 0; i < PURE_MODULE_STATES; ++i) {
        pure_draft_close(app->texts[i]);
        pure_list_free(app->lists[i]);
        pure_draft_close(app->list_drafts[i]);
    }
    pure_draft_close(app->stored_values);
    pure_undo_free(&app->undo);
    free(app->arena.bytes);
    free(app->view_arena.bytes);
    memset(app, 0, sizeof(*app));
}

/* Where one event began inside the burst: a failed event returns there. */
typedef struct { PureVersion texts[PURE_MODULE_STATES], lists[PURE_MODULE_STATES]; size_t undo; bool opened; } Mark;

static void mark_event(PureApp *app, Mark *mark) {
    mark->opened = !app->open;
    open_burst(app);
    for (uint32_t i = 0; i < app->module->states; ++i) {
        if (app->texts[i]) mark->texts[i] = pure_draft_version(app->texts[i]);
        if (app->list_drafts[i]) mark->lists[i] = pure_draft_version(app->list_drafts[i]);
    }
    mark->undo = app->undo.count;
}

static void undo_event(PureApp *app, const Mark *mark) {
    for (uint32_t i = 0; i < app->module->states; ++i) {
        if (app->texts[i]) pure_draft_restore(app->texts[i], mark->texts[i]);
        if (app->list_drafts[i]) pure_draft_restore(app->list_drafts[i], mark->lists[i]);
    }
    pure_undo_rollback(&app->undo, mark->undo);
    if (mark->opened) close_burst(app, true); /* nothing happened: release the holds */
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

/* A key or text event saves before it returns unless saving is deferred; a
 * frame leaves that to the next event (frames come 60 times a second). */
static PureResult dispatch(PureApp *app, uint32_t event, Event *ev) {
    double pending[PURE_MODULE_STATES];
    Mark mark;
    mark_event(app, &mark);
    PureResult result = run_handlers(app, event, ev, pending);
    if (result != PURE_OK) { undo_event(app, &mark); return result; }
    memcpy(app->values, pending, sizeof(pending));
    ++app->events;
    if (!event) ++app->ticks;
    return event && !app->deferred ? pure_app_save(app) : PURE_OK;
}

PureResult pure_app_frame(PureApp *app) {
    pure_app_finish(app);
    Event ev = {.app = app, .arena = &app->arena, .dt = 1.0 / 60.0};
    return dispatch(app, 0, &ev);
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
    if (shape->keyed && m->collections) {
        if (!app->data.open) return PURE_INVALID;
        /* A key joins a burst once, so its revision moves once per save. */
        bool seen = app->pending_count == PURE_APP_PENDING;
        for (unsigned i = 0; i < app->pending_count && !seen; ++i) seen = !strcmp(app->pending[i].key, key);
        if (seen) {
            PureResult saved = pure_app_save(app);
            if (saved != PURE_OK) return saved;
        }
        PureResult opened = app->data.open(app->data.context, 0, key, &app->document, &app->revision);
        if (opened != PURE_OK) { app->document = NULL; return opened; }
        if (base >= 0 && base != app->revision) { pure_app_finish(app); return PURE_CONFLICT; }
    }
    app->payload = payload;
    Event ev = {.app = app, .shape = shape, .arena = &app->arena, .exists = app->revision > 0};
    double pending_values[PURE_MODULE_STATES];
    Mark mark;
    mark_event(app, &mark);
    PureResult result = run_handlers(app, 1 + 2 * input, &ev, pending_values);
    if (result == PURE_OK && ev.modified && base < 0) result = PURE_INVALID;
    /* A save rebases an edited document and releasing texts may reclaim
     * earlier text versions: keep such a reply by copying it first. */
    if (result == PURE_OK && app->has_reply &&
        ((app->reply.source == FROM_DOCUMENT && ev.modified) || app->reply.source == FROM_TEXT))
        result = keep_reply(app);
    if (result != PURE_OK) {
        undo_event(app, &mark);
        pure_app_finish(app);
        return result;
    }
    memcpy(app->values, pending_values, sizeof(pending_values));
    ++app->events;
    if (app->document) {
        app->pending[app->pending_count].document = app->document;
        app->pending[app->pending_count].edited = ev.modified;
        memcpy(app->pending[app->pending_count++].key, key, strlen(key) + 1);
        app->revision += ev.modified; /* a save moves an edited document one revision */
    }
    if (app->deferred) return PURE_OK;
    result = pure_app_save(app);
    if (result != PURE_OK) pure_app_finish(app);
    return result;
}

PureResult pure_app_reply(const PureApp *app, bool (*visit)(void *, PureSpan), void *context) {
    if (!app->has_reply) return PURE_OK;
    return visit_value(app, &app->arena, app->reply, visit, context);
}

/* A view that fails reports it by its result; the fault stays the last event's. */
static PureResult evaluate(PureApp *app, uint32_t view, size_t row, Slot *out) {
    double values[PURE_MODULE_STATES];
    memcpy(values, app->values, sizeof(values));
    app->view_arena.used = 0;
    Event ev = {.app = app, .arena = &app->view_arena, .row = row};
    PureFault fault = app->fault;
    uint32_t fault_pc = app->fault_pc;
    PureResult result = run(app->module, values, app->module->view_code[view], &ev, out);
    app->fault = fault;
    app->fault_pc = fault_pc;
    return result;
}

/* A view outside a table (column false), or a column and a row of its table. */
static PureResult view_of(PureApp *app, uint32_t view, bool column, size_t row, uint8_t type, Slot *slot) {
    const PureModule *m = app->module;
    if (view >= m->views || m->view_type[view] != type || (m->view_table[view] != 0) != column) return PURE_INVALID;
    if (column && row >= pure_list_count(app->lists[m->view_list[view]])) return PURE_NOT_FOUND;
    return evaluate(app, view, row, slot);
}

bool pure_app_view(PureApp *app, uint32_t view, double *out) {
    Slot slot;
    if (view_of(app, view, false, 0, PURE_VIEW_NUM, &slot) != PURE_OK) return false;
    *out = slot.num;
    return true;
}

PureResult pure_app_view_text(PureApp *app, uint32_t view, bool (*visit)(void *, PureSpan), void *context) {
    Slot slot;
    PureResult result = view_of(app, view, false, 0, PURE_VIEW_TEXT, &slot);
    return result == PURE_OK ? visit_value(app, &app->view_arena, slot.bytes, visit, context) : result;
}

size_t pure_app_rows(const PureApp *app, uint32_t table) {
    const PureModule *m = app->module;
    if (table >= m->views || m->view_type[table] != PURE_VIEW_TABLE) return 0;
    return pure_list_count(app->lists[m->view_list[table]]);
}

bool pure_app_cell(PureApp *app, uint32_t column, size_t row, double *out) {
    Slot slot;
    if (view_of(app, column, true, row, PURE_VIEW_NUM, &slot) != PURE_OK) return false;
    *out = slot.num;
    return true;
}

PureResult pure_app_cell_text(PureApp *app, uint32_t column, size_t row, bool (*visit)(void *, PureSpan), void *context) {
    Slot slot;
    PureResult result = view_of(app, column, true, row, PURE_VIEW_TEXT, &slot);
    return result == PURE_OK ? visit_value(app, &app->view_arena, slot.bytes, visit, context) : result;
}

/* A list with record's fields: each takes the field of `from` with the same
 * name and kind, or its default. */
PureResult pure_list_migrate(PureList **out, const PureList *from, const char *const *names,
                             const uint8_t *text, unsigned count, const PureModule *m, uint32_t record) {
    const PureRecord *r = &m->record[record];
    int source[PURE_RECORD_FIELDS];
    for (unsigned f = 0; f < r->count; ++f) {
        source[f] = -1;
        for (unsigned k = 0; k < count; ++k)
            if (!strcmp(m->record_field_names[r->first + f], names[k]) && m->record_field_text[r->first + f] == text[k])
                source[f] = (int)k;
    }
    if (!(*out = new_list(m, record))) return PURE_ALLOCATION;
    PureResult result = PURE_OK;
    for (size_t at = 0; result == PURE_OK && at < pure_list_count(from); ++at) {
        PureCell cells[PURE_RECORD_FIELDS];
        for (unsigned f = 0; f < r->count; ++f) {
            uint32_t field = r->first + f, constant = (uint32_t)m->record_field_default[field];
            cells[f] = source[f] >= 0 ? pure_list_get(from, at, (unsigned)source[f]) :
                       m->record_field_text[field] ?
                           (PureCell){0, (unsigned char *)m->constant_data + m->constant_offset[constant],
                                      m->constant_length[constant]} :
                           (PureCell){m->record_field_default[field], NULL, 0};
        }
        result = pure_list_insert(*out, at, cells, NULL);
    }
    if (result != PURE_OK) { pure_list_free(*out); *out = NULL; }
    return result;
}

/* A kept list keeps its items; with the same record it is the same list. */
static PureResult keep_list(PureList **out, PureList *from, const PureModule *old, uint32_t old_record,
                            const PureModule *m, uint32_t record) {
    const PureRecord *r = &m->record[record], *o = &old->record[old_record];
    const char *names[PURE_RECORD_FIELDS];
    bool same = r->count == o->count;
    for (unsigned k = 0; k < o->count; ++k) {
        names[k] = old->record_field_names[o->first + k];
        same = same && !strcmp(names[k], m->record_field_names[r->first + k]) &&
               old->record_field_text[o->first + k] == m->record_field_text[r->first + k];
    }
    if (same) { *out = from; return PURE_OK; }
    return pure_list_migrate(out, from, names, old->record_field_text + o->first, o->count, m, record);
}

/* States keep their values by name and kind. With stored states, a state new
 * to this app starts from its stored value, a list whose record changed is
 * stored again, and the next event or save publishes the new module's states;
 * a state the new module drops stays stored as it was. */
PureResult pure_app_replace(PureApp *app, const PureModule *module) {
    pure_app_finish(app);
    if (app->open || app->pending_count) {
        PureResult saved = pure_app_save(app); /* the burst belongs to the module that made it */
        if (saved != PURE_OK) return saved;
    }
    const PureModule *old = app->module;
    bool stored = pure_save_enabled(app);
    double values[PURE_MODULE_STATES] = {0};
    PureDraft *texts[PURE_MODULE_STATES] = {0}, *list_drafts[PURE_MODULE_STATES] = {0};
    PureList *lists[PURE_MODULE_STATES] = {0};
    PureVersion saved[PURE_MODULE_STATES] = {{0, 0}}, before[PURE_MODULE_STATES] = {{0, 0}};
    int kept[PURE_MODULE_STATES];
    bool rewrite[PURE_MODULE_STATES] = {false};
    PureResult result = PURE_OK;
    for (uint32_t i = 0; i < module->states; ++i) {
        int previous = pure_module_state(old, module->state_names[i]);
        kept[i] = previous >= 0 && old->state_type[previous] == module->state_type[i] ? previous : -1;
        if (kept[i] >= 0) saved[i] = app->saved[kept[i]];
        if (module->state_type[i] == PURE_STATE_TEXT) {
            if (kept[i] >= 0) texts[i] = app->texts[kept[i]];
            else if (result == PURE_OK)
                result = stored ? pure_save_open_text(app, module, i, &texts[i], &saved[i]) :
                         new_text(&texts[i], module, module->state_ref[i]);
        } else if (module->state_type[i] == PURE_STATE_LIST) {
            if (result != PURE_OK) continue;
            if (kept[i] >= 0) {
                result = keep_list(&lists[i], app->lists[kept[i]], old, old->state_ref[kept[i]], module,
                                   module->state_ref[i]);
                list_drafts[i] = app->list_drafts[kept[i]];
                rewrite[i] = result == PURE_OK && list_drafts[i] && lists[i] != app->lists[kept[i]];
            } else if (stored) {
                result = pure_save_open_list(app, module, i, &lists[i], &list_drafts[i], &saved[i]);
            } else if (!(lists[i] = new_list(module, module->state_ref[i]))) result = PURE_ALLOCATION;
        } else values[i] = kept[i] >= 0 ? app->values[kept[i]] : module->initial[i];
    }
    /* A cursor or index keeps its position only while its text or list is the
     * one it belonged to. */
    for (uint32_t i = 0; i < module->states; ++i) {
        if (module->state_type[i] != PURE_STATE_CURSOR && module->state_type[i] != PURE_STATE_INDEX) continue;
        uint32_t owner = module->state_ref[i];
        bool same = kept[i] >= 0 && kept[owner] >= 0 && old->state_ref[kept[i]] == (unsigned)kept[owner];
        values[i] = same ? app->values[kept[i]] : 0;
    }
    if (result == PURE_OK && stored) result = pure_save_open_values(app, module, values, false);
    /* Store the lists whose record changed last, so that a failure can put
     * every draft back as it was. */
    for (uint32_t i = 0; i < module->states; ++i)
        if (rewrite[i]) {
            before[i] = pure_draft_version(list_drafts[i]);
            pure_draft_hold(list_drafts[i], true);
        }
    for (uint32_t i = 0; i < module->states && result == PURE_OK; ++i)
        if (rewrite[i]) result = pure_save_rewrite_list(module, i, lists[i], list_drafts[i]);
    for (uint32_t i = 0; i < module->states; ++i)
        if (rewrite[i]) {
            if (result != PURE_OK) pure_draft_restore(list_drafts[i], before[i]);
            pure_draft_hold(list_drafts[i], false);
        }
    if (result != PURE_OK) {
        for (uint32_t i = 0; i < module->states; ++i) {
            if (kept[i] >= 0) {
                if (lists[i] && lists[i] != app->lists[kept[i]]) pure_list_free(lists[i]);
                continue;
            }
            pure_draft_close(texts[i]);
            pure_list_free(lists[i]);
            pure_draft_close(list_drafts[i]);
        }
        return result;
    }
    for (uint32_t i = 0; i < old->states; ++i) {
        bool reused = false, draft_reused = false;
        for (uint32_t k = 0; k < module->states; ++k) {
            reused = reused || (texts[k] && texts[k] == app->texts[i]) || (lists[k] && lists[k] == app->lists[i]);
            draft_reused = draft_reused || (list_drafts[k] && list_drafts[k] == app->list_drafts[i]);
        }
        if (!reused) {
            pure_draft_close(app->texts[i]);
            pure_list_free(app->lists[i]);
        }
        if (!draft_reused) pure_draft_close(app->list_drafts[i]);
    }
    memcpy(app->values, values, sizeof(values));
    memcpy(app->texts, texts, sizeof(texts));
    memcpy(app->lists, lists, sizeof(lists));
    memcpy(app->list_drafts, list_drafts, sizeof(list_drafts));
    memcpy(app->saved, saved, sizeof(saved));
    app->module = module;
    return PURE_OK;
}
