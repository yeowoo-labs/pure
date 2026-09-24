#include "plan.h"
#include <string.h>

uint32_t pure_scalar_load(PureScalar *vm, const PureScalarPlan *plan,
                          const PureScalarPlan *previous, const double *restored) {
    uint32_t code = pure_scalar_begin_program(vm, plan->state_count, plan->input_count);
    if (code) return code;
    for (uint32_t i = 0; i < plan->state_count; ++i) {
        int32_t old = -1;
        if (!restored && previous)
            for (uint32_t j = 0; j < previous->state_count; ++j)
                if (!strcmp(plan->states[i].name, previous->states[j].name)) { old = (int32_t)j; break; }
        code = pure_scalar_put_state(vm, i, plan->states[i].initial, old);
        if (code) goto fail;
        if (restored && (code = pure_scalar_put_restored_value(vm, i, restored[i]))) goto fail;
    }
    for (uint32_t i = 0; i < plan->instruction_count; ++i) {
        const PureScalarInstruction *op = &plan->instructions[i];
        if ((code = pure_scalar_put_instruction(vm, op->op, op->arg, op->value))) goto fail;
    }
    for (uint32_t i = 0; i < plan->target_count; ++i) {
        const PureScalarTarget *target = &plan->targets[i];
        if ((code = pure_scalar_put_pointer_target(vm, target->input, target->base, target->count))) goto fail;
    }
    for (uint32_t i = 0; i < plan->handler_count; ++i) {
        const PureScalarHandler *handler = &plan->handlers[i];
        if ((code = pure_scalar_put_handler(vm, handler->event, handler->start, handler->length))) goto fail;
        if (handler->count) {
            code = handler->base == 255 ? pure_scalar_bind_target_handler(vm, i, handler->count)
                 : pure_scalar_bind_handler(vm, i, handler->base, handler->count);
            if (code) goto fail;
        }
    }
    return restored ? pure_scalar_commit_restored_program(vm) : pure_scalar_commit_program(vm);
fail:
    pure_scalar_abort_program(vm);
    return code;
}
