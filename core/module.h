#ifndef PURE_MODULE_H
#define PURE_MODULE_H
#include "flow.h"
#include "result.h"
#include "module-list.h"
#include "state.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Pure module format v7 (docs/design/008-module-format.md). A toolchain such
 * as aup produces it; Pure verifies it completely before anything executes:
 * operand types (including which text a cursor and which list an index
 * belongs to), stack heights and the stack at every join, forward-only
 * branches, names, records, tables, constants and effects. */
enum {
    PURE_MODULE_CODE = 65535, PURE_MODULE_STACK = 64,
    PURE_MODULE_NAME = 32, PURE_MODULE_STRINGS = 32768, PURE_MODULE_HEADER = 38,
    /* A message's fields at most: a send pushes them all (the stack's), a width of the stored sends too (027's 4f-3a). */
    PURE_MESSAGE_FIELDS = PURE_MODULE_STACK, PURE_MODULE_CONSTANTS = 4096,
    PURE_MODULE_CONSTANT_BYTES = 1 << 20,
    /* A record's fields at most: a list's leaf keeps 16 columns (module-list.c, its runs and arrays too). */
    PURE_RECORD_FIELDS = 16, PURE_MODULE_JOINS = 8
};

typedef enum {
    PURE_OP_CONST, PURE_OP_LOAD, PURE_OP_ADD, PURE_OP_SUB, PURE_OP_MUL,
    PURE_OP_DIV, PURE_OP_NEG, PURE_OP_LT, PURE_OP_LE, PURE_OP_GT, PURE_OP_GE,
    PURE_OP_EQ, PURE_OP_NE, PURE_OP_AND, PURE_OP_OR, PURE_OP_NOT, PURE_OP_MIN,
    PURE_OP_MAX, PURE_OP_CLAMP, PURE_OP_STORE, PURE_OP_JZ, PURE_OP_JMP,
    PURE_OP_FIELD, PURE_OP_LEN,
    PURE_OP_REPLY, PURE_OP_REQUIRE,
    PURE_OP_TEXT_CONST, PURE_OP_TEXT_LOAD, PURE_OP_NUM_TEXT,
    PURE_OP_CONCAT, PURE_OP_CHARS, PURE_OP_LINES, PURE_OP_TEXT_BYTES,
    PURE_OP_ANCHOR_LOAD, PURE_OP_ANCHOR_START, PURE_OP_ANCHOR_END, PURE_OP_ANCHOR_MOVE,
    PURE_OP_ANCHOR_STORE, PURE_OP_TEXT_INSERT, PURE_OP_TEXT_REMOVE, PURE_OP_TEXT_LINE,
    PURE_OP_TEXT_SET, PURE_OP_TEXT_FROM, PURE_OP_BYTES_FROM, PURE_OP_ANCHOR_COUNT,
    PURE_OP_LIST_COUNT, PURE_OP_LIST_APPEND, PURE_OP_LIST_INSERT, PURE_OP_LIST_REMOVE,
    PURE_OP_ITEM_GET, PURE_OP_ITEM_SET, PURE_OP_ROW,
    PURE_OP_PICK, PURE_OP_SLIDE, PURE_OP_FOLD, PURE_OP_TEXT_COMPARE, PURE_OP_LIST_FIND, PURE_OP_LIST_HAS,
    PURE_OP_NOW, PURE_OP_FLOOR, PURE_OP_MOD, PURE_OP_SEND, PURE_OP_OUTER_ROW, PURE_OP_EACH,
    PURE_OP_LOCAL_LOAD, PURE_OP_LOCAL_STORE,
    PURE_OP_I64_ADD, PURE_OP_I64_SUB, PURE_OP_I64_MUL, PURE_OP_I64_DIV, PURE_OP_I64_MOD, PURE_OP_I64_NEG,
    PURE_OP_I64_COMPARE, PURE_OP_I64_MIN, PURE_OP_I64_MAX, PURE_OP_I64_FROM, PURE_OP_NUM_FROM,
    PURE_OP_LOOP, PURE_OP_BREAK,
    PURE_OP_I64_BITS, PURE_OP_BYTE_AT, PURE_OP_BYTES_OF, PURE_OP_ANCHOR_AT,
    PURE_OP_BYTES_SLICE, PURE_OP_BYTE, PURE_OP_BYTES_LOAD, PURE_OP_BYTES_STORE,
    PURE_OP_LIST_VALUE, PURE_OP_LIST_BIND, PURE_OP_APPEND_IN, PURE_OP_NUM_PARSE,
    PURE_OP_CALL, PURE_OP_RETURN, PURE_OP_CHAR_AT, PURE_OP_CHAR,
    PURE_OP_FLOORDIV,
    PURE_OP_COUNT
} PureOpcode;
/* 027's 4c: an anchor is a place in a text or a list that follows its edits. A text's (a cursor) is a gap between
 * characters: what is inserted at it goes before it (a mark: after it), and in a removed range it goes to the range's
 * start. A list's (an index) is the gap before an item, naming it: what is inserted at it goes before it (it follows its
 * item); it names the next item when its item goes, the last one past the end (0 in an empty list). The instructions
 * name what their anchor is in: LOAD and STORE an anchor state; START and END a text (its start and end) or a list (its
 * first and last item); MOVE, COUNT and AT the text or list in bits 0-15 and a unit in bits 16-23 (a text's: 0 a
 * character, 1 a line; a list's: 0, an item). MOVE moves by a whole number of units (else LIMIT), stopping at the ends;
 * COUNT gives the units before it (lines from 0); AT gives the anchor a whole number of units from the start (else
 * LIMIT), NOT_FOUND past the end. The load holds each as the runtime's own instruction for a cursor or an index
 * (module-run.h). */
enum { PURE_ANCHOR_UNIT = 16 };
/* A bool is its own type (docs/design/016-bool-and-i64.md), held as a
 * number 0 or 1 while it runs: comparisons (LT to NE, TEXT_COMPARE) and
 * LIST_HAS push one, AND, OR and NOT work on them, JZ and REQUIRE take one,
 * and EQ and NE compare two bools as they compare two numbers. CONST with
 * argument 1 pushes a bool (its value 0 or 1); LOAD and STORE work on a
 * number or a bool state as its type says; NUM_TEXT with argument 1 makes
 * a bool text (true, false).
 * An i64 is its own type too, an int64_t in a value's 8 bytes: CONST with
 * argument 2 pushes one (its constant's 8 bytes), NUM_TEXT with argument 2
 * makes its decimal text, the I64_ opcodes compute on them (ADD, SUB, MUL
 * and NEG fail as OVERFLOW past its range; DIV and MOD floor, and fail as
 * DIVIDE by zero; COMPARE takes a relation as TEXT_COMPARE does and gives a
 * bool), I64_FROM makes one of a whole number in range and NUM_FROM a
 * number of one within 2^53 (else LIMIT). LOAD, STORE, the item and local
 * opcodes, FIELD and SEND move them as they move numbers.
 * I64_BITS computes on two i64s as on integers in two's complement without
 * end (docs/design/027): its argument says how (PureBits). AND, OR and XOR
 * work bit by bit; LEFT is a * 2^b, failing as OVERFLOW past the range;
 * RIGHT is floor(a / 2^b); a shift by b outside 0-63 fails as LIMIT.
 * BYTE_AT gives the byte of a bytes value at a position (argument 0: a
 * number, 1: an i64) as an i64 0-255, failing as LIMIT where there is none
 * (not a whole number, or outside the value). BYTES_OF makes the bytes of a
 * list state whose record is one i64 field, an item a byte (a value outside
 * 0-255 fails as LIMIT). INDEX_AT makes the index of a list state's item at
 * a position (a number), failing as LIMIT if it is not a whole number and
 * as NOT_FOUND where there is no item. */
typedef enum { PURE_BITS_AND, PURE_BITS_OR, PURE_BITS_XOR, PURE_BITS_LEFT, PURE_BITS_RIGHT } PureBits;
/* 027's stage 2a. CONCAT with argument 1 joins two bytes values (0: two
 * texts). BYTES_SLICE takes a bytes value and two positions a <= c (numbers,
 * or i64s with argument 1) and gives its bytes from a up to before c, failing
 * as LIMIT unless both are whole and within it. BYTE makes one byte of an
 * i64 0-255 (else LIMIT). BYTES_LOAD and BYTES_STORE read and set one of an
 * event's PURE_MODULE_HELD values held by name (a let), empty when the event
 * or a view's body begins: the argument's bits 0-4 name it, a text below 16
 * bytes and from 16 on a text (so a text one holds UTF-8 alone); STORE with
 * bit 8 appends the value to what it holds. What they hold counts in the
 * event's room (MEMORY past it).
 * NUM_TEXT's argument: bits 0-1 the value's type (as CONST's), 8-15 a width
 * the text is padded to on the left, bit 16 padding with zeros after a sign
 * (else spaces), 17-19 the form (PureFormat), 20-24 one more than the digits
 * after the point of FIXED or of DECIMAL (there the integer in minor units:
 * 1234 at two digits is 12.34, as money's cents). A form of an integer takes
 * an i64 or a whole number (else LIMIT), in its base with a sign; FIXED
 * takes a number. */
enum { PURE_MODULE_HELD = 32, PURE_HELD_TEXT = 16, PURE_HELD_APPEND = 1 << 8 };
/* 027's stage 2b: lists in records. A record field of type LIST holds a list
 * of the record its default names. In an event's list (and the lists in its
 * items) the value is the list's number among the event's lists made so (0:
 * an empty one), an i64 in its cell, so moves and reads (ITEM_GET) are an
 * integer's; such lists go as the event or a view's body ends. A list is a
 * value: storing one in a second cell shares it, and a list shared or bound
 * is copied before it changes (so no other value sees the change). A
 * message's rows may hold lists (027's 4a): each comes in as one of the
 * event's lists, read from its stored form (below) before the handler runs.
 * Since stage 3 a stored or a transient list's records may hold lists too
 * (never as a key): there the cell holds the value in its
 * items' stored form (PURE_CELL_LIST), read into one of the event's lists
 * as ITEM_GET reads it and written from one as it is set (LIST_APPEND,
 * LIST_INSERT, ITEM_SET; APPEND_IN reads, appends and writes back), so
 * saving, undoing and sharing with readers hold it as a text's bytes.
 * LIST_VALUE pushes the value of an event's list state (the argument): a
 * copy of its items. LIST_BIND gives the bound list state (a register: an
 * event's list flagged PURE_LIST_BOUND, read and walked, never changed by
 * code) the list on the stack. APPEND_IN appends an item (its fields on the
 * stack after an index) to the list in field (bits 16-31) of the item the
 * index names in list (bits 0-15), an event's list. NUM_PARSE reads a text
 * written as a JSON number as the nearest number (failing as LIMIT on any
 * other text, or a value past the finite ones). */
typedef enum { PURE_FORMAT_PLAIN, PURE_FORMAT_DECIMAL, PURE_FORMAT_HEX, PURE_FORMAT_HEX_UPPER, PURE_FORMAT_BINARY,
               PURE_FORMAT_OCTAL, PURE_FORMAT_FIXED } PureFormat;
enum { PURE_FORMAT_WIDTH = 8, PURE_FORMAT_ZEROS = 1 << 16, PURE_FORMAT_FORM = 17, PURE_FORMAT_DIGITS = 20 };
/* 027's stage 2c: functions and characters. A function (PureFunction) is a
 * block CALL runs (its argument the function's number): the block begins with
 * the call's arguments as its stack, its parameters' types, and RETURN ends
 * it with its results on top (the rest of its stack let go), which take the
 * arguments' place on the caller's stack. Each call has its own stack and
 * locals (0 as it begins), and its own of what the function's code names:
 * values held by name (empty), an event's lists (empty) and registers (bound
 * to none); the caller's are as they were when the call returns. A function
 * runs as a handler's code does but for a message (no field or reply); a
 * handler and the functions run CALL (not a view), and a walk around
 * a call keeps to what the callee changes as it keeps to its own block's
 * changes. Calls nest as deep as the event's room allows: each counts its
 * frame's bytes there (MEMORY past it) and its block's length as work.
 * Outside a function, LOOP and EACH take their operands from a stack that
 * holds nothing else: a loop's block begins at the bottom of its run's stack.
 * CHAR_AT gives the character at a byte position of a text (an i64: its code
 * point), failing as LIMIT where no character begins there; CHAR makes the
 * text of a code point, failing as LIMIT outside 0-0x10FFFF and as ENCODING
 * for a surrogate. */
enum { PURE_MODULE_FUNCTIONS = 64, PURE_MODULE_SIGNATURES = 1024 };
/* What a call's frame counts in the event's room: a table, as gas is, not the implementation's sizes (each at least
 * what it stands for), so a module's calls fail at the same depth wherever and on whatever build they run: so much a
 * frame, a value of its stack's height, a local, a loop it nests, a list of its own, a held value and a register. */
enum { PURE_CALL_BYTES = 256, PURE_CALL_VALUE = 64, PURE_CALL_LOCAL = 8, PURE_CALL_LOOP = 128, PURE_CALL_LIST = 1024,
       PURE_CALL_HELD = 128, PURE_CALL_REGISTER = 16 };
/* A local: one of an event's PURE_MODULE_LOCALS slots, 0 when the event
 * begins and never saved. LOCAL_LOAD and LOCAL_STORE name the slot (bits
 * 0-7), what it holds (bits 8-15: 0 a number, 1 an index, 2 a bool, 3 an i64,
 * 4 a list value, 027's 2c) and an index's list or a list's record (bits
 * 16-31); a handler uses a slot for one of them. A handler, the
 * walks in it and the folds in those read locals; the handler and its walks
 * store them. */
enum { PURE_MODULE_LOCALS = 64 };
/* EACH's argument: the list state (bits 8-15) and the length of the block
 * that follows (16-31), run in a handler once for every item the list had
 * when it began, in order, with ROW the item; the block may change states,
 * the list's items and other lists, but not insert into the list it walks
 * (an item it removes, or one before it, moves the walk with it). */
/* FOLD's argument: the kind (bits 0-5, with flags above: an i64's, a
 * slice's), the list state (8-15) and the length of the block that follows
 * (16-31), run once per item as a column of that list. count counts the
 * items whose block leaves true; sum, max, min and distinct take a value
 * and whether to include it (a bool) from their block, the value a number
 * or, with PURE_FOLD_INTEGER, an i64 (a sum past its range fails as
 * OVERFLOW; distinct counts the different values, -0 as 0); first gives the
 * index of the first item whose block leaves true. */
typedef enum { PURE_FOLD_COUNT, PURE_FOLD_SUM, PURE_FOLD_MAX, PURE_FOLD_MIN, PURE_FOLD_FIRST, PURE_FOLD_DISTINCT } PureFold;
enum { PURE_FOLD_KIND = 0x3F, PURE_FOLD_INTEGER = 0x40 };
/* A slice: a fold whose kind has PURE_FOLD_SLICED, or a walk whose argument
 * has PURE_EACH_SLICED, first takes two numbers, a low and a high key, and
 * runs for the items of its list (keyed by a number) whose keys are from
 * low up to before high, found by key. */
enum { PURE_FOLD_SLICED = 0x80, PURE_EACH_SLICED = 1 };
/* LOOP's argument: a local slot (bits 0-7), PURE_LOOP_INTEGER (an i64's
 * loop, else a number's) and the length of the block that follows (16-31).
 * It takes a low and a high value, both whole numbers within 2^53 (else it
 * fails as LIMIT) or both i64s, and runs the block once for each value from
 * low up to before high, in order, with the slot holding it; none when high
 * is not above low. The block is a handler's code (or a walk's, in one),
 * and its work counts as it runs: the block's length each time. BREAK, at
 * an empty stack in the block of a LOOP or an EACH (in an if within it
 * too), ends the innermost of them: the code after it runs next.
 * Loops (LOOP and EACH) nest 8 deep. Within a walk, code that a loop runs
 * may change the items of any list but not add to or remove from a list an
 * outer walk walks; the innermost walk may remove from its own. */
/* PURE_MODULE_NESTING: the loops nested that a run keeps on its stack; a module nesting deeper (027's 4f-4) runs in the
 * executor that calls, its loops in its app's room (PureApp.deep). */
enum { PURE_LOOP_INTEGER = 256, PURE_MODULE_NESTING = 8 };
/* What a unit (an event, or a view's evaluation) may run when its app sets
 * no work budget (pure_app_work), and what an event may hold for what it
 * adds (its lists' and texts' bytes, and their records to save and undo)
 * unless a larger memory budget allows more: 2^28 instructions, 2^28 bytes.
 * With a budget, what an event adds (as stored) fails it once it passes the
 * budget, as it adds it. */
#define PURE_WORK_DEFAULT (UINT64_C(1) << 28)
#define PURE_GROWTH_DEFAULT ((size_t)1 << 28)
/* TEXT_COMPARE's argument: the relation, in the order of the number
 * comparisons (LT LE GT GE EQ NE), and PURE_COMPARE_BYTES with it when its
 * two are bytes (texts and bytes alike order by their bytes). */
typedef enum { PURE_LESS, PURE_LESS_EQUAL, PURE_GREATER, PURE_GREATER_EQUAL, PURE_EQUAL, PURE_NOT_EQUAL } PureRelation;
#define PURE_COMPARE_BYTES 0x100u

/* An input is a message (027's 4d: the only thing an app takes), an output
 * or a view's arguments. An output is a message the app sends (SEND, in a
 * handler; it has fields as a message has and no handler): what receives it
 * is the host's wiring. Arguments are the fields a view takes (a view with
 * arguments, its columns and its filter read them as a message handler reads
 * its message); such an input has no handler and names its view, whose name
 * it bears with .args after it. */
typedef enum { PURE_KIND_MESSAGE = 1, PURE_KIND_OUTPUT = 2, PURE_KIND_ARGUMENTS = 3 } PureInputKind;
/* What makes a message, for its hosts and surfaces (027's 4d): nothing said
 * (a sender's), a key pressed or released (the key's name: no fields), text
 * that came in (one text field), a timer's tick every period milliseconds
 * (no fields: the time is the event's now), a frame of the screen (one
 * number field, the seconds since the last) or a pointing at a row of a
 * table view (one u32 field, the row). The runtime reads none of it but the
 * fields it says a message has. */
typedef enum { PURE_BIND_NONE, PURE_BIND_PRESS, PURE_BIND_RELEASE, PURE_BIND_TEXT, PURE_BIND_TIMER, PURE_BIND_FRAME,
               PURE_BIND_POINT } PureBinding;
enum { PURE_TIMER_LONGEST = 86400000, PURE_SOURCE = 64 };
/* The largest number a delivery may have: it is kept as a binary64. */
#define PURE_SEQ_MOST (UINT64_C(1) << 53)
/* A message field: a u32le, a binary64, a bool's one byte or an i64's eight
 * (fixed, at its offset in the payload's prefix), or bytes, text or rows of any length. The
 * prefix also holds each variable field's u32le length but the last one's
 * (its offset is PURE_FIELD_REST), which takes what follows the others; the
 * variable fields' bytes follow the prefix in declaration order. A text is
 * UTF-8, a number finite and a bool 0 or 1, or the message fails. Rows are
 * records of a list of a message's rows (rows, a list state), laid out as a
 * list's stored form: a message fills that list with them, and fails when
 * they are not whole. */
typedef enum { PURE_FIELD_U32 = 1, PURE_FIELD_BYTES = 2, PURE_FIELD_NUMBER = 3, PURE_FIELD_TEXT = 4,
               PURE_FIELD_ROWS = 5, PURE_FIELD_BOOL = 6, PURE_FIELD_I64 = 7 } PureFieldType;
#define PURE_FIELD_REST UINT32_MAX /* a macro: an enum's values are ints (C17) */
/* A field held in its payload's prefix (a u32, a number, a bool, an i64), and the bytes it takes there. */
static inline bool pure_field_fixed(uint8_t type) {
    return type == PURE_FIELD_U32 || type == PURE_FIELD_NUMBER || type == PURE_FIELD_BOOL || type == PURE_FIELD_I64;
}
static inline uint32_t pure_field_size(uint8_t type) {
    return type == PURE_FIELD_NUMBER || type == PURE_FIELD_I64 ? 8 : type == PURE_FIELD_BOOL ? 1 : 4;
}
/* A loaded module's states: a module's anchor state (027's 4c: PURE_MODULE_ANCHOR, type 2) is a cursor when it is in a
 * text and an index when it is in a list; the rest are the module's types, a bool's and an i64's 4 and 5 in a module. */
typedef enum {
    PURE_STATE_NUM = 0, PURE_STATE_TEXT = 1, PURE_STATE_CURSOR = 2, PURE_STATE_LIST = 3, PURE_STATE_INDEX = 4,
    PURE_STATE_BOOL = 5, PURE_STATE_I64 = 6
} PureStateType;
/* A state's type in a module: a number, a text, an anchor, a list, a bool, an i64 or bytes (028: held as a text is, its
 * flag PURE_TEXT_BYTES, UTF-8 asked of none of it). */
enum { PURE_MODULE_NUM, PURE_MODULE_TEXT, PURE_MODULE_ANCHOR, PURE_MODULE_LIST, PURE_MODULE_BOOL, PURE_MODULE_I64,
       PURE_MODULE_BYTES };
#define PURE_TEXT_BYTES 1u /* a text state's flag (state_flags): its type is bytes (028) */
/* A number's, a bool's or an i64's state as a list cell's kind (PURE_CELL_...,
 * for pure_cell_as), or 0xFF for any other state. */
static inline uint8_t pure_value_kind(uint8_t state_type) {
    return state_type == PURE_STATE_NUM ? PURE_CELL_NUMBER : state_type == PURE_STATE_BOOL ? PURE_CELL_BOOL :
           state_type == PURE_STATE_I64 ? PURE_CELL_INTEGER : 0xFF;
}
/* A record field: a number, a text (its default a constant), a bool, an i64, a list (027's 2b) or bytes (4b: its
 * default a constant too, held as a text is but for UTF-8). */
typedef enum { PURE_RECORD_NUMBER = 0, PURE_RECORD_TEXT = 1, PURE_RECORD_BOOL = 2, PURE_RECORD_I64 = 3, PURE_RECORD_LIST = 4,
               PURE_RECORD_BYTES = 5 } PureRecordType;
/* A field held as bytes are: a text or bytes. */
static inline bool pure_record_bytes(uint8_t type) { return type == PURE_RECORD_TEXT || type == PURE_RECORD_BYTES; }
/* A value's 8 bytes: a number (a bool's 0 or 1, a cursor's or an index's
 * position too), or an integer. What a value is its state's or field's type
 * says, and the verifier keeps to it: moves copy the bytes, and only what
 * computes reads them as one or the other. */
typedef union { double num; int64_t i64; } PureValue;
/* A cursor that stays before text inserted at it (a mark); other cursors go
 * after it (a caret). An event's list (its state's flags hold
 * PURE_LIST_ROWS beside the key byte; its name a path, input.field or
 * let.name): it is emptied when every event ends, and it is never saved
 * nor logged for undoing. A message's rows field fills the one its path
 * names (unkeyed) as the message arrives; code fills a let's (docs/design/018). */
enum { PURE_CURSOR_STAYS = 1, PURE_LIST_ROWS = 256, PURE_LIST_BOUND = 512 };
/* A transient state (bit 8 of its type word in a module, a bit of
 * PureModule.transient): the app holds it while it runs, and undoes it with
 * a failed event, but never saves it: each start begins it at its initial
 * value (docs/design/018). A stored cursor or index is of a stored text or
 * list. */
enum { PURE_STATE_TRANSIENT = 256 };
/* A view's type is a record field's (027's 4e). A list view's rows are a
 * list's items: a list state's (its filter's, when it has one), or those of
 * the list its code leaves (computed once a change, kept for its rows and
 * cells until the next). Its columns are its record's fields, read from the
 * items as they are, or for a list state's map (a view of no record) the
 * views that follow it, each evaluated for one row at a time. A list cell
 * is lent in its items' stored form (pure_app_cell_items). A view outside
 * a list may compute (docs/design/018): its code may hold LOOP, EACH, BREAK
 * and locals, and change an event's lists, but no state; each evaluation
 * begins with its locals 0 and those lists empty (a reader's own: readers
 * of one snapshot compute apart). */
typedef enum { PURE_VIEW_NUM = PURE_RECORD_NUMBER, PURE_VIEW_TEXT = PURE_RECORD_TEXT, PURE_VIEW_BOOL = PURE_RECORD_BOOL,
               PURE_VIEW_I64 = PURE_RECORD_I64, PURE_VIEW_LIST = PURE_RECORD_LIST, PURE_VIEW_BYTES = PURE_RECORD_BYTES } PureViewType;
typedef enum { PURE_UNIT_CHAR = 0, PURE_UNIT_LINE = 1 } PureUnit;
/* EXISTS: an item with that key is already in the keyed list (the event
 * reports PURE_CONFLICT). MEMORY: the event would leave the states holding
 * more than the app's budget (pure_app_memory; it reports PURE_BUDGET).
 * OVERFLOW: an i64 past its range. WORK: the event (or a view's evaluation)
 * would run more than the app's work budget (pure_app_work; PURE_LIMIT). */
typedef enum {
    PURE_FAULT_NONE, PURE_FAULT_DIVIDE, PURE_FAULT_NON_FINITE, PURE_FAULT_CLAMP,
    PURE_FAULT_LIMIT, PURE_FAULT_NOT_FOUND, PURE_FAULT_ENCODING, PURE_FAULT_EXISTS,
    PURE_FAULT_MEMORY, PURE_FAULT_OVERFLOW, PURE_FAULT_WORK, PURE_FAULT_COUNT
} PureFault;
/* A fault's name as hosts report it: none, divide, non_finite, clamp, limit,
 * not_found, encoding, exists, memory, overflow, work. */
const char *pure_fault_name(PureFault);

/* plan, shape and range are the runtime's, set at load: a fold whose block
 * has a form for every instruction that works on a leaf of its list at once
 * runs so (plan; for an item's field, or a keyed list's key, read or set:
 * whether it is a text), one in a column may be answered for every row from one
 * pass over its list (shape: its PureShape + 1), and a fold or a walk whose
 * test bounds a keyed list's key reads only the items in bounds (range: its
 * PureRange + 1). A LIST_VALUE's plan 1: nothing changes its list after it
 * (030's N4), so the value is that list itself, lent. */
typedef struct { union { double value; int64_t integer; }; uint32_t arg; uint8_t op, plan, shape, range; } PureOp;
typedef struct { uint32_t start, length; } PureBlock;
typedef struct { uint32_t event; PureBlock code; } PureHandler; /* event: the message input it handles (027's 4d) */
/* A function's block and its parameters' and results' types, from first among the module's signature items: a record
 * field's type (PureRecordType: a list's with its record, bytes as PURE_RECORD_BYTES). Set at load: the event's lists,
 * registers and held values each call has its own of (a bit each), the lists not an event's it changes (or a function
 * it calls does), and its frame: its stack's height at most, its locals and its loops. */
typedef struct {
    PureBlock code;
    uint16_t first;
    uint8_t params, results;
    uint32_t held;
    uint8_t height, locals;
    uint32_t nesting;
    uint32_t own_lists, own_registers; /* how many of each its calls keep of the caller's (set at load) */
} PureFunction;
typedef struct { char name[PURE_MODULE_NAME]; uint8_t type; uint16_t rows; uint32_t offset; } PureField;
typedef struct {
    uint8_t kind, binding; /* PureInputKind, PureBinding (a key's name in input_keys) */
    uint16_t field_start, field_count;
    uint32_t prefix; /* fixed payload bytes before the variable fields */
    uint32_t period; /* a timer's, in milliseconds */
    uint32_t view;   /* arguments: the view they are for; a pointing's: its table */
} PureInputShape;

/* A record type: fields numbered from first, each a number or text. */
typedef struct { uint16_t first, count; } PureRecord;

/* A fold in a column whose test compares a field of its item with a key the
 * row gives (key, a block reading no item; test, the comparison), the rest
 * of it reading the item alone: a group (the field equals the key: one pass
 * folds every group) or a rank (a count of items whose field orders against
 * the key: one sort answers every key). relation is the comparison's
 * opcode with the item on its left. */
enum { PURE_SHAPE_GROUP = 1, PURE_SHAPE_RANK = 2, PURE_MODULE_SHAPES = 64 };
typedef struct { uint8_t form, relation; uint16_t field; uint32_t test; PureBlock key; } PureShape;

/* A test (a fold's, a walk's where, a filter) that ANDs comparisons of a
 * keyed list's item key with values reading no item: lower (the key GT or
 * GE it) and upper (LT or LE; a relation of 0: none; EQ is both). Only the
 * items with keys in bounds can pass it, so only those are read: a filter
 * leaves the rest out anyway, and a fold's or walk's other parts cannot
 * fault (what the skipped items would have run). */
enum { PURE_MODULE_RANGES = 64 };
/* whole: the test is those comparisons alone (an item in bounds passes it): a count is the range's length, a first its
 * first (030's step 1: the tree's order holds what a pass would find). */
typedef struct { uint8_t lower_relation, upper_relation, whole; PureBlock lower, upper; } PureRange;


typedef struct {
    uint16_t states, inputs, views, handlers, fields, constants, records, record_fields;
    uint32_t code_count;
    /* Its tables (027's 4f-1), each sized to it in one block (tables) that load takes and pure_module_release gives
     * back: a state's, an input's, a message field's, a view's (the columns the load makes among them), a handler's, a
     * record's, a record field's, a function's and a signature item's entries. */
    void *tables;
    char (*state_names)[PURE_MODULE_NAME];
    char (*input_names)[PURE_MODULE_NAME];
    char (*view_names)[PURE_MODULE_NAME];
    char (*input_keys)[PURE_MODULE_NAME];
    uint8_t *state_type;
    uint16_t *state_ref; /* text: constant; cursor: text; list: record; index: list */
    uint8_t *state_flags; /* cursor: PURE_CURSOR_STAYS; list: PURE_LIST_ROWS >> 8 */
    bool *state_transient; /* PURE_STATE_TRANSIENT */
    bool *state_counted; /* what the states hold counts it (a text, a list not a message's rows: the memory budget's) */
    uint16_t state_words; /* a set of states' words, a bit each (027's 4f-2): (states + 63) / 64, at least 1 */
    /* A keyed list: its key field + 1 (0: none). Its items stay in key order
     * (numbers by value, texts by bytes), each key once, and keys do not change. */
    uint8_t *state_key;
    bool *state_indexed; /* list: an index state refers to it (set at load) */
    uint8_t *height;    /* the operand stack's height at each instruction, from its block's start (the verifier's) */
    /* In a batch's block (module-batch.c), a value computed again by the same code as an earlier one whose operands
     * nothing changed between: at its code's first instruction, the earlier root + 1 and its code's length (reuse_from,
     * reuse_length; 0: none); at such an earlier root, reuse_from PURE_REUSE_KEPT (its value kept for them). */
    uint32_t *reuse_from, *reuse_length;
    /* What a call of each function keeps of its caller's (030's step 1, set at load): its own lists and registers in
     * ascending order of state, a register's with PURE_KEEP_REGISTER, function f's from keep_start[f] up to
     * keep_start[f + 1]: a call walks them, not its sets' words. */
    uint32_t *keeps, *keep_start;
    bool reuses;         /* some value of it is planned so (else a batch need not look) */
    struct PureBatchPlan **batch_plans; /* each batch loop's block as the load reads it (module-batch.c, docs/design/025), */
    uint32_t batch_plan_count;          /* in their loops' order */
    struct PureJit *jit; /* its blocks compiled (module-jit.c, docs/design/021), or NULL */
    bool batches;        /* a loop of it is planned to run in batches (module-batch.c, docs/design/024) */
    uint8_t runner;      /* which executor runs its blocks: 0 run's own steps, 1 with batches, 2 with calls (module.c) */
    bool held;           /* it holds bytes or texts by name (BYTES_STORE): events and views empty them as they begin */
    PureInputShape *input;
    PureField *field;
    PureValue *initial;
    uint8_t *view_type;
    uint16_t *view_table; /* a column's list view + 1, or 0 */
    uint16_t *view_list;  /* the list state of a list view over one, or its column's */
    uint16_t *view_record; /* a list view's or a list column's items' record + 1 (a map's: 0) */
    uint16_t *view_field;  /* a column the load made: its item's field + 1 (read as it is), or 0 */
    bool *view_computed;   /* a list view whose code leaves its list (a column's: its view's) */
    PureBlock *view_code;
    uint8_t *view_plan; /* a table's filter runs a leaf of its list at once (as a fold may) */
    uint32_t *view_arguments; /* the input of a view's arguments + 1 (a column's: its table's), or 0 */
    /* Set at load: a view's code computes (its locals and loops: a body), and the event's lists it changes (a set of
     * states, state_words for each view). */
    bool *view_body;
    uint64_t *view_lists;
    PureShape shape[PURE_MODULE_SHAPES];
    uint8_t shapes;
    PureRange range[PURE_MODULE_RANGES];
    uint8_t ranges;
    uint8_t *view_range; /* a table's filter's PureRange + 1, or 0 */
    PureHandler *handler;
    /* The code and the text constants, sized to the module in one block
     * (storage) that load takes and pure_module_release gives back. */
    PureOp *code;
    uint32_t *constant_offset, *constant_length;
    unsigned char *constant_data;
    void *storage;
    char (*record_names)[PURE_MODULE_NAME];
    PureRecord *record;
    char (*record_field_names)[PURE_MODULE_NAME];
    uint8_t *record_field_type; /* PureRecordType */
    PureValue *record_field_default; /* a number (a bool's 0 or 1), an i64, a text constant's index or a list's record
                                      * (LIST, its i64) */
    bool *record_nested; /* a record with a LIST field (set at load) */
    uint16_t *record_held; /* its fields held as bytes are (pure_record_bytes), a bit each (set at load) */
    uint16_t functions;
    PureFunction *function;
    /* Each function's sets of states (state_words each): the event's lists and the registers its code names, each
     * call's own, and the lists not an event's it changes (or a function it calls does). */
    uint64_t *function_lists, *function_registers, *function_changes;
    /* Each input's sets of states (state_words each), from its handlers' code and the functions they call (set at load):
     * the event's lists to empty as its event ends (those its handlers name, not the registers, and its message's rows),
     * and the stored lists they name (to settle as it ends: docs/design/022). What an event never touches is never
     * walked for it (030's step 1). */
    uint64_t *event_clears, *event_settles;
    uint8_t *signature_type;
    uint16_t *signature_record;
    bool calls; /* it has functions: its blocks run in the executor that calls (module.c) */
    uint32_t nesting; /* its loops' deepest nesting (set at load) */
    /* Which load this is, of the process's: never 0, never the same twice. Who keeps a module's address to ask later
     * whether it is the same module asks this instead, since another may come to that address once one is freed. */
    uint64_t serial;
} PureModule;

/* Whether a state of m is transient (PURE_STATE_TRANSIENT). */
static inline bool pure_state_transient(const PureModule *m, uint32_t state) { return m->state_transient[state]; }
/* Whether a set of states (state_words words) holds state. */
static inline bool pure_states_has(const uint64_t *set, uint32_t state) { return set[state >> 6] >> (state & 63) & 1; }

/* Decode and verify into out, which must be zeroed (a static one is) or hold
 * a loaded module: what it held is given back first. On failure out holds
 * nothing and error names the rule. */
PureResult pure_module_load(const void *bytes, size_t size, PureModule *out,
                            char *error, size_t error_size);
/* Give back a loaded module's code and constants (no app may still run it). */
void pure_module_release(PureModule *);
/* Read a module file whole and load it (NOT_FOUND when it cannot be read). */
PureResult pure_module_read(const char *path, PureModule *out, char *error, size_t error_size);
/* Index of a name, or -1. Views are those outside tables. */
int pure_module_state(const PureModule *, const char *name);
int pure_module_input(const PureModule *, const char *name);
int pure_module_view(const PureModule *, const char *name);
int pure_module_column(const PureModule *, uint32_t table, const char *name);
/* What a message of an input looks like, as 32 bits: its fields' names,
 * types and places (FNV-1a). A sender's output and the input it sends to
 * across nodes agree on it, or the input takes the output's fields by name
 * (link/link.h, docs/design/014-fields-by-name.md). */
uint32_t pure_module_shape(const PureModule *, uint32_t input);
/* An input's (or output's) fields as a sender names them to a receiver: u8
 * count, each field's {u8 type, u8 name length, name}. Its size (at most
 * PURE_FIELDS_NAMED), written to out. */
enum { PURE_FIELDS_NAMED = 1 + PURE_MESSAGE_FIELDS * (2 + PURE_MODULE_NAME) };
size_t pure_module_fields(const PureModule *, uint32_t input, unsigned char out[PURE_FIELDS_NAMED]);
/* Whether input takes messages laid out with the fields named (as
 * pure_module_fields names them): it has every one by name, of the same type
 * or one that holds all its values (a u32 in a number, text in bytes). A
 * field it has that they lack is 0 or empty; none of theirs is dropped. */
bool pure_module_takes(const PureModule *, uint32_t input, const unsigned char *fields, size_t size);
/* A payload laid out with the fields named, laid out anew for input by
 * name (pure_module_takes says it can be; out is malloc'd). STORE_ERROR
 * when payload is not laid out with them. */
PureResult pure_module_recast(const PureModule *, uint32_t input, const unsigned char *fields, size_t size,
                              PureSpan payload, unsigned char **out, size_t *out_size);
/* The text state a text view shows as it is (a view that is just the
 * state), or -1: a host may lend its stored extents instead of copying. */
int pure_module_text_state(const PureModule *, uint32_t view);

/* A data host keeps the app's states (for example the tree store); without one (all of it NULL) they live in memory
 * alone. state opens a runtime-owned value by name as a draft (a missing one is empty at revision 0); save publishes
 * the given drafts together, all or nothing. */
typedef struct {
    void *context;
    PureResult (*state)(void *context, const char *name, PureDraft **out);
    PureResult (*save)(void *context, PureDraft *const *states, size_t count);
    /* Optional, with state and save: the directory where the app's lists are
     * kept, their images and the log of their changes (core/log.h); without
     * one, list states are not stored. */
    const char *(*directory)(void *context);
} PureData;

/* A byte or text value inside one event or view: a range of the payload, a
 * text version, a module constant, a copy or the event's arena. */
typedef struct { uint8_t source; uint16_t owner; PureVersion version; uint64_t offset, length; } PureBytes;
typedef struct { unsigned char *bytes; size_t used, capacity; } PureArena;

/* One app instance: its states and at most one open event. Text states are
 * runtime-owned drafts; cursors are positions the runtime keeps on character
 * boundaries; lists are runtime-owned and an index always names an item
 * (or 0 in an empty list). Every event is atomic: on failure nothing changes.
 *
 * Saving. When the data host keeps states the app starts from the stored
 * ones. A save publishes, in one commit, the states changed since the last
 * save; if it fails, every event since the last save is undone. By default
 * each key, text and message event saves before it returns (a frame waits for
 * the next one). Deferred, events apply at once and a host saves a burst with
 * one commit: an event is durable once a later save succeeds. */
/* The runtime's lists' places among the stored lists' records (PureApp),
 * past any state's: the numbers heard, and the outbox. A place's slot in
 * the arrays of them is a state's number, or states and states + 1. */
enum { PURE_LIST_HEARD = 0x10000, PURE_LIST_OUTBOX, PURE_LIST_RUNTIME = 2 };
typedef struct {
    const PureModule *module;
    uint64_t module_serial; /* a reader's: its module's serial when it read (the module may since be freed) */
    PureData data;
    /* The arrays of each state (027's 4f-2), sized to the module in one block (states) for the states it was laid out
     * for (laid). */
    void *states;
    uint32_t laid;
    PureValue *values; /* numbers, bools, cursor byte positions and list indices */
    PureDraft **texts;
    PureList **lists;
    PureUndo undo; /* list changes of the current event */
    PureDraft **list_drafts; /* lists the tree kept before images, until emptied */
    /* Stored lists (module-save.c), by slot (a state's, then the runtime's:
     * the heard list and the outbox): the save each one's image holds, the
     * first save logged since that changed it (0: none), and the bytes of its
     * changes logged since; the log, the tree's number of its last save, the
     * last save's number, and a checkpoint being written. */
    uint64_t *image_seq, *logged_since;
    size_t *logged;
    struct PureLog *log;
    PureDraft *stored_seq;
    uint64_t seq;
    struct PureCheckpoint *checkpoint;
    struct PureStartLog *start_log; /* the log as read at a start, while lists open */
    bool background_checkpoints;
    bool uncertain; /* a save may or may not be stored: none follows until a start */
    PureDraft *stored_values;                   /* numbers, cursors and indices, when kept */
    PureVersion *saved; /* text and list versions at the last save */
    /* Events since the last save: what a failed save returns to. */
    bool deferred, open;
    PureValue *durable_values;
    PureVersion *durable_texts;
    size_t *durable_lists; /* each stored list's journal length */
    uint64_t durable_events;
    PureFault fault;
    uint32_t fault_pc;
    size_t memory; /* the budget of what the states hold (0: none): pure_app_memory */
    uint64_t work_budget, work; /* pure_app_work's budget (0: PURE_WORK_DEFAULT), and what the unit running has used */
    int64_t grown, spent; /* what the event running has added to the states (and its lists), as stored and in all */
    PureList **own; /* a reader's: the event's lists its views compute in, its own */
    size_t room;          /* what it may hold for that in all: PURE_GROWTH_DEFAULT, or the memory budget if larger */
    PureFault view_fault; /* why the last view evaluated had no value (pure_app_view_fault) */
    char why[128]; /* why the last start or replace was refused, when it says (else empty; a failed start keeps it) */
    uint64_t events;
    /* The time events see as now, in milliseconds since 1970 (UTC). The host
     * sets it, so a script can replay any time; views never see it. */
    double now;
    uint64_t changes; /* counts every change of states, never back: what a filter's rows are fresh for */
    /* A filtered list view's items, or a computed one's list (kept: its lists in stored form). */
    struct PureRows { uint32_t table; uint64_t changes, given; size_t *items, count, capacity; PureFault fault; PureList *list; } *rows;
    unsigned row_tables;
    /* Current message or text event, kept until pure_app_finish. */
    PureSpan payload;
    bool has_reply;
    PureBytes reply;
    unsigned char *copied;
    PureArena arena, view_arena;
    /* Links (runtime-owned, saved with the states): the outbox, messages sent
     * on outputs and not trimmed, in the order sent, a list of {output (its
     * name), seq, payload (bytes)} stored as a list state is (module-save.c)
     * and changed with the events that send (the first sent_saved are saved:
     * the host sees only those; a trim lets go at once, and a failed save
     * takes it back); each output's last number (and as saved), and those of
     * outputs a module dropped. */
    PureList *outbox;
    size_t sent_saved, durable_sent_saved, durable_outbox;
    uint64_t sends_gone; /* how many ever left it: a host's places in it move back at most that many */
    uint64_t *last_sent, *saved_sent; /* each input's (by_input), an output's numbers */
    struct PureCarried { char name[PURE_MODULE_NAME]; uint64_t last; } *carried;
    unsigned carried_count;
    /* The last number delivered from each source: a list of {source (text),
     * number}, in order of source, stored as a list state is (module-save.c)
     * and changed with the events that deliver, so a failed save takes its
     * numbers back with them; its journal's length when the burst began. */
    PureList *heard;
    size_t durable_heard;
    PureDraft *stored_links;
    bool links_changed; /* since the last save */
    bool duplicate;     /* the last delivery was heard before and not applied */
    struct { const char *source; uint64_t seq; uint32_t release; bool pending; } delivering;
    /* The arguments given for a view (pure_app_arguments): its input + 1 (0:
     * none), where their fields are, and a count of the times they were given. */
    unsigned argued;
    PureSpan arguments;
    uint32_t argument_spans[PURE_MESSAGE_FIELDS][2];
    uint64_t arguments_given;
    /* Passes over lists that answer shaped folds, by the list's version. */
    struct PurePass *passes;
    unsigned pass_count;
    struct PureSnapshot *snapshot; /* the newest snapshot made, or NULL */
    PureRetired retired;           /* list items let go since it was made */
    bool reading;                  /* a reader: its states are a snapshot's */
    /* The values an event or a view's body holds by name (BYTES_STORE): emptied as it begins. */
    /* A value of the event's payload, arguments or constants is held as it is (by_ref: ref, nothing copied). */
    struct { unsigned char *data; size_t size, capacity; PureBytes ref; bool by_ref; } held[PURE_MODULE_HELD];
    /* The lists in records an event or a view's body made (LIST fields: a value is a number here, 1 on), how many
     * values share each (PURE_NESTED_LENT: an event's list itself, lent by LIST_VALUE: shared, so a change copies it
     * first, and never freed here), and the bound lists' own (a register's list while another is bound to it). */
    struct { PureList **list; uint32_t *shared; size_t count, capacity; } nested;
    PureList **unbound;
    uint32_t *bound, bound_count; /* the registers bound since the last unbind (each once): what unbind gives back */
    /* Calls (027's 2c): the frames of those open, in blocks that never move (code points into a frame), the block in
     * use and how much of it; and the event's lists calls let go, emptied, each state's for its next calls. */
    struct { unsigned char **blocks; size_t *sizes; unsigned count, at; size_t used; } calls;
    struct PureSpare { PureList **lists; unsigned count, capacity; } *spare;
    PureValue *pending; /* the values an event works on until it ends */
    void *deep; /* a run's loops, a module nesting past PURE_MODULE_NESTING's (its nesting of them) */
    uint32_t deep_room;
    PureVersion *marked_texts; /* where the event running began: each text's version, each stored list's journal, */
    size_t *marked_lists;
    /* Each input's: its last number sent and as saved, and where the event running began; one block (by_input) for
     * the inputs of the module it was laid out for (027's 4f-3a). */
    uint64_t *marked_sent;
    void *by_input;
} PureApp;

PureResult pure_app_start(PureApp *, const PureModule *, PureData data);
/* Publish every change since the last save in one commit, or undo them all. */
PureResult pure_app_save(PureApp *);
/* Defer saving to pure_app_save (false: save after each event). Needs a data
 * host that saves together (PureData.save). */
bool pure_app_defer(PureApp *, bool deferred);
/* Leaves unsaved changes unsaved: call pure_app_save first to keep them. */
void pure_app_stop(PureApp *);
/* Deliver a message (a key's, a text's, a tick's or a frame's too, as its binding says it is laid out). payload stays
 * alive until pure_app_finish. */
PureResult pure_app_message(PureApp *, uint32_t input, PureSpan payload);
/* The same, with the variable fields where the host found them: field f
 * starts at placed[2f] in payload and has placed[2f + 1] bytes (entries of
 * fixed fields are not read, nor are the variable fields' lengths in the
 * prefix). A host that parsed a request in place passes the fields' bytes
 * where they are. */
PureResult pure_app_message_fields(PureApp *, uint32_t input, PureSpan payload, const uint32_t *placed);
/* Visit the reply of the last message in order, without copying, until the
 * next event (deferred: copy it before a later save too). */
PureResult pure_app_reply(const PureApp *, bool (*visit)(void *, PureSpan), void *context);
void pure_app_finish(PureApp *);
/* A number view, or a bool view (0 or 1: its type says which); false for a
 * text or i64 view or a failed evaluation. */
bool pure_app_view(PureApp *, uint32_t view, double *out);
/* An i64 view, and an i64 column's cell; false for any other or a failed evaluation. */
bool pure_app_view_i64(PureApp *, uint32_t view, int64_t *out);
bool pure_app_cell_i64(PureApp *, uint32_t column, size_t row, int64_t *out);
/* A text or bytes view, lent in order until the next view evaluation. */
PureResult pure_app_view_text(PureApp *, uint32_t view, bool (*visit)(void *, PureSpan), void *context);
/* The rows of a list view (its list's items, those its filter keeps, or
 * the items of the list its code leaves), or 0 for another view. A host
 * evaluates only the cells it shows. */
size_t pure_app_rows(PureApp *, uint32_t table);
/* A view that takes arguments (a column: its table's): the input whose
 * fields they are, or -1. */
int pure_module_arguments(const PureModule *, uint32_t view);
/* The arguments of view, as a message payload of their input's shape (its
 * variable fields where placed says, or after the prefix in order): the
 * view, its columns and its rows read them until they are given again. The
 * payload stays the caller's and must outlive what it shows. INVALID when
 * the view takes none, LIMIT, NON_FINITE or ENCODING as a message fails. */
PureResult pure_app_arguments(PureApp *, uint32_t view, PureSpan payload, const uint32_t *placed);
/* How many passes the app keeps, made to answer shaped folds in columns. */
unsigned pure_app_passes(const PureApp *);
/* The item a row of a list view shows, and the row that shows an item
 * (false: its filter leaves the item out). Without a filter both are the
 * identity. */
bool pure_app_row_item(PureApp *, uint32_t table, size_t row, size_t *item);
bool pure_app_item_row(PureApp *, uint32_t table, size_t item, size_t *row);
/* A number or bool column's cell (as pure_app_view), or a text or bytes column's. */
bool pure_app_cell(PureApp *, uint32_t column, size_t row, double *out);
PureResult pure_app_cell_text(PureApp *, uint32_t column, size_t row, bool (*visit)(void *, PureSpan), void *context);
/* A list column's cell: its items in their stored form (027's stage 3), of
 * the column's record (view_record - 1), lent until the next view
 * evaluation or change; pure_items_next reads them. */
PureResult pure_app_cell_items(PureApp *, uint32_t column, size_t row, PureSpan *items);
/* The next item of items (of record's, in stored form) at *at: its cells
 * (a text's, a bytes' and a list's in items' bytes, a list's in this form
 * again), *at moved past it; false at the end or where the bytes are not
 * that form. */
bool pure_items_next(const PureModule *, uint32_t record, PureSpan items, size_t *at, PureCell *cells);
/* Switch to another verified module. States keep their value by name (text
 * states keep their content, cursors their position in that text); new states
 * start at their initial value. The module must outlive the app. */
PureResult pure_app_replace(PureApp *, const PureModule *);

/* Connections between apps (docs/research/078-app-connections.md). A host
 * sees only saved sends, delivers each at least once, in order per output,
 * and trims what its receiver has saved; a receiver applies each once. */
typedef bool (*PureSentVisit)(void *context, uint32_t output, uint64_t seq, PureSpan payload);
void pure_app_outbox(const PureApp *, PureSentVisit visit, void *context);
/* The saved send at a place in the outbox (0 the oldest, below sent_saved);
 * false when its output is not the module's. */
bool pure_app_sent(const PureApp *, size_t at, uint32_t *output, uint64_t *seq, PureSpan *payload);
/* Forget output's saved sends numbered up to through (saved with the next save). */
void pure_app_trim(PureApp *, uint32_t output, uint64_t through);
/* Deliver a message event from source (a sender's name, 1 to 64 printable
 * ASCII bytes) numbered seq (from 1 to PURE_SEQ_MOST, growing per source).
 * An app whose states are kept takes deliveries only where its lists are
 * (a directory: the numbers heard are a list). A number heard
 * before is not applied again: it succeeds with app->duplicate set. The
 * number counts as heard with the event's changes, and also when the app
 * refuses the message (a fault: it was delivered, and refused); not when a
 * save fails, nor when the app has no memory for it (pure_app_memory: it
 * comes again). placed is as for pure_app_message_fields, or NULL. */
PureResult pure_app_deliver(PureApp *, uint32_t input, const char *source, uint64_t seq, PureSpan payload,
                            const uint32_t *placed);
/* An answer to the send numbered seq of output (serve/deliver.h's --reply),
 * delivered as pure_app_deliver delivers from source; in the same event the
 * output's saved sends through seq leave the outbox, so the answer has the
 * room its send held. */
PureResult pure_app_answer(PureApp *, uint32_t input, uint32_t output, const char *source, uint64_t seq,
                           PureSpan payload, const uint32_t *placed);
/* The last number heard from source (0 when none): a host that gets
 * numbered messages out of order hands them over from the one after it. */
uint64_t pure_app_heard(const PureApp *, const char *source);

/* An app's memory budget (docs/design/013-memory-budget.md): what its
 * states hold, as they are stored (their lists' items, their texts, the
 * outbox and the numbers heard), at most bytes (0: no budget, at first). An
 * event after which they hold more than that, and more than before it,
 * fails with the fault PURE_FAULT_MEMORY (PURE_BUDGET) and changes nothing;
 * one that frees or keeps them goes on. A delivery refused so is not heard:
 * its sender sends it again. */
void pure_app_memory(PureApp *, size_t bytes);
/* What the states hold now, as the budget counts it. */
size_t pure_app_state_bytes(const PureApp *);

/* An app's work budget (docs/design/017-apps-on-one-host.md): what one
 * event, or one evaluation of a view (a value, a cell, a table's filter),
 * may run, in instructions counted as written, whatever the plan: a block's
 * length each time it runs, and a fold's or a walk's block length for each
 * item of its list (of its slice, when it takes one) it goes over, all
 * counted as the fold or walk begins. Past it the event fails with the fault
 * PURE_FAULT_WORK (PURE_LIMIT) and changes nothing, and a view has no value.
 * A LOOP counts its block's length + 1 each time round, as it goes round.
 * 0, as at first: PURE_WORK_DEFAULT (docs/design/018: every unit ends). */
void pure_app_work(PureApp *, uint64_t instructions);
/* Why the last view evaluated on this app (or a table's rows asked for)
 * had no value, once it had none: PURE_FAULT_WORK past the work budget,
 * another fault, or PURE_FAULT_NONE. pure_app_rows always says. */
PureFault pure_app_view_fault(const PureApp *);

/* Readers on other threads. Right after a save an app can make a snapshot of
 * its saved states, for evaluating views anywhere while it keeps applying
 * events: numbers are copied; text states share their stored versions and
 * lists their items (a list unchanged since the last snapshot shares that
 * snapshot's copy), so the cost does not grow with the text. NULL when a
 * change waits for a save, when a text state is not all stored (for example
 * without a data host that keeps states), or when memory runs out. The caller
 * gets a reference; the app keeps its own until the next snapshot. */
typedef struct PureSnapshot PureSnapshot;
/* Checkpoints (with a directory for lists): a list whose logged changes
 * outgrew its items gets a new image, written from a snapshot, and then the
 * log lets go the entries the images hold. By default each save writes the
 * images due before it returns. With background checkpoints the host takes
 * one due after a save (NULL: none, or one is still out), writes it on any
 * thread, and hands the result back on the app's thread before the app stops
 * or is replaced. */
typedef struct PureCheckpoint PureCheckpoint;
void pure_app_background_checkpoints(PureApp *, bool background);
PureCheckpoint *pure_app_checkpoint(PureApp *);
PureResult pure_checkpoint_write(PureCheckpoint *);
void pure_app_checkpointed(PureApp *, PureCheckpoint *, PureResult);
PureSnapshot *pure_app_snapshot(PureApp *);
void pure_snapshot_hold(PureSnapshot *);
/* On any thread; the last reference frees it. */
/* Let a reference go: true when it was the last, the snapshot freed. */
bool pure_snapshot_release(PureSnapshot *);
/* The module the snapshot's app ran when it was taken. */
const PureModule *pure_snapshot_module(const PureSnapshot *);
/* Show a snapshot's states in reader, a zeroed app or a reader before: its
 * views read them while the snapshot is held, with the reader's own arena and
 * row cache (one thread at a time). It applies no events. pure_app_stop frees
 * the reader and leaves the snapshot alone. */
bool pure_app_read(PureApp *reader, const PureSnapshot *); /* false without room for its states: it shows none */
#endif
