#ifndef PURE_RUNTIME_H
#define PURE_RUNTIME_H
#include "program.h"
#include "result.h"
#include <stdint.h>

/* First execution profile: authored selection -> read/conditional write ->
 * reply. No socket, file descriptor, allocator, parser or OS type in the core.
 * All control calls and connected domains share one owner thread. Host effects
 * are synchronous in this profile and must not reenter the runtime. */
typedef struct PureRuntime PureRuntime;
typedef struct PureWork PureWork;
typedef struct { PureRuntime *runtime; size_t slot; uint64_t generation; } PureTicket;
typedef enum { PURE_INPUT_READ, PURE_INPUT_WRITE } PureInput;

/* The host owns this resource until it successfully returns it to the core.
 * release must end all access and free/unmap allocation exactly once. */
typedef struct {
    PureSpan body;
    void *allocation;
    size_t allocation_size;
    void (*release)(void *allocation, size_t size);
} PureResource;
typedef struct {
    void *context;
    bool (*allocate)(void *, size_t, PureResource *);
    PureResult (*read)(void *, const char *, PureRegion, size_t minimum,
                       size_t available, PureResource *, int64_t *revision);
    PureResult (*write)(void *, const char *, int64_t expected,
                        PureSpan, int64_t *revision);
    PureResult (*splice)(void *, const char *, int64_t expected, size_t offset,
                         size_t remove, PureSpan insert, int64_t *revision);
} PureHost;
typedef struct { PureResult status; int64_t revision; PureSpan body; } PureOutcome;

/* Caller-provided work slots: no background thread or per-domain heap. These
 * records are implementation storage, not an ABI or a wire representation. */
struct PureWork {
    uint64_t generation;
    size_t next, charged, pins;
    unsigned state;
    bool held;
    PureInput input;
    int64_t expected;
    char key[65];
    PureResource resource;
    PureTicket parent;
    PureOutcome outcome;
};
struct PureRuntime {
    const PureProgram *program;
    PureHost host;
    PureWork *work;
    size_t capacity, free_head, ready_head, ready_tail;
    size_t budget, retained, peak_retained, active, peak_active;
    uint64_t serial, accepted, commits, gets, conflicts, budget_rejections;
    uint64_t releases, borrowed_output_bytes;
    bool accepting, running;
};

/* Initialize fresh storage once. Use restart, not init, to reuse a domain;
 * the monotonic ticket sequence then survives restart. program is immutable
 * and remains alive until all work has drained. */
bool pure_runtime_init(PureRuntime *, PureWork *, size_t capacity,
                       const PureProgram *, PureHost, size_t budget);
bool pure_runtime_restart(PureRuntime *, const PureProgram *, PureHost, size_t budget);
void pure_runtime_stop(PureRuntime *); /* stop admission, allow queued work to drain */

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
#endif
