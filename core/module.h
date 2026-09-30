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
    PURE_MODULE_HANDLERS = 64, PURE_MODULE_CODE = 65535, PURE_MODULE_STACK = 64,
    PURE_MODULE_NAME = 32, PURE_MODULE_STRINGS = 32768, PURE_MODULE_HEADER = 40,
    PURE_MODULE_FIELDS = 64, PURE_MODULE_COLLECTIONS = 1, PURE_MODULE_CONSTANTS = 4096,
    PURE_MODULE_CONSTANT_BYTES = 1 << 20, PURE_MODULE_RECORDS = 16, PURE_MODULE_RECORD_FIELDS = 128,
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
    PURE_OP_NOW, PURE_OP_FLOOR, PURE_OP_MOD, PURE_OP_SEND, PURE_OP_OUTER_ROW, PURE_OP_EACH,
    PURE_OP_LOCAL_LOAD, PURE_OP_LOCAL_STORE,
    PURE_OP_COUNT
} PureOpcode;
/* A local: one of an event's PURE_MODULE_LOCALS slots, 0 when the event
 * begins and never saved. LOCAL_LOAD and LOCAL_STORE name the slot (bits
 * 0-7), what it holds (bits 8-15: 0 a number, 1 an index) and an index's
 * list (bits 16-31); a handler uses a slot for one of them. A handler, the
 * walks in it and the folds in those read locals; the handler and its walks
 * store them. */
enum { PURE_MODULE_LOCALS = 64 };
/* EACH's argument: the list state (bits 8-15) and the length of the block
 * that follows (16-31), run in a handler once for every item the list had
 * when it began, in order, with ROW the item; the block may change states,
 * the list's items and other lists, but not insert into the list it walks
 * (an item it removes, or one before it, moves the walk with it). */
/* FOLD's argument: the kind (bits 0-7), the list state (8-15) and the length
 * of the block that follows (16-31), run once per item as a column of that
 * list. count counts the items whose block leaves a non-zero number; sum,
 * max, min and distinct take a value and an include flag from their block
 * (distinct counts the different values, -0 as 0); first gives the index of
 * the first item whose block leaves a non-zero number. */
typedef enum { PURE_FOLD_COUNT, PURE_FOLD_SUM, PURE_FOLD_MAX, PURE_FOLD_MIN, PURE_FOLD_FIRST, PURE_FOLD_DISTINCT } PureFold;
/* A slice: a fold whose kind has PURE_FOLD_SLICED, or a walk whose argument
 * has PURE_EACH_SLICED, first takes two numbers, a low and a high key, and
 * runs for the items of its list (keyed by a number) whose keys are from
 * low up to before high, found by key. */
enum { PURE_FOLD_SLICED = 0x80, PURE_EACH_SLICED = 1 };
/* TEXT_COMPARE's argument: the relation, in the order of the number
 * comparisons (LT LE GT GE EQ NE); texts order by their bytes. */
typedef enum { PURE_LESS, PURE_LESS_EQUAL, PURE_GREATER, PURE_GREATER_EQUAL, PURE_EQUAL, PURE_NOT_EQUAL } PureRelation;

/* A timer ticks every `period` milliseconds while a host runs the app. An
 * output is a message the app sends (SEND, in a handler; it has fields as a
 * message has and no handler): what receives it is the host's wiring.
 * Arguments are the fields a view takes (a view with arguments, its
 * columns and its filter read them as a message handler reads its message);
 * such an input has no handler and names its view, whose name it bears
 * with .arguments after it. */
typedef enum { PURE_KIND_KEY = 1, PURE_KIND_MESSAGE = 2, PURE_KIND_TEXT = 3, PURE_KIND_TIMER = 4,
               PURE_KIND_OUTPUT = 5, PURE_KIND_ARGUMENTS = 6 } PureInputKind;
enum { PURE_TIMER_LONGEST = 86400000, PURE_SOURCE = 64 };
/* The largest number a delivery may have: it is kept as a binary64. */
#define PURE_SEQ_MOST (UINT64_C(1) << 53)
/* A message field: a u32le or a binary64 (fixed, at its offset in the
 * payload's prefix), or bytes, text or rows of any length. The prefix also
 * holds each variable field's u32le length but the last one's (its offset is
 * PURE_FIELD_REST), which takes what follows the others; the variable
 * fields' bytes follow the prefix in declaration order. A text is UTF-8 and
 * a number finite, or the message fails. Rows are records of a list of a
 * message's rows (rows, a list state), laid out as a list's stored form: a
 * message fills that list with them, and fails when they are not whole. */
typedef enum { PURE_FIELD_U32 = 1, PURE_FIELD_BYTES = 2, PURE_FIELD_NUMBER = 3, PURE_FIELD_TEXT = 4,
               PURE_FIELD_ROWS = 5 } PureFieldType;
enum { PURE_FIELD_REST = UINT32_MAX };
typedef enum {
    PURE_STATE_NUM = 0, PURE_STATE_TEXT = 1, PURE_STATE_CURSOR = 2, PURE_STATE_LIST = 3, PURE_STATE_INDEX = 4
} PureStateType;
/* A cursor that stays before text inserted at it (a mark); other cursors go
 * after it (a caret). A list of a message's rows (its state's flags hold
 * PURE_LIST_ROWS beside the key byte; its name is the field's path): a
 * message's rows field fills it as the message arrives, it is emptied when
 * every event ends, and it is never saved nor logged for undoing. */
enum { PURE_CURSOR_STAYS = 1, PURE_LIST_ROWS = 256 };
/* A table view has one row per item of a list, or per item its filter
 * keeps; its columns are number or text views evaluated for one row at a
 * time. */
typedef enum { PURE_VIEW_NUM = 0, PURE_VIEW_TEXT = 1, PURE_VIEW_TABLE = 2 } PureViewType;
typedef enum { PURE_UNIT_CHAR = 0, PURE_UNIT_LINE = 1 } PureUnit;
typedef enum { PURE_EDGE_DOWN, PURE_EDGE_UP } PureEdge;
/* EXISTS: an item with that key is already in the keyed list (the event
 * reports PURE_CONFLICT). MEMORY: the event would leave the states holding
 * more than the app's budget (pure_app_memory; it reports PURE_BUDGET). */
typedef enum {
    PURE_FAULT_NONE, PURE_FAULT_DIVIDE, PURE_FAULT_NON_FINITE, PURE_FAULT_CLAMP,
    PURE_FAULT_LIMIT, PURE_FAULT_NOT_FOUND, PURE_FAULT_ENCODING, PURE_FAULT_EXISTS,
    PURE_FAULT_MEMORY
} PureFault;

/* plan, shape and range are the runtime's, set at load: a fold whose block
 * has a form for every instruction that works on a leaf of its list at once
 * runs so (plan), one in a column may be answered for every row from one
 * pass over its list (shape: its PureShape + 1), and a fold or a walk whose
 * test bounds a keyed list's key reads only the items in bounds (range: its
 * PureRange + 1). */
typedef struct { double value; uint32_t arg; uint8_t op, plan, shape, range; } PureOp;
typedef struct { uint32_t start, length; } PureBlock;
typedef struct { uint32_t event; PureBlock code; } PureHandler;
typedef struct { char name[PURE_MODULE_NAME]; uint8_t type; uint16_t rows; uint32_t offset; } PureField;
typedef struct {
    uint8_t kind;
    bool keyed;
    uint16_t field_start, field_count;
    uint32_t prefix; /* fixed payload bytes before the variable fields */
    uint32_t period; /* a timer's, in milliseconds */
    uint32_t view;   /* arguments: the view they are for */
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
typedef struct { uint8_t lower_relation, upper_relation; PureBlock lower, upper; } PureRange;

_Static_assert(PURE_MODULE_STATES <= 32, "a state is a bit of PureApp.counted");

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
    uint8_t state_flags[PURE_MODULE_STATES]; /* cursor: PURE_CURSOR_STAYS; list: PURE_LIST_ROWS >> 8 */
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
    uint8_t view_plan[PURE_MODULE_VIEWS]; /* a table's filter runs a leaf of its list at once (as a fold may) */
    uint8_t view_arguments[PURE_MODULE_VIEWS]; /* the input of a view's arguments + 1 (a column's: its table's), or 0 */
    PureShape shape[PURE_MODULE_SHAPES];
    uint8_t shapes;
    PureRange range[PURE_MODULE_RANGES];
    uint8_t ranges;
    uint8_t view_range[PURE_MODULE_VIEWS]; /* a table's filter's PureRange + 1, or 0 */
    PureHandler handler[PURE_MODULE_HANDLERS];
    /* The code and the text constants, sized to the module in one block
     * (storage) that load takes and pure_module_release gives back. */
    PureOp *code;
    uint32_t *constant_offset, *constant_length;
    unsigned char *constant_data;
    void *storage;
    char record_names[PURE_MODULE_RECORDS][PURE_MODULE_NAME];
    PureRecord record[PURE_MODULE_RECORDS];
    char record_field_names[PURE_MODULE_RECORD_FIELDS][PURE_MODULE_NAME];
    uint8_t record_field_text[PURE_MODULE_RECORD_FIELDS];
    double record_field_default[PURE_MODULE_RECORD_FIELDS]; /* number, or text constant index */
} PureModule;

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
 * across nodes must agree on it (link/link.h). */
uint32_t pure_module_shape(const PureModule *, uint32_t input);
/* The index state a number view shows as its position (a view that is just
 * `at.position`), or -1: what a host moves with pure_app_point. */
int pure_module_position(const PureModule *, uint32_t view);
/* The text state a text view shows as it is (a view that is just the
 * state), or -1: a host may lend its stored extents instead of copying. */
int pure_module_text_state(const PureModule *, uint32_t view);

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
    /* Optional, with state and save: the directory where the app's lists are
     * kept, their images and the log of their changes (core/log.h); without
     * one, list states are not stored. */
    const char *(*directory)(void *context);
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
/* The runtime's lists' places among the stored lists' records (PureApp):
 * the numbers heard, and the outbox. */
enum { PURE_LIST_HEARD = PURE_MODULE_STATES, PURE_LIST_OUTBOX, PURE_LISTS_STORED };
typedef struct {
    const PureModule *module;
    PureData data;
    double values[PURE_MODULE_STATES]; /* numbers, cursor byte positions and list indices */
    PureDraft *texts[PURE_MODULE_STATES];
    PureList *lists[PURE_MODULE_STATES];
    PureUndo undo; /* list changes of the current event */
    PureDraft *list_drafts[PURE_MODULE_STATES]; /* lists the tree kept before images, until emptied */
    /* Stored lists (module-save.c), by state, then the runtime's (the heard
     * list and the outbox): the save each one's image holds, the first save
     * logged since that changed it (0: none), and the bytes of its changes
     * logged since; the log, the tree's number of its last save, the last
     * save's number, and a checkpoint being written. */
    uint64_t image_seq[PURE_LISTS_STORED], logged_since[PURE_LISTS_STORED];
    size_t logged[PURE_LISTS_STORED];
    struct PureLog *log;
    PureDraft *stored_seq;
    uint64_t seq;
    struct PureCheckpoint *checkpoint;
    struct PureStartLog *start_log; /* the log as read at a start, while lists open */
    bool background_checkpoints;
    bool uncertain; /* a save may or may not be stored: none follows until a start */
    PureDraft *stored_values;                   /* numbers, cursors and indices, when kept */
    PureVersion saved[PURE_MODULE_STATES];      /* text and list versions at the last save */
    /* Events since the last save: what a failed save returns to, and the
     * documents messages opened (held until the save). */
    bool deferred, open;
    double durable_values[PURE_MODULE_STATES];
    PureVersion durable_texts[PURE_MODULE_STATES];
    size_t durable_lists[PURE_MODULE_STATES]; /* each stored list's journal length */
    uint64_t durable_ticks, durable_events;
    struct PurePending { void *document; char key[65]; bool edited; } *pending; /* grows to PURE_APP_PENDING */
    unsigned pending_count, pending_capacity;
    /* The pending documents by key, so a message finds its key in one look:
     * open addressing over the key's hash, a slot holding one more than the
     * document's place in pending (0: none). */
    uint16_t pending_index[2 * PURE_APP_PENDING];
    bool document_pending; /* the current event's document is among them */
    PureFault fault;
    uint32_t fault_pc;
    size_t memory; /* the budget of what the states hold (0: none): pure_app_memory */
    char why[128]; /* why the last start or replace was refused, when it says (else empty; a failed start keeps it) */
    uint32_t counted; /* the states it counts, a bit each: lists (not a message's rows) and texts */
    uint64_t ticks, events;
    /* The time events see as now, in milliseconds since 1970 (UTC). The host
     * sets it, so a script can replay any time; views never see it. */
    double now;
    uint64_t changes; /* counts every change of states, never back: what a filter's rows are fresh for */
    struct PureRows { uint32_t table; uint64_t changes, given; size_t *items, count, capacity; } *rows; /* a filtered table's items */
    unsigned row_tables;
    /* Current message or text event, kept until pure_app_finish. */
    void *document;
    PureSpan payload;
    bool has_reply;
    PureBytes reply;
    unsigned char *copied;
    int64_t revision;
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
    uint64_t last_sent[PURE_MODULE_INPUTS], saved_sent[PURE_MODULE_INPUTS];
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
    uint32_t argument_spans[PURE_MODULE_FIELDS][2];
    uint64_t arguments_given;
    /* Passes over lists that answer shaped folds, by the list's version. */
    struct PurePass *passes;
    unsigned pass_count;
    struct PureSnapshot *snapshot; /* the newest snapshot made, or NULL */
    PureRetired retired;           /* list items let go since it was made */
    bool reading;                  /* a reader: its states are a snapshot's */
} PureApp;

PureResult pure_app_start(PureApp *, const PureModule *, PureData data);
/* Publish every change since the last save in one commit, or undo them all. */
PureResult pure_app_save(PureApp *);
/* Defer saving to pure_app_save (false: save after each event). Needs a data
 * host that saves documents together (PureData.save). */
bool pure_app_defer(PureApp *, bool deferred);
/* Whether key's document is in the unsaved burst: a message for it would
 * save the burst first. */
bool pure_app_pending(const PureApp *, const char *key);
/* Leaves unsaved frame changes unsaved: call pure_app_save first to keep them. */
void pure_app_stop(PureApp *);
PureResult pure_app_frame(PureApp *);
/* A timer input's tick: an event saved like a key's. */
PureResult pure_app_tick(PureApp *, uint32_t input);
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
/* The same, with the variable fields where the host found them: field f
 * starts at placed[2f] in payload and has placed[2f + 1] bytes (entries of
 * fixed fields are not read, nor are the variable fields' lengths in the
 * prefix). A host that parsed a request in place passes the fields' bytes
 * where they are. */
PureResult pure_app_message_fields(PureApp *, uint32_t input, const char *key, int64_t base, PureSpan payload,
                                   const uint32_t *placed);
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
void pure_snapshot_release(PureSnapshot *);
/* Show a snapshot's states in reader, a zeroed app or a reader before: its
 * views read them while the snapshot is held, with the reader's own arena and
 * row cache (one thread at a time). It applies no events. pure_app_stop frees
 * the reader and leaves the snapshot alone. */
void pure_app_read(PureApp *reader, const PureSnapshot *);
#endif
