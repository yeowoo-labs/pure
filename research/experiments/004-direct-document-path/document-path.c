#define _POSIX_C_SOURCE 200809L
/* Two separately linked versions of one durable document workload. The direct
 * build invokes the same store primitives that the Pure host uses; the Pure
 * build runs the prepared editor.pure program through its execution core. */
#include "store.h"
#ifndef DIRECT_PATH
#include "runtime.h"
#include "store-port.h"
#include "embedded-plan.h"
#endif
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef PURE_BENCH_BODY_BYTES
#define PURE_BENCH_BODY_BYTES 8192
#endif
enum { BODY_BYTES = PURE_BENCH_BODY_BYTES };
static uint64_t digest = UINT64_C(14695981039346656037);

static void fail(const char *message) {
    fprintf(stderr, "%s\n", message);
    exit(1);
}

static void check(int condition, const char *message) {
    if (!condition) fail(message);
}

static void hash_body(const unsigned char *body, size_t size, int64_t revision) {
    for (size_t i = 0; i < size; ++i) {
        digest ^= body[i];
        digest *= UINT64_C(1099511628211);
    }
    digest ^= (uint64_t)revision;
    digest *= UINT64_C(1099511628211);
}

static uint64_t nanoseconds(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) fail("clock_gettime failed");
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
}

static void pattern(unsigned char *body, unsigned document) {
    for (size_t i = 0; i < BODY_BYTES; ++i)
        body[i] = (unsigned char)('a' + (document + i) % 26);
}

#ifdef DIRECT_PATH
static void create(const char *store, const char *key, unsigned document,
                   const unsigned char *expected) {
    unsigned char *input = malloc(BODY_BYTES);
    check(input != NULL, "direct input allocation failed");
    pattern(input, document);
    check(!memcmp(input, expected, BODY_BYTES), "direct input mismatch");
    int64_t revision = 0;
    PureResult status = pure_store_splice(store, key, 0, 0, 0,
                                          (PureSpan){input, BODY_BYTES}, &revision);
    check(status == PURE_OK && revision == 1, "direct create failed");
    free(input);
}

static void edit_last(const char *store, const char *key, unsigned char replacement) {
    unsigned char *input = malloc(1);
    check(input != NULL, "direct edit allocation failed");
    *input = replacement;
    int64_t revision = 0;
    PureResult status = pure_store_splice(store, key, 1, BODY_BYTES - 1, 1,
                                          (PureSpan){input, 1}, &revision);
    check(status == PURE_OK && revision == 2, "direct edit failed");
    free(input);
}

static void read_checked(const char *store, const char *key,
                         const unsigned char *expected, int64_t revision) {
    PureStoredView view = {0};
    PureResult status = pure_store_select(store, key, 0, 0, true, 0,
                                          4u * 1024u * 1024u, &view);
    check(status == PURE_OK && view.revision == revision &&
          view.body.size == BODY_BYTES && !memcmp(view.body.data, expected, BODY_BYTES),
          "direct read mismatch");
    hash_body(view.body.data, view.body.size, revision);
    pure_store_release(&view);
}
#else
static PureRuntime runtime;
static PureWork work[1];

static PureOutcome completed(PureTicket ticket) {
    PureOutcome result;
    while (!pure_runtime_result(ticket, &result))
        check(pure_runtime_step(&runtime), "runtime made no progress");
    return result;
}

static void write_checked(const char *key, int64_t expected_revision,
                          size_t offset, size_t remove, size_t insert_size,
                          unsigned document, unsigned char replacement,
                          const unsigned char *expected_reply, int64_t revision) {
    PureTicket ticket;
    check(pure_runtime_accept(&runtime, PURE_INPUT_WRITE, key, expected_revision,
                              8 + insert_size, &ticket) == PURE_OK, "runtime write admission failed");
    size_t size = 0;
    unsigned char *input = pure_runtime_ingress(ticket, &size);
    check(input != NULL && size == 8 + insert_size, "runtime input loan failed");
    for (unsigned i = 0; i < 4; ++i) {
        input[i] = (unsigned char)(offset >> (8 * i));
        input[4 + i] = (unsigned char)(remove >> (8 * i));
    }
    if (insert_size == BODY_BYTES) pattern(input + 8, document);
    else input[8] = replacement;
    check(pure_runtime_ready(ticket), "runtime publication failed");
    PureOutcome result = completed(ticket);
    check(result.status == PURE_OK && result.revision == revision &&
          result.body.size == insert_size &&
          !memcmp(result.body.data, expected_reply, insert_size),
          "runtime write result mismatch");
    check(pure_runtime_release(ticket) && !runtime.active && !runtime.retained,
          "runtime write resource not released");
}

static void read_checked(const char *store, const char *key,
                         const unsigned char *expected, int64_t revision) {
    (void)store;
    PureTicket ticket;
    check(pure_runtime_accept(&runtime, PURE_INPUT_READ, key, 0, 0, &ticket) == PURE_OK,
          "runtime read admission failed");
    PureOutcome result = completed(ticket);
    check(result.status == PURE_OK && result.revision == revision &&
          result.body.size == BODY_BYTES && !memcmp(result.body.data, expected, BODY_BYTES),
          "runtime read mismatch");
    hash_body(result.body.data, result.body.size, revision);
    check(pure_runtime_release(ticket) && !runtime.active && !runtime.retained,
          "runtime read resource not released");
}

static void create(const char *store, const char *key, unsigned document,
                   const unsigned char *expected) {
    (void)store;
    write_checked(key, 0, 0, 0, BODY_BYTES, document, 0, expected, 1);
}

static void edit_last(const char *store, const char *key, unsigned char replacement) {
    (void)store;
    write_checked(key, 1, BODY_BYTES - 1, 1, 1, 0, replacement, &replacement, 2);
}
#endif

int main(int argc, char **argv) {
    if (argc != 3) fail("usage: document-path STORE DOCUMENT_COUNT");
    char *end = NULL;
    unsigned long parsed = strtoul(argv[2], &end, 10);
    check(end && !*end && parsed > 0 && parsed <= 4096,
          "DOCUMENT_COUNT must be 1..4096");
    unsigned documents = (unsigned)parsed;
    check(pure_store_init(argv[1]) == PURE_OK, "store initialization failed");
    unsigned char *expected = malloc(BODY_BYTES);
    check(expected != NULL, "expected document allocation failed");
#ifndef DIRECT_PATH
    check(pure_embedded_program.splice && pure_embedded_program.write.offset == 8 &&
          pure_embedded_program.splice_offset.value == 0 &&
          pure_embedded_program.splice_remove.value == 4 &&
          pure_embedded_program.reply_input && pure_embedded_program.read.to_end,
          "prepared editor.pure no longer matches direct baseline");
    check(pure_runtime_init(&runtime, work, 1, &pure_embedded_program,
                            pure_store_host(argv[1]), 4u * 1024u * 1024u),
          "runtime initialization failed");
#endif
    uint64_t start = nanoseconds();
    uint64_t stages[4] = {0};
    for (unsigned document = 0; document < documents; ++document) {
        char key[16];
        snprintf(key, sizeof(key), "doc%04u", document);
        pattern(expected, document);
        uint64_t before = nanoseconds();
        create(argv[1], key, document, expected);
        stages[0] += nanoseconds() - before;
        before = nanoseconds();
        read_checked(argv[1], key, expected, 1);
        stages[1] += nanoseconds() - before;
        unsigned char replacement = (unsigned char)('A' + document % 26);
        before = nanoseconds();
        edit_last(argv[1], key, replacement);
        stages[2] += nanoseconds() - before;
        expected[BODY_BYTES - 1] = replacement;
        before = nanoseconds();
        read_checked(argv[1], key, expected, 2);
        stages[3] += nanoseconds() - before;
    }
    uint64_t elapsed = nanoseconds() - start;
    free(expected);
    printf("{\"variant\":\"%s\",\"documents\":%u,\"operations\":%u,"
           "\"body_bytes\":%u,\"elapsed_ns\":%" PRIu64 ","
           "\"stage_ns\":[%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 "],"
           "\"digest\":\"%016" PRIx64 "\"}\n",
#ifdef DIRECT_PATH
           "direct",
#else
           "pure",
#endif
           documents, documents * 4, (unsigned)BODY_BYTES, elapsed,
           stages[0], stages[1], stages[2], stages[3], digest);
    return 0;
}
