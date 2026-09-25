#define _POSIX_C_SOURCE 200809L
#include "module.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Test host for a verified module: reads commands from stdin and prints one
 * JSON snapshot per command. Not a product interface.
 *   down NAME | up NAME | frame [N] | type NAME HEX|- | replace PATH | show
 * Text states and text views print as JSON strings; cursors as byte positions. */
static const char *const fault_names[] = {"none", "divide", "non_finite", "clamp", "limit", "not_found", "encoding"};

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

static bool json_span(void *context, PureSpan span) {
    (void)context;
    for (size_t i = 0; i < span.size; ++i) {
        unsigned char c = span.data[i];
        if (c == '"' || c == '\\') printf("\\%c", c);
        else if (c < 0x20) printf("\\u%04x", c);
        else putchar(c);
    }
    return true;
}

static void show(PureApp *app, const char *failure) {
    const PureModule *m = app->module;
    printf("{\"states\":{");
    for (uint32_t i = 0; i < m->states; ++i) {
        printf("%s\"%s\":", i ? "," : "", m->state_names[i]);
        if (m->state_type[i] == PURE_STATE_TEXT) {
            putchar('"');
            pure_draft_read(app->texts[i], 0, pure_draft_size(app->texts[i]), json_span, NULL);
            putchar('"');
        } else printf("%.17g", app->values[i]);
    }
    printf("},\"views\":{");
    for (uint32_t i = 0; i < m->views; ++i) {
        printf("%s\"%s\":", i ? "," : "", m->view_names[i]);
        double value;
        if (m->view_type[i] == PURE_VIEW_TEXT) {
            putchar('"');
            if (pure_app_view_text(app, i, json_span, NULL) != PURE_OK) printf("\\u0000failed");
            putchar('"');
        } else if (pure_app_view(app, i, &value)) printf("%.17g", value);
        else printf("null");
    }
    printf("},\"tick\":%llu,\"successfulEvents\":%llu,\"fault\":\"%s\",\"error\":%s%s%s}\n",
           (unsigned long long)app->ticks, (unsigned long long)app->events, fault_names[app->fault],
           failure ? "\"" : "", failure ? failure : "null", failure ? "\"" : "");
}

static int hex(char c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

int main(int argc, char **argv) {
    static PureModule modules[2];
    if (argc != 2 || !load(argv[1], &modules[0])) {
        if (argc != 2) fputs("pure-run MODULE < commands\n", stderr);
        return 2;
    }
    unsigned active = 0;
    PureApp app;
    if (pure_app_start(&app, &modules[active], (PureData){0}) != PURE_OK) { fputs("cannot start\n", stderr); return 2; }
    static char line[1 << 16];
    static unsigned char text[1 << 15];
    int status = 0;
    while (fgets(line, sizeof(line), stdin)) {
        char command[16] = "", argument[256] = "", data[sizeof(line)] = "";
        const char *failure = NULL;
        line[strcspn(line, "\r\n")] = 0;
        if (sscanf(line, "%15s %255s %65535s", command, argument, data) < 1) continue;
        if (!strcmp(command, "down") || !strcmp(command, "up")) {
            int input = pure_module_input(app.module, argument);
            if (input < 0) failure = "unknown input";
            else if (pure_app_input(&app, (uint32_t)input, command[0] == 'd' ? PURE_EDGE_DOWN : PURE_EDGE_UP) != PURE_OK)
                failure = "event failed";
        } else if (!strcmp(command, "type")) {
            int input = pure_module_input(app.module, argument);
            size_t length = strcmp(data, "-") ? strlen(data) / 2 : 0;
            for (size_t i = 0; i < length && length * 2 == strlen(data); ++i)
                text[i] = (unsigned char)(hex(data[2 * i]) << 4 | hex(data[2 * i + 1]));
            if (input < 0) failure = "unknown input";
            else if (pure_app_text(&app, (uint32_t)input, (PureSpan){text, length}) != PURE_OK) failure = "event failed";
        } else if (!strcmp(command, "frame")) {
            long count = *argument ? strtol(argument, NULL, 10) : 1;
            if (count < 1 || count > 600) failure = "frame count";
            for (long i = 0; !failure && i < count; ++i)
                if (pure_app_frame(&app) != PURE_OK) failure = "event failed";
        } else if (!strcmp(command, "replace")) {
            if (!load(argument, &modules[!active])) failure = "invalid module";
            else if (pure_app_replace(&app, &modules[!active]) != PURE_OK) failure = "replace failed";
            else active = !active;
        } else if (strcmp(command, "show")) failure = "unknown command";
        if (failure) status = 1;
        show(&app, failure);
    }
    pure_app_stop(&app);
    return status;
}
