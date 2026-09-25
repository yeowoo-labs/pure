#define _POSIX_C_SOURCE 200809L
#include "module-store.h"
#include "store.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The document CLI contract of edit.c, driven by a verified Pure module
 * instead of a document program. The module declares keyed messages named
 * show and edit; edit has u32 fields start and remove and a bytes tail. */
static bool number(const char *text, uint64_t limit, uint64_t *out) {
    if (!*text) return false;
    uint64_t value = 0;
    for (; *text; ++text) {
        if (*text < '0' || *text > '9') return false;
        unsigned digit = (unsigned)(*text - '0');
        if (digit > limit || value > (limit - digit) / 10) return false;
        value = value * 10 + digit;
    }
    *out = value;
    return true;
}

static const char *message(PureResult status) {
    switch (status) {
    case PURE_OK: return "ok";
    case PURE_NOT_FOUND: return "not_found";
    case PURE_CONFLICT: return "conflict";
    case PURE_BUSY: return "busy";
    case PURE_BUDGET: return "payload_budget";
    case PURE_LIMIT: return "limit";
    case PURE_UNCERTAIN: return "durability_unknown";
    case PURE_DENIED: return "denied";
    case PURE_ALLOCATION: return "allocation";
    case PURE_INVALID: return "invalid";
    default: return "failed";
    }
}

static bool valid_key(const char *key) {
    size_t length = strlen(key);
    if (!length || length > 64) return false;
    for (size_t i = 0; i < length; ++i) {
        char c = key[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '-' || c == '_')) return false;
    }
    return true;
}

static void put32(unsigned char *to, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) to[i] = (unsigned char)(value >> (8 * i));
}

static int field(const PureModule *m, int input, const char *name, uint8_t type) {
    const PureInputShape *shape = &m->input[input];
    for (uint32_t i = 0; i < shape->field_count; ++i)
        if (!strcmp(m->field[shape->field_start + i].name, name) && m->field[shape->field_start + i].type == type)
            return (int)(shape->field_start + i);
    return -1;
}

static bool write_span(void *context, PureSpan span) {
    return fwrite(span.data, 1, span.size, context) == span.size;
}

static bool count_span(void *context, PureSpan span) {
    *(size_t *)context += span.size;
    return true;
}

int main(int argc, char **argv) {
    const char *module_path = NULL, *store_path = NULL;
    size_t budget = 4u * 1024u * 1024u;
    int at = 1;
    for (; at + 1 < argc && !strncmp(argv[at], "--", 2); at += 2) {
        uint64_t value;
        if (!strcmp(argv[at], "--store")) store_path = argv[at + 1];
        else if (!strcmp(argv[at], "--module")) module_path = argv[at + 1];
        else if (!strcmp(argv[at], "--budget") && number(argv[at + 1], SIZE_MAX, &value)) budget = (size_t)value;
        else goto usage;
    }
    if (!module_path || !store_path || at >= argc) goto usage;
    bool show = !strcmp(argv[at], "show"), edit = !strcmp(argv[at], "edit");
    if ((!show && !edit && strcmp(argv[at], "apply")) ||
        argc - at != (show ? 2 : edit ? 6 : 4)) goto usage;
    uint64_t expected = 0, offset = 0, remove = 0;
    if (!show && !number(argv[at + 2], INT64_MAX, &expected)) goto usage;
    if (edit && (!number(argv[at + 3], UINT32_MAX, &offset) || !number(argv[at + 4], UINT32_MAX, &remove))) goto usage;

    static unsigned char image[1 << 17];
    static PureModule module;
    char error[160];
    FILE *source = fopen(module_path, "rb");
    size_t image_size = source ? fread(image, 1, sizeof(image), source) : 0;
    if (!source || ferror(source) || image_size == sizeof(image)) {
        if (source) fclose(source);
        fputs("Module: cannot read module\n", stderr);
        return 2;
    }
    fclose(source);
    if (pure_module_load(image, image_size, &module, error, sizeof(error)) != PURE_OK) {
        fprintf(stderr, "Module: %s\n", error);
        return 2;
    }
    int input = pure_module_input(&module, show ? "show" : "edit");
    if (input < 0 || module.input[input].kind != PURE_KIND_MESSAGE || !module.input[input].keyed) {
        fprintf(stderr, "Module: needs a keyed message named %s\n", show ? "show" : "edit");
        return 2;
    }
    const PureInputShape *shape = &module.input[input];
    int start_field = field(&module, input, "start", PURE_FIELD_U32);
    int remove_field = field(&module, input, "remove", PURE_FIELD_U32);
    size_t prefix = edit ? shape->prefix : 0;
    if (edit && (start_field < 0 || remove_field < 0 || !shape->field_count ||
                 module.field[shape->field_start + shape->field_count - 1].type != PURE_FIELD_BYTES)) {
        fputs("edit requires u32 fields start and remove and trailing insertion bytes; use apply for a custom input\n", stderr);
        return 2;
    }
    FILE *file = NULL;
    size_t length = 0;
    if (!show) {
        file = fopen(argv[at + (edit ? 5 : 3)], "rb");
        if (!file) { perror("input"); return 2; }
        long bytes;
        if (fseek(file, 0, SEEK_END) || (bytes = ftell(file)) < 0 ||
            (uintmax_t)bytes > PURE_OBJECT_LIMIT - prefix || fseek(file, 0, SEEK_SET)) {
            fputs("input must be a seekable file within the request size limit\n", stderr);
            fclose(file); return 2;
        }
        length = (size_t)bytes;
    }
    if (pure_store_init(store_path) != PURE_OK) {
        if (file) fclose(file);
        fputs("cannot initialize store\n", stderr); return 2;
    }
    const char *key = argv[at + 1];
    if (!valid_key(key) || prefix + length > budget) {
        if (file) fclose(file);
        fprintf(stderr, "%s\n", valid_key(key) ? "payload_budget" : "invalid"); return 2;
    }
    unsigned char *payload = NULL;
    if (!show) {
        payload = calloc(1, prefix + length ? prefix + length : 1);
        if (!payload) { fclose(file); fputs("allocation\n", stderr); return 2; }
        if (edit) {
            put32(payload + module.field[start_field].offset, (uint32_t)offset);
            put32(payload + module.field[remove_field].offset, (uint32_t)remove);
        }
        bool valid = !length || fread(payload + prefix, 1, length, file) == length;
        if (fgetc(file) != EOF || ferror(file)) valid = false;
        if (fclose(file)) valid = false;
        if (!valid) {
            free(payload);
            fputs("input changed or could not be read; nothing was applied\n", stderr); return 2;
        }
    }
    PureApp app;
    pure_app_start(&app, &module, pure_tree_data(store_path));
    PureResult status = pure_app_message(&app, (uint32_t)input, key, show ? -1 : (int64_t)expected,
                                         (PureSpan){payload, prefix + length});
    size_t reply = 0;
    if (status == PURE_OK) status = pure_app_reply(&app, count_span, &reply);
    if (status == PURE_OK && show && reply > budget) { status = PURE_BUDGET; app.revision = 0; }
    int exit_code = 0;
    if (status != PURE_OK) {
        fprintf(stderr, "{\"error\":\"%s\",\"revision\":\"%" PRId64 "\"}\n", message(status), app.revision);
        exit_code = status == PURE_CONFLICT ? 3 : 2;
    } else if (show) {
        if (pure_app_reply(&app, write_span, stdout) != PURE_OK) exit_code = 2;
        fprintf(stderr, "{\"revision\":\"%" PRId64 "\",\"bytes\":%zu}\n", app.revision, reply);
    } else if (printf("{\"revision\":\"%" PRId64 "\",\"reply_bytes\":%zu}\n", app.revision, reply) < 0) exit_code = 2;
    if (fflush(stdout)) exit_code = 2;
    pure_app_finish(&app);
    free(payload);
    return exit_code;
usage:
    fputs("pure-edit-module --module FILE --store DIRECTORY [--budget BYTES] show KEY\n"
          "pure-edit-module --module FILE --store DIRECTORY [--budget BYTES] edit KEY REVISION BYTE_OFFSET REMOVE_BYTES INSERT_FILE\n"
          "pure-edit-module --module FILE --store DIRECTORY [--budget BYTES] apply KEY REVISION INPUT_FILE\n"
          "edit uses the module's start and remove fields; apply accepts its raw message bytes.\n", stderr);
    return 2;
}
