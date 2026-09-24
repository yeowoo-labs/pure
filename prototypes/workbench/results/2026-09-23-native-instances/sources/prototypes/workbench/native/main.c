#include "app.h"
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const PureScalarPlan pure_app_plan;

static void string(const char *value) {
    putchar('"');
    for (const unsigned char *p = (const unsigned char *)value; *p; ++p) {
        if (*p == '"' || *p == '\\') { putchar('\\'); putchar(*p); }
        else if (*p < 32) printf("\\u%04x", *p);
        else putchar(*p);
    }
    putchar('"');
}
static void number(double value) {
    if (isfinite(value)) printf("%.17g", value); else fputs("null", stdout);
}
static void show(PureNativeApp *app, PureOutcome result) {
    printf("{\"status\":%u,\"scalar_error\":%u,\"revision\":\"%" PRId64 "\",\"stored_revision\":\"%" PRId64
           "\",\"result_revision\":\"%" PRId64 "\",\"held\":%u,\"states\":{",
           (unsigned)result.status, app->last_scalar_error, app->revision, app->stored_revision, result.revision, app->held);
    for (uint32_t i = 0; i < app->plan->state_count; ++i) {
        if (i) putchar(',');
        string(app->plan->states[i].name); putchar(':'); number(pure_scalar_get_state(app->scalar, i));
    }
    fputs("},\"views\":{", stdout);
    pure_scalar_begin_evaluation(app->scalar, 2048);
    uint32_t display_error = 0;
    for (uint32_t i = 0; i < app->plan->view_count; ++i) {
        const PureScalarView *view = &app->plan->views[i];
        if (i) putchar(',');
        string(view->id); fputs(":{", stdout);
        const char *fields[] = {"x", "y", !strcmp(view->type, "text") ? "value" : !strcmp(view->type, "circle") ? "radius" : "width", "height"};
        for (uint32_t j = 0; j < view->count; ++j) {
            if (j) putchar(',');
            string(fields[j]); putchar(':');
            double value = pure_scalar_eval_bounded(app->scalar, view->fields[j].start, view->fields[j].length);
            uint32_t code = pure_scalar_evaluation_info(app->scalar, 0);
            if (code && !display_error) display_error = code;
            if (j >= 2 && strcmp(view->type, "text") && value < 0) { display_error = 100; value = NAN; }
            number(value);
        }
        putchar('}');
    }
    printf("},\"display_error\":%u}\n", display_error);
    fflush(stdout);
}
static bool real(const char *text, double *out) {
    if (!text || !*text) return false;
    char *end;
    errno = 0;
    double value = strtod(text, &end);
    if (errno || *end || !isfinite(value)) return false;
    *out = value;
    return true;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fputs("pure-native STORE_DIRECTORY DOCUMENT_KEY\n"
              "Commands: show, key INPUT down|up, value INPUT NUMBER,\n"
              "point INPUT down|move|up|cancel TARGET X Y, frame, reset, save, load, quit.\n", stderr);
        return 2;
    }
    size_t bytes = pure_scalar_bytes();
    void *memory = malloc(bytes);
    if (!memory) return 2;
    PureNativeApp app;
    PureResult status = pure_native_app_init(&app, &pure_app_plan, memory, bytes, argv[1], argv[2], 4096);
    if (status != PURE_OK) { fprintf(stderr, "initialization failed: %u\n", status); free(memory); return 2; }
    show(&app, (PureOutcome){.status = PURE_OK});
    int failed = 0;
    char line[512];
    while (fgets(line, sizeof(line), stdin)) {
        if (!strchr(line, '\n') && !feof(stdin)) {
            int c;
            while ((c = getchar()) != '\n' && c != EOF) {}
            fputs("command exceeds 510 bytes\n", stderr); failed = 1; continue;
        }
        char *words[8];
        size_t count = 0;
        for (char *token = strtok(line, " \t\r\n"); token; token = strtok(NULL, " \t\r\n")) {
            if (count == 8) break;
            words[count++] = token;
        }
        if (!count) continue;
        PureOutcome result = {.status = PURE_INVALID};
        if (count == 1) {
            if (!strcmp(words[0], "quit")) break;
            if (!strcmp(words[0], "show")) result.status = PURE_OK;
            else if (!strcmp(words[0], "save")) result = pure_native_app_save(&app);
            else if (!strcmp(words[0], "load")) result = pure_native_app_restore(&app);
            else if (!strcmp(words[0], "frame") || !strcmp(words[0], "reset"))
                result = pure_native_app_action(&app, (PureAction){.phase = !strcmp(words[0], "frame") ? PURE_FRAME : PURE_RESET});
        } else {
            uint32_t input = app.plan->input_count;
            for (uint32_t i = 0; i < app.plan->input_count; ++i)
                if (!strcmp(words[1], app.plan->inputs[i].name)) { input = i; break; }
            if (input < app.plan->input_count) {
                PureScalarInputKind kind = app.plan->inputs[input].kind;
                PureAction action = {.input = input, .phase = UINT32_MAX, .target = UINT32_MAX};
                if (count == 3 && !strcmp(words[0], "key") && kind == PURE_KEY) {
                    if (!strcmp(words[2], "down")) action.phase = PURE_DOWN;
                    if (!strcmp(words[2], "up")) action.phase = PURE_UP;
                } else if (count == 3 && !strcmp(words[0], "value") && kind == PURE_VALUE) {
                    if (real(words[2], &action.x)) action.phase = PURE_CHANGE;
                } else if (count == 6 && !strcmp(words[0], "point") && (kind == PURE_POINT || kind == PURE_ROUTED)) {
                    bool target = kind == PURE_POINT && !strcmp(words[3], app.plan->inputs[input].target);
                    if (kind == PURE_ROUTED)
                        for (uint32_t i = 0; i < app.plan->target_count; ++i)
                            if (app.plan->targets[i].input == input && !strcmp(words[3], app.plan->targets[i].view)) {
                                target = true; action.target = i; break;
                            }
                    if (target && real(words[4], &action.x) && real(words[5], &action.y)) {
                        const char *phases[] = {"down", "up", "move", "cancel"};
                        for (uint32_t i = 0; i < 4; ++i) if (!strcmp(words[2], phases[i])) action.phase = i;
                    }
                }
                if (action.phase != UINT32_MAX) result = pure_native_app_action(&app, action);
            }
        }
        if (result.status != PURE_OK) failed = 1;
        show(&app, result);
    }
    if (ferror(stdin) || ferror(stdout)) failed = 1;
    pure_runtime_stop(&app.execution);
    if (app.execution.active || app.execution.retained) failed = 1;
    free(memory);
    return failed;
}
