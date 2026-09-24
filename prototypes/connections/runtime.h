#ifndef PURE_RUNTIME_H
#define PURE_RUNTIME_H
#include "flow.h"
#include "result.h"
#include <stdint.h>

/* The optional metrics profile changes PureRuntime's layout. Give its init
 * function a distinct link name so mixed-profile objects cannot link quietly. */
#ifdef PURE_RUNTIME_METRICS
#define pure_runtime_init pure_runtime_init_metrics
#endif

/* One owner thread controls admission, payload credits, completion and loans.
 * An application definition is opaque to this core. Hosts interpret it and
 * perform effects; no parser, storage, socket or OS type is linked here. */
typedef struct PureRuntime PureRuntime;
typedef struct PureWork PureWork;
typedef struct { PureRuntime *runtime; size_t slot; uint64_t generation; } PureTicket;
typedef enum { PURE_INPUT_READ, PURE_INPUT_WRITE } PureInput;

/* The host owns this resource until it successfully returns it to the core.
 * release must end all access and free/unmap allocation exactly once. It runs
 * while that domain is busy; calls back into the same domain are rejected. */
typedef struct {
    PureSpan body;
    void *allocation;
    size_t allocation_size;
    void (*release)(void *allocation, size_t size);
} PureResource;
typedef struct {
    void *context;
    bool (*allocate)(void *, size_t, PureResource *);
    PureResult (*read)(void *, const void *definition, const char *,
                       size_t available, PureResource *, int64_t *revision);
    /* A successful reply may borrow only from input. The runtime retains its
     * backing until all reply consumers finish. Effect errors publish no reply. */
    PureResult (*write)(void *, const void *definition, const char *,
                        int64_t expected, PureSpan input, PureSpan *reply,
                        int64_t *revision);
} PureHost;
typedef struct { PureResult status; int64_t revision; PureSpan body; } PureOutcome;

/* Caller-provided work slots: no background thread or per-domain heap. These
 * records are implementation storage, not an ABI or a wire representation. */
/* Internal storage order packs the fixed key around small flags. This is not
 * a public ABI; the layout is checked as part of the core cost gate. */
struct PureWork {
    PureTicket parent;
    unsigned state;
    PureInput input;
    bool held;
    char key[65];
    int64_t expected;
    PureOutcome outcome;
    PureResource resource;
    uint64_t generation;
    size_t next, charged, pins;
};
struct PureRuntime {
    const void *definition;
    PureHost host;
    PureWork *work;
    size_t capacity, free_head, ready_head, ready_tail;
    size_t budget, retained;
#ifdef PURE_RUNTIME_METRICS
    size_t peak_retained;
#endif
    size_t active;
#ifdef PURE_RUNTIME_METRICS
    size_t peak_active;
#endif
    uint64_t serial;
#ifdef PURE_RUNTIME_METRICS
    uint64_t accepted, commits, gets, conflicts, budget_rejections;
    uint64_t releases, borrowed_output_bytes;
#endif
    bool accepting, running, async_effects;
};

/* Enforce repeated storage cost on 64-bit C hosts, including macOS where the
 * Linux object-section checker is unavailable. */
#if SIZE_MAX == UINT64_MAX && UINTPTR_MAX == UINT64_MAX
_Static_assert(sizeof(PureWork) == 216, "work-slot layout exceeded its reviewed size");
#ifdef PURE_RUNTIME_METRICS
_Static_assert(sizeof(PureRuntime) == 192, "metrics domain layout changed");
#else
_Static_assert(sizeof(PureRuntime) == 120, "domain layout changed");
#endif
#endif

/* Initialize fresh storage once. A NULL work pointer with capacity zero starts
 * an idle domain; bind work before admission. Use restart, not init, to reuse
 * a domain so its monotonic ticket sequence survives restart. definition is
 * host-owned and remains alive until all work has drained. */
bool pure_runtime_init(PureRuntime *, PureWork *, size_t capacity,
                       const void *definition, PureHost, size_t budget);
bool pure_runtime_restart(PureRuntime *, const void *definition, PureHost, size_t budget);
void pure_runtime_stop(PureRuntime *); /* stop admission, allow queued work to drain */
/* Replace caller-owned work storage only after all tickets and loans retire.
 * A zero-capacity binding releases idle slot storage without resetting budget,
 * ticket generation or optional metrics. The caller retains the bound array's life. */
bool pure_runtime_bind(PureRuntime *, PureWork *, size_t capacity);

/* READ queues immediately. WRITE reserves its slot, budget and host allocation
 * before returning. Fill ingress then publish once with ready. A partial input
 * released without ready never executes. On error, out is unchanged. */
PureResult pure_runtime_accept(PureRuntime *, PureInput, const char *key,
                               int64_t expected, size_t input_size, PureTicket *out);
unsigned char *pure_runtime_ingress(PureTicket, size_t *size);
bool pure_runtime_ready(PureTicket);
bool pure_runtime_step(PureRuntime *); /* execute at most one ready job */
bool pure_runtime_result(PureTicket, PureOutcome *out);

/* Pin a completed reply and queue it as another domain's WRITE input, with no
 * allocation, serialization or payload copy. Source may be released as soon
 * as this succeeds; the core retains its backing until the consumer ends.
 * Each domain charges its own retained input bytes as admission credits.
 * Credits across domains are NOT distinct physical memory and must not be summed. */
PureResult pure_runtime_forward(PureTicket source, PureRuntime *destination,
                                const char *key, int64_t expected, PureTicket *out);
bool pure_runtime_cancel(PureTicket); /* only before execution; keeps a completion */
bool pure_runtime_release(PureTicket); /* invalidates caller's ticket, preserves pins */

#ifdef PURE_ASYNC_EFFECTS
/* Optional owner-thread effect suspension. Enable before accepting work. The
 * host may complete effects in any order, but must call complete on the owner
 * thread after workers stop accessing its borrowed spans. An in-flight effect
 * keeps its input alive even if the caller releases the ticket. Effects
 * borrowed by take stay valid until completion. */
typedef enum { PURE_TAKE_EMPTY, PURE_TAKE_EFFECT } PureTake;
typedef struct {
    PureTicket ticket;
    PureInput input;
    const char *key;
    int64_t expected;
    PureSpan value; /* raw ingress for WRITE; empty for READ */
    size_t available; /* remaining payload credits for READ */
} PureEffect;
bool pure_runtime_enable_async(PureRuntime *);
PureTake pure_runtime_take(PureRuntime *, PureEffect *);
/* A valid completion consumes read_resource (even on failure); a rejected call
 * leaves it untouched. write_reply borrows from the retained ingress. */
bool pure_runtime_complete(PureTicket, PureResult status,
                           PureResource *read_resource, PureSpan write_reply,
                           int64_t revision);
#endif
#endif
