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

enum { STATE_BYTES = 16, COLLECTION_BYTES = 12, INPUT_BYTES = 16, FIELD_BYTES = 8,
       CONSTANT_BYTES = 8, RECORD_BYTES = 8, RECORD_FIELD_BYTES = 16, VIEW_BYTES = 16,
       HANDLER_BYTES = 12, OP_BYTES = 16 };
/* T_FIELD and T_ITEM stand for the type of the message or record field
 * named, T_KEY for the type of the named keyed list's key, T_LOCAL for what
 * the named local holds, T_STATE for the type of the number, bool or i64
 * state named. */
enum { T_NUM = 1, T_BYTES = 2, T_TEXT = 3, T_CURSOR = 4, T_FIELD = 5, T_INDEX = 6, T_ITEM = 7, T_KEY = 8, T_LOCAL = 9,
       T_BOOL = 10, T_STATE = 11, T_I64 = 12, T_LIST = 13 }; /* T_LIST: a list value, its owner the record of its items */
enum { FROM_PAYLOAD, FROM_DOCUMENT, FROM_COPY, FROM_CONSTANT, FROM_TEXT, FROM_ARENA, FROM_ARGUMENTS, FROM_HELD };
/* Where an opcode may appear. */
/* IN_ARGUMENTS: fields read where nothing is done with them (a view's arguments, a message's fields in a fold). */
/* IN_LOCALS: a handler's locals, read in its walks and the folds in them. */
/* IN_COMPUTE: what a view's body may do too (018): loops, locals, and changes to an event's lists alone. */
enum { IN_VIEW = 1, IN_HANDLER = 2, IN_MESSAGE = 4, IN_KEYED = 8, IN_TEXT = 16, IN_ROW = 32, IN_OUTER = 64, IN_ARGUMENTS = 128,
       IN_LOCALS = 256, IN_COMPUTE = 512 };
enum { READ = IN_VIEW | IN_HANDLER };
/* What an argument names: nothing (it is 0), a state of one type (A_STATE +
 * the type), a list and one of its record's fields, a message field, the
 * collection, a constant, a unit, the column's row, a branch target, a stack
 * depth, a fold, a relation, a keyed list, an output, the row of the column
 * a fold runs in, a number, bool or i64 state (A_VALUE), or the value's type
 * (A_TYPE: 0 a number, 1 a bool, 2 an i64), how I64_BITS computes (A_BITS),
 * what a position is (A_POSITION: 0 a number, 1 an i64), what CONCAT joins
 * (A_JOINED: 0 texts, 1 bytes), how NUM_TEXT writes (A_FORMAT) or the value
 * held by name (A_HELD). */
enum { A_NONE, A_STATE, A_ITEM = A_STATE + PURE_STATE_I64 + 1, A_FIELD, A_COLLECTION, A_CONSTANT, A_UNIT,
       A_ROW, A_TARGET, A_DEPTH, A_FOLD, A_RELATION, A_KEYED, A_OUTPUT, A_OUTER, A_EACH, A_LOCAL, A_VALUE, A_TYPE,
       A_LOOP, A_BITS, A_POSITION, A_JOINED, A_FORMAT, A_HELD };
/* A fold's block is verified as a column that leaves one bool (count, first)
 * or a value and whether to include it (a bool): a number (FOLD_TWO) or an
 * i64 (FOLD_I64). A table's filter is verified as a column that leaves a
 * bool: these stand for a view type. A walk's block (EACH) is verified as
 * its handler's code that may read the item it runs for (WALK). In a view's
 * body a loop's block leaves nothing either (STEPS), and a walk's reads its
 * item (STEPS_WALK); neither changes a state. */
enum { FOLD_ONE = 100, FOLD_TWO, FOLD_I64, FILTER, WALK, STEPS, STEPS_WALK };
/* The text or list an opcode works on: its cursor and index operands belong
 * to it, and so does a cursor or index it pushes. O_ANY: no rule. O_OPERAND:
 * it pushes a position in its operand's text or list. O_ARG: the argument.
 * O_REF: the text or list of the argument's cursor or index state. O_LIST:
 * the list of an item argument. */
enum { O_ANY, O_OPERAND, O_ARG, O_REF, O_LIST };
static const unsigned char magic[8] = {'P','U','R','E','M','O','D','6'};

/* Operand types popped (bottom to top), the type pushed, where allowed, what
 * the argument names and the text or list worked on. */
#define NUMS(pops) {pops, {T_NUM, T_NUM, T_NUM}, T_NUM, READ, A_NONE, O_ANY}
#define COMPARE {2, {T_NUM, T_NUM}, T_BOOL, READ, A_NONE, O_ANY}
#define LOGIC(pops) {pops, {T_BOOL, T_BOOL}, T_BOOL, READ, A_NONE, O_ANY}
#define I64S(pops) {pops, {T_I64, T_I64}, T_I64, READ, A_NONE, O_ANY}
#define STATE(type) A_STATE + PURE_STATE_##type
static const struct { unsigned char pops, in[3], out; unsigned short where; unsigned char arg, of; } signature[PURE_OP_COUNT] = {
    [PURE_OP_CONST] = {0, {0}, T_NUM, READ, A_TYPE, O_ANY}, [PURE_OP_ADD] = NUMS(2), [PURE_OP_SUB] = NUMS(2),
    [PURE_OP_MUL] = NUMS(2), [PURE_OP_DIV] = NUMS(2), [PURE_OP_NEG] = NUMS(1), [PURE_OP_LT] = COMPARE,
    [PURE_OP_LE] = COMPARE, [PURE_OP_GT] = COMPARE, [PURE_OP_GE] = COMPARE, [PURE_OP_EQ] = COMPARE,
    [PURE_OP_NE] = COMPARE, [PURE_OP_AND] = LOGIC(2), [PURE_OP_OR] = LOGIC(2), [PURE_OP_NOT] = LOGIC(1),
    [PURE_OP_MIN] = NUMS(2), [PURE_OP_MAX] = NUMS(2), [PURE_OP_CLAMP] = NUMS(3),
    [PURE_OP_LOAD] = {0, {0}, T_STATE, READ, A_VALUE, O_ANY},
    [PURE_OP_DT] = {0, {0}, T_NUM, IN_HANDLER, A_NONE, O_ANY},
    [PURE_OP_STORE] = {1, {T_STATE}, 0, IN_HANDLER, A_VALUE, O_ANY},
    [PURE_OP_JZ] = {1, {T_BOOL}, 0, READ, A_TARGET, O_ANY},
    [PURE_OP_JMP] = {0, {0}, 0, READ, A_TARGET, O_ANY},
    [PURE_OP_FIELD] = {0, {0}, T_FIELD, IN_MESSAGE | IN_ARGUMENTS, A_FIELD, O_ANY},
    [PURE_OP_DOC] = {0, {0}, T_BYTES, IN_KEYED, A_COLLECTION, O_ANY},
    [PURE_OP_DOC_SIZE] = {0, {0}, T_NUM, IN_KEYED, A_COLLECTION, O_ANY},
    [PURE_OP_LEN] = {1, {T_BYTES}, T_NUM, READ, A_NONE, O_ANY},
    [PURE_OP_REPLACE] = {3, {T_NUM, T_NUM, T_BYTES}, 0, IN_KEYED, A_COLLECTION, O_ANY},
    [PURE_OP_REPLY] = {1, {T_BYTES}, 0, IN_MESSAGE, A_NONE, O_ANY},
    [PURE_OP_REQUIRE] = {1, {T_BOOL}, 0, IN_HANDLER, A_NONE, O_ANY},
    [PURE_OP_TEXT_CONST] = {0, {0}, T_TEXT, READ, A_CONSTANT, O_ANY},
    [PURE_OP_TEXT_LOAD] = {0, {0}, T_TEXT, READ, STATE(TEXT), O_ANY},
    [PURE_OP_TEXT_INPUT] = {0, {0}, T_TEXT, IN_TEXT, A_NONE, O_ANY},
    [PURE_OP_NUM_TEXT] = {1, {T_NUM}, T_TEXT, READ, A_FORMAT, O_ANY},
    [PURE_OP_CONCAT] = {2, {T_TEXT, T_TEXT}, T_TEXT, READ, A_JOINED, O_ANY},
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
    [PURE_OP_LIST_APPEND] = {0, {0}, 0, IN_HANDLER | IN_COMPUTE, STATE(LIST), O_ANY},
    [PURE_OP_LIST_INSERT] = {1, {T_INDEX}, 0, IN_HANDLER | IN_COMPUTE, STATE(LIST), O_ARG},
    [PURE_OP_LIST_REMOVE] = {1, {T_INDEX}, 0, IN_HANDLER | IN_COMPUTE, STATE(LIST), O_ARG},
    [PURE_OP_ITEM_GET] = {1, {T_INDEX}, T_ITEM, READ, A_ITEM, O_LIST},
    [PURE_OP_ITEM_SET] = {1, {T_INDEX}, 0, IN_HANDLER | IN_COMPUTE, A_ITEM, O_LIST},
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
    [PURE_OP_TEXT_COMPARE] = {2, {T_TEXT, T_TEXT}, T_BOOL, READ, A_RELATION, O_ANY},
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
    [PURE_OP_FLOOR] = NUMS(1), [PURE_OP_MOD] = NUMS(2),
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
    FieldValue was[PURE_MODULE_FIELDS], now[PURE_MODULE_FIELDS];
    *out = NULL;
    *size = 0;
    if (from_count > PURE_MODULE_FIELDS || to_count > PURE_MODULE_FIELDS || !read_fields(from, from_count, payload, was))
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

static unsigned char field_type(const PureModule *m, uint32_t list, uint32_t field) {
    static const unsigned char types[] = {[PURE_RECORD_NUMBER] = T_NUM, [PURE_RECORD_TEXT] = T_TEXT,
                                          [PURE_RECORD_BOOL] = T_BOOL, [PURE_RECORD_I64] = T_I64, [PURE_RECORD_LIST] = T_LIST};
    return types[m->record_field_type[m->record[m->state_ref[list]].first + field]];
}

/* The record of the lists a LIST field of a record holds (field: the module's numbering), or UINT32_MAX. */
static uint32_t nested_record(const PureModule *m, uint32_t field) {
    return m->record_field_type[field] == PURE_RECORD_LIST ? (uint32_t)m->record_field_default[field].i64 : UINT32_MAX;
}

/* An event's list, and one bound by LIST_BIND (a register: never changed by code). */
static bool event_list_state(const PureModule *m, uint32_t list) {
    return list < m->states && m->state_type[list] == PURE_STATE_LIST && m->state_flags[list] & PURE_LIST_ROWS >> 8;
}
static bool bound_list(const PureModule *m, uint32_t list) {
    return event_list_state(m, list) && m->state_flags[list] & PURE_LIST_BOUND >> 8;
}

/* The stack type of a record field (the module's numbering). */
static unsigned char record_type(const PureModule *m, uint32_t field) {
    static const unsigned char types[] = {[PURE_RECORD_NUMBER] = T_NUM, [PURE_RECORD_TEXT] = T_TEXT,
                                          [PURE_RECORD_BOOL] = T_BOOL, [PURE_RECORD_I64] = T_I64, [PURE_RECORD_LIST] = T_LIST};
    return types[m->record_field_type[field]];
}

/* The record of the list value an instruction takes as its operand k (bottom 0). */
static unsigned list_wanted(const PureModule *m, const PureOp *op, unsigned k) {
    uint32_t arg = op->arg;
    switch (op->op) {
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

/* Whether a list's record field is a text. */
static bool text_field(const PureModule *m, uint32_t list, uint32_t field) {
    return m->record_field_type[m->record[m->state_ref[list]].first + field] == PURE_RECORD_TEXT;
}

/* What a local holds (its argument's bits 8-15): a number, an index, a bool or an i64. */
static unsigned char local_type(uint32_t arg) {
    static const unsigned char types[] = {T_NUM, T_INDEX, T_BOOL, T_I64};
    return types[arg >> 8 & 3];
}

PURE_COLD static bool names_arg(const PureModule *m, unsigned kind, uint32_t arg, const PureInputShape *shape, int row,
                                int outer) {
    switch (kind) {
    case A_ITEM:
        return state_of(m, list_of(arg), PURE_STATE_LIST) && item_field(arg) < m->record[m->state_ref[list_of(arg)]].count;
    case A_FIELD: return arg < shape->field_count && m->field[shape->field_start + arg].type != PURE_FIELD_ROWS;
    case A_COLLECTION: return arg < m->collections;
    case A_CONSTANT: return arg < m->constants;
    case A_UNIT: return arg <= PURE_UNIT_LINE;
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
    case A_LOCAL: /* a slot, and a number, an index of a list, a bool or an i64 */
        return (arg & 0xFF) < PURE_MODULE_LOCALS && (arg >> 8 & 0xFF) <= 3 &&
               ((arg >> 8 & 0xFF) == 1 ? state_of(m, arg >> 16, PURE_STATE_LIST) : !(arg >> 16));
    case A_VALUE: return state_of(m, arg, PURE_STATE_NUM) || state_of(m, arg, PURE_STATE_BOOL) || state_of(m, arg, PURE_STATE_I64);
    case A_LOOP: return (arg & 0xFF) < PURE_MODULE_LOCALS && !(arg & 0xFE00); /* a slot, and whether i64s */
    case A_TYPE: return arg <= 2;
    case A_BITS: return arg <= PURE_BITS_RIGHT;
    case A_POSITION: case A_JOINED: return arg <= 1;
    case A_HELD: return (arg & ~(uint32_t)PURE_HELD_APPEND) < PURE_MODULE_HELD;
    case A_FORMAT: { /* a type; a width, zeros, a form fitting it and digits for FIXED alone */
        uint32_t type = arg & 3, form = arg >> PURE_FORMAT_FORM & 7, digits = arg >> PURE_FORMAT_DIGITS & 31;
        return type <= 2 && !(arg & 0xFC) && !(arg >> 25) && form <= PURE_FORMAT_FIXED && (type != 1 || !form) &&
               (form != PURE_FORMAT_FIXED || type == 0) && (form == PURE_FORMAT_FIXED || !digits);
    }
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
                                          bool locals, unsigned nest, uint32_t walked, uint32_t *at);

/* target marks, per instruction of the block, a branch into it with an empty stack. */
/* outer: the row of the column a fold's block runs in (-1: none). */
/* locals: the block is a handler's, or runs in one (a walk, a fold): it reads the handler's locals. */
/* nest: the loops (LOOP, EACH) the block runs in; walked: a bit for each list the walks among them walk. */
PURE_COLD static const char *verify_code(const PureModule *m, PureBlock block, int view, int row, int outer,
                                         uint32_t event, bool locals, unsigned nest, uint32_t walked, uint32_t *at,
                                         unsigned char *target) {
#define REFUSE(why) do { *at = pc; return why; } while (0)
    bool steps = view == STEPS || view == STEPS_WALK, handler = view < 0 || view == WALK || steps;
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
    if (shape && handler && !steps && shape->kind == PURE_KIND_MESSAGE) where |= IN_MESSAGE | (shape->keyed ? IN_KEYED : 0);
    if (shape && handler && !steps && shape->kind == PURE_KIND_TEXT) where |= IN_TEXT;
    if (shape && (!handler || steps) && (shape->kind == PURE_KIND_MESSAGE || shape->kind == PURE_KIND_ARGUMENTS))
        where |= IN_ARGUMENTS;
    if (locals) where |= IN_LOCALS;
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
        if (m->height) m->height[pc] = (uint8_t)height; /* the compiled code's slots (021) */
        const PureOp *op = &m->code[pc];
        if (op->op >= PURE_OP_COUNT) REFUSE("unknown opcode");
        unsigned allowed = signature[op->op].where, kind = signature[op->op].arg, of = signature[op->op].of;
        if (!(allowed & where)) {
            if (allowed & IN_ROW) REFUSE("row outside a column");
            if (allowed & IN_OUTER) REFUSE("outer row outside a fold in a column");
            if (allowed & IN_LOCALS) REFUSE("local outside a handler");
            if (view == FILTER) REFUSE("filter with an effect");
            if (folding) REFUSE("fold with an effect");
            if (!handler || steps) REFUSE("view with an effect");
            if (allowed & IN_KEYED) REFUSE("document operation outside a keyed message handler");
            if (allowed & IN_TEXT) REFUSE("text input outside a text handler");
            REFUSE("message operation outside a message handler");
        }
        uint32_t arg = op->arg;
        /* A constant: a finite number, a bool +0 or 1, any i64 (arg says which). */
        bool constant = op->op == PURE_OP_CONST, truth = constant && arg == 1, integer = constant && arg == 2;
        if ((kind == A_NONE && arg) ||
            (truth ? !canonical_bool(op->value) : integer ? false : constant ? !isfinite(op->value) : op->integer != 0))
            REFUSE("non-canonical operand");
        if (!names_arg(m, kind, arg, shape, row, outer)) REFUSE("operand names no such state, field, constant or unit");
        /* A keyed list places its items by key, and an item's key stays. */
        if (op->op == PURE_OP_LIST_INSERT && m->state_key[arg]) REFUSE("insert by position into a keyed list");
        /* A walk ends: no list a walk walks grows while it runs, and only the innermost walk's may shrink (it moves with it). */
        if ((op->op == PURE_OP_LIST_APPEND || op->op == PURE_OP_LIST_INSERT) && walked >> arg & 1)
            REFUSE("insert into the list walked");
        if (op->op == PURE_OP_LIST_REMOVE && walked >> arg & 1 && (int)arg != row)
            REFUSE("remove from a list an outer walk walks");
        if (nest && op->op == PURE_OP_REPLY) REFUSE("reply inside a loop");
        if (nest == PURE_MODULE_NESTING && (op->op == PURE_OP_EACH || op->op == PURE_OP_LOOP)) REFUSE("loops nested deeper than 8");
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
        if (op->op == PURE_OP_APPEND_IN &&
            (!event_list_state(m, list_of(arg)) || field_type(m, list_of(arg), item_field(arg)) != T_LIST))
            REFUSE("append into a field not an event's list's list");
        /* A view's body changes an event's lists alone: its states are a snapshot's. */
        if (body && (op->op == PURE_OP_LIST_APPEND || op->op == PURE_OP_LIST_INSERT || op->op == PURE_OP_LIST_REMOVE ||
                     op->op == PURE_OP_ITEM_SET) &&
            !(m->state_flags[op->op == PURE_OP_ITEM_SET ? list_of(arg) : arg] & PURE_LIST_ROWS >> 8))
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
            const char *why = kind == A_EACH ? verify_block(m, (PureBlock){pc + 1, skip}, body ? STEPS_WALK : WALK, (int)list, -1,
                                                            event, locals, nest + 1, walked | UINT32_C(1) << (list & 31), &inner)
                            : kind == A_LOOP ? verify_block(m, (PureBlock){pc + 1, skip}, body && !steps ? STEPS : view, row,
                                                            outer, event, locals, nest + 1, walked, &inner)
                                             : verify_block(m, (PureBlock){pc + 1, skip},
                                                            how == PURE_FOLD_COUNT || how == PURE_FOLD_FIRST ? FOLD_ONE :
                                                            arg & PURE_FOLD_INTEGER ? FOLD_I64 : FOLD_TWO,
                                                            (int)list, row, event, locals, nest, walked, &inner);
            if (why) { *at = inner; return why; }
            for (uint32_t k = i + 1; k <= i + skip; ++k) {
                const char *into = kind == A_FOLD ? "branch into a fold" : kind == A_EACH ? "branch into an each" :
                                   "branch into a loop";
                if (target[k]) REFUSE(into);
                for (unsigned j = 0; j < open; ++j) if (joins[j].target == block.start + k) REFUSE(into);
            }
        }
        /* Operand types, bottom to top; record operations take their fields. */
        unsigned char need[PURE_MODULE_FIELDS + 2];
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
        } else if (op->op == PURE_OP_CONCAT && arg) {
            need[0] = need[1] = T_BYTES;
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
        }
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
        if ((op->op == PURE_OP_CONCAT && arg) || (op->op == PURE_OP_BYTES_LOAD && (arg & 0xFF) < PURE_HELD_TEXT)) out = T_BYTES;
        else if (op->op == PURE_OP_BYTES_LOAD) out = T_TEXT;
        if (out) {
            if (height == PURE_MODULE_STACK) REFUSE("stack overflow");
            owners[height] = (unsigned char)(out != T_CURSOR && out != T_INDEX ? 0 : of == O_OPERAND ? owners[height] : owner);
            if (out == T_LIST) /* the record of its items */
                owners[height] = (unsigned char)(op->op == PURE_OP_LIST_VALUE ? m->state_ref[arg] :
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
        i += skip;
    }
    if (!reachable) return "block end is unreachable";
    if (view == FILTER) return height != 1 || types[0] != T_BOOL ? "filter must leave one bool" : NULL;
    if (view == FOLD_ONE) return height != 1 || types[0] != T_BOOL ? "fold block must leave one bool" : NULL;
    if (view == FOLD_TWO)
        return height != 2 || types[0] != T_NUM || types[1] != T_BOOL ? "fold block must leave a number and a bool" : NULL;
    if (view == FOLD_I64)
        return height != 2 || types[0] != T_I64 || types[1] != T_BOOL ? "fold block must leave an i64 and a bool" : NULL;
    unsigned char result = view == PURE_VIEW_TEXT ? T_TEXT : view == PURE_VIEW_BOOL ? T_BOOL : view == PURE_VIEW_I64 ? T_I64 : T_NUM;
    if (!handler ? height != 1 || types[0] != result : height != 0)
        return !handler ? "view must leave one value of its type" : "handler must leave no value";
    return NULL;
#undef REFUSE
}

PURE_COLD static const char *verify_block(const PureModule *m, PureBlock block, int view, int row, int outer,
                                          uint32_t event, bool locals, unsigned nest, uint32_t walked, uint32_t *at) {
    if (block.start > m->code_count || block.length > m->code_count - block.start) return "code range outside module";
    unsigned char *target = calloc((size_t)block.length + 1, 1);
    if (!target) return "out of memory";
    const char *why = verify_code(m, block, view, row, outer, event, locals, nest, walked, at, target);
    free(target);
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
    case PURE_OP_LOCAL_LOAD: return local_type(op->arg) != T_I64;
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
            if (local_type(op->arg) == T_I64) return false;
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
        if (!bounded && !faults && !faultless(m, block, nodes, tests[k])) ok = false;
    }
    free(nodes);
    if (!ok || (!range.lower_relation && !range.upper_relation)) return false;
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
    pure_jit_free(m->jit);
    m->jit = NULL;
    m->batches = false;
    free(m->height);
    m->height = NULL;
    free(m->reuse_from), free(m->reuse_length);
    m->reuse_from = m->reuse_length = NULL;
    m->reuses = false;
    pure_batch_plans_free(m);
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
    if (!p || size < PURE_MODULE_HEADER || memcmp(p, magic, sizeof(magic))) REFUSE("not a Pure module v6", 0);
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
    if (!(out->height = calloc(out->code_count + 1, 1)) || !(out->reuse_from = calloc(out->code_count + 1, sizeof(uint32_t))) ||
        !(out->reuse_length = calloc(out->code_count + 1, sizeof(uint32_t)))) {
        result = PURE_ALLOCATION;
        REFUSE("out of memory", 0);
    }
    out->constant_offset = (uint32_t *)(out->code + out->code_count);
    out->constant_length = out->constant_offset + out->constants;
    out->constant_data = (unsigned char *)(out->constant_length + out->constants);
    const unsigned char *at = p + PURE_MODULE_HEADER;
    const unsigned char *text = p + size - constant_bytes - strings;
    if (constant_bytes) memcpy(out->constant_data, p + size - constant_bytes, constant_bytes);
    for (uint32_t i = 0; i < out->states; ++i, at += STATE_BYTES) {
        uint32_t type = read32(at + 4);
        out->state_type[i] = (uint8_t)type;
        if ((type & ~(uint32_t)PURE_STATE_TRANSIENT) > PURE_STATE_I64) REFUSE("state type", i);
        if (type & PURE_STATE_TRANSIENT) out->transient |= UINT32_C(1) << i;
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
            out->state_flags[i] = cursor ? (uint8_t)flags : (uint8_t)((rows ? PURE_LIST_ROWS >> 8 : 0) | (bound ? PURE_LIST_BOUND >> 8 : 0));
            out->state_key[i] = (uint8_t)key;
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
        /* Arguments bear their view's name (view.args); other inputs are identifiers. */
        if (!name_at(text, strings, read32(at), shape->kind == PURE_KIND_ARGUMENTS ? NAME_PATH : NAME_IDENTIFIER,
                     out->input_names[i])) REFUSE("input name", i);
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
        bool arguments = shape->kind == PURE_KIND_ARGUMENTS;
        if ((shape->kind != PURE_KIND_MESSAGE && shape->kind != PURE_KIND_OUTPUT && !arguments) || (!arguments && read32(at + 8)) ||
            ((shape->kind == PURE_KIND_OUTPUT || arguments) && shape->keyed) || shape->field_start != next_field ||
            shape->field_count > out->fields - next_field) REFUSE("message shape", i);
        shape->view = arguments ? read32(at + 8) : 0;
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
            if (!name_at(text, strings, read32(entry), NAME_PATH, out->record_field_names[f]) || type > PURE_RECORD_LIST)
                REFUSE("record field", f);
            for (uint32_t k = record->first; k < f; ++k)
                if (!strcmp(out->record_field_names[k], out->record_field_names[f])) REFUSE("duplicate record field", f);
            /* A default: a text's constant, an i64's 8 bytes, a number (a bool's +0 or 1). */
            if (type == PURE_RECORD_TEXT) out->record_field_default[f].num = read32(entry + 8);
            else if (type == PURE_RECORD_I64 || type == PURE_RECORD_LIST) /* a LIST's: the record of its items */
                out->record_field_default[f].i64 = (int64_t)(read32(entry + 8) | (uint64_t)read32(entry + 12) << 32);
            else out->record_field_default[f].num = read_double(entry + 8);
            double given = out->record_field_default[f].num;
            if (type == PURE_RECORD_TEXT ? read32(entry + 12) || read32(entry + 8) >= out->constants
                : type == PURE_RECORD_LIST ? (uint64_t)out->record_field_default[f].i64 >= out->records
                : type != PURE_RECORD_I64 && (!isfinite(given) || (type == PURE_RECORD_BOOL && !canonical_bool(given))))
                REFUSE("record field default", f);
            if (type == PURE_RECORD_LIST) out->record_nested[i] = true;
        }
        next_record_field += record->count;
    }
    if (next_record_field != out->record_fields) REFUSE("unused record fields", next_record_field);
    at = fields_at + (size_t)out->record_fields * RECORD_FIELD_BYTES;
    for (uint32_t i = 0; i < out->states; ++i)
        if ((out->state_type[i] == PURE_STATE_TEXT && out->state_ref[i] >= out->constants) ||
            (out->state_type[i] == PURE_STATE_CURSOR && !state_of(out, out->state_ref[i], PURE_STATE_TEXT)) ||
            (out->state_type[i] == PURE_STATE_LIST &&
             (out->state_ref[i] >= out->records || out->state_key[i] > out->record[out->state_ref[i]].count ||
              (out->state_key[i] &&
               out->record_field_type[out->record[out->state_ref[i]].first + out->state_key[i] - 1u] == PURE_RECORD_BOOL))) ||
            (out->state_type[i] == PURE_STATE_INDEX && !state_of(out, out->state_ref[i], PURE_STATE_LIST)))
            REFUSE("state reference", i);
    /* A list in a record is an event's alone (027's stage 2b): no stored, transient or message's list holds one, and no
     * list is keyed by one; a register is no message's rows. */
    for (uint32_t i = 0; i < out->states; ++i) {
        if (out->state_type[i] != PURE_STATE_LIST) continue;
        bool nested = out->record_nested[out->state_ref[i]];
        if ((nested && !(out->state_flags[i] & PURE_LIST_ROWS >> 8)) ||
            (out->state_key[i] && out->record_field_type[out->record[out->state_ref[i]].first + out->state_key[i] - 1u] == PURE_RECORD_LIST))
            REFUSE("list in a record outside an event's list", i);
    }
    for (uint32_t f = 0; f < out->fields; ++f)
        if (out->field[f].type == PURE_FIELD_ROWS &&
            (out->record_nested[out->state_ref[out->field[f].rows]] || out->state_flags[out->field[f].rows] & PURE_LIST_BOUND >> 8))
            REFUSE("rows field", f);
    /* A stored position into what is not stored would come back meaning nothing. */
    for (uint32_t i = 0; i < out->states; ++i)
        if ((out->state_type[i] == PURE_STATE_CURSOR || out->state_type[i] == PURE_STATE_INDEX) &&
            !(out->transient >> i & 1) && out->transient >> out->state_ref[i] & 1)
            REFUSE("stored position in a transient state", i);
    /* A table names its list in place of code; its columns follow it. */
    for (uint32_t i = 0; i < out->views; ++i, at += VIEW_BYTES) {
        if (!name_at(text, strings, read32(at), NAME_IDENTIFIER, out->view_names[i])) REFUSE("view name", i);
        out->view_type[i] = at[4];
        out->view_table[i] = (uint16_t)read16(at + 6);
        out->view_code[i] = (PureBlock){read32(at + 8), read32(at + 12)};
        uint32_t table = out->view_table[i];
        if (at[4] > PURE_VIEW_I64 || (at[5] && at[4] != PURE_VIEW_TABLE) ||
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
    /* Arguments are a view's (not a column's: a table's columns and filter
     * read the table's), one set a view, bearing its name. */
    for (uint32_t i = 0; i < out->inputs; ++i) {
        const PureInputShape *shape = &out->input[i];
        if (shape->kind != PURE_KIND_ARGUMENTS) continue;
        char path[PURE_MODULE_NAME + 8];
        if (shape->view >= out->views || out->view_table[shape->view] || out->view_arguments[shape->view]) REFUSE("arguments", i);
        snprintf(path, sizeof(path), "%s.args", out->view_names[shape->view]);
        if (strcmp(path, out->input_names[i])) REFUSE("arguments", i);
        out->view_arguments[shape->view] = (uint8_t)(i + 1);
    }
    for (uint32_t i = 0; i < out->views; ++i)
        if (out->view_table[i]) out->view_arguments[i] = out->view_arguments[out->view_table[i] - 1];
    for (uint32_t i = 0; i < out->handlers; ++i, at += HANDLER_BYTES) {
        uint32_t event = read32(at);
        out->handler[i] = (PureHandler){event, {read32(at + 4), read32(at + 8)}};
        int input = event_input(event);
        if (event > 2u * out->inputs || (input >= 0 && out->input[input].kind != PURE_KIND_KEY && !(event & 1)) ||
            (input >= 0 && (out->input[input].kind == PURE_KIND_OUTPUT || out->input[input].kind == PURE_KIND_ARGUMENTS)))
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
        uint32_t event = out->view_arguments[i] ? 2u * out->view_arguments[i] - 1u : 0; /* the arguments' input, as an arrival */
        bool computes = !table && !out->view_table[i]; /* a view outside a table may have a body */
        if ((!table || out->view_code[i].length) &&
            (why = verify_block(out, out->view_code[i], table ? FILTER : out->view_type[i],
                                table || out->view_table[i] ? out->view_list[i] : -1, -1, event, computes, 0, 0, &bad)))
            goto refused;
        if (computes && !locals_agree(out, out->view_code[i])) { why = "local used as two things"; goto refused; }
        for (uint32_t pc = out->view_code[i].start; computes && pc < out->view_code[i].start + out->view_code[i].length; ++pc) {
            const PureOp *op = &out->code[pc];
            if (op->op == PURE_OP_LOCAL_LOAD || op->op == PURE_OP_LOCAL_STORE || op->op == PURE_OP_LOOP || op->op == PURE_OP_EACH)
                out->view_body[i] = true;
            if (op->op == PURE_OP_LIST_APPEND || op->op == PURE_OP_LIST_INSERT || op->op == PURE_OP_LIST_REMOVE)
                out->view_lists[i] |= UINT32_C(1) << (op->arg & 31);
            if (op->op == PURE_OP_ITEM_SET) out->view_lists[i] |= UINT32_C(1) << (list_of(op->arg) & 31);
        }
        if (out->view_lists[i]) out->view_body[i] = true;
    }
    for (uint32_t i = 0; i < out->handlers; ++i) {
        bad = i;
        if ((why = verify_block(out, out->handler[i].code, -1, -1, -1, out->handler[i].event, true, 0, 0, &bad))) goto refused;
        if (!locals_agree(out, out->handler[i].code)) { why = "local used as two things"; goto refused; }
    }
    for (uint32_t i = 0; i < out->views; ++i) {
        int given = (int)out->view_arguments[i] - 1;
        plan_folds(out, out->view_code[i], given);
        out->view_plan[i] = out->view_type[i] == PURE_VIEW_TABLE && out->view_code[i].length &&
                            block_has_lanes(out, out->view_code[i], given);
        PureRange range;
        if (out->view_plan[i] && block_range(out, out->view_code[i], out->view_list[i], true, &range))
            out->view_range[i] = keep_range(out, &range);
        if (out->view_type[i] != PURE_VIEW_TABLE) range_code(out, out->view_code[i], given);
        /* A fold in a column runs for every row: one pass may answer them all. */
        for (uint32_t pc = out->view_code[i].start; out->view_table[i] && pc < out->view_code[i].start + out->view_code[i].length; ++pc)
            if (out->code[pc].op == PURE_OP_FOLD) {
                shape_fold(out, pc);
                pc += out->code[pc].arg >> 16;
            }
    }
    for (uint32_t i = 0; i < out->handlers; ++i) {
        int input = event_input(out->handler[i].event);
        input = input >= 0 && out->input[input].kind == PURE_KIND_MESSAGE ? input : -1;
        plan_folds(out, out->handler[i].code, input);
        range_code(out, out->handler[i].code, input);
    }
    /* What a cell is, read once here: an item's field, or a keyed list's key, a text (PLAN_TEXT) or not. */
    for (uint32_t pc = 0; pc < out->code_count; ++pc) {
        PureOp *op = &out->code[pc];
        if (op->op == PURE_OP_ITEM_GET || op->op == PURE_OP_ITEM_SET) op->plan = text_field(out, list_of(op->arg), item_field(op->arg));
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
        if (op->op == PURE_OP_LIST_FIND && next->op == PURE_OP_ITEM_GET && list_of(next->arg) == op->arg && !landed[pc + 1])
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
    out->jit = pure_jit_make(out); /* none: the executor runs it all */
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
    if (!size || bytes[0] > PURE_MODULE_FIELDS) return -1;
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
    PureField sent[PURE_MODULE_FIELDS];
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
    PureField sent[PURE_MODULE_FIELDS];
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
 * 1 where that passes the room (MEMORY), 2 out of memory, else 0. */
static int hold(PureApp *app, PureArena *arena, unsigned slot, PureBytes v, bool append) {
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

PURE_COLD static PureList *new_list(const PureModule *m, uint32_t record);

/* The values that share each list in the LIST fields of cells (an item's, just stored). */
static void share_cells(PureApp *app, const PureModule *m, uint32_t record, const PureCell *cells) {
    const PureRecord *r = &m->record[record];
    for (unsigned f = 0; f < r->count; ++f)
        if (m->record_field_type[r->first + f] == PURE_RECORD_LIST && cells[f].i64) ++app->nested.shared[cells[f].i64 - 1];
}

/* A new list of record's items among the event's (empty, or a copy of from's items): its number, or 0 out of memory. */
static uint64_t nest(PureApp *app, uint32_t record, const PureList *from) {
    const PureModule *m = app->module;
    if (app->nested.count == app->nested.capacity) {
        size_t capacity = app->nested.capacity ? app->nested.capacity * 2 : 16;
        PureList **lists = realloc(app->nested.list, capacity * sizeof(*lists));
        if (lists) app->nested.list = lists;
        uint32_t *shared = lists ? realloc(app->nested.shared, capacity * sizeof(*shared)) : NULL;
        if (!shared) return 0;
        app->nested.shared = shared;
        app->nested.capacity = capacity;
    }
    PureList *list = new_list(m, record);
    if (!list) return 0;
    const PureRecord *r = &m->record[record];
    for (size_t at = 0, count = from ? pure_list_count(from) : 0; at < count; ++at) {
        PureCell cells[PURE_RECORD_FIELDS];
        for (unsigned f = 0; f < r->count; ++f) cells[f] = pure_list_get(from, at, f);
        if (pure_list_insert(list, at, cells, NULL) != PURE_OK) { pure_list_free(list); return 0; }
        if (m->record_nested[record]) share_cells(app, m, record, cells);
    }
    app->nested.list[app->nested.count] = list;
    app->nested.shared[app->nested.count] = 0;
    return ++app->nested.count;
}

/* The bound lists given back their own, and the event's lists in records let go (an event or a view begins). */
static void unbind(PureApp *app) {
    for (uint32_t i = 0; i < PURE_MODULE_STATES; ++i)
        if (app->unbound[i]) app->lists[i] = app->unbound[i], app->unbound[i] = NULL;
}
static void drop_nested(PureApp *app) {
    for (size_t i = 0; i < app->nested.count; ++i) pure_list_free(app->nested.list[i]);
    app->nested.count = 0;
}

/* NUM_TEXT's text of v in a format (the argument past its type, PureFormat): false where an integer's form meets a
 * number that is not a whole one within 2^53. */
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
        unsigned count = 0;
        do reversed[count++] = set[magnitude % base], magnitude /= base; while (magnitude);
        if (i < 0) body[n++] = '-';
        while (count) body[n++] = reversed[--count];
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
 * so far, over numbers (total) or i64s (whole; a sum past the range
 * overflows and ends the fold). */
typedef struct { unsigned how; bool integer, overflow; double total; int64_t whole; bool any, done; Seen seen; } Fold;

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

/* One item run as written: a number's, or an i64's (a sum past the range
 * overflows and ends the fold; count and first take no i64). */
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
    if (fold->how == PURE_FOLD_SUM) {
        if (__builtin_add_overflow(fold->whole, v, &fold->whole)) fold->overflow = fold->done = true;
    } else if (!fold->any || (fold->how == PURE_FOLD_MAX ? v > fold->whole : v < fold->whole))
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
    case PURE_OP_DT: stack[sp++].num = ev->dt; NEXT;
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
    case PURE_OP_DOC: case PURE_OP_DOC_SIZE: {
        if (op->op == PURE_OP_DOC && !ev->exists) { kind = PURE_FAULT_NOT_FOUND; FAILED; }
        PureVersion version = app->data.version(app->data.context, app->document);
        if (op->op == PURE_OP_DOC) stack[sp++].bytes = (PureBytes){FROM_DOCUMENT, 0, version, 0, version.bytes};
        else stack[sp++].num = (double)version.bytes;
        NEXT;
    }
    case PURE_OP_LEN: case PURE_OP_TEXT_BYTES: top->num = (double)top->bytes.length; NEXT;
    case PURE_OP_REPLACE: {
        PureBytes insert = stack[--sp].bytes;
        double last = stack[--sp].num, first = stack[--sp].num;
        double size = (double)app->data.version(app->data.context, app->document).bytes;
        if (first != floor(first) || last != floor(last) || first < 0 || last < first || last > size) {
            kind = PURE_FAULT_LIMIT;
            FAILED;
        }
        PureSpan span;
        size_t mark = arena->used;
        if ((status = contiguous(app, arena, insert, &span)) == PURE_OK)
            status = app->data.splice(app->data.context, app->document, (size_t)first, (size_t)(last - first), span);
        arena->used = mark;
        if (status != PURE_OK) REFUSED;
        ev->modified = ev->exists = true;
        NEXT;
    }
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
    case PURE_OP_TEXT_INPUT: stack[sp++].bytes = (PureBytes){FROM_PAYLOAD, 0, {0, 0}, 0, app->payload.size}; NEXT;
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
        at->num = (double)(arg == PURE_UNIT_CHAR ? pure_text_step(text, version, (size_t)at->num, count)
                                                 : pure_text_line_step(text, version, (size_t)at->num, count));
        NEXT;
    }
    case PURE_OP_CUR_COUNT: {
        PureDraft *text = app->texts[top->bytes.owner];
        top->num = (double)pure_text_count(text, pure_draft_version(text), (size_t)top->num, arg);
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
        /* Room for every text first, so no span moves while the next is copied. */
        size_t mark = arena->used, total = 0;
        for (unsigned f = 0; f < record->count; ++f)
            if (text_field(m, arg, f)) total += (size_t)stack[base + f].bytes.length;
        if (!reserve(arena, total)) STOPPED(PURE_ALLOCATION);
        PureCell cells[PURE_RECORD_FIELDS];
        for (unsigned f = 0; f < record->count; ++f) {
            PureSpan span = {0};
            if (text_field(m, arg, f) && (status = contiguous(app, arena, stack[base + f].bytes, &span)) != PURE_OK)
                STOPPED(status);
            cells[f] = (PureCell){.i64 = stack[base + f].i64, .text = (unsigned char *)span.data, .size = span.size};
        }
        Held before = list_held(app, list);
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
        if (!grows(app, before, list_held(app, list))) { kind = PURE_FAULT_MEMORY; FAILED; }
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
        if (!(op->plan & PLAN_TEXT)) top->i64 = cell.i64; /* the bits: a number's, a bool's or an i64's (else a text) */
        else if ((status = keep_bytes(arena, cell.text, cell.size, &top->bytes)) != PURE_OK) STOPPED(status);
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
            if (op->range) /* only the items a range of keys allows can count */
                range_bounds(m, values, &m->range[op->range - 1], list, m->state_key[arg >> 8 & 0xFF] - 1u, &per, &from, &to);
            status = op->plan ? fold_runs(m, values, inner, list, &per, &fold, from, to)
                              : fold_rows(m, values, inner, &per, from, to, &fold);
            free(fold.seen.keys);
            if (status != PURE_OK) STOPPED(status);
        }
        if (fold.overflow) { kind = PURE_FAULT_OVERFLOW; FAILED; }
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
        stack[sp - 1].num = related(arg, order);
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
        stack[sp++].bytes = (PureBytes){FROM_HELD, (uint16_t)(arg & 0xFF), {0, 0}, 0, app->held[arg & 0xFF].size};
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
    case PURE_OP_LIST_VALUE: { /* a copy of an event's list's items (an empty one is 0, made of nothing) */
        if (!pure_list_count(app->lists[arg])) { stack[sp++].i64 = 0; NEXT; }
        uint64_t made = nest(app, m->state_ref[arg], app->lists[arg]);
        if (!made) STOPPED(PURE_ALLOCATION);
        if (!grows(app, (Held){0, 0}, (Held){0, pure_list_size(app->nested.list[made - 1])})) { kind = PURE_FAULT_MEMORY; FAILED; }
        stack[sp++].i64 = (int64_t)made;
        NEXT;
    }
    case PURE_OP_LIST_BIND: { /* the register reads the list (shared: no one changes it in place from now) */
        uint64_t list = (uint64_t)stack[--sp].i64;
        if (!app->unbound[arg]) app->unbound[arg] = app->lists[arg];
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
            if (m->record_field_type[record->first + f] == PURE_RECORD_TEXT) total += (size_t)stack[base + f].bytes.length;
        if (!reserve(arena, total)) STOPPED(PURE_ALLOCATION);
        PureCell cells[PURE_RECORD_FIELDS];
        for (unsigned f = 0; f < record->count; ++f) {
            PureSpan span = {0};
            if (m->record_field_type[record->first + f] == PURE_RECORD_TEXT &&
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
        value = fmod(a, b);
        if (value != 0 && (value < 0) != (b < 0)) value += b;
        else if (value == 0) value = copysign(0.0, b);
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
    if (m->batches) return run_batches(m, values, block, ev, result);
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

PURE_COLD static PureList *new_list(const PureModule *m, uint32_t record) {
    const PureRecord *r = &m->record[record];
    uint8_t kinds[PURE_RECORD_FIELDS];
    for (unsigned f = 0; f < r->count; ++f) kinds[f] = pure_record_kind(m, r->first + f);
    return pure_list_new(r->count, kinds);
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

/* The start of a burst: what a failed save returns to. Text drafts stay held
 * until the save, so every version since stays readable; a stored list's
 * journal is as long as its changes since the last save. */
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
        if (app->lists[i]) app->durable_lists[i] = pure_list_changes(app->lists[i]).size;
    }
    if (app->heard) app->durable_heard = pure_list_changes(app->heard).size;
    if (app->outbox) app->durable_outbox = pure_list_changes(app->outbox).size;
    app->durable_sent_saved = app->sent_saved;
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
        if (app->lists[i] && !keep) pure_list_forget(app->lists[i], app->durable_lists[i]);
    }
    if (app->heard && !keep) pure_list_forget(app->heard, app->durable_heard);
    if (app->outbox && !keep) pure_list_forget(app->outbox, app->durable_outbox);
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

/* After a save: what it stored is the links' saved state, every send in the
 * outbox saved; or, when it failed, the links return to the last saved
 * state (the sends, trims and numbers heard go back with the burst's list
 * changes). */
static void settle_links(PureApp *app, bool saved) {
    if (saved) {
        app->sent_saved = app->outbox ? pure_list_count(app->outbox) : 0;
        memcpy(app->saved_sent, app->last_sent, sizeof(app->saved_sent));
    } else {
        app->sent_saved = app->durable_sent_saved;
        memcpy(app->last_sent, app->saved_sent, sizeof(app->last_sent));
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
    PureValue values[PURE_MODULE_STATES];
    PureDraft *texts[PURE_MODULE_STATES];
    PureList *lists[PURE_MODULE_STATES];
    PureList *runtime[PURE_LISTS_STORED - PURE_LIST_HEARD]; /* only in a checkpoint's of one */
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

void pure_snapshot_release(PureSnapshot *snapshot) {
    if (!snapshot || __atomic_sub_fetch(&snapshot->refs, 1, __ATOMIC_ACQ_REL)) return;
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
    for (uint32_t i = 0; i < PURE_MODULE_STATES; ++i) {
        pure_draft_close(snapshot->texts[i]);
        pure_list_free(snapshot->lists[i]);
    }
    for (unsigned r = 0; r < PURE_LISTS_STORED - PURE_LIST_HEARD; ++r) pure_list_free(snapshot->runtime[r]);
    free(snapshot);
    if (all && !__atomic_sub_fetch(&all->refs, 1, __ATOMIC_ACQ_REL)) {
        pure_retired_free(&all->returned); /* the app is gone: no list takes it back */
        free(all);
    }
}

static void let_own_go(PureApp *app);

void pure_app_stop(PureApp *app) {
    unbind(app); /* a register's own list goes with the lists, the bound one with the event's */
    pure_app_finish(app);
    close_burst(app, true); /* unsaved changes are dropped with the app */
    let_own_go(app);
    for (uint32_t i = 0; i < PURE_MODULE_STATES && !app->reading; ++i) {
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
    drop_nested(app);
    free(app->nested.list);
    free(app->nested.shared);
    pure_draft_close(app->stored_values);
    free(app->carried);
    pure_draft_close(app->stored_links);
    pure_draft_close(app->stored_seq);
    pure_save_close_start(app);
    pure_log_close(app->log);
    free(app->pending);
    pure_undo_free(&app->undo);
    for (unsigned i = 0; i < app->row_tables; ++i) free(app->rows[i].items);
    free(app->rows);
    drop_passes(app);
    free(app->arena.bytes);
    free(app->view_arena.bytes);
    memset(app, 0, sizeof(*app));
}

/* Where one event began inside the burst: a failed event returns there. */
typedef struct {
    PureVersion texts[PURE_MODULE_STATES];
    size_t lists[PURE_MODULE_STATES], heard, outbox; /* stored lists' journal lengths */
    size_t undo, sent_saved;
    uint64_t last_sent[PURE_MODULE_INPUTS];
    bool opened;
} Mark;

static void mark_event(PureApp *app, Mark *mark) {
    mark->opened = !app->open;
    open_burst(app);
    for (uint32_t i = 0; i < app->module->states; ++i) {
        if (app->texts[i]) mark->texts[i] = pure_draft_version(app->texts[i]);
        if (app->lists[i]) mark->lists[i] = pure_list_changes(app->lists[i]).size;
    }
    mark->heard = app->heard ? pure_list_changes(app->heard).size : 0;
    mark->outbox = app->outbox ? pure_list_changes(app->outbox).size : 0;
    mark->undo = app->undo.count;
    mark->sent_saved = app->sent_saved;
    memcpy(mark->last_sent, app->last_sent, sizeof(mark->last_sent));
}

static void undo_event(PureApp *app, const Mark *mark) {
    for (uint32_t i = 0; i < app->module->states; ++i) {
        if (app->texts[i]) pure_draft_restore(app->texts[i], mark->texts[i]);
        if (app->lists[i]) pure_list_forget(app->lists[i], mark->lists[i]);
    }
    if (app->heard) pure_list_forget(app->heard, mark->heard);
    if (app->outbox) pure_list_forget(app->outbox, mark->outbox);
    pure_undo_rollback(&app->undo, mark->undo);
    app->sent_saved = mark->sent_saved;
    memcpy(app->last_sent, mark->last_sent, sizeof(app->last_sent));
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
static void clear_rows(PureApp *app) {
    const PureModule *m = app->module;
    unbind(app);
    for (uint32_t i = 0; i < m->states; ++i)
        if (m->state_type[i] == PURE_STATE_LIST && m->state_flags[i] & PURE_LIST_ROWS >> 8) pure_list_clear(app->lists[i]);
}

void pure_app_memory(PureApp *app, size_t bytes) { app->memory = bytes; }

void pure_app_work(PureApp *app, uint64_t instructions) {
    app->work_budget = instructions;
    for (unsigned i = 0; i < app->row_tables; ++i) app->rows[i].changes = UINT64_MAX; /* a filter refused before may pass */
}

PureFault pure_app_view_fault(const PureApp *app) { return app->view_fault; }

size_t pure_app_state_bytes(const PureApp *app) {
    size_t bytes = (app->outbox ? pure_list_size(app->outbox) : 0) + (app->heard ? pure_list_size(app->heard) : 0);
    for (uint32_t left = app->counted; left; left &= left - 1) {
        unsigned i = (unsigned)__builtin_ctz(left);
        bytes += app->lists[i] ? pure_list_size(app->lists[i]) : app->texts[i] ? pure_draft_size(app->texts[i]) : 0;
    }
    return bytes;
}

/* One event: every handler for it runs in module order against pending
 * states, all or nothing. A key, text or message event saves before it
 * returns unless saving is deferred; a frame leaves that to the next event
 * (frames come 60 times a second). A message's document joins the burst.
 * With a memory budget, an event that leaves the states over it, and
 * holding more than before, fails. */
static PureResult dispatch(PureApp *app, uint32_t event, Event *ev) {
    const PureModule *m = app->module;
    PureValue pending[PURE_MODULE_STATES], locals[PURE_MODULE_LOCALS] = {{0}};
    ev->locals = locals;
    size_t held = app->memory ? pure_app_state_bytes(app) : 0;
    /* What the event may add as it runs, whatever it gives back later: the
     * budget (which what it leaves is held to below), and in all what it
     * holds for it the default room, or the budget when that is larger. */
    app->grown = app->spent = 0;
    app->room = app->memory > PURE_GROWTH_DEFAULT ? app->memory : PURE_GROWTH_DEFAULT;
    for (unsigned i = 0; m->held && i < PURE_MODULE_HELD; ++i) app->held[i].size = 0;
    if (app->nested.count) unbind(app), drop_nested(app); /* a view's body bound some on the app itself */
    Mark mark;
    mark_event(app, &mark);
    memcpy(pending, app->values, sizeof(app->values));
    app->fault = PURE_FAULT_NONE;
    app->fault_pc = UINT32_MAX;
    app->work = 0;
    PureResult result = PURE_OK;
    for (uint32_t i = 0; i < m->handlers && result == PURE_OK; ++i)
        if (m->handler[i].event == event)
            result = charge(app, m->handler[i].code.length) ? run(m, pending, m->handler[i].code, ev, NULL)
                                                            : fault(app, PURE_FAULT_WORK, m->handler[i].code.start);
    /* An edit needs the revision it was made against. A save rebases an
     * edited document and releasing texts may reclaim earlier text versions:
     * keep such a reply by copying it first. */
    if (result == PURE_OK && ev->modified && ev->base < 0) result = PURE_INVALID;
    if (result == PURE_OK && app->has_reply &&
        ((app->reply.source == FROM_DOCUMENT && ev->modified) || app->reply.source == FROM_TEXT))
        result = keep_reply(app);
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
        clear_rows(app);
        return result;
    }
    clear_rows(app);
    /* No run of a list is open between events: one searched often since it
     * last changed is packed now (docs/design/022). */
    for (uint32_t i = 0; i < m->states; ++i)
        if (app->lists[i]) (void)pure_list_settle(app->lists[i]);
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
    app->delivering.pending = false;
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
    app->values[state].num = (double)position;
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
    return view < m->views && m->view_arguments[view] ? m->view_arguments[view] - 1 : -1;
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
    uint32_t spans[PURE_MODULE_FIELDS][2];
    PureFault wrong = place_fields(m, shape, payload, placed, spans);
    if (wrong != PURE_FAULT_NONE) return fault(app, wrong, UINT32_MAX);
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
    /* Its rows fill their lists (emptied when the event ends); rows that are not whole fail it. */
    for (uint32_t f = 0; f < shape->field_count; ++f) {
        const PureField *field = &m->field[shape->field_start + f];
        if (field->type != PURE_FIELD_ROWS) continue;
        PureResult filled = pure_list_read(app->lists[field->rows], payload.data + spans[f][0], spans[f][1]);
        if (filled == PURE_OK) continue;
        clear_rows(app);
        pure_app_finish(app);
        return filled == PURE_ALLOCATION ? filled : fault(app, PURE_FAULT_LIMIT, UINT32_MAX);
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
    if (input >= m->inputs || m->input[input].kind != PURE_KIND_MESSAGE || m->input[input].keyed || !length ||
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
    PureResult result = message(app, input, NULL, -1, payload, placed);
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
static bool own_lists(PureApp *app, uint32_t lists) {
    const PureModule *m = app->module;
    for (uint32_t left = lists; app->reading && left; left &= left - 1) {
        unsigned i = (unsigned)__builtin_ctz(left);
        if (!app->own[i]) {
            if (!(app->own[i] = new_list(m, m->state_ref[i]))) return false;
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
    PureValue values[PURE_MODULE_STATES], locals[PURE_MODULE_LOCALS];
    memcpy(values, app->values, sizeof(values));
    app->view_arena.used = 0;
    Event ev = {.app = app, .arena = &app->view_arena, .row = row};
    if (!argue(app, view, &ev)) return PURE_INVALID;
    if (m->view_body[view]) { /* a body computes: its locals from 0, its lists its own, its additions counted */
        memset(locals, 0, sizeof(locals));
        ev.locals = locals;
        if (!own_lists(app, m->view_lists[view])) return PURE_ALLOCATION;
        app->grown = app->spent = 0;
        app->room = app->memory > PURE_GROWTH_DEFAULT ? app->memory : PURE_GROWTH_DEFAULT;
        for (unsigned i = 0; m->held && i < PURE_MODULE_HELD; ++i) app->held[i].size = 0;
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
    for (uint32_t left = m->view_lists[view]; left; left &= left - 1) pure_list_clear(app->lists[__builtin_ctz(left)]);
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
    if (rows->changes == app->changes && (!m->view_arguments[table] || rows->given == app->arguments_given)) return rows;
    rows->count = 0;
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
    if (table >= m->views || m->view_type[table] != PURE_VIEW_TABLE) return false;
    if (!m->view_code[table].length) {
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
    if (view >= m->views || (m->view_type[view] == PURE_VIEW_BOOL ? PURE_VIEW_NUM : m->view_type[view]) != type ||
        (m->view_table[view] != 0) != column) { /* a bool's value is 0 or 1 */
        app->view_fault = PURE_FAULT_NONE;
        return PURE_INVALID;
    }
    if (column && !pure_app_row_item(app, m->view_table[view] - 1u, row, &row)) return PURE_NOT_FOUND; /* it says why */
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
    if (table >= m->views || m->view_type[table] != PURE_VIEW_TABLE) return 0;
    struct PureRows *rows = filtered(app, table);
    app->view_fault = rows ? rows->fault : PURE_FAULT_NONE;
    return rows ? rows->count : m->view_code[table].length ? 0 : pure_list_count(app->lists[m->view_list[table]]);
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
    if (returned.count) {
        PureList *lists[PURE_MODULE_STATES + 2];
        unsigned count = 0;
        for (uint32_t i = 0; i < app->module->states; ++i)
            if (app->lists[i]) lists[count++] = app->lists[i];
        lists[count++] = app->outbox;
        lists[count++] = app->heard;
        pure_retired_reuse(&returned, lists, count);
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
    PureSnapshot *made = calloc(1, sizeof(*made)), *last = app->snapshot;
    Register *all = last ? last->all : calloc(1, sizeof(*all));
    if (!made || !all) {
        free(made);
        if (!last) free(all);
        return NULL;
    }
    made->refs = 1;
    made->module = m;
    made->changes = app->changes;
    made->epoch = all->epoch + 1; /* the app's thread alone makes them */
    memcpy(made->values, app->values, sizeof(made->values));
    bool ok = true;
    for (uint32_t i = 0; i < m->states && ok; ++i) {
        /* A stored text is shared as its store holds it; a transient one, never stored, is copied. */
        if (app->texts[i])
            ok = (made->texts[i] = pure_state_transient(m, i) ? text_copy(app->texts[i]) : pure_draft_freeze(app->texts[i])) !=
                 NULL;
        if (!ok || !app->lists[i]) continue;
        if (last && last->lists[i] && pure_list_current(last->lists[i], app->lists[i])) {
            pure_list_hold(last->lists[i]);
            made->lists[i] = last->lists[i];
            pure_list_mark(app->lists[i], made->epoch);
        } else
            ok = (made->lists[i] = pure_list_share(app->lists[i], &app->retired, made->epoch)) != NULL;
    }
    if (ok && runtime >= PURE_LIST_HEARD && runtime < PURE_LISTS_STORED) {
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
    for (uint32_t i = 0; i < PURE_MODULE_STATES; ++i) {
        pure_list_free(app->own[i]);
        app->own[i] = app->unbound[i] = NULL;
    }
}

void pure_app_read(PureApp *reader, const PureSnapshot *snapshot) {
    if (reader->module != snapshot->module) drop_passes(reader), let_own_go(reader); /* laid out for its module */
    reader->module = snapshot->module;
    reader->reading = true;
    reader->changes = snapshot->changes; /* what the row cache is fresh for */
    memcpy(reader->values, snapshot->values, sizeof(reader->values));
    memcpy(reader->texts, snapshot->texts, sizeof(reader->texts));
    memcpy(reader->lists, snapshot->lists, sizeof(reader->lists));
}

uint8_t pure_record_kind(const PureModule *m, uint32_t field) {
    static const uint8_t kinds[] = {[PURE_RECORD_NUMBER] = PURE_CELL_NUMBER, [PURE_RECORD_TEXT] = PURE_CELL_TEXT,
                                    [PURE_RECORD_BOOL] = PURE_CELL_BOOL, [PURE_RECORD_I64] = PURE_CELL_INTEGER,
                                    [PURE_RECORD_LIST] = PURE_CELL_INTEGER}; /* a list in a record: its number */
    return kinds[m->record_field_type[field]];
}

/* A number, a bool and an i64 carry over each other (when the value fits); texts only to texts. */
static bool kinds_carry(uint8_t from, uint8_t to) {
    return from == to || (!pure_cell_text(from) && !pure_cell_text(to));
}

/* A list with record's fields: each takes the field of `from` with the same
 * name and a kind it carries over from, or its default. */
PURE_COLD PureResult pure_list_migrate(PureList **out, const PureList *from, const char *const *names,
                             const uint8_t *kinds, unsigned count, const PureModule *m, uint32_t record) {
    const PureRecord *r = &m->record[record];
    int source[PURE_RECORD_FIELDS];
    for (unsigned f = 0; f < r->count; ++f) {
        source[f] = -1;
        for (unsigned k = 0; k < count; ++k)
            if (!strcmp(m->record_field_names[r->first + f], names[k]) && kinds_carry(kinds[k], pure_record_kind(m, r->first + f)))
                source[f] = (int)k;
    }
    if (!(*out = new_list(m, record))) return PURE_ALLOCATION;
    PureResult result = PURE_OK;
    for (size_t at = 0; result == PURE_OK && at < pure_list_count(from); ++at) {
        PureCell cells[PURE_RECORD_FIELDS];
        for (unsigned f = 0; f < r->count; ++f) {
            uint32_t field = r->first + f, constant = (uint32_t)m->record_field_default[field].num;
            PureCell given = source[f] >= 0 ? pure_list_get(from, at, (unsigned)source[f]) : (PureCell){0};
            if (source[f] >= 0 && pure_cell_as(kinds[source[f]], pure_record_kind(m, field), &given)) cells[f] = given;
            else if (m->record_field_type[field] == PURE_RECORD_TEXT)
                cells[f] = (PureCell){.text = (unsigned char *)m->constant_data + m->constant_offset[constant],
                                      .size = m->constant_length[constant]};
            else cells[f] = (PureCell){.i64 = m->record_field_default[field].i64}; /* a number's, a bool's or an i64's bits */
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
    uint8_t kinds[PURE_RECORD_FIELDS];
    bool same = r->count == o->count;
    for (unsigned k = 0; k < o->count; ++k) {
        names[k] = old->record_field_names[o->first + k];
        kinds[k] = pure_record_kind(old, o->first + k);
        same = same && !strcmp(names[k], m->record_field_names[r->first + k]) && kinds[k] == pure_record_kind(m, r->first + k);
    }
    if (same) { *out = from; return PURE_OK; }
    return pure_list_migrate(out, from, names, kinds, o->count, m, record);
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
PURE_COLD static PureResult remap_outputs(PureApp *app, const PureModule *old, const PureModule *module, bool apply,
                                          PureList **recast, bool *changed) {
    int map[PURE_MODULE_INPUTS];
    bool differs[PURE_MODULE_INPUTS] = {false}, recasting = false;
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
                return PURE_CONFLICT;
            }
            recasting = recasting || differs[i];
        }
        if (!recasting) return PURE_OK;
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
        if (result != PURE_OK) {
            pure_list_free(made);
            return result;
        }
        *recast = made;
        return PURE_OK;
    }
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
    memcpy(app->last_sent, last, sizeof(last));
    memcpy(app->saved_sent, saved, sizeof(saved));
    return PURE_OK;
}

PURE_COLD static PureResult open_module(PureApp *app, const PureModule *module) {
    const PureModule *old = app->module;
    bool stored = pure_save_enabled(app);
    PureValue values[PURE_MODULE_STATES] = {0};
    PureDraft *texts[PURE_MODULE_STATES] = {0}, *list_drafts[PURE_MODULE_STATES] = {0};
    PureList *lists[PURE_MODULE_STATES] = {0};
    PureVersion saved[PURE_MODULE_STATES] = {{0, 0}};
    PureSaved listed[PURE_MODULE_STATES] = {{0}};
    int kept[PURE_MODULE_STATES];
    bool rewrite[PURE_MODULE_STATES] = {false};
    /* Lists are stored where the host names a directory for them (module-save.c). */
    bool lists_stored = stored && app->data.directory && app->data.directory(app->data.context);
    app->why[0] = 0;
    if (old && app->checkpoint) return PURE_CONFLICT; /* the host has a checkpoint of the running module out */
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
        kept[i] = previous >= 0 && (was == now || valued) ? previous : -1;
        if (kept[i] >= 0) saved[i] = app->saved[kept[i]];
        /* A transient state is never stored (018): one that was stored is now as a dropped one is. */
        bool transient = pure_state_transient(module, i), was_transient = kept[i] >= 0 && pure_state_transient(old, kept[i]);
        if (module->state_type[i] == PURE_STATE_TEXT) {
            if (kept[i] >= 0 && (transient || !was_transient || !stored)) texts[i] = app->texts[kept[i]];
            else if (result == PURE_OK && kept[i] >= 0 && /* a text stored from now on: its draft in the store holds it */
                     (result = pure_save_open_text(app, module, i, &texts[i], &saved[i])) == PURE_OK)
                result = copy_text(texts[i], app->texts[kept[i]], &saved[i]);
            else if (result == PURE_OK)
                result = stored && !transient ? pure_save_open_text(app, module, i, &texts[i], &saved[i]) :
                         new_text(&texts[i], module, module->state_ref[i]);
        } else if (module->state_type[i] == PURE_STATE_LIST) {
            if (result != PURE_OK) continue;
            bool rows = module->state_flags[i] & PURE_LIST_ROWS >> 8; /* rows are never stored */
            bool saving = lists_stored && !rows && !transient;
            if (kept[i] >= 0) {
                result = keep_list(&lists[i], app->lists[kept[i]], old, old->state_ref[kept[i]], module,
                                   module->state_ref[i]);
                list_drafts[i] = app->list_drafts[kept[i]];
                if (!transient && !was_transient)
                    listed[i] = (PureSaved){app->image_seq[kept[i]], app->logged_since[kept[i]], app->logged[kept[i]], false};
                /* Its record changed, or it is stored from now on: its image, in the new fields, holds it before any change is logged. */
                rewrite[i] = result == PURE_OK && saving && (lists[i] != app->lists[kept[i]] || was_transient);
            } else if (saving) {
                result = pure_save_open_list(app, module, i, &lists[i], &list_drafts[i], &listed[i]);
                rewrite[i] = result == PURE_OK && listed[i].rewrite;
            } else if (!(lists[i] = new_list(module, module->state_ref[i]))) result = PURE_ALLOCATION;
        } else if (kept[i] >= 0 && valued) {
            PureCell cell = {.i64 = app->values[kept[i]].i64};
            values[i] = pure_cell_as(pure_value_kind(was), pure_value_kind(now), &cell) ? (PureValue){.i64 = cell.i64}
                                                                                     : module->initial[i];
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
        result = old ? remap_outputs(app, old, module, false, &recast, &outputs_changed) :
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
    for (uint32_t i = 0; i < PURE_MODULE_STATES; ++i) {
        app->image_seq[i] = listed[i].image;
        app->logged_since[i] = listed[i].since;
        app->logged[i] = listed[i].logged;
    }
    if (!old) {
        app->image_seq[PURE_LIST_HEARD] = heard.image;
        app->logged_since[PURE_LIST_HEARD] = heard.since;
        app->logged[PURE_LIST_HEARD] = heard.logged;
        app->image_seq[PURE_LIST_OUTBOX] = sent.image;
        app->logged_since[PURE_LIST_OUTBOX] = sent.since;
        app->logged[PURE_LIST_OUTBOX] = sent.logged;
        if (lists_stored) pure_list_journal(app->heard, true), pure_list_journal(app->outbox, true);
    }
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
    if (old) remap_outputs(app, old, module, true, NULL, NULL);
    if (recast) { /* in the place of the outbox, its sends now the new module's */
        pure_list_free(app->outbox);
        app->outbox = recast;
        app->durable_outbox = 0;
        if (lists_stored) pure_list_journal(recast, true);
    }
    if (outbox_imaged) {
        app->image_seq[PURE_LIST_OUTBOX] = app->seq;
        app->logged_since[PURE_LIST_OUTBOX] = 0;
        app->logged[PURE_LIST_OUTBOX] = 0;
    }
    drop_passes(app); /* kept by instruction of the module they were made for */
    app->module = module;
    app->counted = 0;
    for (uint32_t i = 0; i < module->states; ++i)
        if (module->state_type[i] == PURE_STATE_TEXT ||
            (module->state_type[i] == PURE_STATE_LIST && !(module->state_flags[i] & PURE_LIST_ROWS >> 8)))
            app->counted |= UINT32_C(1) << i; /* a message's rows are its own, gone when it ends */
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
    pure_app_finish(app);
    if (app->open || app->pending_count) {
        PureResult saved = pure_app_save(app); /* the burst belongs to the module that made it */
        if (saved != PURE_OK) return saved;
    }
    return open_module(app, module);
}
