#define _POSIX_C_SOURCE 200809L
#include "module-store.h"
#include "store.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Test host for a verified module: reads commands from stdin and prints one
 * JSON snapshot per command. Not a product interface. With --store the app's
 * states live in that tree store: it starts from them, and frame changes are
 * saved at the end of input or by save.
 *   down NAME | up NAME | frame [N] | type NAME HEX|- | replace PATH | save | show
 * Text states and text views print as JSON strings; cursors as byte positions;
 * lists as arrays of objects; indices as positions; a table view as an array
 * with an object of its columns per row. A failed view or cell is null. */
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

/* A text view is collected first: it may fail after some bytes. */
typedef struct { unsigned char bytes[1 << 16]; size_t used; bool fits; } Collected;

static bool collect(void *context, PureSpan span) {
    Collected *c = context;
    c->fits = c->fits && span.size <= sizeof(c->bytes) - c->used;
    if (c->fits && span.size) memcpy(c->bytes + c->used, span.data, span.size);
    if (c->fits) c->used += span.size;
    return true;
}

static void show_list(const PureModule *m, const PureList *list, const PureRecord *record) {
    putchar('[');
    for (size_t at = 0; at < pure_list_count(list); ++at) {
        printf("%s{", at ? "," : "");
        for (unsigned f = 0; f < record->count; ++f) {
            PureCell cell = pure_list_get(list, at, f);
            printf("%s\"%s\":", f ? "," : "", m->record_field_names[record->first + f]);
            if (!m->record_field_text[record->first + f]) printf("%.17g", cell.num);
            else {
                putchar('"');
                json_span(NULL, (PureSpan){cell.text, cell.size});
                putchar('"');
            }
        }
        putchar('}');
    }
    putchar(']');
}

/* A number or text view, or a column at a row. */
static void show_value(PureApp *app, uint32_t view, size_t row) {
    const PureModule *m = app->module;
    static Collected text;
    text.used = 0;
    text.fits = true;
    double value;
    bool column = m->view_table[view];
    if (m->view_type[view] == PURE_VIEW_TEXT) {
        PureResult result = column ? pure_app_cell_text(app, view, row, collect, &text)
                                   : pure_app_view_text(app, view, collect, &text);
        if (result != PURE_OK || !text.fits) printf("null");
        else {
            putchar('"');
            json_span(NULL, (PureSpan){text.bytes, text.used});
            putchar('"');
        }
    } else if (column ? pure_app_cell(app, view, row, &value) : pure_app_view(app, view, &value)) printf("%.17g", value);
    else printf("null");
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
        } else if (m->state_type[i] == PURE_STATE_LIST) show_list(m, app->lists[i], &m->record[m->state_ref[i]]);
        else printf("%.17g", app->values[i]);
    }
    printf("},\"views\":{");
    bool first = true;
    for (uint32_t i = 0; i < m->views; ++i) {
        if (m->view_table[i]) continue;
        printf("%s\"%s\":", first ? "" : ",", m->view_names[i]);
        first = false;
        if (m->view_type[i] != PURE_VIEW_TABLE) { show_value(app, i, 0); continue; }
        putchar('[');
        for (size_t row = 0; row < pure_app_rows(app, i); ++row) {
            printf("%s{", row ? "," : "");
            for (uint32_t c = 0, shown = 0; c < m->views; ++c) {
                if (m->view_table[c] != i + 1) continue;
                printf("%s\"%s\":", shown++ ? "," : "", m->view_names[c]);
                show_value(app, c, row);
            }
            putchar('}');
        }
        putchar(']');
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
    bool stored = argc == 4 && !strcmp(argv[2], "--store");
    if ((argc != 2 && !stored) || !load(argv[1], &modules[0])) {
        if (argc != 2 && !stored) fputs("pure-run MODULE [--store PATH] < commands\n", stderr);
        return 2;
    }
    if (stored && pure_store_init(argv[3]) != PURE_OK) { fprintf(stderr, "cannot open store %s\n", argv[3]); return 2; }
    unsigned active = 0;
    PureApp app;
    PureResult started = pure_app_start(&app, &modules[active], stored ? pure_tree_data(argv[3]) : (PureData){0});
    if (started != PURE_OK) { fprintf(stderr, "cannot start (%d)\n", (int)started); return 2; }
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
        } else if (!strcmp(command, "save")) {
            if (pure_app_save(&app) != PURE_OK) failure = "save failed";
        } else if (strcmp(command, "show")) failure = "unknown command";
        if (failure) status = 1;
        show(&app, failure);
    }
    if (pure_app_save(&app) != PURE_OK) { fputs("cannot save\n", stderr); status = 1; }
    pure_app_stop(&app);
    return status;
}
