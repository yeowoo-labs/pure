#define _POSIX_C_SOURCE 200809L
/* Optional GTK document adapter for the same prepared edit program used by
 * pure-edit and pure-connections. GTK text lives at the application boundary;
 * Pure's execution core remains independent of the UI and document encoding. */
#include "runtime.h"
#include "store-port.h"
#include "store.h"
#include <gtk/gtk.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    PureProgram program;
    PureRuntime runtime;
    PureWork work[1];
    const char *directory, *program_path;
    char key[65];
    size_t prefix, baseline_size;
    char *baseline;
    char *definition_baseline;
    size_t definition_baseline_size;
    uint64_t definition_generation;
    size_t last_output_bytes;
    bool has_edit_output;
    bool definition_needs_apply;
    int64_t revision;
    bool exists;
    bool closing, asking_to_close;
    GtkWidget *window, *text, *status, *version, *heading, *key_entry;
    GtkWidget *source, *source_revealer, *source_status;
} Editor;

static const char *result_name(PureResult result) {
    switch (result) {
    case PURE_OK: return "ok";
    case PURE_NOT_FOUND: return "not found";
    case PURE_CONFLICT: return "version conflict";
    case PURE_BUSY: return "busy";
    case PURE_BUDGET: return "request budget exceeded";
    case PURE_LIMIT: return "document limit or invalid range";
    case PURE_UNCERTAIN: return "durability unknown; inspect the stored revision before retrying";
    case PURE_DENIED: return "operation denied";
    case PURE_ALLOCATION: return "allocation failed";
    default: return "operation failed";
    }
}

static bool valid_key(const char *key) {
    size_t length = strlen(key);
    if (!length || length > 64) return false;
    for (size_t i = 0; i < length; ++i) {
        char c = key[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
    }
    return true;
}

/* The window supplies the convenience layout accepted by pure-edit. The
 * prepared program, not the window, determines the splice field locations. */
static bool edit_layout(const PureProgram *p, size_t *prefix) {
    if (!p->splice || !p->write.to_end || p->write_step || p->input_steps ||
        !p->splice_offset.from_u32le || !p->splice_remove.from_u32le ||
        p->read.offset || !p->read.to_end || p->read_required) return false;
    size_t start = p->splice_offset.value, remove = p->splice_remove.value;
    *prefix = p->write.offset;
    if (*prefix > PURE_OBJECT_LIMIT || p->input_required > *prefix || start > *prefix ||
        remove > *prefix || *prefix - start < 4 || *prefix - remove < 4) return false;
    return start < remove ? remove - start >= 4 : start - remove >= 4;
}

static void put32(unsigned char *to, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) to[i] = (unsigned char)(value >> (8 * i));
}

static void status(Editor *e, const char *text) {
    gtk_label_set_text(GTK_LABEL(e->status), text);
    char revision[64];
    snprintf(revision, sizeof(revision), "%s · revision %" PRId64,
             e->exists ? "Stored" : "New", e->revision);
    gtk_label_set_text(GTK_LABEL(e->version), revision);
}

static void definition_status(Editor *e, const char *text) {
    gtk_label_set_text(GTK_LABEL(e->source_status), text);
}

static void definition_title(Editor *e) {
    char title[128];
    if (e->has_edit_output)
        snprintf(title, sizeof(title), "%s · Pure document · definition %" PRIu64
                 " · output %zu B", e->key, e->definition_generation, e->last_output_bytes);
    else snprintf(title, sizeof(title), "%s · Pure document · definition %" PRIu64,
                  e->key, e->definition_generation);
    gtk_window_set_title(GTK_WINDOW(e->window), title);
}

static void apply_definition(Editor *e) {
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(e->source));
    GtkTextIter first, last;
    gtk_text_buffer_get_bounds(buffer, &first, &last);
    char *draft = gtk_text_buffer_get_text(buffer, &first, &last, FALSE);
    size_t length = strlen(draft);
    bool changed = length != e->definition_baseline_size ||
                   memcmp(draft, e->definition_baseline, length) != 0;
    if (!changed && !e->definition_needs_apply) {
        gtk_text_buffer_set_modified(buffer, FALSE);
        definition_status(e, "No definition changes to apply");
        g_free(draft);
        return;
    }
    PureProgram candidate;
    size_t prefix = 0;
    char error[160] = {0};
    if (length > PURE_PROGRAM_LIMIT ||
        !pure_program_parse(draft, length, &candidate, error, sizeof(error)) ||
        !edit_layout(&candidate, &prefix)) {
        char message[240];
        snprintf(message, sizeof(message), "Definition rejected: %s; draft preserved",
                 error[0] ? error : "document edit/read contract required");
        definition_status(e, message);
        g_free(draft);
        return;
    }
    char *on_disk = NULL;
    gsize on_disk_size = 0;
    GError *file_error = NULL;
    bool read = g_file_get_contents(e->program_path, &on_disk, &on_disk_size, &file_error);
    bool same = read && on_disk_size == e->definition_baseline_size &&
                !memcmp(on_disk, e->definition_baseline, on_disk_size);
    g_free(on_disk);
    if (file_error) g_error_free(file_error);
    if (!same) {
        definition_status(e, "Definition changed on disk; draft preserved. Reload or resolve it first");
        g_free(draft);
        return;
    }
    if (e->runtime.active || e->runtime.running) {
        definition_status(e, "A document operation is active; definition draft preserved");
        g_free(draft);
        return;
    }
    if (changed && !g_file_set_contents_full(e->program_path, draft, (gssize)length,
            G_FILE_SET_CONTENTS_CONSISTENT | G_FILE_SET_CONTENTS_DURABLE, 0600, &file_error)) {
        char message[240];
        snprintf(message, sizeof(message), "Could not save definition: %s; draft preserved",
                 file_error ? file_error->message : "unknown error");
        definition_status(e, message);
        if (file_error) g_error_free(file_error);
        g_free(draft);
        return;
    }
    PureProgram previous = e->program;
    size_t previous_prefix = e->prefix;
    e->program = candidate;
    e->prefix = prefix;
    if (!pure_runtime_restart(&e->runtime, &e->program,
                              pure_store_host(e->directory), 4u * 1024u * 1024u)) {
        e->program = previous;
        e->prefix = previous_prefix;
        bool restored = !changed || g_file_set_contents_full(e->program_path, e->definition_baseline,
            (gssize)e->definition_baseline_size,
            G_FILE_SET_CONTENTS_CONSISTENT | G_FILE_SET_CONTENTS_DURABLE, 0600, NULL);
        definition_status(e, restored ? "Could not install definition; previous source restored" :
                             "Definition installation failed; inspect source file before retrying");
        g_free(draft);
        return;
    }
    g_free(e->definition_baseline);
    e->definition_baseline = draft;
    e->definition_baseline_size = length;
    gtk_text_buffer_set_modified(buffer, FALSE);
    e->definition_needs_apply = false;
    ++e->definition_generation;
    e->has_edit_output = false;
    definition_title(e);
    definition_status(e, "Applied locally & saved; other hosts load it after restart");
}

static void apply_definition_clicked(GtkButton *button, gpointer data) {
    (void)button;
    apply_definition(data);
}

static void reload_definition(Editor *e) {
    char *source = NULL;
    gsize length = 0;
    GError *error = NULL;
    if (!g_file_get_contents(e->program_path, &source, &length, &error) ||
        length > PURE_PROGRAM_LIMIT || memchr(source, 0, length) ||
        !g_utf8_validate(source, (gssize)length, NULL)) {
        definition_status(e, "Could not load a UTF-8 definition within 16 KiB; draft preserved");
        if (error) g_error_free(error);
        g_free(source);
        return;
    }
    g_free(e->definition_baseline);
    e->definition_baseline = source;
    e->definition_baseline_size = length;
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(e->source));
    gtk_text_buffer_set_text(buffer, source, (int)length);
    gtk_text_buffer_set_modified(buffer, FALSE);
    e->definition_needs_apply = true;
    definition_status(e, "Definition reloaded from disk; Apply to run it");
}

static void reload_definition_clicked(GtkButton *button, gpointer data) {
    (void)button;
    Editor *e = data;
    if (gtk_text_buffer_get_modified(gtk_text_view_get_buffer(GTK_TEXT_VIEW(e->source)))) {
        definition_status(e, "Definition draft has changes; use Discard & reload");
        return;
    }
    reload_definition(e);
}

static void discard_definition_clicked(GtkButton *button, gpointer data) {
    (void)button;
    reload_definition(data);
}

static void toggle_definition(Editor *e) {
    bool shown = gtk_revealer_get_reveal_child(GTK_REVEALER(e->source_revealer));
    gtk_revealer_set_reveal_child(GTK_REVEALER(e->source_revealer), !shown);
    gtk_widget_grab_focus(shown ? e->text : e->source);
}

static void toggle_definition_clicked(GtkButton *button, gpointer data) {
    (void)button;
    toggle_definition(data);
}

static PureOutcome finish(Editor *e, PureTicket ticket) {
    PureOutcome result;
    while (!pure_runtime_result(ticket, &result))
        if (!pure_runtime_step(&e->runtime)) return (PureOutcome){.status = PURE_INVALID};
    return result;
}

static bool open_document_key(Editor *e, const char *key) {
    PureTicket ticket;
    PureResult accepted = pure_runtime_accept(&e->runtime, PURE_INPUT_READ,
                                               key, 0, 0, &ticket);
    if (accepted != PURE_OK) { status(e, result_name(accepted)); return false; }
    PureOutcome result = finish(e, ticket);
    char *copy = NULL;
    if (result.status == PURE_OK) {
        if (result.body.size > G_MAXINT ||
            (result.body.size && (!result.body.data ||
             memchr(result.body.data, 0, result.body.size) ||
             !g_utf8_validate((const char *)result.body.data, result.body.size, NULL)))) {
            pure_runtime_release(ticket);
            status(e, "Stored bytes are not plain UTF-8 text; use pure-edit for binary data");
            return false;
        }
        copy = g_malloc(result.body.size + 1);
        if (result.body.size) memcpy(copy, result.body.data, result.body.size);
        copy[result.body.size] = 0;
    }
    pure_runtime_release(ticket);
    if (result.status != PURE_OK && result.status != PURE_NOT_FOUND) {
        status(e, result_name(result.status));
        return false;
    }
    if (strcmp(key, e->key) != 0) memcpy(e->key, key, strlen(key) + 1);
    g_free(e->baseline);
    e->baseline = copy ? copy : g_strdup("");
    e->baseline_size = result.status == PURE_OK ? result.body.size : 0;
    e->revision = result.status == PURE_OK ? result.revision : 0;
    e->exists = result.status == PURE_OK;
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(e->text));
    gtk_text_buffer_set_text(buffer, e->baseline, (int)e->baseline_size);
    gtk_text_buffer_set_modified(buffer, FALSE);
    e->has_edit_output = false;
    gtk_label_set_text(GTK_LABEL(e->heading), e->key);
    definition_title(e);
    char message[128];
    snprintf(message, sizeof(message), "%s · %zu bytes", e->exists ? "Loaded" : "New document",
             e->baseline_size);
    status(e, message);
    return true;
}

static void open_document(Editor *e) {
    (void)open_document_key(e, e->key);
}

static void open_key(Editor *e) {
    GtkTextBuffer *document = gtk_text_view_get_buffer(GTK_TEXT_VIEW(e->text));
    GtkTextBuffer *definition = gtk_text_view_get_buffer(GTK_TEXT_VIEW(e->source));
    if (gtk_text_buffer_get_modified(document) || gtk_text_buffer_get_modified(definition)) {
        status(e, "Save or discard the document and definition drafts before opening another key");
        return;
    }
    const char *key = gtk_editable_get_text(GTK_EDITABLE(e->key_entry));
    if (!valid_key(key)) { status(e, "Key must be 1–64 ASCII letters, digits, _ or -"); return; }
    if (open_document_key(e, key)) gtk_widget_grab_focus(e->text);
}

static void open_key_clicked(GtkButton *button, gpointer data) {
    (void)button;
    open_key(data);
}

static void open_key_activated(GtkEntry *entry, gpointer data) {
    (void)entry;
    open_key(data);
}

static void reload_clicked(GtkButton *button, gpointer data) {
    (void)button;
    Editor *e = data;
    if (gtk_text_buffer_get_modified(gtk_text_view_get_buffer(GTK_TEXT_VIEW(e->text)))) {
        status(e, "Draft has changes; use Discard & reload to replace it");
        return;
    }
    open_document(e);
}

static void discard_clicked(GtkButton *button, gpointer data) {
    (void)button;
    open_document(data);
}

static bool continuation(unsigned char c) { return (c & 0xc0u) == 0x80u; }

/* Find the smallest changed byte interval with valid UTF-8 boundaries. The
 * storage effect still sees bytes; this only keeps the UI's edit interval
 * meaningful when two code points happen to share leading/trailing bytes. */
static void change_span(const char *old, size_t old_size, const char *draft, size_t draft_size,
                        size_t *start, size_t *remove, size_t *insert) {
    size_t prefix = 0;
    while (prefix < old_size && prefix < draft_size && old[prefix] == draft[prefix]) ++prefix;
    while (prefix && prefix < old_size && continuation((unsigned char)old[prefix])) --prefix;
    size_t suffix = 0;
    while (suffix < old_size - prefix && suffix < draft_size - prefix &&
           old[old_size - suffix - 1] == draft[draft_size - suffix - 1]) ++suffix;
    while (suffix && (continuation((unsigned char)old[old_size - suffix]) ||
                      continuation((unsigned char)draft[draft_size - suffix]))) --suffix;
    *start = prefix;
    *remove = old_size - prefix - suffix;
    *insert = draft_size - prefix - suffix;
}

static void save_document(Editor *e) {
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(e->text));
    GtkTextIter first, last;
    gtk_text_buffer_get_bounds(buffer, &first, &last);
    char *draft = gtk_text_buffer_get_text(buffer, &first, &last, FALSE);
    size_t draft_size = strlen(draft);
    size_t offset, remove, insert;
    change_span(e->baseline, e->baseline_size, draft, draft_size, &offset, &remove, &insert);
    if (e->exists && !remove && !insert) {
        gtk_text_buffer_set_modified(buffer, FALSE);
        status(e, "No changes to save");
        g_free(draft);
        return;
    }
    if (draft_size > PURE_OBJECT_LIMIT || offset > UINT32_MAX || remove > UINT32_MAX ||
        insert > PURE_OBJECT_LIMIT - e->prefix) {
        status(e, "Document or edit exceeds the selected byte limit");
        g_free(draft);
        return;
    }
    PureTicket ticket;
    PureResult accepted = pure_runtime_accept(&e->runtime, PURE_INPUT_WRITE, e->key,
                                               e->revision, e->prefix + insert, &ticket);
    if (accepted != PURE_OK) {
        status(e, result_name(accepted));
        g_free(draft);
        return;
    }
    unsigned char *input = pure_runtime_ingress(ticket, NULL);
    memset(input, 0, e->prefix);
    put32(input + e->program.splice_offset.value, (uint32_t)offset);
    put32(input + e->program.splice_remove.value, (uint32_t)remove);
    if (insert) memcpy(input + e->prefix, draft + offset, insert);
    if (!pure_runtime_ready(ticket)) {
        pure_runtime_release(ticket);
        status(e, "Could not publish edit");
        g_free(draft);
        return;
    }
    PureOutcome result = finish(e, ticket);
    size_t reply_size = result.body.size;
    pure_runtime_release(ticket);
    if (result.status != PURE_OK) {
        char message[160];
        if (result.status == PURE_CONFLICT)
            snprintf(message, sizeof(message), "Version conflict: stored revision %" PRId64
                     "; draft preserved", result.revision);
        else snprintf(message, sizeof(message), "%s; draft preserved", result_name(result.status));
        status(e, message);
        g_free(draft);
        return;
    }
    g_free(e->baseline);
    e->baseline = draft;
    e->baseline_size = draft_size;
    e->revision = result.revision;
    e->exists = true;
    e->last_output_bytes = reply_size;
    e->has_edit_output = true;
    definition_title(e);
    gtk_text_buffer_set_modified(buffer, FALSE);
    char message[128];
    snprintf(message, sizeof(message), "Saved %zu-byte edit at byte %zu · output %zu bytes",
             insert, offset, reply_size);
    status(e, message);
}

static void save_clicked(GtkButton *button, gpointer data) {
    (void)button;
    save_document(data);
}

static gboolean key_pressed(GtkEventControllerKey *controller, guint keyval, guint keycode,
                            GdkModifierType modifiers, gpointer data) {
    (void)controller; (void)keycode;
    Editor *e = data;
    if ((modifiers & GDK_CONTROL_MASK) && (modifiers & GDK_SHIFT_MASK) &&
        (keyval == GDK_KEY_d || keyval == GDK_KEY_D)) {
        toggle_definition(e);
        return TRUE;
    }
    if ((modifiers & GDK_CONTROL_MASK) &&
        (keyval == GDK_KEY_o || keyval == GDK_KEY_O)) {
        gtk_widget_grab_focus(e->key_entry);
        return TRUE;
    }
    if ((modifiers & GDK_CONTROL_MASK) &&
        (keyval == GDK_KEY_s || keyval == GDK_KEY_S)) {
        if ((modifiers & GDK_SHIFT_MASK) ||
            gtk_window_get_focus(GTK_WINDOW(e->window)) == e->source)
            apply_definition(e);
        else save_document(e);
        return TRUE;
    }
    if ((modifiers & GDK_CONTROL_MASK) &&
        (keyval == GDK_KEY_w || keyval == GDK_KEY_W)) {
        gtk_window_close(GTK_WINDOW(e->window));
        return TRUE;
    }
    return FALSE;
}

static void close_decision(GObject *source, GAsyncResult *response, gpointer data) {
    Editor *e = data;
    GError *error = NULL;
    int selected = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(source), response, &error);
    if (error) g_error_free(error);
    e->asking_to_close = false;
    if (selected == 1) {
        e->closing = true;
        gtk_window_destroy(GTK_WINDOW(e->window));
    }
}

static gboolean close_requested(GtkWindow *window, gpointer data) {
    Editor *e = data;
    bool document_changed = gtk_text_buffer_get_modified(gtk_text_view_get_buffer(GTK_TEXT_VIEW(e->text)));
    bool definition_changed = gtk_text_buffer_get_modified(gtk_text_view_get_buffer(GTK_TEXT_VIEW(e->source)));
    if (e->closing || (!document_changed && !definition_changed))
        return FALSE;
    if (e->asking_to_close) return TRUE;
    e->asking_to_close = true;
    GtkAlertDialog *dialog = gtk_alert_dialog_new("Discard unsaved document or definition changes?");
    gtk_alert_dialog_set_detail(dialog, "The current draft has not been saved or applied.");
    const char *buttons[] = {"Keep editing", "Discard changes", NULL};
    gtk_alert_dialog_set_buttons(dialog, buttons);
    gtk_alert_dialog_set_cancel_button(dialog, 0);
    gtk_alert_dialog_set_default_button(dialog, 0);
    gtk_alert_dialog_choose(dialog, window, NULL, close_decision, e);
    g_object_unref(dialog);
    return TRUE;
}

static void activate(GtkApplication *application, gpointer data) {
    Editor *e = data;
    if (e->window) { gtk_window_present(GTK_WINDOW(e->window)); return; }
    e->window = gtk_application_window_new(application);
    g_signal_connect(e->window, "close-request", G_CALLBACK(close_requested), e);
    gtk_window_set_default_size(GTK_WINDOW(e->window), 960, 680);
    definition_title(e);
    GtkWidget *column = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_window_set_child(GTK_WINDOW(e->window), column);
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_margin_start(bar, 14);
    gtk_widget_set_margin_end(bar, 14);
    gtk_widget_set_margin_top(bar, 12);
    gtk_widget_set_margin_bottom(bar, 12);
    gtk_box_append(GTK_BOX(column), bar);
    e->heading = gtk_label_new(e->key);
    gtk_widget_add_css_class(e->heading, "title-3");
    gtk_box_append(GTK_BOX(bar), e->heading);
    e->version = gtk_label_new("");
    gtk_widget_add_css_class(e->version, "dim-label");
    gtk_box_append(GTK_BOX(bar), e->version);
    GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(spacer, TRUE);
    gtk_box_append(GTK_BOX(bar), spacer);
    GtkWidget *definition = gtk_button_new_with_label("Definition · Ctrl+Shift+D");
    g_signal_connect(definition, "clicked", G_CALLBACK(toggle_definition_clicked), e);
    gtk_box_append(GTK_BOX(bar), definition);
    GtkWidget *reload = gtk_button_new_with_label("Reload");
    g_signal_connect(reload, "clicked", G_CALLBACK(reload_clicked), e);
    gtk_box_append(GTK_BOX(bar), reload);
    GtkWidget *discard = gtk_button_new_with_label("Discard & reload");
    g_signal_connect(discard, "clicked", G_CALLBACK(discard_clicked), e);
    gtk_box_append(GTK_BOX(bar), discard);
    GtkWidget *save = gtk_button_new_with_label("Save · Ctrl+S");
    gtk_widget_add_css_class(save, "suggested-action");
    g_signal_connect(save, "clicked", G_CALLBACK(save_clicked), e);
    gtk_box_append(GTK_BOX(bar), save);
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_box_append(GTK_BOX(column), scroll);
    e->text = gtk_text_view_new();
    gtk_accessible_update_property(GTK_ACCESSIBLE(e->text),
                                   GTK_ACCESSIBLE_PROPERTY_LABEL, "Document text", -1);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(e->text), TRUE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(e->text), GTK_WRAP_WORD_CHAR);
    gtk_widget_set_margin_start(e->text, 18);
    gtk_widget_set_margin_end(e->text, 18);
    gtk_widget_set_margin_top(e->text, 12);
    gtk_widget_set_margin_bottom(e->text, 12);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), e->text);
    e->source_revealer = gtk_revealer_new();
    gtk_box_append(GTK_BOX(column), e->source_revealer);
    GtkWidget *source_panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(source_panel, 14);
    gtk_widget_set_margin_end(source_panel, 14);
    gtk_widget_set_margin_bottom(source_panel, 8);
    gtk_revealer_set_child(GTK_REVEALER(e->source_revealer), source_panel);
    GtkWidget *source_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(source_panel), source_bar);
    GtkWidget *source_heading = gtk_label_new("Application definition · editor.pure");
    gtk_widget_add_css_class(source_heading, "title-4");
    gtk_box_append(GTK_BOX(source_bar), source_heading);
    GtkWidget *source_spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(source_spacer, TRUE);
    gtk_box_append(GTK_BOX(source_bar), source_spacer);
    GtkWidget *source_reload = gtk_button_new_with_label("Reload definition");
    g_signal_connect(source_reload, "clicked", G_CALLBACK(reload_definition_clicked), e);
    gtk_box_append(GTK_BOX(source_bar), source_reload);
    GtkWidget *source_discard = gtk_button_new_with_label("Discard & reload");
    g_signal_connect(source_discard, "clicked", G_CALLBACK(discard_definition_clicked), e);
    gtk_box_append(GTK_BOX(source_bar), source_discard);
    GtkWidget *source_apply = gtk_button_new_with_label("Apply & save · Ctrl+S");
    gtk_widget_add_css_class(source_apply, "suggested-action");
    g_signal_connect(source_apply, "clicked", G_CALLBACK(apply_definition_clicked), e);
    gtk_box_append(GTK_BOX(source_bar), source_apply);
    GtkWidget *source_scroll = gtk_scrolled_window_new();
    gtk_widget_set_size_request(source_scroll, -1, 260);
    gtk_box_append(GTK_BOX(source_panel), source_scroll);
    e->source = gtk_text_view_new();
    gtk_accessible_update_property(GTK_ACCESSIBLE(e->source),
                                   GTK_ACCESSIBLE_PROPERTY_LABEL, "Application definition source", -1);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(e->source), TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(source_scroll), e->source);
    GtkTextBuffer *source_buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(e->source));
    gtk_text_buffer_set_text(source_buffer, e->definition_baseline,
                             (int)e->definition_baseline_size);
    gtk_text_buffer_set_modified(source_buffer, FALSE);
    e->source_status = gtk_label_new("Definition is active; edit and apply to change it");
    gtk_label_set_xalign(GTK_LABEL(e->source_status), 0);
    gtk_box_append(GTK_BOX(source_panel), e->source_status);
    GtkWidget *footer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_start(footer, 14);
    gtk_widget_set_margin_end(footer, 14);
    gtk_widget_set_margin_top(footer, 9);
    gtk_widget_set_margin_bottom(footer, 11);
    gtk_box_append(GTK_BOX(column), footer);
    e->status = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(e->status), 0);
    gtk_widget_set_hexpand(e->status, TRUE);
    gtk_box_append(GTK_BOX(footer), e->status);
    e->key_entry = gtk_entry_new();
    gtk_entry_set_max_length(GTK_ENTRY(e->key_entry), 64);
    gtk_editable_set_text(GTK_EDITABLE(e->key_entry), e->key);
    gtk_entry_set_placeholder_text(GTK_ENTRY(e->key_entry), "Document key");
    gtk_editable_set_width_chars(GTK_EDITABLE(e->key_entry), 15);
    gtk_accessible_update_property(GTK_ACCESSIBLE(e->key_entry),
                                   GTK_ACCESSIBLE_PROPERTY_LABEL, "Document key", -1);
    g_signal_connect(e->key_entry, "activate", G_CALLBACK(open_key_activated), e);
    gtk_box_append(GTK_BOX(footer), e->key_entry);
    GtkWidget *open = gtk_button_new_with_label("Open · Ctrl+O");
    g_signal_connect(open, "clicked", G_CALLBACK(open_key_clicked), e);
    gtk_box_append(GTK_BOX(footer), open);
    GtkEventController *keys = gtk_event_controller_key_new();
    g_signal_connect(keys, "key-pressed", G_CALLBACK(key_pressed), e);
    gtk_widget_add_controller(e->window, keys);
    open_document(e);
    gtk_window_present(GTK_WINDOW(e->window));
}

int main(int argc, char **argv) {
    if (argc != 6 || strcmp(argv[1], "--program") || strcmp(argv[3], "--store") ||
        !valid_key(argv[5])) {
        fputs("pure-edit-window --program FILE --store DIRECTORY KEY\n", stderr);
        return 2;
    }
    Editor e = {.directory = argv[4], .program_path = argv[2], .definition_generation = 1};
    memcpy(e.key, argv[5], strlen(argv[5]) + 1);
    char error[160] = {0};
    gsize source_size = 0;
    GError *source_error = NULL;
    if (!g_file_get_contents(e.program_path, &e.definition_baseline, &source_size, &source_error) ||
        source_size > PURE_PROGRAM_LIMIT ||
        memchr(e.definition_baseline, 0, source_size) ||
        !g_utf8_validate(e.definition_baseline, (gssize)source_size, NULL) ||
        !pure_program_parse(e.definition_baseline, source_size, &e.program, error, sizeof(error)) ||
        !edit_layout(&e.program, &e.prefix)) {
        fprintf(stderr, "Program must define the simple splice edit layout: %s\n",
                error[0] ? error : source_error ? source_error->message : "unsupported layout");
        if (source_error) g_error_free(source_error);
        g_free(e.definition_baseline);
        return 2;
    }
    e.definition_baseline_size = source_size;
    if (pure_store_init(e.directory) != PURE_OK ||
        !pure_runtime_init(&e.runtime, e.work, 1, &e.program,
                           pure_store_host(e.directory), 4u * 1024u * 1024u)) {
        fputs("Cannot initialize document store or execution domain\n", stderr);
        return 2;
    }
    e.baseline = g_strdup("");
    GtkApplication *application = gtk_application_new("org.pure.DocumentEditor", G_APPLICATION_NON_UNIQUE);
    g_signal_connect(application, "activate", G_CALLBACK(activate), &e);
    char *app_argv[] = {argv[0], NULL};
    int result = g_application_run(G_APPLICATION(application), 1, app_argv);
    g_object_unref(application);
    g_free(e.baseline);
    g_free(e.definition_baseline);
    return result;
}
