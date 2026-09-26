#ifndef PURE_MODULE_H
#define PURE_MODULE_H
#include "flow.h"
#include "result.h"
#include "module-list.h"
#include "state.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Pure module format v5 (docs/design/008-module-format.md). A toolchain such
 * as aup produces it; Pure verifies it completely before anything executes:
 * operand types (including which text a cursor and which list an index
 * belongs to), stack heights and the stack at every join, forward-only
 * branches, names, records, tables, constants and effects. */
enum {
    PURE_MODULE_STATES = 32, PURE_MODULE_INPUTS = 32, PURE_MODULE_VIEWS = 64,
    PURE_MODULE_HANDLERS = 64, PURE_MODULE_CODE = 512, PURE_MODULE_STACK = 64,
    PURE_MODULE_NAME = 32, PURE_MODULE_STRINGS = 32768, PURE_MODULE_HEADER = 40,
    PURE_MODULE_FIELDS = 32, PURE_MODULE_COLLECTIONS = 1, PURE_MODULE_CONSTANTS = 64,
    PURE_MODULE_CONSTANT_BYTES = 8192, PURE_MODULE_RECORDS = 8, PURE_MODULE_RECORD_FIELDS = 64,
    PURE_RECORD_FIELDS = 16, PURE_MODULE_JOINS = 8, PURE_APP_PENDING = 256
};

typedef enum {
    PURE_OP_CONST, PURE_OP_LOAD, PURE_OP_DT, PURE_OP_ADD, PURE_OP_SUB, PURE_OP_MUL,
    PURE_OP_DIV, PURE_OP_NEG, PURE_OP_LT, PURE_OP_LE, PURE_OP_GT, PURE_OP_GE,
    PURE_OP_EQ, PURE_OP_NE, PURE_OP_AND, PURE_OP_OR, PURE_OP_NOT, PURE_OP_MIN,
    PURE_OP_MAX, PURE_OP_CLAMP, PURE_OP_STORE, PURE_OP_JZ, PURE_OP_JMP,
    PURE_OP_FIELD, PURE_OP_DOC, PURE_OP_DOC_SIZE, PURE_OP_LEN, PURE_OP_REPLACE,
    PURE_OP_REPLY, PURE_OP_REQUIRE,
    PURE_OP_TEXT_CONST, PURE_OP_TEXT_LOAD, PURE_OP_TEXT_INPUT, PURE_OP_NUM_TEXT,
    PURE_OP_CONCAT, PURE_OP_CHARS, PURE_OP_LINES, PURE_OP_TEXT_BYTES,
    PURE_OP_CUR_LOAD, PURE_OP_CUR_START, PURE_OP_CUR_END, PURE_OP_CUR_MOVE,
    PURE_OP_CUR_STORE, PURE_OP_TEXT_INSERT, PURE_OP_TEXT_REMOVE, PURE_OP_TEXT_LINE,
    PURE_OP_TEXT_SET, PURE_OP_TEXT_FROM, PURE_OP_BYTES_FROM, PURE_OP_CUR_COUNT,
    PURE_OP_LIST_COUNT, PURE_OP_LIST_APPEND, PURE_OP_LIST_INSERT, PURE_OP_LIST_REMOVE,
    PURE_OP_ITEM_GET, PURE_OP_ITEM_SET, PURE_OP_INDEX_LOAD, PURE_OP_INDEX_STORE,
    PURE_OP_INDEX_MOVE, PURE_OP_INDEX_FIRST, PURE_OP_INDEX_LAST, PURE_OP_INDEX_NUM, PURE_OP_ROW,
    PURE_OP_PICK, PURE_OP_SLIDE, PURE_OP_FOLD, PURE_OP_TEXT_COMPARE, PURE_OP_LIST_FIND, PURE_OP_LIST_HAS,
    PURE_OP_COUNT
} PureOpcode;
/* FOLD's argument: the kind (bits 0-7), the list state (8-15) and the length
 * of the block that follows (16-31), run once per item as a column of that
 * list. count counts the items whose block leaves a non-zero number; sum,
 * max and min take a value and an include flag from their block; first gives
 * the index of the first item whose block leaves a non-zero number. */
typedef enum { PURE_FOLD_COUNT, PURE_FOLD_SUM, PURE_FOLD_MAX, PURE_FOLD_MIN, PURE_FOLD_FIRST } PureFold;
/* TEXT_COMPARE's argument: the relation, in the order of the number
 * comparisons (LT LE GT GE EQ NE); texts order by their bytes. */
typedef enum { PURE_LESS, PURE_LESS_EQUAL, PURE_GREATER, PURE_GREATER_EQUAL, PURE_EQUAL, PURE_NOT_EQUAL } PureRelation;

typedef enum { PURE_KIND_KEY = 1, PURE_KIND_MESSAGE = 2, PURE_KIND_TEXT = 3 } PureInputKind;
/* A message field: a u32le or a binary64 (fixed, at its offset in the
 * payload's prefix), or bytes or text of any length. The prefix also holds
 * each variable field's u32le length but the last one's (its offset is
 * PURE_FIELD_REST), which takes what follows the others; the variable
 * fields' bytes follow the prefix in declaration order. A text is UTF-8 and
 * a number finite, or the message fails. */
typedef enum { PURE_FIELD_U32 = 1, PURE_FIELD_BYTES = 2, PURE_FIELD_NUMBER = 3, PURE_FIELD_TEXT = 4 } PureFieldType;
enum { PURE_FIELD_REST = UINT32_MAX };
typedef enum {
    PURE_STATE_NUM = 0, PURE_STATE_TEXT = 1, PURE_STATE_CURSOR = 2, PURE_STATE_LIST = 3, PURE_STATE_INDEX = 4
} PureStateType;
/* A cursor that stays before text inserted at it (a mark); other cursors go
 * after it (a caret). */
enum { PURE_CURSOR_STAYS = 1 };
/* A table view has one row per item of a list, or per item its filter
 * keeps; its columns are number or text views evaluated for one row at a
 * time. */
typedef enum { PURE_VIEW_NUM = 0, PURE_VIEW_TEXT = 1, PURE_VIEW_TABLE = 2 } PureViewType;
typedef enum { PURE_UNIT_CHAR = 0, PURE_UNIT_LINE = 1 } PureUnit;
typedef enum { PURE_EDGE_DOWN, PURE_EDGE_UP } PureEdge;
/* EXISTS: an item with that key is already in the keyed list (the event
 * reports PURE_CONFLICT). */
typedef enum {
    PURE_FAULT_NONE, PURE_FAULT_DIVIDE, PURE_FAULT_NON_FINITE, PURE_FAULT_CLAMP,
    PURE_FAULT_LIMIT, PURE_FAULT_NOT_FOUND, PURE_FAULT_ENCODING, PURE_FAULT_EXISTS
} PureFault;

typedef struct { double value; uint32_t arg; uint8_t op; } PureOp;
typedef struct { uint32_t start, length; } PureBlock;
typedef struct { uint32_t event; PureBlock code; } PureHandler;
typedef struct { char name[PURE_MODULE_NAME]; uint8_t type; uint32_t offset; } PureField;
typedef struct {
    uint8_t kind;
    bool keyed;
    uint16_t field_start, field_count;
    uint32_t prefix; /* fixed payload bytes before an optional bytes tail */
} PureInputShape;

/* A record type: fields numbered from first, each a number or text. */
typedef struct { uint16_t first, count; } PureRecord;

typedef struct {
    uint16_t states, inputs, views, handlers, fields, collections, constants, records, record_fields;
    uint32_t code_count;
    char state_names[PURE_MODULE_STATES][PURE_MODULE_NAME];
    char input_names[PURE_MODULE_INPUTS][PURE_MODULE_NAME];
    char view_names[PURE_MODULE_VIEWS][PURE_MODULE_NAME];
    char collection_names[PURE_MODULE_COLLECTIONS][PURE_MODULE_NAME];
    char input_keys[PURE_MODULE_INPUTS][PURE_MODULE_NAME];
    uint8_t state_type[PURE_MODULE_STATES];
    uint16_t state_ref[PURE_MODULE_STATES]; /* text: constant; cursor: text; list: record; index: list */
    uint8_t state_flags[PURE_MODULE_STATES]; /* cursor: PURE_CURSOR_STAYS */
    /* A keyed list: its key field + 1 (0: none). Its items stay in key order
     * (numbers by value, texts by bytes), each key once, and keys do not change. */
    uint8_t state_key[PURE_MODULE_STATES];
    PureInputShape input[PURE_MODULE_INPUTS];
    PureField field[PURE_MODULE_FIELDS];
    double initial[PURE_MODULE_STATES];
    uint8_t view_type[PURE_MODULE_VIEWS];
    uint16_t view_table[PURE_MODULE_VIEWS]; /* a column's table view + 1, or 0 */
    uint16_t view_list[PURE_MODULE_VIEWS];  /* the list of a table or column */
    PureBlock view_code[PURE_MODULE_VIEWS];
    PureHandler handler[PURE_MODULE_HANDLERS];
    PureOp code[PURE_MODULE_CODE];
    uint32_t constant_offset[PURE_MODULE_CONSTANTS], constant_length[PURE_MODULE_CONSTANTS];
    char record_names[PURE_MODULE_RECORDS][PURE_MODULE_NAME];
    PureRecord record[PURE_MODULE_RECORDS];
    char record_field_names[PURE_MODULE_RECORD_FIELDS][PURE_MODULE_NAME];
    uint8_t record_field_text[PURE_MODULE_RECORD_FIELDS];
    double record_field_default[PURE_MODULE_RECORD_FIELDS]; /* number, or text constant index */
    unsigned char constant_data[PURE_MODULE_CONSTANT_BYTES];
} PureModule;

/* Decode and verify. On failure out is unspecified and error names the rule. */
PureResult pure_module_load(const void *bytes, size_t size, PureModule *out,
                            char *error, size_t error_size);
/* Index of a name, or -1. Views are those outside tables. */
int pure_module_state(const PureModule *, const char *name);
int pure_module_input(const PureModule *, const char *name);
int pure_module_view(const PureModule *, const char *name);
int pure_module_column(const PureModule *, uint32_t table, const char *name);
/* The index state a number view shows as its position (a view that is just
 * `at.position`), or -1: what a host moves with pure_app_point. */
int pure_module_position(const PureModule *, uint32_t view);

/* Keyed collections live in a data host (for example the tree store). One
 * event opens at most one document, the message's key, at its current
 * version; edits stay in that working version until the event commits it.
 * A version's bytes field is the value's size. */
typedef struct {
    void *context;
    PureResult (*open)(void *context, uint32_t collection, const char *key,
                       void **document, int64_t *revision);
    PureVersion (*version)(void *context, void *document);
    PureResult (*read)(void *context, void *document, PureVersion, size_t offset, size_t length,
                       bool (*visit)(void *, PureSpan), void *visit_context);
    PureResult (*splice)(void *context, void *document, size_t offset, size_t remove, PureSpan insert);
    PureResult (*commit)(void *context, void *document, int64_t *revision);
    void (*close)(void *context, void *document);
    /* Optional: the host keeps the app's states. state opens a runtime-owned
     * value by name as a draft (a missing one is empty at revision 0); save
     * publishes the given documents and state drafts together, all or
     * nothing, and names each document's new revision. */
    PureResult (*state)(void *context, const char *name, PureDraft **out);
    PureResult (*save)(void *context, void *const *documents, size_t document_count, PureDraft *const *states,
                       size_t count, int64_t *revisions);
} PureData;

/* A byte or text value inside one event or view: a range of the payload, a
 * document or text version, a module constant, a copy or the event's arena. */
typedef struct { uint8_t source; uint16_t owner; PureVersion version; uint64_t offset, length; } PureBytes;
typedef struct { unsigned char *bytes; size_t used, capacity; } PureArena;

/* One app instance: its states and at most one open event. Text states are
 * runtime-owned drafts; cursors are positions the runtime keeps on character
 * boundaries; lists are runtime-owned and an index always names an item
 * (or 0 in an empty list). Every event is atomic: on failure nothing changes.
 *
 * Saving. When the data host keeps states the app starts from the stored
 * ones. A save publishes, in one commit, the documents edited by messages and
 * the states changed since the last save; if it fails, every event since the
 * last save is undone. By default each key, text and message event saves
 * before it returns (a frame waits for the next one). Deferred, events apply
 * at once and a host saves a burst with one commit: an event is durable once
 * a later save succeeds, and each key's document joins a burst once (a second
 * message for it, or one past PURE_APP_PENDING keys, saves the burst first). */
typedef struct {
    const PureModule *module;
    PureData data;
    double values[PURE_MODULE_STATES]; /* numbers, cursor byte positions and list indices */
    PureDraft *texts[PURE_MODULE_STATES];
    PureList *lists[PURE_MODULE_STATES];
    PureUndo undo; /* list changes of the current event */
    PureDraft *list_drafts[PURE_MODULE_STATES]; /* stored lists, when kept */
    PureDraft *stored_values;                   /* numbers, cursors and indices, when kept */
    PureVersion saved[PURE_MODULE_STATES];      /* text and list versions at the last save */
    /* Events since the last save: what a failed save returns to, and the
     * documents messages opened (held until the save). */
    bool deferred, open;
    double durable_values[PURE_MODULE_STATES];
    PureVersion durable_texts[PURE_MODULE_STATES], durable_lists[PURE_MODULE_STATES];
    uint64_t durable_ticks, durable_events;
    struct PurePending { void *document; char key[65]; bool edited; } *pending; /* grows to PURE_APP_PENDING */
    unsigned pending_count, pending_capacity;
    PureFault fault;
    uint32_t fault_pc;
    uint64_t ticks, events;
    uint64_t changes; /* counts every change of states, never back: what a filter's rows are fresh for */
    struct PureRows { uint32_t table; uint64_t changes; size_t *items, count, capacity; } *rows; /* a filtered table's items */
    unsigned row_tables;
    /* Current message or text event, kept until pure_app_finish. */
    void *document;
    PureSpan payload;
    bool has_reply;
    PureBytes reply;
    unsigned char *copied;
    int64_t revision;
    PureArena arena, view_arena;
} PureApp;

PureResult pure_app_start(PureApp *, const PureModule *, PureData data);
/* Publish every change since the last save in one commit, or undo them all. */
PureResult pure_app_save(PureApp *);
/* Defer saving to pure_app_save (false: save after each event). Needs a data
 * host that saves documents together (PureData.save). */
bool pure_app_defer(PureApp *, bool deferred);
/* Leaves unsaved frame changes unsaved: call pure_app_save first to keep them. */
void pure_app_stop(PureApp *);
PureResult pure_app_frame(PureApp *);
PureResult pure_app_input(PureApp *, uint32_t input, PureEdge);
/* Point an index state at the item at position (0 first), as a user pointing
 * at a row: an event with no handler, saved like a key event. NOT_FOUND past
 * the last item, INVALID for a state that is not an index. */
PureResult pure_app_point(PureApp *, uint32_t state, size_t position);
/* Deliver committed text (for example from an input method). Not UTF-8: invalid. */
PureResult pure_app_text(PureApp *, uint32_t input, PureSpan text);
/* Deliver a message. base is the revision the sender edited, or -1 for none;
 * a handler that edits the document needs one. payload stays alive until
 * pure_app_finish. On return app->revision is the document's revision
 * (deferred: the revision the next save gives it). */
PureResult pure_app_message(PureApp *, uint32_t input, const char *key, int64_t base, PureSpan payload);
/* Visit the reply of the last message in order, without copying, until the
 * next event (deferred: copy it before a later save too). */
PureResult pure_app_reply(const PureApp *, bool (*visit)(void *, PureSpan), void *context);
void pure_app_finish(PureApp *);
/* A number view; false for a text view or a failed evaluation. */
bool pure_app_view(PureApp *, uint32_t view, double *out);
/* A text view, lent in order until the next view evaluation. */
PureResult pure_app_view_text(PureApp *, uint32_t view, bool (*visit)(void *, PureSpan), void *context);
/* The rows of a table view (its list's items, or those its filter keeps),
 * or 0 for another view. A host evaluates only the cells it shows. */
size_t pure_app_rows(PureApp *, uint32_t table);
/* The item a row of a table shows, and the row that shows an item (false:
 * its filter leaves the item out). Without a filter both are the identity. */
bool pure_app_row_item(PureApp *, uint32_t table, size_t row, size_t *item);
bool pure_app_item_row(PureApp *, uint32_t table, size_t item, size_t *row);
bool pure_app_cell(PureApp *, uint32_t column, size_t row, double *out);
PureResult pure_app_cell_text(PureApp *, uint32_t column, size_t row, bool (*visit)(void *, PureSpan), void *context);
/* Switch to another verified module. States keep their value by name (text
 * states keep their content, cursors their position in that text); new states
 * start at their initial value. The module must outlive the app. */
PureResult pure_app_replace(PureApp *, const PureModule *);
#endif
