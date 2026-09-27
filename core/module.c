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
/* T_FIELD and T_ITEM stand for the type of the message or record field
 * named, T_KEY for the type of the named keyed list's key. */
enum { T_NUM = 1, T_BYTES = 2, T_TEXT = 3, T_CURSOR = 4, T_FIELD = 5, T_INDEX = 6, T_ITEM = 7, T_KEY = 8 };
enum { FROM_PAYLOAD, FROM_DOCUMENT, FROM_COPY, FROM_CONSTANT, FROM_TEXT, FROM_ARENA };
/* Where an opcode may appear. */
enum { IN_VIEW = 1, IN_HANDLER = 2, IN_MESSAGE = 4, IN_KEYED = 8, IN_TEXT = 16, IN_ROW = 32, IN_OUTER = 64 };
enum { READ = IN_VIEW | IN_HANDLER };
/* What an argument names: nothing (it is 0), a state of one type (A_STATE +
 * the type), a list and one of its record's fields, a message field, the
 * collection, a constant, a unit, the column's row, a branch target, a stack
 * depth, a fold, a relation, a keyed list, an output, or the row of the
 * column a fold runs in. */
enum { A_NONE, A_STATE, A_ITEM = A_STATE + PURE_STATE_INDEX + 1, A_FIELD, A_COLLECTION, A_CONSTANT, A_UNIT,
       A_ROW, A_TARGET, A_DEPTH, A_FOLD, A_RELATION, A_KEYED, A_OUTPUT, A_OUTER };
/* A fold's block is verified as a column that leaves one number (count) or
 * two (a value and whether to include it), a table's filter as a column that
 * leaves one: these stand for a view type. */
enum { FOLD_ONE = 100, FOLD_TWO, FILTER };
/* The text or list an opcode works on: its cursor and index operands belong
 * to it, and so does a cursor or index it pushes. O_ANY: no rule. O_OPERAND:
 * it pushes a position in its operand's text or list. O_ARG: the argument.
 * O_REF: the text or list of the argument's cursor or index state. O_LIST:
 * the list of an item argument. */
enum { O_ANY, O_OPERAND, O_ARG, O_REF, O_LIST };
static const unsigned char magic[8] = {'P','U','R','E','M','O','D','5'};

/* Operand types popped (bottom to top), the type pushed, where allowed, what
 * the argument names and the text or list worked on. */
#define NUMS(pops) {pops, {T_NUM, T_NUM, T_NUM}, T_NUM, READ, A_NONE, O_ANY}
#define STATE(type) A_STATE + PURE_STATE_##type
static const struct { unsigned char pops, in[3], out, where, arg, of; } signature[PURE_OP_COUNT] = {
    [PURE_OP_CONST] = NUMS(0), [PURE_OP_ADD] = NUMS(2), [PURE_OP_SUB] = NUMS(2), [PURE_OP_MUL] = NUMS(2),
    [PURE_OP_DIV] = NUMS(2), [PURE_OP_NEG] = NUMS(1), [PURE_OP_LT] = NUMS(2), [PURE_OP_LE] = NUMS(2),
    [PURE_OP_GT] = NUMS(2), [PURE_OP_GE] = NUMS(2), [PURE_OP_EQ] = NUMS(2), [PURE_OP_NE] = NUMS(2),
    [PURE_OP_AND] = NUMS(2), [PURE_OP_OR] = NUMS(2), [PURE_OP_NOT] = NUMS(1), [PURE_OP_MIN] = NUMS(2),
    [PURE_OP_MAX] = NUMS(2), [PURE_OP_CLAMP] = NUMS(3),
    [PURE_OP_LOAD] = {0, {0}, T_NUM, READ, STATE(NUM), O_ANY},
    [PURE_OP_DT] = {0, {0}, T_NUM, IN_HANDLER, A_NONE, O_ANY},
    [PURE_OP_STORE] = {1, {T_NUM}, 0, IN_HANDLER, STATE(NUM), O_ANY},
    [PURE_OP_JZ] = {1, {T_NUM}, 0, READ, A_TARGET, O_ANY},
    [PURE_OP_JMP] = {0, {0}, 0, READ, A_TARGET, O_ANY},
    [PURE_OP_FIELD] = {0, {0}, T_FIELD, IN_MESSAGE, A_FIELD, O_ANY},
    [PURE_OP_DOC] = {0, {0}, T_BYTES, IN_KEYED, A_COLLECTION, O_ANY},
    [PURE_OP_DOC_SIZE] = {0, {0}, T_NUM, IN_KEYED, A_COLLECTION, O_ANY},
    [PURE_OP_LEN] = {1, {T_BYTES}, T_NUM, IN_HANDLER, A_NONE, O_ANY},
    [PURE_OP_REPLACE] = {3, {T_NUM, T_NUM, T_BYTES}, 0, IN_KEYED, A_COLLECTION, O_ANY},
    [PURE_OP_REPLY] = {1, {T_BYTES}, 0, IN_MESSAGE, A_NONE, O_ANY},
    [PURE_OP_REQUIRE] = {1, {T_NUM}, 0, IN_HANDLER, A_NONE, O_ANY},
    [PURE_OP_TEXT_CONST] = {0, {0}, T_TEXT, READ, A_CONSTANT, O_ANY},
    [PURE_OP_TEXT_LOAD] = {0, {0}, T_TEXT, READ, STATE(TEXT), O_ANY},
    [PURE_OP_TEXT_INPUT] = {0, {0}, T_TEXT, IN_TEXT, A_NONE, O_ANY},
    [PURE_OP_NUM_TEXT] = {1, {T_NUM}, T_TEXT, READ, A_NONE, O_ANY},
    [PURE_OP_CONCAT] = {2, {T_TEXT, T_TEXT}, T_TEXT, READ, A_NONE, O_ANY},
    [PURE_OP_CHARS] = {1, {T_TEXT}, T_NUM, READ, A_NONE, O_ANY},
    [PURE_OP_LINES] = {1, {T_TEXT}, T_NUM, READ, A_NONE, O_ANY},
    [PURE_OP_TEXT_BYTES] = {1, {T_TEXT}, T_NUM, READ, A_NONE, O_ANY},
    [PURE_OP_CUR_LOAD] = {0, {0}, T_CURSOR, READ, STATE(CURSOR), O_REF},
    [PURE_OP_CUR_START] = {0, {0}, T_CURSOR, READ, STATE(TEXT), O_ARG},
    [PURE_OP_CUR_END] = {0, {0}, T_CURSOR, READ, STATE(TEXT), O_ARG},
    [PURE_OP_CUR_MOVE] = {2, {T_CURSOR, T_NUM}, T_CURSOR, READ, A_UNIT, O_OPERAND},
    [PURE_OP_CUR_STORE] = {1, {T_CURSOR}, 0, IN_HANDLER, STATE(CURSOR), O_REF},
    [PURE_OP_TEXT_INSERT] = {2, {T_CURSOR, T_TEXT}, 0, IN_HANDLER, STATE(TEXT), O_ARG},
    [PURE_OP_TEXT_REMOVE] = {2, {T_CURSOR, T_CURSOR}, 0, IN_HANDLER, STATE(TEXT), O_ARG},
    [PURE_OP_TEXT_LINE] = {1, {T_CURSOR}, T_TEXT, READ, STATE(TEXT), O_ARG},
    [PURE_OP_TEXT_SET] = {1, {T_TEXT}, 0, IN_HANDLER, STATE(TEXT), O_ANY},
    [PURE_OP_TEXT_FROM] = {1, {T_BYTES}, T_TEXT, READ, A_NONE, O_ANY},
    [PURE_OP_BYTES_FROM] = {1, {T_TEXT}, T_BYTES, READ, A_NONE, O_ANY},
    [PURE_OP_CUR_COUNT] = {1, {T_CURSOR}, T_NUM, READ, A_UNIT, O_ANY},
    /* Record operations also take as many values as the list's record has fields. */
    [PURE_OP_LIST_COUNT] = {0, {0}, T_NUM, READ, STATE(LIST), O_ANY},
    [PURE_OP_LIST_APPEND] = {0, {0}, 0, IN_HANDLER, STATE(LIST), O_ANY},
    [PURE_OP_LIST_INSERT] = {1, {T_INDEX}, 0, IN_HANDLER, STATE(LIST), O_ARG},
    [PURE_OP_LIST_REMOVE] = {1, {T_INDEX}, 0, IN_HANDLER, STATE(LIST), O_ARG},
    [PURE_OP_ITEM_GET] = {1, {T_INDEX}, T_ITEM, READ, A_ITEM, O_LIST},
    [PURE_OP_ITEM_SET] = {1, {T_INDEX}, 0, IN_HANDLER, A_ITEM, O_LIST},
    [PURE_OP_INDEX_LOAD] = {0, {0}, T_INDEX, READ, STATE(INDEX), O_REF},
    [PURE_OP_INDEX_STORE] = {1, {T_INDEX}, 0, IN_HANDLER, STATE(INDEX), O_REF},
    [PURE_OP_INDEX_MOVE] = {2, {T_INDEX, T_NUM}, T_INDEX, READ, A_NONE, O_OPERAND},
    [PURE_OP_INDEX_FIRST] = {0, {0}, T_INDEX, READ, STATE(LIST), O_ARG},
    [PURE_OP_INDEX_LAST] = {0, {0}, T_INDEX, READ, STATE(LIST), O_ARG},
    [PURE_OP_INDEX_NUM] = {1, {T_INDEX}, T_NUM, READ, A_NONE, O_ANY},
    [PURE_OP_ROW] = {0, {0}, T_INDEX, IN_ROW, A_ROW, O_ARG},
    /* In a fold that runs in a column, the column's row (the fold's own item is ROW's). */
    [PURE_OP_OUTER_ROW] = {0, {0}, T_INDEX, IN_OUTER, A_OUTER, O_ARG},
    /* Stack copies: their types come from the values they move. */
    [PURE_OP_PICK] = {0, {0}, 0, READ, A_DEPTH, O_ANY},
    [PURE_OP_SLIDE] = {0, {0}, 0, READ, A_DEPTH, O_ANY},
    [PURE_OP_FOLD] = {0, {0}, T_NUM, READ, A_FOLD, O_ANY},
    [PURE_OP_TEXT_COMPARE] = {2, {T_TEXT, T_TEXT}, T_NUM, READ, A_RELATION, O_ANY},
    [PURE_OP_LIST_FIND] = {1, {T_KEY}, T_INDEX, READ, A_KEYED, O_ARG},
    [PURE_OP_LIST_HAS] = {1, {T_KEY}, T_NUM, READ, A_KEYED, O_ANY},
    [PURE_OP_NOW] = {0, {0}, T_NUM, IN_HANDLER, A_NONE, O_ANY},
    [PURE_OP_SEND] = {0, {0}, 0, IN_HANDLER, A_OUTPUT, O_ANY},
    [PURE_OP_FLOOR] = NUMS(1), [PURE_OP_MOD] = NUMS(2),
};
#undef NUMS
#undef STATE

static uint32_t read16(const unsigned char *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }

static uint32_t read32(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void write32(unsigned char *p, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) p[i] = (unsigned char)(value >> (8 * i));
}

static double read_double(const unsigned char *p) {
    uint64_t bits = read32(p) | (uint64_t)read32(p + 4) << 32;
    double value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

PURE_COLD static bool key_name(const char *key) {
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

/* Strings are [u8 length][bytes]; a name is 1..31 bytes: an ASCII identifier,
 * a path of identifiers joined by dots (a message field inside a group, as
 * user.name), or any printable ASCII (a key's name). */
enum { NAME_IDENTIFIER, NAME_PATH, NAME_PRINTABLE };
PURE_COLD static bool name_at(const unsigned char *strings, uint32_t size, uint32_t offset,
                    int kind, char out[PURE_MODULE_NAME]) {
    if (offset >= size) return false;
    uint32_t length = strings[offset];
    if (!length || length >= PURE_MODULE_NAME || length > size - offset - 1) return false;
    for (uint32_t i = 0, start = 0; i < length; ++i) {
        unsigned char c = strings[offset + 1 + i];
        bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
        if (kind == NAME_PATH && c == '.' && i > start && i + 1 < length) { start = i + 1; continue; }
        if (!letter && !(i > start && c >= '0' && c <= '9') && (kind != NAME_PRINTABLE || c < 32 || c > 126))
            return false;
    }
    memcpy(out, strings + offset + 1, length);
    out[length] = 0;
    return true;
}

/* The input whose handler this is, or -1 for frame. */
static int event_input(uint32_t event) { return event ? (int)((event - 1) / 2) : -1; }

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

PURE_COLD static bool names_arg(const PureModule *m, unsigned kind, uint32_t arg, const PureInputShape *shape, int row,
                                int outer) {
    switch (kind) {
    case A_ITEM:
        return state_of(m, list_of(arg), PURE_STATE_LIST) && item_field(arg) < m->record[m->state_ref[list_of(arg)]].count;
    case A_FIELD: return arg < shape->field_count;
    case A_COLLECTION: return arg < m->collections;
    case A_CONSTANT: return arg < m->constants;
    case A_UNIT: return arg <= PURE_UNIT_LINE;
    case A_ROW: return (int)arg == row;
    case A_OUTER: return outer >= 0 && (int)arg == outer;
    case A_FOLD: return (arg & 0xFF) <= PURE_FOLD_FIRST && state_of(m, arg >> 8 & 0xFF, PURE_STATE_LIST) && arg >> 16;
    case A_RELATION: return arg <= PURE_NOT_EQUAL;
    case A_KEYED: return state_of(m, arg, PURE_STATE_LIST) && m->state_key[arg];
    case A_OUTPUT: return arg < m->inputs && m->input[arg].kind == PURE_KIND_OUTPUT;
    case A_NONE: case A_TARGET: case A_DEPTH: return true;
    default: return state_of(m, arg, (uint8_t)(kind - A_STATE));
    }
}

/* The stack a branch brings to its target, while the target is ahead. */
typedef struct { uint32_t target; int height; unsigned char types[PURE_MODULE_STACK], owners[PURE_MODULE_STACK]; } Join;

static bool same_stack(const Join *join, int height, const unsigned char *types, const unsigned char *owners) {
    return join->height == height && !memcmp(join->types, types, (size_t)height) &&
           !memcmp(join->owners, owners, (size_t)height);
}

/* Types and heights are checked per instruction; a cursor's type records
 * which text it belongs to and an index's type which list (other values
 * belong to nothing). Branches go forward within the block. Every path into
 * a join brings the same stack (types and owners), so a value may cross a
 * join, as in a conditional expression; at most 8 joins with a non-empty
 * stack are ahead at once. Handlers end empty, views with one value of their
 * type. A column (row is its list) may read the row it is evaluated for.
 * A fold's block is verified as a column of its list (reads only, no fold
 * in it); the block around treats it as one instruction and may not branch
 * into it. Returns why the block is refused, with at moved to the
 * instruction when one is at fault, or NULL. */
PURE_COLD static const char *verify_block(const PureModule *, PureBlock, int view, int row, int outer, uint32_t event,
                                          uint32_t *at);

/* target marks, per instruction of the block, a branch into it with an empty stack. */
/* outer: the row of the column a fold's block runs in (-1: none). */
PURE_COLD static const char *verify_code(const PureModule *m, PureBlock block, int view, int row, int outer,
                                         uint32_t event, uint32_t *at, unsigned char *target) {
#define REFUSE(why) do { *at = pc; return why; } while (0)
    if (view >= 0 && !block.length) return "empty view";
    int input = view >= 0 ? -1 : event_input(event);
    const PureInputShape *shape = input >= 0 ? &m->input[input] : NULL;
    unsigned where = view >= 0 ? IN_VIEW | (row >= 0 ? IN_ROW : 0) | (view >= FOLD_ONE && outer >= 0 ? IN_OUTER : 0)
                               : IN_HANDLER;
    if (shape && shape->kind == PURE_KIND_MESSAGE) where |= IN_MESSAGE | (shape->keyed ? IN_KEYED : 0);
    if (shape && shape->kind == PURE_KIND_TEXT) where |= IN_TEXT;
    unsigned char types[PURE_MODULE_STACK], owners[PURE_MODULE_STACK];
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
                REFUSE("join with different stacks");
            height = joins[join].height;
            memcpy(types, joins[join].types, (size_t)height);
            memcpy(owners, joins[join].owners, (size_t)height);
            joins[join] = joins[--open];
            reachable = true;
        } else if (target[i]) {
            if (reachable && height) REFUSE("join with different stacks");
            height = 0;
            reachable = true;
        }
        if (i == block.length) break;
        if (!reachable) REFUSE("unreachable instruction");
        const PureOp *op = &m->code[pc];
        if (op->op >= PURE_OP_COUNT) REFUSE("unknown opcode");
        unsigned allowed = signature[op->op].where, kind = signature[op->op].arg, of = signature[op->op].of;
        if (!(allowed & where)) {
            if (allowed & IN_ROW) REFUSE("row outside a column");
            if (allowed & IN_OUTER) REFUSE("outer row outside a fold in a column");
            if (view == FILTER) REFUSE("filter with an effect");
            if (view >= FOLD_ONE) REFUSE("fold with an effect");
            if (view >= 0) REFUSE("view with an effect");
            if (allowed & IN_KEYED) REFUSE("document operation outside a keyed message handler");
            if (allowed & IN_TEXT) REFUSE("text input outside a text handler");
            REFUSE("message operation outside a message handler");
        }
        uint32_t arg = op->arg;
        if ((kind == A_NONE && arg) || (op->op == PURE_OP_CONST ? !isfinite(op->value) : op->value != 0))
            REFUSE("non-canonical operand");
        if (!names_arg(m, kind, arg, shape, row, outer)) REFUSE("operand names no such state, field, constant or unit");
        /* A keyed list places its items by key, and an item's key stays. */
        if (op->op == PURE_OP_LIST_INSERT && m->state_key[arg]) REFUSE("insert by position into a keyed list");
        if (op->op == PURE_OP_ITEM_SET && m->state_key[list_of(arg)] == item_field(arg) + 1)
            REFUSE("change of a keyed list's key");
        /* PICK copies the value arg below the top; SLIDE drops arg values under the top. */
        if (kind == A_DEPTH) {
            bool pick = op->op == PURE_OP_PICK;
            if (pick ? arg >= (uint32_t)height : !arg || arg >= (uint32_t)height) REFUSE("stack underflow");
            if (pick && height == PURE_MODULE_STACK) REFUSE("stack overflow");
            int from = height - 1 - (int)arg, to = pick ? height++ : from;
            types[to] = pick ? types[from] : types[height - 1];
            owners[to] = pick ? owners[from] : owners[height - 1];
            if (!pick) height = from + 1;
            continue;
        }
        uint32_t skip = 0;
        if (kind == A_FOLD) {
            uint32_t inner = pc;
            skip = arg >> 16;
            if (view >= FOLD_ONE) REFUSE(view == FILTER ? "fold inside a filter" : "fold inside a fold");
            if (skip > block.length - i - 1) REFUSE("fold block outside its block");
            unsigned how = arg & 0xFF;
            const char *why = verify_block(m, (PureBlock){pc + 1, skip},
                                           how == PURE_FOLD_COUNT || how == PURE_FOLD_FIRST ? FOLD_ONE : FOLD_TWO,
                                           (int)(arg >> 8 & 0xFF), row, 0, &inner);
            if (why) { *at = inner; return why; }
            for (uint32_t k = i + 1; k <= i + skip; ++k) {
                if (target[k]) REFUSE("branch into a fold");
                for (unsigned j = 0; j < open; ++j) if (joins[j].target == block.start + k) REFUSE("branch into a fold");
            }
        }
        /* Operand types, bottom to top; record operations take their fields. */
        unsigned char need[PURE_MODULE_FIELDS + 2];
        unsigned pops = signature[op->op].pops;
        memcpy(need, signature[op->op].in, sizeof(signature[op->op].in));
        if (op->op == PURE_OP_SEND) {
            const PureInputShape *output = &m->input[arg];
            for (unsigned f = 0; f < output->field_count; ++f) {
                uint8_t type = m->field[output->field_start + f].type;
                need[f] = type == PURE_FIELD_TEXT ? T_TEXT : type == PURE_FIELD_BYTES ? T_BYTES : T_NUM;
            }
            pops = output->field_count;
        } else if (op->op == PURE_OP_LIST_APPEND || op->op == PURE_OP_LIST_INSERT) {
            const PureRecord *record = &m->record[m->state_ref[arg]];
            for (unsigned f = 0; f < record->count; ++f) need[pops + f] = field_type(m, arg, f);
            pops += record->count;
        } else if (op->op == PURE_OP_ITEM_SET) {
            need[pops++] = field_type(m, list_of(arg), item_field(arg));
        } else if (need[0] == T_KEY) {
            need[0] = field_type(m, arg, m->state_key[arg] - 1u);
        }
        if (height < (int)pops) REFUSE("stack underflow");
        height -= (int)pops;
        for (unsigned k = 0; k < pops; ++k)
            if (types[height + (int)k] != need[k]) REFUSE("operand type");
        unsigned owner = of == O_ARG ? arg : of == O_REF ? m->state_ref[arg] : of == O_LIST ? list_of(arg) : 0;
        for (unsigned k = 0; of >= O_ARG && k < pops; ++k) {
            unsigned char type = types[height + (int)k];
            if ((type == T_CURSOR || type == T_INDEX) && owners[height + (int)k] != owner)
                REFUSE(type == T_CURSOR ? "cursor of another text" : "index of another list");
        }
        unsigned char out = signature[op->op].out;
        if (kind == A_FOLD && (arg & 0xFF) == PURE_FOLD_FIRST) { /* an index of its list */
            out = T_INDEX;
            of = O_ARG;
            owner = arg >> 8 & 0xFF;
        }
        if (out == T_FIELD) {
            uint8_t type = m->field[shape->field_start + arg].type;
            out = type == PURE_FIELD_TEXT ? T_TEXT : type == PURE_FIELD_BYTES ? T_BYTES : T_NUM;
        }
        if (out == T_ITEM) out = field_type(m, list_of(arg), item_field(arg));
        if (out) {
            if (height == PURE_MODULE_STACK) REFUSE("stack overflow");
            owners[height] = (unsigned char)(out != T_CURSOR && out != T_INDEX ? 0 : of == O_OPERAND ? owners[height] : owner);
            types[height++] = out;
        }
        if (kind == A_TARGET) {
            if (arg <= pc || arg > block.start + block.length) REFUSE("branch not forward within its block");
            join = open;
            for (unsigned k = 0; k < open; ++k) if (joins[k].target == arg) join = k;
            if (!height) {
                if (join < open) REFUSE("join with different stacks");
                target[arg - block.start] = 1;
            } else if (join < open) {
                if (!same_stack(&joins[join], height, types, owners)) REFUSE("join with different stacks");
            } else {
                if (target[arg - block.start]) REFUSE("join with different stacks");
                if (open == PURE_MODULE_JOINS) REFUSE("more than 8 joins ahead");
                joins[open] = (Join){.target = arg, .height = height};
                memcpy(joins[open].types, types, (size_t)height);
                memcpy(joins[open++].owners, owners, (size_t)height);
            }
            if (op->op == PURE_OP_JMP) reachable = false;
        }
        i += skip;
    }
    if (!reachable) return "block end is unreachable";
    if (view == FILTER) return height != 1 || types[0] != T_NUM ? "filter must leave one number" : NULL;
    if (view >= FOLD_ONE) {
        int want = view - FOLD_ONE + 1;
        return height != want || types[0] != T_NUM || types[want - 1] != T_NUM ? "fold block must leave its numbers" : NULL;
    }
    unsigned char result = view == PURE_VIEW_TEXT ? T_TEXT : T_NUM;
    if (view >= 0 ? height != 1 || types[0] != result : height != 0)
        return view >= 0 ? "view must leave one value of its type" : "handler must leave no value";
    return NULL;
#undef REFUSE
}

PURE_COLD static const char *verify_block(const PureModule *m, PureBlock block, int view, int row, int outer,
                                          uint32_t event, uint32_t *at) {
    if (block.start > m->code_count || block.length > m->code_count - block.start) return "code range outside module";
    unsigned char *target = calloc((size_t)block.length + 1, 1);
    if (!target) return "out of memory";
    const char *why = verify_code(m, block, view, row, outer, event, at, target);
    free(target);
    return why;
}

/* Whether an instruction has a form that works on the items of a run at
 * once: numbers and indexes, with no branch and no text. */
PURE_COLD static bool has_lanes(const PureModule *m, const PureOp *op) {
    switch (op->op) {
    case PURE_OP_CONST: case PURE_OP_ADD: case PURE_OP_SUB: case PURE_OP_MUL: case PURE_OP_DIV: case PURE_OP_NEG:
    case PURE_OP_LT: case PURE_OP_LE: case PURE_OP_GT: case PURE_OP_GE: case PURE_OP_EQ: case PURE_OP_NE:
    case PURE_OP_AND: case PURE_OP_OR: case PURE_OP_NOT: case PURE_OP_MIN: case PURE_OP_MAX: case PURE_OP_CLAMP:
    case PURE_OP_FLOOR: case PURE_OP_MOD: case PURE_OP_LOAD: case PURE_OP_LIST_COUNT: case PURE_OP_INDEX_LOAD:
    case PURE_OP_INDEX_MOVE: case PURE_OP_INDEX_FIRST: case PURE_OP_INDEX_LAST: case PURE_OP_INDEX_NUM:
    case PURE_OP_ROW: case PURE_OP_OUTER_ROW: case PURE_OP_PICK: case PURE_OP_SLIDE:
        return true;
    case PURE_OP_ITEM_GET: return field_type(m, list_of(op->arg), item_field(op->arg)) == T_NUM;
    case PURE_OP_LIST_FIND: case PURE_OP_LIST_HAS: return field_type(m, op->arg, m->state_key[op->arg] - 1u) == T_NUM;
    default: return false;
    }
}

PURE_COLD static bool block_has_lanes(const PureModule *m, PureBlock block) {
    for (uint32_t pc = block.start; pc < block.start + block.length; ++pc)
        if (!has_lanes(m, &m->code[pc])) return false;
    return true;
}

/* A fold in a verified block whose own block has lane forms throughout runs
 * a leaf of its list at a time. */
PURE_COLD static void plan_folds(PureModule *m, PureBlock block) {
    for (uint32_t pc = block.start; pc < block.start + block.length; ++pc) {
        PureOp *op = &m->code[pc];
        if (op->op != PURE_OP_FOLD) continue;
        op->plan = block_has_lanes(m, (PureBlock){pc + 1, op->arg >> 16});
        pc += op->arg >> 16;
    }
}

PURE_COLD PureResult pure_module_read(const char *path, PureModule *out, char *error, size_t error_size) {
    FILE *file = fopen(path, "rb");
    unsigned char *bytes = NULL;
    size_t size = 0, capacity = 0;
    bool ok = file != NULL;
    /* The largest module is some 2 MiB: code, strings and constants at their limits. */
    while (ok && !feof(file) && capacity < (8u << 20)) {
        capacity = capacity ? capacity * 2 : 1 << 16;
        unsigned char *grown = realloc(bytes, capacity);
        if (!grown) { ok = false; break; }
        bytes = grown;
        size += fread(bytes + size, 1, capacity - size, file);
        ok = !ferror(file);
    }
    ok = ok && feof(file);
    if (file) fclose(file);
    PureResult result = PURE_NOT_FOUND;
    if (!ok) {
        if (error_size) snprintf(error, error_size, "cannot read %s", path);
        pure_module_release(out);
    } else result = pure_module_load(bytes, size, out, error, error_size);
    free(bytes);
    return result;
}

void pure_module_release(PureModule *m) {
    free(m->storage);
    m->storage = NULL;
    m->code = NULL;
    m->constant_offset = m->constant_length = NULL;
    m->constant_data = NULL;
}

PURE_COLD PureResult pure_module_load(const void *bytes, size_t size, PureModule *out,
                            char *error, size_t error_size) {
    const unsigned char *p = bytes;
    const char *why;
    uint32_t bad = 0;
    PureResult result = PURE_INVALID;
#define REFUSE(message, index) do { why = message; bad = (uint32_t)(index); goto refused; } while (0)
    if (error_size) error[0] = 0;
    pure_module_release(out);
    memset(out, 0, sizeof(*out));
    if (!p || size < PURE_MODULE_HEADER || memcmp(p, magic, sizeof(magic))) REFUSE("not a Pure module v5", 0);
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
    if (read16(p + 38)) REFUSE("reserved header bytes", 38);
    if (out->states > PURE_MODULE_STATES || out->inputs > PURE_MODULE_INPUTS ||
        out->views > PURE_MODULE_VIEWS || out->handlers > PURE_MODULE_HANDLERS ||
        out->code_count > PURE_MODULE_CODE || strings > PURE_MODULE_STRINGS ||
        out->fields > PURE_MODULE_FIELDS || out->collections > PURE_MODULE_COLLECTIONS ||
        out->constants > PURE_MODULE_CONSTANTS || constant_bytes > PURE_MODULE_CONSTANT_BYTES ||
        out->records > PURE_MODULE_RECORDS || out->record_fields > PURE_MODULE_RECORD_FIELDS) {
        result = PURE_LIMIT;
        REFUSE("module exceeds a limit", 8);
    }
    size_t expected = PURE_MODULE_HEADER + (size_t)out->states * STATE_BYTES +
        (size_t)out->collections * COLLECTION_BYTES + (size_t)out->inputs * INPUT_BYTES +
        (size_t)out->fields * FIELD_BYTES + (size_t)out->constants * CONSTANT_BYTES +
        (size_t)out->records * RECORD_BYTES + (size_t)out->record_fields * RECORD_FIELD_BYTES +
        (size_t)out->views * VIEW_BYTES + (size_t)out->handlers * HANDLER_BYTES +
        (size_t)out->code_count * OP_BYTES + strings + constant_bytes;
    if (size != expected) REFUSE("module size", expected);
    /* The code, the constants' offsets and lengths, then their bytes, in one block. */
    size_t storage = (size_t)out->code_count * sizeof(PureOp) + (size_t)out->constants * 2 * sizeof(uint32_t) +
                     constant_bytes;
    if (!(out->storage = malloc(storage ? storage : 1))) {
        result = PURE_ALLOCATION;
        REFUSE("out of memory", 0);
    }
    out->code = out->storage;
    out->constant_offset = (uint32_t *)(out->code + out->code_count);
    out->constant_length = out->constant_offset + out->constants;
    out->constant_data = (unsigned char *)(out->constant_length + out->constants);
    const unsigned char *at = p + PURE_MODULE_HEADER;
    const unsigned char *text = p + size - constant_bytes - strings;
    if (constant_bytes) memcpy(out->constant_data, p + size - constant_bytes, constant_bytes);
    for (uint32_t i = 0; i < out->states; ++i, at += STATE_BYTES) {
        out->state_type[i] = (uint8_t)read32(at + 4);
        if (!name_at(text, strings, read32(at), NAME_IDENTIFIER, out->state_names[i])) REFUSE("state name", i);
        if (read32(at + 4) > PURE_STATE_INDEX) REFUSE("state type", i);
        if (out->state_type[i] == PURE_STATE_NUM) {
            out->initial[i] = read_double(at + 8);
            if (!isfinite(out->initial[i])) REFUSE("non-finite initial value", i);
        } else {
            /* Flags: a mark (cursor), or a keyed list's key field + 1. */
            uint32_t flags = read32(at + 12);
            bool cursor = out->state_type[i] == PURE_STATE_CURSOR, list = out->state_type[i] == PURE_STATE_LIST;
            if ((flags && !(cursor && flags == PURE_CURSOR_STAYS) && !(list && flags <= PURE_RECORD_FIELDS)) ||
                read32(at + 8) > UINT16_MAX) REFUSE("state reference", i);
            out->state_ref[i] = (uint16_t)read32(at + 8);
            out->state_flags[i] = cursor ? (uint8_t)flags : 0;
            out->state_key[i] = list ? (uint8_t)flags : 0;
        }
    }
    for (uint32_t i = 0; i < out->collections; ++i, at += COLLECTION_BYTES) {
        if (!name_at(text, strings, read32(at), NAME_IDENTIFIER, out->collection_names[i])) REFUSE("collection name", i);
        if (read32(at + 4) != 1 || read32(at + 8) != 1) REFUSE("collection must hold bytes by key", i);
    }
    uint32_t next_field = 0;
    const unsigned char *fields = at + (size_t)out->inputs * INPUT_BYTES;
    for (uint32_t i = 0; i < out->inputs; ++i, at += INPUT_BYTES) {
        PureInputShape *shape = &out->input[i];
        shape->kind = at[4];
        shape->keyed = at[5];
        shape->field_start = (uint16_t)read16(at + 12);
        shape->field_count = (uint16_t)read16(at + 14);
        if (!name_at(text, strings, read32(at), NAME_IDENTIFIER, out->input_names[i])) REFUSE("input name", i);
        if (read16(at + 6) || at[5] > 1) REFUSE("input flags", i);
        if (shape->kind == PURE_KIND_KEY) {
            if (shape->keyed || shape->field_start || shape->field_count ||
                !name_at(text, strings, read32(at + 8), NAME_PRINTABLE, out->input_keys[i]) ||
                !key_name(out->input_keys[i])) REFUSE("input key", i);
            for (uint32_t k = 0; k < i; ++k)
                if (out->input[k].kind == PURE_KIND_KEY && !strcmp(out->input_keys[k], out->input_keys[i]))
                    REFUSE("duplicate key", i);
            continue;
        }
        if (shape->kind == PURE_KIND_TEXT) {
            if (shape->keyed || shape->field_start || shape->field_count || read32(at + 8))
                REFUSE("text input shape", i);
            continue;
        }
        if (shape->kind == PURE_KIND_TIMER) {
            shape->period = read32(at + 8);
            if (shape->keyed || shape->field_start || shape->field_count || !shape->period ||
                shape->period > PURE_TIMER_LONGEST) REFUSE("timer input", i);
            continue;
        }
        if ((shape->kind != PURE_KIND_MESSAGE && shape->kind != PURE_KIND_OUTPUT) || read32(at + 8) ||
            (shape->kind == PURE_KIND_OUTPUT && shape->keyed) || shape->field_start != next_field ||
            shape->field_count > out->fields - next_field) REFUSE("message shape", i);
        /* The prefix holds the fixed fields and the variable fields' lengths
         * in order, but the last variable field's: it takes the rest. */
        uint32_t last = UINT32_MAX;
        for (uint32_t f = 0; f < shape->field_count; ++f) {
            uint32_t type = read32(fields + (size_t)(next_field + f) * FIELD_BYTES + 4);
            if (type == PURE_FIELD_BYTES || type == PURE_FIELD_TEXT) last = f;
        }
        for (uint32_t f = 0; f < shape->field_count; ++f) {
            const unsigned char *entry = fields + (size_t)(next_field + f) * FIELD_BYTES;
            PureField *field = &out->field[next_field + f];
            field->type = (uint8_t)read32(entry + 4);
            if (!name_at(text, strings, read32(entry), NAME_PATH, field->name) || field->type < PURE_FIELD_U32 ||
                read32(entry + 4) > PURE_FIELD_TEXT)
                REFUSE("message field", next_field + f);
            /* A name is a field or a group, never both (user and user.name). */
            for (uint32_t k = 0; k < f; ++k) {
                const char *a = out->field[next_field + k].name, *b = field->name;
                size_t la = strlen(a), lb = strlen(b);
                if (!strcmp(a, b)) REFUSE("duplicate field", next_field + f);
                if ((la < lb && !memcmp(a, b, la) && b[la] == '.') || (lb < la && !memcmp(a, b, lb) && a[lb] == '.'))
                    REFUSE("field and group", next_field + f);
            }
            field->offset = f == last ? PURE_FIELD_REST : shape->prefix;
            if (f != last) shape->prefix += field->type == PURE_FIELD_NUMBER ? 8 : 4;
        }
        next_field += shape->field_count;
    }
    if (next_field != out->fields) REFUSE("unused message fields", next_field);
    at = fields + (size_t)out->fields * FIELD_BYTES;
    for (uint32_t i = 0; i < out->constants; ++i, at += CONSTANT_BYTES) {
        out->constant_offset[i] = read32(at);
        out->constant_length[i] = read32(at + 4);
        if (out->constant_offset[i] > constant_bytes || out->constant_length[i] > constant_bytes - out->constant_offset[i] ||
            !pure_utf8_valid((PureSpan){out->constant_data + out->constant_offset[i], out->constant_length[i]}))
            REFUSE("text constant", i);
    }
    /* Records: fields in order, numbered from first; a text default is a constant. */
    const unsigned char *fields_at = at + (size_t)out->records * RECORD_BYTES;
    uint32_t next_record_field = 0;
    for (uint32_t i = 0; i < out->records; ++i, at += RECORD_BYTES) {
        PureRecord *record = &out->record[i];
        record->first = (uint16_t)read16(at + 4);
        record->count = (uint16_t)read16(at + 6);
        if (!name_at(text, strings, read32(at), NAME_IDENTIFIER, out->record_names[i]) || record->first != next_record_field ||
            !record->count || record->count > PURE_RECORD_FIELDS || record->count > out->record_fields - next_record_field)
            REFUSE("record", i);
        for (uint32_t f = record->first; f < record->first + record->count; ++f) {
            const unsigned char *entry = fields_at + (size_t)f * RECORD_FIELD_BYTES;
            uint32_t type = read32(entry + 4);
            out->record_field_text[f] = type == 1;
            if (!name_at(text, strings, read32(entry), NAME_IDENTIFIER, out->record_field_names[f]) || type > 1)
                REFUSE("record field", f);
            for (uint32_t k = record->first; k < f; ++k)
                if (!strcmp(out->record_field_names[k], out->record_field_names[f])) REFUSE("duplicate record field", f);
            out->record_field_default[f] = type ? read32(entry + 8) : read_double(entry + 8);
            if (type ? read32(entry + 12) || read32(entry + 8) >= out->constants : !isfinite(out->record_field_default[f]))
                REFUSE("record field default", f);
        }
        next_record_field += record->count;
    }
    if (next_record_field != out->record_fields) REFUSE("unused record fields", next_record_field);
    at = fields_at + (size_t)out->record_fields * RECORD_FIELD_BYTES;
    for (uint32_t i = 0; i < out->states; ++i)
        if ((out->state_type[i] == PURE_STATE_TEXT && out->state_ref[i] >= out->constants) ||
            (out->state_type[i] == PURE_STATE_CURSOR && !state_of(out, out->state_ref[i], PURE_STATE_TEXT)) ||
            (out->state_type[i] == PURE_STATE_LIST &&
             (out->state_ref[i] >= out->records || out->state_key[i] > out->record[out->state_ref[i]].count)) ||
            (out->state_type[i] == PURE_STATE_INDEX && !state_of(out, out->state_ref[i], PURE_STATE_LIST)))
            REFUSE("state reference", i);
    /* A table names its list in place of code; its columns follow it. */
    for (uint32_t i = 0; i < out->views; ++i, at += VIEW_BYTES) {
        if (!name_at(text, strings, read32(at), NAME_IDENTIFIER, out->view_names[i])) REFUSE("view name", i);
        out->view_type[i] = at[4];
        out->view_table[i] = (uint16_t)read16(at + 6);
        out->view_code[i] = (PureBlock){read32(at + 8), read32(at + 12)};
        uint32_t table = out->view_table[i];
        if (at[4] > PURE_VIEW_TABLE || (at[5] && at[4] != PURE_VIEW_TABLE) ||
            (table && (table > i || out->view_type[table - 1] != PURE_VIEW_TABLE || at[4] == PURE_VIEW_TABLE)))
            REFUSE("view type", i);
        /* A table names its list in start, or, filtered, in the byte after its
         * type (list + 1) with its filter's block in start and length. */
        if (at[4] == PURE_VIEW_TABLE) {
            uint32_t list = at[5] ? at[5] - 1u : out->view_code[i].start;
            if (!state_of(out, list, PURE_STATE_LIST) || !at[5] != !out->view_code[i].length) REFUSE("table list", i);
            out->view_list[i] = (uint16_t)list;
        } else if (table) out->view_list[i] = out->view_list[table - 1];
    }
    for (uint32_t i = 0; i < out->handlers; ++i, at += HANDLER_BYTES) {
        uint32_t event = read32(at);
        out->handler[i] = (PureHandler){event, {read32(at + 4), read32(at + 8)}};
        int input = event_input(event);
        if (event > 2u * out->inputs || (input >= 0 && out->input[input].kind != PURE_KIND_KEY && !(event & 1)) ||
            (input >= 0 && out->input[input].kind == PURE_KIND_OUTPUT))
            REFUSE("handler event", i);
    }
    for (uint32_t i = 0; i < out->code_count; ++i, at += OP_BYTES) {
        if (at[1] || at[2] || at[3]) REFUSE("instruction padding", i);
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
            if (!strcmp(names[i], names[k])) REFUSE("duplicate name", i);
    /* Column names are unique within their table. */
    for (uint32_t i = 0; i < out->views; ++i)
        for (uint32_t k = 0; k < i; ++k)
            if (out->view_table[i] && out->view_table[k] == out->view_table[i] &&
                !strcmp(out->view_names[i], out->view_names[k])) REFUSE("duplicate column", i);
    for (uint32_t i = 0; i < out->views; ++i) {
        bad = i;
        bool table = out->view_type[i] == PURE_VIEW_TABLE;
        if ((!table || out->view_code[i].length) &&
            (why = verify_block(out, out->view_code[i], table ? FILTER : out->view_type[i],
                                table || out->view_table[i] ? out->view_list[i] : -1, -1, 0, &bad))) goto refused;
    }
    for (uint32_t i = 0; i < out->handlers; ++i) {
        bad = i;
        if ((why = verify_block(out, out->handler[i].code, -1, -1, -1, out->handler[i].event, &bad))) goto refused;
    }
    for (uint32_t i = 0; i < out->views; ++i) {
        plan_folds(out, out->view_code[i]);
        out->view_plan[i] = out->view_type[i] == PURE_VIEW_TABLE && out->view_code[i].length &&
                            block_has_lanes(out, out->view_code[i]);
    }
    for (uint32_t i = 0; i < out->handlers; ++i) plan_folds(out, out->handler[i].code);
    return PURE_OK;
refused:
    if (error_size) snprintf(error, error_size, "%s (%u)", why, bad);
    pure_module_release(out);
    return result;
#undef REFUSE
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

int pure_module_position(const PureModule *m, uint32_t view) {
    if (view >= m->views || m->view_type[view] != PURE_VIEW_NUM || m->view_table[view]) return -1;
    const PureBlock *block = &m->view_code[view];
    const PureOp *code = &m->code[block->start];
    return block->length == 2 && code[0].op == PURE_OP_INDEX_LOAD && code[1].op == PURE_OP_INDEX_NUM ? (int)code[0].arg : -1;
}

int pure_module_text_state(const PureModule *m, uint32_t view) {
    if (view >= m->views || m->view_type[view] != PURE_VIEW_TEXT || m->view_table[view]) return -1;
    const PureBlock *block = &m->view_code[view];
    return block->length == 1 && m->code[block->start].op == PURE_OP_TEXT_LOAD ? (int)m->code[block->start].arg : -1;
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
    const char *key;             /* a keyed message's key */
    int64_t base;                /* the revision a message's edit was made against, or -1 */
    PureArena *arena;
    double dt;
    size_t row;   /* the row a column is evaluated for (in a fold, the fold's item) */
    size_t outer; /* in a fold, the row of the column it runs in */
    bool modified, exists;
    uint32_t (*spans)[2]; /* a message's variable fields: start and size in the payload */
} Event;

static PureResult fault(PureApp *app, PureFault kind, uint32_t pc) {
    app->fault = kind;
    app->fault_pc = pc;
    return kind == PURE_FAULT_LIMIT ? PURE_LIMIT : kind == PURE_FAULT_NOT_FOUND ? PURE_NOT_FOUND :
           kind == PURE_FAULT_EXISTS ? PURE_CONFLICT : PURE_INVALID;
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
        } else if (position > at || (position == at && !(m->state_flags[c] & PURE_CURSOR_STAYS)))
            position += (double)inserted;
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

/* Append bytes to the arena as a value of it. */
static PureResult keep_bytes(PureArena *arena, const void *bytes, size_t length, PureBytes *out) {
    if (!reserve(arena, length)) return PURE_ALLOCATION;
    if (length) memcpy(arena->bytes + arena->used, bytes, length);
    *out = (PureBytes){FROM_ARENA, 0, {0, 0}, arena->used, length};
    arena->used += length;
    return PURE_OK;
}

/* Run a verified block. Types and stack bounds were proven at load. A change
 * the data refuses (a size limit, a missing item) fails the event with its
 * fault; other failures pass through. */
typedef struct { unsigned char *at; } Copy;

static bool copy_out(void *context, PureSpan span) {
    Copy *copy = context;
    if (span.size) memcpy(copy->at, span.data, span.size);
    copy->at += span.size;
    return true;
}

/* An output's message from its field values, laid out as a message's
 * payload (size bytes), numbered and kept with the event's changes. */
static PureResult send(PureApp *app, uint32_t output, const Slot *fields, const PureArena *arena, size_t size) {
    const PureModule *m = app->module;
    const PureInputShape *shape = &m->input[output];
    if (app->sent_count == app->sent_capacity) {
        size_t capacity = app->sent_capacity ? 2 * app->sent_capacity : 16;
        struct PureSent *grown = realloc(app->sent, capacity * sizeof(*grown));
        if (!grown) return PURE_ALLOCATION;
        app->sent = grown;
        app->sent_capacity = capacity;
    }
    unsigned char *payload = malloc(size ? size : 1);
    if (!payload) return PURE_ALLOCATION;
    Copy copy = {payload + shape->prefix};
    for (uint32_t f = 0; f < shape->field_count; ++f) {
        const PureField *field = &m->field[shape->field_start + f];
        if (field->type == PURE_FIELD_U32) write32(payload + field->offset, (uint32_t)fields[f].num);
        else if (field->type == PURE_FIELD_NUMBER) {
            uint64_t bits;
            memcpy(&bits, &fields[f].num, 8);
            write32(payload + field->offset, (uint32_t)bits);
            write32(payload + field->offset + 4, (uint32_t)(bits >> 32));
        } else {
            if (field->offset != PURE_FIELD_REST) write32(payload + field->offset, (uint32_t)fields[f].bytes.length);
            PureResult result = visit_value(app, arena, fields[f].bytes, copy_out, &copy);
            if (result != PURE_OK) { free(payload); return result; }
        }
    }
    app->sent[app->sent_count++] = (struct PureSent){output, ++app->last_sent[output], payload, size, false};
    app->links_changed = true;
    return PURE_OK;
}

static PureResult run(const PureModule *m, double *values, PureBlock block, Event *ev, Slot *result);

/* ---- Folds and filters a leaf at a time ------------------------------
 * A block whose instructions all have lane forms (has_lanes) runs for a run
 * of its list's items at once: each value is one per item (a lane), the
 * items' own positions, or one for all. An item for which run would fault
 * makes the whole run go again item by item through run, so a fold faults,
 * and a filter leaves an item out, exactly as they would have; the lanes
 * only see what run would compute. */
typedef struct {
    const double *lane; /* a value per item, or NULL */
    double value;       /* without lanes: the value for all */
    bool rows;          /* without lanes: the items' own positions */
    uint16_t owner;     /* an index's list */
} Lanes;

static bool uniform(const Lanes *v) { return !v->lane && !v->rows; }

/* A lookup by a value for all items reads no item of the run: every run of
 * a fold (or a filter) finds the same, so it is made once, by instruction. */
enum { MEMO = 64 };
typedef struct { double value[MEMO]; uint64_t known; } Memo;

static bool recalled(const Memo *memo, uint32_t slot, double *value) {
    if (slot >= MEMO || !(memo->known >> slot & 1)) return false;
    *value = memo->value[slot];
    return true;
}

static void remember(Memo *memo, uint32_t slot, double value) {
    if (slot >= MEMO) return;
    memo->value[slot] = value;
    memo->known |= (uint64_t)1 << slot;
}

/* A value per item, in scratch when it is not one already. */
static const double *lanes_of(const Lanes *v, double *scratch, const PureRun *items) {
    if (v->lane) return v->lane;
    for (unsigned i = 0; i < items->count; ++i) scratch[i] = v->rows ? (double)(items->start + i) : v->value;
    return scratch;
}

/* a op b as run computes it; false where run faults. */
static bool number_op(uint8_t op, double a, double b, double *out) {
    double value;
    switch (op) {
    case PURE_OP_ADD: value = a + b; break;
    case PURE_OP_SUB: value = a - b; break;
    case PURE_OP_MUL: value = a * b; break;
    case PURE_OP_DIV:
        if (b == 0) return false;
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
    case PURE_OP_MOD:
        if (b == 0) return false;
        value = fmod(a, b);
        if (value != 0 && (value < 0) != (b < 0)) value += b;
        else if (value == 0) value = copysign(0.0, b);
        break;
    default: value = maximum(a, b); break;
    }
    *out = value;
    return isfinite(value);
}

/* a op b for every item into out: a loop per operation, reading a value for
 * all with stride 0. false where run faults for some item. */
static bool lanes_op(uint8_t op, const double *a, size_t sa, const double *b, size_t sb, double *out, unsigned n) {
    bool bad = false;
#define EACH(expr) for (unsigned i = 0; i < n; ++i) { double x = a[i * sa], y = b[i * sb]; out[i] = (expr); }
    switch (op) {
    case PURE_OP_ADD: EACH(x + y) break;
    case PURE_OP_SUB: EACH(x - y) break;
    case PURE_OP_MUL: EACH(x * y) break;
    case PURE_OP_LT: EACH(x < y) return true;
    case PURE_OP_LE: EACH(x <= y) return true;
    case PURE_OP_GT: EACH(x > y) return true;
    case PURE_OP_GE: EACH(x >= y) return true;
    case PURE_OP_EQ: EACH(x == y) return true;
    case PURE_OP_NE: EACH(x != y) return true;
    case PURE_OP_AND: EACH(x != 0 && y != 0) return true;
    case PURE_OP_OR: EACH(x != 0 || y != 0) return true;
    case PURE_OP_MIN: EACH(minimum(x, y)) return true;
    case PURE_OP_MAX: EACH(maximum(x, y)) return true;
    default:
        for (unsigned i = 0; i < n; ++i) bad |= !number_op(op, a[i * sa], b[i * sb], &out[i]);
        return !bad;
    }
#undef EACH
    for (unsigned i = 0; i < n; ++i) bad |= !isfinite(out[i]);
    return !bad;
}

/* The block for the items of a run at once, as a column of their list:
 * result holds what it leaves (lanes may point into store, a row of lanes
 * per stack depth, or into the list's leaf). false when run would fault for
 * some item. */
static bool run_lanes(const PureModule *m, const double *values, PureBlock block, const Event *ev, const PureRun *items,
                      Lanes *result, double (*store)[PURE_LIST_RUN], Memo *memo) {
    PureApp *app = ev->app;
    Lanes stack[PURE_MODULE_STACK];
    double scratch[2][PURE_LIST_RUN];
    size_t sp = 0;
    unsigned n = items->count;
    for (uint32_t pc = block.start, end = block.start + block.length; pc < end; ++pc) {
        const PureOp *op = &m->code[pc];
        uint32_t arg = op->arg;
        Lanes *top = sp ? &stack[sp - 1] : stack;
        double *out = store[sp ? sp - 1 : 0]; /* where the top's new lanes go */
        switch (op->op) {
        case PURE_OP_CONST: stack[sp++] = (Lanes){NULL, op->value, false, 0}; continue;
        case PURE_OP_LOAD: stack[sp++] = (Lanes){NULL, values[arg], false, 0}; continue;
        case PURE_OP_INDEX_LOAD: stack[sp++] = (Lanes){NULL, values[arg], false, m->state_ref[arg]}; continue;
        case PURE_OP_ROW: stack[sp++] = (Lanes){NULL, 0, true, (uint16_t)arg}; continue;
        case PURE_OP_OUTER_ROW: stack[sp++] = (Lanes){NULL, (double)ev->outer, false, (uint16_t)arg}; continue;
        case PURE_OP_LIST_COUNT: stack[sp++] = (Lanes){NULL, (double)pure_list_count(app->lists[arg]), false, 0}; continue;
        case PURE_OP_INDEX_FIRST: case PURE_OP_INDEX_LAST: {
            size_t last = op->op == PURE_OP_INDEX_LAST ? pure_list_count(app->lists[arg]) : 0;
            stack[sp++] = (Lanes){NULL, last ? (double)(last - 1) : 0, false, (uint16_t)arg};
            continue;
        }
        case PURE_OP_INDEX_NUM: continue;
        case PURE_OP_PICK: stack[sp] = stack[sp - 1 - arg]; ++sp; continue;
        case PURE_OP_SLIDE: {
            /* The top moves down: lanes kept in a depth it leaves come along. */
            Lanes moved = stack[sp - 1];
            size_t to = sp - 1 - arg;
            if ((uintptr_t)moved.lane >= (uintptr_t)store[to + 1] && (uintptr_t)moved.lane < (uintptr_t)store[sp]) {
                memcpy(store[to], moved.lane, n * sizeof(double));
                moved.lane = store[to];
            }
            stack[to] = moved;
            sp = to + 1;
            continue;
        }
        case PURE_OP_ITEM_GET: {
            uint32_t list = list_of(arg), field = item_field(arg);
            const PureList *from = app->lists[list];
            size_t count = pure_list_count(from);
            if (top->rows) { /* the items themselves: the leaf's column */
                *top = (Lanes){items->column[field].numbers, 0, false, 0};
                continue;
            }
            if (!top->lane) {
                top->owner = 0;
                if (recalled(memo, pc - block.start, &top->value)) continue;
                if (top->value >= (double)count) return false;
                top->value = pure_list_get(from, (size_t)top->value, field).num;
                remember(memo, pc - block.start, top->value);
                continue;
            }
            for (unsigned i = 0; i < n; ++i) {
                if (top->lane[i] >= (double)count) return false;
                out[i] = pure_list_get(from, (size_t)top->lane[i], field).num;
            }
            *top = (Lanes){out, 0, false, 0};
            continue;
        }
        case PURE_OP_LIST_FIND: case PURE_OP_LIST_HAS: {
            const PureList *keyed = app->lists[arg];
            unsigned key = m->state_key[arg] - 1u;
            bool find = op->op == PURE_OP_LIST_FIND, found;
            if (uniform(top)) {
                double value;
                if (!recalled(memo, pc - block.start, &value)) {
                    size_t at = pure_list_seek(keyed, key, (PureCell){top->value, NULL, 0}, &found);
                    if (find && !found) return false;
                    remember(memo, pc - block.start, value = find ? (double)at : found);
                }
                *top = (Lanes){NULL, value, false, find ? (uint16_t)arg : 0};
                continue;
            }
            const double *keys = lanes_of(top, scratch[0], items);
            for (unsigned i = 0; i < n; ++i) {
                size_t at = pure_list_seek(keyed, key, (PureCell){keys[i], NULL, 0}, &found);
                if (find && !found) return false;
                out[i] = find ? (double)at : found;
            }
            *top = (Lanes){out, 0, false, find ? (uint16_t)arg : 0};
            continue;
        }
        case PURE_OP_INDEX_MOVE: {
            Lanes by = stack[--sp];
            top = &stack[sp - 1];
            out = store[sp - 1];
            size_t count = pure_list_count(app->lists[top->owner]);
            double last = count ? (double)(count - 1) : 0;
            if (uniform(top) && uniform(&by)) {
                if (by.value != floor(by.value)) return false;
                top->value = maximum(0, minimum(top->value + by.value, last));
                continue;
            }
            const double *at = lanes_of(top, scratch[0], items), *step = lanes_of(&by, scratch[1], items);
            for (unsigned i = 0; i < n; ++i) {
                if (step[i] != floor(step[i])) return false;
                out[i] = maximum(0, minimum(at[i] + step[i], last));
            }
            *top = (Lanes){out, 0, false, top->owner};
            continue;
        }
        case PURE_OP_NEG: case PURE_OP_NOT: case PURE_OP_FLOOR: {
            if (uniform(top)) {
                top->value = op->op == PURE_OP_NEG ? -top->value : op->op == PURE_OP_NOT ? top->value == 0 : floor(top->value);
                top->owner = 0;
                continue;
            }
            const double *a = lanes_of(top, scratch[0], items);
            if (op->op == PURE_OP_NEG) for (unsigned i = 0; i < n; ++i) out[i] = -a[i];
            else if (op->op == PURE_OP_NOT) for (unsigned i = 0; i < n; ++i) out[i] = a[i] == 0;
            else for (unsigned i = 0; i < n; ++i) out[i] = floor(a[i]);
            *top = (Lanes){out, 0, false, 0};
            continue;
        }
        case PURE_OP_CLAMP: {
            Lanes high = stack[--sp], low = stack[--sp];
            top = &stack[sp - 1];
            out = store[sp - 1];
            if (uniform(top) && uniform(&low) && uniform(&high)) {
                if (low.value > high.value) return false;
                top->value = maximum(low.value, minimum(top->value, high.value));
                continue;
            }
            double spare[PURE_LIST_RUN];
            const double *x = lanes_of(top, scratch[0], items), *lo = lanes_of(&low, scratch[1], items),
                         *hi = lanes_of(&high, spare, items);
            for (unsigned i = 0; i < n; ++i) {
                if (lo[i] > hi[i]) return false;
                out[i] = maximum(lo[i], minimum(x[i], hi[i]));
            }
            *top = (Lanes){out, 0, false, 0};
            continue;
        }
        default: { /* two numbers */
            Lanes b = stack[--sp];
            top = &stack[sp - 1];
            out = store[sp - 1];
            if (uniform(top) && uniform(&b)) {
                if (!number_op(op->op, top->value, b.value, &top->value)) return false;
                top->owner = 0;
                continue;
            }
            const double *x = top->rows ? lanes_of(top, scratch[0], items) : top->lane ? top->lane : &top->value;
            const double *y = b.rows ? lanes_of(&b, scratch[1], items) : b.lane ? b.lane : &b.value;
            if (!lanes_op(op->op, x, uniform(top) ? 0 : 1, y, uniform(&b) ? 0 : 1, out, n)) return false;
            *top = (Lanes){out, 0, false, 0};
            continue;
        }
        }
    }
    memcpy(result, stack, sp * sizeof(*stack));
    return true;
}

/* A fold's running value: what count, sum, max, min or first has so far. */
typedef struct { unsigned how; double total; bool any, done; } Fold;

/* One item: its value, and whether it counts (for count and first, the value is). */
static void fold_in(Fold *fold, size_t row, double value, double counts) {
    if (counts == 0) return;
    unsigned how = fold->how;
    if (how == PURE_FOLD_FIRST) {
        fold->total = (double)row;
        fold->any = fold->done = true;
        return;
    }
    fold->total = how == PURE_FOLD_COUNT ? fold->total + 1 : how == PURE_FOLD_SUM ? fold->total + value :
                  !fold->any ? value : how == PURE_FOLD_MAX ? maximum(fold->total, value) : minimum(fold->total, value);
    fold->any = true;
}

/* The fold's block for its items from first to last, one at a time. */
static PureResult fold_rows(const PureModule *m, double *values, PureBlock inner, Event *per, size_t first,
                            size_t last, Fold *fold) {
    bool one = fold->how == PURE_FOLD_COUNT || fold->how == PURE_FOLD_FIRST;
    for (size_t row = first; row < last && !fold->done; ++row) {
        Slot out[2];
        size_t mark = per->arena->used;
        per->row = row;
        PureResult status = run(m, values, inner, per, out);
        per->arena->used = mark;
        if (status != PURE_OK) return status;
        fold_in(fold, row, out[0].num, one ? out[0].num : out[1].num);
    }
    return PURE_OK;
}

/* The fold's block a leaf of its list at a time; a run where an item would
 * fault goes item by item. The items join the fold in order, so a sum adds
 * as it would one at a time. */
static PureResult fold_runs(const PureModule *m, double *values, PureBlock inner, const PureList *list, Event *per,
                            Fold *fold) {
    double store[PURE_MODULE_STACK][PURE_LIST_RUN], scratch[2][PURE_LIST_RUN];
    bool one = fold->how == PURE_FOLD_COUNT || fold->how == PURE_FOLD_FIRST;
    PureRun items;
    Memo memo;
    memo.known = 0;
    for (bool more = pure_list_run(list, 0, &items); more && !fold->done; more = pure_list_next(list, &items)) {
        Lanes out[2];
        if (!run_lanes(m, values, inner, per, &items, out, store, &memo)) {
            PureResult status = fold_rows(m, values, inner, per, items.start, items.start + items.count, fold);
            if (status != PURE_OK) return status;
            continue;
        }
        const Lanes *counts = &out[one ? 0 : 1];
        if (uniform(counts) && counts->value == 0) continue;
        const double *c = lanes_of(counts, scratch[0], &items), *v = lanes_of(&out[0], scratch[1], &items);
        if (fold->how == PURE_FOLD_COUNT) {
            size_t counted = 0;
            for (unsigned i = 0; i < items.count; ++i) counted += c[i] != 0;
            fold->total += (double)counted;
            fold->any = fold->any || counted;
        } else if (fold->how == PURE_FOLD_SUM) {
            double total = fold->total;
            bool any = fold->any;
            for (unsigned i = 0; i < items.count; ++i)
                if (c[i] != 0) total += v[i], any = true;
            fold->total = total;
            fold->any = any;
        } else
            for (unsigned i = 0; i < items.count && !fold->done; ++i) fold_in(fold, items.start + i, v[i], c[i]);
    }
    return PURE_OK;
}

static PureResult run(const PureModule *m, double *values, PureBlock block, Event *ev, Slot *result) {
    Slot stack[PURE_MODULE_STACK];
    size_t sp = 0;
    PureApp *app = ev->app;
    PureArena *arena = ev->arena;
    PureResult status;
    PureFault kind;
    const PureOp *op = NULL;
    for (uint32_t pc = block.start, end = block.start + block.length; pc < end;) {
        op = &m->code[pc++];
        uint32_t arg = op->arg;
        Slot *top = sp ? &stack[sp - 1] : stack;
        switch (op->op) {
        case PURE_OP_CONST: stack[sp++].num = op->value; continue;
        case PURE_OP_LOAD: stack[sp++].num = values[arg]; continue;
        case PURE_OP_DT: stack[sp++].num = ev->dt; continue;
        case PURE_OP_NOW: stack[sp++].num = app->now; continue;
        case PURE_OP_FLOOR: top->num = floor(top->num); continue;
        case PURE_OP_STORE: case PURE_OP_CUR_STORE: values[arg] = stack[--sp].num; continue;
        case PURE_OP_JZ: if (stack[--sp].num == 0) pc = arg; continue;
        case PURE_OP_JMP: pc = arg; continue;
        case PURE_OP_NEG: top->num = -top->num; continue;
        case PURE_OP_NOT: top->num = top->num == 0; continue;
        case PURE_OP_CLAMP: {
            double high = stack[--sp].num, low = stack[--sp].num;
            if (low > high) { kind = PURE_FAULT_CLAMP; goto failed; }
            stack[sp - 1].num = maximum(low, minimum(stack[sp - 1].num, high));
            continue;
        }
        case PURE_OP_FIELD: {
            const PureField *field = &m->field[ev->shape->field_start + arg];
            if (field->type == PURE_FIELD_U32) stack[sp++].num = read32(app->payload.data + field->offset);
            else if (field->type == PURE_FIELD_NUMBER) stack[sp++].num = read_double(app->payload.data + field->offset);
            else stack[sp++].bytes = (PureBytes){FROM_PAYLOAD, 0, {0, 0}, ev->spans[arg][0], ev->spans[arg][1]};
            continue;
        }
        case PURE_OP_DOC: case PURE_OP_DOC_SIZE: {
            if (op->op == PURE_OP_DOC && !ev->exists) { kind = PURE_FAULT_NOT_FOUND; goto failed; }
            PureVersion version = app->data.version(app->data.context, app->document);
            if (op->op == PURE_OP_DOC) stack[sp++].bytes = (PureBytes){FROM_DOCUMENT, 0, version, 0, version.bytes};
            else stack[sp++].num = (double)version.bytes;
            continue;
        }
        case PURE_OP_LEN: case PURE_OP_TEXT_BYTES: top->num = (double)top->bytes.length; continue;
        case PURE_OP_REPLACE: {
            PureBytes insert = stack[--sp].bytes;
            double last = stack[--sp].num, first = stack[--sp].num;
            double size = (double)app->data.version(app->data.context, app->document).bytes;
            if (first != floor(first) || last != floor(last) || first < 0 || last < first || last > size) {
                kind = PURE_FAULT_LIMIT;
                goto failed;
            }
            PureSpan span;
            size_t mark = arena->used;
            if ((status = contiguous(app, arena, insert, &span)) == PURE_OK)
                status = app->data.splice(app->data.context, app->document, (size_t)first, (size_t)(last - first), span);
            arena->used = mark;
            if (status != PURE_OK) goto refused;
            ev->modified = ev->exists = true;
            continue;
        }
        /* A reply outlives the arena mark of later statements: keep it by value. */
        case PURE_OP_REPLY: app->reply = stack[--sp].bytes; app->has_reply = true; continue;
        case PURE_OP_REQUIRE: if (stack[--sp].num == 0) { kind = PURE_FAULT_LIMIT; goto failed; } continue;
        case PURE_OP_TEXT_CONST:
            stack[sp++].bytes = (PureBytes){FROM_CONSTANT, 0, {0, 0}, m->constant_offset[arg], m->constant_length[arg]};
            continue;
        case PURE_OP_TEXT_LOAD: stack[sp++].bytes = text_of(app, arg); continue;
        case PURE_OP_TEXT_INPUT: stack[sp++].bytes = (PureBytes){FROM_PAYLOAD, 0, {0, 0}, 0, app->payload.size}; continue;
        case PURE_OP_NUM_TEXT: {
            char digits[32];
            if ((status = keep_bytes(arena, digits, pure_format_number(top->num, digits), &top->bytes)) != PURE_OK)
                return status;
            continue;
        }
        case PURE_OP_CONCAT: {
            PureBytes right = stack[--sp].bytes, left = stack[sp - 1].bytes;
            size_t start = arena->used;
            if ((status = append_value(app, arena, left)) != PURE_OK ||
                (status = append_value(app, arena, right)) != PURE_OK) return status;
            stack[sp - 1].bytes = (PureBytes){FROM_ARENA, 0, {0, 0}, start, left.length + right.length};
            continue;
        }
        case PURE_OP_CHARS: case PURE_OP_LINES: case PURE_OP_TEXT_FROM: {
            Measure count = {0, op->op == PURE_OP_LINES ? 1 : op->op == PURE_OP_TEXT_FROM ? 2 : 0, 0, true};
            if ((status = visit_value(app, arena, top->bytes, measure, &count)) != PURE_OK) return status;
            if (op->op != PURE_OP_TEXT_FROM) top->num = (double)(count.count + (op->op == PURE_OP_LINES));
            else if (!count.valid || !pure_utf8_complete(count.utf8)) { kind = PURE_FAULT_ENCODING; goto failed; }
            continue;
        }
        /* Positions: a cursor or an index, with the text or list it is in. */
        case PURE_OP_CUR_LOAD: case PURE_OP_INDEX_LOAD:
            stack[sp].num = values[arg];
            stack[sp++].bytes.owner = m->state_ref[arg];
            continue;
        case PURE_OP_CUR_START: case PURE_OP_CUR_END: case PURE_OP_INDEX_FIRST: case PURE_OP_INDEX_LAST:
        case PURE_OP_ROW: {
            size_t last = op->op == PURE_OP_CUR_END ? pure_draft_size(app->texts[arg]) :
                          op->op == PURE_OP_INDEX_LAST ? pure_list_count(app->lists[arg]) : 0;
            stack[sp].num = op->op == PURE_OP_ROW ? (double)ev->row :
                            op->op == PURE_OP_INDEX_LAST && last ? (double)(last - 1) : (double)last;
            stack[sp++].bytes.owner = (uint16_t)arg;
            continue;
        }
        case PURE_OP_OUTER_ROW:
            stack[sp].num = (double)ev->outer;
            stack[sp++].bytes.owner = (uint16_t)arg;
            continue;
        case PURE_OP_CUR_MOVE: case PURE_OP_INDEX_MOVE: {
            double count = stack[--sp].num;
            Slot *at = &stack[sp - 1];
            if (count != floor(count)) { kind = PURE_FAULT_LIMIT; goto failed; }
            if (op->op == PURE_OP_INDEX_MOVE) {
                size_t items = pure_list_count(app->lists[at->bytes.owner]);
                at->num = maximum(0, minimum(at->num + count, items ? (double)(items - 1) : 0));
                continue;
            }
            PureDraft *text = app->texts[at->bytes.owner];
            PureVersion version = pure_draft_version(text);
            at->num = (double)(arg == PURE_UNIT_CHAR ? pure_text_step(text, version, (size_t)at->num, count)
                                                     : pure_text_line_step(text, version, (size_t)at->num, count));
            continue;
        }
        case PURE_OP_CUR_COUNT: {
            PureDraft *text = app->texts[top->bytes.owner];
            top->num = (double)pure_text_count(text, pure_draft_version(text), (size_t)top->num, arg);
            continue;
        }
        case PURE_OP_TEXT_INSERT: case PURE_OP_TEXT_SET: {
            PureBytes insert = stack[--sp].bytes;
            size_t at = op->op == PURE_OP_TEXT_INSERT ? (size_t)stack[--sp].num : 0;
            size_t removed = op->op == PURE_OP_TEXT_SET ? pure_draft_size(app->texts[arg]) : 0;
            PureSpan span;
            size_t mark = arena->used;
            if ((status = contiguous(app, arena, insert, &span)) == PURE_OK)
                status = pure_draft_splice(app->texts[arg], at, removed, span);
            arena->used = mark;
            if (status != PURE_OK) goto refused;
            if (op->op == PURE_OP_TEXT_SET) {
                for (uint32_t c = 0; c < m->states; ++c)
                    if (m->state_type[c] == PURE_STATE_CURSOR && m->state_ref[c] == arg) values[c] = 0;
            } else move_cursors(m, values, arg, at, 0, span.size);
            continue;
        }
        case PURE_OP_TEXT_REMOVE: {
            double y = stack[--sp].num, x = stack[--sp].num;
            size_t first = (size_t)minimum(x, y), last = (size_t)maximum(x, y);
            if ((status = pure_draft_splice(app->texts[arg], first, last - first, (PureSpan){0})) != PURE_OK) goto refused;
            if (last > first) move_cursors(m, values, arg, first, last - first, 0);
            continue;
        }
        case PURE_OP_TEXT_LINE: {
            PureDraft *text = app->texts[arg];
            PureVersion version = pure_draft_version(text);
            size_t first = pure_text_line_start(text, version, (size_t)top->num);
            size_t last = pure_text_line_end(text, version, (size_t)top->num);
            top->bytes = (PureBytes){FROM_TEXT, (uint16_t)arg, version, first, last - first};
            continue;
        }
        case PURE_OP_BYTES_FROM: case PURE_OP_INDEX_NUM: continue; /* the same bytes or position, retyped */
        case PURE_OP_LIST_COUNT: stack[sp++].num = (double)pure_list_count(app->lists[arg]); continue;
        case PURE_OP_SEND: {
            const PureInputShape *output = &m->input[arg];
            size_t base = sp - output->field_count, size = output->prefix;
            sp = base;
            for (uint32_t f = 0; f < output->field_count; ++f) {
                uint8_t type = m->field[output->field_start + f].type;
                double number = stack[base + f].num;
                if (type == PURE_FIELD_BYTES || type == PURE_FIELD_TEXT) size += (size_t)stack[base + f].bytes.length;
                else if (type == PURE_FIELD_U32 && !(number >= 0 && number <= UINT32_MAX && number == floor(number))) {
                    kind = PURE_FAULT_LIMIT;
                    goto failed;
                }
            }
            if (size > UINT32_MAX) { kind = PURE_FAULT_LIMIT; goto failed; }
            if ((status = send(app, arg, stack + base, arena, size)) != PURE_OK) return status;
            continue;
        }
        case PURE_OP_LIST_APPEND: case PURE_OP_LIST_INSERT: {
            PureList *list = app->lists[arg];
            const PureRecord *record = &m->record[m->state_ref[arg]];
            size_t base = sp - record->count, count = pure_list_count(list), at = count;
            sp = base;
            if (op->op == PURE_OP_LIST_INSERT && (at = (size_t)stack[--sp].num) > count) {
                kind = PURE_FAULT_NOT_FOUND;
                goto failed;
            }
            /* Room for every text first, so no span moves while the next is copied. */
            size_t mark = arena->used, total = 0;
            for (unsigned f = 0; f < record->count; ++f)
                if (text_field(m, arg, f)) total += (size_t)stack[base + f].bytes.length;
            if (!reserve(arena, total)) return PURE_ALLOCATION;
            PureCell cells[PURE_RECORD_FIELDS];
            for (unsigned f = 0; f < record->count; ++f) {
                PureSpan span = {0};
                if (text_field(m, arg, f) && (status = contiguous(app, arena, stack[base + f].bytes, &span)) != PURE_OK)
                    return status;
                cells[f] = (PureCell){stack[base + f].num, (unsigned char *)span.data, span.size};
            }
            if (m->state_key[arg]) { /* a keyed list places the item by its key */
                bool found;
                unsigned key = m->state_key[arg] - 1u;
                at = pure_list_seek(list, key, cells[key], &found);
                if (found) { arena->used = mark; kind = PURE_FAULT_EXISTS; goto failed; }
            }
            status = pure_list_insert(list, at, cells, &app->undo);
            arena->used = mark;
            if (status != PURE_OK) goto refused;
            move_indices(m, values, arg, pure_list_count(list), at, true);
            continue;
        }
        case PURE_OP_LIST_REMOVE: {
            size_t at = (size_t)stack[--sp].num;
            if ((status = pure_list_remove(app->lists[arg], at, &app->undo)) != PURE_OK) goto refused;
            move_indices(m, values, arg, pure_list_count(app->lists[arg]), at, false);
            continue;
        }
        case PURE_OP_ITEM_GET: {
            uint32_t list = list_of(arg), field = item_field(arg);
            size_t at = (size_t)top->num;
            if (at >= pure_list_count(app->lists[list])) { kind = PURE_FAULT_NOT_FOUND; goto failed; }
            PureCell cell = pure_list_get(app->lists[list], at, field);
            /* A text is copied: the item may change or go before the value is used. */
            if (!text_field(m, list, field)) top->num = cell.num;
            else if ((status = keep_bytes(arena, cell.text, cell.size, &top->bytes)) != PURE_OK) return status;
            continue;
        }
        case PURE_OP_ITEM_SET: {
            uint32_t list = list_of(arg), field = item_field(arg);
            Slot value = stack[--sp];
            size_t at = (size_t)stack[--sp].num, mark = arena->used;
            PureSpan span = {0};
            status = text_field(m, list, field) ? contiguous(app, arena, value.bytes, &span) : PURE_OK;
            if (status == PURE_OK)
                status = pure_list_set(app->lists[list], at, field,
                                       (PureCell){value.num, (unsigned char *)span.data, span.size}, &app->undo);
            arena->used = mark;
            if (status != PURE_OK) goto refused;
            continue;
        }
        case PURE_OP_INDEX_STORE: {
            /* An index from before a remove may be past the end. */
            double position = stack[--sp].num;
            if (position && position >= (double)pure_list_count(app->lists[m->state_ref[arg]])) {
                kind = PURE_FAULT_NOT_FOUND;
                goto failed;
            }
            values[arg] = position;
            continue;
        }
        case PURE_OP_FOLD: {
            /* The block that follows runs once per item, as a column of the
             * list: a leaf of it at a time when planned so. */
            PureBlock inner = {pc, arg >> 16};
            PureList *list = app->lists[arg >> 8 & 0xFF];
            pc += inner.length;
            Fold fold = {arg & 0xFF, 0, false, false};
            Event per = *ev;
            per.outer = ev->row;
            status = op->plan ? fold_runs(m, values, inner, list, &per, &fold)
                              : fold_rows(m, values, inner, &per, 0, pure_list_count(list), &fold);
            if (status != PURE_OK) return status;
            if (!fold.any && fold.how >= PURE_FOLD_MAX) { kind = PURE_FAULT_NOT_FOUND; goto failed; }
            if (!isfinite(fold.total)) { kind = PURE_FAULT_NON_FINITE; goto failed; }
            stack[sp].num = fold.total;
            stack[sp++].bytes.owner = (uint16_t)(arg >> 8 & 0xFF); /* first: the list the index is in */
            continue;
        }
        case PURE_OP_TEXT_COMPARE: {
            /* Texts order by their bytes: a shorter text before a longer one it begins. */
            PureBytes right = stack[--sp].bytes, left = stack[sp - 1].bytes;
            size_t mark = arena->used;
            PureSpan a, b;
            if (!reserve(arena, (size_t)(left.length + right.length))) return PURE_ALLOCATION;
            if ((status = contiguous(app, arena, left, &a)) != PURE_OK ||
                (status = contiguous(app, arena, right, &b)) != PURE_OK) return status;
            size_t shorter = a.size < b.size ? a.size : b.size;
            int order = shorter ? memcmp(a.data, b.data, shorter) : 0;
            if (!order) order = (a.size > b.size) - (a.size < b.size);
            arena->used = mark;
            stack[sp - 1].num = arg == PURE_LESS ? order < 0 : arg == PURE_LESS_EQUAL ? order <= 0 :
                                arg == PURE_GREATER ? order > 0 : arg == PURE_GREATER_EQUAL ? order >= 0 :
                                arg == PURE_EQUAL ? order == 0 : order != 0;
            continue;
        }
        case PURE_OP_LIST_FIND: case PURE_OP_LIST_HAS: {
            /* A keyed list's item by key, found by halving: FIND gives its index, HAS whether it is there. */
            unsigned key = m->state_key[arg] - 1u;
            size_t mark = arena->used;
            PureCell cell = {top->num, NULL, 0};
            if (text_field(m, arg, key)) {
                PureSpan span;
                if ((status = contiguous(app, arena, top->bytes, &span)) != PURE_OK) return status;
                cell = (PureCell){0, (unsigned char *)span.data, span.size};
            }
            bool found;
            size_t at = pure_list_seek(app->lists[arg], key, cell, &found);
            arena->used = mark;
            if (op->op == PURE_OP_LIST_HAS) { top->num = found; continue; }
            if (!found) { kind = PURE_FAULT_NOT_FOUND; goto failed; }
            top->num = (double)at;
            top->bytes.owner = (uint16_t)arg;
            continue;
        }
        case PURE_OP_PICK: stack[sp] = stack[sp - 1 - arg]; ++sp; continue;
        case PURE_OP_SLIDE: stack[sp - 1 - arg] = stack[sp - 1]; sp -= arg; continue;
        default: break;
        }
        double b = stack[--sp].num, a = stack[sp - 1].num, value;
        switch (op->op) {
        case PURE_OP_ADD: value = a + b; break;
        case PURE_OP_SUB: value = a - b; break;
        case PURE_OP_MUL: value = a * b; break;
        case PURE_OP_DIV:
            if (b == 0) { kind = PURE_FAULT_DIVIDE; goto failed; }
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
        case PURE_OP_MOD: /* the divisor's sign, as floor division leaves it (a zero too) */
            if (b == 0) { kind = PURE_FAULT_DIVIDE; goto failed; }
            value = fmod(a, b);
            if (value != 0 && (value < 0) != (b < 0)) value += b;
            else if (value == 0) value = copysign(0.0, b);
            break;
        default: value = maximum(a, b); break;
        }
        if (!isfinite(value)) { kind = PURE_FAULT_NON_FINITE; goto failed; }
        stack[sp - 1].num = value;
    }
    if (result) memcpy(result, stack, sp * sizeof(*stack)); /* a view's value, or a fold block's numbers */
    return PURE_OK;
refused:
    kind = status == PURE_LIMIT ? PURE_FAULT_LIMIT : status == PURE_NOT_FOUND ? PURE_FAULT_NOT_FOUND : PURE_FAULT_NONE;
    if (kind == PURE_FAULT_NONE) return status;
failed:
    return fault(app, kind, (uint32_t)(op - m->code));
}

PURE_COLD static PureResult new_text(PureDraft **out, const PureModule *m, uint32_t constant) {
    *out = pure_draft_new();
    if (!*out) return PURE_ALLOCATION;
    PureSpan initial = {m->constant_data + m->constant_offset[constant], m->constant_length[constant]};
    PureResult result = pure_draft_splice(*out, 0, 0, initial);
    if (result != PURE_OK) { pure_draft_close(*out); *out = NULL; }
    return result;
}

PURE_COLD static PureList *new_list(const PureModule *m, uint32_t record) {
    return pure_list_new(m->record[record].count, m->record_field_text + m->record[record].first);
}

/* Where key is in the pending index: its slot, or the empty one it would take. */
static unsigned pending_slot(const PureApp *app, const char *key) {
    uint32_t hash = 2166136261u; /* FNV-1a */
    for (const char *c = key; *c; ++c) hash = (hash ^ (unsigned char)*c) * 16777619u;
    unsigned mask = 2 * PURE_APP_PENDING - 1, slot = hash & mask;
    while (app->pending_index[slot] && strcmp(app->pending[app->pending_index[slot] - 1].key, key)) slot = (slot + 1) & mask;
    return slot;
}

bool pure_app_pending(const PureApp *app, const char *key) {
    return app->pending_count && app->pending_index[pending_slot(app, key)];
}

void pure_app_finish(PureApp *app) {
    /* A document in the unsaved burst stays open until the save. */
    if (app->document && !app->document_pending) app->data.close(app->data.context, app->document);
    app->document_pending = false;
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
    if (app->pending_count) memset(app->pending_index, 0, sizeof(app->pending_index));
    app->pending_count = 0;
    app->document_pending = false;
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
        ++app->changes;
    }
    app->open = false;
}

static void drop_sent(PureApp *app, size_t count) {
    while (app->sent_count > count) free(app->sent[--app->sent_count].payload);
}

/* After a save: what it stored is the links' saved state (trimmed sends go),
 * or, when it failed, the links return to the last saved state. */
static void settle_links(PureApp *app, bool saved) {
    if (saved) {
        size_t kept = 0;
        for (size_t i = 0; i < app->sent_count; ++i) {
            if (app->sent[i].trimmed) free(app->sent[i].payload);
            else app->sent[kept++] = app->sent[i];
        }
        app->sent_count = app->sent_durable = kept;
        memcpy(app->saved_sent, app->last_sent, sizeof(app->saved_sent));
        for (unsigned i = 0; i < app->heard_count; ++i) app->heard[i].saved = app->heard[i].seq;
    } else {
        drop_sent(app, app->sent_durable);
        for (size_t i = 0; i < app->sent_count; ++i) app->sent[i].trimmed = false;
        memcpy(app->last_sent, app->saved_sent, sizeof(app->last_sent));
        unsigned kept = 0;
        for (unsigned i = 0; i < app->heard_count; ++i)
            if (app->heard[i].saved) {
                app->heard[kept] = app->heard[i];
                app->heard[kept++].seq = app->heard[i].saved;
            }
        app->heard_count = kept;
    }
    app->links_changed = false;
}

PureResult pure_app_save(PureApp *app) {
    void *documents[PURE_APP_PENDING];
    int64_t revisions[PURE_APP_PENDING];
    size_t count = 0;
    for (unsigned i = 0; i < app->pending_count; ++i)
        if (app->pending[i].edited) documents[count++] = app->pending[i].document;
    PureResult result = pure_save_commit(app, app->values, documents, count, revisions);
    settle_links(app, result == PURE_OK);
    close_burst(app, result == PURE_OK);
    return result;
}

bool pure_app_defer(PureApp *app, bool deferred) {
    if (deferred && !pure_save_enabled(app)) return false;
    app->deferred = deferred;
    return true;
}

/* ---- Snapshots. Snapshots of one app form a chain from oldest to newest,
 * each holding the next, so they go oldest first. List items the app lets go
 * while a snapshot is its newest may be read by that one and older ones, so
 * they go with it. */
struct PureSnapshot {
    unsigned refs;
    const PureModule *module;
    uint64_t changes;
    double values[PURE_MODULE_STATES];
    PureDraft *texts[PURE_MODULE_STATES];
    PureList *lists[PURE_MODULE_STATES];
    PureRetired retired;
    struct PureSnapshot *newer;
};

void pure_snapshot_hold(PureSnapshot *snapshot) { __atomic_add_fetch(&snapshot->refs, 1, __ATOMIC_RELAXED); }

void pure_snapshot_release(PureSnapshot *snapshot) {
    while (snapshot && !__atomic_sub_fetch(&snapshot->refs, 1, __ATOMIC_ACQ_REL)) {
        PureSnapshot *newer = snapshot->newer;
        for (uint32_t i = 0; i < PURE_MODULE_STATES; ++i) {
            pure_draft_close(snapshot->texts[i]);
            pure_list_free(snapshot->lists[i]);
        }
        pure_retired_free(&snapshot->retired);
        free(snapshot);
        snapshot = newer;
    }
}

void pure_app_stop(PureApp *app) {
    pure_app_finish(app);
    close_burst(app, true); /* unsaved changes are dropped with the app */
    for (uint32_t i = 0; i < PURE_MODULE_STATES && !app->reading; ++i) {
        pure_draft_close(app->texts[i]);
        pure_list_free(app->lists[i]);
        pure_draft_close(app->list_drafts[i]);
    }
    /* Items the newest snapshot may still read go when it goes. */
    if (app->snapshot) {
        app->snapshot->retired = app->retired;
        pure_snapshot_release(app->snapshot);
    } else
        pure_retired_free(&app->retired);
    pure_draft_close(app->stored_values);
    drop_sent(app, 0);
    free(app->sent);
    free(app->heard);
    free(app->carried);
    pure_draft_close(app->stored_links);
    free(app->pending);
    pure_undo_free(&app->undo);
    for (unsigned i = 0; i < app->row_tables; ++i) free(app->rows[i].items);
    free(app->rows);
    free(app->arena.bytes);
    free(app->view_arena.bytes);
    memset(app, 0, sizeof(*app));
}

/* Where one event began inside the burst: a failed event returns there. */
typedef struct {
    PureVersion texts[PURE_MODULE_STATES], lists[PURE_MODULE_STATES];
    size_t undo, sent;
    uint64_t last_sent[PURE_MODULE_INPUTS];
    bool opened;
} Mark;

static void mark_event(PureApp *app, Mark *mark) {
    mark->opened = !app->open;
    open_burst(app);
    for (uint32_t i = 0; i < app->module->states; ++i) {
        if (app->texts[i]) mark->texts[i] = pure_draft_version(app->texts[i]);
        if (app->list_drafts[i]) mark->lists[i] = pure_draft_version(app->list_drafts[i]);
    }
    mark->undo = app->undo.count;
    mark->sent = app->sent_count;
    memcpy(mark->last_sent, app->last_sent, sizeof(mark->last_sent));
}

static void undo_event(PureApp *app, const Mark *mark) {
    for (uint32_t i = 0; i < app->module->states; ++i) {
        if (app->texts[i]) pure_draft_restore(app->texts[i], mark->texts[i]);
        if (app->list_drafts[i]) pure_draft_restore(app->list_drafts[i], mark->lists[i]);
    }
    pure_undo_rollback(&app->undo, mark->undo);
    drop_sent(app, mark->sent);
    memcpy(app->last_sent, mark->last_sent, sizeof(app->last_sent));
    if (mark->opened) close_burst(app, true); /* nothing happened: release the holds */
}

/* The last number heard from source, or NULL. */
static struct PureHeard *heard_from(PureApp *app, const char *source) {
    for (unsigned i = 0; i < app->heard_count; ++i)
        if (!strcmp(app->heard[i].source, source)) return &app->heard[i];
    return NULL;
}

/* Hear seq from source (room for a new source was made before). */
static void hear(PureApp *app, const char *source, uint64_t seq) {
    struct PureHeard *heard = heard_from(app, source);
    if (!heard) {
        heard = &app->heard[app->heard_count++];
        *heard = (struct PureHeard){.saved = 0};
        memcpy(heard->source, source, strlen(source) + 1);
    }
    heard->seq = seq;
    app->links_changed = true;
}

static PureResult keep_reply(PureApp *app);

/* One event: every handler for it runs in module order against pending
 * states, all or nothing. A key, text or message event saves before it
 * returns unless saving is deferred; a frame leaves that to the next event
 * (frames come 60 times a second). A message's document joins the burst. */
static PureResult dispatch(PureApp *app, uint32_t event, Event *ev) {
    const PureModule *m = app->module;
    double pending[PURE_MODULE_STATES];
    Mark mark;
    mark_event(app, &mark);
    memcpy(pending, app->values, sizeof(app->values));
    app->fault = PURE_FAULT_NONE;
    app->fault_pc = UINT32_MAX;
    PureResult result = PURE_OK;
    for (uint32_t i = 0; i < m->handlers && result == PURE_OK; ++i)
        if (m->handler[i].event == event) result = run(m, pending, m->handler[i].code, ev, NULL);
    /* An edit needs the revision it was made against. A save rebases an
     * edited document and releasing texts may reclaim earlier text versions:
     * keep such a reply by copying it first. */
    if (result == PURE_OK && ev->modified && ev->base < 0) result = PURE_INVALID;
    if (result == PURE_OK && app->has_reply &&
        ((app->reply.source == FROM_DOCUMENT && ev->modified) || app->reply.source == FROM_TEXT))
        result = keep_reply(app);
    if (result != PURE_OK) {
        undo_event(app, &mark);
        return result;
    }
    memcpy(app->values, pending, sizeof(pending));
    ++app->events;
    ++app->changes;
    if (!event) ++app->ticks;
    if (app->document) {
        app->pending_index[pending_slot(app, ev->key)] = (uint16_t)(app->pending_count + 1); /* not there: a key joins once */
        app->pending[app->pending_count].document = app->document;
        app->pending[app->pending_count].edited = ev->modified;
        memcpy(app->pending[app->pending_count++].key, ev->key, strlen(ev->key) + 1);
        app->document_pending = true;
        app->revision += ev->modified; /* a save moves an edited document one revision */
    }
    if (app->delivering.pending) {
        hear(app, app->delivering.source, app->delivering.seq);
        app->delivering.pending = false;
    }
    return event && !app->deferred ? pure_app_save(app) : PURE_OK;
}

PureResult pure_app_frame(PureApp *app) {
    pure_app_finish(app);
    Event ev = {.app = app, .arena = &app->arena, .dt = 1.0 / 60.0};
    return dispatch(app, 0, &ev);
}

PureResult pure_app_tick(PureApp *app, uint32_t input) {
    if (input >= app->module->inputs || app->module->input[input].kind != PURE_KIND_TIMER) return PURE_INVALID;
    pure_app_finish(app);
    Event ev = {.app = app, .arena = &app->arena};
    return dispatch(app, 1 + 2 * input, &ev);
}

PureResult pure_app_input(PureApp *app, uint32_t input, PureEdge edge) {
    if (input >= app->module->inputs || app->module->input[input].kind != PURE_KIND_KEY ||
        (edge != PURE_EDGE_DOWN && edge != PURE_EDGE_UP)) return PURE_INVALID;
    pure_app_finish(app);
    Event ev = {.app = app, .arena = &app->arena};
    return dispatch(app, 1 + 2 * input + (edge == PURE_EDGE_UP), &ev);
}

PureResult pure_app_point(PureApp *app, uint32_t state, size_t position) {
    const PureModule *m = app->module;
    pure_app_finish(app);
    if (state >= m->states || m->state_type[state] != PURE_STATE_INDEX) return PURE_INVALID;
    if (position >= pure_list_count(app->lists[m->state_ref[state]])) return PURE_NOT_FOUND;
    open_burst(app); /* what a failed save returns to */
    app->fault = PURE_FAULT_NONE;
    app->fault_pc = UINT32_MAX;
    app->values[state] = (double)position;
    ++app->events;
    ++app->changes;
    return app->deferred ? PURE_OK : pure_app_save(app);
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

/* A message whose variable fields are where placed says (start and size in
 * the payload), or, without it, one after another after the prefix. */
static PureResult message(PureApp *app, uint32_t input, const char *key, int64_t base, PureSpan payload,
                          const uint32_t *placed) {
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
    /* Where each variable field is; a length past the payload, a text that
     * is not UTF-8 or a number that is not finite fails the message. */
    uint32_t spans[PURE_MODULE_FIELDS][2];
    size_t at = shape->prefix;
    for (uint32_t f = 0; f < shape->field_count; ++f) {
        const PureField *field = &m->field[shape->field_start + f];
        if (field->type == PURE_FIELD_NUMBER && !isfinite(read_double(payload.data + field->offset)))
            return fault(app, PURE_FAULT_NON_FINITE, UINT32_MAX);
        if (field->type != PURE_FIELD_BYTES && field->type != PURE_FIELD_TEXT) continue;
        size_t start = placed ? placed[2 * f] : at;
        if (start > payload.size) return fault(app, PURE_FAULT_LIMIT, UINT32_MAX);
        size_t size = placed ? placed[2 * f + 1] : field->offset == PURE_FIELD_REST ? payload.size - at :
                      read32(payload.data + field->offset);
        if (size > payload.size - start) return fault(app, PURE_FAULT_LIMIT, UINT32_MAX);
        if (field->type == PURE_FIELD_TEXT && !pure_utf8_valid((PureSpan){size ? payload.data + start : NULL, size}))
            return fault(app, PURE_FAULT_ENCODING, UINT32_MAX);
        spans[f][0] = (uint32_t)start;
        spans[f][1] = (uint32_t)size;
        at = start + size;
    }
    if (shape->keyed && m->collections) {
        if (!app->data.open) return PURE_INVALID;
        /* A key joins a burst once, so its revision moves once per save. */
        if (app->pending_count == PURE_APP_PENDING || pure_app_pending(app, key)) {
            PureResult saved = pure_app_save(app);
            if (saved != PURE_OK) return saved;
        }
        if (app->pending_count == app->pending_capacity) {
            unsigned capacity = app->pending_capacity ? 2 * app->pending_capacity : 16;
            struct PurePending *grown = realloc(app->pending, capacity * sizeof(*grown));
            if (!grown) return PURE_ALLOCATION;
            app->pending = grown;
            app->pending_capacity = capacity;
        }
        PureResult opened = app->data.open(app->data.context, 0, key, &app->document, &app->revision);
        if (opened != PURE_OK) { app->document = NULL; return opened; }
        if (base >= 0 && base != app->revision) { pure_app_finish(app); return PURE_CONFLICT; }
    }
    app->payload = payload;
    Event ev = {.app = app, .shape = shape, .key = key, .base = base, .arena = &app->arena, .exists = app->revision > 0,
                .spans = spans};
    PureResult result = dispatch(app, 1 + 2 * input, &ev);
    if (result != PURE_OK) pure_app_finish(app);
    return result;
}

PureResult pure_app_message(PureApp *app, uint32_t input, const char *key, int64_t base, PureSpan payload) {
    return message(app, input, key, base, payload, NULL);
}

PureResult pure_app_message_fields(PureApp *app, uint32_t input, const char *key, int64_t base, PureSpan payload,
                                   const uint32_t *placed) {
    return message(app, input, key, base, payload, placed);
}

void pure_app_outbox(const PureApp *app, PureSentVisit visit, void *context) {
    for (size_t i = 0; i < app->sent_durable; ++i) {
        const struct PureSent *sent = &app->sent[i];
        if (!sent->trimmed && !visit(context, sent->output, sent->seq, (PureSpan){sent->payload, sent->size})) return;
    }
}

void pure_app_outbox_newest(const PureApp *app, PureSentVisit visit, void *context) {
    for (size_t i = app->sent_durable; i-- > 0;) {
        const struct PureSent *sent = &app->sent[i];
        if (!sent->trimmed && !visit(context, sent->output, sent->seq, (PureSpan){sent->payload, sent->size})) return;
    }
}

void pure_app_trim(PureApp *app, uint32_t output, uint64_t through) {
    for (size_t i = 0; i < app->sent_durable; ++i)
        if (app->sent[i].output == output && app->sent[i].seq <= through && !app->sent[i].trimmed) {
            app->sent[i].trimmed = true;
            app->links_changed = true;
        }
}

PureResult pure_app_deliver(PureApp *app, uint32_t input, const char *source, uint64_t seq, PureSpan payload,
                            const uint32_t *placed) {
    const PureModule *m = app->module;
    size_t length = source ? strlen(source) : 0;
    app->duplicate = false;
    if (input >= m->inputs || m->input[input].kind != PURE_KIND_MESSAGE || m->input[input].keyed || !length ||
        length > PURE_SOURCE || !seq) return PURE_INVALID;
    for (size_t i = 0; i < length; ++i)
        if (source[i] <= ' ' || source[i] > '~') return PURE_INVALID;
    struct PureHeard *heard = heard_from(app, source);
    if (heard && seq <= heard->seq) {
        pure_app_finish(app);
        app->duplicate = true;
        return PURE_OK;
    }
    if (!heard && app->heard_count == app->heard_capacity) {
        unsigned capacity = app->heard_capacity ? 2 * app->heard_capacity : 4;
        struct PureHeard *grown = realloc(app->heard, capacity * sizeof(*grown));
        if (!grown) return PURE_ALLOCATION;
        app->heard = grown;
        app->heard_capacity = capacity;
    }
    app->delivering.source = source;
    app->delivering.seq = seq;
    app->delivering.pending = true;
    PureResult result = message(app, input, NULL, -1, payload, placed);
    if (!app->delivering.pending) return result;
    /* Not applied. Refused by the app (a fault), the message was delivered
     * all the same: its number is heard, saved like a key event's changes. */
    app->delivering.pending = false;
    if (app->fault == PURE_FAULT_NONE) return result;
    Mark mark;
    mark_event(app, &mark);
    hear(app, source, seq);
    PureResult saved = app->deferred ? PURE_OK : pure_app_save(app);
    return saved == PURE_OK ? result : saved;
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

static bool keep_row(struct PureRows *rows, size_t item) {
    if (rows->count == rows->capacity) {
        size_t capacity = rows->capacity ? 2 * rows->capacity : 16;
        size_t *grown = realloc(rows->items, capacity * sizeof(*grown));
        if (!grown) return false;
        rows->items = grown;
        rows->capacity = capacity;
    }
    rows->items[rows->count++] = item;
    return true;
}

/* The item kept when the filter, run for it alone, keeps it; false without room. */
static bool keep_evaluated(PureApp *app, uint32_t table, struct PureRows *rows, size_t item) {
    Slot kept;
    return evaluate(app, table, item, &kept) != PURE_OK || kept.num == 0 || keep_row(rows, item);
}

/* A filtered table's items, as fresh as the app's states: the filter runs
 * once per item after each change (an item it fails on is left out), not
 * once per cell. NULL for a table without a filter, or without room. */
static struct PureRows *filtered(PureApp *app, uint32_t table) {
    const PureModule *m = app->module;
    if (!m->view_code[table].length) return NULL;
    struct PureRows *rows = NULL;
    for (unsigned i = 0; i < app->row_tables && !rows; ++i) if (app->rows[i].table == table) rows = &app->rows[i];
    if (!rows) {
        struct PureRows *grown = realloc(app->rows, (app->row_tables + 1) * sizeof(*grown));
        if (!grown) return NULL;
        app->rows = grown;
        rows = &app->rows[app->row_tables++];
        *rows = (struct PureRows){.table = table, .changes = UINT64_MAX};
    }
    if (rows->changes == app->changes) return rows;
    rows->count = 0;
    /* A leaf of the list at a time when the filter has lane forms; a leaf
     * where an item would fault goes item by item, leaving that item out. */
    const PureList *list = app->lists[m->view_list[table]];
    PureRun run;
    if (m->view_plan[table]) {
        double store[PURE_MODULE_STACK][PURE_LIST_RUN], scratch[PURE_LIST_RUN];
        Event ev = {.app = app, .arena = &app->view_arena};
        Memo memo;
        memo.known = 0;
        for (bool more = pure_list_run(list, 0, &run); more; more = pure_list_next(list, &run)) {
            Lanes keep;
            if (run_lanes(m, app->values, m->view_code[table], &ev, &run, &keep, store, &memo)) {
                const double *kept = lanes_of(&keep, scratch, &run);
                for (unsigned i = 0; i < run.count; ++i)
                    if (kept[i] != 0 && !keep_row(rows, run.start + i)) return NULL;
            } else
                for (size_t item = run.start; item < run.start + run.count; ++item)
                    if (!keep_evaluated(app, table, rows, item)) return NULL;
        }
    } else
        for (size_t item = 0, items = pure_list_count(list); item < items; ++item)
            if (!keep_evaluated(app, table, rows, item)) return NULL;
    rows->changes = app->changes;
    return rows;
}

bool pure_app_row_item(PureApp *app, uint32_t table, size_t row, size_t *item) {
    const PureModule *m = app->module;
    if (table >= m->views || m->view_type[table] != PURE_VIEW_TABLE) return false;
    if (!m->view_code[table].length) {
        *item = row;
        return row < pure_list_count(app->lists[m->view_list[table]]);
    }
    struct PureRows *rows = filtered(app, table);
    if (!rows || row >= rows->count) return false;
    *item = rows->items[row];
    return true;
}

bool pure_app_item_row(PureApp *app, uint32_t table, size_t item, size_t *row) {
    const PureModule *m = app->module;
    if (table >= m->views || m->view_type[table] != PURE_VIEW_TABLE) return false;
    if (!m->view_code[table].length) {
        *row = item;
        return item < pure_list_count(app->lists[m->view_list[table]]);
    }
    struct PureRows *rows = filtered(app, table);
    size_t low = 0, high = rows ? rows->count : 0; /* the items are in order */
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        if (rows->items[middle] < item) low = middle + 1;
        else high = middle;
    }
    if (low == (rows ? rows->count : 0) || rows->items[low] != item) return false;
    *row = low;
    return true;
}

/* A view outside a table (column false), or a column and a row of its table. */
static PureResult view_of(PureApp *app, uint32_t view, bool column, size_t row, uint8_t type, Slot *slot) {
    const PureModule *m = app->module;
    if (view >= m->views || m->view_type[view] != type || (m->view_table[view] != 0) != column) return PURE_INVALID;
    if (column && !pure_app_row_item(app, m->view_table[view] - 1u, row, &row)) return PURE_NOT_FOUND;
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

size_t pure_app_rows(PureApp *app, uint32_t table) {
    const PureModule *m = app->module;
    if (table >= m->views || m->view_type[table] != PURE_VIEW_TABLE) return 0;
    struct PureRows *rows = filtered(app, table);
    return rows ? rows->count : m->view_code[table].length ? 0 : pure_list_count(app->lists[m->view_list[table]]);
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

PureSnapshot *pure_app_snapshot(PureApp *app) {
    if (app->open || app->reading) return NULL;
    const PureModule *m = app->module;
    PureSnapshot *made = calloc(1, sizeof(*made)), *last = app->snapshot;
    if (!made) return NULL;
    made->refs = 1;
    made->module = m;
    made->changes = app->changes;
    memcpy(made->values, app->values, sizeof(made->values));
    bool ok = true;
    for (uint32_t i = 0; i < m->states && ok; ++i) {
        if (app->texts[i]) ok = (made->texts[i] = pure_draft_freeze(app->texts[i])) != NULL;
        if (!ok || !app->lists[i]) continue;
        if (last && last->lists[i] && pure_list_current(last->lists[i], app->lists[i])) {
            pure_list_hold(last->lists[i]);
            made->lists[i] = last->lists[i];
        } else
            ok = (made->lists[i] = pure_list_share(app->lists[i], &app->retired)) != NULL;
    }
    if (!ok) {
        pure_snapshot_release(made);
        return NULL;
    }
    made->refs = last ? 3 : 2; /* the app's, the caller's and the last one's */
    if (last) {
        last->retired = app->retired;
        last->newer = made;
        pure_snapshot_release(last);
    } else
        pure_retired_free(&app->retired); /* shared only by a snapshot never made */
    app->retired = (PureRetired){0};
    app->snapshot = made;
    return made;
}

void pure_app_read(PureApp *reader, const PureSnapshot *snapshot) {
    reader->module = snapshot->module;
    reader->reading = true;
    reader->changes = snapshot->changes; /* what the row cache is fresh for */
    memcpy(reader->values, snapshot->values, sizeof(reader->values));
    memcpy(reader->texts, snapshot->texts, sizeof(reader->texts));
    memcpy(reader->lists, snapshot->lists, sizeof(reader->lists));
}

/* A list with record's fields: each takes the field of `from` with the same
 * name and kind, or its default. */
PURE_COLD PureResult pure_list_migrate(PureList **out, const PureList *from, const char *const *names,
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
PURE_COLD static PureResult keep_list(PureList **out, PureList *from, const PureModule *old, uint32_t old_record,
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

/* Open a module in an app. States keep their values by name and kind (texts
 * their content, cursors their position in it, lists their items, moved by
 * field name when the record changed); every other state starts from its
 * stored value, or its initial one. A list whose record changed is stored
 * again, and the next event or save publishes the module's states; a state
 * the module drops stays stored as it was. Starting an app opens its module
 * over none. */
/* Outputs of old that module has by name and with the same fields keep their
 * sends and numbers; one it lacks or changed refuses the module while sends
 * are left on it (CONFLICT), and otherwise keeps its number for later. */
PURE_COLD static PureResult remap_outputs(PureApp *app, const PureModule *old, const PureModule *module, bool apply) {
    int map[PURE_MODULE_INPUTS];
    for (uint32_t i = 0; i < old->inputs; ++i) {
        map[i] = -1;
        if (old->input[i].kind != PURE_KIND_OUTPUT) continue;
        int to = pure_module_input(module, old->input_names[i]);
        bool same = to >= 0 && module->input[to].kind == PURE_KIND_OUTPUT &&
                    module->input[to].field_count == old->input[i].field_count;
        for (uint32_t f = 0; same && f < old->input[i].field_count; ++f)
            same = module->field[module->input[to].field_start + f].type == old->field[old->input[i].field_start + f].type;
        map[i] = same ? to : -1;
        for (size_t s = 0; !same && s < app->sent_count; ++s)
            if (app->sent[s].output == i) return PURE_CONFLICT;
    }
    if (!apply) return PURE_OK;
    uint64_t last[PURE_MODULE_INPUTS] = {0}, saved[PURE_MODULE_INPUTS] = {0};
    for (uint32_t i = 0; i < old->inputs; ++i) {
        if (old->input[i].kind != PURE_KIND_OUTPUT || !app->last_sent[i]) continue;
        if (map[i] >= 0) {
            last[map[i]] = app->last_sent[i];
            saved[map[i]] = app->saved_sent[i];
            continue;
        }
        struct PureCarried *grown = realloc(app->carried, (app->carried_count + 1) * sizeof(*grown));
        if (!grown) continue; /* its numbers would start again: kept only while memory allows */
        app->carried = grown;
        memcpy(grown[app->carried_count].name, old->input_names[i], PURE_MODULE_NAME);
        grown[app->carried_count++].last = app->last_sent[i];
        app->links_changed = true;
    }
    for (unsigned c = 0; c < app->carried_count;) {
        int to = pure_module_input(module, app->carried[c].name);
        if (to < 0 || module->input[to].kind != PURE_KIND_OUTPUT || last[to]) { ++c; continue; }
        last[to] = saved[to] = app->carried[c].last;
        app->carried[c] = app->carried[--app->carried_count];
        app->links_changed = true;
    }
    for (size_t s = 0; s < app->sent_count; ++s) app->sent[s].output = (uint32_t)map[app->sent[s].output];
    memcpy(app->last_sent, last, sizeof(last));
    memcpy(app->saved_sent, saved, sizeof(saved));
    return PURE_OK;
}

PURE_COLD static PureResult open_module(PureApp *app, const PureModule *module) {
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
        int previous = old ? pure_module_state(old, module->state_names[i]) : -1;
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
    /* A keyed list keeps its items in key order: one that was not keyed, or
     * keyed by another field, is put in order and its indices follow their
     * items. Two items with one key refuse the module: nothing is dropped. */
    size_t *moved[PURE_MODULE_STATES] = {0};
    for (uint32_t i = 0; i < module->states && result == PURE_OK; ++i) {
        if (module->state_type[i] != PURE_STATE_LIST || !module->state_key[i]) continue;
        size_t count = pure_list_count(lists[i]);
        PureList *sorted = NULL;
        if (count && !(moved[i] = malloc(count * sizeof(**moved)))) result = PURE_ALLOCATION;
        else result = pure_list_sorted(lists[i], module->state_key[i] - 1u, &sorted, moved[i]);
        if (!sorted) {
            free(moved[i]);
            moved[i] = NULL;
            continue;
        }
        if (kept[i] < 0 || lists[i] != app->lists[kept[i]]) pure_list_free(lists[i]);
        lists[i] = sorted;
        rewrite[i] = list_drafts[i] != NULL;
    }
    /* A cursor or index keeps its position only while its text or list is the
     * one it belonged to. */
    for (uint32_t i = 0; i < module->states; ++i) {
        if (module->state_type[i] != PURE_STATE_CURSOR && module->state_type[i] != PURE_STATE_INDEX) continue;
        uint32_t owner = module->state_ref[i];
        bool same = kept[i] >= 0 && kept[owner] >= 0 && old->state_ref[kept[i]] == (unsigned)kept[owner];
        values[i] = same ? app->values[kept[i]] : 0;
    }
    if (result == PURE_OK && stored) result = pure_save_open_values(app, module, values, kept, texts, lists);
    /* Outputs keep their sends and numbers by name. */
    if (result == PURE_OK) result = old ? remap_outputs(app, old, module, false) : stored ? pure_save_open_links(app, module) : PURE_OK;
    for (uint32_t i = 0; i < module->states; ++i) {
        uint32_t list = module->state_ref[i];
        if (module->state_type[i] == PURE_STATE_INDEX && moved[list] && values[i] < (double)pure_list_count(lists[list]))
            values[i] = (double)moved[list][(size_t)values[i]];
    }
    for (uint32_t i = 0; i < module->states; ++i) free(moved[i]);
    /* Store the lists whose record or order changed last, so that a failure
     * can put every draft back as it was. */
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
    for (uint32_t i = 0; old && i < old->states; ++i) {
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
    if (old) remap_outputs(app, old, module, true);
    app->module = module;
    ++app->changes;
    return PURE_OK;
}

PURE_COLD PureResult pure_app_start(PureApp *app, const PureModule *module, PureData data) {
    *app = (PureApp){.data = data, .fault_pc = UINT32_MAX};
    PureResult result = open_module(app, module);
    if (result == PURE_OK) result = pure_app_save(app);
    if (result != PURE_OK) pure_app_stop(app);
    return result;
}

PURE_COLD PureResult pure_app_replace(PureApp *app, const PureModule *module) {
    pure_app_finish(app);
    if (app->open || app->pending_count) {
        PureResult saved = pure_app_save(app); /* the burst belongs to the module that made it */
        if (saved != PURE_OK) return saved;
    }
    return open_module(app, module);
}
