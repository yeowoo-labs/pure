#define _GNU_SOURCE
/* A surface for tests/check.py (serve/surface.h): as a person at the node's
 * display would, it types each line of TEXTS into the text input and then
 * presses the key input; after each it writes what the views show, and then,
 * until the server stops, what they show after each save the node made for
 * others (HTTP requests). One JSON line each into OUTPUT.
 *
 * pure-serve ... --surface surface-probe.so -- OUTPUT TEXT_INPUT KEY_INPUT TEXTS VIEW... */
#include "surface.h"
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int named(const char (*names)[PURE_MODULE_NAME], unsigned count, const char *name) {
    for (unsigned i = 0; i < count; ++i)
        if (!strcmp(names[i], name)) return (int)i;
    return -1;
}

static bool put_text(void *context, PureSpan span) {
    fwrite(span.data, 1, span.size, context);
    return true;
}

/* {"event":...,"changes":N,"views":{"name":value,...}} as the last save shows them. */
static void show(const PureSurface *s, PureApp *reader, FILE *out, const char *event, char **views, int count) {
    PureSnapshot *snapshot = s->read(s->host, reader);
    fprintf(out, "{\"event\":\"%s\",\"changes\":%llu,\"views\":{", event,
            snapshot ? (unsigned long long)reader->changes : 0ull);
    for (int i = 0; i < count; ++i) {
        int view = named(s->module->view_names, s->module->views, views[i]);
        fprintf(out, "%s\"%s\":", i ? "," : "", views[i]);
        double value;
        if (!snapshot || view < 0) fputs("null", out);
        else if (s->module->view_type[view] == PURE_VIEW_TEXT) {
            fputc('"', out);
            pure_app_view_text(reader, (uint32_t)view, put_text, out);
            fputc('"', out);
        } else if (!pure_app_view(reader, (uint32_t)view, &value)) fputs("null", out);
        else if (s->module->view_type[view] == PURE_VIEW_BOOL) fputs(value ? "true" : "false", out);
        else fprintf(out, "%g", value);
    }
    fputs("}}\n", out);
    fflush(out);
    if (snapshot) s->release(s->host, snapshot);
}

int pure_surface_run(const PureSurface *s, int argc, char **argv) {
    if (s->version != PURE_SURFACE_VERSION || s->app_size != sizeof(PureApp) || s->module_size != sizeof(PureModule))
        return 2;
    if (argc < 5) return 2;
    FILE *out = fopen(argv[1], "w");
    int text = named(s->module->input_names, s->module->inputs, argv[2]);
    int key = named(s->module->input_names, s->module->inputs, argv[3]);
    if (!out || text < 0 || key < 0) return 2;
    static PureApp reader; /* zeroed the first time, as read wants */
    char **views = argv + 5;
    int view_count = argc - 5;
    show(s, &reader, out, "start", views, view_count);
    char texts[256];
    snprintf(texts, sizeof(texts), "%s", argv[4]);
    for (char *line = strtok(texts, ","); line; line = strtok(NULL, ",")) {
        PureResult typed = s->type(s->host, (uint32_t)text, (PureSpan){(const unsigned char *)line, strlen(line)});
        char event[300];
        snprintf(event, sizeof(event), "typed %s: %d", line, (int)typed);
        show(s, &reader, out, event, views, view_count);
    }
    char event[64];
    snprintf(event, sizeof(event), "pressed: %d", (int)s->press(s->host, (uint32_t)key));
    show(s, &reader, out, event, views, view_count);
    /* Refusals: an input of another kind, and one past the module's. */
    snprintf(event, sizeof(event), "wrong kind: %d, no input: %d", (int)s->press(s->host, (uint32_t)text),
             (int)s->type(s->host, s->module->inputs, (PureSpan){0}));
    show(s, &reader, out, event, views, view_count);
    while (!s->stopping(s->host)) {
        struct pollfd wait = {s->changes, POLLIN, 0};
        if (poll(&wait, 1, 1000) <= 0) continue;
        uint64_t times;
        if (read(s->changes, &times, sizeof(times)) != sizeof(times) || s->stopping(s->host)) continue;
        show(s, &reader, out, "changed", views, view_count);
    }
    pure_app_stop(&reader);
    fclose(out);
    return 0;
}
