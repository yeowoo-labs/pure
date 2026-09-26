#include "module-save.h"
#include "module-text.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Stored forms. values: "PVS1", then per state {u8 type, u8 name length,
 * name, binary64 bits (little-endian)}. A list: "PLS1", u8 field count, per
 * field {u8 0 number | 1 text, u8 name length, name}, then its items
 * (module-list.h). A text state: its UTF-8 bytes. */
static const unsigned char values_magic[4] = {'P', 'V', 'S', '1'}, list_magic[4] = {'P', 'L', 'S', '1'};
static const PureVersion unsaved = {UINT64_MAX, UINT64_MAX};

bool pure_save_enabled(const PureApp *app) { return app->data.state && app->data.save; }

static PureResult open_state(PureApp *app, const char *prefix, const char *name, PureDraft **out) {
    char key[64];
    snprintf(key, sizeof(key), "%s%s", prefix, name);
    return app->data.state(app->data.context, key, out);
}

/* The whole value of a draft, contiguous; free it. */
typedef struct { unsigned char *bytes; size_t used; } Whole;

static bool gather(void *context, PureSpan span) {
    Whole *w = context;
    memcpy(w->bytes + w->used, span.data, span.size);
    w->used += span.size;
    return true;
}

static PureResult read_whole(const PureDraft *draft, unsigned char **out, size_t *size) {
    *size = pure_draft_size(draft);
    Whole w = {malloc(*size ? *size : 1), 0};
    if (!w.bytes) return PURE_ALLOCATION;
    PureResult result = pure_draft_read(draft, 0, *size, gather, &w);
    if (result != PURE_OK) { free(w.bytes); return result; }
    *out = w.bytes;
    return PURE_OK;
}

/* One named entry of a stored form: u8 kind, u8 name length, the name (an
 * identifier), then width more bytes. */
PURE_COLD static bool named(const unsigned char *bytes, size_t size, size_t *at, size_t width, uint8_t *kind,
                  char name[PURE_MODULE_NAME]) {
    if (size - *at < 2) return false;
    size_t length = bytes[*at + 1];
    if (size - *at - 2 < length + width || !length || length >= PURE_MODULE_NAME) return false;
    for (size_t i = 0; i < length; ++i) {
        unsigned char c = bytes[*at + 2 + i];
        bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
        if (!letter && !(i && c >= '0' && c <= '9')) return false;
    }
    *kind = bytes[*at];
    memcpy(name, bytes + *at + 2, length);
    name[length] = 0;
    *at += 2 + length;
    return true;
}

PURE_COLD PureResult pure_save_open_text(PureApp *app, const PureModule *m, uint32_t state, PureDraft **out,
                               PureVersion *saved) {
    PureResult result = open_state(app, "text-", m->state_names[state], out);
    if (result != PURE_OK) return result;
    *saved = pure_draft_version(*out);
    if (!pure_draft_revision(*out)) {
        uint32_t constant = m->state_ref[state];
        PureSpan initial = {m->constant_data + m->constant_offset[constant], m->constant_length[constant]};
        result = pure_draft_splice(*out, 0, pure_draft_size(*out), initial);
        *saved = unsaved;
    } else {
        unsigned char *bytes;
        size_t size;
        result = read_whole(*out, &bytes, &size);
        if (result == PURE_OK) {
            if (!pure_utf8_valid((PureSpan){bytes, size})) result = PURE_STORE_ERROR;
            free(bytes);
        }
    }
    if (result != PURE_OK) { pure_draft_close(*out); *out = NULL; }
    return result;
}

static size_t list_header(const PureModule *m, uint32_t record, unsigned char *out) {
    const PureRecord *r = &m->record[record];
    size_t at = 0;
    memcpy(out, list_magic, 4);
    at = 4;
    out[at++] = (unsigned char)r->count;
    for (unsigned f = 0; f < r->count; ++f) {
        const char *name = m->record_field_names[r->first + f];
        out[at++] = m->record_field_text[r->first + f];
        out[at++] = (unsigned char)strlen(name);
        memcpy(out + at, name, strlen(name));
        at += strlen(name);
    }
    return at;
}

PURE_COLD PureResult pure_save_rewrite_list(const PureModule *m, uint32_t state, PureList *list, PureDraft *draft) {
    unsigned char header[5 + PURE_RECORD_FIELDS * (2 + PURE_MODULE_NAME)];
    size_t length = list_header(m, m->state_ref[state], header);
    PureVersion before = pure_draft_version(draft);
    pure_draft_hold(draft, true);
    PureResult result = pure_draft_splice(draft, 0, pure_draft_size(draft), (PureSpan){header, length});
    pure_list_mirror(list, draft, length);
    if (result == PURE_OK) result = pure_list_write(list);
    if (result != PURE_OK) {
        pure_draft_restore(draft, before);
        pure_list_mirror(list, NULL, 0);
    }
    pure_draft_hold(draft, false);
    return result;
}

PURE_COLD PureResult pure_save_open_list(PureApp *app, const PureModule *m, uint32_t state, PureList **out, PureDraft **draft,
                               PureVersion *saved) {
    uint32_t record = m->state_ref[state];
    const PureRecord *r = &m->record[record];
    *out = NULL;
    PureResult result = open_state(app, "list-", m->state_names[state], draft);
    if (result != PURE_OK) return result;
    unsigned char *bytes = NULL;
    size_t size = 0;
    if (pure_draft_revision(*draft)) result = read_whole(*draft, &bytes, &size);
    /* The stored record: its fields' kinds and names. */
    char names[PURE_RECORD_FIELDS][PURE_MODULE_NAME] = {{0}};
    uint8_t text[PURE_RECORD_FIELDS] = {0};
    unsigned count = 0;
    size_t at = 0;
    bool stored = result == PURE_OK && bytes;
    if (stored) {
        bool ok = size >= 5 && !memcmp(bytes, list_magic, 4) && bytes[4] <= PURE_RECORD_FIELDS;
        count = ok ? bytes[4] : 0;
        at = 5;
        for (unsigned f = 0; ok && f < count; ++f) {
            ok = named(bytes, size, &at, 0, &text[f], names[f]) && text[f] <= 1;
            for (unsigned k = 0; k < f; ++k) ok = ok && strcmp(names[k], names[f]);
        }
        if (!ok) result = PURE_STORE_ERROR;
    }
    PureList *from = NULL;
    if (result == PURE_OK && stored) {
        from = pure_list_new(count, text);
        result = from ? pure_list_read(from, bytes + at, size - at) : PURE_ALLOCATION;
    }
    free(bytes);
    bool same = stored && count == r->count;
    for (unsigned f = 0; same && f < count; ++f)
        same = text[f] == m->record_field_text[r->first + f] && !strcmp(names[f], m->record_field_names[r->first + f]);
    if (result == PURE_OK && same) {
        *out = from;
        pure_list_mirror(from, *draft, at);
        *saved = pure_draft_version(*draft);
        return PURE_OK;
    }
    if (result == PURE_OK) {
        /* New, or stored with another record: its items move by field name. */
        const char *field_names[PURE_RECORD_FIELDS];
        for (unsigned f = 0; f < count; ++f) field_names[f] = names[f];
        if (from) result = pure_list_migrate(out, from, field_names, text, count, m, record);
        else if (!(*out = pure_list_new(r->count, m->record_field_text + r->first))) result = PURE_ALLOCATION;
    }
    pure_list_free(from);
    if (result == PURE_OK) result = pure_save_rewrite_list(m, state, *out, *draft);
    if (result != PURE_OK) {
        pure_list_free(*out);
        pure_draft_close(*draft);
        *out = NULL;
        *draft = NULL;
        return result;
    }
    *saved = unsaved;
    return PURE_OK;
}

static bool scalar(uint8_t type) {
    return type == PURE_STATE_NUM || type == PURE_STATE_CURSOR || type == PURE_STATE_INDEX;
}

/* A stored value still fits its state: a cursor on a character boundary of
 * its text, an index on an item of its list (or 0), a finite number. */
PURE_COLD static bool fits(const PureModule *m, PureDraft *const *texts, PureList *const *lists, uint32_t state, double value) {
    if (!isfinite(value)) return false;
    if (m->state_type[state] == PURE_STATE_NUM) return true;
    if (value < 0 || value != floor(value)) return false;
    if (m->state_type[state] == PURE_STATE_INDEX) {
        size_t count = pure_list_count(lists[m->state_ref[state]]);
        return value <= (count ? (double)(count - 1) : 0);
    }
    const PureDraft *text = texts[m->state_ref[state]];
    size_t size = pure_draft_size(text), at = (size_t)value;
    if (value > (double)size) return false;
    if (at == size) return true;
    unsigned char byte = 0;
    Whole w = {&byte, 0};
    return pure_draft_read(text, at, 1, gather, &w) == PURE_OK && (byte & 0xC0) != 0x80;
}

static size_t encode_values(const PureModule *m, const double *values, unsigned char *out) {
    size_t at = 4;
    memcpy(out, values_magic, 4);
    for (uint32_t i = 0; i < m->states; ++i) {
        if (!scalar(m->state_type[i])) continue;
        size_t length = strlen(m->state_names[i]);
        uint64_t bits;
        memcpy(&bits, &values[i], sizeof(bits));
        out[at++] = m->state_type[i];
        out[at++] = (unsigned char)length;
        memcpy(out + at, m->state_names[i], length);
        at += length;
        for (unsigned b = 0; b < 8; ++b) out[at++] = (unsigned char)(bits >> (8 * b));
    }
    return at;
}

enum { VALUES_BYTES = 4 + PURE_MODULE_STATES * (2 + PURE_MODULE_NAME + 8) };

PURE_COLD PureResult pure_save_open_values(PureApp *app, const PureModule *m, double *values, const int *kept,
                                 PureDraft *const *texts, PureList *const *lists) {
    bool scalars = false, fresh = false;
    for (uint32_t i = 0; i < m->states; ++i)
        if (scalar(m->state_type[i])) {
            scalars = true;
            fresh = fresh || kept[i] < 0;
        }
    if (!scalars) return PURE_OK;
    PureResult result = app->stored_values ? PURE_OK : open_state(app, "", "values", &app->stored_values);
    if (result != PURE_OK || !fresh || !pure_draft_revision(app->stored_values)) return result;
    unsigned char *bytes;
    size_t size;
    if ((result = read_whole(app->stored_values, &bytes, &size)) != PURE_OK) return result;
    bool ok = size >= 4 && !memcmp(bytes, values_magic, 4);
    for (size_t at = 4; ok && at < size;) {
        char name[PURE_MODULE_NAME];
        uint8_t type;
        if (!(ok = named(bytes, size, &at, 8, &type, name))) break;
        uint64_t bits = 0;
        for (unsigned b = 0; b < 8; ++b) bits |= (uint64_t)bytes[at + b] << (8 * b);
        at += 8;
        double value;
        memcpy(&value, &bits, sizeof(value));
        int state = pure_module_state(m, name);
        if (state >= 0 && kept[state] < 0 && m->state_type[state] == type && scalar(type) &&
            fits(m, texts, lists, (uint32_t)state, value))
            values[state] = value;
    }
    free(bytes);
    return ok ? PURE_OK : PURE_STORE_ERROR;
}

static bool same_version(PureVersion a, PureVersion b) { return a.ref == b.ref && a.bytes == b.bytes; }

PureResult pure_save_commit(PureApp *app, const double *values, void *const *documents, size_t document_count,
                            int64_t *revisions) {
    if (!pure_save_enabled(app)) {
        /* Without a host that saves together, a save has one document at most. */
        PureResult result = PURE_OK;
        for (size_t i = 0; i < document_count && result == PURE_OK; ++i)
            result = app->data.commit(app->data.context, documents[i], &revisions[i]);
        return result;
    }
    const PureModule *m = app->module;
    PureDraft *drafts[2 * PURE_MODULE_STATES + 1];
    uint32_t states[2 * PURE_MODULE_STATES + 1];
    size_t count = 0;
    for (uint32_t i = 0; i < m->states; ++i) {
        PureDraft *draft = m->state_type[i] == PURE_STATE_TEXT ? app->texts[i] :
                           m->state_type[i] == PURE_STATE_LIST ? app->list_drafts[i] : NULL;
        if (draft && !same_version(pure_draft_version(draft), app->saved[i])) {
            states[count] = i;
            drafts[count++] = draft;
        }
    }
    /* Numbers, cursors and indices: stored again only when their bytes change. */
    PureVersion values_before = {0, 0};
    bool values_changed = false;
    if (app->stored_values) {
        unsigned char encoded[VALUES_BYTES], *stored;
        size_t length = encode_values(m, values, encoded), size;
        PureResult result = read_whole(app->stored_values, &stored, &size);
        if (result != PURE_OK) return result;
        values_changed = size != length || memcmp(stored, encoded, length);
        free(stored);
        if (values_changed) {
            values_before = pure_draft_version(app->stored_values);
            pure_draft_hold(app->stored_values, true);
            result = pure_draft_splice(app->stored_values, 0, size, (PureSpan){encoded, length});
            if (result != PURE_OK) { pure_draft_hold(app->stored_values, false); return result; }
            states[count] = UINT32_MAX;
            drafts[count++] = app->stored_values;
        }
    }
    if (!count && !document_count) return PURE_OK;
    PureResult result = app->data.save(app->data.context, documents, document_count, drafts, count, revisions);
    for (size_t k = 0; result == PURE_OK && k < count; ++k)
        if (states[k] != UINT32_MAX) app->saved[states[k]] = pure_draft_version(drafts[k]);
    if (values_changed) {
        if (result != PURE_OK) pure_draft_restore(app->stored_values, values_before);
        pure_draft_hold(app->stored_values, false);
    }
    return result;
}
