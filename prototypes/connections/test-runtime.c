#define _POSIX_C_SOURCE 200809L
#include "runtime.h"
#include "store-port.h"
#include "store.h"
#include <assert.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const PureProgram identity = {
    .read = {.to_end = true}, .write = {.to_end = true},
    .reply = {.to_end = true}, .reply_input = true
};

static void remove_store(const char *path) {
    DIR *directory = opendir(path);
    assert(directory);
    struct dirent *entry;
    while ((entry = readdir(directory))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        assert(unlinkat(dirfd(directory), entry->d_name, 0) == 0);
    }
    assert(closedir(directory) == 0 && rmdir(path) == 0);
}
typedef struct {
    size_t allocations, writes, frees, live;
    bool allocation_failure, invalid_allocation, oversized_read, foreign_reply, reenter, reenter_release;
    PureRuntime *runtime;
    PureTicket release_ticket;
    unsigned char received[256];
    size_t received_size;
} Host;
typedef struct { Host *host; unsigned char bytes[]; } Allocation;

static void free_input(void *pointer, size_t size) {
    Allocation *a = pointer;
    assert(size > 0 && a->host->live > 0);
    if (a->host->reenter_release) {
        PureTicket ignored = {0};
        assert(!pure_runtime_release(a->host->release_ticket));
        assert(pure_runtime_accept(a->host->runtime, PURE_INPUT_WRITE,
                                   "reenter", 0, 0, &ignored) == PURE_INVALID);
    }
    ++a->host->frees;
    --a->host->live;
    free(a);
}

static bool allocate(void *context, size_t size, PureResource *out) {
    Host *host = context;
    if (host->allocation_failure) return false;
    Allocation *a = malloc(sizeof(*a) + size);
    assert(a);
    a->host = host;
    ++host->allocations;
    ++host->live;
    *out = (PureResource){.body = {a->bytes, size + (host->invalid_allocation ? 1u : 0u)}, .allocation = a,
        .allocation_size = size, .release = free_input};
    return true;
}

static PureResult write_value(void *context, const void *definition, const char *key,
                              int64_t expected, PureSpan input, PureSpan *reply,
                              int64_t *revision) {
    Host *host = context;
    (void)key;
    PureSpan body;
    if (!pure_program_input(definition, input, &body, reply)) return PURE_LIMIT;
    if (host->foreign_reply) {
        *reply = (PureSpan){(const unsigned char *)"outside", 7};
        *revision = 1;
        return PURE_OK;
    }
    if (host->reenter) {
        PureTicket ignored;
        assert(pure_runtime_accept(host->runtime, PURE_INPUT_WRITE, "reenter", 0, 0, &ignored) == PURE_INVALID);
        assert(!pure_runtime_step(host->runtime));
    }
    if (expected == 99) { *revision = 1; return PURE_CONFLICT; }
    ++host->writes;
    assert(body.size <= sizeof(host->received));
    if (body.size) memcpy(host->received, body.data, body.size);
    host->received_size = body.size;
    *revision = 1;
    return PURE_OK;
}

static PureResult read_value(void *context, const void *definition, const char *key,
                             size_t available, PureResource *out, int64_t *revision) {
    Host *host = context;
    (void)definition; (void)key;
    if (!host->oversized_read) return PURE_NOT_FOUND;
    assert(available < 1000);
    assert(allocate(host, available + 1, out));
    *revision = 1;
    return PURE_OK; /* Deliberately violates the available-credit contract. */
}

static PureHost port(Host *host) {
    return (PureHost){.context = host, .allocate = allocate, .read = read_value, .write = write_value};
}

static PureTicket submit(PureRuntime *r, const char *key, const void *bytes, size_t length) {
    PureTicket ticket;
    assert(pure_runtime_accept(r, PURE_INPUT_WRITE, key, 0, length, &ticket) == PURE_OK);
    size_t capacity;
    unsigned char *input = pure_runtime_ingress(ticket, &capacity);
    assert(capacity == length);
    if (length) memcpy(input, bytes, length);
    assert(pure_runtime_ready(ticket));
    assert(!pure_runtime_ready(ticket));
    return ticket;
}

static PureOutcome completed(PureRuntime *r, PureTicket ticket) {
    PureOutcome result;
    while (!pure_runtime_result(ticket, &result)) assert(pure_runtime_step(r));
    return result;
}

static void admission_and_lifecycle(void) {
    Host host = {0};
    PureRuntime r;
    PureWork work[3];
    assert(pure_runtime_init(&r, work, 3, &identity, port(&host), 5));
    host.runtime = &r;
    PureTicket a, untouched = {0}, out = untouched;
    assert(pure_runtime_accept(&r, PURE_INPUT_WRITE, "../bad", 0, 1, &out) == PURE_INVALID);
    assert(!out.runtime && !host.allocations);
    assert(pure_runtime_accept(&r, PURE_INPUT_WRITE, "a", 0, 4, &a) == PURE_OK);
    assert(r.retained == 4 && !pure_runtime_step(&r) && !host.writes);
    assert(pure_runtime_accept(&r, PURE_INPUT_WRITE, "b", 0, 2, &out) == PURE_BUDGET);
    assert(!out.runtime && host.allocations == 1);
    host.reenter_release = true;
    host.release_ticket = a;
    assert(pure_runtime_cancel(a));
    host.reenter_release = false;
    PureOutcome cancelled;
    assert(pure_runtime_result(a, &cancelled) && cancelled.status == PURE_CANCELLED);
    assert(r.retained == 0 && host.frees == 1 && !pure_runtime_cancel(a));
    assert(pure_runtime_release(a) && !pure_runtime_release(a));
    assert(!pure_runtime_result(a, &cancelled));

    PureTicket first = submit(&r, "first", "1", 1);
    PureTicket middle = submit(&r, "middle", "2", 1);
    PureTicket last = submit(&r, "last", "3", 1);
    assert(pure_runtime_accept(&r, PURE_INPUT_WRITE, "full", 0, 0, &out) == PURE_BUSY);
    assert(pure_runtime_cancel(middle));
    assert(pure_runtime_release(middle));
    pure_runtime_stop(&r);
    assert(pure_runtime_accept(&r, PURE_INPUT_WRITE, "closed", 0, 0, &out) == PURE_CLOSED);
    assert(!pure_runtime_restart(&r, &identity, port(&host), 5));
    host.reenter = true;
    assert(completed(&r, first).status == PURE_OK && host.received[0] == '1');
    assert(completed(&r, last).status == PURE_OK && host.received[0] == '3');
    assert(!pure_runtime_step(&r));
    assert(pure_runtime_release(first) && pure_runtime_release(last));
    assert(r.active == 0 && r.retained == 0 && host.live == 0);
    assert(pure_runtime_restart(&r, &identity, port(&host), 5));
    PureTicket next = submit(&r, "next", "4", 1);
    assert(next.generation > last.generation && !pure_runtime_ready(a));
    assert(!pure_runtime_release(last));
    host.reenter_release = true;
    host.release_ticket = next;
    assert(pure_runtime_release(next)); /* queued input: no effect */
    host.reenter_release = false;
    assert(host.writes == 2 && !pure_runtime_step(&r));

    host.allocation_failure = true;
    assert(pure_runtime_accept(&r, PURE_INPUT_WRITE, "no_memory", 0, 1, &out) == PURE_ALLOCATION);
    assert(r.active == 0 && r.retained == 0);
    host.allocation_failure = false;
    host.invalid_allocation = host.reenter_release = true;
    assert(pure_runtime_accept(&r, PURE_INPUT_WRITE, "bad_allocation", 0, 1, &out) == PURE_ALLOCATION);
    assert(r.active == 0 && r.retained == 0 && host.live == 0);
    host.invalid_allocation = host.reenter_release = false;
    PureHost denied = port(&host);
    denied.write = NULL;
    assert(pure_runtime_restart(&r, &identity, denied, 5));
    assert(pure_runtime_accept(&r, PURE_INPUT_WRITE, "denied", 0, 1, &out) == PURE_DENIED);
    assert(pure_runtime_restart(&r, &identity, port(&host), 5));
    host.oversized_read = true;
    assert(pure_runtime_accept(&r, PURE_INPUT_READ, "bad_host", 0, 0, &out) == PURE_OK);
    assert(completed(&r, out).status == PURE_BUDGET && host.live == 0 && r.retained == 0);
    assert(pure_runtime_release(out));
    host.foreign_reply = true;
    PureTicket invalid_reply = submit(&r, "bad_reply", "x", 1);
    assert(completed(&r, invalid_reply).status == PURE_STORE_ERROR);
    assert(r.retained == 0 && host.live == 0 && pure_runtime_release(invalid_reply));
    host.foreign_reply = false;
    r.serial = UINT64_MAX;
    assert(pure_runtime_accept(&r, PURE_INPUT_WRITE, "exhausted", 0, 0, &out) == PURE_LIMIT);
}

static void forwarding(void) {
    Host a = {0}, b = {0}, c = {0};
    PureRuntime source, left, right;
    PureWork source_work[1], left_work[1], right_work[1];
    assert(pure_runtime_init(&source, source_work, 1, &identity, port(&a), 8));
    assert(pure_runtime_init(&left, left_work, 1, &identity, port(&b), 2));
    assert(pure_runtime_init(&right, right_work, 1, &identity, port(&c), 8));
    PureTicket original = submit(&source, "original", "value", 5), x = {0}, y;
    PureOutcome result = completed(&source, original);
    assert(result.status == PURE_OK);
    const unsigned char *backing = result.body.data;
    assert(pure_runtime_forward(original, &left, "left", 0, &x) == PURE_BUDGET);
    assert(!x.runtime && source.work[0].pins == 0);
    assert(pure_runtime_restart(&left, &identity, port(&b), 8));
    assert(pure_runtime_forward(original, &left, "left", 0, &x) == PURE_OK);
    assert(pure_runtime_forward(original, &right, "right", 0, &y) == PURE_OK);
    assert(pure_runtime_release(original));
    assert(!pure_runtime_result(original, &result));
    pure_runtime_stop(&source);
    assert(!pure_runtime_restart(&source, &identity, port(&a), 8));
    assert(a.live == 1 && !b.allocations && !c.allocations);
    assert(pure_runtime_cancel(x) && pure_runtime_release(x));
    assert(a.live == 1 && !b.writes);
    result = completed(&right, y);
    assert(result.body.data == backing && result.body.size == 5 && !memcmp(result.body.data, "value", 5));
    assert(c.writes == 1 && !c.allocations && source.retained == 5);
    assert(!pure_runtime_cancel(y)); /* effects cannot be rolled back */
    assert(pure_runtime_release(y));
    assert(a.frees == 1 && !source.active && !source.retained && !right.active && !right.retained);
    assert(pure_runtime_restart(&source, &identity, port(&a), 8));

    /* A completed consumer does not retain its input when the program has no
     * input-backed reply. The producing domain can then be reclaimed early. */
    PureProgram no_reply = identity;
    no_reply.reply_input = false;
    assert(pure_runtime_restart(&right, &no_reply, port(&c), 8));
    original = submit(&source, "again", "last", 4);
    assert(completed(&source, original).status == PURE_OK);
    assert(pure_runtime_forward(original, &right, "no_reply", 0, &y) == PURE_OK);
    assert(pure_runtime_release(original));
    assert(completed(&right, y).body.size == 0 && a.live == 0);
    assert(source.active == 0 && right.active == 1 && right.retained == 0);
    assert(pure_runtime_release(y));
}

static void idle_binding(void) {
    Host a = {0}, b = {0};
    PureRuntime source, destination;
    PureWork source_work[1], destination_work[1];
    PureTicket first = {0};
    assert(pure_runtime_init(&source, NULL, 0, &identity, port(&a), 8));
    assert(pure_runtime_init(&destination, destination_work, 1, &identity, port(&b), 8));
    assert(pure_runtime_accept(&source, PURE_INPUT_WRITE, "idle", 0, 1, &first) == PURE_BUSY);
    assert(!first.runtime && !pure_runtime_bind(&source, NULL, 1));
    assert(pure_runtime_bind(&source, source_work, 1));
    first = submit(&source, "source", "abc", 3);
    assert(!pure_runtime_bind(&source, NULL, 0));
    assert(completed(&source, first).status == PURE_OK);
    PureTicket receiver;
    assert(pure_runtime_forward(first, &destination, "receiver", 0, &receiver) == PURE_OK);
    assert(pure_runtime_release(first));
    assert(!pure_runtime_bind(&source, NULL, 0)); /* The receiver still pins its backing. */
    assert(completed(&destination, receiver).status == PURE_OK);
    assert(pure_runtime_release(receiver));
    assert(!source.active && !source.retained && !a.live);
    assert(pure_runtime_bind(&source, NULL, 0));
    assert(!pure_runtime_result(first, &(PureOutcome){0}));
    assert(pure_runtime_bind(&source, source_work, 1));
    PureTicket second = submit(&source, "again", "d", 1);
    assert(second.generation > first.generation && !pure_runtime_ready(first));
    assert(completed(&source, second).status == PURE_OK);
    assert(pure_runtime_release(second));
    assert(pure_runtime_bind(&source, NULL, 0));
    assert(source.accepted == 2 && source.releases == 2 && source.serial == second.generation);
}

static void long_chain(void) {
    enum { COUNT = 1024 };
    PureWork *work = calloc(COUNT, sizeof(*work));
    assert(work);
    Host host = {0};
    PureRuntime r;
    assert(pure_runtime_init(&r, work, COUNT, &identity, port(&host), COUNT));
    PureTicket ticket = submit(&r, "chain", "x", 1);
    for (size_t i = 1; i < COUNT; ++i) {
        assert(completed(&r, ticket).status == PURE_OK);
        PureTicket next;
        assert(pure_runtime_forward(ticket, &r, "chain", 0, &next) == PURE_OK);
        assert(pure_runtime_release(ticket));
        ticket = next;
    }
    assert(completed(&r, ticket).status == PURE_OK);
    assert(host.allocations == 1 && r.active == COUNT);
    assert(pure_runtime_release(ticket));
    assert(r.retained == 0 && r.active == 0 && host.frees == 1);
    free(work);
}

static void actual_storage(void) {
    char directory[] = "/tmp/pure-runtime-check-XXXXXX";
    assert(mkdtemp(directory));
    char source_path[128], target_path[128];
    snprintf(source_path, sizeof(source_path), "%s/source", directory);
    snprintf(target_path, sizeof(target_path), "%s/target", directory);
    assert(pure_store_init(source_path) == PURE_OK && pure_store_init(target_path) == PURE_OK);
    PureProgram selection;
    char error[160];
    assert(pure_program_load("examples/selection.pure", &selection, error, sizeof(error)));
    PureRuntime source, target;
    PureWork a[2], b[2];
    assert(pure_runtime_init(&source, a, 2, &selection, pure_store_host(source_path), 128));
    assert(pure_runtime_init(&target, b, 2, &identity, pure_store_host(target_path), 128));
    const unsigned char bytes[] = {1,0,0,0, 3,0,0,0, 'a','b','c','d','e'};
    PureTicket ticket = submit(&source, "excerpt", bytes, sizeof(bytes)), consumer;
    PureOutcome result = completed(&source, ticket);
    assert(result.status == PURE_OK && result.body.size == 3 && !memcmp(result.body.data, "bcd", 3));
    assert(pure_runtime_forward(ticket, &target, "archive", 0, &consumer) == PURE_OK);
    assert(pure_runtime_release(ticket));
    assert(source.active == 1 && source.retained == sizeof(bytes));
    assert(completed(&target, consumer).status == PURE_OK);
    assert(pure_runtime_release(consumer) && source.active == 0);
    PureStoredView stored = {0};
    assert(pure_store_get(target_path, "archive", 128, &stored) == PURE_OK);
    assert(stored.body.size == 3 && !memcmp(stored.body.data, "bcd", 3));
    pure_store_release(&stored);

    assert(pure_runtime_accept(&source, PURE_INPUT_READ, "excerpt", 0, 0, &ticket) == PURE_OK);
    assert(completed(&source, ticket).status == PURE_OK);
    assert(pure_runtime_forward(ticket, &target, "snapshot", 0, &consumer) == PURE_OK);
    assert(pure_runtime_release(ticket));
    int64_t revision;
    assert(pure_store_put(source_path, "excerpt", 1, (PureSpan){(const unsigned char *)"new", 3}, &revision) == PURE_OK);
    result = completed(&target, consumer);
    assert(result.body.size == 3 && !memcmp(result.body.data, "bcd", 3));
    assert(pure_runtime_release(consumer));

    unsigned char invalid[] = {255,255,255,255, 1,0,0,0, 'x'};
    ticket = submit(&source, "invalid", invalid, sizeof(invalid));
    assert(completed(&source, ticket).status == PURE_LIMIT);
    assert(pure_runtime_release(ticket));
    assert(pure_store_get(source_path, "invalid", 128, &stored) == PURE_NOT_FOUND);
    assert(source.active == 0 && target.active == 0 && source.retained == 0 && target.retained == 0);
    remove_store(source_path);
    remove_store(target_path);
    assert(rmdir(directory) == 0);
}

int main(void) {
    admission_and_lifecycle();
    forwarding();
    idle_binding();
    long_chain();
    actual_storage();
    printf("{\"runtime_tests\":\"passed\",\"domain_bytes\":%zu,\"work_slot_bytes\":%zu,"
        "\"ticket_bytes\":%zu,\"prepared_program_bytes\":%zu,\"forwarding_chain\":1024,"
        "\"native_storage_pipeline\":true,\"idle_slot_binding\":true,"
        "\"release_reentrancy_guard\":true}\n", sizeof(PureRuntime), sizeof(PureWork),
        sizeof(PureTicket), sizeof(PureProgram));
    return 0;
}
