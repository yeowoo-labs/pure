#ifndef PURE_SCALAR_H
#define PURE_SCALAR_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* This selected scalar module has no default instance or allocator. Allocate
 * bytes() aligned to alignment(), initialize once, and retain it through all
 * calls. Each instance has one owner at a time; independent instances share
 * no mutable state. Passing invalid pointers is outside this trusted C ABI.
 * This is an in-process API, not a stable wire or persistent representation. */
typedef struct PureScalar PureScalar;
size_t pure_scalar_bytes(void);
size_t pure_scalar_alignment(void);
PureScalar *pure_scalar_initialize(void *memory, size_t size);
void pure_scalar_reset_all(PureScalar *instance);
uint32_t pure_scalar_begin_program(PureScalar *instance, uint32_t state_count, uint32_t input_count);
void pure_scalar_abort_program(PureScalar *instance);
uint32_t pure_scalar_put_state(PureScalar *instance, uint32_t id, double initial, int32_t old_id);
uint32_t pure_scalar_put_instruction(PureScalar *instance, uint32_t op, uint32_t arg, double value);
uint32_t pure_scalar_put_handler(PureScalar *instance, uint32_t event, uint32_t start, uint32_t length);
uint32_t pure_scalar_bind_handler(PureScalar *instance, uint32_t handler, uint32_t base, uint32_t count);
uint32_t pure_scalar_put_pointer_target(PureScalar *instance, uint32_t input, uint32_t base, uint32_t count);
uint32_t pure_scalar_bind_target_handler(PureScalar *instance, uint32_t handler, uint32_t count);
uint32_t pure_scalar_put_restored_value(PureScalar *instance, uint32_t id, double value);
uint32_t pure_scalar_commit_program(PureScalar *instance);
uint32_t pure_scalar_commit_restored_program(PureScalar *instance);
void pure_scalar_reset_state(PureScalar *instance);
uint32_t pure_scalar_dispatch(PureScalar *instance, uint32_t event, double dt);
uint32_t pure_scalar_dispatch_point(PureScalar *instance, uint32_t event, double x, double y);
uint32_t pure_scalar_dispatch_target(PureScalar *instance, uint32_t event, uint32_t target, double x, double y);
uint32_t pure_scalar_dispatch_value(PureScalar *instance, uint32_t event, double value);
double pure_scalar_eval_at(PureScalar *instance, uint32_t start, uint32_t length);
uint32_t pure_scalar_configure_evaluation(PureScalar *instance, uint32_t cache, uint32_t history);
void pure_scalar_forget_evaluation(PureScalar *instance, uint32_t flags);
void pure_scalar_begin_evaluation(PureScalar *instance, uint32_t limit);
double pure_scalar_eval_bounded(PureScalar *instance, uint32_t start, uint32_t length);
uint32_t pure_scalar_evaluation_info(PureScalar *instance, uint32_t id);
double pure_scalar_get_state(PureScalar *instance, uint32_t id);
uint32_t pure_scalar_info(PureScalar *instance, uint32_t id);
uint32_t pure_scalar_layout(PureScalar *instance, uint32_t id);
#ifdef __cplusplus
}
#endif
#endif
