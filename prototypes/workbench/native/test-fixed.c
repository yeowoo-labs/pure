#include "plan.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

extern const PureScalarPlan pure_app_plan;

int main(void) {
    size_t bytes = pure_scalar_bytes();
    /* This authored three-target app is the selected repeated-instance cost. */
    assert(bytes <= 512);
    void *first_memory = malloc(bytes), *second_memory = malloc(bytes);
    assert(first_memory && second_memory);
    PureScalar *first = pure_scalar_initialize(first_memory, bytes);
    PureScalar *second = pure_scalar_initialize(second_memory, bytes);
    assert(first && second);
    assert(pure_scalar_info(first, 5) == 0 && pure_scalar_info(first, 6) == 0);
    assert(pure_scalar_layout(first, 4) == 0 && pure_scalar_layout(first, 5) == 0);
    assert(isnan(pure_scalar_get_state(first, 0)) && isnan(pure_scalar_eval_at(first, 0, 0)));
    assert(pure_scalar_dispatch(first, 1, 0) == 9);
    assert(pure_scalar_dispatch(first, 0, 1.0 / 60.0) == 0);
    assert(pure_scalar_info(first, 0) == 1 && pure_scalar_info(first, 9) == 1);
    pure_scalar_reset_state(first);
    assert(pure_scalar_info(first, 0) == 0 && pure_scalar_info(first, 9) == 0);
    assert(first && second && pure_scalar_load(first, &pure_app_plan, NULL, NULL) == 0);
    assert(pure_scalar_load(second, &pure_app_plan, NULL, NULL) == 0);
    assert(pure_scalar_layout(first, 0) == bytes && pure_scalar_layout(first, 1) > bytes);
    assert(pure_scalar_get_state(first, 0) == 0 && pure_scalar_get_state(second, 0) == 0);
    assert(pure_scalar_evaluation_info(first, 8) == 0);
    assert(pure_scalar_configure_evaluation(first, 1, 1) == 1);
    assert(pure_scalar_configure_evaluation(first, 0, 0) == 0);

    /* Two state vectors execute the same compiled program independently. */
    assert(pure_scalar_dispatch_target(first, 9, 0, 90, 130) == 0);
    assert(pure_scalar_get_state(first, 0) == 1 && pure_scalar_get_state(second, 0) == 0);
    uint32_t before = pure_scalar_info(first, 4);
    double values[32] = {0};
    for (uint32_t i = 0; i < pure_app_plan.state_count; ++i)
        values[i] = pure_scalar_get_state(first, i);

    PureScalarPlan wrong = pure_app_plan;
    wrong.identity[0] ^= 1;
    assert(pure_scalar_load(first, &wrong, NULL, NULL) == 2);
    wrong = pure_app_plan;
    wrong.instruction_count++;
    assert(pure_scalar_load(first, &wrong, NULL, NULL) == 2);
    assert(pure_scalar_load(first, &pure_app_plan, &pure_app_plan, NULL) == 2);
    values[0] = NAN;
    assert(pure_scalar_load(first, &pure_app_plan, NULL, values) == 5);
    assert(pure_scalar_info(first, 4) == before && pure_scalar_get_state(first, 0) == 1);
    values[0] = -0.0;
    assert(pure_scalar_load(second, &pure_app_plan, NULL, values) == 0);
    assert(signbit(pure_scalar_get_state(second, 0)) && pure_scalar_get_state(first, 0) == 1);
    assert(pure_scalar_begin_program(first, pure_app_plan.state_count, pure_app_plan.input_count) == 1);
    assert(pure_scalar_info(first, 4) == before && pure_scalar_get_state(first, 0) == 1);
    pure_scalar_reset_all(first);
    assert(pure_scalar_info(first, 5) == 0);
    assert(pure_scalar_load(first, &pure_app_plan, NULL, NULL) == 0);
    assert(pure_scalar_get_state(first, 0) == 0);

    printf("{\"native_instances\":2,\"scalar_instance_bytes\":%zu,"
           "\"shared_program_bytes\":%u,\"checks\":"
           "\"empty-state-independent-state-identity-capacity-atomic-restore-signed-zero-reset\"}\n",
           bytes, pure_scalar_layout(first, 1));
    free(first_memory); free(second_memory);
    return 0;
}
