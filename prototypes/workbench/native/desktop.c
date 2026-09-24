/* Linux authoring shell. GTK, Cairo, the compiler and module loader stay outside
 * the application/execution core; the standalone CLI links none of them. */
#include "app.h"
#include <gtk/gtk.h>
#include <glib/gstdio.h>
#include <gdk/gdkkeysyms.h>
#include <pango/pangocairo.h>
#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#ifndef PURE_PREPARE_PATH
#error PURE_PREPARE_PATH must name the existing build-time compiler adapter
#endif
#ifndef PURE_NATIVE_HEADERS
#error PURE_NATIVE_HEADERS must name the native plan headers
#endif
#ifndef PURE_CC
#define PURE_CC "cc"
#endif

extern const PureScalarPlan pure_app_plan;

typedef struct {
    PureNativeApp app;
    void *scalar_memory, *module;
    const char *source_path;
    char *initial_source;
    gsize initial_source_bytes;
    char *scratch;
    unsigned generation;
    GtkWidget *window, *canvas, *status, *values, *source;
    bool pointer_active;
    uint32_t pointer_input, pointer_target;
    double pointer_x, pointer_y, pointer_px, pointer_py;
    uint32_t keys_down;
} Desktop;

typedef struct { double field[4]; bool valid; } SceneView;
typedef struct { Desktop *desktop; uint32_t input; } ValueControl;

static void free_value_control(gpointer data, GClosure *closure) {
    (void)closure;
    g_free(data);
}

static void report(Desktop *d, const char *name, PureOutcome result) {
    char message[200];
    snprintf(message, sizeof(message), "%s · status %u · scalar %u · local %" PRId64 " · stored %" PRId64,
             name, (unsigned)result.status, d->app.last_scalar_error, d->app.revision, d->app.stored_revision);
    gtk_label_set_text(GTK_LABEL(d->status), message);
    gtk_widget_queue_draw(d->canvas);
}

static bool scene(Desktop *d, SceneView result[24]) {
    if (d->app.plan->view_count > 24) return false;
    pure_scalar_begin_evaluation(d->app.scalar, 2048);
    for (uint32_t i = 0; i < d->app.plan->view_count; ++i) {
        const PureScalarView *view = &d->app.plan->views[i];
        result[i].valid = view->count <= 4;
        for (uint32_t j = 0; j < view->count && j < 4; ++j) {
            result[i].field[j] = pure_scalar_eval_bounded(d->app.scalar, view->fields[j].start, view->fields[j].length);
            if (pure_scalar_evaluation_info(d->app.scalar, 0) || !isfinite(result[i].field[j])) result[i].valid = false;
        }
        if (view->count >= 3 && strcmp(view->type, "text") && result[i].field[2] < 0) result[i].valid = false;
        if (view->count == 4 && result[i].field[3] < 0) result[i].valid = false;
    }
    return true;
}

static void viewport(int width, int height, double *scale, double *left, double *top) {
    *scale = fmin((double)width / 640.0, (double)height / 360.0);
    if (*scale <= 0) *scale = 1;
    *left = (width - 640 * *scale) / 2;
    *top = (height - 360 * *scale) / 2;
}

static void set_color(cairo_t *cr, const char *text) {
    GdkRGBA color;
    if (!text || !gdk_rgba_parse(&color, text)) gdk_rgba_parse(&color, "#79a7a1");
    cairo_set_source_rgba(cr, color.red, color.green, color.blue, color.alpha);
}

static void draw(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer data) {
    Desktop *d = data;
    cairo_set_source_rgb(cr, .055, .075, .09);
    cairo_paint(cr);
    double scale, left, top;
    viewport(width, height, &scale, &left, &top);
    cairo_save(cr);
    cairo_translate(cr, left, top);
    cairo_scale(cr, scale, scale);
    cairo_set_source_rgb(cr, .08, .115, .14);
    cairo_rectangle(cr, 0, 0, 640, 360);
    cairo_fill(cr);
    SceneView values[24] = {0};
    if (scene(d, values)) for (uint32_t i = 0; i < d->app.plan->view_count; ++i) {
        const PureScalarView *v = &d->app.plan->views[i];
        const double *f = values[i].field;
        if (!values[i].valid) continue;
        if (!strcmp(v->type, "box") && v->count == 4) {
            set_color(cr, v->decoration);
            cairo_rectangle(cr, f[0], f[1], f[2], f[3]);
            cairo_fill(cr);
        } else if (!strcmp(v->type, "circle") && v->count == 3) {
            set_color(cr, v->decoration);
            cairo_arc(cr, f[0], f[1], f[2], 0, 2 * G_PI);
            cairo_fill(cr);
        } else if (!strcmp(v->type, "text") && v->count == 3) {
            char number[64];
            snprintf(number, sizeof(number), "%.6g", f[2]);
            char *label = g_strconcat(v->decoration ? v->decoration : "", number, NULL);
            PangoLayout *layout = gtk_widget_create_pango_layout(GTK_WIDGET(area), label);
            PangoFontDescription *font = pango_font_description_from_string("Sans 15");
            pango_layout_set_font_description(layout, font);
            cairo_set_source_rgb(cr, .88, .92, .92);
            cairo_move_to(cr, f[0], f[1]);
            pango_cairo_show_layout(cr, layout);
            pango_font_description_free(font);
            g_object_unref(layout);
            g_free(label);
        }
    }
    cairo_restore(cr);
}

static bool hit(Desktop *d, double px, double py, uint32_t *input, uint32_t *target) {
    double scale, left, top;
    viewport(gtk_widget_get_width(d->canvas), gtk_widget_get_height(d->canvas), &scale, &left, &top);
    double x = (px - left) / scale, y = (py - top) / scale;
    if (x < 0 || y < 0 || x >= 640 || y >= 360) return false;
    SceneView views[24] = {0};
    if (!scene(d, views)) return false;
    for (uint32_t ri = d->app.plan->view_count; ri; --ri) {
        uint32_t i = ri - 1;
        const PureScalarView *v = &d->app.plan->views[i];
        const double *f = views[i].field;
        if (!views[i].valid) continue;
        bool inside = (!strcmp(v->type, "box") && v->count == 4 &&
                       x >= f[0] && x <= f[0] + f[2] && y >= f[1] && y <= f[1] + f[3]) ||
                      (!strcmp(v->type, "circle") && v->count == 3 &&
                       (x - f[0]) * (x - f[0]) + (y - f[1]) * (y - f[1]) <= f[2] * f[2]);
        if (!inside) continue;
        for (uint32_t j = 0; j < d->app.plan->target_count; ++j)
            if (!strcmp(v->id, d->app.plan->targets[j].view)) {
                *input = d->app.plan->targets[j].input; *target = j; return true;
            }
        for (uint32_t j = 0; j < d->app.plan->input_count; ++j)
            if (d->app.plan->inputs[j].kind == PURE_POINT &&
                !strcmp(v->id, d->app.plan->inputs[j].target)) {
                *input = j; *target = UINT32_MAX; return true;
            }
    }
    return false;
}

static void pointer(Desktop *d, PureActionPhase phase, double px, double py) {
    double scale, left, top;
    viewport(gtk_widget_get_width(d->canvas), gtk_widget_get_height(d->canvas), &scale, &left, &top);
    d->pointer_px = px;
    d->pointer_py = py;
    d->pointer_x = (px - left) / scale;
    d->pointer_y = (py - top) / scale;
    PureAction a = {.input = d->pointer_input, .phase = phase, .target = d->pointer_target,
                    .source = 0, .x = d->pointer_x, .y = d->pointer_y};
    report(d, "pointer", pure_native_app_action(&d->app, a));
}
static void drag_begin(GtkGestureDrag *gesture, double x, double y, gpointer data) {
    (void)gesture;
    Desktop *d = data;
    if (d->pointer_active || !hit(d, x, y, &d->pointer_input, &d->pointer_target)) return;
    gtk_widget_grab_focus(d->canvas);
    pointer(d, PURE_DOWN, x, y);
    uint32_t slot = d->pointer_target == UINT32_MAX ? d->pointer_input : 8 + d->pointer_target;
    d->pointer_active = (d->app.held & (1u << slot)) != 0;
}
static void drag_update(GtkGestureDrag *gesture, double dx, double dy, gpointer data) {
    Desktop *d = data;
    if (!d->pointer_active) return;
    double x, y;
    if (gtk_gesture_drag_get_start_point(gesture, &x, &y)) pointer(d, PURE_MOVE, x + dx, y + dy);
}
static void drag_end(GtkGestureDrag *gesture, double dx, double dy, gpointer data) {
    Desktop *d = data;
    if (!d->pointer_active) return;
    double x, y;
    if (gtk_gesture_drag_get_start_point(gesture, &x, &y)) pointer(d, PURE_UP, x + dx, y + dy);
    else pointer(d, PURE_CANCEL, d->pointer_px, d->pointer_py);
    d->pointer_active = false;
}
static void drag_cancel(GtkGesture *gesture, GdkEventSequence *sequence, gpointer data) {
    (void)gesture; (void)sequence;
    Desktop *d = data;
    if (d->pointer_active) {
        pointer(d, PURE_CANCEL, d->pointer_px, d->pointer_py);
        d->pointer_active = false;
    }
}

static bool key_matches(const char *key, guint value) {
    if (!strcmp(key, "ArrowLeft")) return value == GDK_KEY_Left;
    if (!strcmp(key, "ArrowRight")) return value == GDK_KEY_Right;
    if (!strcmp(key, "ArrowUp")) return value == GDK_KEY_Up;
    if (!strcmp(key, "ArrowDown")) return value == GDK_KEY_Down;
    if (!strcmp(key, "Space")) return value == GDK_KEY_space;
    if (strlen(key) == 4 && !memcmp(key, "Key", 3) && value < 128)
        return g_ascii_toupper((char)value) == key[3];
    return false;
}
static gboolean key_event(GtkEventControllerKey *controller, guint keyval, guint keycode,
                           GdkModifierType modifiers, gpointer data) {
    (void)keycode; (void)modifiers;
    Desktop *d = data;
    bool pressed = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(controller), "pressed"));
    bool handled = false;
    for (uint32_t i = 0; i < d->app.plan->input_count; ++i) {
        const PureScalarInput *input = &d->app.plan->inputs[i];
        if (input->kind != PURE_KEY || !key_matches(input->key, keyval)) continue;
        handled = true;
        if (pressed) d->keys_down |= 1u << i; else d->keys_down &= ~(1u << i);
        report(d, input->name, pure_native_app_action(&d->app, (PureAction){.input = i,
            .phase = pressed ? PURE_DOWN : PURE_UP, .source = 0}));
    }
    return handled;
}
static gboolean key_down(GtkEventControllerKey *c, guint key, guint code, GdkModifierType mods, gpointer d) {
    g_object_set_data(G_OBJECT(c), "pressed", GINT_TO_POINTER(1));
    return key_event(c, key, code, mods, d);
}
static void key_up(GtkEventControllerKey *c, guint key, guint code, GdkModifierType mods, gpointer d) {
    g_object_set_data(G_OBJECT(c), "pressed", NULL);
    (void)key_event(c, key, code, mods, d);
}
static void focus_left(GtkEventControllerFocus *controller, gpointer data) {
    (void)controller;
    Desktop *d = data;
    if (d->pointer_active) {
        pointer(d, PURE_CANCEL, d->pointer_px, d->pointer_py);
        d->pointer_active = false;
    }
    for (uint32_t i = 0; i < d->app.plan->input_count; ++i)
        if (d->keys_down & (1u << i))
            report(d, "key release", pure_native_app_action(&d->app, (PureAction){.input = i, .phase = PURE_UP}));
    d->keys_down = 0;
}

static void value_enter(GtkEntry *entry, gpointer data) {
    ValueControl *control = data;
    const char *s = gtk_editable_get_text(GTK_EDITABLE(entry));
    char *end;
    double n = g_ascii_strtod(s, &end);
    if (!*s || *end || !isfinite(n)) {
        gtk_label_set_text(GTK_LABEL(control->desktop->status), "Value must be a finite number");
        return;
    }
    report(control->desktop, "value", pure_native_app_action(&control->desktop->app,
        (PureAction){.input = control->input, .phase = PURE_CHANGE, .x = n}));
}
static void rebuild_values(Desktop *d) {
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(d->values))) gtk_box_remove(GTK_BOX(d->values), child);
    for (uint32_t i = 0; i < d->app.plan->input_count; ++i) {
        if (d->app.plan->inputs[i].kind != PURE_VALUE) continue;
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *label = gtk_label_new(d->app.plan->inputs[i].name);
        GtkWidget *entry = gtk_entry_new();
        gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "number ↵");
        gtk_widget_set_hexpand(entry, TRUE);
        gtk_box_append(GTK_BOX(row), label);
        gtk_box_append(GTK_BOX(row), entry);
        gtk_box_append(GTK_BOX(d->values), row);
        ValueControl *control = g_new(ValueControl, 1);
        *control = (ValueControl){d, i};
        g_signal_connect_data(entry, "activate", G_CALLBACK(value_enter), control, free_value_control, 0);
    }
}

static bool run(char **arguments, char **failure) {
    GError *error = NULL;
    char *output = NULL, *err = NULL;
    int status = 0;
    bool launched = g_spawn_sync(NULL, arguments, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL,
                                 &output, &err, &status, &error);
    bool ok = launched && g_spawn_check_wait_status(status, &error);
    if (!ok) {
        if (err && *err) {
            const char *begin = strstr(err, "Error: ");
            if (!begin) begin = err;
            const char *end = strchr(begin, '\n');
            *failure = g_strndup(begin, end ? (size_t)(end - begin) : 300);
        } else *failure = g_strdup(error ? error->message : "tool failed");
    }
    g_clear_error(&error);
    g_free(output); g_free(err);
    return ok;
}

static void apply_source(Desktop *d, bool save_source) {
    if (d->app.held) {
        gtk_label_set_text(GTK_LABEL(d->status), "Release held inputs before Apply");
        return;
    }
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(d->source));
    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(buffer, &start, &end);
    char *source = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
    size_t bytes = strlen(source);
    if (bytes > 32768) {
        gtk_label_set_text(GTK_LABEL(d->status), "Source exceeds the 32 KiB compiler limit");
        g_free(source); return;
    }
    char *input = g_build_filename(d->scratch, "source.pure", NULL);
    char *output = g_strdup_printf("%s/plan-%u.c", d->scratch, ++d->generation);
    char *module = g_strdup_printf("%s/plan-%u.so", d->scratch, d->generation);
    char *failure = NULL;
    GError *error = NULL;
    if (!g_file_set_contents(input, source, bytes, &error)) {
        failure = g_strdup(error->message); g_clear_error(&error); goto done;
    }
    char *prepare_argv[] = {"node", PURE_PREPARE_PATH, input, output, NULL};
    if (!run(prepare_argv, &failure)) goto done;
    char *headers = g_strconcat("-I", PURE_NATIVE_HEADERS, NULL);
    char *compile_argv[] = {PURE_CC, "-std=c17", "-O2", "-fPIC", "-shared", "-Wall", "-Wextra", "-Werror",
                            headers, output, "-o", module, NULL};
    bool built = run(compile_argv, &failure);
    g_free(headers);
    if (!built) goto done;
    void *handle = dlopen(module, RTLD_NOW | RTLD_LOCAL);
    if (!handle) { failure = g_strdup(dlerror()); goto done; }
    const PureScalarPlan *candidate = dlsym(handle, "pure_app_plan");
    bool same = candidate && !memcmp(candidate->identity, d->app.plan->identity, sizeof(candidate->identity));
    if (!same) {
        PureResult status = pure_native_app_replace_plan(&d->app, candidate);
        if (status != PURE_OK) {
            failure = g_strdup_printf("plan rejected: status %u, scalar %u", status, d->app.last_scalar_error);
            dlclose(handle); goto done;
        }
        if (d->module) dlclose(d->module);
        d->module = handle;
        rebuild_values(d);
    } else {
        dlclose(handle);
    }
    if (save_source && !g_file_set_contents(d->source_path, source, bytes, &error)) {
        failure = g_strdup_printf("Applied in memory; source save failed: %s", error->message);
        g_clear_error(&error);
    } else {
        gtk_text_buffer_set_modified(buffer, FALSE);
        report(d, "Applied source", (PureOutcome){.status = PURE_OK});
    }
done:
    if (failure) {
        char message[350];
        g_snprintf(message, sizeof(message), "Apply: %.335s", failure);
        gtk_label_set_text(GTK_LABEL(d->status), message);
        g_free(failure);
    }
    g_remove(input); g_remove(output); g_remove(module);
    g_free(input); g_free(output); g_free(module); g_free(source);
    gtk_widget_queue_draw(d->canvas);
}
static void apply(GtkButton *button, gpointer data) {
    (void)button;
    apply_source(data, true);
}

static void save_state(GtkButton *button, gpointer data) {
    (void)button;
    Desktop *d = data;
    report(d, "Save state", pure_native_app_save(&d->app));
}
static void load_state(GtkButton *button, gpointer data) {
    (void)button;
    Desktop *d = data;
    report(d, "Load state", pure_native_app_restore(&d->app));
}
static void reset_state(GtkButton *button, gpointer data) {
    (void)button;
    Desktop *d = data;
    report(d, "Reset", pure_native_app_action(&d->app, (PureAction){.phase = PURE_RESET}));
}

static GtkWidget *button(GtkWidget *parent, const char *title, GCallback callback, Desktop *d) {
    GtkWidget *item = gtk_button_new_with_label(title);
    gtk_box_append(GTK_BOX(parent), item);
    g_signal_connect(item, "clicked", callback, d);
    return item;
}
static void activate(GtkApplication *application, gpointer data) {
    Desktop *d = data;
    d->window = gtk_application_window_new(application);
    gtk_window_set_title(GTK_WINDOW(d->window), "Pure · native authoring");
    gtk_window_set_default_size(GTK_WINDOW(d->window), 1320, 720);
    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(root, 12); gtk_widget_set_margin_end(root, 12);
    gtk_widget_set_margin_top(root, 12); gtk_widget_set_margin_bottom(root, 12);
    gtk_window_set_child(GTK_WINDOW(d->window), root);
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(root), bar);
    button(bar, "Apply source", G_CALLBACK(apply), d);
    button(bar, "Save state", G_CALLBACK(save_state), d);
    button(bar, "Load state", G_CALLBACK(load_state), d);
    button(bar, "Reset", G_CALLBACK(reset_state), d);
    d->status = gtk_label_new("Edit source and Apply; click the scene for keyboard input");
    gtk_label_set_xalign(GTK_LABEL(d->status), 0);
    gtk_label_set_single_line_mode(GTK_LABEL(d->status), TRUE);
    gtk_label_set_ellipsize(GTK_LABEL(d->status), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(d->status, TRUE);
    gtk_box_append(GTK_BOX(bar), d->status);
    GtkWidget *panes = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_vexpand(panes, TRUE);
    gtk_box_append(GTK_BOX(root), panes);
    GtkWidget *left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_paned_set_start_child(GTK_PANED(panes), left);
    GtkWidget *source_title = gtk_label_new(d->source_path);
    gtk_label_set_xalign(GTK_LABEL(source_title), 0);
    gtk_box_append(GTK_BOX(left), source_title);
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_widget_set_hexpand(scroll, TRUE); gtk_widget_set_vexpand(scroll, TRUE);
    gtk_box_append(GTK_BOX(left), scroll);
    d->source = gtk_text_view_new();
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(d->source), TRUE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(d->source), GTK_WRAP_NONE);
    gtk_text_buffer_set_enable_undo(gtk_text_view_get_buffer(GTK_TEXT_VIEW(d->source)), TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), d->source);
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(d->source)),
                             d->initial_source, (int)d->initial_source_bytes);
    GtkWidget *right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_paned_set_end_child(GTK_PANED(panes), right);
    d->canvas = gtk_drawing_area_new();
    gtk_widget_set_hexpand(d->canvas, TRUE); gtk_widget_set_vexpand(d->canvas, TRUE);
    gtk_widget_set_focusable(d->canvas, TRUE);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(d->canvas), draw, d, NULL);
    gtk_box_append(GTK_BOX(right), d->canvas);
    d->values = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_box_append(GTK_BOX(right), d->values);
    rebuild_values(d);
    GtkGesture *drag = gtk_gesture_drag_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(drag), GDK_BUTTON_PRIMARY);
    g_signal_connect(drag, "drag-begin", G_CALLBACK(drag_begin), d);
    g_signal_connect(drag, "drag-update", G_CALLBACK(drag_update), d);
    g_signal_connect(drag, "drag-end", G_CALLBACK(drag_end), d);
    g_signal_connect(drag, "cancel", G_CALLBACK(drag_cancel), d);
    gtk_widget_add_controller(d->canvas, GTK_EVENT_CONTROLLER(drag));
    GtkEventController *keys = gtk_event_controller_key_new();
    g_signal_connect(keys, "key-pressed", G_CALLBACK(key_down), d);
    g_signal_connect(keys, "key-released", G_CALLBACK(key_up), d);
    gtk_widget_add_controller(d->canvas, keys);
    GtkEventController *focus = gtk_event_controller_focus_new();
    g_signal_connect(focus, "leave", G_CALLBACK(focus_left), d);
    gtk_widget_add_controller(d->canvas, focus);
    gtk_paned_set_position(GTK_PANED(panes), 560);
    apply_source(d, false);
    gtk_window_present(GTK_WINDOW(d->window));
}

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "pure-desktop STORE_DIRECTORY DOCUMENT_KEY SOURCE.pure\n");
        return 2;
    }
    Desktop d = {.source_path = argv[3]};
    GError *error = NULL;
    if (!g_file_get_contents(d.source_path, &d.initial_source, &d.initial_source_bytes, &error)) {
        fprintf(stderr, "source read failed: %s\n", error->message);
        g_clear_error(&error);
        return 2;
    }
    if (d.initial_source_bytes > 32768) {
        fprintf(stderr, "source exceeds the 32 KiB compiler limit\n");
        g_free(d.initial_source); return 2;
    }
    if (!g_utf8_validate(d.initial_source, (gssize)d.initial_source_bytes, NULL)) {
        fprintf(stderr, "source must be valid UTF-8 text\n");
        g_free(d.initial_source); return 2;
    }
    d.scalar_memory = malloc(pure_scalar_bytes());
    if (!d.scalar_memory) { g_free(d.initial_source); return 2; }
    PureResult status = pure_native_app_init(&d.app, &pure_app_plan, d.scalar_memory,
                                             pure_scalar_bytes(), argv[1], argv[2], 4096);
    if (status != PURE_OK) {
        fprintf(stderr, "initialization failed: %u\n", status);
        free(d.scalar_memory); g_free(d.initial_source); return 2;
    }
    d.scratch = g_dir_make_tmp("pure-author-XXXXXX", &error);
    if (!d.scratch) {
        fprintf(stderr, "authoring scratch: %s\n", error->message);
        g_clear_error(&error); free(d.scalar_memory); g_free(d.initial_source); return 2;
    }
    GtkApplication *application = gtk_application_new("dev.pure.authoring", G_APPLICATION_NON_UNIQUE);
    g_signal_connect(application, "activate", G_CALLBACK(activate), &d);
    int result = g_application_run(G_APPLICATION(application), 0, NULL);
    g_object_unref(application);
    pure_runtime_stop(&d.app.execution);
    if (d.module) dlclose(d.module);
    g_rmdir(d.scratch); g_free(d.scratch);
    free(d.scalar_memory);
    g_free(d.initial_source);
    return result;
}
