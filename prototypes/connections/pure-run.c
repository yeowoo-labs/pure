#define _POSIX_C_SOURCE 200809L
#include "module.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Test host for a verified module: reads commands from stdin and prints one
 * JSON snapshot per command. Not a product interface.
 *   down NAME | up NAME | frame [N] | replace PATH | show */
static const char *const fault_names[] = {"none", "divide", "non_finite", "clamp"};

static bool load(const char *path, PureModule *module) {
    FILE *file = fopen(path, "rb");
    static unsigned char bytes[1 << 17];
    size_t size = file ? fread(bytes, 1, sizeof(bytes), file) : 0;
    bool ok = file && !ferror(file) && size < sizeof(bytes);
    if (file) fclose(file);
    char error[128];
    if (!ok) { fprintf(stderr, "cannot read %s\n", path); return false; }
    if (pure_module_load(bytes, size, module, error, sizeof(error)) != PURE_OK) {
        fprintf(stderr, "%s: %s\n", path, error);
        return false;
    }
    return true;
}

static void show(const PureApp *app, const char *failure) {
    const PureModule *m = app->module;
    printf("{\"states\":{");
    for (uint32_t i = 0; i < m->states; ++i)
        printf("%s\"%s\":%.17g", i ? "," : "", m->state_names[i], app->values[i]);
    printf("},\"views\":{");
    for (uint32_t i = 0; i < m->views; ++i) {
        double value;
        if (pure_app_view(app, i, &value)) printf("%s\"%s\":%.17g", i ? "," : "", m->view_names[i], value);
        else printf("%s\"%s\":null", i ? "," : "", m->view_names[i]);
    }
    printf("},\"tick\":%llu,\"successfulEvents\":%llu,\"fault\":\"%s\",\"error\":%s%s%s}\n",
           (unsigned long long)app->ticks, (unsigned long long)app->events, fault_names[app->fault],
           failure ? "\"" : "", failure ? failure : "null", failure ? "\"" : "");
}

int main(int argc, char **argv) {
    static PureModule modules[2];
    if (argc != 2 || !load(argv[1], &modules[0])) {
        if (argc != 2) fputs("pure-run MODULE < commands\n", stderr);
        return 2;
    }
    unsigned active = 0;
    PureApp app;
    pure_app_start(&app, &modules[active], (PureData){0});
    char line[4096];
    int status = 0;
    while (fgets(line, sizeof(line), stdin)) {
        char command[16] = "", argument[4000] = "";
        const char *failure = NULL;
        line[strcspn(line, "\r\n")] = 0;
        if (sscanf(line, "%15s %3999s", command, argument) < 1) continue;
        if (!strcmp(command, "down") || !strcmp(command, "up")) {
            int input = pure_module_input(app.module, argument);
            if (input < 0) failure = "unknown input";
            else if (pure_app_input(&app, (uint32_t)input, command[0] == 'd' ? PURE_EDGE_DOWN : PURE_EDGE_UP) != PURE_OK)
                failure = "event failed";
        } else if (!strcmp(command, "frame")) {
            long count = *argument ? strtol(argument, NULL, 10) : 1;
            if (count < 1 || count > 600) failure = "frame count";
            for (long i = 0; !failure && i < count; ++i)
                if (pure_app_frame(&app) != PURE_OK) failure = "event failed";
        } else if (!strcmp(command, "replace")) {
            if (!load(argument, &modules[!active])) failure = "invalid module";
            else { active = !active; pure_app_replace(&app, &modules[active]); }
        } else if (strcmp(command, "show")) failure = "unknown command";
        if (failure) status = 1;
        show(&app, failure);
    }
    return status;
}
