#include "runtime.h"
#include "store-port.h"
#include "store.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "storage-plan.h"

enum { MAX_TARGETS = 8 };
typedef struct {
    PureRuntime runtime;
    PureWork work[1];
    PureTicket ticket;
    PureOutcome outcome;
} Target;

static const char *result_name(PureResult result) {
    static const char *const names[] = {
        "ok", "not_found", "conflict", "busy", "budget", "limit",
        "store_error", "durability_unknown", "invalid", "denied",
        "closed", "cancelled", "allocation"
    };
    return (unsigned)result < sizeof(names) / sizeof(names[0]) ? names[result] : "unknown";
}

static bool number(const char *text, uint64_t limit, uint64_t *out) {
    uint64_t value = 0;
    if (!*text) return false;
    for (; *text; ++text) {
        if (*text < '0' || *text > '9') return false;
        unsigned digit = (unsigned)(*text - '0');
        if (digit > limit || value > (limit - digit) / 10) return false;
        value = value * 10 + digit;
    }
    *out = value;
    return true;
}

int main(int argc, char **argv) {
    int at = 1;
    size_t target_budget = PURE_OBJECT_LIMIT;
    uint64_t parsed;
    if (argc > 2 && !strcmp(argv[1], "--target-budget")) {
        if (!number(argv[2], SIZE_MAX, &parsed)) return 2;
        target_budget = (size_t)parsed;
        at += 2;
    }
    if (argc - at < 5 || (argc - at - 2) % 3 ||
        (argc - at - 2) / 3 > MAX_TARGETS) {
        fputs("usage: pure-mirror [--target-budget BYTES] SOURCE_STORE SOURCE_KEY TARGET_STORE TARGET_KEY EXPECTED_REVISION [TARGET_STORE TARGET_KEY EXPECTED_REVISION ...]\n", stderr);
        return 2;
    }
    size_t count = (size_t)(argc - at - 2) / 3;
    int64_t expected[MAX_TARGETS];
    for (size_t i = 0; i < count; ++i) {
        if (!number(argv[at + 4 + i * 3], INT64_MAX, &parsed)) return 2;
        expected[i] = (int64_t)parsed;
    }
    if (pure_store_init(argv[at]) != PURE_OK) return 2;
    PureRuntime source;
    PureWork source_work[1];
    if (!pure_runtime_init(&source, source_work, 1, &pure_embedded_program,
                           pure_store_host(argv[at]), PURE_OBJECT_LIMIT)) return 2;
    PureTicket read;
    PureResult status = pure_runtime_accept(&source, PURE_INPUT_READ, argv[at + 1], 0, 0, &read);
    if (status != PURE_OK) {
        printf("{\"source_status\":\"%s\"}\n", result_name(status));
        return 2;
    }
    PureOutcome snapshot;
    if (!pure_runtime_step(&source) || !pure_runtime_result(read, &snapshot)) {
        pure_runtime_release(read);
        return 2;
    }
    if (snapshot.status != PURE_OK) {
        printf("{\"source_status\":\"%s\"}\n", result_name(snapshot.status));
        pure_runtime_release(read);
        return 2;
    }
    Target targets[MAX_TARGETS] = {0};
    size_t forwarded = 0;
    for (size_t i = 0; i < count; ++i) {
        const char *store = argv[at + 2 + i * 3], *key = argv[at + 3 + i * 3];
        PureHost host = pure_store_host(store);
        host.allocate = NULL; /* Forwarded ingress uses the source mapping. */
        if (pure_store_init(store) != PURE_OK ||
            !pure_runtime_init(&targets[i].runtime, targets[i].work, 1,
                               &pure_embedded_program, host,
                               target_budget)) status = PURE_STORE_ERROR;
        else status = pure_runtime_forward(read, &targets[i].runtime, key,
                                           expected[i], &targets[i].ticket);
        if (status != PURE_OK) {
            for (size_t j = 0; j < forwarded; ++j) {
                pure_runtime_cancel(targets[j].ticket);
                pure_runtime_release(targets[j].ticket);
            }
            pure_runtime_release(read);
            printf("{\"admission_status\":\"%s\",\"target_index\":%zu}\n",
                   result_name(status), i);
            return 2;
        }
        ++forwarded;
    }
    /* Each queued target pins the mapped source snapshot independently. */
    pure_runtime_release(read);
    if (snapshot.body.size && source.retained != snapshot.body.size) {
        for (size_t i = 0; i < count; ++i) {
            pure_runtime_cancel(targets[i].ticket);
            pure_runtime_release(targets[i].ticket);
        }
        return 2;
    }
    int exit_status = 0;
    for (size_t i = 0; i < count; ++i) {
        if (!pure_runtime_step(&targets[i].runtime) ||
            !pure_runtime_result(targets[i].ticket, &targets[i].outcome)) {
            for (size_t j = i; j < count; ++j) {
                pure_runtime_cancel(targets[j].ticket);
                pure_runtime_release(targets[j].ticket);
            }
            return 2;
        }
        if (targets[i].outcome.status == PURE_CONFLICT && !exit_status) exit_status = 3;
        else if (targets[i].outcome.status != PURE_OK &&
                 targets[i].outcome.status != PURE_CONFLICT) exit_status = 2;
        pure_runtime_release(targets[i].ticket);
        if (snapshot.body.size && i + 1 < count &&
            source.retained != snapshot.body.size) {
            for (size_t j = i + 1; j < count; ++j) {
                pure_runtime_cancel(targets[j].ticket);
                pure_runtime_release(targets[j].ticket);
            }
            return 2;
        }
    }
    if (source.active || source.retained) return 2;
    printf("{\"source_revision\":\"%" PRId64 "\",\"bytes\":%zu,\"targets\":[",
           snapshot.revision, snapshot.body.size);
    for (size_t i = 0; i < count; ++i)
        printf("%s{\"index\":%zu,\"status\":\"%s\",\"revision\":\"%" PRId64 "\"}",
               i ? "," : "", i, result_name(targets[i].outcome.status),
               targets[i].outcome.revision);
    puts("]}");
    return fflush(stdout) ? 2 : exit_status;
}
