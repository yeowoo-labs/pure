#ifndef PURE_NATIVE_APP_H
#define PURE_NATIVE_APP_H
#include "plan.h"
#include "../../connections/runtime.h"

typedef enum { PURE_DOWN, PURE_UP, PURE_MOVE, PURE_CANCEL, PURE_CHANGE, PURE_FRAME, PURE_RESET } PureActionPhase;
/* In-process event value, never a persistent or network representation. */
typedef struct { uint32_t input, phase, target, source; double x, y; } PureAction;
typedef struct {
    PureScalar *scalar;
    const PureScalarPlan *plan;
    const char *directory;
    char document[65];
    PureRuntime execution;
    PureWork slots[2];
    int64_t revision, stored_revision;
    uint32_t last_scalar_error, held, pointer_owners[32], key_sources[8];
} PureNativeApp;

/* Caller supplies instance memory and keeps plan/directory alive. This selected
 * host uses the existing SQL-free store. Single owner; effects are synchronous. */
PureResult pure_native_app_init(PureNativeApp *, const PureScalarPlan *, void *memory,
                                size_t size, const char *directory, const char *key, size_t budget);
PureOutcome pure_native_app_action(PureNativeApp *, PureAction);
PureOutcome pure_native_app_save(PureNativeApp *);
PureOutcome pure_native_app_restore(PureNativeApp *);
/* Authoring host only. Candidate plan stays alive on success; previous plan
 * stays alive until this call returns. A failed reload retains the old app. */
#ifdef PURE_NATIVE_AUTHORING
PureResult pure_native_app_replace_plan(PureNativeApp *, const PureScalarPlan *);
#endif
#endif
