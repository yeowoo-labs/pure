#ifndef PURE_SCALAR_PLAN_H
#define PURE_SCALAR_PLAN_H
#include "scalar.h"
#include <stdbool.h>

typedef struct { const char *name; double initial; } PureScalarState;
typedef struct { uint32_t op, arg; double value; } PureScalarInstruction;
typedef struct { uint32_t event, start, length, base, count; } PureScalarHandler;
typedef struct { uint32_t input, base, count; const char *view; } PureScalarTarget;
typedef enum { PURE_KEY, PURE_POINT, PURE_ROUTED, PURE_VALUE } PureScalarInputKind;
typedef struct { const char *name, *key, *target; PureScalarInputKind kind; } PureScalarInput;
typedef struct { uint32_t start, length; } PureScalarBinding;
typedef struct {
    const char *id, *type, *decoration;
    uint32_t count;
    PureScalarBinding fields[4];
} PureScalarView;
/* Immutable, build-prepared application data. In fixed CLI/peer builds,
 * instructions and handlers are NULL: the same validated program is linked
 * once into the scalar module. Other plan fields remain available to hosts.
 * C layout is an internal build contract; no pointers or native structs are
 * persisted or sent on the wire. */
typedef struct {
    uint32_t state_count, input_count, instruction_count, handler_count, target_count, view_count;
    const PureScalarState *states;
    const PureScalarInput *inputs;
    const PureScalarInstruction *instructions;
    const PureScalarHandler *handlers;
    const PureScalarTarget *targets;
    const PureScalarView *views;
    unsigned char identity[32];
} PureScalarPlan;

/* Commit a prepared definition with initials, name-preserved previous values,
 * or a complete restored state vector. A failed candidate leaves active state
 * intact. The caller ensures the application is idle before replacing it. */
uint32_t pure_scalar_load(PureScalar *, const PureScalarPlan *,
                          const PureScalarPlan *previous, const double *restored);
#endif
