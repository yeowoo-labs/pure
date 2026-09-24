#define _POSIX_C_SOURCE 200809L
#include "app.h"
#include "../../connections/store.h"
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern const PureScalarPlan pure_app_plan;
static void move(PureNativeApp *app, uint32_t target, double x, double y) {
    assert(pure_native_app_action(app, (PureAction){.input = 4, .phase = PURE_DOWN, .target = target, .x = x, .y = y}).status == PURE_OK);
    assert(pure_native_app_action(app, (PureAction){.input = 4, .phase = PURE_MOVE, .target = target, .x = x + 70, .y = y + 40}).status == PURE_OK);
    assert(pure_native_app_action(app, (PureAction){.input = 4, .phase = PURE_UP, .target = target, .x = x + 70, .y = y + 40}).status == PURE_OK);
}
static void drained(PureNativeApp *app) {
    assert(app->execution.active == 0 && app->execution.retained == 0);
    assert(app->execution.work == NULL && app->execution.capacity == 0);
    assert(app->execution.peak_active <= 2 && app->execution.peak_retained <= app->execution.budget);
}
static void values(PureNativeApp *app, double output[32]) {
    for (uint32_t i = 0; i < app->plan->state_count; ++i) output[i] = pure_scalar_get_state(app->scalar, i);
}
static void same(PureNativeApp *app, const double *expected) {
    for (uint32_t i = 0; i < app->plan->state_count; ++i) assert(pure_scalar_get_state(app->scalar, i) == expected[i]);
}

int main(void) {
    size_t size = pure_scalar_bytes();
    unsigned char *a = malloc(size), *b = malloc(size), *c = malloc(size);
    assert(a && b && c);
    assert(!pure_scalar_initialize(NULL, size));
    assert(!pure_scalar_initialize(a, size - 1));
    assert(pure_scalar_alignment() > 1 && !pure_scalar_initialize(a + 1, size));
    char path[] = "/tmp/pure-native-instance-XXXXXX";
    assert(mkdtemp(path));
    PureNativeApp first, second, limited;
    assert(pure_native_app_init(&first, &pure_app_plan, a, size, path, "scene", 4096) == PURE_OK);
    assert(pure_native_app_init(&second, &pure_app_plan, b, size, path, "scene", 4096) == PURE_OK);
    assert(pure_native_app_init(&limited, &pure_app_plan, c, size, path, "limited", sizeof(PureAction) - 1) == PURE_OK);
    assert(pure_native_app_action(&limited, (PureAction){.phase = PURE_FRAME}).status == PURE_BUDGET);
    assert(pure_scalar_info(limited.scalar, 9) == 0);

    assert(pure_native_app_action(&first, (PureAction){.input = 4, .phase = PURE_DOWN, .target = 0, .x = 90, .y = 130}).status == PURE_OK);
    assert(pure_native_app_save(&first).status == PURE_BUSY);
    assert(pure_native_app_action(&first, (PureAction){.input = 4, .phase = PURE_MOVE, .target = 1, .x = 230, .y = 210}).status == PURE_INVALID);
    assert(pure_native_app_action(&first, (PureAction){.input = 4, .phase = PURE_MOVE, .target = 0, .x = 230, .y = 210}).status == PURE_OK);
    move(&second, 1, 290, 170);
    assert(pure_scalar_get_state(second.scalar, 1) == 80 && pure_scalar_get_state(second.scalar, 9) == 350);
    assert(pure_scalar_get_state(first.scalar, 1) == 220 && pure_scalar_get_state(first.scalar, 9) == 280);
    assert(pure_native_app_action(&first, (PureAction){.input = 4, .phase = PURE_UP, .target = 0, .x = 230, .y = 210}).status == PURE_OK);
    assert(first.held == 0 && second.held == 0);
    assert(pure_native_app_save(&first).status == PURE_OK && first.stored_revision == 1);
    double second_values[32] = {0};
    values(&second, second_values);
    PureOutcome conflict = pure_native_app_save(&second);
    assert(conflict.status == PURE_CONFLICT && conflict.revision == 1 && second.stored_revision == 0);
    same(&second, second_values);
    assert(pure_native_app_restore(&second).status == PURE_OK && second.stored_revision == 1);
    assert(pure_scalar_get_state(second.scalar, 1) == 220 && pure_scalar_get_state(second.scalar, 9) == 280);

    /* Staging/error/evaluation state also belongs to the selected instance. */
    assert(pure_scalar_begin_program(first.scalar, 1, 0) == 0);
    assert(pure_scalar_put_state(first.scalar, 0, 999, -1) == 0);
    assert(pure_scalar_configure_evaluation(first.scalar, 2, 2) == 0);
    move(&second, 2, 460, 230);
    assert(pure_scalar_put_instruction(first.scalar, 999, 0, 0) != 0);
    assert(pure_scalar_commit_program(first.scalar) != 0);
    assert(pure_scalar_get_state(first.scalar, 1) == 220);
    assert(pure_scalar_get_state(first.scalar, 17) == 450 && pure_scalar_get_state(second.scalar, 17) == 520);
    assert(pure_native_app_save(&second).status == PURE_OK && second.stored_revision == 2);
    assert(pure_native_app_save(&first).status == PURE_CONFLICT && first.stored_revision == 1);

    /* Mismatched definition and nonfinite restored values never replace state. */
    PureStoredView saved;
    assert(pure_store_get(path, "scene", 300, &saved) == PURE_OK);
    unsigned char invalid[300];
    assert(saved.body.size <= sizeof(invalid));
    size_t length = saved.body.size;
    memcpy(invalid, saved.body.data, length);
    pure_store_release(&saved);
    invalid[8] ^= 1;
    int64_t revision;
    assert(pure_store_put(path, "scene", 2, (PureSpan){invalid, length}, &revision) == PURE_OK);
    double first_values[32] = {0};
    values(&first, first_values);
    assert(pure_native_app_restore(&first).status == PURE_INVALID && first.stored_revision == 1);
    same(&first, first_values);
    invalid[8] ^= 1;
    const unsigned char nan[8] = {0,0,0,0,0,0,248,127};
    memcpy(invalid + 44, nan, 8);
    assert(pure_store_put(path, "scene", 3, (PureSpan){invalid, length}, &revision) == PURE_OK);
    assert(pure_native_app_restore(&first).status == PURE_INVALID && first.last_scalar_error == 5);
    same(&first, first_values);
    assert(first.stored_revision == 1);
    assert(pure_native_app_action(&first, (PureAction){.input = 4, .phase = PURE_DOWN, .target = 0, .source = 11, .x = 230, .y = 210}).status == PURE_OK);
    assert(pure_native_app_action(&first, (PureAction){.input = 4, .phase = PURE_DOWN, .target = 1, .source = 22, .x = 290, .y = 170}).status == PURE_OK);
    assert(pure_native_app_action(&first, (PureAction){.input = 4, .phase = PURE_MOVE, .target = 0, .source = 22}).status == PURE_INVALID);
    assert(pure_native_app_action(&first, (PureAction){.input = 4, .phase = PURE_CANCEL, .target = 0, .source = 11}).status == PURE_OK);
    assert(first.held && pure_native_app_save(&first).status == PURE_BUSY);
    assert(pure_native_app_action(&first, (PureAction){.input = 4, .phase = PURE_UP, .target = 1, .source = 22}).status == PURE_OK);
    assert(first.held == 0);
    assert(pure_native_app_action(&first, (PureAction){.input = 1, .phase = PURE_DOWN, .source = 1}).status == PURE_OK);
    double after_key = pure_scalar_get_state(first.scalar, 9);
    assert(pure_native_app_action(&first, (PureAction){.input = 1, .phase = PURE_DOWN, .source = 2}).status == PURE_OK);
    assert(pure_scalar_get_state(first.scalar, 9) == after_key);
    assert(pure_native_app_action(&first, (PureAction){.input = 1, .phase = PURE_UP, .source = 1}).status == PURE_OK && first.held);
    assert(pure_native_app_action(&first, (PureAction){.input = 1, .phase = PURE_UP, .source = 2}).status == PURE_OK && !first.held);

    /* The native editor swaps only a fully accepted definition. Matching state
     * names survive; a new name starts at its declared initial value. */
    PureScalarState changed_states[32];
    memcpy(changed_states, pure_app_plan.states, pure_app_plan.state_count * sizeof(changed_states[0]));
    changed_states[1] = (PureScalarState){"new.green.x", 123};
    PureScalarPlan changed = pure_app_plan;
    changed.states = changed_states;
    changed.identity[0] ^= 1;
    assert(pure_native_app_action(&first, (PureAction){.input = 1, .phase = PURE_DOWN}).status == PURE_OK);
    assert(pure_native_app_replace_plan(&first, &changed) == PURE_BUSY);
    assert(pure_native_app_action(&first, (PureAction){.input = 1, .phase = PURE_UP}).status == PURE_OK);
    double old_y = pure_scalar_get_state(first.scalar, 2);
    int64_t before_replace = first.revision;
    uint64_t before_accept = first.execution.accepted;
    assert(pure_native_app_replace_plan(&first, &changed) == PURE_OK);
    assert(first.execution.accepted == before_accept + 1);
    assert(first.plan == &changed && first.revision == before_replace + 1);
    assert(pure_scalar_get_state(first.scalar, 1) == 123 && pure_scalar_get_state(first.scalar, 2) == old_y);
    PureScalarInstruction invalid_program[512];
    assert(changed.instruction_count <= 512);
    memcpy(invalid_program, changed.instructions, changed.instruction_count * sizeof(invalid_program[0]));
    invalid_program[0].op = 999;
    PureScalarPlan rejected = changed;
    rejected.instructions = invalid_program;
    before_replace = first.revision;
    before_accept = first.execution.accepted;
    assert(pure_native_app_replace_plan(&first, &rejected) == PURE_INVALID && first.last_scalar_error != 0);
    assert(first.execution.accepted == before_accept + 1);
    assert(first.plan == &changed && first.revision == before_replace);
    assert(pure_scalar_get_state(first.scalar, 1) == 123 && pure_scalar_get_state(first.scalar, 2) == old_y);
    drained(&first); drained(&second); drained(&limited);

    char file[256];
    snprintf(file, sizeof(file), "%s/obj-scene", path); assert(unlink(file) == 0);
    snprintf(file, sizeof(file), "%s/.writer.lock", path); assert(unlink(file) == 0);
    assert(rmdir(path) == 0);
    free(a); free(b); free(c);
    printf("{\"native_instances\":3,\"checks\":\"independent-state-staging-inputs-errors-budget-snapshot-conflict-restore-plan-reload\","
           "\"scalar_instance_bytes\":%zu,\"scalar_alignment\":%zu,\"app_controller_bytes\":%zu,\"action_bytes\":%zu,"
           "\"snapshot_bytes\":%zu,\"all_work_drained\":true}\n", size, pure_scalar_alignment(), sizeof(PureNativeApp), sizeof(PureAction), length);
    return 0;
}
