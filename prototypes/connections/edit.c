#define _POSIX_C_SOURCE 200809L
#include "runtime.h"
#include "store-port.h"
#include "store.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#ifdef PURE_EMBEDDED_PROGRAM
#include "embedded-plan.h"
#endif

/* Native automation entry for the same prepared document program as the
 * network adapter. No socket or body serialization in the local execution. */
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

static void put32(unsigned char *to, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) to[i] = (unsigned char)(value >> (8 * i));
}

/* The convenience command fills the program's two numeric splice arguments
 * and its trailing insertion bytes. A more general input uses apply. */
static bool edit_layout(const PureProgram *p, size_t *prefix) {
    if (!p->splice || !p->write.to_end || p->write_step || p->input_steps ||
        !p->splice_offset.from_u32le || !p->splice_remove.from_u32le) return false;
    size_t start = p->splice_offset.value, remove = p->splice_remove.value;
    *prefix = p->write.offset;
    if (*prefix > PURE_OBJECT_LIMIT || p->input_required > *prefix || start > *prefix ||
        remove > *prefix || *prefix - start < 4 || *prefix - remove < 4) return false;
    return start < remove ? remove - start >= 4 : start - remove >= 4;
}

int main(int argc, char **argv) {
#ifndef PURE_EMBEDDED_PROGRAM
    const char *program_path = NULL, *store_path = NULL;
#else
    const char *store_path = NULL;
#endif
    size_t budget = 4u * 1024u * 1024u;
    int at = 1;
    for (; at + 1 < argc && !strncmp(argv[at], "--", 2); at += 2) {
        uint64_t value;
        if (!strcmp(argv[at], "--store")) store_path = argv[at + 1];
#ifndef PURE_EMBEDDED_PROGRAM
        else if (!strcmp(argv[at], "--program")) program_path = argv[at + 1];
#endif
        else if (!strcmp(argv[at], "--budget") && number(argv[at + 1], SIZE_MAX, &value)) budget = (size_t)value;
        else goto usage;
    }
#ifndef PURE_EMBEDDED_PROGRAM
    if (!program_path) goto usage;
#endif
    if (!store_path || at >= argc) goto usage;
    bool show = !strcmp(argv[at], "show"), edit = !strcmp(argv[at], "edit");
    if ((!show && !edit && strcmp(argv[at], "apply")) ||
        argc - at != (show ? 2 : edit ? 6 : 4)) goto usage;
    uint64_t expected = 0, offset = 0, remove = 0;
    if (!show && !number(argv[at + 2], INT64_MAX, &expected)) goto usage;
    if (edit && (!number(argv[at + 3], UINT32_MAX, &offset) || !number(argv[at + 4], UINT32_MAX, &remove))) goto usage;
#ifndef PURE_EMBEDDED_PROGRAM
    PureProgram loaded;
    char error[160];
    if (!pure_program_load(program_path, &loaded, error, sizeof(error))) {
        fprintf(stderr, "Program: %s\n", error); return 2;
    }
    const PureProgram *program = &loaded;
#else
    const PureProgram *program = &pure_embedded_program;
#endif
    size_t prefix = 0;
    if (edit && !edit_layout(program, &prefix)) {
        fputs("edit requires two distinct u32le splice fields and trailing insertion bytes; use apply for a custom input\n", stderr);
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
    PureRuntime runtime;
    PureWork work[1];
    if (!pure_runtime_init(&runtime, work, 1, program, pure_store_host(store_path), budget)) {
        if (file) fclose(file);
        return 2;
    }
    PureTicket ticket;
    PureResult admitted = pure_runtime_accept(&runtime, show ? PURE_INPUT_READ : PURE_INPUT_WRITE,
        argv[at + 1], (int64_t)expected, prefix + length, &ticket);
    if (admitted != PURE_OK) {
        if (file) fclose(file);
        fprintf(stderr, "%s\n", message(admitted)); return 2;
    }
    if (!show) {
        unsigned char *input = pure_runtime_ingress(ticket, NULL);
        if (prefix) memset(input, 0, prefix);
        if (edit) {
            put32(input + program->splice_offset.value, (uint32_t)offset);
            put32(input + program->splice_remove.value, (uint32_t)remove);
        }
        bool valid = !length || fread(input + prefix, 1, length, file) == length;
        if (fgetc(file) != EOF || ferror(file)) valid = false;
        if (fclose(file)) valid = false;
        if (!valid) {
            pure_runtime_release(ticket);
            fputs("input changed or could not be read; nothing was applied\n", stderr); return 2;
        }
        if (!pure_runtime_ready(ticket)) { pure_runtime_release(ticket); return 2; }
    }
    PureOutcome outcome;
    while (!pure_runtime_result(ticket, &outcome)) {
        if (!pure_runtime_step(&runtime)) { pure_runtime_release(ticket); return 2; }
    }
    int status = 0;
    if (outcome.status != PURE_OK) {
        fprintf(stderr, "{\"error\":\"%s\",\"revision\":\"%" PRId64 "\"}\n", message(outcome.status), outcome.revision);
        status = outcome.status == PURE_CONFLICT ? 3 : 2;
    } else if (show) {
        if (outcome.body.size && fwrite(outcome.body.data, 1, outcome.body.size, stdout) != outcome.body.size) status = 2;
        fprintf(stderr, "{\"revision\":\"%" PRId64 "\",\"bytes\":%zu}\n", outcome.revision, outcome.body.size);
    } else {
        if (printf("{\"revision\":\"%" PRId64 "\",\"reply_bytes\":%zu}\n", outcome.revision, outcome.body.size) < 0) status = 2;
    }
    if (fflush(stdout)) status = 2;
    pure_runtime_release(ticket);
    return status;
usage:
    fputs(
#ifdef PURE_EMBEDDED_PROGRAM
          "pure-edit-fixed --store DIRECTORY [--budget BYTES] show KEY\n"
          "pure-edit-fixed --store DIRECTORY [--budget BYTES] edit KEY REVISION BYTE_OFFSET REMOVE_BYTES INSERT_FILE\n"
          "pure-edit-fixed --store DIRECTORY [--budget BYTES] apply KEY REVISION INPUT_FILE\n"
#else
          "pure-edit --program FILE --store DIRECTORY [--budget BYTES] show KEY\n"
          "pure-edit --program FILE --store DIRECTORY [--budget BYTES] edit KEY REVISION BYTE_OFFSET REMOVE_BYTES INSERT_FILE\n"
          "pure-edit --program FILE --store DIRECTORY [--budget BYTES] apply KEY REVISION INPUT_FILE\n"
#endif
          "edit uses the prepared program's field locations; apply accepts its raw input bytes.\n", stderr);
    return 2;
}
