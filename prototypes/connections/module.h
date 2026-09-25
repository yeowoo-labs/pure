#ifndef PURE_MODULE_H
#define PURE_MODULE_H
#include "result.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Pure module format v1 (docs/design/008-module-format.md). A toolchain such
 * as aup produces it; Pure verifies it completely before anything executes, so
 * a loaded module cannot overflow the stack, branch backwards or name a
 * missing state. Fixed capacity: no allocation in load, run or replace. */
enum {
    PURE_MODULE_STATES = 32, PURE_MODULE_INPUTS = 8, PURE_MODULE_VIEWS = 24,
    PURE_MODULE_HANDLERS = 64, PURE_MODULE_CODE = 512, PURE_MODULE_STACK = 64,
    PURE_MODULE_NAME = 32, PURE_MODULE_STRINGS = 32768, PURE_MODULE_HEADER = 32
};

typedef enum {
    PURE_OP_CONST, PURE_OP_LOAD, PURE_OP_DT, PURE_OP_ADD, PURE_OP_SUB, PURE_OP_MUL,
    PURE_OP_DIV, PURE_OP_NEG, PURE_OP_LT, PURE_OP_LE, PURE_OP_GT, PURE_OP_GE,
    PURE_OP_EQ, PURE_OP_NE, PURE_OP_AND, PURE_OP_OR, PURE_OP_NOT, PURE_OP_MIN,
    PURE_OP_MAX, PURE_OP_CLAMP, PURE_OP_STORE, PURE_OP_JZ, PURE_OP_JMP, PURE_OP_COUNT
} PureOpcode;

typedef enum { PURE_EDGE_DOWN, PURE_EDGE_UP } PureEdge;
typedef enum {
    PURE_FAULT_NONE, PURE_FAULT_DIVIDE, PURE_FAULT_NON_FINITE, PURE_FAULT_CLAMP
} PureFault;

typedef struct { double value; uint32_t arg; uint8_t op; } PureOp;
typedef struct { uint32_t start, length; } PureBlock;
typedef struct { uint32_t event; PureBlock code; } PureHandler;

typedef struct {
    uint16_t states, inputs, views, handlers;
    uint32_t code_count;
    char state_names[PURE_MODULE_STATES][PURE_MODULE_NAME];
    char input_names[PURE_MODULE_INPUTS][PURE_MODULE_NAME];
    char view_names[PURE_MODULE_VIEWS][PURE_MODULE_NAME];
    char input_keys[PURE_MODULE_INPUTS][PURE_MODULE_NAME];
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

/* One app instance: state values of a module. Every event is atomic: on a
 * fault the values are exactly those before the event. */
typedef struct {
    const PureModule *module;
    double values[PURE_MODULE_STATES];
    PureFault fault;
    uint32_t fault_pc;
    uint64_t ticks, events;
} PureApp;

void pure_app_start(PureApp *, const PureModule *);
PureResult pure_app_frame(PureApp *);
PureResult pure_app_input(PureApp *, uint32_t input, PureEdge);
bool pure_app_view(const PureApp *, uint32_t view, double *out);
/* Switch to another verified module. States keep their value by name; new
 * states start at their initial value. The module must outlive the app. */
void pure_app_replace(PureApp *, const PureModule *);
#endif
