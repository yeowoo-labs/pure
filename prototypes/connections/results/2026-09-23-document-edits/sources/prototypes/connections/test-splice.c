#define _POSIX_C_SOURCE 200809L
#include "runtime.h"
#include "store-port.h"
#include "store.h"
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
static int failed_stage = -1;
int pure_store_test_fault(PureStoreStage stage) { return (int)stage == failed_stage; }

static void expect(const char *path, const char *key, int64_t revision, PureSpan bytes) {
    PureStoredView view;
    CHECK(pure_store_get(path, key, PURE_OBJECT_LIMIT, &view) == PURE_OK);
    CHECK(view.revision == revision && view.body.size == bytes.size);
    CHECK(!bytes.size || !memcmp(view.body.data, bytes.data, bytes.size));
    pure_store_release(&view);
}

static void parser(void) {
    const char *get = "on get { let o = store.read(request.key); reply(o, o.body); }";
    const char *layout = "layout E { marker: bytes[1]; remove: u32le; start: u32le; insert: bytes; }";
    char source[2048], error[160];
    PureProgram p;
    snprintf(source, sizeof(source), "%s %s on put { let e = view(E, request.body); "
        "let s = store.splice(request.key, request.revision, e.start, e.remove, e.insert); reply(s, e.insert); }", layout, get);
    CHECK(pure_program_parse(source, strlen(source), &p, error, sizeof(error)));
    CHECK(p.splice && p.input_required == 9 && p.splice_offset.value == 5 && p.splice_remove.value == 1);
    unsigned char input[] = {0, 2,0,0,0, 3,0,0,0, 0,255};
    PureSpan write, reply;
    size_t offset, remove;
    for (size_t n = 0; n < 9; ++n)
        CHECK(!pure_program_input(&p, (PureSpan){input, n}, &write, &reply));
    CHECK(pure_program_input(&p, (PureSpan){input, sizeof(input)}, &write, &reply));
    CHECK(write.size == 2 && write.data == input + 9 && reply.data == write.data);
    CHECK(pure_program_splice(&p, (PureSpan){input, sizeof(input)}, &offset, &remove));
    CHECK(offset == 3 && remove == 2);

    snprintf(source, sizeof(source), "%s on put { let s = store.splice(request.key, request.revision, 0, 0, request.body); reply(s); }", get);
    CHECK(pure_program_parse(source, strlen(source), &p, error, sizeof(error)));
    CHECK(pure_program_splice(&p, (PureSpan){0}, &offset, &remove) && offset == 0 && remove == 0);
    const char *invalid[] = {
        "let s = store.splice(request.key, request.revision, e.insert, 0, e.insert); reply(s);",
        "let s = store.splice(request.key, request.revision, 0, 1449985, e.insert); reply(s);",
        "let s = store.splice(request.key, request.revision, 0, 0, e.start); reply(s);",
        "let s = store.splice(request.key, request.revision, 0, 0, e.insert); let t = store.write(request.key, request.revision, e.insert); reply(t);"
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(*invalid); ++i) {
        snprintf(source, sizeof(source), "%s %s on put { let e = view(E, request.body); %s }", layout, get, invalid[i]);
        unsigned char before[sizeof(p)];
        memcpy(before, &p, sizeof(p));
        CHECK(!pure_program_parse(source, strlen(source), &p, error, sizeof(error)));
        CHECK(*error && !memcmp(before, &p, sizeof(p)));
    }
}

static void storage(const char *path) {
    /* Reference whole-value assembly exists only in this test oracle. */
    unsigned char oracle[65536], before[65536], insert[9000];
    for (size_t i = 0; i < sizeof(insert); ++i) insert[i] = (unsigned char)(i * 19u);
    size_t length = 49157;
    for (size_t i = 0; i < length; ++i) oracle[i] = (unsigned char)((i * 17u) ^ (i >> 8));
    int64_t revision;
    CHECK(pure_store_splice(path, "work", 0, 0, 0, (PureSpan){oracle, length}, &revision) == PURE_OK);
    CHECK(revision == 1);
    /* Prefix/suffix copies cross both scratch and mapping page boundaries. */
    const size_t edits[][3] = {{16381, 7, 9000}, {0, 0, 5}, {50000, 1, 0}, {4095, 4097, 3}, {0, 0, 0}};
    for (size_t i = 0; i < sizeof(edits) / sizeof(*edits); ++i) {
        size_t offset = edits[i][0], remove = edits[i][1], count = edits[i][2], previous = length;
        memcpy(before, oracle, length);
        PureStoredView old;
        CHECK(pure_store_get(path, "work", PURE_OBJECT_LIMIT, &old) == PURE_OK);
        int64_t expected = revision;
        CHECK(pure_store_splice(path, "work", expected, offset, remove, (PureSpan){insert, count}, &revision) == PURE_OK);
        CHECK(revision == expected + 1);
        memmove(oracle + offset + count, oracle + offset + remove, length - offset - remove);
        memcpy(oracle + offset, insert, count);
        length = length - remove + count;
        expect(path, "work", revision, (PureSpan){oracle, length});
        CHECK(old.body.size == previous && !memcmp(old.body.data, before, previous));
        pure_store_release(&old);
    }
    int64_t latest = revision;
    CHECK(pure_store_splice(path, "work", latest - 1, 0, 0, (PureSpan){insert, 1}, &revision) == PURE_CONFLICT);
    CHECK(revision == latest);
    CHECK(pure_store_splice(path, "work", latest, SIZE_MAX, 0, (PureSpan){0}, &revision) == PURE_LIMIT);
    CHECK(pure_store_splice(path, "work", latest, 0, SIZE_MAX, (PureSpan){0}, &revision) == PURE_LIMIT);
    CHECK(pure_store_splice(path, "work", latest, length + 1, 0, (PureSpan){0}, &revision) == PURE_LIMIT);
    /* Rejected before reading the intentionally undersized insertion pointer. */
    CHECK(pure_store_splice(path, "work", latest, 0, 0, (PureSpan){insert, PURE_OBJECT_LIMIT}, &revision) == PURE_LIMIT);
    expect(path, "work", latest, (PureSpan){oracle, length});
    CHECK(pure_store_splice(path, "missing", 0, 1, 0, (PureSpan){0}, &revision) == PURE_LIMIT);
    PureStoredView view;
    CHECK(pure_store_get(path, "missing", 0, &view) == PURE_NOT_FOUND);
    for (failed_stage = PURE_AFTER_HEADER; failed_stage <= PURE_BEFORE_RENAME; ++failed_stage) {
        CHECK(pure_store_splice(path, "work", latest, 1, 3, (PureSpan){insert, 5}, &revision) == PURE_STORE_ERROR);
        expect(path, "work", latest, (PureSpan){oracle, length});
    }
    failed_stage = PURE_BEFORE_DIR_SYNC;
    CHECK(pure_store_splice(path, "work", latest, length, 0, (PureSpan){insert, 5}, &revision) == PURE_UNCERTAIN);
    CHECK(revision == latest + 1);
    failed_stage = -1;
    memcpy(oracle + length, insert, 5); length += 5;
    expect(path, "work", revision, (PureSpan){oracle, length});
    CHECK(pure_store_splice(path, "work", revision, 0, length, (PureSpan){0}, &revision) == PURE_OK);
    expect(path, "work", revision, (PureSpan){0});
    char file[256];
    snprintf(file, sizeof(file), "%s/obj-work", path);
    int fd = open(file, O_WRONLY);
    const unsigned char maximum[] = {255,255,255,255,255,255,255,127};
    CHECK(fd >= 0 && pwrite(fd, maximum, 8, 8) == 8 && close(fd) == 0);
    CHECK(pure_store_splice(path, "work", INT64_MAX, 0, 0, (PureSpan){0}, &revision) == PURE_LIMIT);
}

static void runtime(const char *path) {
    PureProgram program;
    char error[160];
    CHECK(pure_program_load("examples/editor.pure", &program, error, sizeof(error)));
    PureRuntime runtime;
    PureWork slots[1];
    PureHost denied = pure_store_host(path);
    denied.splice = NULL; /* ordinary writes do not grant permission to splice */
    CHECK(pure_runtime_init(&runtime, slots, 1, &program, denied, 32));
    PureTicket ticket = {0};
    CHECK(pure_runtime_accept(&runtime, PURE_INPUT_WRITE, "native", 0, 9, &ticket) == PURE_DENIED);
    CHECK(!ticket.runtime && runtime.active == 0 && runtime.retained == 0);
    CHECK(pure_runtime_restart(&runtime, &program, pure_store_host(path), 32));
    CHECK(pure_runtime_accept(&runtime, PURE_INPUT_WRITE, "native", 0, 9, &ticket) == PURE_OK);
    CHECK(pure_runtime_cancel(ticket) && pure_runtime_release(ticket));
    CHECK(!pure_runtime_step(&runtime));
    PureStoredView view;
    CHECK(pure_store_get(path, "native", 32, &view) == PURE_NOT_FOUND);
    CHECK(pure_runtime_accept(&runtime, PURE_INPUT_WRITE, "native", 0, 7, &ticket) == PURE_OK);
    memset(pure_runtime_ingress(ticket, NULL), 0, 7);
    CHECK(pure_runtime_ready(ticket) && pure_runtime_step(&runtime));
    PureOutcome result;
    CHECK(pure_runtime_result(ticket, &result) && result.status == PURE_LIMIT);
    CHECK(pure_runtime_release(ticket) && runtime.commits == 0);
    CHECK(pure_runtime_accept(&runtime, PURE_INPUT_WRITE, "native", 0, 9, &ticket) == PURE_OK);
    unsigned char *input = pure_runtime_ingress(ticket, NULL);
    memset(input, 0, 8); input[8] = 'x';
    CHECK(pure_runtime_ready(ticket) && pure_runtime_step(&runtime));
    CHECK(pure_runtime_result(ticket, &result) && result.status == PURE_OK && result.revision == 1);
    CHECK(result.body.size == 1 && result.body.data == input + 8 && runtime.retained == 9);
    expect(path, "native", 1, (PureSpan){(const unsigned char *)"x", 1});
    CHECK(pure_runtime_release(ticket) && runtime.retained == 0 && runtime.active == 0);
}

int main(void) {
    parser();
    char path[] = "/tmp/pure-splice-check-XXXXXX";
    CHECK(mkdtemp(path) && pure_store_init(path) == PURE_OK);
    storage(path);
    runtime(path);
    DIR *entries = opendir(path);
    CHECK(entries);
    struct dirent *entry;
    while ((entry = readdir(entries))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        CHECK(strncmp(entry->d_name, ".pending-", 9));
        CHECK(unlinkat(dirfd(entries), entry->d_name, 0) == 0);
    }
    CHECK(closedir(entries) == 0 && rmdir(path) == 0);
    puts("{\"splice_tests\":\"passed\",\"checks\":[\"authored-fields-and-atomic-parse-failure\","
         "\"binary-edit-oracle-and-old-snapshot\",\"bounds-conflict-size-overflow\","
         "\"publication-failures-and-uncertain-durability\",\"cancel-admission-and-borrowed-reply\"]}");
    return 0;
}
