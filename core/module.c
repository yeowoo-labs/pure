#include "module.h"
#include "module-run.h"
#include "module-save.h"
#include "log.h"
#include "module-text.h"
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { FUNCTION_BYTES = 12, SIGNATURE_BYTES = 4 };
enum { STATE_BYTES = 16, INPUT_BYTES = 16, FIELD_BYTES = 8,
       CONSTANT_BYTES = 8, RECORD_BYTES = 8, RECORD_FIELD_BYTES = 16, VIEW_BYTES = 20,
       HANDLER_BYTES = 12, OP_BYTES = 16 };
/* T_FIELD and T_ITEM stand for the type of the message or record field
 * named, T_KEY for the type of the named keyed list's key, T_LOCAL for what
 * the named local holds, T_STATE for the type of the number, bool or i64
 * state named. */
enum { T_NUM = 1, T_BYTES = 2, T_TEXT = 3, T_CURSOR = 4, T_FIELD = 5, T_INDEX = 6, T_ITEM = 7, T_KEY = 8, T_LOCAL = 9,
       T_BOOL = 10, T_STATE = 11, T_I64 = 12, T_LIST = 13 }; /* T_LIST: a list value, its owner the record of its items */
enum { FROM_PAYLOAD, FROM_COPY, FROM_CONSTANT, FROM_TEXT, FROM_ARENA, FROM_ARGUMENTS, FROM_HELD };
/* Where an opcode may appear. */
/* IN_ARGUMENTS: fields read where nothing is done with them (a view's arguments, a message's fields in a fold). */
/* IN_LOCALS: a handler's locals, read in its walks and the folds in them. */
/* IN_COMPUTE: what a view's body may do too (018): loops, locals, and changes to an event's lists alone. */
/* IN_FUNCTION: a function's code (027's 2c), a handler's but for a message. */
enum { IN_VIEW = 1, IN_HANDLER = 2, IN_MESSAGE = 4, IN_ROW = 32, IN_OUTER = 64, IN_ARGUMENTS = 128,
       IN_LOCALS = 256, IN_COMPUTE = 512, IN_FUNCTION = 1024 };
enum { READ = IN_VIEW | IN_HANDLER };
/* What an argument names: nothing (it is 0), a state of one type (A_STATE +
 * the type), a list and one of its record's fields, a message field, a
 * constant, a unit, the column's row, a branch target, a stack
 * depth, a fold, a relation, a keyed list, an output, the row of the column
 * a fold runs in, a number, bool or i64 state (A_VALUE), or the value's type
 * (A_TYPE: 0 a number, 1 a bool, 2 an i64), how I64_BITS computes (A_BITS),
 * what a position is (A_POSITION: 0 a number, 1 an i64), what CONCAT joins
 * (A_JOINED: 0 texts, 1 bytes), what TEXT_COMPARE relates (A_ORDER: a relation,
 * and PURE_COMPARE_BYTES for two bytes), how NUM_TEXT writes (A_FORMAT) or the value
 * held by name (A_HELD), or a function (A_FUNCTION: CALL's, and RETURN's own). */
enum { A_NONE, A_STATE, A_ITEM = A_STATE + PURE_STATE_I64 + 1, A_FIELD, A_CONSTANT, A_IN_TEXT, A_IN_LIST,
       A_ROW, A_TARGET, A_DEPTH, A_FOLD, A_RELATION, A_KEYED, A_OUTPUT, A_OUTER, A_EACH, A_LOCAL, A_VALUE, A_TYPE,
       A_LOOP, A_BITS, A_POSITION, A_JOINED, A_FORMAT, A_HELD, A_FUNCTION, A_ORDER };
/* A fold's block is verified as a column that leaves one bool (count, first)
 * or a value and whether to include it (a bool): a number (FOLD_TWO) or an
 * i64 (FOLD_I64). A table's filter is verified as a column that leaves a
 * bool: these stand for a view type. A walk's block (EACH) is verified as
 * its handler's code that may read the item it runs for (WALK). In a view's
 * body a loop's block leaves nothing either (STEPS), and a walk's reads its
 * item (STEPS_WALK); neither changes a state. A function's block begins with
 * its parameters on the stack and ends at RETURN alone (FUNCTION); the loops'
 * blocks in it are a handler's code. */
enum { FOLD_ONE = 100, FOLD_TWO, FOLD_I64, FILTER, WALK, STEPS, STEPS_WALK, FUNCTION, LIST_OF = 200 }; /* LIST_OF + r: a list of r's */
/* The text or list an opcode works on: its cursor and index operands belong
 * to it, and so does a cursor or index it pushes. O_ANY: no rule. O_ARG: the
 * argument. O_REF: the text or list of the argument's cursor or index state.
 * O_LIST: the argument's bits 0-15 (an item argument's list, an anchor
 * instruction's text or list). */
enum { O_ANY, O_ARG, O_REF, O_LIST };
static const unsigned char magic[8] = {'P','U','R','E','M','O','D','7'};

/* Operand types popped (bottom to top), the type pushed, where allowed, what
 * the argument names and the text or list worked on. */
#define NUMS(pops) {pops, {T_NUM, T_NUM, T_NUM}, T_NUM, READ, A_NONE, O_ANY}
#define COMPARE {2, {T_NUM, T_NUM}, T_BOOL, READ, A_NONE, O_ANY}
#define LOGIC(pops) {pops, {T_BOOL, T_BOOL}, T_BOOL, READ, A_NONE, O_ANY}
#define I64S(pops) {pops, {T_I64, T_I64}, T_I64, READ, A_NONE, O_ANY}
#define STATE(type) A_STATE + PURE_STATE_##type
static const struct { unsigned char pops, in[3], out; unsigned short where; unsigned char arg, of; } signature[PURE_OP_KEPT_GET] = {
    [PURE_OP_CONST] = {0, {0}, T_NUM, READ, A_TYPE, O_ANY}, [PURE_OP_ADD] = NUMS(2), [PURE_OP_SUB] = NUMS(2),
    [PURE_OP_MUL] = NUMS(2), [PURE_OP_DIV] = NUMS(2), [PURE_OP_NEG] = NUMS(1), [PURE_OP_LT] = COMPARE,
    [PURE_OP_LE] = COMPARE, [PURE_OP_GT] = COMPARE, [PURE_OP_GE] = COMPARE, [PURE_OP_EQ] = COMPARE,
    [PURE_OP_NE] = COMPARE, [PURE_OP_AND] = LOGIC(2), [PURE_OP_OR] = LOGIC(2), [PURE_OP_NOT] = LOGIC(1),
    [PURE_OP_MIN] = NUMS(2), [PURE_OP_MAX] = NUMS(2), [PURE_OP_CLAMP] = NUMS(3),
    [PURE_OP_LOAD] = {0, {0}, T_STATE, READ, A_VALUE, O_ANY},
    [PURE_OP_STORE] = {1, {T_STATE}, 0, IN_HANDLER, A_VALUE, O_ANY},
    [PURE_OP_JZ] = {1, {T_BOOL}, 0, READ, A_TARGET, O_ANY},
    [PURE_OP_JMP] = {0, {0}, 0, READ, A_TARGET, O_ANY},
    [PURE_OP_FIELD] = {0, {0}, T_FIELD, IN_MESSAGE | IN_ARGUMENTS, A_FIELD, O_ANY},
    [PURE_OP_LEN] = {1, {T_BYTES}, T_NUM, READ, A_NONE, O_ANY},
    [PURE_OP_REPLY] = {1, {T_BYTES}, 0, IN_MESSAGE, A_NONE, O_ANY},
    [PURE_OP_REQUIRE] = {1, {T_BOOL}, 0, IN_HANDLER, A_NONE, O_ANY},
    [PURE_OP_TEXT_CONST] = {0, {0}, T_TEXT, READ, A_CONSTANT, O_ANY},
    [PURE_OP_TEXT_LOAD] = {0, {0}, T_TEXT, READ, STATE(TEXT), O_ANY},
    [PURE_OP_NUM_TEXT] = {1, {T_NUM}, T_TEXT, READ, A_FORMAT, O_ANY},
    [PURE_OP_CONCAT] = {2, {T_TEXT, T_TEXT}, T_TEXT, READ, A_JOINED, O_ANY},
    [PURE_OP_CHARS] = {1, {T_TEXT}, T_NUM, READ, A_NONE, O_ANY},
    [PURE_OP_LINES] = {1, {T_TEXT}, T_NUM, READ, A_NONE, O_ANY},
    [PURE_OP_TEXT_BYTES] = {1, {T_TEXT}, T_NUM, READ, A_NONE, O_ANY},
    [PURE_OP_CUR_LOAD] = {0, {0}, T_CURSOR, READ, STATE(CURSOR), O_REF},
    [PURE_OP_CUR_START] = {0, {0}, T_CURSOR, READ, STATE(TEXT), O_ARG},
    [PURE_OP_CUR_END] = {0, {0}, T_CURSOR, READ, STATE(TEXT), O_ARG},
    [PURE_OP_CUR_MOVE] = {2, {T_CURSOR, T_NUM}, T_CURSOR, READ, A_IN_TEXT, O_LIST},
    [PURE_OP_CUR_STORE] = {1, {T_CURSOR}, 0, IN_HANDLER, STATE(CURSOR), O_REF},
    [PURE_OP_TEXT_INSERT] = {2, {T_CURSOR, T_TEXT}, 0, IN_HANDLER, STATE(TEXT), O_ARG},
    [PURE_OP_TEXT_REMOVE] = {2, {T_CURSOR, T_CURSOR}, 0, IN_HANDLER, STATE(TEXT), O_ARG},
    [PURE_OP_TEXT_LINE] = {1, {T_CURSOR}, T_TEXT, READ, STATE(TEXT), O_ARG},
    [PURE_OP_TEXT_SET] = {1, {T_TEXT}, 0, IN_HANDLER, STATE(TEXT), O_ANY},
    [PURE_OP_TEXT_FROM] = {1, {T_BYTES}, T_TEXT, READ, A_NONE, O_ANY},
    [PURE_OP_BYTES_FROM] = {1, {T_TEXT}, T_BYTES, READ, A_NONE, O_ANY},
    [PURE_OP_CUR_COUNT] = {1, {T_CURSOR}, T_NUM, READ, A_IN_TEXT, O_LIST},
    [PURE_OP_CUR_AT] = {1, {T_NUM}, T_CURSOR, READ, A_IN_TEXT, O_LIST},
    /* Record operations also take as many values as the list's record has fields. */
    [PURE_OP_LIST_COUNT] = {0, {0}, T_NUM, READ, STATE(LIST), O_ANY},
    [PURE_OP_LIST_APPEND] = {0, {0}, 0, IN_HANDLER | IN_COMPUTE, STATE(LIST), O_ANY},
    [PURE_OP_LIST_INSERT] = {1, {T_INDEX}, 0, IN_HANDLER | IN_COMPUTE, STATE(LIST), O_ARG},
    [PURE_OP_LIST_REMOVE] = {1, {T_INDEX}, 0, IN_HANDLER | IN_COMPUTE, STATE(LIST), O_ARG},
    [PURE_OP_ITEM_GET] = {1, {T_INDEX}, T_ITEM, READ, A_ITEM, O_LIST},
    [PURE_OP_ITEM_SET] = {1, {T_INDEX}, 0, IN_HANDLER | IN_COMPUTE, A_ITEM, O_LIST},
    [PURE_OP_INDEX_LOAD] = {0, {0}, T_INDEX, READ, STATE(INDEX), O_REF},
    [PURE_OP_INDEX_STORE] = {1, {T_INDEX}, 0, IN_HANDLER, STATE(INDEX), O_REF},
    [PURE_OP_INDEX_MOVE] = {2, {T_INDEX, T_NUM}, T_INDEX, READ, A_IN_LIST, O_LIST},
    [PURE_OP_INDEX_FIRST] = {0, {0}, T_INDEX, READ, STATE(LIST), O_ARG},
    [PURE_OP_INDEX_LAST] = {0, {0}, T_INDEX, READ, STATE(LIST), O_ARG},
    [PURE_OP_INDEX_NUM] = {1, {T_INDEX}, T_NUM, READ, A_IN_LIST, O_LIST},
    [PURE_OP_ROW] = {0, {0}, T_INDEX, IN_ROW, A_ROW, O_ARG},
    /* In a fold that runs in a column, the column's row (the fold's own item is ROW's). */
    [PURE_OP_OUTER_ROW] = {0, {0}, T_INDEX, IN_OUTER, A_OUTER, O_ARG},
    /* Stack copies: their types come from the values they move. */
    [PURE_OP_PICK] = {0, {0}, 0, READ, A_DEPTH, O_ANY},
    [PURE_OP_SLIDE] = {0, {0}, 0, READ, A_DEPTH, O_ANY},
    [PURE_OP_FOLD] = {0, {0}, T_NUM, READ, A_FOLD, O_ANY},
    [PURE_OP_TEXT_COMPARE] = {2, {T_TEXT, T_TEXT}, T_BOOL, READ, A_ORDER, O_ANY},
    [PURE_OP_LIST_FIND] = {1, {T_KEY}, T_INDEX, READ, A_KEYED, O_ARG},
    [PURE_OP_LIST_HAS] = {1, {T_KEY}, T_BOOL, READ, A_KEYED, O_ANY},
    [PURE_OP_NOW] = {0, {0}, T_NUM, IN_HANDLER, A_NONE, O_ANY},
    [PURE_OP_SEND] = {0, {0}, 0, IN_HANDLER, A_OUTPUT, O_ANY},
    [PURE_OP_EACH] = {0, {0}, 0, IN_HANDLER | IN_COMPUTE, A_EACH, O_ANY},
    [PURE_OP_LOCAL_LOAD] = {0, {0}, T_LOCAL, IN_LOCALS, A_LOCAL, O_ANY},
    [PURE_OP_LOCAL_STORE] = {1, {T_LOCAL}, 0, IN_HANDLER | IN_COMPUTE, A_LOCAL, O_ANY},
    /* LOOP takes its low and high, two numbers or two i64s (the operands are its argument's). */
    [PURE_OP_LOOP] = {0, {0}, 0, IN_HANDLER | IN_COMPUTE, A_LOOP, O_ANY},
    [PURE_OP_BREAK] = {0, {0}, 0, IN_HANDLER | IN_COMPUTE, A_NONE, O_ANY},
    [PURE_OP_FLOOR] = NUMS(1), [PURE_OP_MOD] = NUMS(2), [PURE_OP_FLOORDIV] = NUMS(2),
    [PURE_OP_I64_ADD] = I64S(2), [PURE_OP_I64_SUB] = I64S(2), [PURE_OP_I64_MUL] = I64S(2), [PURE_OP_I64_DIV] = I64S(2),
    [PURE_OP_I64_MOD] = I64S(2), [PURE_OP_I64_NEG] = I64S(1), [PURE_OP_I64_MIN] = I64S(2), [PURE_OP_I64_MAX] = I64S(2),
    [PURE_OP_I64_COMPARE] = {2, {T_I64, T_I64}, T_BOOL, READ, A_RELATION, O_ANY},
    [PURE_OP_I64_FROM] = {1, {T_NUM}, T_I64, READ, A_NONE, O_ANY},
    [PURE_OP_NUM_FROM] = {1, {T_I64}, T_NUM, READ, A_NONE, O_ANY},
    [PURE_OP_I64_BITS] = {2, {T_I64, T_I64}, T_I64, READ, A_BITS, O_ANY},
    [PURE_OP_BYTE_AT] = {2, {T_BYTES, T_NUM}, T_I64, READ, A_POSITION, O_ANY},
    [PURE_OP_BYTES_OF] = {0, {0}, T_BYTES, READ, STATE(LIST), O_ANY},
    [PURE_OP_INDEX_AT] = {1, {T_NUM}, T_INDEX, READ, STATE(LIST), O_ARG},
    [PURE_OP_BYTES_SLICE] = {3, {T_BYTES, T_NUM, T_NUM}, T_BYTES, READ, A_POSITION, O_ANY},
    [PURE_OP_BYTE] = {1, {T_I64}, T_BYTES, READ, A_NONE, O_ANY},
    [PURE_OP_BYTES_LOAD] = {0, {0}, T_BYTES, IN_LOCALS, A_HELD, O_ANY},
    [PURE_OP_BYTES_STORE] = {1, {T_BYTES}, 0, IN_HANDLER | IN_COMPUTE, A_HELD, O_ANY},
    [PURE_OP_LIST_VALUE] = {0, {0}, T_LIST, IN_HANDLER | IN_COMPUTE, STATE(LIST), O_ANY},
    [PURE_OP_LIST_BIND] = {1, {T_LIST}, 0, IN_HANDLER | IN_COMPUTE, STATE(LIST), O_ANY},
    /* APPEND_IN also takes as many values as the record of the list in the field has fields. */
    [PURE_OP_APPEND_IN] = {1, {T_INDEX}, 0, IN_HANDLER | IN_COMPUTE, A_ITEM, O_LIST},
    [PURE_OP_NUM_PARSE] = {1, {T_TEXT}, T_NUM, READ, A_NONE, O_ANY},
    /* CALL takes its function's parameters and gives its results; RETURN takes its function's results (verify_code). */
    [PURE_OP_CALL] = {0, {0}, 0, IN_HANDLER, A_FUNCTION, O_ANY},
    [PURE_OP_RETURN] = {0, {0}, 0, IN_FUNCTION, A_FUNCTION, O_ANY},
    [PURE_OP_CHAR_AT] = {2, {T_TEXT, T_I64}, T_I64, READ, A_NONE, O_ANY},
    [PURE_OP_CHAR] = {1, {T_I64}, T_TEXT, READ, A_NONE, O_ANY},
};
#undef NUMS
#undef COMPARE
#undef LOGIC
#undef I64S
#undef STATE

static uint32_t read16(const unsigned char *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }


static void write32(unsigned char *p, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) p[i] = (unsigned char)(value >> (8 * i));
}


static void write_double(unsigned char *p, double value) {
    uint64_t bits;
    memcpy(&bits, &value, sizeof(bits));
    write32(p, (uint32_t)bits);
    write32(p + 4, (uint32_t)(bits >> 32));
}

static int64_t read_i64(const unsigned char *p) { return (int64_t)(read32(p) | (uint64_t)read32(p + 4) << 32); }

static void write_i64(unsigned char *p, int64_t value) {
    write32(p, (uint32_t)(uint64_t)value);
    write32(p + 4, (uint32_t)((uint64_t)value >> 32));
}

const char *pure_fault_name(PureFault fault) {
    static const char *const names[PURE_FAULT_COUNT] = {"none", "divide", "non_finite", "clamp", "limit", "not_found",
                                                        "encoding", "exists", "memory", "overflow", "work"};
    return fault < PURE_FAULT_COUNT ? names[fault] : "none";
}

/* A bool's value: 0 (not -0) or 1. */
static bool canonical_bool(double value) { return value == 1 || (value == 0 && !signbit(value)); }

static bool variable_field(uint8_t type) {
    return type == PURE_FIELD_BYTES || type == PURE_FIELD_TEXT || type == PURE_FIELD_ROWS;
}

uint32_t pure_fields_lay_out(PureField *fields, uint32_t count) {
    uint32_t last = UINT32_MAX, prefix = 0;
    for (uint32_t f = 0; f < count; ++f)
        if (variable_field(fields[f].type)) last = f;
    for (uint32_t f = 0; f < count; ++f) {
        fields[f].offset = f == last ? PURE_FIELD_REST : prefix;
        if (f != last) prefix += pure_field_size(fields[f].type);
    }
    return prefix;
}

/* A field's value in a payload: a number (a u32's, a bool's) or an i64, or bytes (of the payload). */
typedef struct { union { double num; int64_t i64; }; PureSpan bytes; } FieldValue;

/* Each field's value in payload, laid out by fields (their offsets set):
 * false when the payload is not so laid out. */
static bool read_fields(const PureField *fields, uint32_t count, PureSpan payload, FieldValue *values) {
    uint32_t prefix = 0;
    for (uint32_t f = 0; f < count; ++f)
        if (fields[f].offset != PURE_FIELD_REST) prefix += pure_field_size(fields[f].type);
    if (payload.size < prefix) return false;
    size_t at = prefix;
    for (uint32_t f = 0; f < count; ++f) {
        const PureField *field = &fields[f];
        values[f] = (FieldValue){0};
        if (field->type == PURE_FIELD_U32) values[f].num = read32(payload.data + field->offset);
        else if (field->type == PURE_FIELD_NUMBER) values[f].num = read_double(payload.data + field->offset);
        else if (field->type == PURE_FIELD_I64) values[f].i64 = read_i64(payload.data + field->offset);
        else if (field->type == PURE_FIELD_BOOL) {
            if (payload.data[field->offset] > 1) return false;
            values[f].num = payload.data[field->offset];
        } else if (field->type == PURE_FIELD_BYTES || field->type == PURE_FIELD_TEXT) {
            size_t size = field->offset == PURE_FIELD_REST ? payload.size - at : read32(payload.data + field->offset);
            if (size > payload.size - at) return false;
            values[f].bytes = (PureSpan){payload.data + at, size};
            at += size;
        } else return false; /* rows: an output sends none */
    }
    return at == payload.size;
}

/* A fixed field's value as a list cell's kind (a u32 is a number). */
static uint8_t field_kind(uint8_t type) {
    return type == PURE_FIELD_BOOL ? PURE_CELL_BOOL : type == PURE_FIELD_I64 ? PURE_CELL_INTEGER : PURE_CELL_NUMBER;
}

PureResult pure_send_recast(const PureField *from, uint32_t from_count, const PureField *to, uint32_t to_count,
                            PureSpan payload, unsigned char **out, size_t *size) {
    FieldValue was[PURE_MESSAGE_FIELDS], now[PURE_MESSAGE_FIELDS];
    *out = NULL;
    *size = 0;
    if (from_count > PURE_MESSAGE_FIELDS || to_count > PURE_MESSAGE_FIELDS || !read_fields(from, from_count, payload, was))
        return PURE_STORE_ERROR;
    size_t total = 0;
    for (uint32_t g = 0; g < to_count; ++g) {
        const PureField *field = &to[g];
        now[g] = (FieldValue){0};
        uint32_t f = 0;
        while (f < from_count && strcmp(from[f].name, field->name)) ++f;
        bool value = pure_field_fixed(field->type);
        if (f < from_count && value && pure_field_fixed(from[f].type)) {
            /* A number, a u32, a bool or an i64: the value when the field can
             * hold it (pure_cell_as; a u32 a whole number in its range), as a
             * state's is kept; else 0. */
            PureCell cell = {.i64 = was[f].i64};
            bool fits = pure_cell_as(field_kind(from[f].type), field_kind(field->type), &cell) &&
                        (field->type != PURE_FIELD_U32 || (cell.num >= 0 && cell.num <= UINT32_MAX && cell.num == floor(cell.num)));
            now[g].i64 = fits ? cell.i64 : 0;
        } else if (f < from_count && !value && (from[f].type == PURE_FIELD_BYTES || from[f].type == PURE_FIELD_TEXT)) {
            bool fits = field->type == PURE_FIELD_BYTES || from[f].type == PURE_FIELD_TEXT || pure_utf8_valid(was[f].bytes);
            if (fits) now[g].bytes = was[f].bytes;
        }
        total += field->offset == PURE_FIELD_REST || value ? 0 : 4;
        total += value ? pure_field_size(field->type) : now[g].bytes.size;
    }
    unsigned char *made = malloc(total ? total : 1);
    if (!made) return PURE_ALLOCATION;
    uint32_t prefix = 0;
    for (uint32_t g = 0; g < to_count; ++g)
        if (to[g].offset != PURE_FIELD_REST) prefix += pure_field_size(to[g].type);
    size_t at = prefix;
    for (uint32_t g = 0; g < to_count; ++g) {
        const PureField *field = &to[g];
        unsigned char *place = field->offset == PURE_FIELD_REST ? made + at : made + field->offset;
        if (field->type == PURE_FIELD_U32) write32(place, (uint32_t)now[g].num);
        else if (field->type == PURE_FIELD_NUMBER) write_double(place, now[g].num);
        else if (field->type == PURE_FIELD_I64) write_i64(place, now[g].i64);
        else if (field->type == PURE_FIELD_BOOL) *place = (unsigned char)now[g].num;
        else {
            if (field->offset != PURE_FIELD_REST) write32(place, (uint32_t)now[g].bytes.size);
            if (now[g].bytes.size) memcpy(made + at, now[g].bytes.data, now[g].bytes.size);
            at += now[g].bytes.size;
        }
    }
    *out = made;
    *size = at;
    return PURE_OK;
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

/* The verifier's arrival: the message input a handler handles or a view's arguments come in, as its number + 1 (0:
 * none, a function's or a view's without arguments). */
static int event_input(uint32_t event) { return event ? (int)event - 1 : -1; }

/* The state an argument names must have this type. */
static bool state_of(const PureModule *m, uint32_t state, uint8_t type) {
    return state < m->states && m->state_type[state] == type;
}

/* The list an item operation names: its low 16 bits (the field is above). */

/* The stack type of a record field's type (PureRecordType), and of a record field (the module's numbering). */
static unsigned char record_type_of(uint8_t type) {
    static const unsigned char types[] = {[PURE_RECORD_NUMBER] = T_NUM, [PURE_RECORD_TEXT] = T_TEXT, [PURE_RECORD_BOOL] = T_BOOL,
                                          [PURE_RECORD_I64] = T_I64, [PURE_RECORD_LIST] = T_LIST, [PURE_RECORD_BYTES] = T_BYTES};
    return types[type];
}
static unsigned char field_type(const PureModule *m, uint32_t list, uint32_t field) {
    return record_type_of(m->record_field_type[m->record[m->state_ref[list]].first + field]);
}

/* The record of the lists a LIST field of a record holds (field: the module's numbering), or UINT32_MAX. */
static uint32_t nested_record(const PureModule *m, uint32_t field) {
    return m->record_field_type[field] == PURE_RECORD_LIST ? (uint32_t)m->record_field_default[field].i64 : UINT32_MAX;
}

/* A list view (not a list column), and one over a list state (not computed: its rows that state's items) (027's 4e). */
static bool list_view(const PureModule *m, uint32_t view) { return m->view_type[view] == PURE_VIEW_LIST && !m->view_table[view]; }
static bool over_state(const PureModule *m, uint32_t view) { return list_view(m, view) && !m->view_computed[view]; }

/* A list kept past an event: stored or transient, not an event's (027's stage 3). */
static bool kept_list(const PureModule *m, uint32_t list) { return !(m->state_flags[list] & PURE_LIST_ROWS >> 8); }
/* The steps of a kept list's lists, apart from the instructions' own (027's stage 3): the load gives ITEM_GET and
 * ITEM_SET of a kept list's list field, and LIST_APPEND, LIST_INSERT and APPEND_IN of a kept list whose record holds
 * lists, the runtime's own instructions (PURE_OP_KEPT_*, module-run.h), so the others' steps stay as they were. */

/* An event's list, and one bound by LIST_BIND (a register: never changed by code). */
static bool event_list_state(const PureModule *m, uint32_t list) {
    return list < m->states && m->state_type[list] == PURE_STATE_LIST && m->state_flags[list] & PURE_LIST_ROWS >> 8;
}
static bool bound_list(const PureModule *m, uint32_t list) {
    return event_list_state(m, list) && m->state_flags[list] & PURE_LIST_BOUND >> 8;
}

static unsigned char record_type(const PureModule *m, uint32_t field) { return record_type_of(m->record_field_type[field]); }

/* The stack type of a function's signature item (a record field's type). */
static unsigned char signature_stack_type(const PureModule *m, uint32_t item) { return record_type_of(m->signature_type[item]); }

/* The record of the list value an instruction takes as its operand k (bottom 0). */
static unsigned list_wanted(const PureModule *m, const PureOp *op, unsigned k) {
    uint32_t arg = op->arg;
    switch (op->op) {
    case PURE_OP_CALL: return m->signature_record[m->function[arg].first + k];
    case PURE_OP_LOCAL_STORE: return arg >> 16;
    case PURE_OP_RETURN: return m->signature_record[m->function[arg].first + m->function[arg].params + k];
    case PURE_OP_LIST_BIND: return m->state_ref[arg];
    case PURE_OP_LIST_APPEND: return nested_record(m, m->record[m->state_ref[arg]].first + k);
    case PURE_OP_LIST_INSERT: return nested_record(m, m->record[m->state_ref[arg]].first + k - 1); /* the index first */
    case PURE_OP_ITEM_SET: return nested_record(m, m->record[m->state_ref[list_of(arg)]].first + item_field(arg));
    case PURE_OP_APPEND_IN:
        return nested_record(m, m->record[nested_record(m, m->record[m->state_ref[list_of(arg)]].first + item_field(arg))].first + k - 1);
    default: return UINT32_MAX;
    }
}

/* The stack type of a message field's value (a u32 is a number). */
static unsigned char message_type(uint8_t type) {
    return type == PURE_FIELD_TEXT ? T_TEXT : type == PURE_FIELD_BYTES ? T_BYTES : type == PURE_FIELD_BOOL ? T_BOOL :
           type == PURE_FIELD_I64 ? T_I64 : T_NUM;
}

/* The stack type of a number, bool or i64 state's value. */
static unsigned char state_type(const PureModule *m, uint32_t state) {
    return m->state_type[state] == PURE_STATE_BOOL ? T_BOOL : m->state_type[state] == PURE_STATE_I64 ? T_I64 : T_NUM;
}

/* Whether a list's record field is held as bytes are: a text or bytes. */
static bool text_field(const PureModule *m, uint32_t list, uint32_t field) {
    return pure_record_bytes(m->record_field_type[m->record[m->state_ref[list]].first + field]);
}

/* Whether its cells are bytes: a text, bytes, or a list in a list kept past an event (027's stage 3). */
static bool bytes_field(const PureModule *m, uint32_t list, uint32_t field) {
    uint8_t type = m->record_field_type[m->record[m->state_ref[list]].first + field];
    return pure_record_bytes(type) || (type == PURE_RECORD_LIST && !(m->state_flags[list] & PURE_LIST_ROWS >> 8));
}

/* What a local holds (its argument's bits 8-15): a number, an index, a bool, an i64 or a list (027's 2c). */
static unsigned char local_type(uint32_t arg) {
    static const unsigned char types[] = {T_NUM, T_INDEX, T_BOOL, T_I64, T_LIST};
    return (arg >> 8 & 0xFF) <= 4 ? types[arg >> 8 & 0xFF] : T_NUM;
}

PURE_COLD static bool names_arg(const PureModule *m, unsigned kind, uint32_t arg, const PureInputShape *shape, int row,
                                int outer) {
    switch (kind) {
    case A_ITEM:
        return state_of(m, list_of(arg), PURE_STATE_LIST) && item_field(arg) < m->record[m->state_ref[list_of(arg)]].count;
    case A_FIELD: return arg < shape->field_count && m->field[shape->field_start + arg].type != PURE_FIELD_ROWS;
    case A_CONSTANT: return arg < m->constants;
    case A_IN_TEXT: /* 027's 4c: the text an anchor is in, and a unit */
        return state_of(m, arg & 0xFFFF, PURE_STATE_TEXT) && arg >> PURE_ANCHOR_UNIT <= PURE_UNIT_LINE;
    case A_IN_LIST: return state_of(m, arg, PURE_STATE_LIST); /* the list, its unit an item (0) */
    case A_ROW: return (int)arg == row;
    case A_OUTER: return outer >= 0 && (int)arg == outer;
    case A_FOLD: case A_EACH: { /* a slice's list is keyed by a number */
        uint32_t list = arg >> 8 & 0xFF, flags = arg & 0xFF, how = flags & PURE_FOLD_KIND;
        bool sliced = kind == A_FOLD ? flags & PURE_FOLD_SLICED : flags & PURE_EACH_SLICED;
        /* An i64's fold sums, keeps the most or least or counts the different values. */
        bool valued = how != PURE_FOLD_COUNT && how != PURE_FOLD_FIRST;
        unsigned char key = state_of(m, list, PURE_STATE_LIST) && m->state_key[list] ?
                            field_type(m, list, m->state_key[list] - 1u) : 0;
        return (kind == A_FOLD ? how <= PURE_FOLD_DISTINCT && (valued || !(flags & PURE_FOLD_INTEGER)) : flags <= PURE_EACH_SLICED) &&
               state_of(m, list, PURE_STATE_LIST) && arg >> 16 && (!sliced || key == T_NUM || key == T_I64);
    }
    case A_LOCAL: /* a slot, and a number, an index of a list, a bool, an i64 or a list of a record */
        return (arg & 0xFF) < PURE_MODULE_LOCALS && (arg >> 8 & 0xFF) <= 4 &&
               ((arg >> 8 & 0xFF) == 1 ? state_of(m, arg >> 16, PURE_STATE_LIST) :
                (arg >> 8 & 0xFF) == 4 ? (arg >> 16) < m->records : !(arg >> 16));
    case A_VALUE: return state_of(m, arg, PURE_STATE_NUM) || state_of(m, arg, PURE_STATE_BOOL) || state_of(m, arg, PURE_STATE_I64);
    case A_LOOP: return (arg & 0xFF) < PURE_MODULE_LOCALS && !(arg & 0xFE00); /* a slot, and whether i64s */
    case A_TYPE: return arg <= 2;
    case A_BITS: return arg <= PURE_BITS_RIGHT;
    case A_POSITION: case A_JOINED: return arg <= 1;
    case A_HELD: return (arg & ~(uint32_t)PURE_HELD_APPEND) < PURE_MODULE_HELD;
    case A_FUNCTION: return arg < m->functions;
    case A_FORMAT: { /* a type; a width, zeros, a form fitting it and digits for FIXED and DECIMAL alone */
        uint32_t type = arg & 3, form = arg >> PURE_FORMAT_FORM & 7, digits = arg >> PURE_FORMAT_DIGITS & 31;
        return type <= 2 && !(arg & 0xFC) && !(arg >> 25) && form <= PURE_FORMAT_FIXED && (type != 1 || !form) &&
               (form != PURE_FORMAT_FIXED || type == 0) && (form == PURE_FORMAT_FIXED || form == PURE_FORMAT_DECIMAL || !digits);
    }
    case A_RELATION: return arg <= PURE_NOT_EQUAL;
    case A_ORDER: return (arg & ~PURE_COMPARE_BYTES) <= PURE_NOT_EQUAL; /* a relation, of texts or of bytes */
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
                                          bool locals, unsigned nest, uint32_t *walked, unsigned walks, int fn, uint32_t *at);

/* Whether a walk among walks walks list. */
static bool walking(const uint32_t *walked, unsigned walks, uint32_t list) {
    for (unsigned k = 0; k < walks; ++k)
        if (walked[k] == list) return true;
    return false;
}

/* target marks, per instruction of the block, a branch into it with an empty stack. */
/* outer: the row of the column a fold's block runs in (-1: none). */
/* locals: the block is a handler's, or runs in one (a walk, a fold): it reads the handler's locals. */
/* nest: the loops (LOOP, EACH) the block runs in; walked: the lists the walks among them walk, walks of them (its room
 * the module's code: a walk's list goes after them for the walk's block). */
/* fn: the function whose code the block is (its own block, FUNCTION, or a loop's in it), or -1. */
PURE_COLD static const char *verify_code(const PureModule *m, PureBlock block, int view, int row, int outer,
                                         uint32_t event, bool locals, unsigned nest, uint32_t *walked, unsigned walks, int fn,
                                         uint32_t *at, unsigned char *target) {
#define REFUSE(why) do { *at = pc; return why; } while (0)
    bool steps = view == STEPS || view == STEPS_WALK, handler = view < 0 || view == WALK || steps || view == FUNCTION;
    bool folding = view == FOLD_ONE || view == FOLD_TWO || view == FOLD_I64;
    /* A view outside a table computes (locals: its code reads them): a body, and the loops in it. */
    bool body = steps || (!handler && !folding && view != FILTER && row < 0 && locals);
    if (!handler && !block.length) return "empty view";
    /* A handler's message or text; a view's arguments, or the message of the handler a fold is in. */
    int input = event_input(event);
    const PureInputShape *shape = input >= 0 ? &m->input[input] : NULL;
    unsigned where = steps ? IN_VIEW | IN_COMPUTE | (view == STEPS_WALK ? IN_ROW : 0)
                   : !handler ? IN_VIEW | (row >= 0 ? IN_ROW : 0) | (folding && outer >= 0 ? IN_OUTER : 0) | (body ? IN_COMPUTE : 0)
                              : IN_HANDLER | (view == WALK ? IN_ROW : 0);
    if (shape && handler && !steps && shape->kind == PURE_KIND_MESSAGE) where |= IN_MESSAGE;
    if (shape && (!handler || steps) && (shape->kind == PURE_KIND_MESSAGE || shape->kind == PURE_KIND_ARGUMENTS))
        where |= IN_ARGUMENTS;
    if (locals) where |= IN_LOCALS;
    if (fn >= 0) where |= IN_FUNCTION;
    unsigned char types[PURE_MODULE_STACK], owners[PURE_MODULE_STACK];
    Join joins[PURE_MODULE_JOINS];
    unsigned open = 0;
    int height = 0;
    bool reachable = true;
    if (view == FUNCTION) /* its parameters */
        for (const PureFunction *f = &m->function[fn]; height < f->params; ++height) {
            types[height] = signature_stack_type(m, f->first + (uint32_t)height);
            owners[height] = (unsigned char)(types[height] == T_LIST ? m->signature_record[f->first + height] : 0);
        }
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
        if (m->height) m->height[pc] = (uint8_t)height; /* the compiled code's slots (021) */
        const PureOp *op = &m->code[pc];
        if (op->op >= PURE_OP_KEPT_GET) REFUSE("unknown opcode"); /* (a module's own were refused as it loaded) */
        unsigned allowed = signature[op->op].where, kind = signature[op->op].arg, of = signature[op->op].of;
        if (!(allowed & where)) {
            if (allowed & IN_FUNCTION) REFUSE("return outside a function");
            if (op->op == PURE_OP_CALL) REFUSE("call outside a handler or a function");
            if (allowed & IN_ROW) REFUSE("row outside a column");
            if (allowed & IN_OUTER) REFUSE("outer row outside a fold in a column");
            if (allowed & IN_LOCALS) REFUSE("local outside a handler");
            if (view == FILTER) REFUSE("filter with an effect");
            if (folding) REFUSE("fold with an effect");
            if (!handler || steps) REFUSE("view with an effect");
            REFUSE("message operation outside a message handler");
        }
        uint32_t arg = op->arg;
        /* A constant: a finite number, a bool +0 or 1, any i64 (arg says which). */
        bool constant = op->op == PURE_OP_CONST, truth = constant && arg == 1, integer = constant && arg == 2;
        if ((kind == A_NONE && arg) ||
            (truth ? !canonical_bool(op->value) : integer ? false : constant ? !isfinite(op->value) : op->integer != 0))
            REFUSE("non-canonical operand");
        if (!names_arg(m, kind, arg, shape, row, outer)) REFUSE("operand names no such state, field, constant or unit");
        /* A bytes state (028) is read whole and set whole: no anchor is in it, so no op of a text's anchors takes it. */
        if ((kind == A_STATE + PURE_STATE_TEXT || kind == A_IN_TEXT) && op->op != PURE_OP_TEXT_LOAD && op->op != PURE_OP_TEXT_SET &&
            m->state_flags[arg & 0xFFFF] & PURE_TEXT_BYTES)
            REFUSE("anchor in bytes");
        /* A keyed list places its items by key, and an item's key stays. */
        if (op->op == PURE_OP_LIST_INSERT && m->state_key[arg]) REFUSE("insert by position into a keyed list");
        /* A walk ends: no list a walk walks grows while it runs, and only the innermost walk's may shrink (it moves with it). */
        if ((op->op == PURE_OP_LIST_APPEND || op->op == PURE_OP_LIST_INSERT) && walking(walked, walks, arg))
            REFUSE("insert into the list walked");
        if (op->op == PURE_OP_LIST_REMOVE && walking(walked, walks, arg) && (int)arg != row)
            REFUSE("remove from a list an outer walk walks");
        if (nest && op->op == PURE_OP_REPLY) REFUSE("reply inside a loop");
        if (op->op == PURE_OP_BREAK && !nest) REFUSE("break outside a loop");
        if (op->op == PURE_OP_BREAK && height) REFUSE("break with values on the stack");
        if (op->op == PURE_OP_ITEM_SET && m->state_key[list_of(arg)] == item_field(arg) + 1)
            REFUSE("change of a keyed list's key");
        if (op->op == PURE_OP_BYTES_OF && (m->record[m->state_ref[arg]].count != 1 || field_type(m, arg, 0) != T_I64))
            REFUSE("bytes of a list not of one i64");
        /* Lists in records (027's 2b): a register is read alone; values are of an event's lists; APPEND_IN appends to a
         * list in a field of an event's list. */
        if (((op->op == PURE_OP_LIST_APPEND || op->op == PURE_OP_LIST_INSERT || op->op == PURE_OP_LIST_REMOVE) && bound_list(m, arg)) ||
            ((op->op == PURE_OP_ITEM_SET || op->op == PURE_OP_APPEND_IN) && bound_list(m, list_of(arg))))
            REFUSE("change of a bound list");
        if (op->op == PURE_OP_LIST_VALUE && !event_list_state(m, arg)) REFUSE("value of a list not an event's");
        if (op->op == PURE_OP_LIST_BIND && !bound_list(m, arg)) REFUSE("list bound to a list not bound");
        if (op->op == PURE_OP_APPEND_IN && field_type(m, list_of(arg), item_field(arg)) != T_LIST)
            REFUSE("append into a field not a list");
        /* A view's body changes an event's lists alone: its states are a snapshot's. */
        if (body && (op->op == PURE_OP_LIST_APPEND || op->op == PURE_OP_LIST_INSERT || op->op == PURE_OP_LIST_REMOVE ||
                     op->op == PURE_OP_ITEM_SET || op->op == PURE_OP_APPEND_IN) &&
            !(m->state_flags[op->op == PURE_OP_ITEM_SET || op->op == PURE_OP_APPEND_IN ? list_of(arg) : arg] & PURE_LIST_ROWS >> 8))
            REFUSE("view changes a state");
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
        if (kind == A_FOLD || kind == A_EACH || kind == A_LOOP) {
            uint32_t inner = pc;
            skip = arg >> 16;
            if (kind == A_FOLD && (folding || view == FILTER))
                REFUSE(view == FILTER ? "fold inside a filter" : "fold inside a fold");
            if (skip > block.length - i - 1)
                REFUSE(kind == A_FOLD ? "fold block outside its block" : kind == A_EACH ? "each block outside its block" :
                       "loop block outside its block");
            unsigned how = arg & PURE_FOLD_KIND;
            uint32_t list = arg >> 8 & 0xFF;
            /* A fold in a walk reads the walk's item as a fold in a column reads its row; a loop's block is its block's code. */
            if (kind == A_EACH) walked[walks] = list; /* its block's walks: these and its own */
            const char *why = kind == A_EACH ? verify_block(m, (PureBlock){pc + 1, skip}, body ? STEPS_WALK : WALK, (int)list, -1,
                                                            event, locals, nest + 1, walked, walks + 1, fn, &inner)
                            : kind == A_LOOP ? verify_block(m, (PureBlock){pc + 1, skip}, body && !steps ? STEPS :
                                                            view == FUNCTION ? -1 : view, row, outer, event, locals, nest + 1,
                                                            walked, walks, fn, &inner)
                                             : verify_block(m, (PureBlock){pc + 1, skip},
                                                            how == PURE_FOLD_COUNT || how == PURE_FOLD_FIRST ? FOLD_ONE :
                                                            arg & PURE_FOLD_INTEGER ? FOLD_I64 : FOLD_TWO,
                                                            (int)list, row, event, locals, nest, walked, walks, -1, &inner);
            if (why) { *at = inner; return why; }
            for (uint32_t k = i + 1; k <= i + skip; ++k) {
                const char *into = kind == A_FOLD ? "branch into a fold" : kind == A_EACH ? "branch into an each" :
                                   "branch into a loop";
                if (target[k]) REFUSE(into);
                for (unsigned j = 0; j < open; ++j) if (joins[j].target == block.start + k) REFUSE(into);
            }
        }
        /* Operand types, bottom to top; record operations take their fields. */
        unsigned char need[PURE_MESSAGE_FIELDS + 2];
        unsigned pops = signature[op->op].pops;
        memcpy(need, signature[op->op].in, sizeof(signature[op->op].in));
        if (op->op == PURE_OP_SEND) {
            const PureInputShape *output = &m->input[arg];
            for (unsigned f = 0; f < output->field_count; ++f) need[f] = message_type(m->field[output->field_start + f].type);
            pops = output->field_count;
        } else if (op->op == PURE_OP_LIST_APPEND || op->op == PURE_OP_LIST_INSERT) {
            const PureRecord *record = &m->record[m->state_ref[arg]];
            for (unsigned f = 0; f < record->count; ++f) need[pops + f] = field_type(m, arg, f);
            pops += record->count;
        } else if (op->op == PURE_OP_ITEM_SET) {
            need[pops++] = field_type(m, list_of(arg), item_field(arg));
        } else if (op->op == PURE_OP_APPEND_IN) { /* an item of the list in the field */
            const PureRecord *record = &m->record[nested_record(m, m->record[m->state_ref[list_of(arg)]].first + item_field(arg))];
            for (unsigned f = 0; f < record->count; ++f) need[pops + f] = record_type(m, record->first + f);
            pops += record->count;
        } else if (need[0] == T_KEY) {
            need[0] = field_type(m, arg, m->state_key[arg] - 1u);
        } else if (need[0] == T_LOCAL) {
            need[0] = local_type(arg);
        } else if (need[0] == T_STATE) {
            need[0] = state_type(m, arg);
        } else if (op->op == PURE_OP_NUM_TEXT && arg & 3) {
            need[0] = (arg & 3) == 1 ? T_BOOL : T_I64;
        } else if ((op->op == PURE_OP_CONCAT && arg) || (op->op == PURE_OP_TEXT_COMPARE && arg & PURE_COMPARE_BYTES)) {
            need[0] = need[1] = T_BYTES;
        } else if (op->op == PURE_OP_TEXT_SET && m->state_flags[arg] & PURE_TEXT_BYTES) {
            need[0] = T_BYTES; /* a bytes state's whole value (028) */
        } else if (op->op == PURE_OP_BYTES_SLICE && arg) {
            need[1] = need[2] = T_I64; /* the positions i64s */
        } else if (op->op == PURE_OP_BYTES_STORE && (arg & 0xFF) >= PURE_HELD_TEXT) {
            need[0] = T_TEXT;
        } else if (op->op == PURE_OP_BYTE_AT && arg) {
            need[1] = T_I64; /* the position an i64 */
        } else if ((op->op == PURE_OP_EQ || op->op == PURE_OP_NE) && height >= 2 && types[height - 1] == T_BOOL &&
                   types[height - 2] == T_BOOL) {
            need[0] = need[1] = T_BOOL; /* two bools compare as two numbers do */
        } else if ((kind == A_FOLD && arg & PURE_FOLD_SLICED) || (kind == A_EACH && arg & PURE_EACH_SLICED)) {
            uint32_t list = arg >> 8 & 0xFF;
            need[0] = need[1] = field_type(m, list, m->state_key[list] - 1u); /* a slice's low and high keys */
            pops = 2;
        } else if (kind == A_LOOP) {
            need[0] = need[1] = arg & PURE_LOOP_INTEGER ? T_I64 : T_NUM; /* low and high */
            pops = 2;
        } else if (kind == A_FUNCTION) { /* a function's parameters, or the results its RETURN gives */
            const PureFunction *f = &m->function[arg];
            if (op->op == PURE_OP_RETURN && (int)arg != fn) REFUSE("return from another function");
            uint32_t first = f->first + (op->op == PURE_OP_RETURN ? f->params : 0);
            pops = op->op == PURE_OP_RETURN ? f->results : f->params;
            for (unsigned k = 0; k < pops; ++k) need[k] = signature_stack_type(m, first + k);
        }
        /* A loop's block begins at the bottom of its run's stack: its operands are all the stack holds. */
        if ((kind == A_LOOP || kind == A_EACH) && height != (int)pops) REFUSE("loop above values");
        /* What a walk keeps to, a call keeps to as well: a call changes no list a walk around it walks. */
        for (unsigned k = 0; op->op == PURE_OP_CALL && k < walks; ++k)
            if (pure_states_has(m->function_changes + (size_t)arg * m->state_words, walked[k])) REFUSE("call that changes a list walked");
        if (height < (int)pops) REFUSE("stack underflow");
        height -= (int)pops;
        for (unsigned k = 0; k < pops; ++k)
            if (types[height + (int)k] != need[k]) REFUSE("operand type");
        for (unsigned k = 0; k < pops; ++k) /* a list value goes where a list of its record goes */
            if (need[k] == T_LIST && owners[height + (int)k] != list_wanted(m, op, k)) REFUSE("list of another record");
        unsigned owner = of == O_ARG ? arg : of == O_REF ? m->state_ref[arg] : of == O_LIST ? list_of(arg) : 0;
        if (kind == A_LOCAL && arg >> 8 & 0xFF) { /* an index local belongs to its list */
            owner = arg >> 16;
            of = O_ARG;
        }
        for (unsigned k = 0; of >= O_ARG && k < pops; ++k) {
            unsigned char type = types[height + (int)k];
            if ((type == T_CURSOR || type == T_INDEX) && owners[height + (int)k] != owner)
                REFUSE(type == T_CURSOR ? "cursor of another text" : "index of another list");
        }
        unsigned char out = signature[op->op].out;
        if (kind == A_FOLD && (arg & PURE_FOLD_KIND) == PURE_FOLD_FIRST) { /* an index of its list */
            out = T_INDEX;
            of = O_ARG;
            owner = arg >> 8 & 0xFF;
        } else if (kind == A_FOLD && arg & PURE_FOLD_INTEGER && (arg & PURE_FOLD_KIND) != PURE_FOLD_DISTINCT)
            out = T_I64; /* an i64's sum, most or least */
        if (out == T_FIELD) out = message_type(m->field[shape->field_start + arg].type);
        if (out == T_ITEM) out = field_type(m, list_of(arg), item_field(arg));
        if (out == T_LOCAL) out = local_type(arg);
        if (out == T_STATE) out = state_type(m, arg);
        if (truth) out = T_BOOL;
        if (integer) out = T_I64;
        if ((op->op == PURE_OP_CONCAT && arg) || (op->op == PURE_OP_BYTES_LOAD && (arg & 0xFF) < PURE_HELD_TEXT) ||
            (op->op == PURE_OP_TEXT_LOAD && m->state_flags[arg] & PURE_TEXT_BYTES))
            out = T_BYTES;
        else if (op->op == PURE_OP_BYTES_LOAD) out = T_TEXT;
        if (op->op == PURE_OP_CALL) /* its results */
            for (uint32_t k = 0, first = m->function[arg].first + m->function[arg].params; k < m->function[arg].results; ++k) {
                if (height == PURE_MODULE_STACK) REFUSE("stack overflow");
                types[height] = signature_stack_type(m, first + k);
                owners[height] = (unsigned char)(types[height] == T_LIST ? m->signature_record[first + k] : 0);
                ++height;
            }
        if (out) {
            if (height == PURE_MODULE_STACK) REFUSE("stack overflow");
            owners[height] = (unsigned char)(out != T_CURSOR && out != T_INDEX ? 0 : owner);
            if (out == T_LIST) /* the record of its items */
                owners[height] = (unsigned char)(op->op == PURE_OP_LIST_VALUE ? m->state_ref[arg] : op->op == PURE_OP_LOCAL_LOAD ? arg >> 16 :
                                                 nested_record(m, m->record[m->state_ref[list_of(arg)]].first + item_field(arg)));
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
        if (op->op == PURE_OP_BREAK) { /* as a jump to the block's end: the loop goes on after it */
            target[block.length] = 1;
            reachable = false;
        }
        if (op->op == PURE_OP_RETURN) reachable = false; /* the rest of its stack let go */
        i += skip;
    }
    if (view == FUNCTION) return reachable ? "function ends without return" : NULL;
    if (!reachable && fn < 0) return "block end is unreachable";
    if (!reachable) return NULL; /* a loop's block in a function that returns */
    if (view == FILTER) return height != 1 || types[0] != T_BOOL ? "filter must leave one bool" : NULL;
    if (view == FOLD_ONE) return height != 1 || types[0] != T_BOOL ? "fold block must leave one bool" : NULL;
    if (view == FOLD_TWO)
        return height != 2 || types[0] != T_NUM || types[1] != T_BOOL ? "fold block must leave a number and a bool" : NULL;
    if (view == FOLD_I64)
        return height != 2 || types[0] != T_I64 || types[1] != T_BOOL ? "fold block must leave an i64 and a bool" : NULL;
    if (view >= LIST_OF) /* a list view's or a list column's: a list of its record's items */
        return height != 1 || types[0] != T_LIST || owners[0] != view - LIST_OF ? "view must leave one value of its type" : NULL;
    unsigned char result = view == PURE_VIEW_TEXT ? T_TEXT : view == PURE_VIEW_BOOL ? T_BOOL : view == PURE_VIEW_I64 ? T_I64 :
                           view == PURE_VIEW_BYTES ? T_BYTES : T_NUM;
    if (!handler ? height != 1 || types[0] != result : height != 0)
        return !handler ? "view must leave one value of its type" : "handler must leave no value";
    return NULL;
#undef REFUSE
}

PURE_COLD static const char *verify_block(const PureModule *m, PureBlock block, int view, int row, int outer,
                                          uint32_t event, bool locals, unsigned nest, uint32_t *walked, unsigned walks, int fn,
                                          uint32_t *at) {
    if (block.start > m->code_count || block.length > m->code_count - block.start) return "code range outside module";
    unsigned char *target = calloc((size_t)block.length + 1, 1);
    uint32_t *own = walked ? NULL : malloc(((size_t)m->code_count + 1) * sizeof(*own)); /* a walk's an instruction at least */
    if (!target || (!walked && !own)) {
        free(target);
        return "out of memory";
    }
    const char *why = verify_code(m, block, view, row, outer, event, locals, nest, walked ? walked : own, walks, fn, at, target);
    free(target), free(own);
    return why;
}

/* Whether an instruction has a form that works on the items of a run at
 * once: numbers, bools and indexes, with no branch, no text and no i64. */
PURE_COLD static bool has_lanes(const PureModule *m, const PureOp *op, int input) {
    switch (op->op) {
    case PURE_OP_FIELD: { /* a number or bool the message or the arguments give, the same for every item */
        uint8_t type = input >= 0 ? m->field[m->input[input].field_start + op->arg].type : 0;
        return type == PURE_FIELD_U32 || type == PURE_FIELD_NUMBER || type == PURE_FIELD_BOOL;
    }
    case PURE_OP_CONST: return op->arg != 2;
    case PURE_OP_LOAD: return m->state_type[op->arg] != PURE_STATE_I64;
    case PURE_OP_LOCAL_LOAD: return local_type(op->arg) != T_I64 && local_type(op->arg) != T_LIST;
    case PURE_OP_ADD: case PURE_OP_SUB: case PURE_OP_MUL: case PURE_OP_DIV: case PURE_OP_NEG:
    case PURE_OP_LT: case PURE_OP_LE: case PURE_OP_GT: case PURE_OP_GE: case PURE_OP_EQ: case PURE_OP_NE:
    case PURE_OP_AND: case PURE_OP_OR: case PURE_OP_NOT: case PURE_OP_MIN: case PURE_OP_MAX: case PURE_OP_CLAMP:
    case PURE_OP_FLOOR: case PURE_OP_MOD: case PURE_OP_LIST_COUNT: case PURE_OP_INDEX_LOAD:
    case PURE_OP_INDEX_MOVE: case PURE_OP_INDEX_FIRST: case PURE_OP_INDEX_LAST: case PURE_OP_INDEX_NUM:
    case PURE_OP_ROW: case PURE_OP_OUTER_ROW: case PURE_OP_PICK: case PURE_OP_SLIDE:
        return true; /* no i64 comes from the others, so PICK and SLIDE move none */
    case PURE_OP_ITEM_GET: {
        unsigned char type = field_type(m, list_of(op->arg), item_field(op->arg));
        return type == T_NUM || type == T_BOOL;
    }
    case PURE_OP_LIST_FIND: case PURE_OP_LIST_HAS: return field_type(m, op->arg, m->state_key[op->arg] - 1u) == T_NUM;
    default: return false;
    }
}

PURE_COLD static bool block_has_lanes(const PureModule *m, PureBlock block, int input) {
    for (uint32_t pc = block.start; pc < block.start + block.length; ++pc)
        if (!has_lanes(m, &m->code[pc], input)) return false;
    return true;
}

/* The end of the body of if !L.has(k) { ... } whose HAS is at h, when its
 * code is a line of lane forms and appends (no jump, loop, break, local set,
 * item set or HAS) within [h, end): a filter (module-batch.c); else 0. */
PURE_COLD static uint32_t filter_body(const PureModule *m, uint32_t h, uint32_t end) {
    const PureOp *op = &m->code[h];
    if (op->op != PURE_OP_LIST_HAS || h + 3 > end || op[1].op != PURE_OP_NOT || op[2].op != PURE_OP_JZ || op[2].arg > end ||
        op[2].arg <= h + 3 || m->height[h] != 1)
        return 0;
    for (uint32_t at = h + 3; at < op[2].arg; ++at)
        switch (m->code[at].op) {
        case PURE_OP_JZ: case PURE_OP_JMP: case PURE_OP_LOOP: case PURE_OP_EACH: case PURE_OP_BREAK: case PURE_OP_LOCAL_STORE:
        case PURE_OP_ITEM_SET: case PURE_OP_LIST_HAS:
            return 0;
        default:;
        }
    return op[2].arg;
}

/* Whether the block [start, end) of a loop a batch runs (depth: the loops
 * around it the batch runs too) has batch forms throughout: lane forms, a
 * local set (a number, an index or a bool), read only after its round sets
 * it (sure: set for certain since the round began) or a reduction's
 * (reduced: x = x op e alone), a branch with nothing else on the stack to a
 * place in the block, a loop in it (one deep: its rounds run together, a
 * BREAK leaving it), and a number or bool set in an item (not its key; not
 * in a nested loop). shape: 1 a nested loop, 2 a set. */
PURE_COLD static bool batch_block(const PureModule *m, uint32_t start, uint32_t end, int input, unsigned depth, uint64_t *sure,
                                  const uint64_t *set, const uint64_t *reduced, unsigned *shape) {
    uint32_t arms = start, body = 0; /* code before arms: in a branch's way; before body: a filter's */
    for (uint32_t at = start; at < end; ++at) {
        const PureOp *op = &m->code[at];
        unsigned local = op->arg & 0xFF;
        uint64_t bit = UINT64_C(1) << (local % 64);
        uint32_t filtered = op->op == PURE_OP_LIST_HAS && at >= arms ? filter_body(m, at, end) : 0;
        if (filtered) body = filtered;
        switch (op->op) {
        case PURE_OP_LOOP: {
            uint32_t inner = at + 1, stop = inner + (op->arg >> 16);
            if (depth || op->arg & PURE_LOOP_INTEGER || stop > end || m->height[inner]) return false;
            uint64_t scope[4];
            memcpy(scope, sure, sizeof(scope));
            scope[local / 64] |= bit; /* its slot, set as each round begins */
            *shape |= 1;
            if (!batch_block(m, inner, stop, input, depth + 1, scope, set, reduced, shape)) return false;
            at = stop - 1;
            break;
        }
        case PURE_OP_EACH: { /* a walk of a list keyed by a number (its slice), no where's range: each lane's items together */
            uint32_t inner = at + 1, stop = inner + (op->arg >> 16), walked = op->arg >> 8 & 0xFF;
            if (depth || stop > end || m->height[inner] || (!(op->arg & PURE_EACH_SLICED) && op->range) ||
                ((op->arg & PURE_EACH_SLICED) && field_type(m, walked, m->state_key[walked] - 1u) != T_NUM))
                return false;
            uint64_t scope[4];
            memcpy(scope, sure, sizeof(scope));
            *shape |= 1;
            if (!batch_block(m, inner, stop, input, depth + 1, scope, set, reduced, shape)) return false;
            at = stop - 1;
            break;
        }
        case PURE_OP_BREAK:
            if (!depth) return false; /* a nested loop's alone */
            break;
        case PURE_OP_JZ: case PURE_OP_JMP:
            if (op->arg > end || m->height[at] != (op->op == PURE_OP_JZ)) return false;
            if (op->arg > arms) arms = op->arg;
            break;
        case PURE_OP_LOCAL_STORE:
            if (local_type(op->arg) == T_I64 || local_type(op->arg) == T_LIST) return false;
            if (at >= arms) sure[local / 64] |= bit;
            break;
        case PURE_OP_LOCAL_LOAD:
            if (!has_lanes(m, op, input) || set[local / 64] & ~sure[local / 64] & ~reduced[local / 64] & bit) return false;
            break;
        case PURE_OP_ITEM_SET: {
            unsigned char type = field_type(m, list_of(op->arg), item_field(op->arg));
            if (depth || (type != T_NUM && type != T_BOOL) || item_field(op->arg) == m->state_key[list_of(op->arg)] - 1u) return false;
            *shape |= 2;
            break;
        }
        case PURE_OP_LIST_APPEND: { /* an item of numbers and bools, its list read nowhere in the block (appended) */
            for (uint32_t i = 0; i < m->states; ++i) /* an index into it: its moves (state_indexed is set after the plans) */
                if (m->state_type[i] == PURE_STATE_INDEX && m->state_ref[i] == op->arg) return false;
            if (depth && at >= body) return false; /* in a nested loop: a filter's alone */
            for (unsigned f = 0; f < m->record[m->state_ref[op->arg]].count; ++f)
                if (field_type(m, op->arg, f) != T_NUM && field_type(m, op->arg, f) != T_BOOL) return false;
            *shape |= at < body ? 8 : 4;
            break;
        }
        default:
            if (!has_lanes(m, op, input)) return false;
        }
    }
    return true;
}

/* Whether instructions [a, a + n) and [b, b + n) are the same code. */
PURE_COLD static bool same_code_of(const PureModule *m, uint32_t a, uint32_t b, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i)
        if (m->code[a + i].op != m->code[b + i].op || m->code[a + i].arg != m->code[b + i].arg ||
            m->code[a + i].integer != m->code[b + i].integer)
            return false;
    return true;
}

/* The list an instruction of a batch's block reads, or -1. */
PURE_COLD static int read_list(const PureOp *op) {
    switch (op->op) {
    case PURE_OP_ITEM_GET: case PURE_OP_ITEM_SET: return (int)list_of(op->arg);
    case PURE_OP_LIST_FIND: case PURE_OP_LIST_HAS: case PURE_OP_LIST_COUNT: case PURE_OP_INDEX_FIRST: case PURE_OP_INDEX_LAST:
    case PURE_OP_ROW: case PURE_OP_OUTER_ROW: case PURE_OP_BYTES_OF: case PURE_OP_INDEX_AT: return (int)(op->arg & 0xFFFF);
    default: return -1;
    }
}

PURE_COLD static bool walk_spreads(const PureModule *m, uint32_t start, uint32_t end, uint32_t each, const uint64_t *reduced);

/* A count's loop or a walk whose block has batch forms throughout
 * (batch_block, after a count's guard: if slot >= L.count { break }, *guard)
 * runs a batch of its rounds at a time (module-batch.c,
 * docs/design/024), unless it
 * sets or appends items or has a recurrence and has a nested loop (not yet),
 * reads a list it appends to, or PURE_BATCH=0 in the environment says none
 * does (the executor alone, to measure it). A local is a reduction when every
 * read of it begins x = x op e and every set of it ends one; a recurrence
 * (recurs) when a round reads it before setting it and every set of it is
 * x = e of x alone (pure_batch_recurs), in no branch's way. */
PURE_COLD static bool loop_batches(const PureModule *m, uint32_t pc, int input, uint64_t *recurs, bool *guard, uint64_t *reductions) {
    static int off = -1;
    if (off < 0) {
        const char *batches = getenv("PURE_BATCH");
        off = batches && !strcmp(batches, "0");
    }
    const PureOp *loop = &m->code[pc];
    bool walk = loop->op == PURE_OP_EACH; /* a walk: no slot, its list the block's to read alone */
    uint32_t start = pc + 1, end = start + (loop->arg >> 16);
    if (off || (!walk && loop->arg & PURE_LOOP_INTEGER) || start >= end || m->height[start]) return false;
    const PureOp *g = &m->code[start];
    *guard = !walk && end - start > 5 && g[0].op == PURE_OP_LOCAL_LOAD && (g[0].arg & 0xFF) == (loop->arg & 0xFF) &&
             g[1].op == PURE_OP_LIST_COUNT && g[2].op == PURE_OP_GE && g[3].op == PURE_OP_JZ && g[3].arg == start + 5 &&
             g[4].op == PURE_OP_BREAK; /* if slot >= L.count { break }: a batch to L's count (its block only adds to L) */
    uint8_t *ends = calloc(end - start, 1); /* the sets that end x = x op e */
    if (!ends) return false;
    uint64_t set[4] = {0}, sure[4] = {0}, reduced[4] = {0}, plain[4] = {0}; /* plain: read or set otherwise */
    for (uint32_t at = start; at < end; ++at) {
        const PureOp *op = &m->code[at];
        uint64_t bit = UINT64_C(1) << (op->arg & 63);
        unsigned word = (op->arg & 0xFF) / 64;
        uint32_t by;
        if (op->op == PURE_OP_LOCAL_STORE || op->op == PURE_OP_LOOP) set[word] |= bit;
        if (op->op == PURE_OP_LOCAL_LOAD && local_type(op->arg) == T_NUM && pure_batch_reduces(m, at, end, &by))
            reduced[word] |= bit, ends[by + 1 - start] = 1;
        else if (op->op == PURE_OP_LOCAL_LOAD)
            plain[word] |= bit;
    }
    for (uint32_t at = start; at < end; ++at)
        if (m->code[at].op == PURE_OP_LOCAL_STORE && !ends[at - start]) plain[(m->code[at].arg & 0xFF) / 64] |= UINT64_C(1) << (m->code[at].arg & 63);
    free(ends);
    for (unsigned w = 0; w < 4; ++w) reduced[w] &= set[w] & ~plain[w];
    uint64_t sure0[4] = {0}, exempt[4]; /* a local read before the round sets it, not a reduction's: a recurrence's, if all its sets are */
    uint32_t arms = start, from;
    unsigned slot = walk ? 256 : loop->arg & 0xFF;
    for (uint32_t at = start; at < end; ++at) {
        const PureOp *op = &m->code[at];
        uint64_t bit = UINT64_C(1) << (op->arg & 63);
        unsigned word = (op->arg & 0xFF) / 64;
        if (op->op == PURE_OP_LOOP || op->op == PURE_OP_EACH) at += op->arg >> 16; /* (none with a nested loop) */
        else if ((op->op == PURE_OP_JZ || op->op == PURE_OP_JMP) && op->arg > arms) arms = op->arg;
        else if (op->op == PURE_OP_LOCAL_STORE && at >= arms) sure0[word] |= bit;
        else if (op->op == PURE_OP_LOCAL_LOAD && set[word] & ~sure0[word] & ~reduced[word] & bit) recurs[word] |= bit;
    }
    arms = start;
    for (uint32_t at = start; at < end; ++at) { /* each set of one: in no branch's way or nested loop, x = e of x alone */
        const PureOp *op = &m->code[at];
        unsigned local = op->arg & 0xFF;
        if ((op->op == PURE_OP_JZ || op->op == PURE_OP_JMP) && op->arg > arms) arms = op->arg;
        if (op->op == PURE_OP_LOOP || op->op == PURE_OP_EACH) arms = at + 1 + (op->arg >> 16) > arms ? at + 1 + (op->arg >> 16) : arms;
        if (op->op == PURE_OP_LOCAL_STORE && recurs[local / 64] >> (local % 64) & 1 &&
            (at < arms || local_type(op->arg) != T_NUM || !pure_batch_recurs(m, at, start, set, slot, &from)))
            recurs[local / 64] &= ~(UINT64_C(1) << (local % 64));
    }
    for (unsigned w = 0; w < 4; ++w) exempt[w] = reduced[w] | recurs[w], reductions[w] = reduced[w];
    unsigned shape = 0;
    bool any = recurs[0] | recurs[1] | recurs[2] | recurs[3];
    if (!batch_block(m, *guard ? start + 5 : start, end, input, 0, sure, set, exempt, &shape) || (shape & 1 && (shape & 6 || any)) ||
        (shape & 8 && shape & 6)) /* a filter's appends: no other append or set beside them */
        return false;
    /* A list appended to is read nowhere else in the block (an earlier round's item not there yet), nor walked; one a
     * filter's code alone adds to (grown) is read only as no earlier round's item would change: its count in the guard,
     * its first, an item by its place when it has no key (added at its end), a field by key (found at once: not there,
     * the batch gives its rounds back), and has only in its filter (asked again at the end). */
    uint32_t appended = 0, grown = 0, tested = 0, body = 0, each = 0, nest = 0; /* nest: a nested LOOP's end */
    for (uint32_t at = start; at < end; ++at) {
        const PureOp *op = &m->code[at];
        uint32_t filtered = filter_body(m, at, end);
        if (each && at >= each + 1 + (m->code[each].arg >> 16)) each = 0;
        if (filtered) { /* in the block or in a walk its items' rounds spread (not a nested LOOP's), one a list */
            if (at < nest || (each && !walk_spreads(m, start, end, each, reduced)) || op->arg >= 32 || tested >> op->arg & 1) return false;
            body = filtered, tested |= UINT32_C(1) << op->arg;
        }
        if (op->op == PURE_OP_EACH) each = at;
        if (op->op == PURE_OP_LOOP) nest = at + 1 + (op->arg >> 16);
        if (op->op == PURE_OP_LIST_APPEND) {
            if (op->arg >= 32) return false;
            if (at < body) grown |= UINT32_C(1) << op->arg;
            else appended |= UINT32_C(1) << op->arg;
        }
    }
    if (grown & appended || (walk && (appended | grown) >> (loop->arg >> 8 & 0xFF) & 1)) return false;
    body = 0;
    for (uint32_t at = start; at < end; ++at) {
        const PureOp *op = &m->code[at];
        uint32_t filtered = filter_body(m, at, end);
        int list = read_list(op);
        if (filtered) body = filtered;
        if (list < 0 || list >= 32 || !((appended | grown) >> list & 1)) continue;
        if (appended >> list & 1) return false;
        bool keyed = m->state_key[list] != 0;
        switch (op->op) {
        case PURE_OP_LIST_COUNT: if (!(*guard && at == start + 1)) return false; break;
        case PURE_OP_INDEX_FIRST: break;
        case PURE_OP_ITEM_GET:
            if (keyed && !(at > start && m->code[at - 1].op == PURE_OP_LIST_FIND && m->code[at - 1].arg == (uint32_t)list)) return false;
            break;
        case PURE_OP_LIST_FIND: if (at + 1 >= end || m->code[at + 1].op != PURE_OP_ITEM_GET || list_of(m->code[at + 1].arg) != (uint32_t)list) return false; break;
        case PURE_OP_LIST_HAS: if (!filtered || !(tested >> list & 1)) return false; break;
        default: return false;
        }
    }
    for (uint32_t at = start, b = 0, t = 0; at < end; ++at) { /* a tested list: added to in its filter's code alone */
        uint32_t filtered = filter_body(m, at, end);
        if (filtered) b = filtered, t = m->code[at].arg;
        if (m->code[at].op == PURE_OP_LIST_APPEND && tested >> (m->code[at].arg & 31) & 1 && !(at < b && m->code[at].arg == t)) return false;
    }
    return true;
}

/* Whether the EACH at each in the block [start, end) of a loop a batch runs
 * may spread its items' rounds apart (module-batch.c): no BREAK in it, and
 * each local it sets a reduction's (reduced) or its items' own: set in an
 * item before it reads it there (in no branch's way), read nowhere else in
 * the loop's block. */
PURE_COLD static bool walk_spreads(const PureModule *m, uint32_t start, uint32_t end, uint32_t each, const uint64_t *reduced) {
    uint32_t inner = each + 1, stop = inner + (m->code[each].arg >> 16), arms = inner;
    uint64_t own[4] = {0}, sure[4] = {0};
    for (uint32_t at = inner; at < stop; ++at) {
        const PureOp *op = &m->code[at];
        unsigned local = op->arg & 0xFF;
        if (op->op == PURE_OP_BREAK) return false;
        if (op->op == PURE_OP_LOCAL_STORE && !(reduced[local / 64] >> (local % 64) & 1)) own[local / 64] |= UINT64_C(1) << (local % 64);
    }
    for (uint32_t at = inner; at < stop; ++at) {
        const PureOp *op = &m->code[at];
        unsigned local = op->arg & 0xFF;
        uint64_t bit = UINT64_C(1) << (local % 64);
        if ((op->op == PURE_OP_JZ || op->op == PURE_OP_JMP) && op->arg > arms) arms = op->arg;
        else if (op->op == PURE_OP_LOCAL_STORE && at >= arms) sure[local / 64] |= bit;
        else if (op->op == PURE_OP_LOCAL_LOAD && own[local / 64] & bit & ~sure[local / 64]) return false;
    }
    for (uint32_t at = start; at < end; ++at) {
        if (at == inner) at = stop;
        if (at >= end) break;
        const PureOp *op = &m->code[at];
        if ((op->op == PURE_OP_LOCAL_LOAD || op->op == PURE_OP_LOCAL_STORE) && own[(op->arg & 0xFF) / 64] >> (op->arg & 63) & 1) return false;
    }
    return true;
}

/* Whether the instruction at pc computes a value a batch may keep for a
 * later copy of its code (plan_reuse): no branch, loop, set, append, find
 * by position, has (a filter's), text, PICK or SLIDE, nor a reduction's x. */
PURE_COLD static bool reusable(const PureModule *m, uint32_t pc, const uint64_t *reduced) {
    const PureOp *op = &m->code[pc];
    switch (op->op) {
    case PURE_OP_CONST: case PURE_OP_LOAD: case PURE_OP_FIELD: case PURE_OP_ROW: case PURE_OP_OUTER_ROW: case PURE_OP_INDEX_LOAD:
    case PURE_OP_INDEX_FIRST: case PURE_OP_INDEX_LAST: case PURE_OP_INDEX_NUM: case PURE_OP_INDEX_MOVE: case PURE_OP_LIST_COUNT:
    case PURE_OP_ADD: case PURE_OP_SUB: case PURE_OP_MUL: case PURE_OP_DIV: case PURE_OP_MOD: case PURE_OP_MIN: case PURE_OP_MAX:
    case PURE_OP_NEG: case PURE_OP_NOT: case PURE_OP_FLOOR: case PURE_OP_CLAMP: case PURE_OP_LT: case PURE_OP_LE: case PURE_OP_GT:
    case PURE_OP_GE: case PURE_OP_EQ: case PURE_OP_NE: case PURE_OP_AND: case PURE_OP_OR:
        return true;
    case PURE_OP_LOCAL_LOAD: return !(reduced[(op->arg & 0xFF) / 64] >> (op->arg & 63) & 1);
    case PURE_OP_ITEM_GET: return !(op->plan & PLAN_TEXT);
    case PURE_OP_LIST_FIND: return op->plan & PLAN_JOINED && !(op->plan & PLAN_TEXT);
    default: return false;
    }
}

/* What the instructions [s, e) read: the locals (a bit each) and the lists (a bit each, lists past 32 as one bit). */
PURE_COLD static void reads_of(const PureModule *m, uint32_t s, uint32_t e, uint64_t *locals, uint64_t *lists) {
    for (uint32_t at = s; at < e; ++at) {
        const PureOp *op = &m->code[at];
        if (op->op == PURE_OP_LOCAL_LOAD) locals[(op->arg & 0xFF) / 64] |= UINT64_C(1) << (op->arg & 63);
        int list = read_list(op);
        if (list >= 0) *lists |= UINT64_C(1) << (list < 63 ? list : 63);
    }
}

/* In the block [start, end) of a loop a batch runs: each value computed by
 * the same code as an earlier value's (same_code's: a subtree of value
 * instructions, reusable throughout), the two in one line of the same
 * innermost block (no JMP, loop, break or jump landing between; a JZ
 * between only narrows the lanes) with nothing between setting a local or
 * changing a list the code reads, is planned to take the earlier value
 * (module-batch.c): reuse_from and reuse_length at its code's start, the
 * earlier root kept (PURE_REUSE_KEPT). */
PURE_COLD static void plan_reuse(PureModule *m, uint32_t start, uint32_t end, const uint64_t *reduced) {
    bool *landed = calloc(end - start + 1, 1);
    uint32_t *block = calloc(end - start, sizeof(uint32_t)); /* each instruction's innermost block's start */
    if (!landed || !block) {
        free(landed), free(block);
        return;
    }
    for (uint32_t at = start; at < end; ++at) {
        const PureOp *op = &m->code[at];
        if ((op->op == PURE_OP_JZ || op->op == PURE_OP_JMP) && op->arg >= start && op->arg <= end) landed[op->arg - start] = true;
    }
    for (uint32_t at = start, inner = start, stop = end; at < end; ++at) {
        if (at >= stop) inner = start, stop = end; /* (one deep) */
        block[at - start] = inner;
        const PureOp *op = &m->code[at];
        if (op->op == PURE_OP_LOOP || op->op == PURE_OP_EACH) inner = at + 1, stop = at + 1 + (op->arg >> 16);
    }
    for (uint32_t root = start; root < end; ++root) {
        if (!reusable(m, root, reduced) || m->reuse_from[root]) continue;
        uint32_t s = root; /* its code: back to where the stack was one below its result */
        while (s > block[root - start] && m->height[s] != m->height[root + 1] - 1) --s;
        if (m->height[s] != m->height[root + 1] - 1 || root == s) continue; /* (a leaf alone: nothing computed) */
        bool pure = true;
        for (uint32_t at = s; at <= root && pure; ++at) {
            pure = reusable(m, at, reduced) && block[at - start] == block[root - start] && !landed[at - start];
            if (m->code[at].op == PURE_OP_LIST_FIND) ++at; /* its ITEM_GET with it */
        }
        if (!pure) continue;
        uint32_t length = root + 1 - s;
        uint64_t locals[4] = {0}, lists = 0;
        reads_of(m, s, root + 1, locals, &lists);
        for (uint32_t at = root + 1, from = root; at + length <= end; ++at) { /* a later copy, in one line */
            const PureOp *op = &m->code[at];
            if (op->op == PURE_OP_JMP || op->op == PURE_OP_LOOP || op->op == PURE_OP_EACH || op->op == PURE_OP_BREAK ||
                block[at - start] != block[root - start] || (landed[at - start] && at > root + 1))
                break;
            if ((op->op == PURE_OP_LOCAL_STORE || op->op == PURE_OP_LOOP) && locals[(op->arg & 0xFF) / 64] >> (op->arg & 63) & 1) break;
            if ((op->op == PURE_OP_ITEM_SET && lists >> (list_of(op->arg) < 63 ? list_of(op->arg) : 63) & 1) ||
                ((op->op == PURE_OP_LIST_APPEND || op->op == PURE_OP_LIST_INSERT || op->op == PURE_OP_LIST_REMOVE) &&
                 lists >> (op->arg < 63 ? op->arg : 63) & 1))
                break;
            if (at >= from && !m->reuse_length[at] && same_code_of(m, s, at, length)) {
                bool line = true; /* the copy itself in the line (no landing in it but at its start, no loop) */
                for (uint32_t i = at + 1; i < at + length; ++i) line &= !landed[i - start] && block[i - start] == block[root - start];
                if (!line) break;
                m->reuse_from[at] = root + 1, m->reuse_length[at] = length;
                m->reuse_from[root] = PURE_REUSE_KEPT;
                m->reuses = true;
                from = at + length, at = at + length - 1;
            }
        }
    }
    free(landed), free(block);
}

/* A fold in a verified block whose own block has lane forms throughout runs
 * a leaf of its list at a time; a count's loop so planned, a batch of its
 * rounds. */
PURE_COLD static void plan_folds(PureModule *m, PureBlock block, int input) {
    for (uint32_t pc = block.start; pc < block.start + block.length; ++pc) {
        PureOp *op = &m->code[pc];
        uint64_t recurs[4] = {0}, reduced[4] = {0};
        bool guard = false;
        if ((op->op == PURE_OP_LOOP || op->op == PURE_OP_EACH) &&
            (op->plan = (op->plan & PLAN_SPREAD) | (loop_batches(m, pc, input, recurs, &guard, reduced) ? PLAN_BATCH | (guard ? PLAN_GUARD : 0) : 0)) &
                PLAN_BATCH) {
            m->batches = true;
            for (uint32_t at = pc + 1; at < pc + 1 + (op->arg >> 16); ++at) { /* its walks whose items may run apart; its filters */
                if (m->code[at].op == PURE_OP_EACH && walk_spreads(m, pc + 1, pc + 1 + (op->arg >> 16), at, reduced))
                    m->code[at].plan |= PLAN_SPREAD;
                if (filter_body(m, at, pc + 1 + (op->arg >> 16))) m->code[at].plan |= PLAN_FILTER;
            }
            plan_reuse(m, pc + 1, pc + 1 + (op->arg >> 16), reduced);
            for (uint32_t at = pc + 1; at < pc + 1 + (op->arg >> 16); ++at) /* a recurrence's sets, in the loop's own block */
                if (m->code[at].op == PURE_OP_LOOP || m->code[at].op == PURE_OP_EACH) at += m->code[at].arg >> 16;
                else if (m->code[at].op == PURE_OP_LOCAL_STORE && recurs[(m->code[at].arg & 0xFF) / 64] >> (m->code[at].arg & 63) & 1)
                    m->code[at].plan |= PLAN_RECUR;
        }
        if (op->op != PURE_OP_FOLD) continue;
        op->plan = !(op->arg & PURE_FOLD_INTEGER) && block_has_lanes(m, (PureBlock){pc + 1, op->arg >> 16}, input);
        pc += op->arg >> 16;
    }
}

/* A planned fold's block as a tree (its lane forms have no branch; one
 * with PICK or SLIDE is left alone): per instruction, its operands' nodes
 * and the first instruction of its subtree, whether it reads the fold's
 * item (item), and whether it reads nothing but the fold's list, its item
 * and constants (own): what one pass over the list can compute. */
typedef struct { uint32_t first; int kid[3]; bool item, own; } Node;

PURE_COLD static unsigned arity(uint8_t op) {
    switch (op) {
    case PURE_OP_CONST: case PURE_OP_LOAD: case PURE_OP_INDEX_LOAD: case PURE_OP_ROW: case PURE_OP_OUTER_ROW:
    case PURE_OP_LIST_COUNT: case PURE_OP_INDEX_FIRST: case PURE_OP_INDEX_LAST: case PURE_OP_FIELD: case PURE_OP_LOCAL_LOAD:
    case PURE_OP_BYTES_OF: case PURE_OP_BYTES_LOAD: case PURE_OP_LIST_VALUE:
        return 0;
    case PURE_OP_ITEM_GET: case PURE_OP_LIST_FIND: case PURE_OP_LIST_HAS: case PURE_OP_NEG: case PURE_OP_NOT:
    case PURE_OP_FLOOR: case PURE_OP_INDEX_NUM: case PURE_OP_I64_NEG: case PURE_OP_I64_FROM: case PURE_OP_NUM_FROM:
    case PURE_OP_INDEX_AT: case PURE_OP_BYTE: case PURE_OP_NUM_PARSE:
        return 1;
    case PURE_OP_CLAMP: case PURE_OP_BYTES_SLICE: return 3;
    default: return 2;
    }
}

/* The list an instruction reads, if any (-1: none). */
PURE_COLD static int list_read(const PureOp *op) {
    switch (op->op) {
    case PURE_OP_ITEM_GET: return (int)list_of(op->arg);
    case PURE_OP_LIST_COUNT: case PURE_OP_INDEX_FIRST: case PURE_OP_INDEX_LAST: case PURE_OP_LIST_FIND:
    case PURE_OP_LIST_HAS: case PURE_OP_ROW: case PURE_OP_BYTES_OF: case PURE_OP_INDEX_AT: case PURE_OP_LIST_VALUE:
        return (int)op->arg;
    default: return -1;
    }
}

PURE_COLD static bool block_tree(const PureModule *m, PureBlock block, int list, Node *nodes, int *roots, unsigned *count) {
    int stack[PURE_MODULE_STACK];
    unsigned sp = 0;
    for (uint32_t i = 0; i < block.length; ++i) {
        const PureOp *op = &m->code[block.start + i];
        if (op->op == PURE_OP_PICK || op->op == PURE_OP_SLIDE) return false;
        unsigned pops = arity(op->op);
        Node *node = &nodes[i];
        int read = list_read(op);
        node->first = block.start + i;
        node->item = op->op == PURE_OP_ROW;
        node->own = op->op != PURE_OP_LOAD && op->op != PURE_OP_INDEX_LOAD && op->op != PURE_OP_OUTER_ROW &&
                    op->op != PURE_OP_FIELD && op->op != PURE_OP_LOCAL_LOAD && (read < 0 || read == list);
        for (unsigned k = 0; k < 3; ++k) node->kid[k] = -1;
        for (unsigned k = 0; k < pops; ++k) {
            int kid = stack[sp - pops + k];
            node->kid[k] = kid;
            node->item = node->item || nodes[kid].item;
            node->own = node->own && nodes[kid].own;
        }
        if (pops) node->first = nodes[stack[sp - pops]].first;
        sp -= pops;
        stack[sp++] = (int)i;
    }
    memcpy(roots, stack, sp * sizeof(*stack));
    *count = sp;
    return true;
}

/* The comparisons a test ANDs together (at most 16). */
PURE_COLD static bool conjuncts(const PureModule *m, PureBlock block, const Node *nodes, int at, int *out, unsigned *count) {
    if (m->code[block.start + at].op == PURE_OP_AND)
        return conjuncts(m, block, nodes, nodes[at].kid[0], out, count) &&
               conjuncts(m, block, nodes, nodes[at].kid[1], out, count);
    if (*count == 16) return false;
    out[(*count)++] = at;
    return true;
}

/* The shape of a planned fold in a column, if it has one (see PureShape). */
PURE_COLD static void shape_fold(PureModule *m, uint32_t pc) {
    PureOp *fold = &m->code[pc];
    unsigned how = fold->arg & 0x7F;
    int list = (int)(fold->arg >> 8 & 0xFF);
    PureBlock block = {pc + 1, fold->arg >> 16};
    if (!fold->plan || m->shapes == PURE_MODULE_SHAPES || how == PURE_FOLD_DISTINCT || fold->arg & PURE_FOLD_SLICED) return;
    Node *nodes = malloc(block.length * sizeof(*nodes));
    int roots[PURE_MODULE_STACK], tests[16];
    unsigned count = 0, tested = 0;
    if (!nodes) return;
    if (!block_tree(m, block, list, nodes, roots, &count) || !conjuncts(m, block, nodes, roots[count - 1], tests, &tested) ||
        (count == 2 && !nodes[roots[0]].own)) {
        free(nodes);
        return;
    }
    /* One comparison of the item's field with a key; the rest the item's own. */
    int test = -1;
    for (unsigned k = 0; k < tested; ++k) {
        if (nodes[tests[k]].own) continue;
        if (test >= 0) test = -2;
        else test = tests[k];
    }
    PureShape shape = {0};
    if (test >= 0) {
        const PureOp *compare = &m->code[block.start + test];
        const Node *node = &nodes[test];
        uint8_t op = compare->op;
        for (unsigned side = 0; side < 2 && !shape.form && (op == PURE_OP_EQ || op == PURE_OP_LT || op == PURE_OP_LE ||
                                                              op == PURE_OP_GT || op == PURE_OP_GE); ++side) {
            int field = node->kid[side], key = node->kid[1 - side];
            const PureOp *get = &m->code[block.start + field];
            if (get->op != PURE_OP_ITEM_GET || m->code[block.start + nodes[field].kid[0]].op != PURE_OP_ROW ||
                nodes[key].item)
                continue;
            static const uint8_t flipped[] = {[PURE_OP_LT] = PURE_OP_GT, [PURE_OP_LE] = PURE_OP_GE,
                                              [PURE_OP_GT] = PURE_OP_LT, [PURE_OP_GE] = PURE_OP_LE};
            shape = (PureShape){op == PURE_OP_EQ ? PURE_SHAPE_GROUP : PURE_SHAPE_RANK,
                                op == PURE_OP_EQ || !side ? op : flipped[op], (uint16_t)item_field(get->arg),
                                block.start + (uint32_t)test,
                                {nodes[key].first, block.start + (uint32_t)key + 1 - nodes[key].first}};
        }
        /* A rank answers a count tested by the comparison alone. */
        if (shape.form == PURE_SHAPE_RANK && (how != PURE_FOLD_COUNT || tested != 1)) shape.form = 0;
    }
    if (shape.form) {
        m->shape[m->shapes++] = shape;
        fold->shape = m->shapes;
    }
    free(nodes);
}

/* A handler (its walks and folds within) uses each local for one thing: a
 * number, or an index of one list. */
PURE_COLD static bool locals_agree(const PureModule *m, PureBlock block) {
    uint32_t used[PURE_MODULE_LOCALS] = {0}; /* what a slot holds + 1 */
    for (uint32_t pc = block.start; pc < block.start + block.length; ++pc) {
        const PureOp *op = &m->code[pc];
        bool loop = op->op == PURE_OP_LOOP; /* its slot holds a number or an i64 */
        if (op->op != PURE_OP_LOCAL_LOAD && op->op != PURE_OP_LOCAL_STORE && !loop) continue;
        uint32_t slot = op->arg & 0xFF, what = loop ? (op->arg & PURE_LOOP_INTEGER ? 3u : 0u) + 1 : (op->arg >> 8) + 1;
        if (used[slot] && used[slot] != what) return false;
        used[slot] = what;
    }
    return true;
}

/* Whether a node's subtree cannot fault: no division, no overflow of a sum
 * or product, no key or item that may be missing (an item read through the
 * item itself or the column's row is there). */
PURE_COLD static bool faultless(const PureModule *m, PureBlock block, const Node *nodes, int at) {
    const PureOp *op = &m->code[block.start + at];
    switch (op->op) {
    case PURE_OP_CONST: case PURE_OP_LOAD: case PURE_OP_FIELD: case PURE_OP_LOCAL_LOAD: case PURE_OP_ROW:
    case PURE_OP_OUTER_ROW: case PURE_OP_LIST_COUNT: case PURE_OP_INDEX_FIRST: case PURE_OP_INDEX_LAST:
    case PURE_OP_INDEX_LOAD: case PURE_OP_INDEX_NUM: case PURE_OP_LT: case PURE_OP_LE: case PURE_OP_GT: case PURE_OP_GE:
    case PURE_OP_EQ: case PURE_OP_NE: case PURE_OP_AND: case PURE_OP_OR: case PURE_OP_NOT: case PURE_OP_NEG:
    case PURE_OP_FLOOR: case PURE_OP_MIN: case PURE_OP_MAX: case PURE_OP_LIST_HAS:
        break;
    case PURE_OP_ITEM_GET: {
        uint8_t index = m->code[block.start + nodes[at].kid[0]].op;
        if (index != PURE_OP_ROW && index != PURE_OP_OUTER_ROW) return false;
        break;
    }
    default: return false;
    }
    for (unsigned k = 0; k < 3; ++k)
        if (nodes[at].kid[k] >= 0 && !faultless(m, block, nodes, nodes[at].kid[k])) return false;
    return true;
}

/* The range of a keyed list's keys a planned block's test allows (see
 * PureRange): a filter's other parts may fault, a fold's or walk's may not;
 * a fold's value (the first of two) neither. */
PURE_COLD static bool block_range(const PureModule *m, PureBlock block, int list, bool faults, PureRange *out) {
    if (!m->state_key[list] || field_type(m, (uint32_t)list, m->state_key[list] - 1u) != T_NUM || !block.length) return false;
    uint32_t key = m->state_key[list] - 1u;
    Node *nodes = malloc(block.length * sizeof(*nodes));
    int roots[PURE_MODULE_STACK], tests[16];
    unsigned count = 0, tested = 0;
    bool ok = nodes && block_tree(m, block, list, nodes, roots, &count) && count >= 1 &&
              conjuncts(m, block, nodes, roots[count - 1], tests, &tested) &&
              (faults || count == 1 || faultless(m, block, nodes, roots[0]));
    PureRange range = {0};
    unsigned bounds = 0;
    for (unsigned k = 0; ok && k < tested; ++k) {
        const Node *node = &nodes[tests[k]];
        uint8_t op = m->code[block.start + tests[k]].op;
        bool bounded = false;
        for (unsigned side = 0; side < 2 && !bounded && op >= PURE_OP_LT && op <= PURE_OP_EQ; ++side) {
            int item = node->kid[side], value = node->kid[1 - side];
            const PureOp *get = &m->code[block.start + item];
            if (get->op != PURE_OP_ITEM_GET || list_of(get->arg) != (uint32_t)list || item_field(get->arg) != key ||
                m->code[block.start + nodes[item].kid[0]].op != PURE_OP_ROW || nodes[value].item)
                continue;
            static const uint8_t flipped[] = {[PURE_OP_LT] = PURE_OP_GT, [PURE_OP_LE] = PURE_OP_GE,
                                              [PURE_OP_GT] = PURE_OP_LT, [PURE_OP_GE] = PURE_OP_LE, [PURE_OP_EQ] = PURE_OP_EQ};
            uint8_t relation = side ? flipped[op] : op; /* the key REL the value */
            PureBlock bound = {nodes[value].first, block.start + (uint32_t)value + 1 - nodes[value].first};
            bool lower = relation == PURE_OP_GT || relation == PURE_OP_GE || relation == PURE_OP_EQ,
                 upper = relation == PURE_OP_LT || relation == PURE_OP_LE || relation == PURE_OP_EQ;
            if ((lower && range.lower_relation) || (upper && range.upper_relation)) continue; /* one of each */
            if (lower) range.lower_relation = relation == PURE_OP_EQ ? PURE_OP_GE : relation, range.lower = bound;
            if (upper) range.upper_relation = relation == PURE_OP_EQ ? PURE_OP_LE : relation, range.upper = bound;
            bounded = true;
        }
        bounds += bounded;
        if (!bounded && !faults && !faultless(m, block, nodes, tests[k])) ok = false;
    }
    free(nodes);
    if (!ok || (!range.lower_relation && !range.upper_relation)) return false;
    range.whole = count == 1 && bounds == tested; /* nothing but the comparisons: the test is the range */
    *out = range;
    return true;
}

PURE_COLD static uint8_t keep_range(PureModule *m, const PureRange *range) {
    if (m->ranges == PURE_MODULE_RANGES) return 0;
    m->range[m->ranges++] = *range;
    return m->ranges;
}

/* Whether a walk's bounds read nothing its block writes: the states and
 * locals it stores, the lists it changes, and what those changes move with
 * them (an insert or remove moves the indices into the list, a text's
 * change the cursors in it). A bound is found once, before the walk; the
 * walk's where reads it anew for every item. */
PURE_COLD static bool bounds_fixed(const PureModule *m, const PureRange *range, PureBlock body) {
    uint64_t states = 0, locals = 0, lists = 0;
    for (uint32_t pc = body.start; pc < body.start + body.length; ++pc) {
        const PureOp *op = &m->code[pc];
        switch (op->op) {
        case PURE_OP_STORE: case PURE_OP_CUR_STORE: case PURE_OP_INDEX_STORE: case PURE_OP_TEXT_INSERT:
        case PURE_OP_TEXT_REMOVE: case PURE_OP_TEXT_SET:
            states |= (uint64_t)1 << (op->arg & 63);
            break;
        case PURE_OP_LOCAL_STORE: case PURE_OP_LOOP: locals |= (uint64_t)1 << (op->arg & 63); break;
        case PURE_OP_LIST_APPEND: case PURE_OP_LIST_INSERT: case PURE_OP_LIST_REMOVE: case PURE_OP_LIST_BIND:
            lists |= (uint64_t)1 << (op->arg & 63);
            break;
        case PURE_OP_ITEM_SET: case PURE_OP_APPEND_IN: lists |= (uint64_t)1 << (list_of(op->arg) & 63); break;
        default: break;
        }
    }
    for (uint32_t s = 0; s < m->states; ++s) {
        uint32_t ref = m->state_ref[s];
        if ((m->state_type[s] == PURE_STATE_INDEX && lists >> (ref & 63) & 1) ||
            (m->state_type[s] == PURE_STATE_CURSOR && states >> (ref & 63) & 1))
            states |= (uint64_t)1 << (s & 63);
    }
    for (unsigned side = 0; side < 2; ++side) {
        PureBlock bound = side ? range->upper : range->lower;
        if (!(side ? range->upper_relation : range->lower_relation)) continue;
        for (uint32_t pc = bound.start; pc < bound.start + bound.length; ++pc) {
            const PureOp *op = &m->code[pc];
            int list = list_read(op);
            if ((op->op == PURE_OP_LOAD || op->op == PURE_OP_INDEX_LOAD || op->op == PURE_OP_CUR_LOAD) &&
                states >> (op->arg & 63) & 1)
                return false;
            if (op->op == PURE_OP_LOCAL_LOAD && locals >> (op->arg & 63) & 1) return false;
            if (list >= 0 && lists >> (list & 63) & 1) return false;
            if (op->op == PURE_OP_INDEX_MOVE && lists) return false; /* the count of its index's list, whichever */
        }
    }
    return true;
}

/* A walk whose block begins with its where (a test, then a jump to the
 * block's end when it fails) and a planned fold: the range their test allows. */
PURE_COLD static void range_code(PureModule *m, PureBlock block, int input) {
    for (uint32_t pc = block.start; pc < block.start + block.length; ++pc) {
        PureOp *op = &m->code[pc];
        PureRange range;
        if (op->op == PURE_OP_FOLD) { /* a slice has its range already */
            if (op->plan && !(op->arg & PURE_FOLD_SLICED) &&
                block_range(m, (PureBlock){pc + 1, op->arg >> 16}, (int)(op->arg >> 8 & 0xFF), false, &range))
                op->range = keep_range(m, &range);
            pc += op->arg >> 16;
            continue;
        }
        if (op->op != PURE_OP_EACH || op->arg & PURE_EACH_SLICED) continue;
        uint32_t end = pc + 1 + (op->arg >> 16);
        int height = 0;
        for (uint32_t at = pc + 1; at < end; ++at) {
            const PureOp *next = &m->code[at];
            if (next->op == PURE_OP_JZ) {
                PureBlock where = {pc + 1, at - pc - 1};
                if (height == 1 && next->arg == end && block_has_lanes(m, where, input) &&
                    block_range(m, where, (int)(op->arg >> 8 & 0xFF), false, &range) &&
                    bounds_fixed(m, &range, (PureBlock){at + 1, end - at - 1}))
                    op->range = keep_range(m, &range);
                break;
            }
            if (!has_lanes(m, next, input) || next->op == PURE_OP_PICK || next->op == PURE_OP_SLIDE) break;
            height += 1 - (int)arity(next->op);
        }
    }
}

/* How deep the loops of m's verified code nest (its blocks apart, each loop's within its own); UINT32_MAX without room. */
static uint32_t nesting_of(const PureModule *m) {
    uint32_t *ends = malloc(((size_t)m->code_count + 1) * sizeof(*ends)), depth = 0, most = 0;
    if (!ends) return UINT32_MAX;
    for (uint32_t pc = 0; pc < m->code_count; ++pc) {
        while (depth && pc >= ends[depth - 1]) --depth;
        if (m->code[pc].op != PURE_OP_EACH && m->code[pc].op != PURE_OP_LOOP) continue;
        ends[depth++] = pc + 1 + (m->code[pc].arg >> 16);
        if (depth > most) most = depth;
    }
    free(ends);
    return most;
}

/* a // b for numbers as Python's float floor division: the quotient of a less its floored remainder, rounded to the
 * nearest whole (it is one but for rounding), a zero with the sign of a / b (027's stage 5). b is not 0. */
/* Whole a and b, each below 2^53 in size (so every step is exact in an i64): a // b and a % b as floor_divide and MOD
 * give them in binary64, bit for bit, zeros signed alike (030: the executor divides wholes as integers, no fmod; whole
 * is checked here, as a proof (N1) would only spare these few compares). false otherwise. */
static inline bool whole_pair(double a, double b) {
    return fabs(a) < 0x1p53 && fabs(b) < 0x1p53 && a == (double)(int64_t)a && b == (double)(int64_t)b;
}
static inline double whole_divide(double a, double b) {
    int64_t x = (int64_t)a, y = (int64_t)b, q = x / y;
    if (x % y && (x < 0) != (y < 0)) --q;
    return q ? (double)q : copysign(0.0, a / b);
}
static inline double whole_mod(double a, double b) {
    int64_t x = (int64_t)a, y = (int64_t)b, r = x % y;
    if (r && (r < 0) != (y < 0)) r += y;
    return r ? (double)r : copysign(0.0, b);
}

static double floor_divide(double a, double b) {
    double mod = fmod(a, b), quotient = (a - mod) / b;
    if (mod != 0 && (b < 0) != (mod < 0)) quotient -= 1.0;
    if (quotient == 0) return copysign(0.0, a / b);
    double whole = floor(quotient);
    return quotient - whole > 0.5 ? whole + 1.0 : whole;
}

/* FLOORDIV's and MOD's values, out of the step (its code and registers as they were: a branch in there costs every op). */
static __attribute__((noinline)) double floor_quotient(double a, double b) {
    return whole_pair(a, b) ? whole_divide(a, b) : floor_divide(a, b);
}
static __attribute__((noinline)) double modulo(double a, double b) { /* the divisor's sign, as floor division leaves it */
    if (whole_pair(a, b)) return whole_mod(a, b);
    double value = fmod(a, b);
    if (value != 0 && (value < 0) != (b < 0)) value += b;
    else if (value == 0) value = copysign(0.0, b);
    return value;
}

/* A name and its place among those of one namespace, in order of name and then of place. */
typedef struct { const char *name; size_t index; } Named;
static int by_name(const void *a, const void *b) {
    const Named *x = a, *y = b;
    int order = strcmp(x->name, y->name);
    return order ? order : (x->index > y->index) - (x->index < y->index);
}

/* A state added to a set of them (027's 4f-2). */
static void states_add(uint64_t *set, uint32_t state) { set[state >> 6] |= UINT64_C(1) << (state & 63); }

/* What a function's code names (027's 2c), read before it is verified (its arguments bounded here): the event's lists,
 * the registers and the values held by name each call has its own of, the lists not an event's it changes, its locals
 * and how deep its loops nest; the functions it calls (calls, a bit each). */
PURE_COLD static bool function_names(PureModule *m, uint32_t fn, uint64_t *calls) {
    PureFunction *f = &m->function[fn];
    uint64_t *lists = m->function_lists + (size_t)fn * m->state_words, *registers = m->function_registers + (size_t)fn * m->state_words,
             *changes = m->function_changes + (size_t)fn * m->state_words;
    if (f->code.start > m->code_count || f->code.length > m->code_count - f->code.start) return true; /* refused as it is verified */
    uint32_t end = f->code.start + f->code.length, *ends = malloc(((size_t)f->code.length + 1) * sizeof(*ends));
    unsigned depth = 0;
    if (!ends) return false;
    for (uint32_t pc = f->code.start; pc < end; ++pc) {
        const PureOp *op = &m->code[pc];
        uint32_t arg = op->arg, list = UINT32_MAX;
        while (depth && pc >= ends[depth - 1]) --depth;
        switch (op->op) {
        case PURE_OP_LIST_APPEND: case PURE_OP_LIST_INSERT: case PURE_OP_LIST_REMOVE:
            list = arg;
            if (state_of(m, list, PURE_STATE_LIST) && !event_list_state(m, list)) states_add(changes, list);
            break;
        case PURE_OP_LIST_COUNT: case PURE_OP_LIST_VALUE: case PURE_OP_LIST_BIND: case PURE_OP_INDEX_FIRST: case PURE_OP_INDEX_LAST:
        case PURE_OP_INDEX_AT: case PURE_OP_BYTES_OF: case PURE_OP_ROW: case PURE_OP_OUTER_ROW: case PURE_OP_LIST_FIND:
        case PURE_OP_LIST_HAS:
            list = arg;
            break;
        case PURE_OP_ITEM_GET: case PURE_OP_ITEM_SET: case PURE_OP_APPEND_IN: list = list_of(arg); break;
        case PURE_OP_EACH: case PURE_OP_FOLD: case PURE_OP_LOOP: case PURE_OP_LOCAL_LOAD: case PURE_OP_LOCAL_STORE:
            if (op->op == PURE_OP_EACH || op->op == PURE_OP_FOLD) list = arg >> 8 & 0xFF;
            if (op->op == PURE_OP_EACH || op->op == PURE_OP_LOOP) { /* (a loop an instruction at least: depth within ends) */
                ends[depth++] = pc + 1 + (arg >> 16);
                if (depth > f->nesting) f->nesting = depth;
            }
            if (op->op == PURE_OP_EACH || op->op == PURE_OP_FOLD) break;
            if ((arg & 0xFF) < PURE_MODULE_LOCALS && (arg & 0xFF) >= f->locals) f->locals = (uint8_t)((arg & 0xFF) + 1);
            if (op->op != PURE_OP_LOOP && (arg >> 8 & 0xFF) == 1) list = arg >> 16; /* an index's list */
            break;
        case PURE_OP_BYTES_LOAD: case PURE_OP_BYTES_STORE: f->held |= UINT32_C(1) << (arg & 31); break;
        case PURE_OP_CALL: if (arg < PURE_MODULE_FUNCTIONS) *calls |= UINT64_C(1) << arg; break;
        default: break;
        }
        if (event_list_state(m, list)) states_add(bound_list(m, list) ? registers : lists, list);
    }
    free(ends);
    return true;
}

/* The lists a verified block names (as function_names reads them): the event's lists into events (not the registers),
 * the stored ones into stored (an index's list with it), the functions it calls into calls. */
PURE_COLD static void block_names(const PureModule *m, PureBlock block, uint64_t *events, uint64_t *stored, uint64_t *calls) {
    for (uint32_t pc = block.start; pc < block.start + block.length; ++pc) {
        const PureOp *op = &m->code[pc];
        uint32_t arg = op->arg, list = UINT32_MAX;
        switch (op->op) {
        case PURE_OP_LIST_APPEND: case PURE_OP_LIST_INSERT: case PURE_OP_LIST_REMOVE: case PURE_OP_LIST_COUNT:
        case PURE_OP_LIST_VALUE: case PURE_OP_LIST_BIND: case PURE_OP_INDEX_FIRST: case PURE_OP_INDEX_LAST: case PURE_OP_INDEX_AT:
        case PURE_OP_BYTES_OF: case PURE_OP_ROW: case PURE_OP_OUTER_ROW: case PURE_OP_LIST_FIND: case PURE_OP_LIST_HAS:
            list = arg;
            break;
        case PURE_OP_ITEM_GET: case PURE_OP_ITEM_SET: case PURE_OP_APPEND_IN: case PURE_OP_KEPT_GET: case PURE_OP_KEPT_SET:
        case PURE_OP_KEPT_APPEND: case PURE_OP_KEPT_INSERT: case PURE_OP_KEPT_APPEND_IN:
            list = list_of(arg);
            break;
        case PURE_OP_EACH: case PURE_OP_FOLD: list = arg >> 8 & 0xFF; break;
        case PURE_OP_LOCAL_LOAD: case PURE_OP_LOCAL_STORE: if ((arg >> 8 & 0xFF) == 1) list = arg >> 16; break;
        case PURE_OP_INDEX_LOAD: case PURE_OP_INDEX_STORE: /* an index state: its list */
            if (arg < m->states) list = m->state_ref[arg];
            break;
        case PURE_OP_INDEX_MOVE: case PURE_OP_INDEX_NUM: list = arg; break; /* the list itself */
        case PURE_OP_CALL: if (arg < PURE_MODULE_FUNCTIONS) *calls |= UINT64_C(1) << arg; break;
        default: break;
        }
        if (event_list_state(m, list)) {
            if (events && !bound_list(m, list)) states_add(events, list);
        } else if (state_of(m, list, PURE_STATE_LIST)) states_add(stored, list);
    }
}

/* 030's step 1: each input's lists to empty and to settle as its event ends (event_clears, event_settles): what its
 * handlers name, with the stored lists the functions they call name (theirs and their calls', until none adds), and
 * its message's rows. false out of memory. */
PURE_COLD static bool event_sets(PureModule *m) {
    size_t words = m->state_words;
    uint64_t *named = calloc((size_t)m->functions * words + 1, sizeof(*named)), calls[PURE_MODULE_FUNCTIONS] = {0};
    if (!named) return false;
    for (uint32_t f = 0; f < m->functions; ++f) block_names(m, m->function[f].code, NULL, named + f * words, &calls[f]);
    for (bool more = m->functions > 0; more;) {
        more = false;
        for (uint32_t f = 0; f < m->functions; ++f)
            for (uint64_t left = calls[f]; left; left &= left - 1)
                for (size_t w = 0; w < words; ++w) {
                    uint64_t was = named[f * words + w];
                    named[f * words + w] |= named[(size_t)__builtin_ctzll(left) * words + w];
                    more = more || named[f * words + w] != was;
                }
    }
    for (uint32_t h = 0; h < m->handlers; ++h) {
        uint32_t input = m->handler[h].event;
        if (input >= m->inputs) continue;
        uint64_t called = 0, *clears = m->event_clears + (size_t)input * words, *settles = m->event_settles + (size_t)input * words;
        block_names(m, m->handler[h].code, clears, settles, &called);
        for (uint64_t left = called; left; left &= left - 1)
            for (size_t w = 0; w < words; ++w) settles[w] |= named[(size_t)__builtin_ctzll(left) * words + w];
    }
    for (uint32_t input = 0; input < m->inputs; ++input) { /* its message's rows fill lists of their own */
        const PureInputShape *shape = &m->input[input];
        for (uint32_t f = 0; f < shape->field_count; ++f) {
            const PureField *field = &m->field[shape->field_start + f];
            if (field->type == PURE_FIELD_ROWS && field->rows < m->states)
                states_add(m->event_clears + (size_t)input * words, field->rows);
        }
    }
    free(named);
    return true;
}

/* Each function's keeps (PureModule's keeps, keep_start) and their counts (own_lists, own_registers), from its sets of
 * lists and registers. false out of memory. */
#define PURE_KEEP_REGISTER (UINT32_C(1) << 31)
PURE_COLD static bool call_keep_lists(PureModule *m) {
    size_t total = 0, words = m->state_words;
    for (uint32_t f = 0; f < m->functions; ++f)
        for (size_t w = 0; w < words; ++w)
            total += (size_t)__builtin_popcountll(m->function_lists[f * words + w] | m->function_registers[f * words + w]);
    if (!(m->keep_start = calloc((size_t)m->functions + 1, sizeof(*m->keep_start))) ||
        !(m->keeps = malloc((total ? total : 1) * sizeof(*m->keeps))))
        return false;
    uint32_t at = 0;
    for (uint32_t f = 0; f < m->functions; ++f) {
        const uint64_t *lists = m->function_lists + f * words, *registers = m->function_registers + f * words;
        PureFunction *fn = &m->function[f];
        fn->own_lists = fn->own_registers = 0;
        m->keep_start[f] = at;
        for (uint32_t state = 0; state < m->states; ++state) {
            bool reg = pure_states_has(registers, state);
            if (!reg && !pure_states_has(lists, state)) continue;
            m->keeps[at++] = state | (reg ? PURE_KEEP_REGISTER : 0);
            reg ? ++fn->own_registers : ++fn->own_lists;
        }
    }
    m->keep_start[m->functions] = at;
    return true;
}

/* Whether op changes list (an event's): its items (append, insert, remove), a cell (ITEM_SET), or a cell's list
 * (APPEND_IN, which may put a new list in the cell). */
static bool changes_list(const PureModule *m, const PureOp *op, uint32_t list) {
    switch (op->op) {
    case PURE_OP_LIST_APPEND: case PURE_OP_LIST_INSERT: case PURE_OP_LIST_REMOVE: return op->arg == list;
    case PURE_OP_ITEM_SET: case PURE_OP_APPEND_IN: return list_of(op->arg) == list;
    default: (void)m; return false;
    }
}

/* 030's N4, proved here: a LIST_VALUE in a handler whose list (an event's, not a register, its record holding no lists)
 * nothing changes once it ran, so the value can be the list itself (plan 1: lent, not copied). What runs after it in
 * its event is the rest of its block from the outermost loop around it (jumps go forward only, loops are EACH, LOOP and
 * a FOLD's block), and the event's later handlers; a function's lists are its own (a call never changes its caller's).
 * The value's own changes copy it first (a lent value counts as shared). */
PURE_COLD static void lend_values(PureModule *m) {
    for (uint32_t h = 0; h < m->handlers; ++h) {
        PureBlock block = m->handler[h].code;
        uint32_t end = block.start + block.length;
        for (uint32_t p = block.start; p < end; ++p) {
            PureOp *op = &m->code[p];
            if (op->op != PURE_OP_LIST_VALUE) continue;
            uint32_t list = op->arg;
            op->plan = 0;
            if (!event_list_state(m, list) || bound_list(m, list) || m->record_nested[m->state_ref[list]]) continue;
            uint32_t from = p + 1; /* the outermost loop around it runs again from its start */
            for (uint32_t q = block.start; q < p; ++q) {
                uint8_t o = m->code[q].op;
                if ((o == PURE_OP_EACH || o == PURE_OP_LOOP || o == PURE_OP_FOLD) && q + 1 + (m->code[q].arg >> 16) > p && q < from)
                    from = q;
            }
            bool changed = false;
            for (uint32_t q = from; q < end && !changed; ++q) changed = changes_list(m, &m->code[q], list);
            for (uint32_t later = h + 1; later < m->handlers && !changed; ++later)
                if (m->handler[later].event == m->handler[h].event)
                    for (uint32_t q = m->handler[later].code.start; q < m->handler[later].code.start + m->handler[later].code.length && !changed; ++q)
                        changed = changes_list(m, &m->code[q], list);
            op->plan = !changed;
        }
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

#ifdef __SANITIZE_ADDRESS__
#include <sanitizer/asan_interface.h>
enum { TABLE_GAP = 16 }; /* poisoned between tables: reading past one is caught */
#else
enum { TABLE_GAP = 0 };
#endif

/* The module's tables laid out from base (027's 4f-1), each a run of its entries aligned to 8; the bytes they take. */
static size_t lay_tables(PureModule *m, uintptr_t base, size_t views, size_t signatures) {
    size_t at = 0;
#define LAY(member, count) \
    (m->member = (void *)(base + at), at += (((size_t)(count) * sizeof(*m->member) + 7) & ~(size_t)7) + TABLE_GAP)
    LAY(state_names, m->states), LAY(state_type, m->states), LAY(state_ref, m->states), LAY(state_flags, m->states);
    LAY(state_key, m->states), LAY(state_indexed, m->states), LAY(initial, m->states);
    LAY(state_transient, m->states), LAY(state_counted, m->states);
    LAY(input_names, m->inputs), LAY(input_keys, m->inputs), LAY(input, m->inputs), LAY(field, m->fields);
    LAY(view_names, views), LAY(view_type, views), LAY(view_table, views), LAY(view_list, views), LAY(view_record, views);
    LAY(view_field, views), LAY(view_computed, views), LAY(view_code, views), LAY(view_plan, views);
    LAY(view_arguments, views), LAY(view_body, views), LAY(view_lists, views * m->state_words), LAY(view_range, views);
    LAY(handler, m->handlers);
    LAY(record_names, m->records), LAY(record, m->records), LAY(record_nested, m->records), LAY(record_held, m->records);
    LAY(record_field_names, m->record_fields), LAY(record_field_type, m->record_fields);
    LAY(record_field_default, m->record_fields);
    LAY(function, m->functions), LAY(signature_type, signatures), LAY(signature_record, signatures);
    LAY(function_lists, (size_t)m->functions * m->state_words), LAY(function_registers, (size_t)m->functions * m->state_words);
    LAY(function_changes, (size_t)m->functions * m->state_words);
    LAY(event_clears, (size_t)m->inputs * m->state_words), LAY(event_settles, (size_t)m->inputs * m->state_words);
#undef LAY
    return at;
}

static bool make_tables(PureModule *m, size_t views, size_t signatures) {
    size_t size = lay_tables(m, 0, views, signatures);
    unsigned char *base = calloc(1, size ? size : 1);
    if (!base) return false;
    m->tables = base;
    lay_tables(m, (uintptr_t)base, views, signatures);
#ifdef __SANITIZE_ADDRESS__
    for (size_t at = 0; at + TABLE_GAP <= size;) { /* each run's gap, found again as it was laid out */
        unsigned char *next = base + size;
#define GAP(member) if ((unsigned char *)m->member > base + at && (unsigned char *)m->member < next) next = (unsigned char *)m->member;
        GAP(state_type) GAP(state_ref) GAP(state_flags) GAP(state_key) GAP(state_indexed) GAP(initial) GAP(input_names)
        GAP(input_keys) GAP(input) GAP(field) GAP(view_names) GAP(view_type) GAP(view_table) GAP(view_list) GAP(view_record)
        GAP(view_field) GAP(view_computed) GAP(view_code) GAP(view_plan) GAP(view_arguments) GAP(view_body) GAP(view_lists)
        GAP(view_range) GAP(handler) GAP(record_names) GAP(record) GAP(record_nested) GAP(record_held) GAP(record_field_names)
        GAP(record_field_type) GAP(record_field_default) GAP(function) GAP(signature_type) GAP(signature_record)
        GAP(state_transient) GAP(state_counted) GAP(function_lists) GAP(function_registers) GAP(function_changes)
        GAP(event_clears) GAP(event_settles)
#undef GAP
        ASAN_POISON_MEMORY_REGION(next - TABLE_GAP, TABLE_GAP);
        at = (size_t)(next - base);
    }
#endif
    return true;
}

void pure_module_release(PureModule *m) {
    free(m->tables);
    m->tables = NULL;
    pure_jit_free(m->jit);
    m->jit = NULL;
    m->batches = m->calls = false;
    m->runner = 0;
    free(m->height);
    m->height = NULL;
    free(m->reuse_from), free(m->reuse_length);
    m->reuse_from = m->reuse_length = NULL;
    free(m->keeps), free(m->keep_start);
    m->keeps = m->keep_start = NULL;
    m->reuses = false;
    pure_batch_plans_free(m);
    free(m->storage);
    m->storage = NULL;
    m->code = NULL;
    m->constant_offset = m->constant_length = NULL;
    m->constant_data = NULL;
}

/* A module's instruction as the runtime holds it: an anchor instruction (027's 4c) is a cursor's when its anchor is in
 * a text and an index's when in a list (the state's, or the text or list it names), else a cursor's that the verifier
 * refuses (what it names is no text); any other is itself. */
static uint8_t anchor_step(const PureModule *m, uint8_t op, uint32_t arg) {
    if (op == PURE_OP_ANCHOR_LOAD || op == PURE_OP_ANCHOR_STORE) {
        bool index = arg < m->states && m->state_type[arg] == PURE_STATE_INDEX;
        return (uint8_t)(op == PURE_OP_ANCHOR_LOAD ? (index ? PURE_OP_INDEX_LOAD : PURE_OP_CUR_LOAD)
                                                   : (index ? PURE_OP_INDEX_STORE : PURE_OP_CUR_STORE));
    }
    bool list = (arg & 0xFFFF) < m->states && m->state_type[arg & 0xFFFF] == PURE_STATE_LIST;
    switch (op) {
    case PURE_OP_ANCHOR_START: return list ? PURE_OP_INDEX_FIRST : PURE_OP_CUR_START;
    case PURE_OP_ANCHOR_END: return list ? PURE_OP_INDEX_LAST : PURE_OP_CUR_END;
    case PURE_OP_ANCHOR_MOVE: return list ? PURE_OP_INDEX_MOVE : PURE_OP_CUR_MOVE;
    case PURE_OP_ANCHOR_COUNT: return list ? PURE_OP_INDEX_NUM : PURE_OP_CUR_COUNT;
    case PURE_OP_ANCHOR_AT: return list ? PURE_OP_INDEX_AT : PURE_OP_CUR_AT;
    default: return op;
    }
}

/* Block's instructions marked in held, as whose (1 a view's, 2 a handler's or a function's): the first of them that is a
 * handler's or a function's and another block's too (views may share one), else twice as it was. */
PURE_COLD static uint32_t hold_block(unsigned char *held, PureBlock block, unsigned char whose, uint32_t twice) {
    for (uint32_t pc = block.start; pc < block.start + block.length; ++pc) {
        if ((held[pc] & 2 || (held[pc] && whose == 2)) && twice == UINT32_MAX) twice = pc;
        held[pc] |= whose;
    }
    return twice;
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
    if (!p || size < PURE_MODULE_HEADER || memcmp(p, magic, sizeof(magic))) REFUSE("not a Pure module v7", 0);
    out->states = (uint16_t)read16(p + 8);
    out->inputs = (uint16_t)read16(p + 10);
    out->views = (uint16_t)read16(p + 12);
    out->handlers = (uint16_t)read16(p + 14);
    out->code_count = read32(p + 16);
    uint32_t strings = read32(p + 20);
    out->fields = (uint16_t)read16(p + 24);
    out->constants = (uint16_t)read16(p + 26);
    out->records = (uint16_t)read16(p + 28);
    uint32_t constant_bytes = read32(p + 30);
    out->record_fields = (uint16_t)read16(p + 34);
    out->functions = (uint16_t)read16(p + 36);
    out->state_words = (uint16_t)((out->states + 63) / 64 + !out->states);
    if (out->code_count > PURE_MODULE_CODE || strings > PURE_MODULE_STRINGS ||
        out->constants > PURE_MODULE_CONSTANTS || constant_bytes > PURE_MODULE_CONSTANT_BYTES ||
        out->functions > PURE_MODULE_FUNCTIONS) {
        result = PURE_LIMIT;
        REFUSE("module exceeds a limit", 8);
    }
    size_t functions_at = PURE_MODULE_HEADER + (size_t)out->states * STATE_BYTES + (size_t)out->inputs * INPUT_BYTES +
        (size_t)out->fields * FIELD_BYTES + (size_t)out->constants * CONSTANT_BYTES +
        (size_t)out->records * RECORD_BYTES + (size_t)out->record_fields * RECORD_FIELD_BYTES +
        (size_t)out->views * VIEW_BYTES + (size_t)out->handlers * HANDLER_BYTES;
    size_t signatures = 0; /* the functions' parameters' and results' items, in their order */
    if (size < functions_at + (size_t)out->functions * FUNCTION_BYTES) REFUSE("module size", functions_at);
    for (uint32_t i = 0; i < out->functions; ++i) signatures += (size_t)p[functions_at + i * FUNCTION_BYTES + 8] +
                                                                 p[functions_at + i * FUNCTION_BYTES + 9];
    if (signatures > PURE_MODULE_SIGNATURES) {
        result = PURE_LIMIT;
        REFUSE("module exceeds a limit", functions_at);
    }
    size_t expected = functions_at + (size_t)out->functions * FUNCTION_BYTES + signatures * SIGNATURE_BYTES +
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
    if (!(out->height = calloc(out->code_count + 1, 1)) || !(out->reuse_from = calloc(out->code_count + 1, sizeof(uint32_t))) ||
        !(out->reuse_length = calloc(out->code_count + 1, sizeof(uint32_t)))) {
        result = PURE_ALLOCATION;
        REFUSE("out of memory", 0);
    }
    out->constant_offset = (uint32_t *)(out->code + out->code_count);
    out->constant_length = out->constant_offset + out->constants;
    out->constant_data = (unsigned char *)(out->constant_length + out->constants);
    /* Its tables, the columns the load makes among the views (027's 4e: a record's fields for each list view of a
     * record), counted from the bytes first. */
    size_t records_at = functions_at - (size_t)out->views * VIEW_BYTES - (size_t)out->handlers * HANDLER_BYTES -
                        (size_t)out->record_fields * RECORD_FIELD_BYTES - (size_t)out->records * RECORD_BYTES;
    size_t made = 0, views_at = records_at + (size_t)out->records * RECORD_BYTES + (size_t)out->record_fields * RECORD_FIELD_BYTES;
    for (uint32_t i = 0; i < out->views; ++i) {
        const unsigned char *v = p + views_at + (size_t)i * VIEW_BYTES;
        uint32_t record = read16(v + 8);
        if (v[4] == PURE_VIEW_LIST && !read16(v + 6) && record && record <= out->records)
            made += read16(p + records_at + (size_t)(record - 1) * RECORD_BYTES + 6);
    }
    if (out->views + made > UINT16_MAX) REFUSE("views and their columns", out->views); /* (numbered in u16s) */
    size_t capacity = out->views + made;
    if (!make_tables(out, capacity, signatures)) {
        result = PURE_ALLOCATION;
        REFUSE("out of memory", 0);
    }
    const unsigned char *at = p + PURE_MODULE_HEADER;
    const unsigned char *text = p + size - constant_bytes - strings;
    if (constant_bytes) memcpy(out->constant_data, p + size - constant_bytes, constant_bytes);
    /* A module's state types as the runtime holds them: an anchor (027's 4c) a cursor here, an index once it is found to
     * be in a list (below). */
    static const uint8_t held_as[] = {[PURE_MODULE_NUM] = PURE_STATE_NUM, [PURE_MODULE_TEXT] = PURE_STATE_TEXT,
                                      [PURE_MODULE_ANCHOR] = PURE_STATE_CURSOR, [PURE_MODULE_LIST] = PURE_STATE_LIST,
                                      [PURE_MODULE_BOOL] = PURE_STATE_BOOL, [PURE_MODULE_I64] = PURE_STATE_I64,
                                      [PURE_MODULE_BYTES] = PURE_STATE_TEXT};
    const unsigned char *states_at = at;
    for (uint32_t i = 0; i < out->states; ++i, at += STATE_BYTES) {
        uint32_t type = read32(at + 4);
        if ((type & ~(uint32_t)PURE_STATE_TRANSIENT) > PURE_MODULE_BYTES) REFUSE("state type", i);
        out->state_type[i] = held_as[type & 0xFF];
        out->state_transient[i] = type & PURE_STATE_TRANSIENT;
        /* A number's, a bool's or an i64's value is its 8 bytes; others have flags:
         * a mark (cursor), a keyed list's key field + 1, or a list of a message's rows. */
        bool value = out->state_type[i] == PURE_STATE_NUM || out->state_type[i] == PURE_STATE_BOOL ||
                     out->state_type[i] == PURE_STATE_I64;
        uint32_t flags = value ? 0 : read32(at + 12);
        bool cursor = out->state_type[i] == PURE_STATE_CURSOR, list = out->state_type[i] == PURE_STATE_LIST;
        bool rows = list && flags & PURE_LIST_ROWS; /* an event's list, keyed or not */
        bool bound = list && flags & PURE_LIST_BOUND; /* a register: an event's list, not keyed */
        uint32_t key = list ? flags & ~(uint32_t)(PURE_LIST_ROWS | PURE_LIST_BOUND) : 0;
        if (bound && (!rows || key)) REFUSE("state type", i);
        /* What the states hold counts a text and a list, a message's rows aside (they are its own, gone when it ends). */
        out->state_counted[i] = out->state_type[i] == PURE_STATE_TEXT || (list && !rows);
        if (rows && type & PURE_STATE_TRANSIENT) REFUSE("state type", i); /* an event's own, saved or not */
        /* A list of a message's rows is named by the field's path (input.field), no other state. */
        if (!name_at(text, strings, read32(at), rows ? NAME_PATH : NAME_IDENTIFIER, out->state_names[i]) ||
            (rows && !strchr(out->state_names[i], '.')))
            REFUSE("state name", i);
        if (out->state_type[i] == PURE_STATE_I64) {
            out->initial[i].i64 = (int64_t)(read32(at + 8) | (uint64_t)read32(at + 12) << 32);
        } else if (value) {
            out->initial[i].num = read_double(at + 8);
            if (!isfinite(out->initial[i].num)) REFUSE("non-finite initial value", i);
            if (out->state_type[i] == PURE_STATE_BOOL && !canonical_bool(out->initial[i].num)) REFUSE("bool initial value", i);
        } else {
            if ((flags && !(cursor && flags == PURE_CURSOR_STAYS) && !(list && key <= PURE_RECORD_FIELDS)) ||
                read32(at + 8) > UINT16_MAX) REFUSE("state reference", i);
            out->state_ref[i] = (uint16_t)read32(at + 8);
            out->state_flags[i] = cursor ? (uint8_t)flags : list ? (uint8_t)((rows ? PURE_LIST_ROWS >> 8 : 0) | (bound ? PURE_LIST_BOUND >> 8 : 0))
                                  : (type & 0xFF) == PURE_MODULE_BYTES ? (uint8_t)PURE_TEXT_BYTES : 0;
            out->state_key[i] = (uint8_t)key;
        }
    }
    /* An anchor in a list is an index (a mark is a text's alone). */
    for (uint32_t i = 0; i < out->states; ++i) {
        uint32_t ref = out->state_ref[i];
        if (out->state_type[i] != PURE_STATE_CURSOR || ref >= out->states ||
            (read32(states_at + (size_t)ref * STATE_BYTES + 4) & 0xFF) != PURE_MODULE_LIST) continue;
        if (out->state_flags[i]) REFUSE("state reference", i);
        out->state_type[i] = PURE_STATE_INDEX;
    }
    uint32_t next_field = 0;
    const unsigned char *fields = at + (size_t)out->inputs * INPUT_BYTES;
    unsigned outputs = 0; /* a store names them by a byte's count (the links, the outbox's image): 255 at most */
    for (uint32_t i = 0; i < out->inputs; ++i, at += INPUT_BYTES) {
        PureInputShape *shape = &out->input[i];
        shape->kind = at[4];
        if (shape->kind == PURE_KIND_OUTPUT && ++outputs > UINT8_MAX) REFUSE("outputs past 255", i);
        shape->binding = at[5];
        shape->field_start = (uint16_t)read16(at + 12);
        shape->field_count = (uint16_t)read16(at + 14);
        /* Arguments bear their view's name (view.args), and a message may bear a path (an aup's hidden one: up.up,
         * list.point); an output is an identifier. */
        if (!name_at(text, strings, read32(at), shape->kind == PURE_KIND_OUTPUT ? NAME_IDENTIFIER : NAME_PATH, out->input_names[i]))
            REFUSE("input name", i);
        bool arguments = shape->kind == PURE_KIND_ARGUMENTS, keyed = shape->binding == PURE_BIND_PRESS || shape->binding == PURE_BIND_RELEASE;
        if (read16(at + 6) || shape->binding > PURE_BIND_POINT || (shape->binding && shape->kind != PURE_KIND_MESSAGE))
            REFUSE("input flags", i);
        /* What a binding names (027's 4d): a key, a timer's period, a pointing's table (checked with the views). */
        if (keyed) {
            if (!name_at(text, strings, read32(at + 8), NAME_PRINTABLE, out->input_keys[i]) || !key_name(out->input_keys[i]))
                REFUSE("input key", i);
            for (uint32_t k = 0; k < i; ++k)
                if (out->input[k].binding == shape->binding && !strcmp(out->input_keys[k], out->input_keys[i]))
                    REFUSE("duplicate key", i);
        }
        if (shape->binding == PURE_BIND_TIMER && (!(shape->period = read32(at + 8)) || shape->period > PURE_TIMER_LONGEST))
            REFUSE("timer input", i);
        shape->view = arguments || shape->binding == PURE_BIND_POINT ? read32(at + 8) : 0;
        if ((shape->kind != PURE_KIND_MESSAGE && shape->kind != PURE_KIND_OUTPUT && !arguments) ||
            (read32(at + 8) && !arguments && !shape->binding) || shape->field_start != next_field ||
            shape->field_count > out->fields - next_field || shape->field_count > PURE_MESSAGE_FIELDS) REFUSE("message shape", i);
        /* A binding's fields: a key's and a timer's none, a text's one text, a frame's one number, a pointing's one u32. */
        static const uint8_t bound_field[] = {[PURE_BIND_TEXT] = PURE_FIELD_TEXT, [PURE_BIND_FRAME] = PURE_FIELD_NUMBER,
                                              [PURE_BIND_POINT] = PURE_FIELD_U32};
        if (shape->binding && shape->field_count != (bound_field[shape->binding] != 0)) REFUSE("binding's fields", i);
        if (shape->binding && shape->field_count &&
            read32(fields + (size_t)next_field * FIELD_BYTES + 4) != bound_field[shape->binding]) REFUSE("binding's fields", i);
        for (uint32_t k = 0; shape->binding == PURE_BIND_FRAME && k < i; ++k)
            if (out->input[k].binding == PURE_BIND_FRAME) REFUSE("second frame input", i);
        for (uint32_t f = 0; f < shape->field_count; ++f) {
            const unsigned char *entry = fields + (size_t)(next_field + f) * FIELD_BYTES;
            PureField *field = &out->field[next_field + f];
            uint32_t type = read32(entry + 4);
            field->type = (uint8_t)type;
            field->rows = (uint16_t)(type >> 8); /* a rows field's list state */
            if (!name_at(text, strings, read32(entry), NAME_PATH, field->name) || field->type < PURE_FIELD_U32 ||
                field->type > PURE_FIELD_I64 || (field->type != PURE_FIELD_ROWS && type >> 8) || type >> 24)
                REFUSE("message field", next_field + f);
            /* Rows fill the list named by the input's name and the field's path; an output sends none. */
            if (field->type == PURE_FIELD_ROWS) {
                char path[2 * PURE_MODULE_NAME + 2];
                snprintf(path, sizeof(path), "%s.%s", out->input_names[i], field->name);
                if (shape->kind != PURE_KIND_MESSAGE || field->rows >= out->states ||
                    out->state_type[field->rows] != PURE_STATE_LIST || !(out->state_flags[field->rows] & PURE_LIST_ROWS >> 8) ||
                    out->state_key[field->rows] || strcmp(out->state_names[field->rows], path))
                    REFUSE("rows field", next_field + f);
            }
            /* A name is a field or a group, never both (user and user.name). */
            for (uint32_t k = 0; k < f; ++k) {
                const char *a = out->field[next_field + k].name, *b = field->name;
                size_t la = strlen(a), lb = strlen(b);
                if (!strcmp(a, b)) REFUSE("duplicate field", next_field + f);
                if ((la < lb && !memcmp(a, b, la) && b[la] == '.') || (lb < la && !memcmp(a, b, lb) && a[lb] == '.'))
                    REFUSE("field and group", next_field + f);
            }
        }
        shape->prefix = pure_fields_lay_out(&out->field[next_field], shape->field_count);
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
            uint32_t type = read32(entry + 4); /* PureRecordType */
            out->record_field_type[f] = (uint8_t)type;
            if (!name_at(text, strings, read32(entry), NAME_PATH, out->record_field_names[f]) || type > PURE_RECORD_BYTES)
                REFUSE("record field", f);
            for (uint32_t k = record->first; k < f; ++k)
                if (!strcmp(out->record_field_names[k], out->record_field_names[f])) REFUSE("duplicate record field", f);
            /* A default: a text's or bytes' constant, an i64's 8 bytes, a number (a bool's +0 or 1). */
            if (pure_record_bytes((uint8_t)type)) out->record_field_default[f].num = read32(entry + 8);
            else if (type == PURE_RECORD_I64 || type == PURE_RECORD_LIST) /* a LIST's: the record of its items */
                out->record_field_default[f].i64 = (int64_t)(read32(entry + 8) | (uint64_t)read32(entry + 12) << 32);
            else out->record_field_default[f].num = read_double(entry + 8);
            double given = out->record_field_default[f].num;
            if (pure_record_bytes((uint8_t)type) ? read32(entry + 12) || read32(entry + 8) >= out->constants
                : type == PURE_RECORD_LIST ? (uint64_t)out->record_field_default[f].i64 >= out->records
                : type != PURE_RECORD_I64 && (!isfinite(given) || (type == PURE_RECORD_BOOL && !canonical_bool(given))))
                REFUSE("record field default", f);
            if (type == PURE_RECORD_LIST) out->record_nested[i] = true;
            if (pure_record_bytes((uint8_t)type)) out->record_held[i] |= (uint16_t)(1u << (f - record->first));
        }
        next_record_field += record->count;
    }
    if (next_record_field != out->record_fields) REFUSE("unused record fields", next_record_field);
    at = fields_at + (size_t)out->record_fields * RECORD_FIELD_BYTES;
    for (uint32_t i = 0; i < out->states; ++i)
        if ((out->state_type[i] == PURE_STATE_TEXT && out->state_ref[i] >= out->constants) ||
            (out->state_type[i] == PURE_STATE_CURSOR &&
             (!state_of(out, out->state_ref[i], PURE_STATE_TEXT) || out->state_flags[out->state_ref[i]] & PURE_TEXT_BYTES)) ||
            (out->state_type[i] == PURE_STATE_LIST &&
             (out->state_ref[i] >= out->records || out->state_key[i] > out->record[out->state_ref[i]].count ||
              (out->state_key[i] &&
               out->record_field_type[out->record[out->state_ref[i]].first + out->state_key[i] - 1u] == PURE_RECORD_BOOL))) ||
            (out->state_type[i] == PURE_STATE_INDEX && !state_of(out, out->state_ref[i], PURE_STATE_LIST)))
            REFUSE("state reference", i);
    /* No list is keyed by a list or bytes in its records (a key is a number or a text); a register is no message's rows. */
    for (uint32_t i = 0; i < out->states; ++i) {
        if (out->state_type[i] != PURE_STATE_LIST || !out->state_key[i]) continue;
        uint8_t key = out->record_field_type[out->record[out->state_ref[i]].first + out->state_key[i] - 1u];
        if (key == PURE_RECORD_LIST) REFUSE("list in a record as a key", i);
        if (key == PURE_RECORD_BYTES) REFUSE("bytes as a key", i);
    }
    for (uint32_t f = 0; f < out->fields; ++f)
        if (out->field[f].type == PURE_FIELD_ROWS && out->state_flags[out->field[f].rows] & PURE_LIST_BOUND >> 8)
            REFUSE("rows field", f);
    /* A stored position into what is not stored would come back meaning nothing. */
    for (uint32_t i = 0; i < out->states; ++i)
        if ((out->state_type[i] == PURE_STATE_CURSOR || out->state_type[i] == PURE_STATE_INDEX) &&
            !out->state_transient[i] && out->state_transient[out->state_ref[i]])
            REFUSE("stored position in a transient state", i);
    /* Views (027's 4e): a list view names its list state (+ 1; its block a filter) or none (its block leaves its list),
     * and its items' record (+ 1; a map of a list state 0, its columns following it, each naming it + 1). */
    uint32_t declared = out->views;
    for (uint32_t i = 0; i < declared; ++i, at += VIEW_BYTES) {
        if (!name_at(text, strings, read32(at), NAME_IDENTIFIER, out->view_names[i])) REFUSE("view name", i);
        out->view_type[i] = at[4];
        out->view_table[i] = (uint16_t)read16(at + 6);
        out->view_record[i] = (uint16_t)read16(at + 8);
        out->view_code[i] = (PureBlock){read32(at + 12), read32(at + 16)};
        uint32_t table = out->view_table[i], record = out->view_record[i], list = read16(at + 10);
        bool listed = at[4] == PURE_VIEW_LIST;
        if (at[4] > PURE_VIEW_BYTES || at[5] || listed != (record || (!table && list)) || record > out->records ||
            (table && (table > i || !over_state(out, table - 1) || out->view_record[table - 1] || list)) ||
            (!listed && list))
            REFUSE("view type", i);
        if (listed && !table) {
            if (list ? !state_of(out, list - 1, PURE_STATE_LIST) || (record && record - 1 != out->state_ref[list - 1])
                     : !out->view_code[i].length)
                REFUSE("list view's list", i);
            out->view_list[i] = (uint16_t)(list ? list - 1 : 0);
            out->view_computed[i] = !list;
        } else if (table) out->view_list[i] = out->view_list[table - 1];
    }
    /* A list view of a record (a list state as it is, or a list its block leaves) has the record's fields as its
     * columns, made here: each reads its field of the row's item as it is. */
    for (uint32_t i = 0; i < declared; ++i) {
        if (!list_view(out, i) || !out->view_record[i]) continue;
        const PureRecord *r = &out->record[out->view_record[i] - 1];
        for (unsigned f = 0; f < r->count; ++f) {
            if (out->views == capacity) REFUSE("views and their columns", i);
            uint32_t c = out->views++, field = r->first + f;
            memcpy(out->view_names[c], out->record_field_names[field], PURE_MODULE_NAME);
            out->view_type[c] = out->record_field_type[field];
            out->view_table[c] = (uint16_t)(i + 1);
            out->view_list[c] = out->view_list[i];
            out->view_record[c] = (uint16_t)(out->view_type[c] == PURE_VIEW_LIST ? nested_record(out, field) + 1 : 0);
            out->view_field[c] = (uint16_t)(f + 1);
            out->view_computed[c] = out->view_computed[i];
            out->view_code[c] = (PureBlock){0, 0};
        }
    }
    /* Arguments are a view's (not a column's: a table's columns and filter
     * read the table's), one set a view, bearing its name. */
    for (uint32_t i = 0; i < out->inputs; ++i) {
        const PureInputShape *shape = &out->input[i];
        /* A pointing points at a table's rows (027's 4d). */
        if (shape->binding == PURE_BIND_POINT && (shape->view >= declared || !over_state(out, shape->view)))
            REFUSE("pointing's table", i);
        if (shape->kind != PURE_KIND_ARGUMENTS) continue;
        char path[PURE_MODULE_NAME + 8];
        if (shape->view >= out->views || out->view_table[shape->view] || out->view_arguments[shape->view]) REFUSE("arguments", i);
        snprintf(path, sizeof(path), "%s.args", out->view_names[shape->view]);
        if (strcmp(path, out->input_names[i])) REFUSE("arguments", i);
        out->view_arguments[shape->view] = i + 1;
    }
    for (uint32_t i = 0; i < out->views; ++i)
        if (out->view_table[i]) out->view_arguments[i] = out->view_arguments[out->view_table[i] - 1];
    for (uint32_t i = 0; i < out->handlers; ++i, at += HANDLER_BYTES) {
        uint32_t event = read32(at);
        out->handler[i] = (PureHandler){event, {read32(at + 4), read32(at + 8)}};
        if (event >= out->inputs || out->input[event].kind != PURE_KIND_MESSAGE) REFUSE("handler event", i);
    }
    /* Functions (027's 2c): a block and the counts of its parameters and results, whose types follow every function's. */
    for (uint32_t i = 0, item = 0; i < out->functions; ++i, at += FUNCTION_BYTES) {
        out->function[i] = (PureFunction){.code = {read32(at), read32(at + 4)}, .first = (uint16_t)item, .params = at[8],
                                          .results = at[9]};
        if (read16(at + 10)) REFUSE("function padding", i);
        item += (uint32_t)at[8] + at[9];
    }
    for (uint32_t k = 0; k < signatures; ++k, at += SIGNATURE_BYTES) {
        out->signature_type[k] = at[0];
        out->signature_record[k] = (uint16_t)read16(at + 2);
        if (at[1] || at[0] > PURE_RECORD_BYTES ||
            (at[0] == PURE_RECORD_LIST ? out->signature_record[k] >= out->records : out->signature_record[k] != 0))
            REFUSE("function signature", k);
    }
    out->calls = out->functions > 0;
    for (uint32_t i = 0; i < out->code_count; ++i, at += OP_BYTES) {
        if (at[1] || at[2] || at[3]) REFUSE("instruction padding", i);
        if (at[0] >= PURE_OP_COUNT) REFUSE("unknown opcode", i);
        out->code[i] = (PureOp){.op = anchor_step(out, at[0], read32(at + 4)), .arg = read32(at + 4), .value = read_double(at + 8)};
    }
    /* States, records, inputs and views share one namespace: in order of name, the first name again (in their order) is
     * refused. */
    Named *names = malloc(((size_t)out->states + out->records + out->inputs + out->views + 1) * sizeof(*names));
    if (!names) {
        result = PURE_ALLOCATION;
        REFUSE("out of memory", 0);
    }
    size_t count = 0, again = SIZE_MAX;
    for (uint32_t i = 0; i < out->states; ++i) names[count] = (Named){out->state_names[i], count}, ++count;
    for (uint32_t i = 0; i < out->records; ++i) names[count] = (Named){out->record_names[i], count}, ++count;
    for (uint32_t i = 0; i < out->inputs; ++i) names[count] = (Named){out->input_names[i], count}, ++count;
    for (uint32_t i = 0; i < out->views; ++i) if (!out->view_table[i]) names[count] = (Named){out->view_names[i], count}, ++count;
    qsort(names, count, sizeof(*names), by_name);
    for (size_t i = 1; i < count; ++i)
        if (!strcmp(names[i].name, names[i - 1].name) && names[i].index < again) again = names[i].index;
    free(names);
    if (again != SIZE_MAX) REFUSE("duplicate name", again);
    /* Column names are unique within their table. */
    for (uint32_t i = 0; i < out->views; ++i)
        for (uint32_t k = 0; k < i; ++k)
            if (out->view_table[i] && out->view_table[k] == out->view_table[i] &&
                !strcmp(out->view_names[i], out->view_names[k])) REFUSE("duplicate column", i);
    for (uint32_t i = 0; i < out->views; ++i) {
        bad = i;
        bool table = over_state(out, i);
        uint32_t event = out->view_arguments[i]; /* the arguments' input, as an arrival (its number + 1) */
        bool computes = !table && !out->view_table[i]; /* a view outside a list over a state may have a body */
        int kind = table ? FILTER : out->view_type[i] == PURE_VIEW_LIST ? LIST_OF + out->view_record[i] - 1 : out->view_type[i];
        if (!out->view_field[i] && (!table || out->view_code[i].length) &&
            (why = verify_block(out, out->view_code[i], kind, table || out->view_table[i] ? out->view_list[i] : -1, -1, event,
                                computes, 0, NULL, 0, -1, &bad)))
            goto refused;
        if (computes && !locals_agree(out, out->view_code[i])) { why = "local used as two things"; goto refused; }
        for (uint32_t pc = out->view_code[i].start; computes && pc < out->view_code[i].start + out->view_code[i].length; ++pc) {
            const PureOp *op = &out->code[pc];
            if (op->op == PURE_OP_LOCAL_LOAD || op->op == PURE_OP_LOCAL_STORE || op->op == PURE_OP_LOOP || op->op == PURE_OP_EACH)
                out->view_body[i] = true;
            if (op->op == PURE_OP_LIST_APPEND || op->op == PURE_OP_LIST_INSERT || op->op == PURE_OP_LIST_REMOVE)
                states_add(out->view_lists + (size_t)i * out->state_words, op->arg);
            if (op->op == PURE_OP_ITEM_SET) states_add(out->view_lists + (size_t)i * out->state_words, list_of(op->arg));
        }
        for (uint32_t w = 0; w < out->state_words; ++w)
            if (out->view_lists[(size_t)i * out->state_words + w]) out->view_body[i] = true;
    }
    /* What each function names, and the lists it changes with those the functions it calls change (until none adds). */
    uint64_t calls[PURE_MODULE_FUNCTIONS] = {0};
    for (uint32_t i = 0; i < out->functions; ++i)
        if (!function_names(out, i, &calls[i])) {
            result = PURE_ALLOCATION;
            REFUSE("out of memory", i);
        }
    for (bool more = out->functions > 0; more;) {
        more = false;
        for (uint32_t i = 0; i < out->functions; ++i)
            for (uint64_t left = calls[i]; left; left &= left - 1)
                for (uint32_t w = 0; w < out->state_words; ++w) {
                    uint64_t *mine = &out->function_changes[(size_t)i * out->state_words + w], was = *mine;
                    *mine |= out->function_changes[(size_t)__builtin_ctzll(left) * out->state_words + w];
                    more = more || *mine != was;
                }
    }
    for (uint32_t i = 0; i < out->functions; ++i) {
        bad = i;
        PureFunction *f = &out->function[i];
        if ((why = verify_block(out, f->code, FUNCTION, -1, -1, 0, true, 0, NULL, 0, (int)i, &bad))) goto refused;
        if (!locals_agree(out, f->code)) { why = "local used as two things"; goto refused; }
        for (uint32_t pc = f->code.start; pc < f->code.start + f->code.length; ++pc) /* its frame's stack: as the verifier went */
            if (out->height[pc] >= f->height) f->height = (uint8_t)(out->height[pc] + 1);
    }
    for (uint32_t i = 0; i < out->handlers; ++i) {
        bad = i;
        if ((why = verify_block(out, out->handler[i].code, -1, -1, -1, out->handler[i].event + 1, true, 0, NULL, 0, -1, &bad)))
            goto refused;
        if (!locals_agree(out, out->handler[i].code)) { why = "local used as two things"; goto refused; }
    }
    /* A handler's or a function's instruction is its block's alone: what the load proves of it (its height, its plan)
     * is that block's (one shared gave a function a handler's plan: a list lent there was the call's, emptied at its
     * return); views may share a block. One in no block never runs and was never verified: it becomes CONST 0, so the
     * passes below, which read every instruction's arguments, read only what was verified. */
    unsigned char *held = calloc(out->code_count + 1, 1);
    if (!held) {
        result = PURE_ALLOCATION;
        REFUSE("out of memory", 0);
    }
    uint32_t twice = UINT32_MAX;
    for (uint32_t i = 0; i < out->handlers; ++i) twice = hold_block(held, out->handler[i].code, 2, twice);
    for (uint32_t i = 0; i < out->functions; ++i) twice = hold_block(held, out->function[i].code, 2, twice);
    for (uint32_t i = 0; i < out->views; ++i) twice = hold_block(held, out->view_code[i], 1, twice);
    for (uint32_t pc = 0; pc < out->code_count; ++pc)
        if (!held[pc]) out->code[pc] = (PureOp){.op = PURE_OP_CONST};
    free(held);
    if (twice != UINT32_MAX) REFUSE("instruction in two blocks", twice);
    if (!event_sets(out) || !call_keep_lists(out)) {
        result = PURE_ALLOCATION;
        REFUSE("out of memory", 0);
    }
    lend_values(out);
    for (uint32_t i = 0; i < out->views; ++i) {
        int given = (int)out->view_arguments[i] - 1;
        plan_folds(out, out->view_code[i], given);
        out->view_plan[i] = over_state(out, i) && out->view_code[i].length && block_has_lanes(out, out->view_code[i], given);
        PureRange range;
        if (out->view_plan[i] && block_range(out, out->view_code[i], out->view_list[i], true, &range))
            out->view_range[i] = keep_range(out, &range);
        if (!over_state(out, i)) range_code(out, out->view_code[i], given);
        /* A fold in a column runs for every row: one pass may answer them all. */
        for (uint32_t pc = out->view_code[i].start; out->view_table[i] && pc < out->view_code[i].start + out->view_code[i].length; ++pc)
            if (out->code[pc].op == PURE_OP_FOLD) {
                shape_fold(out, pc);
                pc += out->code[pc].arg >> 16;
            }
    }
    for (uint32_t i = 0; i < out->handlers; ++i) {
        int input = (int)out->handler[i].event;
        plan_folds(out, out->handler[i].code, input);
        range_code(out, out->handler[i].code, input);
    }
    /* What a cell is, read once here: an item's field, or a keyed list's key, a text (PLAN_TEXT) or not. */
    for (uint32_t pc = 0; pc < out->code_count; ++pc) {
        PureOp *op = &out->code[pc];
        if (op->op == PURE_OP_ITEM_GET || op->op == PURE_OP_ITEM_SET) op->plan = bytes_field(out, list_of(op->arg), item_field(op->arg));
        if (op->op == PURE_OP_ITEM_GET && !op->plan &&
            out->record_field_type[out->record[out->state_ref[list_of(op->arg)]].first + item_field(op->arg)] == PURE_RECORD_LIST)
            op->plan = PLAN_HOLDS; /* an event's list's list field: the value read holds that list */
        else if (op->op == PURE_OP_LIST_FIND || op->op == PURE_OP_LIST_HAS)
            op->plan = (op->plan & PLAN_FILTER) | text_field(out, op->arg, out->state_key[op->arg] - 1u);
    }
    /* A FIND whose index the next instruction, an ITEM_GET of its list, takes
     * at once (no jump lands between them) reads the field by key, as one
     * (PLAN_JOINED, docs/design/021): no position is made. */
    bool *landed = calloc(out->code_count + 1, sizeof(*landed));
    if (!landed) {
        result = PURE_ALLOCATION;
        REFUSE("out of memory", 0);
    }
    for (uint32_t pc = 0; pc < out->code_count; ++pc)
        if (out->code[pc].op == PURE_OP_JZ || out->code[pc].op == PURE_OP_JMP) landed[out->code[pc].arg] = true;
    for (uint32_t pc = 0; pc + 1 < out->code_count; ++pc) {
        PureOp *op = &out->code[pc], *next = op + 1;
        if (op->op == PURE_OP_LIST_FIND && next->op == PURE_OP_ITEM_GET && list_of(next->arg) == op->arg && !landed[pc + 1] &&
            out->record_field_type[out->record[out->state_ref[op->arg]].first + item_field(next->arg)] != PURE_RECORD_LIST)
            op->plan |= PLAN_JOINED;
    }
    /* A FIND whose index waits on the stack for an ITEM_SET of its list,
     * the code between in a line (no jump in or out) that touches no list but
     * to find by key (joined, or HAS): the item is set where it was found, a
     * HASHED list's slot, no position made (PLAN_ROW, docs/design/023). */
    for (uint32_t pc = 0; pc < out->code_count; ++pc) {
        PureOp *find = &out->code[pc];
        if (find->op != PURE_OP_LIST_FIND || find->plan & PLAN_JOINED) continue;
        int height = 1;
        for (uint32_t at = pc + 1; at < out->code_count && height >= 1 && !landed[at]; ++at) {
            const PureOp *o = &out->code[at];
            int pops = -1, pushes = 1;
            switch (o->op) {
            case PURE_OP_CONST: case PURE_OP_LOAD: case PURE_OP_LOCAL_LOAD: case PURE_OP_TEXT_CONST: case PURE_OP_TEXT_LOAD:
            case PURE_OP_FIELD: case PURE_OP_LIST_COUNT:
                pops = 0;
                break;
            case PURE_OP_NEG: case PURE_OP_NOT: case PURE_OP_FLOOR: case PURE_OP_NUM_TEXT: case PURE_OP_I64_NEG:
            case PURE_OP_I64_FROM: case PURE_OP_NUM_FROM: case PURE_OP_LIST_HAS:
                pops = 1;
                break;
            case PURE_OP_ADD: case PURE_OP_SUB: case PURE_OP_MUL: case PURE_OP_DIV: case PURE_OP_MIN: case PURE_OP_MAX:
            case PURE_OP_MOD: case PURE_OP_AND: case PURE_OP_OR: case PURE_OP_LT: case PURE_OP_LE: case PURE_OP_GT:
            case PURE_OP_GE: case PURE_OP_EQ: case PURE_OP_NE: case PURE_OP_CONCAT: case PURE_OP_TEXT_COMPARE:
            case PURE_OP_I64_ADD: case PURE_OP_I64_SUB: case PURE_OP_I64_MUL: case PURE_OP_I64_DIV: case PURE_OP_I64_MOD:
            case PURE_OP_I64_MIN: case PURE_OP_I64_MAX: case PURE_OP_I64_COMPARE:
                pops = 2;
                break;
            case PURE_OP_LIST_FIND: /* joined: its ITEM_GET with it (a key in, the field out) */
                if (o->plan & PLAN_JOINED && !landed[at + 1]) pops = 1, ++at;
                break;
            case PURE_OP_ITEM_SET:
                if (height == 2 && list_of(o->arg) == find->arg) {
                    find->plan |= PLAN_ROW;
                    out->code[at].plan |= PLAN_ROW;
                }
                break;
            default: break;
            }
            if (pops < 0) break;
            height += pushes - pops;
        }
    }
    free(landed);
    for (uint32_t pc = 0; pc < out->code_count; ++pc) out->held = out->held || out->code[pc].op == PURE_OP_BYTES_STORE;
    /* The lists an index state refers to: their inserts tell where (move_indices). */
    for (uint32_t i = 0; i < out->states; ++i)
        if (out->state_type[i] == PURE_STATE_INDEX) out->state_indexed[out->state_ref[i]] = true;
    if (out->batches && !pure_batch_plans(out)) {
        result = PURE_ALLOCATION;
        REFUSE("out of memory", 0);
    }
    for (uint32_t pc = 0; pc < out->code_count; ++pc) { /* a kept list's lists: their own steps (027's stage 3) */
        PureOp *op = &out->code[pc];
        bool item = op->op == PURE_OP_ITEM_GET || op->op == PURE_OP_ITEM_SET || op->op == PURE_OP_APPEND_IN;
        uint32_t list = item ? list_of(op->arg) : op->arg;
        if ((!item && op->op != PURE_OP_LIST_APPEND && op->op != PURE_OP_LIST_INSERT) || !kept_list(out, list) ||
            !out->record_nested[out->state_ref[list]] ||
            ((op->op == PURE_OP_ITEM_GET || op->op == PURE_OP_ITEM_SET) &&
             out->record_field_type[out->record[out->state_ref[list]].first + item_field(op->arg)] != PURE_RECORD_LIST))
            continue;
        op->op = (uint8_t)(op->op == PURE_OP_ITEM_GET ? PURE_OP_KEPT_GET : op->op == PURE_OP_ITEM_SET ? PURE_OP_KEPT_SET :
                           op->op == PURE_OP_LIST_APPEND ? PURE_OP_KEPT_APPEND : op->op == PURE_OP_LIST_INSERT ? PURE_OP_KEPT_INSERT :
                           PURE_OP_KEPT_APPEND_IN);
    }
    /* A view or a column reading a kept list's list counts what it adds as a body does, from none at each evaluation
     * (a reader's too, which no event began). */
    for (uint32_t i = 0; i < out->views; ++i)
        for (uint32_t pc = out->view_code[i].start; pc < out->view_code[i].start + out->view_code[i].length; ++pc)
            if (out->code[pc].op == PURE_OP_KEPT_GET) out->view_body[i] = true;
    /* How deep its loops nest (027's 4f-4): past what a run keeps on its stack, its blocks run in the executor that calls,
     * its loops in its app's room. */
    if ((out->nesting = nesting_of(out)) == UINT32_MAX) {
        result = PURE_ALLOCATION;
        REFUSE("out of memory", 0);
    }
    out->runner = out->calls || out->nesting > PURE_MODULE_NESTING ? 2 : out->batches ? 1 : 0;
    out->jit = pure_jit_make(out); /* none: the executor runs it all */
    static uint64_t loads;
    out->serial = __atomic_add_fetch(&loads, 1, __ATOMIC_RELAXED);
    return PURE_OK;
refused:
    if (error_size) snprintf(error, error_size, "%s (%u)", why, bad);
    pure_module_release(out);
    return result;
#undef REFUSE
}

static int find(char (*names)[PURE_MODULE_NAME], unsigned count, const char *name) {
    for (unsigned i = 0; i < count; ++i)
        if (!strcmp(names[i], name)) return (int)i;
    return -1;
}

int pure_module_state(const PureModule *m, const char *name) { return find(m->state_names, m->states, name); }
int pure_module_input(const PureModule *m, const char *name) { return find(m->input_names, m->inputs, name); }
static uint32_t fnv(uint32_t h, const void *bytes, size_t size) {
    const unsigned char *p = bytes;
    for (size_t i = 0; i < size; ++i) h = (h ^ p[i]) * 16777619u;
    return h;
}

uint32_t pure_module_shape(const PureModule *m, uint32_t input) {
    const PureInputShape *s = &m->input[input];
    uint32_t h = fnv(2166136261u, &s->prefix, sizeof(s->prefix));
    for (uint32_t f = 0; f < s->field_count; ++f) {
        const PureField *field = &m->field[s->field_start + f];
        unsigned char fixed[7] = {field->type, (unsigned char)field->rows, (unsigned char)(field->rows >> 8),
                                  (unsigned char)field->offset, (unsigned char)(field->offset >> 8),
                                  (unsigned char)(field->offset >> 16), (unsigned char)(field->offset >> 24)};
        h = fnv(fnv(h, field->name, strlen(field->name) + 1), fixed, sizeof(fixed));
    }
    return h;
}

size_t pure_module_fields(const PureModule *m, uint32_t input, unsigned char out[PURE_FIELDS_NAMED]) {
    const PureInputShape *s = &m->input[input];
    size_t at = 0;
    out[at++] = (unsigned char)s->field_count;
    for (uint32_t f = 0; f < s->field_count; ++f) {
        const PureField *field = &m->field[s->field_start + f];
        size_t length = strlen(field->name);
        out[at++] = field->type;
        out[at++] = (unsigned char)length;
        memcpy(out + at, field->name, length);
        at += length;
    }
    return at;
}

/* Fields as a sender names them, laid out: their count, or -1 when they are
 * not well named (no rows: a sender sends none). */
static int named_fields(const unsigned char *bytes, size_t size, PureField *fields) {
    if (!size || bytes[0] > PURE_MESSAGE_FIELDS) return -1;
    unsigned count = bytes[0];
    size_t at = 1;
    for (unsigned f = 0; f < count; ++f) {
        size_t length = size - at >= 2 ? bytes[at + 1] : 0;
        if (size - at < 2 || !length || length >= PURE_MODULE_NAME || size - at < 2 + length || !bytes[at] ||
            bytes[at] == PURE_FIELD_ROWS || bytes[at] > PURE_FIELD_I64)
            return -1;
        fields[f] = (PureField){.type = bytes[at]};
        memcpy(fields[f].name, bytes + at + 2, length);
        for (unsigned k = 0; k < f; ++k)
            if (!strcmp(fields[k].name, fields[f].name)) return -1;
        at += 2 + length;
    }
    if (at != size) return -1;
    pure_fields_lay_out(fields, count);
    return (int)count;
}

bool pure_module_takes(const PureModule *m, uint32_t input, const unsigned char *fields, size_t size) {
    PureField sent[PURE_MESSAGE_FIELDS];
    int count = named_fields(fields, size, sent);
    if (count < 0) return false;
    const PureInputShape *s = &m->input[input];
    for (int k = 0; k < count; ++k) {
        uint32_t f = 0;
        while (f < s->field_count && strcmp(m->field[s->field_start + f].name, sent[k].name)) ++f;
        if (f == s->field_count) return false; /* its value would be dropped */
        uint8_t type = m->field[s->field_start + f].type, from = sent[k].type;
        if (type != from && !((type == PURE_FIELD_NUMBER || type == PURE_FIELD_I64) && from == PURE_FIELD_U32) &&
            !(type == PURE_FIELD_BYTES && from == PURE_FIELD_TEXT))
            return false;
    }
    return true;
}

PureResult pure_module_recast(const PureModule *m, uint32_t input, const unsigned char *fields, size_t size,
                              PureSpan payload, unsigned char **out, size_t *out_size) {
    PureField sent[PURE_MESSAGE_FIELDS];
    int count = named_fields(fields, size, sent);
    *out = NULL;
    *out_size = 0;
    if (count < 0) return PURE_STORE_ERROR;
    const PureInputShape *s = &m->input[input];
    return pure_send_recast(sent, (uint32_t)count, &m->field[s->field_start], s->field_count, payload, out, out_size);
}

int pure_module_view(const PureModule *m, const char *name) {
    for (uint32_t i = 0; i < m->views; ++i)
        if (!m->view_table[i] && !strcmp(m->view_names[i], name)) return (int)i;
    return -1;
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


/* Whether two values that order as order (below zero: the first below) are related as a relation says (PureRelation). */
static bool related(uint32_t relation, int order) {
    return relation == PURE_LESS ? order < 0 : relation == PURE_LESS_EQUAL ? order <= 0 : relation == PURE_GREATER ? order > 0 :
           relation == PURE_GREATER_EQUAL ? order >= 0 : relation == PURE_EQUAL ? order == 0 : order != 0;
}

static PureResult fault(PureApp *app, PureFault kind, uint32_t pc) {
    app->fault = kind;
    app->fault_pc = pc;
    return kind == PURE_FAULT_LIMIT || kind == PURE_FAULT_WORK ? PURE_LIMIT : kind == PURE_FAULT_NOT_FOUND ? PURE_NOT_FOUND :
           kind == PURE_FAULT_EXISTS ? PURE_CONFLICT : kind == PURE_FAULT_MEMORY ? PURE_BUDGET : PURE_INVALID;
}

/* Count what the unit running (an event, or a view's evaluation) is about
 * to run, as written: false past the app's work budget (PURE_WORK_DEFAULT
 * when it sets none), which what it counts never passes. */
static inline bool charge(PureApp *app, uint64_t instructions) {
    uint64_t budget = app->work_budget ? app->work_budget : PURE_WORK_DEFAULT;
    if (instructions > budget - app->work) return false;
    app->work += instructions;
    return true;
}

/* What a state (or an event's list) holds: its stored form, and what the
 * app keeps for it besides while the event runs (a list's records to save
 * and to undo). */
typedef struct { size_t stored, kept; } Held;

static inline Held list_held(const PureApp *app, const PureList *list) {
    return (Held){pure_list_size(list), pure_list_changes(list).size + pure_undo_size(&app->undo) + pure_list_extra(list)};
}

static inline Held text_held(const PureApp *app, uint32_t text) { return (Held){pure_draft_size(app->texts[text]), 0}; }

/* A state of the event running went from before to after: false once what
 * the event has added passes the memory budget (in stored form, as the
 * budget counts), or what it holds for it all passes its room. */
static inline bool grows(PureApp *app, Held before, Held after) {
    app->grown += (int64_t)after.stored - (int64_t)before.stored;
    app->spent += (int64_t)(after.stored + after.kept) - (int64_t)(before.stored + before.kept);
    return (!app->memory || app->grown <= (int64_t)app->memory) && app->spent <= (int64_t)app->room;
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
    case FROM_ARGUMENTS: direct = app->arguments.data + v.offset; break;
    case FROM_COPY: direct = app->copied + v.offset; break;
    case FROM_CONSTANT: direct = app->module->constant_data + v.offset; break;
    case FROM_ARENA: direct = arena->bytes + v.offset; break;
    case FROM_HELD: direct = app->held[v.owner].data + v.offset; break;
    default: /* FROM_TEXT */
        return pure_draft_read_version(app->texts[v.owner], v.version, (size_t)v.offset, (size_t)v.length,
                                       visit, context);
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
    if (v.source == FROM_PAYLOAD || v.source == FROM_COPY || v.source == FROM_CONSTANT || v.source == FROM_ARGUMENTS) {
        const unsigned char *base = v.source == FROM_PAYLOAD ? app->payload.data : v.source == FROM_ARGUMENTS ? app->arguments.data :
                                    v.source == FROM_COPY ? app->copied : app->module->constant_data;
        *out = (PureSpan){v.length ? base + v.offset : NULL, (size_t)v.length};
        return PURE_OK;
    }
    if (v.source == FROM_ARENA || v.source == FROM_HELD) {
        *out = (PureSpan){(v.source == FROM_ARENA ? arena->bytes : app->held[v.owner].data) + v.offset, (size_t)v.length};
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

static void move_cursors(const PureModule *m, PureValue *values, uint32_t text, size_t at,
                         size_t removed, size_t inserted) {
    for (uint32_t c = 0; c < m->states; ++c) {
        if (m->state_type[c] != PURE_STATE_CURSOR || m->state_ref[c] != text) continue;
        double position = values[c].num;
        if (removed) {
            if (position >= at + removed) position -= (double)removed;
            else if (position > at) position = (double)at;
        } else if (position > at || (position == at && !(m->state_flags[c] & PURE_CURSOR_STAYS)))
            position += (double)inserted;
        values[c].num = position;
    }
}

/* Indices follow their item across an insert or remove; an index whose item
 * was removed names the next item (or the last, or 0 in an empty list). */
static void move_indices(const PureModule *m, PureValue *values, uint32_t list, size_t count,
                         size_t at, bool inserted) {
    double last = count ? (double)(count - 1) : 0;
    for (uint32_t i = 0; i < m->states; ++i) {
        if (m->state_type[i] != PURE_STATE_INDEX || m->state_ref[i] != list) continue;
        if (inserted) {
            if (count > 1 && values[i].num >= (double)at) ++values[i].num;
        } else {
            if (values[i].num > (double)at) --values[i].num;
            values[i].num = minimum(values[i].num, last);
        }
    }
}



/* The cursor count units (arg's: a character or a line) from the start of the text arg names, in *at; false past the
 * text (027's 4c, AT). */
static __attribute__((cold, noinline)) bool cursor_at(const PureApp *app, uint32_t arg, double count, double *at) {
    PureDraft *text = app->texts[arg & 0xFFFF];
    PureVersion version = pure_draft_version(text);
    unsigned unit = arg >> PURE_ANCHOR_UNIT;
    if (count < 0) return false;
    size_t position = unit == PURE_UNIT_CHAR ? pure_text_step(text, version, 0, count) : pure_text_line_step(text, version, 0, count);
    if ((double)pure_text_count(text, version, position, unit) != count) return false;
    *at = (double)position;
    return true;
}

/* x / 2^n rounded down, whatever the sign (027's RIGHT). */
static inline int64_t floor_shift(int64_t x, unsigned n) { return x >= 0 ? x >> n : ~(~x >> n); }

static bool take_byte(void *context, PureSpan span) {
    if (span.size) *(unsigned char *)context = span.data[0];
    return true;
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

/* BYTES_STORE: the value held by name slot becomes v, or v after it (append); what it holds counts in the room.
 * 1 where that passes the room (MEMORY), 2 out of memory, else 0. A value of the payload, the arguments or the constants
 * lasts the event: it is held as it is (by_ref), nothing copied, until something is appended to it. */
static int hold_copy(PureApp *app, PureArena *arena, unsigned slot, PureBytes v, bool append);
static int hold(PureApp *app, PureArena *arena, unsigned slot, PureBytes v, bool append) {
    __typeof__(app->held[0]) *h = &app->held[slot];
    if (!append && (v.source == FROM_PAYLOAD || v.source == FROM_ARGUMENTS || v.source == FROM_CONSTANT)) {
        if (!h->by_ref) (void)grows(app, (Held){0, h->size}, (Held){0, 0});
        h->ref = v, h->size = (size_t)v.length, h->by_ref = true;
        return 0;
    }
    if (h->by_ref) { /* what it held as it was: copied first when v follows it */
        h->by_ref = false, h->size = 0;
        int held = append ? hold_copy(app, arena, slot, h->ref, false) : 0;
        if (held) return held;
    }
    return hold_copy(app, arena, slot, v, append);
}
static int hold_copy(PureApp *app, PureArena *arena, unsigned slot, PureBytes v, bool append) {
    __typeof__(app->held[0]) *h = &app->held[slot];
    size_t mark = arena->used, before = h->size, at = append ? h->size : 0, size = at + (size_t)v.length;
    if (v.source == FROM_HELD && v.owner == slot && v.length) { /* a part of itself: by way of the arena */
        if (append_value(app, arena, v) != PURE_OK) return 2;
        v = (PureBytes){FROM_ARENA, 0, {0, 0}, mark, v.length};
    }
    if (!grows(app, (Held){0, before}, (Held){0, size})) return 1;
    if (size > h->capacity) {
        size_t capacity = h->capacity ? h->capacity : 64;
        while (capacity < size) capacity *= 2;
        unsigned char *data = realloc(h->data, capacity);
        if (!data) return 2;
        h->data = data, h->capacity = capacity;
    }
    Copy copy = {h->data + at};
    if (visit_value(app, arena, v, copy_out, &copy) != PURE_OK) return 2;
    h->size = size;
    arena->used = mark;
    return 0;
}

/* ---- Lists in records (027's 2b): the event's lists made for LIST fields, numbered from 1 */

PURE_COLD static PureList *new_list(const PureModule *m, uint32_t record, bool kept);
static uint8_t cell_kind(const PureModule *m, uint32_t field, bool kept);

/* The values that share each list in the LIST fields of cells (an item's, just stored). */
static void share_cells(PureApp *app, const PureModule *m, uint32_t record, const PureCell *cells) {
    const PureRecord *r = &m->record[record];
    for (unsigned f = 0; f < r->count; ++f)
        if (m->record_field_type[r->first + f] == PURE_RECORD_LIST && cells[f].i64) ++app->nested.shared[cells[f].i64 - 1];
}

/* Room for one more of the event's lists in records; false out of memory. */
static bool nested_room(PureApp *app) {
    if (app->nested.count < app->nested.capacity) return true;
    size_t capacity = app->nested.capacity ? app->nested.capacity * 2 : 16;
    PureList **lists = realloc(app->nested.list, capacity * sizeof(*lists));
    if (lists) app->nested.list = lists;
    uint32_t *shared = lists ? realloc(app->nested.shared, capacity * sizeof(*shared)) : NULL;
    if (!shared) return false;
    app->nested.shared = shared;
    app->nested.capacity = capacity;
    return true;
}

/* An event's list as a value, itself (030's N4: nothing changes it after this, which the load proved): its number, or 0
 * out of memory. The event's list holds it too (its 1, never given back), so it is always shared and APPEND_IN copies it
 * before a change; it stays the event's to empty (drop_nested leaves it). */
#define PURE_NESTED_LENT (UINT32_C(1) << 31)
static uint64_t lend(PureApp *app, PureList *list) {
    if (!nested_room(app)) return 0;
    app->nested.list[app->nested.count] = list;
    app->nested.shared[app->nested.count] = PURE_NESTED_LENT | 1;
    return ++app->nested.count;
}

/* A new list of record's items among the event's (empty, or a copy of from's items): its number, or 0 out of memory. */
static uint64_t nest(PureApp *app, uint32_t record, const PureList *from) {
    const PureModule *m = app->module;
    if (!nested_room(app)) return 0;
    PureList *list = from && !m->record_nested[record] ? pure_list_copied(from) : NULL; /* numbers in arrays: at once */
    size_t at = list ? pure_list_count(from) : 0;
    if (!list && !(list = new_list(m, record, false))) return 0;
    const PureRecord *r = &m->record[record];
    for (size_t count = from ? pure_list_count(from) : 0; at < count; ++at) {
        PureCell cells[PURE_RECORD_FIELDS];
        for (unsigned f = 0; f < r->count; ++f) cells[f] = pure_list_get(from, at, f);
        if (pure_list_insert(list, at, cells, NULL) != PURE_OK) { pure_list_free(list); return 0; }
        if (m->record_nested[record]) share_cells(app, m, record, cells);
    }
    app->nested.list[app->nested.count] = list;
    app->nested.shared[app->nested.count] = 1; /* the value made holds it (a value is never counted again: so for good) */
    return ++app->nested.count;
}

/* The bound lists given back their own, and the event's lists in records let go (an event or a view begins): those
 * bound since the last time (LIST_BIND keeps them), not every state. */
static void unbind(PureApp *app) {
    for (uint32_t k = 0; k < app->bound_count; ++k) {
        uint32_t i = app->bound[k];
        if (app->unbound[i]) app->lists[i] = app->unbound[i], app->unbound[i] = NULL;
    }
    app->bound_count = 0;
}
static void drop_nested(PureApp *app) {
    for (size_t i = 0; i < app->nested.count; ++i)
        if (!(app->nested.shared[i] & PURE_NESTED_LENT)) pure_list_free(app->nested.list[i]); /* a lent one is its event's */
    app->nested.count = 0;
}

/* ---- Lists kept past an event (027's stage 3): a stored or transient list holds a list field's value in its items'
 * stored form (PURE_CELL_LIST: each item's fields in order, a number's, a bool's and an i64's 8 bytes little-endian, a
 * text's and a list's u32le length and bytes, a list's in this form again). Read, it becomes one of the event's lists;
 * set, it is written from one. */
/* The stored form's bytes of the event's list number list (0: an empty one) of record's items. */
static size_t value_size(const PureApp *app, uint32_t record, uint64_t list) {
    const PureModule *m = app->module;
    const PureRecord *r = &m->record[record];
    const PureList *items = list ? app->nested.list[list - 1] : NULL;
    size_t size = 0;
    for (size_t at = 0, count = items ? pure_list_count(items) : 0; at < count; ++at)
        for (unsigned f = 0; f < r->count; ++f) {
            uint8_t type = m->record_field_type[r->first + f];
            PureCell cell = pure_list_get(items, at, f);
            size += pure_record_bytes(type) ? 4 + cell.size :
                    type == PURE_RECORD_LIST ? 4 + value_size(app, nested_record(m, r->first + f), (uint64_t)cell.i64) : 8;
        }
    return size;
}

static unsigned char *value_put(const PureApp *app, uint32_t record, uint64_t list, unsigned char *out) {
    const PureModule *m = app->module;
    const PureRecord *r = &m->record[record];
    const PureList *items = list ? app->nested.list[list - 1] : NULL;
    for (size_t at = 0, count = items ? pure_list_count(items) : 0; at < count; ++at)
        for (unsigned f = 0; f < r->count; ++f) {
            uint8_t type = m->record_field_type[r->first + f];
            PureCell cell = pure_list_get(items, at, f);
            if (pure_record_bytes(type)) {
                write32(out, (uint32_t)cell.size);
                if (cell.size) memcpy(out + 4, cell.text, cell.size);
                out += 4 + cell.size;
            } else if (type == PURE_RECORD_LIST) {
                unsigned char *end = value_put(app, nested_record(m, r->first + f), (uint64_t)cell.i64, out + 4);
                write32(out, (uint32_t)(end - out - 4));
                out = end;
            } else {
                write_i64(out, cell.i64);
                out += 8;
            }
        }
    return out;
}

/* A list value's stored form (bytes) read into a new one of the event's lists of record's items (or appended to into, a
 * message's rows), its lists in fields too: its number (0: none, or into), or 0 with *made false where the bytes are not
 * that form or memory ran out (*bad: they are not). */
static uint64_t value_take(PureApp *app, uint32_t record, const unsigned char *bytes, size_t size, PureList *into, bool *made,
                           bool *bad) {
    const PureModule *m = app->module;
    const PureRecord *r = &m->record[record];
    *made = true;
    if (!size) return 0;
    uint64_t list = into ? 0 : nest(app, record, NULL);
    if (!into && !list) return *made = false, 0;
    for (size_t at = 0; at < size;) {
        PureCell cells[PURE_RECORD_FIELDS];
        for (unsigned f = 0; f < r->count; ++f) {
            uint32_t field = r->first + f;
            uint8_t type = m->record_field_type[field];
            if (pure_record_bytes(type) || type == PURE_RECORD_LIST) {
                if (size - at < 4) return *bad = true, *made = false, 0;
                uint32_t length = read32(bytes + at);
                if (length > size - at - 4 || (type == PURE_RECORD_TEXT && !pure_utf8_valid((PureSpan){bytes + at + 4, length})))
                    return *bad = true, *made = false, 0;
                cells[f] = (PureCell){.text = (unsigned char *)bytes + at + 4, .size = length};
                if (type == PURE_RECORD_LIST) {
                    cells[f] = (PureCell){.i64 = (int64_t)value_take(app, nested_record(m, field), bytes + at + 4, length, NULL, made,
                                                                     bad)};
                    if (!*made) return 0;
                }
                at += 4 + length;
            } else {
                if (size - at < 8) return *bad = true, *made = false, 0;
                cells[f] = (PureCell){.i64 = read_i64(bytes + at)};
                at += 8;
                if (!pure_cell_fits(pure_record_kind(m, field), cells[f])) return *bad = true, *made = false, 0;
            }
        }
        PureList *items = into ? into : app->nested.list[list - 1]; /* (the lists in its fields may have moved the table) */
        if (pure_list_insert(items, pure_list_count(items), cells, NULL) != PURE_OK) return *made = false, 0;
        if (m->record_nested[record]) share_cells(app, m, record, cells);
    }
    return list;
}

/* An event's list's value as a kept list's cell holds it: its stored form in the arena (room reserved first). */
static __attribute__((cold, noinline)) PureResult value_cell(PureApp *app, PureArena *arena, uint32_t record, uint64_t list,
                                                             PureCell *out) {
    size_t size = value_size(app, record, list);
    if (!reserve(arena, size)) return PURE_ALLOCATION;
    unsigned char *at = arena->bytes + arena->used;
    value_put(app, record, list, at);
    arena->used += size;
    *out = (PureCell){.text = at, .size = size};
    return PURE_OK;
}

/* An item's cells for a kept list of record's items whose fields hold lists, from the stack (base on): its texts and
 * its lists' stored forms in the arena, all the room reserved first (no span moves while the next is made). */
static __attribute__((cold, noinline)) PureResult kept_cells(PureApp *app, PureArena *arena, uint32_t record, const Slot *values,
                                                             PureCell *cells) {
    const PureModule *m = app->module;
    const PureRecord *r = &m->record[record];
    size_t total = 0;
    for (unsigned f = 0; f < r->count; ++f) {
        uint8_t type = m->record_field_type[r->first + f];
        total += type == PURE_RECORD_LIST ? value_size(app, nested_record(m, r->first + f), (uint64_t)values[f].i64) :
                 pure_record_bytes(type) ? (size_t)values[f].bytes.length : 0;
    }
    if (!reserve(arena, total)) return PURE_ALLOCATION;
    for (unsigned f = 0; f < r->count; ++f) {
        uint8_t type = m->record_field_type[r->first + f];
        PureSpan span = {0};
        PureResult status = PURE_OK;
        if (type == PURE_RECORD_LIST) status = value_cell(app, arena, nested_record(m, r->first + f), (uint64_t)values[f].i64, &cells[f]);
        else if (pure_record_bytes(type) && (status = contiguous(app, arena, values[f].bytes, &span)) == PURE_OK)
            cells[f] = (PureCell){.text = (unsigned char *)span.data, .size = span.size};
        else cells[f] = (PureCell){.i64 = values[f].i64};
        if (status != PURE_OK) return status;
    }
    return PURE_OK;
}

static PureUndo *undo_for(PureApp *app, uint32_t list);
static void move_indices(const PureModule *m, PureValue *values, uint32_t list, size_t count, size_t at, bool inserted);

/* A kept list's list value (a cell's stored form) read into one of the event's lists, counted in the event's room: its
 * number in *out; BUDGET past the room. */
static __attribute__((cold, noinline)) PureResult kept_value(PureApp *app, uint32_t list, uint32_t field, PureCell cell,
                                                             int64_t *out) {
    const PureModule *m = app->module;
    bool made, bad = false;
    uint32_t record = nested_record(m, m->record[m->state_ref[list]].first + field);
    uint64_t value = value_take(app, record, cell.text, cell.size, NULL, &made, &bad);
    if (!made) return bad ? PURE_STORE_ERROR : PURE_ALLOCATION;
    if (!grows(app, (Held){0, 0}, (Held){0, cell.size})) return PURE_BUDGET;
    *out = (int64_t)value;
    return PURE_OK;
}

/* APPEND_IN into a kept list's item (at): the list in its field read in, the item (its fields from values) appended,
 * the list written back (undone with the event). 0 done; 1 a fault (*fault); 2 refused and 3 stopped (*status). */
static __attribute__((cold, noinline)) int append_kept(PureApp *app, PureArena *arena, uint32_t list, uint32_t field, size_t at,
                                                       const Slot *values, PureFault *fault, PureResult *status) {
    const PureModule *m = app->module;
    uint32_t inner = nested_record(m, m->record[m->state_ref[list]].first + field);
    const PureRecord *record = &m->record[inner];
    int64_t value = 0;
    if ((*status = kept_value(app, list, field, pure_list_get(app->lists[list], at, field), &value)) != PURE_OK)
        return *status == PURE_BUDGET ? (*fault = PURE_FAULT_MEMORY, 1) : 3;
    uint64_t own = value ? (uint64_t)value : nest(app, inner, NULL);
    if (!own) return *status = PURE_ALLOCATION, 3;
    size_t mark = arena->used, total = 0;
    for (unsigned f = 0; f < record->count; ++f)
        if (pure_record_bytes(m->record_field_type[record->first + f])) total += (size_t)values[f].bytes.length;
    if (!reserve(arena, total)) return *status = PURE_ALLOCATION, 3;
    PureCell cells[PURE_RECORD_FIELDS];
    for (unsigned f = 0; f < record->count; ++f) {
        PureSpan span = {0};
        if (pure_record_bytes(m->record_field_type[record->first + f]) &&
            (*status = contiguous(app, arena, values[f].bytes, &span)) != PURE_OK)
            return 3;
        cells[f] = (PureCell){.i64 = values[f].i64, .text = (unsigned char *)span.data, .size = span.size};
    }
    PureList *in = app->nested.list[own - 1];
    *status = pure_list_insert(in, pure_list_count(in), cells, NULL);
    arena->used = mark;
    if (*status != PURE_OK) return 2;
    if (m->record_nested[inner]) share_cells(app, m, inner, cells);
    PureCell cell;
    Held before = list_held(app, app->lists[list]);
    if ((*status = value_cell(app, arena, inner, own, &cell)) != PURE_OK) return 3;
    *status = pure_list_set(app->lists[list], at, field, cell, undo_for(app, list));
    arena->used = mark;
    if (*status != PURE_OK) return 2;
    if (!grows(app, before, list_held(app, app->lists[list]))) return *fault = PURE_FAULT_MEMORY, 1;
    return 0;
}

/* How many values a kept step takes off the stack (net: KEPT_GET's value takes its index's place). */
static size_t kept_pops(const PureModule *m, const PureOp *op) {
    switch (op->op) {
    case PURE_OP_KEPT_GET: return 0;
    case PURE_OP_KEPT_SET: return 2;
    case PURE_OP_KEPT_APPEND: case PURE_OP_KEPT_INSERT: return m->record[m->state_ref[op->arg]].count + (op->op == PURE_OP_KEPT_INSERT);
    default: return m->record[nested_record(m, m->record[m->state_ref[list_of(op->arg)]].first + item_field(op->arg))].count + 1u;
    }
}

/* A kept step on the stack below sp: 0 done, else how it stopped and why: 1 a fault, 2 refused, 3 stopped (a result),
 * the fault or the result in the bits above. */
static __attribute__((cold, noinline)) int kept_step(PureApp *app, PureArena *arena, PureValue *values, const PureOp *op, Slot *stack,
                                                     size_t sp) {
#define KEPT_FAULT(f) return (int)(f) << 2 | 1
#define KEPT_REFUSED(r) return (int)(r) << 2 | 2
#define KEPT_STOPPED(r) return (int)(r) << 2 | 3
    const PureModule *m = app->module;
    uint32_t arg = op->arg, list = op->op == PURE_OP_KEPT_APPEND || op->op == PURE_OP_KEPT_INSERT ? arg : list_of(arg), field = item_field(arg);
    PureList *items = app->lists[list];
    size_t mark = arena->used;
    PureResult status;
    switch (op->op) {
    case PURE_OP_KEPT_GET: { /* the list in an item's field, read into one of the event's lists */
        size_t at = (size_t)stack[sp - 1].num;
        if (at >= pure_list_count(items)) KEPT_FAULT(PURE_FAULT_NOT_FOUND);
        if ((status = kept_value(app, list, field, pure_list_get(items, at, field), &stack[sp - 1].i64)) == PURE_BUDGET)
            KEPT_FAULT(PURE_FAULT_MEMORY);
        if (status != PURE_OK) KEPT_STOPPED(status);
        return 0;
    }
    case PURE_OP_KEPT_SET: { /* an item's field given a list, written in its stored form */
        size_t at = (size_t)stack[sp - 2].num;
        Held before = list_held(app, items);
        PureCell cell;
        if ((status = value_cell(app, arena, nested_record(m, m->record[m->state_ref[list]].first + field), (uint64_t)stack[sp - 1].i64,
                                 &cell)) != PURE_OK)
            KEPT_STOPPED(status);
        status = op->plan & PLAN_ROW ? pure_list_set_row(items, at, field, cell, undo_for(app, list))
                                     : pure_list_set(items, at, field, cell, undo_for(app, list));
        arena->used = mark;
        if (status != PURE_OK) KEPT_REFUSED(status);
        if (!grows(app, before, list_held(app, items))) KEPT_FAULT(PURE_FAULT_MEMORY);
        return 0;
    }
    case PURE_OP_KEPT_APPEND: case PURE_OP_KEPT_INSERT: { /* an item whose lists go in their stored form */
        const PureRecord *record = &m->record[m->state_ref[list]];
        size_t base = sp - record->count, count = pure_list_count(items), at = count;
        if (op->op == PURE_OP_KEPT_INSERT && (at = (size_t)stack[base - 1].num) > count) KEPT_FAULT(PURE_FAULT_NOT_FOUND);
        PureCell cells[PURE_RECORD_FIELDS];
        if ((status = kept_cells(app, arena, m->state_ref[list], stack + base, cells)) != PURE_OK) KEPT_STOPPED(status);
        Held before = list_held(app, items);
        status = PURE_INVALID;
        if (m->state_key[list]) { /* a keyed list places the item by its key */
            bool found;
            if ((status = pure_list_add(items, cells, undo_for(app, list), m->state_indexed[list] ? &at : NULL, &found)) == PURE_INVALID)
                at = pure_list_seek(items, m->state_key[list] - 1u, cells[m->state_key[list] - 1u], &found);
            if (found) { arena->used = mark; KEPT_FAULT(PURE_FAULT_EXISTS); }
        }
        if (status == PURE_INVALID) status = pure_list_insert(items, at, cells, undo_for(app, list));
        arena->used = mark;
        if (status != PURE_OK) KEPT_REFUSED(status);
        if (m->state_indexed[list]) move_indices(m, values, list, pure_list_count(items), at, true);
        if (!grows(app, before, list_held(app, items))) KEPT_FAULT(PURE_FAULT_MEMORY);
        return 0;
    }
    default: { /* APPEND_IN: an item into the list in an item's field */
        size_t base = sp - (kept_pops(m, op) - 1), at = (size_t)stack[base - 1].num;
        if (at >= pure_list_count(items)) KEPT_FAULT(PURE_FAULT_NOT_FOUND);
        PureFault fault = PURE_FAULT_NONE;
        int how = append_kept(app, arena, list, field, at, stack + base, &fault, &status);
        if (how == 1) KEPT_FAULT(fault);
        if (how == 2) KEPT_REFUSED(status);
        if (how == 3) KEPT_STOPPED(status);
        return 0;
    }
    }
#undef KEPT_FAULT
#undef KEPT_REFUSED
#undef KEPT_STOPPED
}

/* NUM_TEXT's text of v in a format (the argument past its type, PureFormat): false where an integer's form meets a
 * number that is not a whole one within 2^53. DECIMAL with digits writes the integer in minor units: its last
 * digits after a point (money's cents: 1234 at two is 12.34, -5 is -0.05). */
static bool formatted(Slot v, uint32_t arg, char *out, size_t *length) {
    unsigned type = arg & 3, width = arg >> PURE_FORMAT_WIDTH & 0xFF, form = arg >> PURE_FORMAT_FORM & 7,
             digits = arg >> PURE_FORMAT_DIGITS & 31;
    char body[400];
    size_t n = 0;
    if (form == PURE_FORMAT_FIXED) {
        n = (size_t)snprintf(body, sizeof(body), "%.*f", digits ? (int)digits - 1 : 6, v.num == 0 ? 0.0 : v.num);
    } else if (form) {
        int64_t i = v.i64;
        if (type != 2) {
            if (!(fabs(v.num) <= 0x1p53 && v.num == floor(v.num))) return false;
            i = (int64_t)v.num;
        }
        uint64_t magnitude = i < 0 ? 0 - (uint64_t)i : (uint64_t)i;
        unsigned base = form == PURE_FORMAT_DECIMAL ? 10 : form <= PURE_FORMAT_HEX_UPPER ? 16 : form == PURE_FORMAT_BINARY ? 2 : 8;
        const char *set = form == PURE_FORMAT_HEX_UPPER ? "0123456789ABCDEF" : "0123456789abcdef";
        char reversed[64];
        unsigned count = 0, places = form == PURE_FORMAT_DECIMAL && digits ? digits - 1 : 0;
        do reversed[count++] = set[magnitude % base], magnitude /= base; while (magnitude);
        while (count <= places) reversed[count++] = '0'; /* a digit before the point */
        if (i < 0) body[n++] = '-';
        for (; count; --count) {
            if (count == places) body[n++] = '.';
            body[n++] = reversed[count - 1];
        }
    } else
        n = type == 2 ? (size_t)snprintf(body, sizeof(body), "%" PRId64, v.i64)
          : type ? (size_t)snprintf(body, sizeof(body), "%s", v.num ? "true" : "false") : pure_format_number(v.num, body);
    size_t sign = body[0] == '-', pad = width > n ? width - n : 0, k = 0;
    if (arg & PURE_FORMAT_ZEROS) {
        if (sign) out[k++] = '-';
        while (pad--) out[k++] = '0';
        memcpy(out + k, body + sign, n - sign);
        k += n - sign;
    } else {
        while (pad--) out[k++] = ' ';
        memcpy(out + k, body, n);
        k += n;
    }
    *length = k;
    return true;
}

/* An output's message from its field values, laid out as a message's
 * payload (size bytes), numbered and kept with the event's changes. */
static PureResult send(PureApp *app, uint32_t output, const Slot *fields, const PureArena *arena, size_t size) {
    const PureModule *m = app->module;
    const PureInputShape *shape = &m->input[output];
    unsigned char *payload = malloc(size ? size : 1);
    if (!payload) return PURE_ALLOCATION;
    Copy copy = {payload + shape->prefix};
    for (uint32_t f = 0; f < shape->field_count; ++f) {
        const PureField *field = &m->field[shape->field_start + f];
        if (field->type == PURE_FIELD_U32) write32(payload + field->offset, (uint32_t)fields[f].num);
        else if (field->type == PURE_FIELD_BOOL) payload[field->offset] = fields[f].num != 0;
        else if (field->type == PURE_FIELD_I64) write_i64(payload + field->offset, fields[f].i64);
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
    /* The outbox's next item: a change of the list, undone with the event. */
    const char *name = m->input_names[output];
    PureCell item[3] = {{.text = (unsigned char *)name, .size = strlen(name)}, {.num = (double)(app->last_sent[output] + 1)},
                        {.text = payload, .size = size}};
    PureResult result = pure_list_insert(app->outbox, pure_list_count(app->outbox), item, &app->undo);
    free(payload);
    if (result != PURE_OK) return result;
    ++app->last_sent[output];
    app->links_changed = true;
    return PURE_OK;
}

/* The undo log a list's changes go to: none for a list of a message's rows,
 * which is emptied when the event ends whatever it did. */
static PureUndo *undo_for(PureApp *app, uint32_t list) {
    return app->module->state_flags[list] & PURE_LIST_ROWS >> 8 && app->module->state_type[list] == PURE_STATE_LIST
               ? NULL : &app->undo;
}

static PureResult run(const PureModule *m, PureValue *values, PureBlock block, Event *ev, Slot *result);

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
static bool run_lanes(const PureModule *m, const PureValue *values, PureBlock block, const Event *ev, const PureRun *items,
                      Lanes *result, double (*store)[PURE_LIST_RUN], Memo *memo, const PureShape *shape) {
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
        /* A group's pass: every group at once, so its key is not needed and its test holds. */
        if (shape && pc == shape->key.start) {
            stack[sp++] = (Lanes){NULL, 0, false, 0};
            pc = shape->key.start + shape->key.length - 1;
            continue;
        }
        if (shape && pc == shape->test) {
            stack[--sp - 1] = (Lanes){NULL, 1, false, 0};
            continue;
        }
        switch (op->op) {
        case PURE_OP_CONST: stack[sp++] = (Lanes){NULL, op->value, false, 0}; continue;
        case PURE_OP_LOAD: stack[sp++] = (Lanes){NULL, values[arg].num, false, 0}; continue;
        case PURE_OP_FIELD: {
            const PureField *field = &m->field[ev->shape->field_start + arg];
            const unsigned char *given = ev->arguments ? app->arguments.data : app->payload.data;
            double value = field->type == PURE_FIELD_U32 ? read32(given + field->offset)
                         : field->type == PURE_FIELD_BOOL ? given[field->offset] : read_double(given + field->offset);
            stack[sp++] = (Lanes){NULL, value, false, 0};
            continue;
        }
        case PURE_OP_INDEX_LOAD: stack[sp++] = (Lanes){NULL, values[arg].num, false, m->state_ref[arg]}; continue;
        case PURE_OP_ROW: stack[sp++] = (Lanes){NULL, 0, true, (uint16_t)arg}; continue;
        case PURE_OP_LOCAL_LOAD: stack[sp++] = (Lanes){NULL, ev->locals[arg & 0xFF].num, false, (uint16_t)(arg >> 16)}; continue;
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
                    size_t at = find || !pure_list_contains(keyed, top->value, &found) ?
                                pure_list_seek(keyed, key, (PureCell){.num = top->value}, &found) : 0;
                    if (find && !found) return false;
                    remember(memo, pc - block.start, value = find ? (double)at : found);
                }
                *top = (Lanes){NULL, value, false, find ? (uint16_t)arg : 0};
                continue;
            }
            const double *keys = lanes_of(top, scratch[0], items);
            for (unsigned i = 0; i < n; ++i) {
                size_t at = find || !pure_list_contains(keyed, keys[i], &found) ?
                            pure_list_seek(keyed, key, (PureCell){.num = keys[i]}, &found) : 0;
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

/* The values a distinct fold has seen: open addressing over their bits (-0
 * as 0), UINT64_MAX free (no finite number's bits; an i64's -1, whose
 * being seen is marked apart). */
typedef struct { uint64_t *keys; size_t capacity, count; bool marked; } Seen;

/* A fold's running value: what count, sum, max, min, first or distinct has
 * so far, over numbers (total) or i64s (whole; an i64 sum exactly, in sum:
 * only the result past the range overflows, so its items' order is none of
 * its business: 027's stage 5). */
__extension__ typedef __int128 Wide;
typedef struct { unsigned how; bool integer; double total; int64_t whole; Wide sum; bool any, done; Seen seen; } Fold;

static size_t seen_slot(uint64_t bits, size_t capacity) {
    bits ^= bits >> 33, bits *= 0xff51afd7ed558ccdu, bits ^= bits >> 33;
    return (size_t)bits & (capacity - 1);
}

/* Whether a value's bits are new to the set (added); false without memory too (*full). */
static bool seen_bits(Seen *seen, uint64_t bits, bool *full) {
    if (bits == UINT64_MAX) { /* the free slots' mark: an i64's -1, kept aside */
        if (seen->marked) return false;
        seen->marked = true;
        ++seen->count;
        return true;
    }
    if (2 * (seen->count + 1) > seen->capacity) { /* half the slots free */
        size_t capacity = seen->capacity ? 2 * seen->capacity : 64;
        uint64_t *keys = malloc(capacity * sizeof(*keys));
        if (!keys) { *full = true; return false; }
        memset(keys, 0xFF, capacity * sizeof(*keys));
        for (size_t i = 0; i < seen->capacity; ++i) {
            if (seen->keys[i] == UINT64_MAX) continue;
            size_t at = seen_slot(seen->keys[i], capacity);
            while (keys[at] != UINT64_MAX) at = (at + 1) & (capacity - 1);
            keys[at] = seen->keys[i];
        }
        free(seen->keys);
        seen->keys = keys;
        seen->capacity = capacity;
    }
    size_t at = seen_slot(bits, seen->capacity);
    for (; seen->keys[at] != UINT64_MAX; at = (at + 1) & (seen->capacity - 1))
        if (seen->keys[at] == bits) return false;
    seen->keys[at] = bits;
    ++seen->count;
    return true;
}

static bool seen_new(Seen *seen, double value, bool *full) {
    uint64_t bits;
    value = value == 0 ? 0 : value;
    memcpy(&bits, &value, sizeof(bits));
    return seen_bits(seen, bits, full);
}

/* One item: its value, and whether it counts (for count and first, the
 * value is). false without memory (a distinct fold's set). A fold of i64s
 * is never planned, so a leaf's lanes (numbers) come here alone. */
static bool fold_number(Fold *fold, size_t row, double value, double counts) {
    if (counts == 0) return true;
    unsigned how = fold->how;
    if (how == PURE_FOLD_FIRST) {
        fold->total = (double)row;
        fold->any = fold->done = true;
        return true;
    }
    if (how == PURE_FOLD_DISTINCT) {
        bool full = false;
        fold->total += seen_new(&fold->seen, value, &full);
        fold->any = true;
        return !full;
    }
    fold->total = how == PURE_FOLD_COUNT ? fold->total + 1 : how == PURE_FOLD_SUM ? fold->total + value :
                  !fold->any ? value : how == PURE_FOLD_MAX ? maximum(fold->total, value) : minimum(fold->total, value);
    fold->any = true;
    return true;
}

/* One item run as written: a number's, or an i64's (count and first take no i64). */
static bool fold_in(Fold *fold, size_t row, PureValue value, double counts) {
    if (!fold->integer) return fold_number(fold, row, value.num, counts);
    if (counts == 0) return true;
    int64_t v = value.i64;
    if (fold->how == PURE_FOLD_DISTINCT) {
        bool full = false;
        fold->total += seen_bits(&fold->seen, (uint64_t)v, &full);
        fold->any = true;
        return !full;
    }
    if (fold->how == PURE_FOLD_SUM) fold->sum += v; /* (a Wide holds 2^64 items' worth) */
    else if (!fold->any || (fold->how == PURE_FOLD_MAX ? v > fold->whole : v < fold->whole))
        fold->whole = v;
    fold->any = true;
    return true;
}

/* The fold's block for its items from first to last, one at a time. */
static PureResult fold_rows(const PureModule *m, PureValue *values, PureBlock inner, Event *per, size_t first,
                            size_t last, Fold *fold) {
    bool one = fold->how == PURE_FOLD_COUNT || fold->how == PURE_FOLD_FIRST;
    for (size_t row = first; row < last && !fold->done; ++row) {
        Slot out[2];
        size_t mark = per->arena->used;
        per->row = row;
        PureResult status = run(m, values, inner, per, out);
        per->arena->used = mark;
        if (status != PURE_OK) return status;
        if (!fold_in(fold, row, (PureValue){.i64 = out[0].i64}, one ? out[0].num : out[1].num)) return PURE_ALLOCATION;
    }
    return PURE_OK;
}

/* The fold's block a leaf of its list at a time; a run where an item would
 * fault goes item by item. The items join the fold in order, so a sum adds
 * as it would one at a time. */
static PureResult fold_runs(const PureModule *m, PureValue *values, PureBlock inner, const PureList *list, Event *per,
                            Fold *fold, size_t from, size_t to) {
    double store[PURE_MODULE_STACK][PURE_LIST_RUN], scratch[2][PURE_LIST_RUN];
    bool one = fold->how == PURE_FOLD_COUNT || fold->how == PURE_FOLD_FIRST;
    PureRun items;
    Memo memo;
    memo.known = 0;
    /* The items from and up to before to (a slice's, or a range's): a leaf
     * at a time, its lanes out of them left out (a slice's may fault: the
     * leaf then runs item by item, its items in them alone). */
    for (bool more = from < to && pure_list_run(list, from, &items); more && !fold->done && items.start < to;
         more = pure_list_next(list, &items)) {
        unsigned first = from > items.start ? (unsigned)(from - items.start) : 0,
                 last = to - items.start < items.count ? (unsigned)(to - items.start) : items.count;
        Lanes out[2];
        if (!run_lanes(m, values, inner, per, &items, out, store, &memo, NULL)) {
            PureResult status = fold_rows(m, values, inner, per, items.start + first, items.start + last, fold);
            if (status != PURE_OK) return status;
            continue;
        }
        const Lanes *counts = &out[one ? 0 : 1];
        if (uniform(counts) && counts->value == 0) continue;
        const double *c = lanes_of(counts, scratch[0], &items), *v = lanes_of(&out[0], scratch[1], &items);
        if (fold->how == PURE_FOLD_COUNT) {
            size_t counted = 0;
            for (unsigned i = first; i < last; ++i) counted += c[i] != 0;
            fold->total += (double)counted;
            fold->any = fold->any || counted;
        } else if (fold->how == PURE_FOLD_SUM) {
            double total = fold->total;
            bool any = fold->any;
            for (unsigned i = first; i < last; ++i)
                if (c[i] != 0) total += v[i], any = true;
            fold->total = total;
            fold->any = any;
        } else
            for (unsigned i = first; i < last && !fold->done; ++i)
                if (!fold_number(fold, items.start + i, v[i], c[i])) return PURE_ALLOCATION;
    }
    return PURE_OK;
}

/* The positions of the items whose keys are from low up to before high. */
/* The items whose keys are from low up to before high (numbers or i64s, as the key is). */
static void slice_of(const PureList *list, unsigned key, Slot low, Slot high, size_t *from, size_t *to) {
    *from = pure_list_bound(list, key, (PureCell){.i64 = low.i64});
    *to = pure_list_bound(list, key, (PureCell){.i64 = high.i64});
    if (*to < *from) *to = *from;
}

/* The positions of the items whose keys a range allows: its bounds run
 * alone, then found by key. false when a bound faults: the test then runs
 * for every item, as written (and faults as it would). */
static bool range_bounds(const PureModule *m, PureValue *values, const PureRange *range, const PureList *list, unsigned key,
                         Event *ev, size_t *from, size_t *to) {
    PureApp *app = ev->app;
    PureFault fault = app->fault;
    uint32_t fault_pc = app->fault_pc;
    bool ok = true;
    *from = 0;
    *to = pure_list_count(list);
    for (unsigned side = 0; side < 2 && ok; ++side) {
        uint8_t relation = side ? range->upper_relation : range->lower_relation;
        if (!relation) continue;
        Slot bound;
        size_t mark = ev->arena->used;
        ok = run(m, values, side ? range->upper : range->lower, ev, &bound) == PURE_OK;
        ev->arena->used = mark;
        if (!ok) break;
        bool found;
        size_t at = pure_list_seek(list, key, (PureCell){.num = bound.num}, &found);
        at += (relation == PURE_OP_GT || relation == PURE_OP_LE) && found; /* past the key itself */
        if (side) *to = at < *to ? at : *to;
        else *from = at;
    }
    app->fault = fault, app->fault_pc = fault_pc;
    if (*from > *to) *from = *to;
    return ok;
}

/* ---- Passes: a shaped fold in a column answered for every row --------
 * A fold in a column runs once per row, over its whole list each time. A
 * shaped one (PureShape) is answered from one pass over the list instead,
 * kept by the app for the list's version: a group pass folds the items of
 * every group at once (the fold's test with its comparison taken as true,
 * the item's field naming its group), a rank pass orders the field's values.
 * A row then runs the key alone and looks it up. The pass is made at the
 * second row that asks for the same list, so a fold asked once scans once.
 *
 * It answers exactly as the fold would: items join a group in order (a sum
 * adds as it would), a key that faults leaves the row to run the fold as
 * written (which faults, or not over no item, as it would), and an item
 * that would fault in the pass leaves every row to run it as written. */
typedef struct { double key; Fold fold; bool used; } Group;
struct PurePass {
    const PureModule *module;
    uint32_t pc;             /* the fold's */
    uint64_t list, version;  /* its list as it was */
    unsigned asked;          /* rows that asked before it was made */
    bool made, failed;
    Group *groups;           /* a group pass: open addressing by key */
    size_t capacity, count;
    uint64_t *ordered;       /* a rank pass: the field's values in order (order_bits) */
    size_t items;
};
enum { PASSES = 16 };

static void free_pass(struct PurePass *pass) {
    free(pass->groups);
    free(pass->ordered);
}

unsigned pure_app_passes(const PureApp *app) {
    unsigned made = 0;
    for (unsigned i = 0; i < app->pass_count; ++i) made += app->passes[i].made;
    return made;
}

static void drop_passes(PureApp *app) {
    for (unsigned i = 0; i < app->pass_count; ++i) free_pass(&app->passes[i]);
    free(app->passes);
    app->passes = NULL;
    app->pass_count = 0;
}

/* The app's pass for a fold over a list as it is: one kept, or a new one in
 * place of this fold's older ones (or of the oldest). NULL without room. */
static struct PurePass *pass_for(PureApp *app, const PureModule *m, uint32_t pc, const PureList *list) {
    uint64_t id, version;
    pure_list_stamp(list, &id, &version);
    unsigned kept = 0;
    struct PurePass *found = NULL;
    for (unsigned i = 0; i < app->pass_count; ++i) {
        struct PurePass *pass = &app->passes[i];
        if (pass->module == m && pass->pc == pc && pass->list == id && pass->version == version) found = pass;
        else if (pass->module == m && pass->pc == pc) free_pass(pass); /* the list has moved on */
        else app->passes[kept++] = *pass;
        if (found == pass) app->passes[kept++] = *pass, found = &app->passes[kept - 1];
    }
    app->pass_count = kept;
    if (found) return found;
    if (!app->passes && !(app->passes = calloc(PASSES, sizeof(*app->passes)))) return NULL;
    if (app->pass_count == PASSES) {
        free_pass(&app->passes[0]);
        memmove(app->passes, app->passes + 1, (PASSES - 1) * sizeof(*app->passes));
        --app->pass_count;
    }
    struct PurePass *pass = &app->passes[app->pass_count++];
    *pass = (struct PurePass){.module = m, .pc = pc, .list = id, .version = version};
    return pass;
}

/* A key's bits as they order: -0 as 0, negative numbers below positive ones. */
static uint64_t order_bits(double value) {
    uint64_t bits;
    value = value == 0 ? 0 : value;
    memcpy(&bits, &value, sizeof(bits));
    return bits >> 63 ? ~bits : bits | (uint64_t)1 << 63;
}

static size_t group_slot(uint64_t bits, size_t capacity) {
    bits ^= bits >> 33, bits *= 0xff51afd7ed558ccdu, bits ^= bits >> 33; /* a finalizer's mixing */
    return (size_t)bits & (capacity - 1);
}

/* key's group, added when absent (with add); NULL when absent, or without room. */
static inline __attribute__((always_inline)) Group *group_of(struct PurePass *pass, double key, unsigned how, bool add) {
    key = key == 0 ? 0 : key;
    if (pass->capacity) {
        for (size_t at = group_slot(order_bits(key), pass->capacity);; at = (at + 1) & (pass->capacity - 1)) {
            if (!pass->groups[at].used) break;
            if (pass->groups[at].key == key) return &pass->groups[at];
        }
    }
    if (!add) return NULL;
    if (2 * (pass->count + 1) > pass->capacity) { /* grown to keep half the slots free */
        size_t capacity = pass->capacity ? 2 * pass->capacity : 64;
        Group *groups = calloc(capacity, sizeof(*groups));
        if (!groups) return NULL;
        for (size_t i = 0; i < pass->capacity; ++i) {
            if (!pass->groups[i].used) continue;
            size_t at = group_slot(order_bits(pass->groups[i].key), capacity);
            while (groups[at].used) at = (at + 1) & (capacity - 1);
            groups[at] = pass->groups[i];
        }
        free(pass->groups);
        pass->groups = groups;
        pass->capacity = capacity;
    }
    size_t at = group_slot(order_bits(key), pass->capacity);
    while (pass->groups[at].used) at = (at + 1) & (pass->capacity - 1);
    pass->groups[at] = (Group){key, {.how = how}, true};
    ++pass->count;
    return &pass->groups[at];
}

/* Every group's fold in one pass over the list, a leaf at a time. */
static bool group_pass(const PureModule *m, PureValue *values, PureBlock inner, const PureShape *shape, unsigned how,
                       const PureList *list, Event *per, struct PurePass *pass) {
    double store[PURE_MODULE_STACK][PURE_LIST_RUN], scratch[2][PURE_LIST_RUN];
    bool one = how == PURE_FOLD_COUNT || how == PURE_FOLD_FIRST;
    PureRun items;
    Memo memo;
    memo.known = 0;
    for (bool more = pure_list_run(list, 0, &items); more; more = pure_list_next(list, &items)) {
        Lanes out[2];
        if (!run_lanes(m, values, inner, per, &items, out, store, &memo, shape)) return false;
        const Lanes *counts = &out[one ? 0 : 1];
        if (uniform(counts) && counts->value == 0) continue;
        const double *c = lanes_of(counts, scratch[0], &items), *v = lanes_of(&out[0], scratch[1], &items);
        const double *keys = items.column[shape->field].numbers;
        for (unsigned i = 0; i < items.count; ++i) {
            if (c[i] == 0) continue;
            Group *group = group_of(pass, keys[i], how, true);
            if (!group) return false;
            if (!group->fold.done && !fold_number(&group->fold, items.start + i, v[i], c[i])) return false;
        }
    }
    return true;
}

/* The field's values in order: a radix sort of their order_bits. */
static bool rank_pass(const PureShape *shape, const PureList *list, struct PurePass *pass) {
    size_t count = pure_list_count(list), at = 0;
    uint64_t *keys = malloc((count ? count : 1) * sizeof(*keys)), *spare = malloc((count ? count : 1) * sizeof(*spare));
    if (!keys || !spare) {
        free(keys);
        free(spare);
        return false;
    }
    PureRun items;
    for (bool more = pure_list_run(list, 0, &items); more; more = pure_list_next(list, &items))
        for (unsigned i = 0; i < items.count; ++i) keys[at++] = order_bits(items.column[shape->field].numbers[i]);
    for (unsigned shift = 0; shift < 64; shift += 8) {
        size_t start[257] = {0};
        for (size_t i = 0; i < count; ++i) ++start[(keys[i] >> shift & 0xFF) + 1];
        if (start[(keys[0] >> shift & 0xFF) + 1] == count) continue; /* one digit for all: in order already */
        for (unsigned d = 0; d < 256; ++d) start[d + 1] += start[d];
        for (size_t i = 0; i < count; ++i) spare[start[keys[i] >> shift & 0xFF]++] = keys[i];
        uint64_t *swap = keys;
        keys = spare, spare = swap;
    }
    free(spare);
    pass->ordered = keys;
    pass->items = count;
    return true;
}

/* How many of the ordered values relate to key: the first not below key
 * (or above it), counted from the start or the end. */
static size_t ranked(const struct PurePass *pass, uint8_t relation, double key) {
    uint64_t bits = order_bits(key);
    bool above = relation == PURE_OP_GT || relation == PURE_OP_LE; /* the bound past key's equals */
    size_t low = 0, high = pass->items;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        if (above ? pass->ordered[middle] <= bits : pass->ordered[middle] < bits) low = middle + 1;
        else high = middle;
    }
    return relation == PURE_OP_GT || relation == PURE_OP_GE ? pass->items - low : low;
}

/* A shaped fold from its list's pass: false leaves it to run as written. */
static bool fold_shaped(const PureModule *m, PureValue *values, const PureOp *op, const PureList *list, Event *per,
                        Fold *fold) {
    PureApp *app = per->app;
    const PureShape *shape = &m->shape[op->shape - 1];
    struct PurePass *pass = pass_for(app, m, (uint32_t)(op - m->code), list);
    if (!pass || pass->failed || (!pass->made && ++pass->asked < 2)) return false;
    Slot key;
    PureFault fault = app->fault;
    uint32_t fault_pc = app->fault_pc;
    size_t mark = per->arena->used;
    PureResult status = run(m, values, shape->key, per, &key);
    per->arena->used = mark;
    app->fault = fault, app->fault_pc = fault_pc;
    if (status != PURE_OK) return false;
    if (!pass->made) {
        PureBlock inner = {(uint32_t)(op - m->code) + 1, op->arg >> 16};
        pass->made = shape->form == PURE_SHAPE_GROUP ? group_pass(m, values, inner, shape, fold->how, list, per, pass)
                                                     : rank_pass(shape, list, pass);
        if (!pass->made) {
            free_pass(pass);
            pass->groups = NULL, pass->ordered = NULL;
            pass->failed = true;
            return false;
        }
    }
    if (shape->form == PURE_SHAPE_RANK) {
        size_t count = ranked(pass, shape->relation, key.num);
        fold->total = (double)count;
        fold->any = count > 0;
        return true;
    }
    const Group *group = group_of(pass, key.num, fold->how, false);
    if (group) *fold = group->fold;
    return true;
}

/* EACH at pc: its walk opened (its block next), or nothing to walk (past its block). */
static inline __attribute__((always_inline)) int enter_each(Frame *frame, const PureOp *op, uint32_t pc, uint32_t *next) {
    const PureModule *m = frame->m;
    PureApp *app = frame->app;
    Event *ev = frame->ev;
    Slot *stack = frame->stack;
    uint32_t arg = op->arg, length = arg >> 16;
    /* The block that follows runs for each item the list has, as the
     * handler's code with ROW the item; the list only shrinks meanwhile.
     * It runs here, a loop opened (its end takes pc back), not by recursion. */
    Loop *loop = &frame->loops[frame->depth];
    loop->walking = true;
    loop->walk = (Walk){arg >> 8 & 0xFF, 0, SIZE_MAX, false};
    if (arg & PURE_EACH_SLICED) { /* the items with keys from low up to before high */
        Slot high = stack[--frame->sp], low = stack[--frame->sp];
        slice_of(app->lists[loop->walk.list], m->state_key[loop->walk.list] - 1u, low, high, &loop->walk.at, &loop->walk.end);
    }
    size_t items = pure_list_count(app->lists[loop->walk.list]); /* as written: what it starts with, the list only shrinking */
    if (!charge(app, (uint64_t)length * ((loop->walk.end < items ? loop->walk.end : items) - loop->walk.at))) {
        frame->kind = PURE_FAULT_WORK;
        frame->fault_pc = pc;
        return STEP_FAULT;
    }
    if (!(arg & PURE_EACH_SLICED) && op->range) /* a where that bounds the key: only the items in bounds */
        range_bounds(m, frame->values, &m->range[op->range - 1], app->lists[loop->walk.list], m->state_key[loop->walk.list] - 1u,
                     ev, &loop->walk.at, &loop->walk.end);
    *next = pc + 1;
    if (!(loop->walk.at < loop->walk.end && loop->walk.at < items)) { /* none */
        *next += length;
        return STEP_NEXT;
    }
    loop->start = pc + 1, loop->end = pc + 1 + length, loop->mark = frame->arena->used;
    loop->around = ev->walk, loop->row = ev->row;
    ev->walk = &loop->walk;
    ev->row = loop->walk.at;
    loop->outer = frame->end, frame->end = loop->end, ++frame->depth;
    return STEP_NEXT;
}

/* LOOP at pc: its block once for each value from low up to before high,
 * the slot holding it; its work counts as it runs (the LOOP too, so an
 * empty block counts). It runs here, as a walk does. */
static inline __attribute__((always_inline)) int enter_loop(Frame *frame, const PureOp *op, uint32_t pc, uint32_t *next) {
    Slot *stack = frame->stack;
    uint32_t arg = op->arg, length = arg >> 16;
    Slot high = stack[--frame->sp], low = stack[--frame->sp];
    bool integer = arg & PURE_LOOP_INTEGER;
    uint64_t count = 0;
    frame->fault_pc = pc;
    if (integer) count = high.i64 > low.i64 ? (uint64_t)high.i64 - (uint64_t)low.i64 : 0;
    else if (!(fabs(low.num) <= 0x1p53 && fabs(high.num) <= 0x1p53 && low.num == floor(low.num) &&
               high.num == floor(high.num))) {
        frame->kind = PURE_FAULT_LIMIT;
        return STEP_FAULT;
    } else count = high.num > low.num ? (uint64_t)(high.num - low.num) : 0;
    *next = pc + 1;
    if (!count) {
        *next += length;
        return STEP_NEXT;
    }
    if (!charge(frame->app, (uint64_t)length + 1)) {
        frame->kind = PURE_FAULT_WORK;
        return STEP_FAULT;
    }
    Loop *loop = &frame->loops[frame->depth];
    loop->walking = false;
    loop->integer = integer;
    loop->count = count, loop->k = 0;
    loop->low = (PureValue){.i64 = low.i64};
    loop->slot = &frame->ev->locals[arg & 0xFF];
    if (integer) loop->slot->i64 = low.i64;
    else loop->slot->num = low.num;
    loop->start = pc + 1, loop->end = pc + 1 + length, loop->mark = frame->arena->used;
    loop->outer = frame->end, frame->end = loop->end, ++frame->depth;
    return STEP_NEXT;
}

/* BREAK: the innermost loop ends, the code after its block next. */
static inline __attribute__((always_inline)) uint32_t leave_loop(Frame *frame) {
    Loop *loop = &frame->loops[--frame->depth];
    frame->arena->used = loop->mark;
    if (loop->walking) frame->ev->walk = loop->around, frame->ev->row = loop->row;
    frame->end = loop->outer;
    return loop->end;
}

/* The innermost loop's block ran once: again (*next its start), or the loop
 * ends (*next stays: its end may be its outer's too). */
static inline __attribute__((always_inline)) int iterate(Frame *frame, uint32_t *next) {
    Loop *loop = &frame->loops[frame->depth - 1];
    PureApp *app = frame->app;
    frame->arena->used = loop->mark;
    if (loop->walking) {
        Walk *walk = &loop->walk;
        if (!walk->removed) ++walk->at;
        walk->removed = false;
        if (walk->at < walk->end && walk->at < pure_list_count(app->lists[walk->list])) {
            frame->ev->row = walk->at;
            *next = loop->start;
            return STEP_NEXT;
        }
        frame->ev->walk = loop->around, frame->ev->row = loop->row;
    } else if (++loop->k < loop->count) {
        if (!charge(app, (uint64_t)(loop->end - loop->start) + 1)) {
            frame->kind = PURE_FAULT_WORK;
            frame->fault_pc = loop->start - 1;
            return STEP_FAULT;
        }
        if (loop->integer) loop->slot->i64 = (int64_t)((uint64_t)loop->low.i64 + loop->k);
        else loop->slot->num = loop->low.num + (double)loop->k;
        *next = loop->start;
        return STEP_NEXT;
    }
    frame->end = loop->outer;
    --frame->depth;
    return STEP_NEXT;
}

/* A loop planned PLAN_BATCH from its round loop->k (set up and counted; a
 * walk's, its place) on: batches while they run (module-batch.c), the rounds
 * of one that failed one by one, then batches again, until the loop ends or
 * batches stop (then *next its block: the rest of it as any loop's).
 * STEP_NEXT with *next past its block when it ended here, or how a round
 * stopped. */
static __attribute__((noinline, cold)) int batches(Frame *frame, Loop *loop, uint32_t *next) {
    unsigned depth = frame->depth;
    for (;;) {
        unsigned done;
        int how = pure_batch_run(frame, loop, &done);
        if (how != STEP_NEXT) return how;
        if (done) {
            size_t items = loop->walking ? pure_list_count(frame->app->lists[loop->walk.list]) : 0;
            if (loop->walking ? (loop->walk.at += done) >= loop->walk.end || loop->walk.at >= items : (loop->k += done) >= loop->count) {
                frame->arena->used = loop->mark; /* it ended */
                if (loop->walking) frame->ev->walk = loop->around, frame->ev->row = loop->row;
                frame->end = loop->outer;
                --frame->depth;
                *next = loop->end;
                return STEP_NEXT;
            }
            if (loop->walking) { /* a walk's work counted as it opened */
                frame->ev->row = loop->walk.at;
                continue;
            }
            if (!charge(frame->app, (uint64_t)(loop->end - loop->start) + 1)) {
                frame->kind = PURE_FAULT_WORK;
                frame->fault_pc = loop->start - 1;
                return STEP_FAULT;
            }
            loop->slot->num = loop->low.num + (double)loop->k;
            continue;
        }
        if (loop->resume == UINT64_MAX) break;
        while ((loop->walking ? loop->walk.at : loop->k) < loop->resume) { /* a failed batch's rounds */
            int how = pure_run_rest(frame, loop->start);
            if (how == STEP_SKIP) { /* it left the loop */
                *next = loop->end;
                return STEP_NEXT;
            }
            if (how != STEP_NEXT) return how;
            uint32_t pc = loop->end;
            if ((how = iterate(frame, &pc)) != STEP_NEXT) return how;
            if (frame->depth < depth) {
                *next = loop->end;
                return STEP_NEXT;
            }
        }
    }
    *next = loop->start;
    return STEP_NEXT;
}

/* A LOOP planned PLAN_BATCH the executor runs (step leaves it to its
 * caller): opened, then in batches. */
static __attribute__((noinline, cold)) int open_batches(Frame *frame, uint32_t pc, uint32_t *next) {
    unsigned depth = frame->depth;
    const PureOp *op = &frame->m->code[pc];
    int how = op->op == PURE_OP_EACH ? enter_each(frame, op, pc, next) : enter_loop(frame, op, pc, next);
    if (how != STEP_NEXT || frame->depth == depth) return how;
    Loop *loop = &frame->loops[depth];
    loop->resume = 0, loop->failures = 0, loop->reverse = false;
    return batches(frame, loop, next);
}

#define NEXT do { frame->sp = sp; return STEP_NEXT; } while (0)
#define FAILED do { frame->sp = sp; frame->kind = kind; frame->fault_pc = pc; return STEP_FAULT; } while (0)
#define REFUSED do { frame->sp = sp; frame->status = status; frame->fault_pc = pc; return STEP_REFUSED; } while (0)
#define STOPPED(why) do { frame->status = (why); return STEP_RETURN; } while (0)

/* The instruction op at pc; *next the one to run after it. */
static inline __attribute__((always_inline)) int step(Frame *frame, const PureOp *op, uint32_t pc, uint32_t *next, bool batches) {
    const PureModule *m = frame->m;
    PureValue *values = frame->values;
    Event *ev = frame->ev;
    PureApp *app = frame->app;
    PureArena *arena = frame->arena;
    Slot *stack = frame->stack;
    size_t sp = frame->sp;
    PureResult status;
    PureFault kind;
    uint32_t arg = op->arg;
    Slot *top = sp ? &stack[sp - 1] : stack;
    *next = pc + 1;
    switch (op->op) {
    case PURE_OP_JZ:
        frame->sp = sp - 1;
        if (top->num == 0) *next = arg;
        return STEP_NEXT;
    case PURE_OP_JMP: *next = arg; return STEP_NEXT;
    case PURE_OP_EACH: return batches && op->plan & PLAN_BATCH ? STEP_BATCH : enter_each(frame, op, pc, next);
    case PURE_OP_LOOP: return batches && op->plan & PLAN_BATCH ? STEP_BATCH : enter_loop(frame, op, pc, next);
    case PURE_OP_BREAK: *next = leave_loop(frame); return STEP_NEXT;
    case PURE_OP_CONST: stack[sp++].i64 = op->integer; NEXT; /* the bits: a number's, a bool's or an i64's */
    case PURE_OP_LOAD: stack[sp++].i64 = values[arg].i64; NEXT;
    case PURE_OP_NOW: stack[sp++].num = app->now; NEXT;
    case PURE_OP_FLOOR: top->num = floor(top->num); NEXT;
    case PURE_OP_STORE: case PURE_OP_CUR_STORE: values[arg].i64 = stack[--sp].i64; NEXT;
    case PURE_OP_NEG: top->num = -top->num; NEXT;
    case PURE_OP_NOT: top->num = top->num == 0; NEXT;
    case PURE_OP_CLAMP: {
        double high = stack[--sp].num, low = stack[--sp].num;
        if (low > high) { kind = PURE_FAULT_CLAMP; FAILED; }
        stack[sp - 1].num = maximum(low, minimum(stack[sp - 1].num, high));
        NEXT;
    }
    case PURE_OP_FIELD: {
        const PureField *field = &m->field[ev->shape->field_start + arg];
        const unsigned char *given = ev->arguments ? app->arguments.data : app->payload.data;
        if (field->type == PURE_FIELD_U32) stack[sp++].num = read32(given + field->offset);
        else if (field->type == PURE_FIELD_NUMBER) stack[sp++].num = read_double(given + field->offset);
        else if (field->type == PURE_FIELD_BOOL) stack[sp++].num = given[field->offset];
        else if (field->type == PURE_FIELD_I64) stack[sp++].i64 = read_i64(given + field->offset);
        else stack[sp++].bytes = (PureBytes){ev->arguments ? FROM_ARGUMENTS : FROM_PAYLOAD, 0, {0, 0}, ev->spans[arg][0],
                                             ev->spans[arg][1]};
        NEXT;
    }
    case PURE_OP_LEN: case PURE_OP_TEXT_BYTES: top->num = (double)top->bytes.length; NEXT;
    /* A reply outlives the arena mark of later statements: keep it by value. */
    case PURE_OP_REPLY: { /* by value; a value held by name may change after it, so its bytes go to the arena */
        PureBytes reply = stack[--sp].bytes;
        if (reply.source == FROM_HELD) {
            size_t start = arena->used;
            if ((status = append_value(app, arena, reply)) != PURE_OK) STOPPED(status);
            reply = (PureBytes){FROM_ARENA, 0, {0, 0}, start, reply.length};
        }
        app->reply = reply;
        app->has_reply = true;
        NEXT;
    }
    case PURE_OP_REQUIRE: if (stack[--sp].num == 0) { kind = PURE_FAULT_LIMIT; FAILED; } NEXT;
    case PURE_OP_TEXT_CONST:
        stack[sp++].bytes = (PureBytes){FROM_CONSTANT, 0, {0, 0}, m->constant_offset[arg], m->constant_length[arg]};
        NEXT;
    case PURE_OP_TEXT_LOAD: stack[sp++].bytes = text_of(app, arg); NEXT;
    case PURE_OP_NUM_TEXT: {
        char digits[32];
        if (arg > 2) { /* in a format */
            char text[700];
            size_t length;
            if (!formatted(*top, arg, text, &length)) { kind = PURE_FAULT_LIMIT; FAILED; }
            if ((status = keep_bytes(arena, text, length, &top->bytes)) != PURE_OK) STOPPED(status);
            NEXT;
        }
        size_t length = arg == 2 ? (size_t)snprintf(digits, sizeof(digits), "%" PRId64, top->i64)
                      : arg ? (size_t)snprintf(digits, sizeof(digits), "%s", top->num ? "true" : "false")
                            : pure_format_number(top->num, digits);
        if ((status = keep_bytes(arena, digits, length, &top->bytes)) != PURE_OK) STOPPED(status);
        NEXT;
    }
    case PURE_OP_CONCAT: {
        PureBytes right = stack[--sp].bytes, left = stack[sp - 1].bytes;
        if (!right.length) NEXT; /* joined with nothing: the other as it is, uncopied (values do not change) */
        if (!left.length) { stack[sp - 1].bytes = right; NEXT; }
        size_t start = arena->used;
        if ((status = append_value(app, arena, left)) != PURE_OK ||
            (status = append_value(app, arena, right)) != PURE_OK) STOPPED(status);
        stack[sp - 1].bytes = (PureBytes){FROM_ARENA, 0, {0, 0}, start, left.length + right.length};
        NEXT;
    }
    case PURE_OP_CHARS: case PURE_OP_LINES: case PURE_OP_TEXT_FROM: {
        Measure count = {0, op->op == PURE_OP_LINES ? 1 : op->op == PURE_OP_TEXT_FROM ? 2 : 0, 0, true};
        if ((status = visit_value(app, arena, top->bytes, measure, &count)) != PURE_OK) STOPPED(status);
        if (op->op != PURE_OP_TEXT_FROM) top->num = (double)(count.count + (op->op == PURE_OP_LINES));
        else if (!count.valid || !pure_utf8_complete(count.utf8)) { kind = PURE_FAULT_ENCODING; FAILED; }
        NEXT;
    }
    /* Positions: a cursor or an index, with the text or list it is in. */
    case PURE_OP_CUR_LOAD: case PURE_OP_INDEX_LOAD:
        stack[sp].num = values[arg].num;
        stack[sp++].bytes.owner = m->state_ref[arg];
        NEXT;
    case PURE_OP_CUR_START: case PURE_OP_CUR_END: case PURE_OP_INDEX_FIRST: case PURE_OP_INDEX_LAST:
    case PURE_OP_ROW: {
        size_t last = op->op == PURE_OP_CUR_END ? pure_draft_size(app->texts[arg]) :
                      op->op == PURE_OP_INDEX_LAST ? pure_list_count(app->lists[arg]) : 0;
        stack[sp].num = op->op == PURE_OP_ROW ? (double)ev->row :
                        op->op == PURE_OP_INDEX_LAST && last ? (double)(last - 1) : (double)last;
        stack[sp++].bytes.owner = (uint16_t)arg;
        NEXT;
    }
    case PURE_OP_OUTER_ROW:
        stack[sp].num = (double)ev->outer;
        stack[sp++].bytes.owner = (uint16_t)arg;
        NEXT;
    case PURE_OP_LOCAL_LOAD:
        stack[sp].i64 = ev->locals[arg & 0xFF].i64; /* the bits: a number's, an index's, a bool's or an i64's */
        stack[sp++].bytes.owner = (uint16_t)(arg >> 16); /* an index's list */
        NEXT;
    case PURE_OP_LOCAL_STORE: ev->locals[arg & 0xFF].i64 = stack[--sp].i64; NEXT;
    case PURE_OP_CUR_MOVE: case PURE_OP_INDEX_MOVE: {
        double count = stack[--sp].num;
        Slot *at = &stack[sp - 1];
        if (count != floor(count)) { kind = PURE_FAULT_LIMIT; FAILED; }
        if (op->op == PURE_OP_INDEX_MOVE) {
            size_t items = pure_list_count(app->lists[at->bytes.owner]);
            at->num = maximum(0, minimum(at->num + count, items ? (double)(items - 1) : 0));
            NEXT;
        }
        PureDraft *text = app->texts[at->bytes.owner];
        PureVersion version = pure_draft_version(text);
        at->num = (double)(arg >> PURE_ANCHOR_UNIT == PURE_UNIT_CHAR ? pure_text_step(text, version, (size_t)at->num, count)
                                                                     : pure_text_line_step(text, version, (size_t)at->num, count));
        NEXT;
    }
    case PURE_OP_CUR_COUNT: {
        PureDraft *text = app->texts[top->bytes.owner];
        top->num = (double)pure_text_count(text, pure_draft_version(text), (size_t)top->num, arg >> PURE_ANCHOR_UNIT);
        NEXT;
    }
    case PURE_OP_CUR_AT: {
        double count = top->num;
        if (count != floor(count)) { kind = PURE_FAULT_LIMIT; FAILED; }
        if (!cursor_at(app, arg, count, &top->num)) { kind = PURE_FAULT_NOT_FOUND; FAILED; }
        top->bytes.owner = (uint16_t)(arg & 0xFFFF);
        NEXT;
    }
    case PURE_OP_TEXT_INSERT: case PURE_OP_TEXT_SET: {
        PureBytes insert = stack[--sp].bytes;
        size_t at = op->op == PURE_OP_TEXT_INSERT ? (size_t)stack[--sp].num : 0;
        size_t removed = op->op == PURE_OP_TEXT_SET ? pure_draft_size(app->texts[arg]) : 0;
        PureSpan span;
        size_t mark = arena->used;
        Held before = text_held(app, arg);
        if ((status = contiguous(app, arena, insert, &span)) == PURE_OK)
            status = pure_draft_splice(app->texts[arg], at, removed, span);
        arena->used = mark;
        if (status != PURE_OK) REFUSED;
        if (!grows(app, before, text_held(app, arg))) { kind = PURE_FAULT_MEMORY; FAILED; }
        if (op->op == PURE_OP_TEXT_SET) {
            for (uint32_t c = 0; c < m->states; ++c)
                if (m->state_type[c] == PURE_STATE_CURSOR && m->state_ref[c] == arg) values[c].num = 0;
        } else move_cursors(m, values, arg, at, 0, span.size);
        NEXT;
    }
    case PURE_OP_TEXT_REMOVE: {
        double y = stack[--sp].num, x = stack[--sp].num;
        size_t first = (size_t)minimum(x, y), last = (size_t)maximum(x, y);
        if ((status = pure_draft_splice(app->texts[arg], first, last - first, (PureSpan){0})) != PURE_OK) REFUSED;
        (void)grows(app, (Held){last - first, 0}, (Held){0, 0}); /* it only shrinks */
        if (last > first) move_cursors(m, values, arg, first, last - first, 0);
        NEXT;
    }
    case PURE_OP_TEXT_LINE: {
        PureDraft *text = app->texts[arg];
        PureVersion version = pure_draft_version(text);
        size_t first = pure_text_line_start(text, version, (size_t)top->num);
        size_t last = pure_text_line_end(text, version, (size_t)top->num);
        top->bytes = (PureBytes){FROM_TEXT, (uint16_t)arg, version, first, last - first};
        NEXT;
    }
    case PURE_OP_BYTES_FROM: case PURE_OP_INDEX_NUM: NEXT; /* the same bytes or position, retyped */
    case PURE_OP_LIST_COUNT: stack[sp++].num = (double)pure_list_count(app->lists[arg]); NEXT;
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
                FAILED;
            }
        }
        if (size > UINT32_MAX) { kind = PURE_FAULT_LIMIT; FAILED; }
        Held before = list_held(app, app->outbox);
        if ((status = send(app, arg, stack + base, arena, size)) != PURE_OK) STOPPED(status);
        if (!grows(app, before, list_held(app, app->outbox))) { kind = PURE_FAULT_MEMORY; FAILED; }
        NEXT;
    }
    case PURE_OP_LIST_APPEND: case PURE_OP_LIST_INSERT: {
        PureList *list = app->lists[arg];
        const PureRecord *record = &m->record[m->state_ref[arg]];
        size_t base = sp - record->count, count = pure_list_count(list), at = count;
        sp = base;
        if (op->op == PURE_OP_LIST_INSERT && (at = (size_t)stack[--sp].num) > count) {
            kind = PURE_FAULT_NOT_FOUND;
            FAILED;
        }
        /* Room for every text and bytes first, so no span moves while the next is copied. */
        size_t mark = arena->used, total = 0;
        uint32_t held = m->record_held[m->state_ref[arg]];
        for (unsigned f = 0; f < record->count; ++f)
            if (held >> f & 1) total += (size_t)stack[base + f].bytes.length;
        if (!reserve(arena, total)) STOPPED(PURE_ALLOCATION);
        PureCell cells[PURE_RECORD_FIELDS];
        for (unsigned f = 0; f < record->count; ++f) {
            PureSpan span = {0};
            if (held >> f & 1 && (status = contiguous(app, arena, stack[base + f].bytes, &span)) != PURE_OK)
                STOPPED(status);
            cells[f] = (PureCell){.i64 = stack[base + f].i64, .text = (unsigned char *)span.data, .size = span.size};
        }
        /* An event's list without a key keeps nothing besides its items (no undo, no journal, no members): what an
         * item adds is its stored bytes alone (030's step 1: not the eight calls list_held makes). */
        bool plain = m->state_flags[arg] & PURE_LIST_ROWS >> 8 && !m->state_key[arg];
        Held before = plain ? (Held){pure_list_size(list), 0} : list_held(app, list);
        status = PURE_INVALID;
        if (m->state_key[arg]) { /* a keyed list places the item by its key: found and placed at once when kept by a number */
            bool found;
            /* where it went matters to an index state's alone */
            if ((status = pure_list_add(list, cells, undo_for(app, arg), m->state_indexed[arg] ? &at : NULL, &found)) ==
                PURE_INVALID) {
                unsigned key = m->state_key[arg] - 1u;
                at = pure_list_seek(list, key, cells[key], &found);
            }
            if (found) { arena->used = mark; kind = PURE_FAULT_EXISTS; FAILED; }
        }
        if (status == PURE_INVALID) status = pure_list_insert(list, at, cells, undo_for(app, arg));
        arena->used = mark;
        if (status != PURE_OK) REFUSED;
        if (m->record_nested[m->state_ref[arg]]) share_cells(app, m, m->state_ref[arg], cells);
        if (m->state_indexed[arg]) move_indices(m, values, arg, pure_list_count(list), at, true);
        if (!grows(app, before, plain ? (Held){pure_list_size(list), 0} : list_held(app, list))) { kind = PURE_FAULT_MEMORY; FAILED; }
        NEXT;
    }
    case PURE_OP_LIST_REMOVE: {
        size_t at = (size_t)stack[--sp].num;
        Held before = list_held(app, app->lists[arg]);
        if ((status = pure_list_remove(app->lists[arg], at, undo_for(app, arg))) != PURE_OK) REFUSED;
        /* the item goes, its cells kept to undo and its record to save */
        if (!grows(app, before, list_held(app, app->lists[arg]))) { kind = PURE_FAULT_MEMORY; FAILED; }
        if (m->state_indexed[arg]) move_indices(m, values, arg, pure_list_count(app->lists[arg]), at, false);
        if (ev->walk && ev->walk->list == arg) { /* the walk goes on with the item after */
            if (at < ev->walk->at) --ev->walk->at;
            else if (at == ev->walk->at) ev->walk->removed = true;
            if (at < ev->walk->end && ev->walk->end != SIZE_MAX) --ev->walk->end;
        }
        NEXT;
    }
    case PURE_OP_ITEM_GET: {
        uint32_t list = list_of(arg), field = item_field(arg);
        size_t at = (size_t)top->num;
        if (at >= pure_list_count(app->lists[list])) { kind = PURE_FAULT_NOT_FOUND; FAILED; }
        PureCell cell = pure_list_get(app->lists[list], at, field);
        /* A text is copied: the item may change or go before the value is used. */
        if (!(op->plan & (PLAN_TEXT | PLAN_HOLDS))) top->i64 = cell.i64; /* the bits: a number's, a bool's or an i64's */
        else if (op->plan & PLAN_HOLDS) { /* a list's number: the value holds it too, so an append into the cell copies */
            top->i64 = cell.i64;
            if (cell.i64) ++app->nested.shared[cell.i64 - 1];
        } else if ((status = keep_bytes(arena, cell.text, cell.size, &top->bytes)) != PURE_OK) STOPPED(status);
        NEXT;
    }
    case PURE_OP_ITEM_SET: {
        uint32_t list = list_of(arg), field = item_field(arg);
        Slot value = stack[--sp];
        size_t at = (size_t)stack[--sp].num, mark = arena->used;
        Held before = list_held(app, app->lists[list]);
        PureSpan span = {0};
        status = op->plan & PLAN_TEXT ? contiguous(app, arena, value.bytes, &span) : PURE_OK;
        PureCell cell = {.i64 = value.i64, .text = (unsigned char *)span.data, .size = span.size};
        bool nested = m->record_field_type[m->record[m->state_ref[list]].first + field] == PURE_RECORD_LIST;
        uint64_t was = nested && status == PURE_OK && at < pure_list_count(app->lists[list]) ?
                       (uint64_t)pure_list_get(app->lists[list], at, field).i64 : 0;
        if (status == PURE_OK) /* where its FIND found it (PLAN_ROW: a HASHED list's slot), else its position */
            status = op->plan & PLAN_ROW ? pure_list_set_row(app->lists[list], at, field, cell, undo_for(app, list))
                                         : pure_list_set(app->lists[list], at, field, cell, undo_for(app, list));
        arena->used = mark;
        if (status != PURE_OK) REFUSED;
        if (nested) { /* the list it holds now is shared once more, the one it held once less */
            if (cell.i64) ++app->nested.shared[cell.i64 - 1];
            if (was) --app->nested.shared[was - 1];
        }
        if (!grows(app, before, list_held(app, app->lists[list]))) { kind = PURE_FAULT_MEMORY; FAILED; }
        NEXT;
    }
    case PURE_OP_INDEX_STORE: {
        /* An index from before a remove may be past the end. */
        double position = stack[--sp].num;
        if (position && position >= (double)pure_list_count(app->lists[m->state_ref[arg]])) {
            kind = PURE_FAULT_NOT_FOUND;
            FAILED;
        }
        values[arg].num = position;
        NEXT;
    }
    case PURE_OP_FOLD: {
        /* The block that follows runs once per item, as a column of the
         * list: a leaf of it at a time when planned so. */
        PureBlock inner = {pc + 1, arg >> 16};
        *next = pc + 1 + inner.length;
        PureList *list = app->lists[arg >> 8 & 0xFF];
        Fold fold = {.how = arg & PURE_FOLD_KIND, .integer = arg & PURE_FOLD_INTEGER};
        Event per = *ev;
        per.outer = ev->row;
        size_t from = 0, to = pure_list_count(list);
        if (arg & PURE_FOLD_SLICED) { /* the items with keys from low up to before high */
            Slot high = stack[--sp], low = stack[--sp];
            slice_of(list, m->state_key[arg >> 8 & 0xFF] - 1u, low, high, &from, &to);
        }
        if (!charge(app, (uint64_t)inner.length * (to - from))) { kind = PURE_FAULT_WORK; FAILED; } /* as written */
        if (!op->shape || !fold_shaped(m, values, op, list, &per, &fold)) {
            const PureRange *range = op->range ? &m->range[op->range - 1] : NULL;
            bool bounded = range && /* only the items a range of keys allows can count */
                           range_bounds(m, values, range, list, m->state_key[arg >> 8 & 0xFF] - 1u, &per, &from, &to);
            if (bounded && range->whole && (fold.how == PURE_FOLD_FIRST || fold.how == PURE_FOLD_COUNT)) {
                /* every item in bounds passes the test (it is the bounds): the first is theirs, the count their number */
                fold.any = from < to;
                fold.total = fold.how == PURE_FOLD_FIRST ? (double)from : (double)(to - from);
                status = PURE_OK;
            } else
                status = op->plan ? fold_runs(m, values, inner, list, &per, &fold, from, to)
                                  : fold_rows(m, values, inner, &per, from, to, &fold);
            free(fold.seen.keys);
            if (status != PURE_OK) STOPPED(status);
        }
        if (fold.integer && fold.how == PURE_FOLD_SUM) { /* the exact sum, if an i64 holds it */
            if (fold.sum > INT64_MAX || fold.sum < INT64_MIN) { kind = PURE_FAULT_OVERFLOW; FAILED; }
            fold.whole = (int64_t)fold.sum;
        }
        if (!fold.any && (fold.how == PURE_FOLD_MAX || fold.how == PURE_FOLD_MIN || fold.how == PURE_FOLD_FIRST)) {
            kind = PURE_FAULT_NOT_FOUND;
            FAILED;
        }
        if (fold.integer && fold.how != PURE_FOLD_DISTINCT) stack[sp].i64 = fold.whole;
        else if (!isfinite(fold.total)) { kind = PURE_FAULT_NON_FINITE; FAILED; }
        else stack[sp].num = fold.total;
        stack[sp++].bytes.owner = (uint16_t)(arg >> 8 & 0xFF); /* first: the list the index is in */
        NEXT;
    }
    case PURE_OP_TEXT_COMPARE: {
        /* Texts order by their bytes: a shorter text before a longer one it begins. */
        PureBytes right = stack[--sp].bytes, left = stack[sp - 1].bytes;
        size_t mark = arena->used;
        PureSpan a, b;
        if (!reserve(arena, (size_t)(left.length + right.length))) STOPPED(PURE_ALLOCATION);
        if ((status = contiguous(app, arena, left, &a)) != PURE_OK ||
            (status = contiguous(app, arena, right, &b)) != PURE_OK) STOPPED(status);
        size_t shorter = a.size < b.size ? a.size : b.size;
        int order = shorter ? memcmp(a.data, b.data, shorter) : 0;
        if (!order) order = (a.size > b.size) - (a.size < b.size);
        arena->used = mark;
        stack[sp - 1].num = related(arg & ~PURE_COMPARE_BYTES, order);
        NEXT;
    }
    case PURE_OP_LIST_FIND: case PURE_OP_LIST_HAS: {
        /* A keyed list's item by key, found by halving: FIND gives its index, HAS whether it is there. */
        unsigned key = m->state_key[arg] - 1u;
        size_t mark = arena->used;
        PureCell cell = {.i64 = top->i64};
        if (op->plan & PLAN_TEXT) { /* a text key (set at load) */
            PureSpan span;
            if ((status = contiguous(app, arena, top->bytes, &span)) != PURE_OK) STOPPED(status);
            cell = (PureCell){.text = (unsigned char *)span.data, .size = span.size};
        }
        bool found;
        if (op->plan & PLAN_JOINED) { /* FIND, then ITEM_GET of what it found: the field by key */
            const PureOp *get = &m->code[pc + 1];
            PureCell value;
            found = pure_list_lookup(app->lists[arg], key, cell, item_field(get->arg), &value);
            arena->used = mark;
            if (!found) { kind = PURE_FAULT_NOT_FOUND; FAILED; }
            *next = pc + 2;
            if (!(get->plan & PLAN_TEXT)) top->i64 = value.i64;
            else if ((status = keep_bytes(arena, value.text, value.size, &top->bytes)) != PURE_OK) STOPPED(status);
            NEXT;
        }
        /* An event's list keeps its keys as a set: HAS asks it alone (020); a text key's HASHED list, its table. */
        if (op->op == PURE_OP_LIST_HAS && (op->plan & PLAN_TEXT ? pure_list_contains_text(app->lists[arg], cell, &found)
                                                                 : pure_list_contains(app->lists[arg], cell.num, &found))) {
            arena->used = mark;
            top->num = found;
            NEXT;
        }
        size_t at = op->plan & PLAN_ROW ? pure_list_find_row(app->lists[arg], key, cell, &found) : pure_list_seek(app->lists[arg], key, cell, &found);
        arena->used = mark;
        if (op->op == PURE_OP_LIST_HAS) { top->num = found; NEXT; }
        if (!found) { kind = PURE_FAULT_NOT_FOUND; FAILED; }
        top->num = (double)at;
        top->bytes.owner = (uint16_t)arg;
        NEXT;
    }
    case PURE_OP_PICK: stack[sp] = stack[sp - 1 - arg]; ++sp; NEXT;
    case PURE_OP_SLIDE: stack[sp - 1 - arg] = stack[sp - 1]; sp -= arg; NEXT;
    /* i64s: exact, and failing as OVERFLOW past their range */
    case PURE_OP_I64_ADD: case PURE_OP_I64_SUB: case PURE_OP_I64_MUL: {
        int64_t b = stack[--sp].i64, a = stack[sp - 1].i64, r;
        bool over = op->op == PURE_OP_I64_ADD ? __builtin_add_overflow(a, b, &r) :
                    op->op == PURE_OP_I64_SUB ? __builtin_sub_overflow(a, b, &r) : __builtin_mul_overflow(a, b, &r);
        if (over) { kind = PURE_FAULT_OVERFLOW; FAILED; }
        stack[sp - 1].i64 = r;
        NEXT;
    }
    case PURE_OP_I64_DIV: case PURE_OP_I64_MOD: { /* floored: the remainder takes the divisor's sign */
        int64_t b = stack[--sp].i64, a = stack[sp - 1].i64;
        if (b == 0) { kind = PURE_FAULT_DIVIDE; FAILED; }
        if (b == -1) { /* the least i64 over -1 is past the range; any remainder by -1 is 0 */
            if (op->op == PURE_OP_I64_DIV && a == INT64_MIN) { kind = PURE_FAULT_OVERFLOW; FAILED; }
            stack[sp - 1].i64 = op->op == PURE_OP_I64_DIV ? -a : 0;
            NEXT;
        }
        int64_t q = a / b, r = a % b;
        if (r != 0 && (r < 0) != (b < 0)) { --q; r += b; }
        stack[sp - 1].i64 = op->op == PURE_OP_I64_DIV ? q : r;
        NEXT;
    }
    case PURE_OP_I64_NEG:
        if (top->i64 == INT64_MIN) { kind = PURE_FAULT_OVERFLOW; FAILED; }
        top->i64 = -top->i64;
        NEXT;
    case PURE_OP_I64_MIN: case PURE_OP_I64_MAX: {
        int64_t b = stack[--sp].i64, a = stack[sp - 1].i64;
        stack[sp - 1].i64 = (op->op == PURE_OP_I64_MIN ? b < a : b > a) ? b : a;
        NEXT;
    }
    case PURE_OP_I64_COMPARE: {
        int64_t b = stack[--sp].i64, a = stack[sp - 1].i64;
        stack[sp - 1].num = related(arg, (a > b) - (a < b));
        NEXT;
    }
    case PURE_OP_I64_FROM: case PURE_OP_NUM_FROM: { /* a whole number in range; an i64 within 2^53 */
        PureCell cell = {.i64 = top->i64};
        bool to_i64 = op->op == PURE_OP_I64_FROM;
        if (!pure_cell_as(to_i64 ? PURE_CELL_NUMBER : PURE_CELL_INTEGER, to_i64 ? PURE_CELL_INTEGER : PURE_CELL_NUMBER, &cell)) {
            kind = PURE_FAULT_LIMIT;
            FAILED;
        }
        top->i64 = cell.i64;
        NEXT;
    }
    case PURE_OP_I64_BITS: { /* on integers in two's complement without end (027) */
        int64_t b = stack[--sp].i64, a = stack[sp - 1].i64, r;
        if (arg == PURE_BITS_AND) r = a & b;
        else if (arg == PURE_BITS_OR) r = a | b;
        else if (arg == PURE_BITS_XOR) r = a ^ b;
        else {
            if (b < 0 || b > 63) { kind = PURE_FAULT_LIMIT; FAILED; }
            if (arg == PURE_BITS_RIGHT) r = floor_shift(a, (unsigned)b);
            else if (floor_shift(r = (int64_t)((uint64_t)a << b), (unsigned)b) != a) { kind = PURE_FAULT_OVERFLOW; FAILED; }
        }
        stack[sp - 1].i64 = r;
        NEXT;
    }
    case PURE_OP_BYTE_AT: { /* the byte at a whole position inside the value */
        Slot at = stack[--sp];
        PureBytes v = stack[sp - 1].bytes;
        if (arg ? at.i64 < 0 || (uint64_t)at.i64 >= v.length
                : !(at.num >= 0 && at.num < (double)v.length) || at.num != floor(at.num)) {
            kind = PURE_FAULT_LIMIT;
            FAILED;
        }
        unsigned char byte = 0;
        v.offset += arg ? (uint64_t)at.i64 : (uint64_t)at.num;
        v.length = 1;
        if ((status = visit_value(app, arena, v, take_byte, &byte)) != PURE_OK) STOPPED(status);
        stack[sp - 1].i64 = byte;
        NEXT;
    }
    case PURE_OP_BYTES_OF: { /* a list of one i64, an item a byte */
        const PureList *list = app->lists[arg];
        size_t count = pure_list_count(list);
        if (!reserve(arena, count)) STOPPED(PURE_ALLOCATION);
        for (size_t i = 0; i < count; ++i) {
            int64_t byte = pure_list_get(list, i, 0).i64;
            if (byte < 0 || byte > 255) { kind = PURE_FAULT_LIMIT; FAILED; }
            arena->bytes[arena->used + i] = (unsigned char)byte;
        }
        stack[sp++].bytes = (PureBytes){FROM_ARENA, 0, {0, 0}, arena->used, count};
        arena->used += count;
        NEXT;
    }
    case PURE_OP_INDEX_AT: /* the item at a whole position */
        if (top->num != floor(top->num)) { kind = PURE_FAULT_LIMIT; FAILED; }
        if (top->num < 0 || top->num >= (double)pure_list_count(app->lists[arg])) { kind = PURE_FAULT_NOT_FOUND; FAILED; }
        top->bytes.owner = (uint16_t)arg;
        NEXT;
    case PURE_OP_BYTES_SLICE: { /* from a up to before c, both whole and within */
        Slot high = stack[--sp], low = stack[--sp];
        PureBytes *v = &stack[sp - 1].bytes;
        if (arg ? low.i64 < 0 || high.i64 < low.i64 || (uint64_t)high.i64 > v->length
                : !(low.num >= 0 && high.num >= low.num && high.num <= (double)v->length) || low.num != floor(low.num) ||
                      high.num != floor(high.num)) {
            kind = PURE_FAULT_LIMIT;
            FAILED;
        }
        uint64_t from = arg ? (uint64_t)low.i64 : (uint64_t)low.num, to = arg ? (uint64_t)high.i64 : (uint64_t)high.num;
        v->offset += from;
        v->length = to - from;
        NEXT;
    }
    case PURE_OP_BYTE: {
        if (top->i64 < 0 || top->i64 > 255) { kind = PURE_FAULT_LIMIT; FAILED; }
        unsigned char byte = (unsigned char)top->i64;
        if ((status = keep_bytes(arena, &byte, 1, &top->bytes)) != PURE_OK) STOPPED(status);
        NEXT;
    }
    case PURE_OP_BYTES_LOAD:
        stack[sp++].bytes = app->held[arg & 0xFF].by_ref ? app->held[arg & 0xFF].ref
                                                         : (PureBytes){FROM_HELD, (uint16_t)(arg & 0xFF), {0, 0}, 0, app->held[arg & 0xFF].size};
        NEXT;
    case PURE_OP_BYTES_STORE: {
        int held = hold(app, arena, arg & 0xFF, stack[--sp].bytes, arg & PURE_HELD_APPEND);
        if (held == 1) { kind = PURE_FAULT_MEMORY; FAILED; }
        if (held) STOPPED(PURE_ALLOCATION);
        NEXT;
    }
    case PURE_OP_NUM_PARSE: { /* a text written as a JSON number, its nearest number */
        PureSpan span;
        size_t mark = arena->used;
        if ((status = contiguous(app, arena, top->bytes, &span)) != PURE_OK) STOPPED(status);
        double number;
        bool read = pure_parse_number(span.data, span.size, &number);
        arena->used = mark;
        if (!read) { kind = PURE_FAULT_LIMIT; FAILED; }
        top->num = number;
        NEXT;
    }
    case PURE_OP_LIST_VALUE: { /* a copy of an event's list's items (an empty one is 0, made of nothing), or the list
                                  * itself, lent, where nothing changes it after this (plan, N4): its size counted alike */
        if (!pure_list_count(app->lists[arg])) { stack[sp++].i64 = 0; NEXT; }
        uint64_t made = op->plan ? lend(app, app->lists[arg]) : nest(app, m->state_ref[arg], app->lists[arg]);
        if (!made) STOPPED(PURE_ALLOCATION);
        if (!grows(app, (Held){0, 0}, (Held){0, pure_list_size(app->nested.list[made - 1])})) { kind = PURE_FAULT_MEMORY; FAILED; }
        stack[sp++].i64 = (int64_t)made;
        NEXT;
    }
    case PURE_OP_LIST_BIND: { /* the register reads the list (shared: no one changes it in place from now) */
        uint64_t list = (uint64_t)stack[--sp].i64;
        if (!app->unbound[arg]) app->unbound[arg] = app->lists[arg], app->bound[app->bound_count++] = arg;
        app->lists[arg] = list ? app->nested.list[list - 1] : app->unbound[arg];
        if (list) ++app->nested.shared[list - 1];
        NEXT;
    }
    case PURE_OP_APPEND_IN: { /* an item into the list in a field, made the field's own first when another shares it */
        uint32_t list = list_of(arg), field = item_field(arg), inner = nested_record(m, m->record[m->state_ref[list]].first + field);
        const PureRecord *record = &m->record[inner];
        size_t base = sp - record->count, at = (size_t)stack[base - 1].num;
        sp = base - 1;
        if (at >= pure_list_count(app->lists[list])) { kind = PURE_FAULT_NOT_FOUND; FAILED; }
        uint64_t into = (uint64_t)pure_list_get(app->lists[list], at, field).i64;
        if (!into || app->nested.shared[into - 1] > 1) {
            uint64_t own = nest(app, inner, into ? app->nested.list[into - 1] : NULL);
            if (!own) STOPPED(PURE_ALLOCATION);
            if (into) --app->nested.shared[into - 1];
            app->nested.shared[own - 1] = 1;
            if ((status = pure_list_set(app->lists[list], at, field, (PureCell){.i64 = (int64_t)own}, NULL)) != PURE_OK) REFUSED;
            into = own;
        }
        PureList *in = app->nested.list[into - 1];
        size_t mark = arena->used, total = 0;
        for (unsigned f = 0; f < record->count; ++f)
            if (pure_record_bytes(m->record_field_type[record->first + f])) total += (size_t)stack[base + f].bytes.length;
        if (!reserve(arena, total)) STOPPED(PURE_ALLOCATION);
        PureCell cells[PURE_RECORD_FIELDS];
        for (unsigned f = 0; f < record->count; ++f) {
            PureSpan span = {0};
            if (pure_record_bytes(m->record_field_type[record->first + f]) &&
                (status = contiguous(app, arena, stack[base + f].bytes, &span)) != PURE_OK)
                STOPPED(status);
            cells[f] = (PureCell){.i64 = stack[base + f].i64, .text = (unsigned char *)span.data, .size = span.size};
        }
        Held before = {0, pure_list_size(in)};
        status = pure_list_insert(in, pure_list_count(in), cells, NULL);
        arena->used = mark;
        if (status != PURE_OK) REFUSED;
        if (m->record_nested[inner]) share_cells(app, m, inner, cells);
        if (!grows(app, before, (Held){0, pure_list_size(in)})) { kind = PURE_FAULT_MEMORY; FAILED; }
        NEXT;
    }
    case PURE_OP_KEPT_GET: case PURE_OP_KEPT_SET: case PURE_OP_KEPT_APPEND: case PURE_OP_KEPT_INSERT: case PURE_OP_KEPT_APPEND_IN: {
        int how = kept_step(app, arena, values, op, stack, sp); /* a kept list's lists (027's stage 3): out of the way */
        sp -= kept_pops(m, op);
        if (how & 3) {
            if ((how & 3) == 1) { kind = (PureFault)(how >> 2); FAILED; }
            status = (PureResult)(how >> 2);
            if ((how & 3) == 2) REFUSED;
            STOPPED(status);
        }
        NEXT;
    }
    case PURE_OP_CALL: case PURE_OP_RETURN: /* run_calls' (the frames are its): no other run meets one */
        frame->fault_pc = pc;
        return !batches ? STEP_FAULT : op->op == PURE_OP_CALL ? STEP_CALL : STEP_BACK;
    case PURE_OP_CHAR_AT: { /* the character beginning at a byte position of a text (UTF-8: whole from there) */
        int64_t at = stack[--sp].i64;
        PureBytes v = stack[sp - 1].bytes;
        if (at < 0 || (uint64_t)at >= v.length) { kind = PURE_FAULT_LIMIT; FAILED; }
        unsigned char b[4] = {0};
        Copy into = {b};
        v.offset += (uint64_t)at;
        v.length = v.length - (uint64_t)at < 4 ? v.length - (uint64_t)at : 4;
        if ((status = visit_value(app, arena, v, copy_out, &into)) != PURE_OK) STOPPED(status);
        if ((b[0] & 0xC0) == 0x80) { kind = PURE_FAULT_LIMIT; FAILED; } /* within a character */
        stack[sp - 1].i64 = b[0] < 0x80 ? b[0] : b[0] < 0xE0 ? (b[0] & 0x1F) << 6 | (b[1] & 0x3F) :
                            b[0] < 0xF0 ? (b[0] & 0x0F) << 12 | (b[1] & 0x3F) << 6 | (b[2] & 0x3F) :
                            (b[0] & 0x07) << 18 | (b[1] & 0x3F) << 12 | (b[2] & 0x3F) << 6 | (b[3] & 0x3F);
        NEXT;
    }
    case PURE_OP_CHAR: { /* the text of a code point */
        int64_t c = top->i64;
        if (c < 0 || c > 0x10FFFF) { kind = PURE_FAULT_LIMIT; FAILED; }
        if (c >= 0xD800 && c < 0xE000) { kind = PURE_FAULT_ENCODING; FAILED; }
        unsigned char b[4];
        size_t n = c < 0x80 ? 1 : c < 0x800 ? 2 : c < 0x10000 ? 3 : 4;
        if (n == 1) b[0] = (unsigned char)c;
        else {
            for (size_t k = n - 1; k > 0; --k, c >>= 6) b[k] = (unsigned char)(0x80 | (c & 0x3F));
            b[0] = (unsigned char)((0xF00 >> n & 0xF0) | c); /* 110x, 1110, 11110 */
        }
        if ((status = keep_bytes(arena, b, n, &top->bytes)) != PURE_OK) STOPPED(status);
        NEXT;
    }
    default: break;
    }
    double b = stack[--sp].num, a = stack[sp - 1].num, value;
    switch (op->op) {
    case PURE_OP_ADD: value = a + b; break;
    case PURE_OP_SUB: value = a - b; break;
    case PURE_OP_MUL: value = a * b; break;
    case PURE_OP_DIV:
        if (b == 0) { kind = PURE_FAULT_DIVIDE; FAILED; }
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
        if (b == 0) { kind = PURE_FAULT_DIVIDE; FAILED; }
        value = modulo(a, b);
        break;
    case PURE_OP_FLOORDIV: /* MOD's quotient, exactly (027's stage 5): a less its remainder, divided, as Python's // */
        if (b == 0) { kind = PURE_FAULT_DIVIDE; FAILED; }
        value = floor_quotient(a, b);
        break;
    default: value = maximum(a, b); break;
    }
    if (!isfinite(value)) { kind = PURE_FAULT_NON_FINITE; FAILED; }
    stack[sp - 1].num = value;
    NEXT;
}

/* What a run ends with: its value (a view's, or a fold block's numbers), a fault, or a result as it is. */
static inline __attribute__((always_inline)) PureResult finish(Frame *frame, int how, Slot *result) {
    if (how == STEP_NEXT) {
        if (result) memcpy(result, frame->stack, frame->sp * sizeof(*frame->stack));
        return PURE_OK;
    }
    if (how == STEP_RETURN) return frame->status;
    if (how == STEP_REFUSED) {
        PureResult status = frame->status;
        frame->kind = status == PURE_LIMIT ? PURE_FAULT_LIMIT : status == PURE_NOT_FOUND ? PURE_FAULT_NOT_FOUND : PURE_FAULT_NONE;
        if (frame->kind == PURE_FAULT_NONE) return status;
    }
    return fault(frame->app, frame->kind, frame->fault_pc);
}

#undef NEXT
#undef FAILED
#undef REFUSED
#undef STOPPED

/* The steps compiled code calls (module-run.h). */
int pure_run_step(Frame *frame, uint32_t pc) {
    uint32_t next;
    return step(frame, &frame->m->code[pc], pc, &next, false);
}

int pure_run_each(Frame *frame, uint32_t pc) {
    uint32_t next;
    unsigned depth = frame->depth;
    int how = enter_each(frame, &frame->m->code[pc], pc, &next);
    return how != STEP_NEXT ? how : frame->depth > depth ? STEP_NEXT : STEP_SKIP;
}

int pure_run_loop(Frame *frame, uint32_t pc) {
    uint32_t next;
    unsigned depth = frame->depth;
    int how = enter_loop(frame, &frame->m->code[pc], pc, &next);
    return how != STEP_NEXT ? how : frame->depth > depth ? STEP_NEXT : STEP_SKIP;
}

void pure_run_break(Frame *frame) { (void)leave_loop(frame); }

int pure_run_item(Frame *frame, uint32_t pc) {
    const PureOp *op = &frame->m->code[pc];
    const PureList *list = frame->app->lists[list_of(op->arg)];
    size_t at = (size_t)frame->stack[frame->sp - 1].num;
    PureRun run;
    if (at < pure_list_count(list) && !pure_list_form(list) && pure_list_run(list, at, &run)) {
        uint64_t id;
        frame->run.list = list;
        pure_list_stamp(list, &id, &frame->run.version);
        frame->run.start = run.start, frame->run.count = run.count;
        for (unsigned f = 0; f < 16; ++f) frame->run.columns[f] = run.column[f].numbers;
    }
    uint32_t next;
    return step(frame, op, pc, &next, false);
}

int pure_run_rest(Frame *frame, uint32_t pc) {
    unsigned depth = frame->depth;
    for (;;) {
        if (frame->depth < depth) return STEP_SKIP; /* it left the loop (BREAK) */
        if (pc >= frame->end) { /* a loop's block ran: the compiled code's, done; one opened here, its iteration */
            if (frame->depth == depth) return STEP_NEXT;
            int how = iterate(frame, &pc);
            if (how != STEP_NEXT) return how;
            continue;
        }
        int how = step(frame, &frame->m->code[pc], pc, &pc, false);
        if (how != STEP_NEXT) return how;
    }
}

int pure_run_iterate(Frame *frame) {
    uint32_t next = UINT32_MAX;
    int how = iterate(frame, &next);
    return how != STEP_NEXT ? how : next != UINT32_MAX ? STEP_NEXT : STEP_DONE;
}

/* A frame for block, its fields set one by one: a block runs often (a
 * fold's item, a view's cell), and filling the whole frame each time (the
 * compiled code's run among it) costs more than the block. */
static inline __attribute__((always_inline)) void open_frame(Frame *frame, const PureModule *m, PureValue *values, PureBlock block,
                                                             Event *ev, Slot *stack, Loop *loops) {
    frame->m = m, frame->values = values, frame->ev = ev, frame->app = ev->app, frame->arena = ev->arena;
    frame->stack = stack, frame->sp = 0, frame->loops = loops, frame->depth = 0, frame->kind = PURE_FAULT_NONE;
    frame->end = block.start + block.length;
}

/* A block run by its compiled code. */
static __attribute__((noinline)) PureResult run_compiled(PureCompiled code, const PureModule *m, PureValue *values,
                                                         PureBlock block, Event *ev, Slot *result) {
    Slot stack[PURE_MODULE_STACK];
    Loop loops[PURE_MODULE_NESTING];
    Frame frame;
    open_frame(&frame, m, values, block, ev, stack, loops);
    frame.run.list = NULL; /* no run kept yet */
    return finish(&frame, code(&frame), result);
}

/* ---- Calls (027's 2c). A call's frame, in the app's blocks of frames (they never move: a loop points into its frame):
 * the frame below, where the caller was (what follows its CALL, where its code ends, its loops, stack, locals and walk),
 * where the blocks stood before it and its bytes (counted in the event's room); then what it keeps of the caller's own
 * (the lists the function's code names, its held values) and the callee's stack, locals and loops. */
typedef struct Call {
    struct Call *below;
    uint32_t back, end, function;
    unsigned depth, block;
    size_t sp, used, bytes, row;
    Slot *stack;
    Loop *loops;
    PureValue *locals;
    Walk *walk;
} Call;
typedef __typeof__(((PureApp *)0)->held[0]) Kept;
_Static_assert(sizeof(Call) <= PURE_CALL_BYTES && sizeof(Slot) <= PURE_CALL_VALUE && sizeof(PureValue) <= PURE_CALL_LOCAL &&
               sizeof(Loop) <= PURE_CALL_LOOP && sizeof(Kept) <= PURE_CALL_HELD && sizeof(PureList *) <= PURE_CALL_REGISTER,
               "a frame counts at least what it holds");

/* Room for a frame of bytes among the app's blocks: where they stood before it, or NULL out of memory. */
static void *frame_room(PureApp *app, size_t bytes, unsigned *block, size_t *used) {
    __typeof__(app->calls) *c = &app->calls;
    *block = c->at, *used = c->used;
    if (c->count && c->sizes[c->at] - c->used >= bytes) {
        void *room = c->blocks[c->at] + c->used;
        c->used += bytes;
        return room;
    }
    unsigned next = c->count ? c->at + 1 : 0; /* the blocks after the one in use are free */
    if (next < c->count && c->sizes[next] < bytes) free(c->blocks[next]), c->blocks[next] = NULL;
    if (next == c->count) {
        unsigned char **blocks = realloc(c->blocks, (c->count + 1) * sizeof(*blocks));
        if (blocks) c->blocks = blocks;
        size_t *sizes = blocks ? realloc(c->sizes, (c->count + 1) * sizeof(*sizes)) : NULL;
        if (!sizes) return NULL;
        c->sizes = sizes;
        c->blocks[c->count++] = NULL;
    }
    if (!c->blocks[next]) {
        size_t size = (size_t)64 << 10 << (next < 8 ? next : 8); /* 64 KB, twice as much each block on to 16 MB */
        if (size < bytes) size = bytes;
        if (!(c->blocks[next] = malloc(size))) return NULL;
        c->sizes[next] = size;
    }
    c->at = next, c->used = bytes;
    return c->blocks[next];
}

/* An empty list of an event's list state for a call: one a call let go, or a new one (NULL: out of memory). */
static PureList *spare_list(PureApp *app, uint32_t state) {
    __typeof__(app->spare[0]) *spare = &app->spare[state];
    return spare->count ? spare->lists[--spare->count] : new_list(app->module, app->module->state_ref[state], false);
}

/* A call's list let go: emptied (what it held no longer counted) and kept for the next calls, unless what it keeps
 * when empty passes what a frame counts for it. */
static void let_list_go(PureApp *app, uint32_t state, PureList *list) {
    Held before = list_held(app, list);
    pure_list_clear(list);
    (void)grows(app, before, list_held(app, list));
    __typeof__(app->spare[0]) *spare = &app->spare[state];
    if (pure_list_extra(list) > PURE_CALL_LIST) { pure_list_free(list); return; }
    if (spare->count == spare->capacity) {
        unsigned capacity = spare->capacity ? 2 * spare->capacity : 4;
        PureList **lists = realloc(spare->lists, capacity * sizeof(*lists));
        if (!lists) { pure_list_free(list); return; }
        spare->lists = lists, spare->capacity = capacity;
    }
    spare->lists[spare->count++] = list;
}

static void drop_spares(PureApp *app) {
    for (unsigned i = 0; i < app->laid; ++i) {
        for (unsigned k = 0; k < app->spare[i].count; ++k) pure_list_free(app->spare[i].lists[k]);
        free(app->spare[i].lists);
        app->spare[i].lists = NULL, app->spare[i].count = app->spare[i].capacity = 0;
    }
}

/* The first count of the lists and registers function's code names given back what a call kept of them (its own lists
 * let go). */
static void restore_lists(PureApp *app, uint32_t function, PureList **kept, unsigned count) {
    const PureModule *m = app->module;
    const uint32_t *keeps = m->keeps + m->keep_start[function];
    for (unsigned k = 0; k < count; ++k) {
        uint32_t state = keeps[k] & ~(uint32_t)PURE_KEEP_REGISTER;
        if (!(keeps[k] & PURE_KEEP_REGISTER)) let_list_go(app, state, app->lists[state]);
        app->lists[state] = kept[k];
    }
}

/* What a call of function keeps of its caller's: the lists and registers its code names (its own). */
static unsigned call_keeps(const PureModule *m, uint32_t function) { return m->keep_start[function + 1] - m->keep_start[function]; }

/* A frame closed: the caller's lists, registers and held values given back (the callee's let go), its locals and walk,
 * the frame's room in the blocks and in the event's room. The frame below. */
static Call *give_back(PureApp *app, Call *call, Event *ev) {
    const PureFunction *f = &app->module->function[call->function];
    unsigned keeps = call_keeps(app->module, call->function), j = 0;
    PureList **kept = (PureList **)(call + 1);
    Kept *held = (Kept *)(kept + keeps);
    restore_lists(app, call->function, kept, keeps);
    for (uint32_t left = f->held; left; left &= left - 1, ++j) {
        Kept *h = &app->held[__builtin_ctz(left)];
        if (!h->by_ref) (void)grows(app, (Held){0, h->size}, (Held){0, 0});
        free(h->data);
        *h = held[j];
    }
    ev->locals = call->locals, ev->walk = call->walk, ev->row = call->row;
    (void)grows(app, (Held){0, call->bytes}, (Held){0, 0});
    app->calls.at = call->block, app->calls.used = call->used;
    return call->below;
}

/* A bytes or text value a call would lose (held by name in a slot the callee has its own of): a copy in the arena. */
static bool kept_apart(PureApp *app, PureArena *arena, const PureFunction *f, uint8_t type, PureBytes *v) {
    if (!pure_record_bytes(type) || v->source != FROM_HELD || !(f->held >> v->owner & 1))
        return true;
    size_t start = arena->used;
    if (append_value(app, arena, *v) != PURE_OK) return false;
    *v = (PureBytes){FROM_ARENA, 0, {0, 0}, start, v->length};
    return true;
}

/* CALL (just before *pc): its function's frame open (*top), its block next. STEP_NEXT, or a fault (WORK, MEMORY) or a
 * stop out of memory, nothing opened. */
static __attribute__((noinline, cold)) int enter_call(Frame *frame, Call **top, uint32_t *pc) {
    const PureModule *m = frame->m;
    PureApp *app = frame->app;
    Event *ev = frame->ev;
    uint32_t function = m->code[*pc - 1].arg;
    const PureFunction *f = &m->function[function];
    unsigned keeps = call_keeps(m, function), holds = (unsigned)__builtin_popcount(f->held);
    size_t bytes = (sizeof(Call) + keeps * sizeof(PureList *) + holds * sizeof(Kept) + f->height * sizeof(Slot) +
                    f->locals * sizeof(PureValue) + f->nesting * sizeof(Loop) + 15) & ~(size_t)15;
    size_t counted = PURE_CALL_BYTES + (size_t)f->height * PURE_CALL_VALUE + (size_t)f->locals * PURE_CALL_LOCAL +
                     (size_t)f->nesting * PURE_CALL_LOOP + (size_t)f->own_lists * PURE_CALL_LIST +
                     (size_t)holds * PURE_CALL_HELD + (size_t)f->own_registers * PURE_CALL_REGISTER;
    frame->fault_pc = *pc - 1;
    if (!charge(app, f->code.length)) { frame->kind = PURE_FAULT_WORK; return STEP_FAULT; }
    if (!grows(app, (Held){0, 0}, (Held){0, counted})) { frame->kind = PURE_FAULT_MEMORY; return STEP_FAULT; }
    unsigned block;
    size_t used, base = frame->sp - f->params;
    Call *call = frame_room(app, bytes, &block, &used);
    if (!call) { frame->status = PURE_ALLOCATION; return STEP_RETURN; }
    PureList **kept = (PureList **)(call + 1);
    Kept *held = (Kept *)(kept + keeps);
    Slot *stack = (Slot *)(held + holds);
    PureValue *locals = (PureValue *)(stack + f->height);
    bool whole = true;
    unsigned k = 0;
    for (unsigned i = 0; whole && i < f->params; ++i) { /* its arguments, its stack as it begins */
        stack[i] = frame->stack[base + i];
        whole = kept_apart(app, frame->arena, f, m->signature_type[f->first + i], &stack[i].bytes);
    }
    const uint32_t *own_states = m->keeps + m->keep_start[function];
    for (; whole && k < keeps; ++k) { /* its own lists, registers */
        uint32_t state = own_states[k] & ~(uint32_t)PURE_KEEP_REGISTER;
        PureList *own = own_states[k] & PURE_KEEP_REGISTER ? (app->unbound[state] ? app->unbound[state] : app->lists[state])
                                                           : spare_list(app, state);
        if (!own) {
            restore_lists(app, function, kept, k);
            whole = false;
            break;
        }
        kept[k] = app->lists[state], app->lists[state] = own;
    }
    if (!whole) {
        app->calls.at = block, app->calls.used = used;
        frame->status = PURE_ALLOCATION;
        return STEP_RETURN;
    }
    unsigned j = 0;
    for (uint32_t left = f->held; left; left &= left - 1, ++j) { /* its held values, empty */
        Kept *h = &app->held[__builtin_ctz(left)];
        held[j] = *h;
        *h = (Kept){0};
    }
    *call = (Call){.below = *top, .back = *pc, .end = frame->end, .function = function, .depth = frame->depth, .block = block,
                   .sp = base, .used = used, .bytes = counted, .row = ev->row, .stack = frame->stack, .loops = frame->loops,
                   .locals = ev->locals, .walk = ev->walk};
    if (f->locals) memset(locals, 0, f->locals * sizeof(*locals));
    frame->stack = stack, frame->sp = f->params, frame->loops = (Loop *)(locals + f->locals), frame->depth = 0;
    frame->end = f->code.start + f->code.length;
    ev->locals = locals, ev->walk = NULL;
    *top = call;
    *pc = f->code.start;
    return STEP_NEXT;
}

/* RETURN (just before *pc): its results in the place of the call's arguments, the frame closed, what follows the CALL
 * next. STEP_NEXT, or a stop out of memory (the frame still open). */
static __attribute__((noinline, cold)) int leave_call(Frame *frame, Call **top, uint32_t *pc) {
    const PureModule *m = frame->m;
    Call *call = *top;
    const PureFunction *f = &m->function[call->function];
    Slot *results = frame->stack + frame->sp - f->results;
    for (unsigned i = 0; i < f->results; ++i)
        if (!kept_apart(frame->app, frame->arena, f, m->signature_type[f->first + f->params + i], &results[i].bytes)) {
            frame->status = PURE_ALLOCATION;
            return STEP_RETURN;
        }
    for (unsigned i = 0; i < f->results; ++i) call->stack[call->sp + i] = results[i];
    frame->stack = call->stack, frame->sp = call->sp + f->results, frame->loops = call->loops, frame->depth = call->depth;
    frame->end = call->end;
    *pc = call->back;
    *top = give_back(frame->app, call, frame->ev);
    return STEP_NEXT;
}

/* Every frame still open closed (a run stopped in a call). */
static __attribute__((noinline, cold)) void unwind_calls(Frame *frame, Call *top) {
    while (top) top = give_back(frame->app, top, frame->ev);
}

/* The executor's run of a block of a module with functions (027's 2c): run_batches' steps and batches, and calls: a
 * CALL opens its function's frame and its block runs on in this loop, a RETURN closes it. Its own copy of the steps,
 * as run_batches is. */
/* Room for a run's loops nested deeper than PURE_MODULE_NESTING (027's 4f-4): the app's, grown to nesting; NULL without. */
static __attribute__((cold, noinline)) Loop *deep_loops(PureApp *app, uint32_t nesting) {
    if (app->deep_room < nesting) {
        Loop *grown = realloc(app->deep, nesting * sizeof(*grown));
        if (!grown) return NULL;
        app->deep = grown, app->deep_room = nesting;
    }
    return app->deep;
}

static __attribute__((noinline, aligned(64))) PureResult run_calls(const PureModule *m, PureValue *values, PureBlock block, Event *ev,
                                                      Slot *result) {
    Slot stack[PURE_MODULE_STACK];
    Loop few[PURE_MODULE_NESTING], *loops = m->nesting > PURE_MODULE_NESTING ? deep_loops(ev->app, m->nesting) : few;
    if (!loops) return PURE_ALLOCATION;
    Frame frame;
    open_frame(&frame, m, values, block, ev, stack, loops);
    Call *top = NULL;
    int how = STEP_NEXT;
    for (uint32_t pc = block.start;;) {
        if (pc >= frame.end) {
            if (!frame.depth || (how = iterate(&frame, &pc)) != STEP_NEXT) break;
            continue;
        }
        if ((how = step(&frame, &m->code[pc], pc, &pc, true)) == STEP_NEXT) continue;
        if (how != STEP_CALL && how != STEP_BACK && how != STEP_BATCH) break;
        Frame apart = frame; /* the frame's copy (no call sees the frame itself: its fields stay in registers) */
        if (how == STEP_CALL) how = enter_call(&apart, &top, &pc);
        else if (how == STEP_BACK) how = leave_call(&apart, &top, &pc);
        else {
            uint32_t past = 0;
            how = open_batches(&apart, pc - 1, &past);
            pc = past;
        }
        frame = apart;
        if (how != STEP_NEXT) break;
    }
    if (top) {
        Frame apart = frame;
        unwind_calls(&apart, top);
        frame = apart;
    }
    return finish(&frame, how, result);
}

/* The executor's run of a block of a module with loops planned PLAN_BATCH:
 * run's, those loops in batches (module-batch.c). Its own copy of the
 * steps: in run's, a batch's way among them would cost every instruction
 * registers (research 126). */
static __attribute__((noinline, aligned(64))) PureResult run_batches(const PureModule *m, PureValue *values, PureBlock block, Event *ev,
                                                        Slot *result) {
    Slot stack[PURE_MODULE_STACK];
    Loop loops[PURE_MODULE_NESTING];
    Frame frame;
    open_frame(&frame, m, values, block, ev, stack, loops);
    int how = STEP_NEXT;
    for (uint32_t pc = block.start;;) {
        if (pc >= frame.end) {
            if (!frame.depth || (how = iterate(&frame, &pc)) != STEP_NEXT) break;
            continue;
        }
        if ((how = step(&frame, &m->code[pc], pc, &pc, true)) == STEP_NEXT) continue;
        if (how != STEP_BATCH) break;
        Frame apart = frame; /* the frame's copy (no call sees the frame itself: its fields stay in registers) */
        uint32_t past = 0;
        how = open_batches(&apart, pc - 1, &past);
        frame = apart, pc = past;
        if (how != STEP_NEXT) break;
    }
    return finish(&frame, how, result);
}

/* At a cache line's start: the steps' branches land where they would (research 126: else 10% more cycles, as many instructions). */
static __attribute__((aligned(64))) PureResult run(const PureModule *m, PureValue *values, PureBlock block, Event *ev, Slot *result) {
    PureCompiled code = m->jit ? pure_jit_code(m->jit, block) : NULL;
    if (code) return run_compiled(code, m, values, block, ev, result);
    if (m->runner) return m->runner == 2 ? run_calls(m, values, block, ev, result) : run_batches(m, values, block, ev, result);
    Slot stack[PURE_MODULE_STACK];
    Loop loops[PURE_MODULE_NESTING];
    Frame frame;
    open_frame(&frame, m, values, block, ev, stack, loops);
    int how = STEP_NEXT;
    for (uint32_t pc = block.start;;) {
        if (pc >= frame.end) { /* the block ran; or a loop's ran once: again, or the loop ends (its end may be its outer's) */
            if (!frame.depth || (how = iterate(&frame, &pc)) != STEP_NEXT) break;
            continue;
        }
        if ((how = step(&frame, &m->code[pc], pc, &pc, false)) != STEP_NEXT) break;
    }
    return finish(&frame, how, result);
}

PURE_COLD static PureResult new_text(PureDraft **out, const PureModule *m, uint32_t constant) {
    *out = pure_draft_new();
    if (!*out) return PURE_ALLOCATION;
    PureSpan initial = {m->constant_data + m->constant_offset[constant], m->constant_length[constant]};
    PureResult result = pure_draft_splice(*out, 0, 0, initial);
    if (result != PURE_OK) { pure_draft_close(*out); *out = NULL; }
    return result;
}

/* A stored draft given a transient text's bytes, to be saved (saved: none yet). */
/* Whether a draft's bytes are UTF-8 (a bytes state's, before it is a text's: 028). */
typedef struct { unsigned char *bytes; size_t used; } Drafted;
static bool drafted(void *context, PureSpan span) {
    Drafted *d = context;
    if (span.size) memcpy(d->bytes + d->used, span.data, span.size);
    d->used += span.size;
    return true;
}
PURE_COLD static bool draft_utf8(const PureDraft *draft) {
    size_t size = pure_draft_size(draft);
    Drafted into = {malloc(size ? size : 1), 0};
    bool valid = into.bytes && pure_draft_read(draft, 0, size, drafted, &into) == PURE_OK && into.used == size &&
                 pure_utf8_valid((PureSpan){into.bytes, size});
    free(into.bytes);
    return valid;
}

PURE_COLD static PureResult copy_text(PureDraft *to, const PureDraft *from, PureVersion *saved) {
    size_t size = pure_draft_size(from);
    unsigned char *bytes = size ? malloc(size) : NULL;
    if (size && !bytes) return PURE_ALLOCATION;
    Copy copy = {bytes};
    PureResult result = size ? pure_draft_read(from, 0, size, copy_out, &copy) : PURE_OK;
    if (result == PURE_OK) result = pure_draft_splice(to, 0, pure_draft_size(to), (PureSpan){bytes, size});
    free(bytes);
    *saved = (PureVersion){UINT64_MAX, UINT64_MAX};
    return result;
}

/* A draft in memory with a text's bytes, or NULL. */
static PureDraft *text_copy(const PureDraft *from) {
    PureDraft *copy = pure_draft_new();
    PureVersion unused;
    if (copy && copy_text(copy, from, &unused) != PURE_OK) {
        pure_draft_close(copy);
        copy = NULL;
    }
    return copy;
}

/* A list of record's items: one kept past an event holds a list field's value in its stored form, an event's the number
 * of one of the event's lists (027's 2b and 3). */
PURE_COLD static PureList *new_list(const PureModule *m, uint32_t record, bool kept) {
    const PureRecord *r = &m->record[record];
    uint8_t kinds[PURE_RECORD_FIELDS];
    for (unsigned f = 0; f < r->count; ++f) kinds[f] = cell_kind(m, r->first + f, kept);
    return pure_list_new(r->count, kinds);
}

void pure_app_finish(PureApp *app) {
    free(app->copied);
    app->copied = NULL;
    app->has_reply = false;
    app->payload = (PureSpan){0};
    app->arena.used = 0;
}

/* The start of a burst: what a failed save returns to. Text drafts stay held
 * until the save, so every version since stays readable; a stored list's
 * journal is as long as its changes since the last save. */
static void open_burst(PureApp *app) {
    if (app->open) return;
    const PureModule *m = app->module;
    memcpy(app->durable_values, app->values, m->states * sizeof(*app->values));
    app->durable_events = app->events;
    for (uint32_t i = 0; i < m->states; ++i) {
        if (app->texts[i]) {
            pure_draft_hold(app->texts[i], true);
            app->durable_texts[i] = pure_draft_version(app->texts[i]);
        }
        if (app->lists[i]) app->durable_lists[i] = pure_list_changes(app->lists[i]).size;
    }
    if (app->heard) app->durable_heard = pure_list_changes(app->heard).size;
    if (app->outbox) app->durable_outbox = pure_list_changes(app->outbox).size;
    app->durable_sent_saved = app->sent_saved;
    app->open = true;
}

/* End a burst, keeping it (saved) or undoing all of it. */
static void close_burst(PureApp *app, bool keep) {
    if (!app->open) return;
    const PureModule *m = app->module;
    for (uint32_t i = 0; i < m->states; ++i) {
        if (app->texts[i]) {
            if (!keep) pure_draft_restore(app->texts[i], app->durable_texts[i]);
            pure_draft_hold(app->texts[i], false);
        }
        if (app->lists[i] && !keep) pure_list_forget(app->lists[i], app->durable_lists[i]);
    }
    if (app->heard && !keep) pure_list_forget(app->heard, app->durable_heard);
    if (app->outbox && !keep) pure_list_forget(app->outbox, app->durable_outbox);
    if (keep) pure_undo_commit(&app->undo);
    else {
        pure_undo_rollback(&app->undo, 0);
        memcpy(app->values, app->durable_values, m->states * sizeof(*app->values));
        app->events = app->durable_events;
        ++app->changes;
    }
    app->open = false;
}

/* After a save: what it stored is the links' saved state, every send in the
 * outbox saved; or, when it failed, the links return to the last saved
 * state (the sends, trims and numbers heard go back with the burst's list
 * changes). */
static void settle_links(PureApp *app, bool saved) {
    if (saved) {
        app->sent_saved = app->outbox ? pure_list_count(app->outbox) : 0;
        memcpy(app->saved_sent, app->last_sent, app->module->inputs * sizeof(*app->saved_sent));
    } else {
        app->sent_saved = app->durable_sent_saved;
        memcpy(app->last_sent, app->saved_sent, app->module->inputs * sizeof(*app->last_sent));
    }
    app->links_changed = false;
}

PureResult pure_app_save(PureApp *app) {
    PureResult result = pure_save_commit(app, app->values);
    settle_links(app, result == PURE_OK);
    close_burst(app, result == PURE_OK);
    /* Images due are written now, unless the host writes them on another thread. */
    for (PureCheckpoint *c; result == PURE_OK && !app->background_checkpoints && (c = pure_app_checkpoint(app));)
        pure_app_checkpointed(app, c, pure_checkpoint_write(c));
    return result;
}

bool pure_app_defer(PureApp *app, bool deferred) {
    if (deferred && !pure_save_enabled(app)) return false;
    app->deferred = deferred;
    return true;
}

/* ---- Snapshots. An app's snapshots are numbered as they are made (their
 * epoch), and the ones still held are linked, oldest to newest, under their
 * register (which each of them holds, so it outlives the app). A list's node
 * or text the app lets go while a snapshot is its newest may be read by that
 * snapshot and by older ones made after it was: it goes with that snapshot,
 * and when that one goes, to the newest older one still held that may read
 * it (made after it was), or back to the app, whose lists make their next
 * nodes of it (what they do not need goes back to the system). So a
 * snapshot held for long keeps what it shares, the states as they were
 * then, and nothing made since. The links and retired lists move under the
 * register's lock, a spin (none holds it long; what is freed is freed
 * outside it). */
typedef struct {
    unsigned refs;        /* the snapshots not yet freed */
    bool locked;
    uint64_t epoch;       /* snapshots made */
    PureRetired returned; /* what no snapshot reads any more, for the app's lists (pure_retired_reuse) */
} Register;

struct PureSnapshot {
    unsigned refs;
    const PureModule *module;
    uint64_t changes, epoch;
    uint32_t states;
    PureValue *values; /* each state's (027's 4f-2), after it in its block */
    PureDraft **texts;
    PureList **lists;
    PureList *runtime[PURE_LIST_RUNTIME]; /* only in a checkpoint's of one */
    PureRetired retired;
    struct PureSnapshot *older, *newer; /* the held ones beside it */
    Register *all;
};

static void lock_register(Register *r) {
    while (__atomic_test_and_set(&r->locked, __ATOMIC_ACQUIRE)) { /* another thread moves links: a moment */ }
}
static void unlock_register(Register *r) { __atomic_clear(&r->locked, __ATOMIC_RELEASE); }

void pure_snapshot_hold(PureSnapshot *snapshot) { __atomic_add_fetch(&snapshot->refs, 1, __ATOMIC_RELAXED); }

const PureList *pure_snapshot_list(const PureSnapshot *snapshot, uint32_t state) {
    return state >= PURE_LIST_HEARD ? snapshot->runtime[state - PURE_LIST_HEARD] : snapshot->lists[state];
}

const PureModule *pure_snapshot_module(const PureSnapshot *snapshot) { return snapshot->module; }

bool pure_snapshot_release(PureSnapshot *snapshot) {
    if (!snapshot || __atomic_sub_fetch(&snapshot->refs, 1, __ATOMIC_ACQ_REL)) return false;
    Register *all = snapshot->all;
    if (all) { /* out of the links: what the one before may read goes to it (still linked, so still there) */
        lock_register(all);
        PureSnapshot *older = snapshot->older;
        if (older) older->newer = snapshot->newer;
        if (snapshot->newer) snapshot->newer->older = older;
        pure_retired_pass(&snapshot->retired, older ? older->epoch : 0, older ? &older->retired : NULL, &all->returned);
        unlock_register(all);
    }
    pure_retired_free(&snapshot->retired); /* what it had when never linked, else its room */
    for (uint32_t i = 0; i < snapshot->states; ++i) {
        pure_draft_close(snapshot->texts[i]);
        pure_list_free(snapshot->lists[i]);
    }
    for (unsigned r = 0; r < PURE_LIST_RUNTIME; ++r) pure_list_free(snapshot->runtime[r]);
    free(snapshot);
    if (all && !__atomic_sub_fetch(&all->refs, 1, __ATOMIC_ACQ_REL)) {
        pure_retired_free(&all->returned); /* the app is gone: no list takes it back */
        free(all);
    }
    return true;
}

static void let_own_go(PureApp *app);

/* The arrays of each state an app holds (027's 4f-2) laid out from base for states of them: the bytes they take (a
 * stored list's slots then the runtime's two). */
static size_t lay_states(PureApp *app, uint32_t states, uintptr_t base) {
    size_t at = 0, slots = (size_t)states + PURE_LIST_RUNTIME;
#define LAY(member, count) (app->member = (void *)(base + at), at += ((size_t)(count) * sizeof(*app->member) + 7) & ~(size_t)7)
    LAY(values, states), LAY(texts, states), LAY(lists, states), LAY(list_drafts, states);
    LAY(image_seq, slots), LAY(logged_since, slots), LAY(logged, slots);
    LAY(saved, states), LAY(durable_values, states), LAY(durable_texts, states), LAY(durable_lists, states);
    LAY(own, states), LAY(unbound, states), LAY(bound, states), LAY(spare, states), LAY(pending, states);
    LAY(marked_texts, states), LAY(marked_lists, states);
#undef LAY
    return at;
}

/* A new block of them for states, zeroed, laid out in app (its old one the caller's to give back first, or after it
 * moved what it keeps); false without room (app as it was). */
static bool lay_app(PureApp *app, uint32_t states) {
    PureApp laid = *app;
    size_t size = lay_states(&laid, states, 0);
    unsigned char *base = calloc(1, size ? size : 1);
    if (!base) return false;
    lay_states(app, states, (uintptr_t)base);
    app->states = base, app->laid = states, app->bound_count = 0;
    return true;
}

void pure_app_stop(PureApp *app) {
    unbind(app); /* a register's own list goes with the lists, the bound one with the event's */
    pure_app_finish(app);
    close_burst(app, true); /* unsaved changes are dropped with the app */
    let_own_go(app);
    for (uint32_t i = 0; i < app->laid && !app->reading; ++i) {
        pure_draft_close(app->texts[i]);
        pure_list_free(app->lists[i]);
        pure_draft_close(app->list_drafts[i]);
    }
    pure_list_free(app->outbox); /* the runtime's lists too, before what they let go is handed on */
    pure_list_free(app->heard);
    /* Items the newest snapshot may still read go with it. */
    if (app->snapshot) {
        lock_register(app->snapshot->all);
        pure_retired_pass(&app->retired, UINT64_MAX, &app->snapshot->retired, NULL);
        unlock_register(app->snapshot->all);
        pure_snapshot_release(app->snapshot);
    }
    pure_retired_free(&app->retired);
    for (unsigned i = 0; i < PURE_MODULE_HELD; ++i) free(app->held[i].data);
    for (unsigned i = 0; i < app->calls.count; ++i) free(app->calls.blocks[i]);
    free(app->calls.blocks), free(app->calls.sizes);
    drop_spares(app);
    drop_nested(app);
    free(app->nested.list);
    free(app->nested.shared);
    pure_draft_close(app->stored_values);
    free(app->carried);
    free(app->by_input);
    free(app->deep);
    pure_draft_close(app->stored_links);
    pure_draft_close(app->stored_seq);
    pure_save_close_start(app);
    pure_log_close(app->log);
    pure_undo_free(&app->undo);
    for (unsigned i = 0; i < app->row_tables; ++i) free(app->rows[i].items), pure_list_free(app->rows[i].list);
    free(app->rows);
    drop_passes(app);
    free(app->arena.bytes);
    free(app->view_arena.bytes);
    free(app->states);
    memset(app, 0, sizeof(*app));
}

/* Where one event began inside the burst: a failed event returns there. */
typedef struct { /* (each state's text version and list journal length in the app's marked_texts, marked_lists) */
    size_t heard, outbox; /* stored lists' journal lengths */
    size_t undo, sent_saved; /* (each output's last number in the app's marked_sent) */
    bool opened;
} Mark;

static void mark_event(PureApp *app, Mark *mark) {
    mark->opened = !app->open;
    open_burst(app);
    for (uint32_t i = 0; i < app->module->states; ++i) {
        if (app->texts[i]) app->marked_texts[i] = pure_draft_version(app->texts[i]);
        if (app->lists[i]) app->marked_lists[i] = pure_list_changes(app->lists[i]).size;
    }
    mark->heard = app->heard ? pure_list_changes(app->heard).size : 0;
    mark->outbox = app->outbox ? pure_list_changes(app->outbox).size : 0;
    mark->undo = app->undo.count;
    mark->sent_saved = app->sent_saved;
    memcpy(app->marked_sent, app->last_sent, app->module->inputs * sizeof(*app->marked_sent));
}

static void undo_event(PureApp *app, const Mark *mark) {
    for (uint32_t i = 0; i < app->module->states; ++i) {
        if (app->texts[i]) pure_draft_restore(app->texts[i], app->marked_texts[i]);
        if (app->lists[i]) pure_list_forget(app->lists[i], app->marked_lists[i]);
    }
    if (app->heard) pure_list_forget(app->heard, mark->heard);
    if (app->outbox) pure_list_forget(app->outbox, mark->outbox);
    pure_undo_rollback(&app->undo, mark->undo);
    app->sent_saved = mark->sent_saved;
    memcpy(app->last_sent, app->marked_sent, app->module->inputs * sizeof(*app->last_sent));
    if (mark->opened) close_burst(app, true); /* nothing happened: release the holds */
}

/* Where source's item is in the heard list, or would go; found says which. */
static size_t heard_at(const PureApp *app, const char *source, bool *found) {
    *found = false;
    return app->heard ? pure_list_seek(app->heard, 0, (PureCell){.text = (unsigned char *)source, .size = strlen(source)}, found) : 0;
}

uint64_t pure_app_heard(const PureApp *app, const char *source) {
    bool found;
    size_t at = heard_at(app, source, &found);
    return found ? (uint64_t)pure_list_get(app->heard, at, 1).num : 0;
}

/* Hear seq from source: a change of the heard list, undone with the burst. */
static PureResult hear(PureApp *app, const char *source, uint64_t seq) {
    bool found;
    size_t at = heard_at(app, source, &found);
    if (found) return pure_list_set(app->heard, at, 1, (PureCell){.num = (double)seq}, &app->undo);
    PureCell item[2] = {{.text = (unsigned char *)source, .size = strlen(source)}, {.num = (double)seq}};
    return pure_list_insert(app->heard, at, item, &app->undo);
}

/* The output's saved sends through seq leave the outbox: their receiver
 * stored them, or answered them (in the answer's event). */
static PureResult let_sends_go(PureApp *app, uint32_t output, uint64_t through) {
    if (!app->outbox) return PURE_OK;
    const char *name = app->module->input_names[output];
    size_t length = strlen(name);
    for (size_t i = 0; i < app->sent_saved;) {
        PureCell of = pure_list_get(app->outbox, i, 0);
        if (of.size != length || memcmp(of.text, name, length)) { ++i; continue; }
        if ((uint64_t)pure_list_get(app->outbox, i, 1).num > through) break;
        PureResult removed = pure_list_remove(app->outbox, i, &app->undo);
        if (removed != PURE_OK) return removed;
        --app->sent_saved;
        ++app->sends_gone;
        app->links_changed = true;
    }
    return PURE_OK;
}

static PureResult keep_reply(PureApp *app);

/* Every list of a message's rows emptied: an event's rows are its own. */
/* An event's end (input's): the registers given back their own, and the event's lists it may have filled emptied
 * (event_clears: its handlers' and its message's rows; no other event's list holds an item). */
static void clear_rows(PureApp *app, uint32_t input) {
    const PureModule *m = app->module;
    unbind(app);
    const uint64_t *clears = m->event_clears + (size_t)input * m->state_words;
    for (uint32_t w = 0; w < m->state_words; ++w)
        for (uint64_t left = clears[w]; left; left &= left - 1) pure_list_clear(app->lists[w * 64 + (uint32_t)__builtin_ctzll(left)]);
}

void pure_app_memory(PureApp *app, size_t bytes) { app->memory = bytes; }

void pure_app_work(PureApp *app, uint64_t instructions) {
    app->work_budget = instructions;
    for (unsigned i = 0; i < app->row_tables; ++i) app->rows[i].changes = UINT64_MAX; /* a filter refused before may pass */
}

PureFault pure_app_view_fault(const PureApp *app) { return app->view_fault; }

size_t pure_app_state_bytes(const PureApp *app) {
    size_t bytes = (app->outbox ? pure_list_size(app->outbox) : 0) + (app->heard ? pure_list_size(app->heard) : 0);
    for (uint32_t i = 0; i < app->module->states; ++i)
        if (app->module->state_counted[i])
            bytes += app->lists[i] ? pure_list_size(app->lists[i]) : app->texts[i] ? pure_draft_size(app->texts[i]) : 0;
    return bytes;
}

/* One event: every handler for it runs in module order against pending
 * states, all or nothing. A key, text or message event saves before it
 * returns unless saving is deferred; a frame leaves that to the next event
 * (frames come 60 times a second). With a memory budget, an event that leaves the states over it, and
 * holding more than before, fails. */
static PureResult dispatch(PureApp *app, uint32_t event, Event *ev) {
    const PureModule *m = app->module;
    PureValue *pending = app->pending, locals[PURE_MODULE_LOCALS] = {{0}};
    ev->locals = locals;
    size_t held = app->memory ? pure_app_state_bytes(app) : 0;
    /* What the event may add as it runs, whatever it gives back later: the
     * budget (which what it leaves is held to below), and in all what it
     * holds for it the default room, or the budget when that is larger. */
    app->grown = app->spent = 0;
    app->room = app->memory > PURE_GROWTH_DEFAULT ? app->memory : PURE_GROWTH_DEFAULT;
    for (unsigned i = 0; m->held && i < PURE_MODULE_HELD; ++i) app->held[i].size = 0, app->held[i].by_ref = false;
    if (app->nested.count && !ev->nested) unbind(app), drop_nested(app); /* a view's body bound some on the app itself */
    Mark mark;
    mark_event(app, &mark);
    memcpy(pending, app->values, m->states * sizeof(*pending));
    app->fault = PURE_FAULT_NONE;
    app->fault_pc = UINT32_MAX;
    app->work = 0;
    PureResult result = PURE_OK;
    for (uint32_t i = 0; i < m->handlers && result == PURE_OK; ++i)
        if (m->handler[i].event == event)
            result = charge(app, m->handler[i].code.length) ? run(m, pending, m->handler[i].code, ev, NULL)
                                                            : fault(app, PURE_FAULT_WORK, m->handler[i].code.start);
    /* Releasing texts may reclaim earlier text versions: keep a text's reply by copying it first. */
    if (result == PURE_OK && app->has_reply && app->reply.source == FROM_TEXT) result = keep_reply(app);
    /* A delivery's number is heard with its changes, and an answer's sends go with it. */
    if (result == PURE_OK && app->delivering.pending) result = hear(app, app->delivering.source, app->delivering.seq);
    if (result == PURE_OK && app->delivering.pending && app->delivering.release)
        result = let_sends_go(app, app->delivering.release - 1, app->delivering.seq);
    if (result == PURE_OK && app->memory) {
        size_t holds = pure_app_state_bytes(app);
        if (holds > app->memory && holds > held) result = fault(app, PURE_FAULT_MEMORY, UINT32_MAX);
    }
    if (result != PURE_OK) {
        undo_event(app, &mark);
        clear_rows(app, event);
        return result;
    }
    clear_rows(app, event);
    /* No run of a list is open between events: one searched often since it
     * last changed is packed now (docs/design/022): of those the event names. */
    const uint64_t *settles = m->event_settles + (size_t)event * m->state_words;
    for (uint32_t w = 0; w < m->state_words; ++w)
        for (uint64_t left = settles[w]; left; left &= left - 1) {
            PureList *list = app->lists[w * 64 + (uint32_t)__builtin_ctzll(left)];
            if (list) (void)pure_list_settle(list);
        }
    memcpy(app->values, pending, m->states * sizeof(*pending));
    ++app->events;
    ++app->changes;
    app->delivering.pending = false;
    return app->deferred ? PURE_OK : pure_app_save(app);
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

/* Where each variable field of a payload of shape is: where placed says
 * (start and size in the payload), or, without it, one after another after
 * the prefix. A length past the payload or a bool that is not 0 or 1 is
 * LIMIT, a text that is not UTF-8 ENCODING, a number that is not finite
 * NON_FINITE (else NONE). */
static PureFault place_fields(const PureModule *m, const PureInputShape *shape, PureSpan payload, const uint32_t *placed,
                              uint32_t spans[][2]) {
    if (payload.size < shape->prefix) return PURE_FAULT_LIMIT;
    size_t at = shape->prefix;
    for (uint32_t f = 0; f < shape->field_count; ++f) {
        const PureField *field = &m->field[shape->field_start + f];
        if (field->type == PURE_FIELD_NUMBER && !isfinite(read_double(payload.data + field->offset)))
            return PURE_FAULT_NON_FINITE;
        if (field->type == PURE_FIELD_BOOL && payload.data[field->offset] > 1) return PURE_FAULT_LIMIT;
        if (field->type != PURE_FIELD_BYTES && field->type != PURE_FIELD_TEXT && field->type != PURE_FIELD_ROWS) continue;
        size_t start = placed ? placed[2 * f] : at;
        if (start > payload.size) return PURE_FAULT_LIMIT;
        size_t size = placed ? placed[2 * f + 1] : field->offset == PURE_FIELD_REST ? payload.size - at :
                      read32(payload.data + field->offset);
        if (size > payload.size - start) return PURE_FAULT_LIMIT;
        if (field->type == PURE_FIELD_TEXT && !pure_utf8_valid((PureSpan){size ? payload.data + start : NULL, size}))
            return PURE_FAULT_ENCODING;
        spans[f][0] = (uint32_t)start;
        spans[f][1] = (uint32_t)size;
        at = start + size;
    }
    return PURE_FAULT_NONE;
}

int pure_module_arguments(const PureModule *m, uint32_t view) {
    return view < m->views && m->view_arguments[view] ? (int)m->view_arguments[view] - 1 : -1;
}

PureResult pure_app_arguments(PureApp *app, uint32_t view, PureSpan payload, const uint32_t *placed) {
    const PureModule *m = app->module;
    int input = pure_module_arguments(m, view);
    if (input < 0 || (payload.size && !payload.data)) return PURE_INVALID;
    app->argued = 0;
    ++app->arguments_given; /* rows a filter kept are for other arguments */
    PureFault wrong = place_fields(m, &m->input[input], payload, placed, app->argument_spans);
    if (wrong != PURE_FAULT_NONE) return wrong == PURE_FAULT_LIMIT ? PURE_LIMIT : PURE_INVALID;
    app->arguments = payload;
    app->argued = (unsigned)input + 1;
    return PURE_OK;
}

/* A message whose variable fields are where placed says (start and size in
 * the payload), or, without it, one after another after the prefix. */
static PureResult message(PureApp *app, uint32_t input, PureSpan payload, const uint32_t *placed) {
    pure_app_finish(app);
    const PureModule *m = app->module;
    app->fault = PURE_FAULT_NONE;
    app->fault_pc = UINT32_MAX;
    if (input >= m->inputs || m->input[input].kind != PURE_KIND_MESSAGE || (payload.size && !payload.data)) return PURE_INVALID;
    const PureInputShape *shape = &m->input[input];
    uint32_t spans[PURE_MESSAGE_FIELDS][2];
    PureFault wrong = place_fields(m, shape, payload, placed, spans);
    if (wrong != PURE_FAULT_NONE) return fault(app, wrong, UINT32_MAX);
    /* Its rows fill their lists (emptied when the event ends); rows that are not whole fail it. Rows whose records hold
     * lists (027's 4a) read those into the event's lists as a kept list's cell is read, in the event's lists begun anew. */
    bool nested = false;
    for (uint32_t f = 0; f < shape->field_count; ++f) {
        const PureField *field = &m->field[shape->field_start + f];
        if (field->type != PURE_FIELD_ROWS) continue;
        uint32_t record = m->state_ref[field->rows];
        const unsigned char *rows = payload.data + spans[f][0];
        PureResult filled;
        if (m->record_nested[record]) {
            if (!nested && app->nested.count) unbind(app), drop_nested(app);
            nested = true;
            bool made, bad = false;
            value_take(app, record, rows, spans[f][1], app->lists[field->rows], &made, &bad);
            filled = made ? PURE_OK : bad ? PURE_STORE_ERROR : PURE_ALLOCATION;
        } else /* rows of one field read where the payload has them (030's step 1), else read in */
            filled = pure_list_borrow(app->lists[field->rows], rows, spans[f][1]) ? PURE_OK
                                                                                  : pure_list_read(app->lists[field->rows], rows, spans[f][1]);
        if (filled == PURE_OK) continue;
        clear_rows(app, input);
        if (nested) drop_nested(app);
        pure_app_finish(app);
        return filled == PURE_ALLOCATION ? filled : fault(app, PURE_FAULT_LIMIT, UINT32_MAX);
    }
    app->payload = payload;
    Event ev = {.app = app, .shape = shape, .arena = &app->arena, .nested = nested, .spans = spans};
    PureResult result = dispatch(app, input, &ev);
    if (result != PURE_OK) pure_app_finish(app);
    return result;
}

PureResult pure_app_message(PureApp *app, uint32_t input, PureSpan payload) { return message(app, input, payload, NULL); }

PureResult pure_app_message_fields(PureApp *app, uint32_t input, PureSpan payload, const uint32_t *placed) {
    return message(app, input, payload, placed);
}

/* The outbox's item at: its output (the module's, by name), number and payload. */
bool pure_app_sent(const PureApp *app, size_t at, uint32_t *output, uint64_t *seq, PureSpan *payload) {
    PureCell name = pure_list_get(app->outbox, at, 0), bytes = pure_list_get(app->outbox, at, 2);
    const PureModule *m = app->module;
    uint32_t i = 0;
    while (i < m->inputs && (strlen(m->input_names[i]) != name.size || memcmp(m->input_names[i], name.text, name.size))) ++i;
    *output = i;
    *seq = (uint64_t)pure_list_get(app->outbox, at, 1).num;
    *payload = (PureSpan){bytes.text, bytes.size};
    return i < m->inputs;
}

void pure_app_outbox(const PureApp *app, PureSentVisit visit, void *context) {
    for (size_t i = 0; i < app->sent_saved; ++i) {
        uint32_t output;
        uint64_t seq;
        PureSpan payload;
        if (pure_app_sent(app, i, &output, &seq, &payload) && !visit(context, output, seq, payload)) return;
    }
}

/* An output's sends up to through go at once (for the host), and from the
 * store with the next save; a failed save brings them back. Its sends are
 * in order among the others: the look stops at its first one kept. */
void pure_app_trim(PureApp *app, uint32_t output, uint64_t through) {
    if (!app->outbox || output >= app->module->inputs) return;
    open_burst(app);
    let_sends_go(app, output, through);
}

/* A delivery, or an answer to the sends of output (release: its number + 1). */
static PureResult deliver(PureApp *app, uint32_t input, uint32_t release, const char *source, uint64_t seq,
                          PureSpan payload, const uint32_t *placed) {
    const PureModule *m = app->module;
    size_t length = source ? strlen(source) : 0;
    app->duplicate = false;
    /* Kept states without a directory would not keep the numbers heard. */
    if (input >= m->inputs || m->input[input].kind != PURE_KIND_MESSAGE || !length ||
        length > PURE_SOURCE || !seq || seq > PURE_SEQ_MOST || !app->heard || (pure_save_enabled(app) && !app->log))
        return PURE_INVALID;
    for (size_t i = 0; i < length; ++i)
        if (source[i] <= ' ' || source[i] > '~') return PURE_INVALID;
    if (seq <= pure_app_heard(app, source)) {
        pure_app_finish(app);
        app->duplicate = true;
        return PURE_OK;
    }
    app->delivering.source = source;
    app->delivering.seq = seq;
    app->delivering.release = release;
    app->delivering.pending = true;
    PureResult result = message(app, input, payload, placed);
    if (!app->delivering.pending) return result;
    /* Not applied. Refused by the app (a fault), the message was delivered
     * all the same: its number is heard, saved like a key event's changes.
     * Refused for memory it was not: its sender sends it again. */
    app->delivering.pending = false;
    if (app->fault == PURE_FAULT_NONE || app->fault == PURE_FAULT_MEMORY) return result;
    Mark mark;
    mark_event(app, &mark);
    PureResult heard = hear(app, source, seq);
    if (heard == PURE_OK && release) heard = let_sends_go(app, release - 1, seq);
    if (heard != PURE_OK) {
        undo_event(app, &mark);
        return heard;
    }
    PureResult saved = app->deferred ? PURE_OK : pure_app_save(app);
    return saved == PURE_OK ? result : saved;
}

PureResult pure_app_deliver(PureApp *app, uint32_t input, const char *source, uint64_t seq, PureSpan payload,
                            const uint32_t *placed) {
    return deliver(app, input, 0, source, seq, payload, placed);
}

PureResult pure_app_answer(PureApp *app, uint32_t input, uint32_t output, const char *source, uint64_t seq,
                           PureSpan payload, const uint32_t *placed) {
    if (output >= app->module->inputs || app->module->input[output].kind != PURE_KIND_OUTPUT) return PURE_INVALID;
    return deliver(app, input, output + 1, source, seq, payload, placed);
}

PureResult pure_app_reply(const PureApp *app, bool (*visit)(void *, PureSpan), void *context) {
    if (!app->has_reply) return PURE_OK;
    return visit_value(app, &app->arena, app->reply, visit, context);
}

/* A view that fails reports it by its result; the fault stays the last event's. */
/* The arguments a view reads, set in its event; false when it takes some and they were not given. */
static bool argue(const PureApp *app, uint32_t view, Event *ev) {
    unsigned given = app->module->view_arguments[view];
    if (!given) return true;
    if (app->argued != given) return false;
    ev->shape = &app->module->input[given - 1];
    ev->spans = (uint32_t (*)[2])app->argument_spans;
    ev->arguments = true;
    return true;
}

/* The event's lists a view's body changes, empty: a reader's own (its
 * states are a snapshot's, which other readers read), the app's otherwise
 * (empty between events). */
static bool own_lists(PureApp *app, const uint64_t *lists) {
    const PureModule *m = app->module;
    for (uint32_t w = 0; app->reading && w < m->state_words; ++w)
    for (uint64_t left = lists[w]; left; left &= left - 1) {
        unsigned i = w * 64 + (unsigned)__builtin_ctzll(left);
        if (!app->own[i]) {
            if (!(app->own[i] = new_list(m, m->state_ref[i], false))) return false;
            (void)pure_list_keyed(app->own[i], m->state_key[i] ? (int)m->state_key[i] - 1 : -1);
            if (m->state_key[i]) (void)pure_list_hashed(app->own[i]);
            (void)pure_list_arrays(app->own[i]); /* in arrays, not a tree, while it can (021) */
        }
        app->lists[i] = app->own[i];
    }
    return true;
}

static PureResult evaluate(PureApp *app, uint32_t view, size_t row, Slot *out) {
    const PureModule *m = app->module;
    /* The states as they are: a view's code changes none (the verifier holds it to that). */
    PureValue *values = app->values, locals[PURE_MODULE_LOCALS];
    app->view_arena.used = 0;
    Event ev = {.app = app, .arena = &app->view_arena, .row = row};
    if (!argue(app, view, &ev)) return PURE_INVALID;
    if (app->nested.count) unbind(app), drop_nested(app); /* a kept list's lists another view read in (027's stage 3) */
    if (m->view_body[view]) { /* a body computes: its locals from 0, its lists its own, its additions counted */
        memset(locals, 0, sizeof(locals));
        ev.locals = locals;
        if (!own_lists(app, m->view_lists + (size_t)view * m->state_words)) return PURE_ALLOCATION;
        app->grown = app->spent = 0;
        app->room = app->memory > PURE_GROWTH_DEFAULT ? app->memory : PURE_GROWTH_DEFAULT;
        for (unsigned i = 0; m->held && i < PURE_MODULE_HELD; ++i) app->held[i].size = 0, app->held[i].by_ref = false;
        unbind(app);
        if (app->nested.count) drop_nested(app);
    }
    PureFault fault = app->fault;
    uint32_t fault_pc = app->fault_pc;
    PureBlock code = m->view_code[view];
    app->fault = PURE_FAULT_NONE;
    app->work = 0;
    bool counted = charge(app, code.length);
    PureResult result = counted ? run(m, values, code, &ev, out) : PURE_LIMIT;
    if (result != PURE_OK) app->view_fault = counted ? app->fault : PURE_FAULT_WORK; /* why it has no value */
    app->fault = fault;
    app->fault_pc = fault_pc;
    if (m->view_body[view]) { /* its lists emptied, every word of its set (a view's lists go with its evaluation: no
                                * event's end empties them); only a body has any (a table's columns, a row at a time, none) */
        const uint64_t *lists = m->view_lists + (size_t)view * m->state_words;
        for (uint32_t w = 0; w < m->state_words; ++w)
            for (uint64_t left = lists[w]; left; left &= left - 1)
                pure_list_clear(app->lists[w * 64 + (uint32_t)__builtin_ctzll(left)]);
    }
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

/* A computed list view's list (027's 4e): the one its block leaves, copied into a kept list (its lists in stored form)
 * so its rows and cells outlive the evaluations a host asks for between them; false without room. */
static __attribute__((cold, noinline)) bool computed(PureApp *app, uint32_t view, struct PureRows *rows) {
    const PureModule *m = app->module;
    uint32_t record = m->view_record[view] - 1u;
    const PureRecord *r = &m->record[record];
    pure_list_free(rows->list); /* (another module's view, after a replace, may have been here) */
    if (!(rows->list = new_list(m, record, true))) return false;
    Slot slot;
    PureResult result = evaluate(app, view, 0, &slot);
    rows->changes = app->changes;
    rows->given = app->arguments_given;
    rows->fault = result == PURE_OK ? PURE_FAULT_NONE : app->view_fault;
    if (result != PURE_OK) return result != PURE_ALLOCATION; /* no rows: the fault says why */
    const PureList *made = slot.i64 ? app->nested.list[slot.i64 - 1] : NULL;
    for (size_t at = 0, count = made ? pure_list_count(made) : 0; at < count; ++at) {
        PureCell cells[PURE_RECORD_FIELDS];
        size_t room = 0;
        for (unsigned f = 0; f < r->count; ++f) {
            cells[f] = pure_list_get(made, at, f);
            if (m->record_field_type[r->first + f] == PURE_RECORD_LIST)
                room += value_size(app, nested_record(m, r->first + f), (uint64_t)cells[f].i64);
        }
        app->view_arena.used = 0;
        if (!reserve(&app->view_arena, room)) return false; /* all of it first: no span moves while the next is made */
        for (unsigned f = 0; f < r->count; ++f) {
            if (m->record_field_type[r->first + f] != PURE_RECORD_LIST) continue;
            unsigned char *to = app->view_arena.bytes + app->view_arena.used;
            size_t size = (size_t)(value_put(app, nested_record(m, r->first + f), (uint64_t)cells[f].i64, to) - to);
            app->view_arena.used += size;
            cells[f] = (PureCell){.text = to, .size = size};
        }
        if (pure_list_insert(rows->list, at, cells, NULL) != PURE_OK || !keep_row(rows, at)) return false;
    }
    return true; /* its rows its items, in order (as a filter's are) */
}

/* A list view's rows, as fresh as the app's states: a filtered one's items
 * (the filter runs once per item after each change, an item it fails on
 * left out, not once per cell), or a computed one's list (its block run
 * once after each change). NULL for a list state's view without a filter,
 * or without room. */
static struct PureRows *filtered(PureApp *app, uint32_t table) {
    const PureModule *m = app->module;
    if (!m->view_computed[table] && !m->view_code[table].length) return NULL;
    struct PureRows *rows = NULL;
    for (unsigned i = 0; i < app->row_tables && !rows; ++i) if (app->rows[i].table == table) rows = &app->rows[i];
    if (!rows) {
        struct PureRows *grown = realloc(app->rows, (app->row_tables + 1) * sizeof(*grown));
        if (!grown) return NULL;
        app->rows = grown;
        rows = &app->rows[app->row_tables++];
        *rows = (struct PureRows){.table = table, .changes = UINT64_MAX};
    }
    if (rows->changes == app->changes && (!m->view_arguments[table] || rows->given == app->arguments_given)) return rows;
    rows->count = 0;
    if (m->view_computed[table]) return computed(app, table, rows) ? rows : NULL;
    /* A unit of work of its own, counted as written: the filter for every item. */
    const PureList *list = app->lists[m->view_list[table]];
    app->work = 0;
    rows->fault = charge(app, (uint64_t)m->view_code[table].length * pure_list_count(list)) ? PURE_FAULT_NONE : PURE_FAULT_WORK;
    uint64_t work = app->work;
    if (rows->fault != PURE_FAULT_NONE) {
        rows->changes = app->changes;
        rows->given = app->arguments_given;
        return rows;
    }
    /* A leaf of the list at a time when the filter has lane forms; a leaf
     * where an item would fault goes item by item, leaving that item out. */
    PureRun run;
    if (m->view_plan[table]) {
        double store[PURE_MODULE_STACK][PURE_LIST_RUN], scratch[PURE_LIST_RUN];
        Event ev = {.app = app, .arena = &app->view_arena};
        Memo memo;
        memo.known = 0;
        if (!argue(app, table, &ev)) return NULL;
        size_t from = 0, to = pure_list_count(list);
        if (m->view_range[table]) /* items out of the range fail the filter: they are not read */
            range_bounds(m, app->values, &m->range[m->view_range[table] - 1], list,
                         m->state_key[m->view_list[table]] - 1u, &ev, &from, &to);
        for (bool more = from < to && pure_list_run(list, from, &run); more && run.start < to; more = pure_list_next(list, &run)) {
            Lanes keep;
            if (run_lanes(m, app->values, m->view_code[table], &ev, &run, &keep, store, &memo, NULL)) {
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
    app->work = work; /* the filter's, whichever items ran alone */
    rows->changes = app->changes;
    rows->given = app->arguments_given;
    return rows;
}

bool pure_app_row_item(PureApp *app, uint32_t table, size_t row, size_t *item) {
    const PureModule *m = app->module;
    if (table >= m->views || !list_view(m, table)) return false;
    if (!m->view_code[table].length) { /* (a computed one's has its block, and its rows as a filter's are) */
        *item = row;
        if (row < pure_list_count(app->lists[m->view_list[table]])) return true;
        app->view_fault = PURE_FAULT_NONE;
        return false;
    }
    struct PureRows *rows = filtered(app, table);
    if (rows && row < rows->count) {
        *item = rows->items[row];
        return true;
    }
    app->view_fault = rows ? rows->fault : PURE_FAULT_NONE;
    return false;
}

bool pure_app_item_row(PureApp *app, uint32_t table, size_t item, size_t *row) {
    const PureModule *m = app->module;
    if (table >= m->views || !list_view(m, table)) return false;
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

/* A view's type as it is lent: a bool's value is 0 or 1 (a number's), bytes as a text is. */
static uint8_t lent_type(uint8_t type) {
    return type == PURE_VIEW_BOOL ? PURE_VIEW_NUM : type == PURE_VIEW_BYTES ? PURE_VIEW_TEXT : type;
}

/* A column the load made (027's 4e) at an item: its field as it is, a text's, bytes' or list's (in its stored form)
 * copied into the view's arena. */
static __attribute__((cold, noinline)) PureResult field_of(PureApp *app, uint32_t column, size_t item, Slot *slot) {
    const PureModule *m = app->module;
    uint32_t table = m->view_table[column] - 1u;
    const PureList *items = m->view_computed[table] ? filtered(app, table)->list : app->lists[m->view_list[table]];
    PureCell cell = pure_list_get(items, item, m->view_field[column] - 1u);
    app->view_fault = PURE_FAULT_NONE;
    app->view_arena.used = 0;
    uint8_t type = m->view_type[column];
    if (type != PURE_VIEW_TEXT && type != PURE_VIEW_BYTES && type != PURE_VIEW_LIST) {
        slot->i64 = cell.i64; /* a number's, a bool's or an i64's bits */
        return PURE_OK;
    }
    if (!reserve(&app->view_arena, cell.size)) return PURE_ALLOCATION;
    if (cell.size) memcpy(app->view_arena.bytes, cell.text, cell.size);
    app->view_arena.used = cell.size;
    slot->bytes = (PureBytes){FROM_ARENA, 0, {0, 0}, 0, cell.size};
    return PURE_OK;
}

/* A view outside a list view (column false), or a column and a row of its list view (inlined in each, as it was before
 * 4e added a caller). */
static inline __attribute__((always_inline)) PureResult view_of(PureApp *app, uint32_t view, bool column, size_t row, uint8_t type,
                                                          Slot *slot) {
    const PureModule *m = app->module;
    if (view >= m->views || lent_type(m->view_type[view]) != type || (m->view_table[view] != 0) != column) {
        app->view_fault = PURE_FAULT_NONE;
        return PURE_INVALID;
    }
    if (column && !pure_app_row_item(app, m->view_table[view] - 1u, row, &row)) return PURE_NOT_FOUND; /* it says why */
    if (m->view_field[view]) return field_of(app, view, row, slot);
    return evaluate(app, view, row, slot); /* and so does this */
}

bool pure_app_view(PureApp *app, uint32_t view, double *out) {
    Slot slot;
    if (view_of(app, view, false, 0, PURE_VIEW_NUM, &slot) != PURE_OK) return false;
    *out = slot.num;
    return true;
}

bool pure_app_view_i64(PureApp *app, uint32_t view, int64_t *out) {
    Slot slot;
    if (view_of(app, view, false, 0, PURE_VIEW_I64, &slot) != PURE_OK) return false;
    *out = slot.i64;
    return true;
}

PureResult pure_app_view_text(PureApp *app, uint32_t view, bool (*visit)(void *, PureSpan), void *context) {
    Slot slot;
    PureResult result = view_of(app, view, false, 0, PURE_VIEW_TEXT, &slot);
    return result == PURE_OK ? visit_value(app, &app->view_arena, slot.bytes, visit, context) : result;
}

size_t pure_app_rows(PureApp *app, uint32_t table) {
    const PureModule *m = app->module;
    if (table >= m->views || !list_view(m, table)) return 0;
    struct PureRows *rows = filtered(app, table);
    app->view_fault = rows ? rows->fault : PURE_FAULT_NONE;
    return rows ? rows->count
                : m->view_computed[table] || m->view_code[table].length ? 0 : pure_list_count(app->lists[m->view_list[table]]);
}

bool pure_app_cell(PureApp *app, uint32_t column, size_t row, double *out) {
    Slot slot;
    if (view_of(app, column, true, row, PURE_VIEW_NUM, &slot) != PURE_OK) return false;
    *out = slot.num;
    return true;
}

bool pure_app_cell_i64(PureApp *app, uint32_t column, size_t row, int64_t *out) {
    Slot slot;
    if (view_of(app, column, true, row, PURE_VIEW_I64, &slot) != PURE_OK) return false;
    *out = slot.i64;
    return true;
}

PureResult pure_app_cell_text(PureApp *app, uint32_t column, size_t row, bool (*visit)(void *, PureSpan), void *context) {
    Slot slot;
    PureResult result = view_of(app, column, true, row, PURE_VIEW_TEXT, &slot);
    return result == PURE_OK ? visit_value(app, &app->view_arena, slot.bytes, visit, context) : result;
}

PureResult pure_app_cell_items(PureApp *app, uint32_t column, size_t row, PureSpan *items) {
    Slot slot;
    PureResult result = view_of(app, column, true, row, PURE_VIEW_LIST, &slot);
    if (result != PURE_OK) return result;
    /* A made column's is a kept list's cell (a list state kept past an event, or a computed view's), in that form
     * already; a map's column leaves one of the evaluation's lists, written in that form in the view's arena. */
    if (app->module->view_field[column]) {
        *items = (PureSpan){app->view_arena.bytes + slot.bytes.offset, (size_t)slot.bytes.length};
        return PURE_OK;
    }
    PureCell field;
    if (value_cell(app, &app->view_arena, app->module->view_record[column] - 1u, (uint64_t)slot.i64, &field) != PURE_OK)
        return PURE_ALLOCATION;
    *items = (PureSpan){field.text, field.size};
    return PURE_OK;
}

bool pure_items_next(const PureModule *m, uint32_t record, PureSpan items, size_t *at, PureCell *cells) {
    if (record >= m->records || *at >= items.size) return false;
    const PureRecord *r = &m->record[record];
    size_t from = *at;
    for (unsigned f = 0; f < r->count; ++f) {
        uint8_t type = m->record_field_type[r->first + f];
        if (pure_record_bytes(type) || type == PURE_RECORD_LIST) {
            if (items.size - from < 4 || read32(items.data + from) > items.size - from - 4) return false;
            cells[f] = (PureCell){.text = (unsigned char *)items.data + from + 4, .size = read32(items.data + from)};
            from += 4 + cells[f].size;
        } else {
            if (items.size - from < 8) return false;
            cells[f] = (PureCell){.i64 = read_i64(items.data + from)};
            from += 8;
        }
    }
    *at = from;
    return true;
}

PureSnapshot *pure_app_snapshot(PureApp *app) { return pure_save_snapshot(app, UINT32_MAX); }

/* What no snapshot reads any more back to the app's lists, to make their
 * next nodes of (a list copies each node it changes once a snapshot shares
 * it: made of these, it asks the system for none). The register keeps the
 * room for the next. */
static void reuse_returned(PureApp *app, Register *all) {
    lock_register(all);
    PureRetired returned = all->returned;
    all->returned = (PureRetired){0};
    unlock_register(all);
    PureList *few[64], **lists = !returned.count ? NULL : app->module->states + 2 <= 64 ? few :
                                 malloc((app->module->states + 2) * sizeof(*lists));
    if (lists) {
        unsigned count = 0;
        for (uint32_t i = 0; i < app->module->states; ++i)
            if (app->lists[i]) lists[count++] = app->lists[i];
        lists[count++] = app->outbox;
        lists[count++] = app->heard;
        pure_retired_reuse(&returned, lists, count);
        if (lists != few) free(lists);
    }
    lock_register(all);
    bool kept = !all->returned.entries; /* none came back meanwhile: the room stays for the next */
    if (kept) all->returned = returned;
    unlock_register(all);
    if (!kept) pure_retired_free(&returned);
}

PureSnapshot *pure_save_snapshot(PureApp *app, uint32_t runtime) {
    if (app->open || app->reading) return NULL;
    const PureModule *m = app->module;
    size_t each = sizeof(PureValue) + sizeof(PureDraft *) + sizeof(PureList *);
    PureSnapshot *made = calloc(1, sizeof(*made) + m->states * each), *last = app->snapshot;
    Register *all = last ? last->all : calloc(1, sizeof(*all));
    if (!made || !all) {
        free(made);
        if (!last) free(all);
        return NULL;
    }
    made->refs = 1;
    made->module = m;
    made->states = m->states;
    made->values = (PureValue *)(made + 1);
    made->texts = (PureDraft **)(made->values + m->states);
    made->lists = (PureList **)(made->texts + m->states);
    made->changes = app->changes;
    made->epoch = all->epoch + 1; /* the app's thread alone makes them */
    memcpy(made->values, app->values, m->states * sizeof(*made->values));
    bool ok = true;
    for (uint32_t i = 0; i < m->states && ok; ++i) {
        /* A stored text is shared as its store holds it; a transient one, never stored, is copied. */
        if (app->texts[i])
            ok = (made->texts[i] = pure_state_transient(m, i) ? text_copy(app->texts[i]) : pure_draft_freeze(app->texts[i])) !=
                 NULL;
        if (!ok || !app->lists[i]) continue;
        if (last && i < last->states && last->lists[i] && pure_list_current(last->lists[i], app->lists[i])) {
            pure_list_hold(last->lists[i]);
            made->lists[i] = last->lists[i];
            pure_list_mark(app->lists[i], made->epoch);
        } else
            ok = (made->lists[i] = pure_list_share(app->lists[i], &app->retired, made->epoch)) != NULL;
    }
    if (ok && runtime >= PURE_LIST_HEARD && runtime < PURE_LIST_HEARD + PURE_LIST_RUNTIME) {
        PureList *list = runtime == PURE_LIST_HEARD ? app->heard : app->outbox;
        ok = (made->runtime[runtime - PURE_LIST_HEARD] = pure_list_share(list, &app->retired, made->epoch)) != NULL;
    }
    if (!ok) { /* never linked: it goes alone */
        if (!last) free(all);
        pure_snapshot_release(made);
        return NULL;
    }
    made->refs = 2; /* the app's and the caller's */
    made->all = all;
    lock_register(all);
    all->epoch = made->epoch;
    __atomic_add_fetch(&all->refs, 1, __ATOMIC_ACQ_REL); /* a snapshot's last release takes it off outside the lock */
    made->older = last;
    if (last) { /* what the app let go while last was its newest goes with it */
        last->newer = made;
        pure_retired_pass(&app->retired, UINT64_MAX, &last->retired, NULL);
    }
    unlock_register(all);
    if (last) pure_snapshot_release(last);
    else pure_retired_free(&app->retired); /* shared only by a snapshot never made */
    app->snapshot = made;
    reuse_returned(app, all);
    return made;
}

/* A reader's own lists go: its next views compute in new ones. */
static void let_own_go(PureApp *app) {
    for (uint32_t i = 0; i < app->laid; ++i) {
        pure_list_free(app->own[i]);
        app->own[i] = app->unbound[i] = NULL;
    }
    app->bound_count = 0;
}

bool pure_app_read(PureApp *reader, const PureSnapshot *snapshot) {
    if (reader->module_serial != snapshot->module->serial) /* laid out for its module (by serial: freed, another may */
        drop_passes(reader), let_own_go(reader);           /* come to its address) */
    if (!reader->states || reader->laid != snapshot->states) { /* its arrays for as many states (027's 4f-2) */
        void *was = reader->states;
        if (!lay_app(reader, snapshot->states)) return false;
        free(was);
    }
    reader->module = snapshot->module;
    reader->module_serial = snapshot->module->serial;
    reader->reading = true;
    reader->changes = snapshot->changes; /* what the row cache is fresh for */
    /* A register a view bound last time holds what it read then, the snapshot before's lists (one gone once it is let
     * go): forgotten, not given back, so no later unbind puts it back over this snapshot's. */
    for (uint32_t k = 0; k < reader->bound_count; ++k) reader->unbound[reader->bound[k]] = NULL;
    reader->bound_count = 0;
    memcpy(reader->values, snapshot->values, snapshot->states * sizeof(*reader->values));
    memcpy(reader->texts, snapshot->texts, snapshot->states * sizeof(*reader->texts));
    memcpy(reader->lists, snapshot->lists, snapshot->states * sizeof(*reader->lists));
    return true;
}

uint8_t pure_record_kind(const PureModule *m, uint32_t field) {
    static const uint8_t kinds[] = {[PURE_RECORD_NUMBER] = PURE_CELL_NUMBER, [PURE_RECORD_TEXT] = PURE_CELL_TEXT,
                                    [PURE_RECORD_BOOL] = PURE_CELL_BOOL, [PURE_RECORD_I64] = PURE_CELL_INTEGER,
                                    [PURE_RECORD_LIST] = PURE_CELL_LIST, [PURE_RECORD_BYTES] = PURE_CELL_BYTES};
    return kinds[m->record_field_type[field]];
}

/* A field's kind in a list (kept: past an event, holding a list's stored form as bytes; else an event's, which holds
 * one of the event's lists' numbers). */
static uint8_t cell_kind(const PureModule *m, uint32_t field, bool kept) {
    uint8_t kind = pure_record_kind(m, field);
    return kind != PURE_CELL_LIST ? kind : kept ? PURE_CELL_BYTES : PURE_CELL_INTEGER;
}

/* Whether a stored kind's cell is bytes in the stored form: a text's, or a list's. */
static bool stored_bytes(uint8_t kind) { return pure_cell_text(kind) || kind == PURE_CELL_LIST; }

void pure_layout_begin(PureLayout *l) {
    l->records = 0, l->room = 4;
    l->record = l->few, l->module_record = l->few_records;
}

bool pure_layout_room(PureLayout *l, unsigned records) {
    if (records <= l->room) return true;
    if (records > PURE_LAYOUT_RECORDS) return false;
    PureLayoutRecord *record = malloc(PURE_LAYOUT_RECORDS * sizeof(*record));
    uint16_t *module_record = malloc(PURE_LAYOUT_RECORDS * sizeof(*module_record));
    if (!record || !module_record) {
        free(record), free(module_record);
        return false;
    }
    memcpy(record, l->record, l->records * sizeof(*record));
    memcpy(module_record, l->module_record, l->records * sizeof(*module_record));
    pure_layout_done(l);
    l->record = record, l->module_record = module_record, l->room = PURE_LAYOUT_RECORDS;
    return true;
}

void pure_layout_done(PureLayout *l) {
    if (l->record != l->few) free(l->record), free(l->module_record);
    l->record = l->few, l->module_record = l->few_records, l->room = 4;
}

PURE_COLD bool pure_layout_of(const PureModule *m, uint32_t record, PureLayout *out) {
    out->records = 1;
    out->module_record[0] = (uint16_t)record;
    for (unsigned at = 0; at < out->records; ++at) { /* breadth first: each record once, numbered as first met */
        const PureRecord *r = &m->record[out->module_record[at]];
        out->record[at].count = r->count;
        for (unsigned f = 0; f < r->count; ++f) {
            uint32_t field = r->first + f;
            out->record[at].kinds[f] = pure_record_kind(m, field);
            snprintf(out->record[at].names[f], PURE_MODULE_NAME, "%s", m->record_field_names[field]);
            out->record[at].items[f] = 0;
            if (m->record_field_type[field] != PURE_RECORD_LIST) continue;
            uint32_t items = nested_record(m, field);
            unsigned k = 0;
            while (k < out->records && out->module_record[k] != items) ++k;
            if (k == out->records && !pure_layout_room(out, out->records + 1)) return false;
            if (k == out->records) out->module_record[out->records++] = (uint16_t)items;
            out->record[at].items[f] = (uint8_t)k;
        }
    }
    return true;
}

PURE_COLD bool pure_layout_same(const PureLayout *a, const PureLayout *b) {
    if (a->records != b->records) return false;
    for (unsigned at = 0; at < a->records; ++at) {
        if (a->record[at].count != b->record[at].count) return false;
        for (unsigned f = 0; f < a->record[at].count; ++f)
            if (a->record[at].kinds[f] != b->record[at].kinds[f] || strcmp(a->record[at].names[f], b->record[at].names[f]) ||
                (a->record[at].kinds[f] == PURE_CELL_LIST && a->record[at].items[f] != b->record[at].items[f]))
                return false;
    }
    return true;
}

/* A number, a bool and an i64 carry over each other (when the value fits); texts to texts and bytes, bytes to bytes, lists
 * to lists. */
static bool kinds_carry(uint8_t from, uint8_t to) {
    return from == to || (from == PURE_CELL_TEXT && to == PURE_CELL_BYTES) || (!stored_bytes(from) && !stored_bytes(to));
}

/* Bytes made for a list value's stored form as it moves (a growing buffer). */
typedef struct { unsigned char *bytes; size_t used, capacity; bool full; } Grown;
static unsigned char *grown_room(Grown *g, size_t more) {
    if (g->full) return NULL;
    if (more > g->capacity - g->used) {
        size_t capacity = g->capacity ? g->capacity : 256;
        while (capacity - g->used < more) capacity *= 2;
        unsigned char *bytes = realloc(g->bytes, capacity);
        if (!bytes) { g->full = true; return NULL; }
        g->bytes = bytes, g->capacity = capacity;
    }
    g->used += more;
    return g->bytes + g->used - more;
}

/* For field f of record to's (in m), the field of from's record that gives it (the same name, a kind that carries
 * over), or -1. */
static int source_field(const PureLayout *from, unsigned at, const PureModule *m, uint32_t field) {
    for (unsigned k = 0; k < from->record[at].count; ++k)
        if (!strcmp(from->record[at].names[k], m->record_field_names[field]) &&
            kinds_carry(from->record[at].kinds[k], pure_record_kind(m, field)))
            return (int)k;
    return -1;
}

/* A field's value, the source's or its default, as the stored form writes it (a list's moved itself). */
static bool moved_cell(const PureLayout *from, unsigned at, int k, PureCell given, const PureModule *m, uint32_t field,
                       Grown *out);

/* A list value's stored form (bytes, of from's record at) moved to record's of m by field name, item by item, into
 * out; false on bytes not of that form (or out of memory: out->full). */
static bool value_migrate(const PureLayout *from, unsigned at, const PureModule *m, uint32_t record, const unsigned char *bytes,
                          size_t size, Grown *out) {
    const PureRecord *r = &m->record[record];
    for (size_t k = 0; k < size;) {
        PureCell cells[PURE_RECORD_FIELDS];
        for (unsigned f = 0; f < from->record[at].count; ++f) { /* the item as from's record has it */
            if (stored_bytes(from->record[at].kinds[f])) {
                if (size - k < 4 || read32(bytes + k) > size - k - 4) return false;
                cells[f] = (PureCell){.text = (unsigned char *)bytes + k + 4, .size = read32(bytes + k)};
                k += 4 + cells[f].size;
            } else {
                if (size - k < 8) return false;
                cells[f] = (PureCell){.i64 = read_i64(bytes + k)};
                k += 8;
            }
        }
        for (unsigned f = 0; f < r->count; ++f) {
            int source = source_field(from, at, m, r->first + f);
            if (!moved_cell(from, at, source, source >= 0 ? cells[source] : (PureCell){0}, m, r->first + f, out)) return false;
        }
    }
    return !out->full;
}

static bool moved_cell(const PureLayout *from, unsigned at, int k, PureCell given, const PureModule *m, uint32_t field,
                       Grown *out) {
    uint8_t kind = pure_record_kind(m, field);
    if (kind == PURE_CELL_LIST) { /* the list moved item by item, or none */
        unsigned char *length = grown_room(out, 4);
        size_t was = out->used;
        if (!length) return false;
        if (k >= 0 && !value_migrate(from, from->record[at].items[k], m, nested_record(m, field), given.text, given.size, out))
            return false;
        write32(out->bytes + was - 4, (uint32_t)(out->used - was));
        return true;
    }
    if (k < 0 || !pure_cell_as(from->record[at].kinds[k], kind, &given)) { /* its default */
        uint32_t constant = (uint32_t)m->record_field_default[field].num;
        given = kind == PURE_CELL_TEXT ? (PureCell){.text = (unsigned char *)m->constant_data + m->constant_offset[constant],
                                                    .size = m->constant_length[constant]}
                                       : (PureCell){.i64 = m->record_field_default[field].i64};
    }
    unsigned char *put = grown_room(out, pure_cell_text(kind) ? 4 + given.size : 8);
    if (!put) return false;
    if (!pure_cell_text(kind)) write_i64(put, given.i64);
    else {
        write32(put, (uint32_t)given.size);
        if (given.size) memcpy(put + 4, given.text, given.size);
    }
    return true;
}

/* A list with record's fields: each takes the field of `from` with the same
 * name and a kind it carries over from, or its default; a list field's value
 * moves item by item (027's stage 3). */
PURE_COLD PureResult pure_list_migrate(PureList **out, const PureList *from, const PureLayout *from_layout, const PureModule *m,
                                       uint32_t record, bool kept) {
    const PureRecord *r = &m->record[record];
    int source[PURE_RECORD_FIELDS];
    for (unsigned f = 0; f < r->count; ++f) source[f] = source_field(from_layout, 0, m, r->first + f);
    if (!(*out = new_list(m, record, kept))) return PURE_ALLOCATION;
    PureResult result = PURE_OK;
    Grown moved[PURE_RECORD_FIELDS] = {{0}};
    for (size_t at = 0; result == PURE_OK && at < pure_list_count(from); ++at) {
        PureCell cells[PURE_RECORD_FIELDS];
        for (unsigned f = 0; f < r->count; ++f) {
            uint32_t field = r->first + f, constant = (uint32_t)m->record_field_default[field].num;
            PureCell given = source[f] >= 0 ? pure_list_get(from, at, (unsigned)source[f]) : (PureCell){0};
            if (m->record_field_type[field] == PURE_RECORD_LIST && kept) { /* its items moved, or none */
                moved[f].used = 0;
                if (source[f] >= 0 && !value_migrate(from_layout, from_layout->record[0].items[source[f]], m, nested_record(m, field),
                                                     given.text, given.size, &moved[f]))
                    result = moved[f].full ? PURE_ALLOCATION : PURE_STORE_ERROR;
                cells[f] = (PureCell){.text = moved[f].bytes, .size = moved[f].used};
            } else if (m->record_field_type[field] == PURE_RECORD_LIST) cells[f] = (PureCell){.i64 = 0}; /* an event's: none */
            else if (source[f] >= 0 && pure_cell_as(from_layout->record[0].kinds[source[f]], pure_record_kind(m, field), &given))
                cells[f] = given;
            else if (pure_record_bytes(m->record_field_type[field]))
                cells[f] = (PureCell){.text = (unsigned char *)m->constant_data + m->constant_offset[constant],
                                      .size = m->constant_length[constant]};
            else cells[f] = (PureCell){.i64 = m->record_field_default[field].i64}; /* a number's, a bool's or an i64's bits */
        }
        if (result == PURE_OK) result = pure_list_insert(*out, at, cells, NULL);
    }
    for (unsigned f = 0; f < r->count; ++f) free(moved[f].bytes);
    if (result != PURE_OK) { pure_list_free(*out); *out = NULL; }
    return result;
}

/* A kept list keeps its items; with the same record (laid out the same) it is the same list. */
PURE_COLD static PureResult keep_list(PureList **out, PureList *from, const PureModule *old, uint32_t old_record,
                            const PureModule *m, uint32_t record, bool kept) {
    PureLayout was, now;
    pure_layout_begin(&was), pure_layout_begin(&now);
    PureResult result = !pure_layout_of(old, old_record, &was) || !pure_layout_of(m, record, &now) ? PURE_ALLOCATION :
                        pure_layout_same(&was, &now) ? (*out = from, PURE_OK) : pure_list_migrate(out, from, &was, m, record, kept);
    pure_layout_done(&was), pure_layout_done(&now);
    return result;
}

/* Open a module in an app. States keep their values by name and kind (texts
 * their content, cursors their position in it, lists their items, moved by
 * field name when the record changed); every other state starts from its
 * stored value, or its initial one. A list whose record changed is stored
 * again, and the next event or save publishes the module's states; a state
 * the module drops stays stored as it was. Starting an app opens its module
 * over none. */
/* Whether module's output `to` lays out its fields as old's `from` does:
 * the same names and types, in order (what a send's bytes mean). */
static bool same_fields(const PureModule *old, uint32_t from, const PureModule *module, uint32_t to) {
    if (module->input[to].field_count != old->input[from].field_count) return false;
    for (uint32_t f = 0; f < old->input[from].field_count; ++f) {
        const PureField *now = &module->field[module->input[to].field_start + f],
                        *was = &old->field[old->input[from].field_start + f];
        if (now->type != was->type || strcmp(now->name, was->name)) return false;
    }
    return true;
}

/* Outputs of old that module has by name keep their sends and numbers;
 * the sends of one whose fields changed are recast to its new fields by
 * name (pure_send_recast), into a new outbox (*recast; NULL when none is).
 * One it lacks refuses the module while sends are left on it (CONFLICT,
 * app->why says), and otherwise keeps its number for later. *changed: an
 * output of module is laid out otherwise than old's (or old has none of
 * that name), so the outbox's image, which names them, is written again. */
/* A block of each input's numbers for inputs of them, zeroed, its arrays to be laid out in app (by_input_of); NULL without
 * room (027's 4f-3a). */
static uint64_t *by_input_block(uint32_t inputs) { return calloc(3 * (size_t)inputs + 1, sizeof(uint64_t)); }
static void by_input_of(PureApp *app, uint64_t *block, uint32_t inputs) {
    free(app->by_input);
    app->by_input = block;
    app->last_sent = block, app->saved_sent = block + inputs, app->marked_sent = block + 2 * (size_t)inputs;
}

/* An output's sends and numbers carried to module by name; applied (apply), its numbers in block (by_input_block's,
 * for module's inputs, which it then holds). */
PURE_COLD static PureResult remap_outputs(PureApp *app, const PureModule *old, const PureModule *module, bool apply,
                                          PureList **recast, bool *changed, uint64_t *block) {
    int *map = malloc((old->inputs + 1) * sizeof(*map));
    bool *differs = calloc(old->inputs + 1, sizeof(*differs)), recasting = false;
    if (!map || !differs) {
        free(map), free(differs);
        if (apply) by_input_of(app, block, module->inputs); /* (the numbers start again) */
        return PURE_ALLOCATION;
    }
    for (uint32_t i = 0; i < old->inputs; ++i) {
        map[i] = -1;
        if (old->input[i].kind != PURE_KIND_OUTPUT) continue;
        int to = pure_module_input(module, old->input_names[i]);
        if (to < 0 || module->input[to].kind != PURE_KIND_OUTPUT) continue;
        map[i] = to;
        differs[i] = !same_fields(old, i, module, (uint32_t)to);
    }
    if (!apply) {
        *recast = NULL;
        *changed = false;
        for (uint32_t k = 0; k < module->inputs && !*changed; ++k) {
            if (module->input[k].kind != PURE_KIND_OUTPUT) continue;
            int from = pure_module_input(old, module->input_names[k]);
            *changed = from < 0 || old->input[from].kind != PURE_KIND_OUTPUT || differs[from];
        }
        size_t count = app->outbox ? pure_list_count(app->outbox) : 0;
        for (size_t s = 0; s < count; ++s) {
            PureCell cell = pure_list_get(app->outbox, s, 0);
            char name[PURE_MODULE_NAME] = "";
            if (cell.size < PURE_MODULE_NAME) memcpy(name, cell.text, cell.size);
            int i = pure_module_input(old, name);
            if (i < 0 || map[i] < 0) {
                snprintf(app->why, sizeof(app->why), "sends wait on the output '%s', which this module does not have",
                         name);
                free(map), free(differs);
                return PURE_CONFLICT;
            }
            recasting = recasting || differs[i];
        }
        if (!recasting) {
            free(map), free(differs);
            return PURE_OK;
        }
        PureList *made = pure_runtime_list_new(PURE_LIST_OUTBOX);
        PureResult result = made ? PURE_OK : PURE_ALLOCATION;
        for (size_t s = 0; result == PURE_OK && s < count; ++s) {
            PureCell item[3] = {pure_list_get(app->outbox, s, 0), pure_list_get(app->outbox, s, 1),
                                pure_list_get(app->outbox, s, 2)};
            char name[PURE_MODULE_NAME] = "";
            memcpy(name, item[0].text, item[0].size);
            int i = pure_module_input(old, name);
            unsigned char *bytes = NULL;
            size_t size = 0;
            if (differs[i]) {
                const PureInputShape *was = &old->input[i], *now = &module->input[map[i]];
                result = pure_send_recast(&old->field[was->field_start], was->field_count, &module->field[now->field_start],
                                          now->field_count, (PureSpan){item[2].text, item[2].size}, &bytes, &size);
                item[2] = (PureCell){.text = bytes, .size = size};
            }
            if (result == PURE_OK) result = pure_list_insert(made, s, item, NULL);
            free(bytes);
        }
        free(map), free(differs);
        if (result != PURE_OK) {
            pure_list_free(made);
            return result;
        }
        *recast = made;
        return PURE_OK;
    }
    uint64_t *last = block, *saved = block + module->inputs;
    unsigned outputs = 0; /* the store names them and those carried by a byte's count: 255 in all at most */
    for (uint32_t k = 0; k < module->inputs; ++k) outputs += module->input[k].kind == PURE_KIND_OUTPUT;
    for (uint32_t i = 0; i < old->inputs; ++i) {
        if (old->input[i].kind != PURE_KIND_OUTPUT || !app->last_sent[i]) continue;
        if (map[i] >= 0) {
            last[map[i]] = app->last_sent[i];
            saved[map[i]] = app->saved_sent[i];
            continue;
        }
        struct PureCarried *grown = outputs + app->carried_count < UINT8_MAX ?
                                    realloc(app->carried, (app->carried_count + 1) * sizeof(*grown)) : NULL;
        if (!grown) continue; /* its numbers would start again: kept only while memory and the store's count allow */
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
    free(map), free(differs);
    by_input_of(app, block, module->inputs);
    return PURE_OK;
}

/* The arrays of each state next laid out given to app, its old ones given back (027's 4f-2). */
static void take_states(PureApp *app, const PureApp *next) {
    free(app->states);
    app->states = next->states, app->laid = next->laid;
    app->values = next->values, app->texts = next->texts, app->lists = next->lists, app->list_drafts = next->list_drafts;
    app->image_seq = next->image_seq, app->logged_since = next->logged_since, app->logged = next->logged;
    app->saved = next->saved, app->durable_values = next->durable_values, app->durable_texts = next->durable_texts;
    app->durable_lists = next->durable_lists, app->own = next->own, app->unbound = next->unbound, app->spare = next->spare;
    app->bound = next->bound, app->bound_count = 0;
    app->pending = next->pending, app->marked_texts = next->marked_texts;
    app->marked_lists = next->marked_lists;
}

PURE_COLD static PureResult open_module(PureApp *app, const PureModule *module) {
    const PureModule *old = app->module;
    bool stored = pure_save_enabled(app);
    app->why[0] = 0;
    if (old && app->checkpoint) return PURE_CONFLICT; /* the host has a checkpoint of the running module out */
    /* The new arrays of each state, laid out apart (next) and given to app once nothing can fail; what is worked out
     * for each state. */
    PureApp next = {0};
    uint32_t states = module->states;
    int *kept = malloc((states + 1) * sizeof(*kept));
    PureSaved *listed = calloc(states + 1, sizeof(*listed));
    bool *rewrite = calloc(states + 1, sizeof(*rewrite));
    size_t **moved = calloc(states + 1, sizeof(*moved));
    uint64_t *numbers = by_input_block(module->inputs); /* each input's numbers: the app's from now on */
    if (!kept || !listed || !rewrite || !moved || !numbers || !lay_app(&next, states)) {
        free(kept), free(listed), free(rewrite), free(moved), free(numbers), free(next.states);
        return PURE_ALLOCATION;
    }
    if (!old) by_input_of(app, numbers, module->inputs), numbers = NULL; /* (the stored numbers are read into them) */
    PureValue *values = next.values;
    PureDraft **texts = next.texts, **list_drafts = next.list_drafts;
    PureList **lists = next.lists;
    PureVersion *saved = next.saved;
    /* Lists are stored where the host names a directory for them (module-save.c). */
    bool lists_stored = stored && app->data.directory && app->data.directory(app->data.context);
    PureResult result = !old && lists_stored ? pure_save_open_log(app) : PURE_OK;
    /* The numbers heard and the outbox belong to the app, whatever its module. */
    PureSaved heard = {0}, sent = {0};
    if (!old && result == PURE_OK)
        result = lists_stored ? pure_save_open_runtime(app, PURE_LIST_HEARD, &app->heard, &heard) :
                 (app->heard = pure_runtime_list_new(PURE_LIST_HEARD)) ? PURE_OK : PURE_ALLOCATION;
    if (!old && result == PURE_OK)
        result = lists_stored ? pure_save_open_runtime(app, PURE_LIST_OUTBOX, &app->outbox, &sent) :
                 (app->outbox = pure_runtime_list_new(PURE_LIST_OUTBOX)) ? PURE_OK : PURE_ALLOCATION;
    for (uint32_t i = 0; i < module->states; ++i) {
        int previous = old ? pure_module_state(old, module->state_names[i]) : -1;
        /* A number, a bool and an i64 carry over each other when the value fits (pure_cell_as), else start anew. */
        uint8_t was = previous >= 0 ? old->state_type[previous] : 0, now = module->state_type[i];
        bool valued = pure_value_kind(was) != 0xFF && pure_value_kind(now) != 0xFF;
        /* A text and bytes (028) carry over each other when exact: bytes become a text when they are UTF-8, else it is new. */
        bool anew = previous >= 0 && was == PURE_STATE_TEXT && now == PURE_STATE_TEXT &&
                    old->state_flags[previous] & PURE_TEXT_BYTES && !(module->state_flags[i] & PURE_TEXT_BYTES) &&
                    !draft_utf8(app->texts[previous]);
        kept[i] = previous >= 0 && (was == now || valued) && !anew ? previous : -1;
        if (kept[i] >= 0) saved[i] = app->saved[kept[i]];
        /* A transient state is never stored (018): one that was stored is now as a dropped one is. */
        bool transient = pure_state_transient(module, i), was_transient = kept[i] >= 0 && pure_state_transient(old, kept[i]);
        if (module->state_type[i] == PURE_STATE_TEXT) {
            if (kept[i] >= 0 && (transient || !was_transient || !stored)) texts[i] = app->texts[kept[i]];
            else if (result == PURE_OK && kept[i] >= 0 && /* a text stored from now on: its draft in the store holds it */
                     (result = pure_save_open_text(app, module, i, false, &texts[i], &saved[i])) == PURE_OK)
                result = copy_text(texts[i], app->texts[kept[i]], &saved[i]);
            else if (result == PURE_OK)
                result = stored && !transient ? pure_save_open_text(app, module, i, anew, &texts[i], &saved[i]) :
                         new_text(&texts[i], module, module->state_ref[i]);
        } else if (module->state_type[i] == PURE_STATE_LIST) {
            if (result != PURE_OK) continue;
            bool rows = module->state_flags[i] & PURE_LIST_ROWS >> 8; /* rows are never stored */
            bool saving = lists_stored && !rows && !transient;
            if (kept[i] >= 0) {
                result = keep_list(&lists[i], app->lists[kept[i]], old, old->state_ref[kept[i]], module,
                                   module->state_ref[i], !rows);
                list_drafts[i] = app->list_drafts[kept[i]];
                if (!transient && !was_transient)
                    listed[i] = (PureSaved){app->image_seq[kept[i]], app->logged_since[kept[i]], app->logged[kept[i]], false};
                /* Its record changed, or it is stored from now on: its image, in the new fields, holds it before any change is logged. */
                rewrite[i] = result == PURE_OK && saving && (lists[i] != app->lists[kept[i]] || was_transient);
            } else if (saving) {
                result = pure_save_open_list(app, module, i, &lists[i], &list_drafts[i], &listed[i]);
                rewrite[i] = result == PURE_OK && listed[i].rewrite;
            } else if (!(lists[i] = new_list(module, module->state_ref[i], !rows))) result = PURE_ALLOCATION;
        } else if (kept[i] >= 0 && valued) {
            PureCell cell = {.i64 = app->values[kept[i]].i64};
            values[i] = pure_cell_as(pure_value_kind(was), pure_value_kind(now), &cell) ? (PureValue){.i64 = cell.i64}
                                                                                     : module->initial[i];
        } else values[i] = kept[i] >= 0 ? app->values[kept[i]] : module->initial[i];
    }
    /* A keyed list keeps its items in key order: one that was not keyed, or
     * keyed by another field, is put in order and its indices follow their
     * items. Two items with one key refuse the module: nothing is dropped. */
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
        rewrite[i] = lists_stored && !(module->state_flags[i] & PURE_LIST_ROWS >> 8) && !pure_state_transient(module, i);
    }
    /* A cursor or index keeps its position only while its text or list is the
     * one it belonged to. */
    for (uint32_t i = 0; i < module->states; ++i) {
        if (module->state_type[i] != PURE_STATE_CURSOR && module->state_type[i] != PURE_STATE_INDEX) continue;
        uint32_t owner = module->state_ref[i];
        bool same = kept[i] >= 0 && kept[owner] >= 0 && old->state_ref[kept[i]] == (unsigned)kept[owner];
        values[i] = same ? app->values[kept[i]] : (PureValue){0};
    }
    if (result == PURE_OK && stored) result = pure_save_open_values(app, module, values, kept, texts, lists);
    /* Outputs keep their sends and numbers by name. */
    bool heard_moved = false, sent_moved = false;
    if (result == PURE_OK && !old && !stored) app->sent_saved = pure_list_count(app->outbox);
    PureList *recast = NULL; /* the outbox, its sends laid out with module's fields */
    bool outputs_changed = false;
    if (result == PURE_OK)
        result = old ? remap_outputs(app, old, module, false, &recast, &outputs_changed, NULL) :
                 stored ? pure_save_open_links(app, module, &heard_moved, &sent_moved) : PURE_OK;
    heard.rewrite = heard.rewrite || (heard_moved && lists_stored);
    sent.rewrite = sent.rewrite || (sent_moved && lists_stored);
    for (uint32_t i = 0; i < module->states; ++i) {
        uint32_t list = module->state_ref[i];
        if (module->state_type[i] == PURE_STATE_INDEX && moved[list] && values[i].num < (double)pure_list_count(lists[list]))
            values[i].num = (double)moved[list][(size_t)values[i].num];
    }
    for (uint32_t i = 0; i < module->states; ++i) free(moved[i]);
    if (!old) pure_save_close_start(app);
    /* Images last, once nothing else can fail: first of the lists the new
     * module drops whose changes are only logged (the log lets them go),
     * then of the lists that need one. An image written cannot be taken
     * back: a running app that fails here saves nothing more until it starts
     * again, from what the store has. */
    bool imaged = result == PURE_OK;
    for (uint32_t j = 0; result == PURE_OK && old && j < old->states; ++j) {
        bool dropped = old->state_type[j] == PURE_STATE_LIST && app->logged_since[j];
        for (uint32_t i = 0; dropped && i < module->states; ++i) dropped = kept[i] != (int)j || pure_state_transient(module, i);
        if (dropped) result = pure_save_image(app, old, j, app->lists[j]);
    }
    for (uint32_t i = 0; i < module->states && result == PURE_OK; ++i)
        if (rewrite[i] && (result = pure_save_image(app, module, i, lists[i])) == PURE_OK)
            listed[i] = (PureSaved){.image = app->seq};
    if (result == PURE_OK && heard.rewrite &&
        (result = pure_save_runtime_image(app, module, PURE_LIST_HEARD, NULL)) == PURE_OK)
        heard = (PureSaved){.image = app->seq};
    if (result == PURE_OK && sent.rewrite &&
        (result = pure_save_runtime_image(app, module, PURE_LIST_OUTBOX, NULL)) == PURE_OK)
        sent = (PureSaved){.image = app->seq};
    /* A replace that changes how sends are laid out names it in the outbox's image first. */
    bool outbox_imaged = false;
    if (result == PURE_OK && old && lists_stored && (recast || outputs_changed) &&
        (result = pure_save_runtime_image(app, module, PURE_LIST_OUTBOX, recast)) == PURE_OK)
        outbox_imaged = true;
    if (imaged && result != PURE_OK && old) app->uncertain = true;
    if (result != PURE_OK) {
        pure_list_free(recast);
        for (uint32_t i = 0; i < module->states; ++i) {
            if (kept[i] >= 0) {
                if (lists[i] && lists[i] != app->lists[kept[i]]) pure_list_free(lists[i]);
                if (texts[i] && texts[i] != app->texts[kept[i]]) pure_draft_close(texts[i]);
                continue;
            }
            pure_draft_close(texts[i]);
            pure_list_free(lists[i]);
            pure_draft_close(list_drafts[i]);
        }
        free(kept), free(listed), free(rewrite), free(moved), free(numbers), free(next.states);
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
    for (uint32_t i = 0; i < states; ++i) {
        next.image_seq[i] = listed[i].image;
        next.logged_since[i] = listed[i].since;
        next.logged[i] = listed[i].logged;
    }
    if (!old) {
        next.image_seq[states] = heard.image, next.logged_since[states] = heard.since, next.logged[states] = heard.logged;
        next.image_seq[states + 1] = sent.image, next.logged_since[states + 1] = sent.since, next.logged[states + 1] = sent.logged;
        if (lists_stored) pure_list_journal(app->heard, true), pure_list_journal(app->outbox, true);
    } else
        for (unsigned r = 0; r < PURE_LIST_RUNTIME; ++r) { /* the runtime's lists, past the new module's states */
            next.image_seq[states + r] = app->image_seq[app->laid + r];
            next.logged_since[states + r] = app->logged_since[app->laid + r];
            next.logged[states + r] = app->logged[app->laid + r];
        }
    take_states(app, &next);
    free(kept), free(listed), free(rewrite), free(moved);
    pure_save_release(app);
    for (uint32_t i = 0; i < module->states; ++i) {
        /* A stored list's changes are recorded from now on; a transient one's never. */
        if (lists_stored && module->state_type[i] == PURE_STATE_LIST && !(module->state_flags[i] & PURE_LIST_ROWS >> 8))
            pure_list_journal(lists[i], !pure_state_transient(module, i));
        /* Without memory for its keys a keyed list is still searched, only slower; an event's keeps them as a set too (020). */
        if (module->state_type[i] == PURE_STATE_LIST)
            (void)pure_list_keyed(lists[i], module->state_key[i] ? (int)module->state_key[i] - 1 : -1);
        if (module->state_type[i] == PURE_STATE_LIST && module->state_key[i] && module->state_flags[i] & PURE_LIST_ROWS >> 8)
            (void)pure_list_hashed(lists[i]);
        if (module->state_type[i] == PURE_STATE_LIST && module->state_flags[i] & PURE_LIST_ROWS >> 8)
            (void)pure_list_arrays(lists[i]); /* in arrays, not a tree, while it can (021) */
    }
    if (old) remap_outputs(app, old, module, true, NULL, NULL, numbers);
    if (recast) { /* in the place of the outbox, its sends now the new module's */
        pure_list_free(app->outbox);
        app->outbox = recast;
        app->durable_outbox = 0;
        if (lists_stored) pure_list_journal(recast, true);
    }
    if (outbox_imaged) {
        app->image_seq[states + 1] = app->seq;
        app->logged_since[states + 1] = 0;
        app->logged[states + 1] = 0;
    }
    drop_passes(app); /* kept by instruction of the module they were made for */
    app->module = module;
    ++app->changes;
    return PURE_OK;
}

PURE_COLD PureResult pure_app_start(PureApp *app, const PureModule *module, PureData data) {
    *app = (PureApp){.data = data, .fault_pc = UINT32_MAX};
    PureResult result = open_module(app, module);
    if (result == PURE_OK) result = pure_app_save(app);
    if (result != PURE_OK) { /* stopped, saying why when it can */
        char why[sizeof(app->why)];
        memcpy(why, app->why, sizeof(why));
        pure_app_stop(app);
        memcpy(app->why, why, sizeof(why));
    }
    return result;
}

PURE_COLD PureResult pure_app_replace(PureApp *app, const PureModule *module) {
    unbind(app); /* lists are laid out again: none bound, the event's in records let go */
    drop_nested(app);
    drop_spares(app);
    pure_app_finish(app);
    if (app->open) {
        PureResult saved = pure_app_save(app); /* the burst belongs to the module that made it */
        if (saved != PURE_OK) return saved;
    }
    return open_module(app, module);
}
