#define _POSIX_C_SOURCE 200809L
#include "runtime.h"
#include "store-port.h"
#include "store.h"
#include <dirent.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(test) do { if (!(test)) { fprintf(stderr, "line %d: %s\n", __LINE__, #test); exit(1); } } while (0)

static const PureProgram identity = {
    .read = {.to_end = true}, .write = {.to_end = true},
    .reply = {.to_end = true}, .reply_input = true
};

typedef struct {
    PureEffect effect;
    const PureProgram *definition;
    const char *directory;
    int start, done;
    PureResult status;
    PureSpan reply;
    int64_t revision;
} Worker;

static void *write_after_signal(void *context) {
    Worker *worker = context;
    char byte = 0;
    if (read(worker->start, &byte, 1) != 1) worker->status = PURE_STORE_ERROR;
    else {
        PureHost store = pure_store_host(worker->directory);
        worker->status = store.write(store.context, worker->definition,
                                     worker->effect.key, worker->effect.expected,
                                     worker->effect.value, &worker->reply,
                                     &worker->revision);
    }
    if (write(worker->done, &byte, 1) < 0) { /* full: its reader is woken already */ }
    return NULL;
}

static PureHost async_host(const char *directory) {
    PureHost host = pure_store_host(directory);
    host.read = NULL;
    host.write = NULL;
    return host;
}

static PureTicket submit(PureRuntime *runtime, const char *key,
                         const void *body, size_t size) {
    PureTicket ticket;
    CHECK(pure_runtime_accept(runtime, PURE_INPUT_WRITE, key, 0, size, &ticket) == PURE_OK);
    if (size) memcpy(pure_runtime_ingress(ticket, NULL), body, size);
    CHECK(pure_runtime_ready(ticket));
    return ticket;
}

static void expect(const char *directory, const char *key, const void *body, size_t size) {
    PureStoredView view;
    CHECK(pure_store_get(directory, key, 64, &view) == PURE_OK);
    CHECK(view.body.size == size && !memcmp(view.body.data, body, size));
    pure_store_release(&view);
}

static void worker_and_forward(const char *directory) {
    PureRuntime source, destination;
    PureWork source_slots[2], destination_slot[1];
    CHECK(pure_runtime_init(&source, source_slots, 2, &identity,
                            async_host(directory), 64));
    PureTicket ignored = {0};
    CHECK(pure_runtime_accept(&source, PURE_INPUT_WRITE, "denied", 0, 1,
                              &ignored) == PURE_DENIED);
    CHECK(pure_runtime_enable_async(&source));
    PureTicket slow = submit(&source, "slow", "alpha", 5);
    PureTicket fast = submit(&source, "fast", "beta", 4);
    CHECK(!pure_runtime_step(&source)); /* No synchronous host callback. */
    PureEffect deferred;
    CHECK(pure_runtime_take(&source, &deferred) == PURE_TAKE_EFFECT);
    CHECK(deferred.ticket.generation == slow.generation &&
          deferred.value.size == 5 && !memcmp(deferred.value.data, "alpha", 5));
    CHECK(!pure_runtime_cancel(slow));
    int start_pipe[2], done_pipe[2];
    CHECK(!pipe(start_pipe) && !pipe(done_pipe));
    Worker worker = {.effect = deferred, .definition = &identity, .directory = directory,
                     .start = start_pipe[0], .done = done_pipe[1]};
    pthread_t thread;
    CHECK(!pthread_create(&thread, NULL, write_after_signal, &worker));
    CHECK(pure_runtime_release(slow));
    CHECK(source.active == 2 && source.retained == 9);
    PureEffect immediate;
    CHECK(pure_runtime_take(&source, &immediate) == PURE_TAKE_EFFECT);
    int64_t revision = 0;
    PureHost store = pure_store_host(directory);
    PureSpan reply = {0};
    PureResult written = store.write(store.context, &identity, immediate.key,
                                     immediate.expected, immediate.value, &reply, &revision);
    CHECK(pure_runtime_complete(immediate.ticket, written, NULL, reply, revision));
    PureOutcome result;
    CHECK(pure_runtime_result(fast, &result) && result.status == PURE_OK &&
          result.body.size == 4 && !memcmp(result.body.data, "beta", 4));
    CHECK(pure_runtime_release(fast));
    expect(directory, "fast", "beta", 4);
    PureStoredView absent;
    CHECK(pure_store_get(directory, "slow", 64, &absent) == PURE_NOT_FOUND);
    pure_runtime_stop(&source);
    CHECK(!pure_runtime_restart(&source, &identity, async_host(directory), 64));
    CHECK(pure_runtime_accept(&source, PURE_INPUT_READ, "fast", 0, 0,
                              &ignored) == PURE_CLOSED);
    CHECK(write(start_pipe[1], "x", 1) == 1);
    struct pollfd done = {.fd = done_pipe[0], .events = POLLIN};
    CHECK(poll(&done, 1, 5000) == 1 && (done.revents & POLLIN));
    CHECK(!pthread_join(thread, NULL));
    CHECK(pure_runtime_complete(deferred.ticket, worker.status, NULL,
                                worker.reply, worker.revision));
    CHECK(!pure_runtime_complete(deferred.ticket, PURE_OK, NULL, (PureSpan){0}, 1));
    CHECK(!pure_runtime_result(slow, &result)); /* Caller released before I/O ended. */
    CHECK(source.active == 0 && source.retained == 0 && source.commits == 2);
    expect(directory, "slow", "alpha", 5);
    CHECK(!close(start_pipe[0]) && !close(start_pipe[1]) &&
          !close(done_pipe[0]) && !close(done_pipe[1]));
    CHECK(pure_runtime_restart(&source, &identity, async_host(directory), 64));

    CHECK(pure_runtime_init(&destination, destination_slot, 1, &identity,
                            async_host(directory), 64));
    CHECK(pure_runtime_enable_async(&destination));
    PureTicket read_ticket;
    CHECK(pure_runtime_accept(&source, PURE_INPUT_READ, "fast", 0, 0,
                              &read_ticket) == PURE_OK);
    PureEffect read_effect;
    CHECK(pure_runtime_take(&source, &read_effect) == PURE_TAKE_EFFECT &&
          read_effect.input == PURE_INPUT_READ);
    PureResource mapping = {0};
    revision = 0;
    PureResult read_status = store.read(store.context, &identity, read_effect.key,
                                        read_effect.available, &mapping, &revision);
    CHECK(pure_runtime_complete(read_effect.ticket, read_status, &mapping,
                                (PureSpan){0}, revision));
    CHECK(!mapping.allocation);
    CHECK(pure_runtime_result(read_ticket, &result) && result.status == PURE_OK &&
          result.body.size == 4);
    PureTicket forwarded;
    CHECK(pure_runtime_forward(read_ticket, &destination, "copy", 0,
                               &forwarded) == PURE_OK);
    const unsigned char *borrowed = result.body.data;
    CHECK(pure_runtime_release(read_ticket));
    CHECK(source.active == 1 && source.retained == 4);
    PureEffect output;
    CHECK(pure_runtime_take(&destination, &output) == PURE_TAKE_EFFECT);
    CHECK(output.value.data == borrowed && output.value.size == 4);
    revision = 0;
    reply = (PureSpan){0};
    written = store.write(store.context, &identity, output.key, output.expected,
                          output.value, &reply, &revision);
    CHECK(pure_runtime_complete(output.ticket, written, NULL, reply, revision));
    CHECK(pure_runtime_result(forwarded, &result) && result.status == PURE_OK &&
          result.body.data == borrowed);
    CHECK(pure_runtime_release(forwarded));
    CHECK(source.active == 0 && source.retained == 0 &&
          destination.active == 0 && destination.retained == 0);
    expect(directory, "copy", "beta", 4);
}

static void rejection_and_budget(const char *directory) {
    PureProgram invalid = identity;
    invalid.input_required = 4;
    PureRuntime runtime;
    PureWork slots[2];
    CHECK(pure_runtime_init(&runtime, slots, 2, &invalid,
                            async_host(directory), 6));
    CHECK(pure_runtime_enable_async(&runtime));
    PureTicket rejected = submit(&runtime, "bad", "x", 1);
    PureEffect effect;
    CHECK(pure_runtime_take(&runtime, &effect) == PURE_TAKE_EFFECT);
    PureHost store = pure_store_host(directory);
    PureSpan reply = {0};
    int64_t revision = 0;
    PureResult status = store.write(store.context, &invalid, effect.key,
                                    effect.expected, effect.value, &reply, &revision);
    CHECK(status == PURE_LIMIT);
    CHECK(pure_runtime_complete(effect.ticket, status, NULL, reply, revision));
    PureOutcome result;
    CHECK(pure_runtime_result(rejected, &result) && result.status == PURE_LIMIT);
    CHECK(!pure_runtime_complete(rejected, PURE_OK, NULL, (PureSpan){0}, 1));
    CHECK(pure_runtime_release(rejected));
    CHECK(runtime.active == 0 && runtime.retained == 0);

    CHECK(pure_runtime_restart(&runtime, &identity, async_host(directory), 6));
    PureTicket cancelled = submit(&runtime, "unused", "x", 1);
    CHECK(pure_runtime_cancel(cancelled));
    CHECK(pure_runtime_result(cancelled, &result) && result.status == PURE_CANCELLED);
    CHECK(pure_runtime_release(cancelled));
    PureTicket invalid_read;
    CHECK(pure_runtime_accept(&runtime, PURE_INPUT_READ, "fast", 0, 0,
                              &invalid_read) == PURE_OK);
    PureEffect invalid_effect;
    CHECK(pure_runtime_take(&runtime, &invalid_effect) == PURE_TAKE_EFFECT);
    PureResource foreign = {.body = {(const unsigned char *)"x", 1}};
    CHECK(!pure_runtime_complete(invalid_effect.ticket, PURE_OK, &foreign,
                                 (PureSpan){0}, 1));
    CHECK(foreign.body.size == 1 && runtime.active == 1);
    CHECK(pure_runtime_complete(invalid_effect.ticket, PURE_STORE_ERROR, NULL,
                                (PureSpan){0}, 0));
    CHECK(pure_runtime_release(invalid_read));
    PureTicket bad_reply = submit(&runtime, "bad_reply", "x", 1);
    PureEffect bad_effect;
    CHECK(pure_runtime_take(&runtime, &bad_effect) == PURE_TAKE_EFFECT);
    CHECK(!pure_runtime_complete(bad_effect.ticket, PURE_OK, NULL,
                                 (PureSpan){(const unsigned char *)"outside", 7}, 1));
    CHECK(runtime.active == 1 && runtime.retained == 1);
    CHECK(pure_runtime_complete(bad_effect.ticket, PURE_STORE_ERROR, NULL,
                                (PureSpan){0}, 0));
    CHECK(pure_runtime_release(bad_reply) && runtime.active == 0 && runtime.retained == 0);
    PureTicket first, second;
    CHECK(pure_runtime_accept(&runtime, PURE_INPUT_READ, "fast", 0, 0, &first) == PURE_OK);
    CHECK(pure_runtime_accept(&runtime, PURE_INPUT_READ, "fast", 0, 0, &second) == PURE_OK);
    PureEffect one, two;
    CHECK(pure_runtime_take(&runtime, &one) == PURE_TAKE_EFFECT);
    CHECK(pure_runtime_take(&runtime, &two) == PURE_TAKE_EFFECT);
    CHECK(pure_runtime_take(&runtime, &effect) == PURE_TAKE_EMPTY);
    PureResource a = {0}, b = {0};
    int64_t revision_a = 0, revision_b = 0;
    CHECK(store.read(store.context, &identity, one.key, one.available,
                     &a, &revision_a) == PURE_OK);
    CHECK(store.read(store.context, &identity, two.key, two.available,
                     &b, &revision_b) == PURE_OK);
    CHECK(pure_runtime_complete(two.ticket, PURE_OK, &b, (PureSpan){0}, revision_b));
    CHECK(!b.allocation);
    CHECK(pure_runtime_complete(one.ticket, PURE_OK, &a, (PureSpan){0}, revision_a));
    CHECK(!a.allocation);
    CHECK(pure_runtime_result(first, &result) && result.status == PURE_BUDGET);
    CHECK(pure_runtime_result(second, &result) && result.status == PURE_OK);
    CHECK(runtime.budget_rejections == 1 && runtime.retained == 4);
    CHECK(pure_runtime_release(first) && pure_runtime_release(second));
    CHECK(runtime.active == 0 && runtime.retained == 0);
}

/* A resource sent in pieces is charged like any other and released with its
 * ticket (or at once when it does not fit); it cannot be forwarded. */
static int loans_released;
static void release_loan(void *allocation, size_t size) {
    (void)allocation;
    (void)size;
    ++loans_released;
}

static void vectored_loan(const char *directory) {
    static int mapping;
    PureRuntime runtime, other;
    PureWork slots[2], other_slots[1];
    CHECK(pure_runtime_init(&runtime, slots, 2, &identity, async_host(directory), 6));
    CHECK(pure_runtime_init(&other, other_slots, 1, &identity, async_host(directory), 64));
    CHECK(pure_runtime_enable_async(&runtime) && pure_runtime_enable_async(&other));
    PureTicket first, second;
    CHECK(pure_runtime_accept(&runtime, PURE_INPUT_READ, "lent", 0, 0, &first) == PURE_OK);
    CHECK(pure_runtime_accept(&runtime, PURE_INPUT_READ, "lent", 0, 0, &second) == PURE_OK);
    PureEffect one, two;
    CHECK(pure_runtime_take(&runtime, &one) == PURE_TAKE_EFFECT && pure_runtime_take(&runtime, &two) == PURE_TAKE_EFFECT);
    PureResource a = {.body = {NULL, 4}, .allocation = &mapping, .allocation_size = 4, .release = release_loan}, b = a;
    CHECK(pure_runtime_complete(one.ticket, PURE_OK, &a, (PureSpan){0}, 1) && !a.release);
    PureOutcome result;
    CHECK(pure_runtime_result(first, &result) && result.status == PURE_OK && !result.body.data && result.body.size == 4);
    CHECK(runtime.retained == 4 && loans_released == 0);
    CHECK(pure_runtime_complete(two.ticket, PURE_OK, &b, (PureSpan){0}, 1));
    CHECK(pure_runtime_result(second, &result) && result.status == PURE_BUDGET && loans_released == 1);
    PureTicket forwarded;
    CHECK(pure_runtime_forward(first, &other, "copy", 0, &forwarded) == PURE_INVALID);
    CHECK(pure_runtime_release(first) && loans_released == 2 && runtime.retained == 0);
    CHECK(pure_runtime_release(second) && runtime.active == 0 && other.active == 0);
}

static void authored_document(const char *directory) {
    PureProgram program;
    char error[160];
    CHECK(pure_program_load("examples/editor.pure", &program, error, sizeof(error)));
    PureRuntime runtime;
    PureWork slot[1];
    CHECK(pure_runtime_init(&runtime, slot, 1, &program,
                            async_host(directory), 64));
    CHECK(pure_runtime_enable_async(&runtime));
    const unsigned char create[] = {0,0,0,0, 0,0,0,0, 'H','i'};
    PureTicket ticket = submit(&runtime, "document", create, sizeof(create));
    PureEffect effect;
    CHECK(pure_runtime_take(&runtime, &effect) == PURE_TAKE_EFFECT &&
          effect.value.size == sizeof(create));
    PureSpan value, reply = {0};
    size_t offset, remove;
    CHECK(pure_program_splice(&program, effect.value, &offset, &remove) &&
          pure_program_input(&program, effect.value, &value, &reply) &&
          offset == 0 && remove == 0 && value.size == 2 &&
          !memcmp(value.data, "Hi", 2));
    PureHost store = pure_store_host(directory);
    int64_t revision = 0;
    PureResult status = store.write(store.context, &program, effect.key,
                                    effect.expected, effect.value, &reply, &revision);
    CHECK(pure_runtime_complete(effect.ticket, status, NULL, reply, revision));
    PureOutcome result;
    CHECK(pure_runtime_result(ticket, &result) && result.status == PURE_OK &&
          result.body.size == 2 && !memcmp(result.body.data, "Hi", 2));
    CHECK(pure_runtime_release(ticket));

    const unsigned char append[] = {2,0,0,0, 0,0,0,0, '!'};
    CHECK(pure_runtime_accept(&runtime, PURE_INPUT_WRITE, "document", 1,
                              sizeof(append), &ticket) == PURE_OK);
    memcpy(pure_runtime_ingress(ticket, NULL), append, sizeof(append));
    CHECK(pure_runtime_ready(ticket));
    CHECK(pure_runtime_take(&runtime, &effect) == PURE_TAKE_EFFECT &&
          effect.value.size == sizeof(append));
    CHECK(pure_program_splice(&program, effect.value, &offset, &remove) &&
          offset == 2 && remove == 0);
    reply = (PureSpan){0};
    status = store.write(store.context, &program, effect.key, effect.expected,
                         effect.value, &reply, &revision);
    CHECK(pure_runtime_complete(effect.ticket, status, NULL, reply, revision));
    CHECK(pure_runtime_result(ticket, &result) && result.status == PURE_OK &&
          result.body.size == 1 && result.body.data[0] == '!');
    CHECK(pure_runtime_release(ticket));
    expect(directory, "document", "Hi!", 3);

    ticket = submit(&runtime, "document", append, sizeof(append));
    CHECK(pure_runtime_take(&runtime, &effect) == PURE_TAKE_EFFECT);
    reply = (PureSpan){0};
    status = store.write(store.context, &program, effect.key, effect.expected,
                         effect.value, &reply, &revision);
    CHECK(pure_runtime_complete(effect.ticket, status, NULL, reply, revision));
    CHECK(pure_runtime_result(ticket, &result) && result.status == PURE_CONFLICT &&
          !result.body.size);
    CHECK(pure_runtime_release(ticket));
    expect(directory, "document", "Hi!", 3);

    CHECK(pure_runtime_accept(&runtime, PURE_INPUT_READ, "document", 0, 0,
                              &ticket) == PURE_OK);
    CHECK(pure_runtime_take(&runtime, &effect) == PURE_TAKE_EFFECT);
    PureResource mapping = {0};
    status = store.read(store.context, &program, effect.key,
                        effect.available, &mapping, &revision);
    CHECK(pure_runtime_complete(effect.ticket, status, &mapping,
                                (PureSpan){0}, revision));
    CHECK(pure_runtime_result(ticket, &result) && result.status == PURE_OK &&
          result.body.size == 3 && !memcmp(result.body.data, "Hi!", 3));
    CHECK(pure_runtime_release(ticket));
    CHECK(runtime.active == 0 && runtime.retained == 0 &&
          runtime.commits == 2 && runtime.conflicts == 1 && runtime.gets == 1);
}

int main(void) {
    char template[] = "/tmp/pure-async-effects-XXXXXX";
    char *directory = mkdtemp(template);
    CHECK(directory && pure_store_init(directory) == PURE_OK);
    worker_and_forward(directory);
    rejection_and_budget(directory);
    vectored_loan(directory);
    authored_document(directory);
    DIR *opened = opendir(directory);
    CHECK(opened);
    struct dirent *entry;
    while ((entry = readdir(opened))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        CHECK(unlinkat(dirfd(opened), entry->d_name, 0) == 0);
    }
    CHECK(!closedir(opened) && !rmdir(directory));
    puts("{\"async_effects\":\"authored-document-worker-overlap-release-forward-budget-recovery-vectored-loan\"}");
}
