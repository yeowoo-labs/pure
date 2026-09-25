#ifndef PURE_MODULE_H
#define PURE_MODULE_H
#include "flow.h"
#include "result.h"
#include "state.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Pure module format v2 (docs/design/008-module-format.md). A toolchain such
 * as aup produces it; Pure verifies it completely before anything executes:
 * operand types, stack heights, forward-only branches, names and effects.
 * Fixed capacity: loading and running allocate nothing except a copied reply. */
enum {
    PURE_MODULE_STATES = 32, PURE_MODULE_INPUTS = 8, PURE_MODULE_VIEWS = 24,
    PURE_MODULE_HANDLERS = 64, PURE_MODULE_CODE = 512, PURE_MODULE_STACK = 64,
    PURE_MODULE_NAME = 32, PURE_MODULE_STRINGS = 32768, PURE_MODULE_HEADER = 32,
    PURE_MODULE_FIELDS = 32, PURE_MODULE_COLLECTIONS = 1
};

typedef enum {
    PURE_OP_CONST, PURE_OP_LOAD, PURE_OP_DT, PURE_OP_ADD, PURE_OP_SUB, PURE_OP_MUL,
    PURE_OP_DIV, PURE_OP_NEG, PURE_OP_LT, PURE_OP_LE, PURE_OP_GT, PURE_OP_GE,
    PURE_OP_EQ, PURE_OP_NE, PURE_OP_AND, PURE_OP_OR, PURE_OP_NOT, PURE_OP_MIN,
    PURE_OP_MAX, PURE_OP_CLAMP, PURE_OP_STORE, PURE_OP_JZ, PURE_OP_JMP,
    PURE_OP_FIELD, PURE_OP_DOC, PURE_OP_DOC_SIZE, PURE_OP_LEN, PURE_OP_REPLACE,
    PURE_OP_REPLY, PURE_OP_REQUIRE, PURE_OP_COUNT
} PureOpcode;

typedef enum { PURE_KIND_KEY = 1, PURE_KIND_MESSAGE = 2 } PureInputKind;
typedef enum { PURE_FIELD_U32 = 1, PURE_FIELD_BYTES = 2 } PureFieldType;
typedef enum { PURE_EDGE_DOWN, PURE_EDGE_UP } PureEdge;
typedef enum {
    PURE_FAULT_NONE, PURE_FAULT_DIVIDE, PURE_FAULT_NON_FINITE, PURE_FAULT_CLAMP,
    PURE_FAULT_LIMIT, PURE_FAULT_NOT_FOUND
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

typedef struct {
    uint16_t states, inputs, views, handlers, fields, collections;
    uint32_t code_count;
    char state_names[PURE_MODULE_STATES][PURE_MODULE_NAME];
    char input_names[PURE_MODULE_INPUTS][PURE_MODULE_NAME];
    char view_names[PURE_MODULE_VIEWS][PURE_MODULE_NAME];
    char collection_names[PURE_MODULE_COLLECTIONS][PURE_MODULE_NAME];
    char input_keys[PURE_MODULE_INPUTS][PURE_MODULE_NAME];
    PureInputShape input[PURE_MODULE_INPUTS];
    PureField field[PURE_MODULE_FIELDS];
    double initial[PURE_MODULE_STATES];
    PureBlock view_code[PURE_MODULE_VIEWS];
    PureHandler handler[PURE_MODULE_HANDLERS];
    PureOp code[PURE_MODULE_CODE];
} PureModule;

/* Decode and verify. On failure out is unspecified and error names the rule. */
PureResult pure_module_load(const void *bytes, size_t size, PureModule *out,
                            char *error, size_t error_size);
/* Index of a name, or -1. */
int pure_module_state(const PureModule *, const char *name);
int pure_module_input(const PureModule *, const char *name);
int pure_module_view(const PureModule *, const char *name);

/* Keyed collections live in a data host (for example the tree store). One
 * event opens at most one document, the message's key, at its current
 * version; edits stay in that working version until the event commits it. */
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
} PureData;

/* A byte value: a range of the event's payload, of a document version, or of
 * a reply copied before commit. Internal to one event. */
typedef struct { uint8_t source; PureVersion version; uint64_t offset, length; } PureBytes;

/* One app instance: state values of a module and at most one open event.
 * Every event is atomic: on failure states and documents are unchanged. */
typedef struct {
    const PureModule *module;
    PureData data;
    double values[PURE_MODULE_STATES];
    PureFault fault;
    uint32_t fault_pc;
    uint64_t ticks, events;
    /* Current message event, kept until pure_app_finish. */
    void *document;
    PureSpan payload;
    bool has_reply;
    PureBytes reply;
    unsigned char *copied;
    int64_t revision;
} PureApp;

void pure_app_start(PureApp *, const PureModule *, PureData data);
PureResult pure_app_frame(PureApp *);
PureResult pure_app_input(PureApp *, uint32_t input, PureEdge);
/* Deliver a message. base is the revision the sender edited, or -1 for none;
 * a handler that edits the document needs one. payload stays alive until
 * pure_app_finish. On return app->revision is the document's revision. */
PureResult pure_app_message(PureApp *, uint32_t input, const char *key, int64_t base, PureSpan payload);
/* Visit the reply of the last message in order, without copying. */
PureResult pure_app_reply(const PureApp *, bool (*visit)(void *, PureSpan), void *context);
void pure_app_finish(PureApp *);
bool pure_app_view(const PureApp *, uint32_t view, double *out);
/* Switch to another verified module. States keep their value by name; new
 * states start at their initial value. The module must outlive the app. */
void pure_app_replace(PureApp *, const PureModule *);
#endif
