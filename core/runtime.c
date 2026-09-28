#include "runtime.h"
#include <string.h>

#define NONE SIZE_MAX
enum { FREE, RECEIVING, READY, RUNNING, DONE };
#ifdef PURE_RUNTIME_METRICS
#define METRIC(...) do { __VA_ARGS__; } while (0)
#else
#define METRIC(...) ((void)0)
#endif

static PureWork *lookup(PureTicket ticket, bool require_held) {
    PureRuntime *r = ticket.runtime;
    if (!r || ticket.slot >= r->capacity || !ticket.generation) return NULL;
    PureWork *w = &r->work[ticket.slot];
    if (w->state == FREE || w->generation != ticket.generation ||
        (require_held && !w->held)) return NULL;
    return w;
}

static bool valid_key(const char *key) {
    if (!key || !*key) return false;
    size_t n = 0;
    for (; n <= 64 && key[n]; ++n) {
        unsigned char c = (unsigned char)key[n];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_')) return false;
    }
    return n <= 64;
}

/* WRITE results are loans from the admitted input, not a second allocation.
 * Integer offsets avoid relational comparisons between unrelated C pointers. */
static bool borrowed_reply(PureSpan input, PureSpan reply) {
    if (!reply.size) return true;
    if (!input.data || !reply.data) return false;
    uintptr_t base = (uintptr_t)input.data, start = (uintptr_t)reply.data;
    return start >= base && start - base <= input.size &&
           reply.size <= input.size - (start - base);
}

static void dispose(PureRuntime *r, PureResource *resource) {
    PureResource old = *resource;
    *resource = (PureResource){0};
    if (old.release) {
        bool running = r->running;
        r->running = true;
        old.release(old.allocation, old.allocation_size);
        r->running = running;
    }
}

static void payload_release(PureRuntime *r, PureWork *w);

/* Iteratively retire a chain of forwarded values. An arbitrarily long chain
 * must not turn completion into recursive stack growth. */
static void retire(PureRuntime *r, PureWork *w) {
    while (w && !w->held && !w->pins) {
        PureTicket parent = w->parent;
#ifdef PURE_RUNTIME_METRICS
        bool released_payload = w->charged || w->resource.release || parent.runtime;
#endif
        dispose(r, &w->resource);
        r->retained -= w->charged;
        METRIC(if (released_payload) ++r->releases);
        size_t slot = (size_t)(w - r->work);
        *w = (PureWork){.next = r->free_head};
        r->free_head = slot;
        --r->active;
        if (!parent.runtime) break;
        r = parent.runtime;
        w = lookup(parent, false);
        /* Pins can only be created and removed by the owner-thread core. */
        if (!w || !w->pins) break;
        --w->pins;
    }
}

static void payload_release(PureRuntime *r, PureWork *w) {
    PureTicket parent = w->parent;
#ifdef PURE_RUNTIME_METRICS
    bool released_payload = w->charged || w->resource.release || parent.runtime;
#endif
    dispose(r, &w->resource);
    r->retained -= w->charged;
    METRIC(if (released_payload) ++r->releases);
    w->charged = 0;
    w->parent = (PureTicket){0};
    if (parent.runtime) {
        PureWork *source = lookup(parent, false);
        if (source && source->pins) { --source->pins; retire(parent.runtime, source); }
    }
}

bool pure_runtime_init(PureRuntime *r, PureWork *work, size_t capacity,
                       const void *definition, PureHost host, size_t budget) {
    if (!r || ((work == NULL) != (capacity == 0))) return false;
    *r = (PureRuntime){.definition = definition, .host = host, .free_head = NONE,
        .ready_head = NONE, .ready_tail = NONE,
        .budget = budget, .accepting = true};
    return pure_runtime_bind(r, work, capacity);
}

bool pure_runtime_bind(PureRuntime *r, PureWork *work, size_t capacity) {
    if (!r || r->running || r->active || r->retained ||
        r->ready_head != NONE || r->ready_tail != NONE ||
        ((work == NULL) != (capacity == 0)))
        return false;
    r->work = work;
    r->capacity = capacity;
    r->free_head = capacity ? 0 : NONE;
    for (size_t i = 0; i < capacity; ++i)
        work[i] = (PureWork){.next = i + 1 < capacity ? i + 1 : NONE};
    return true;
}

bool pure_runtime_restart(PureRuntime *r, const void *definition, PureHost host, size_t budget) {
    if (!r || r->running || r->active) return false;
    r->definition = definition;
    r->host = host;
    r->budget = budget;
    r->accepting = true;
    return true;
}

void pure_runtime_stop(PureRuntime *r) { if (r && !r->running) r->accepting = false; }

static PureResult admission(PureRuntime *r, PureInput input, const char *key,
                             int64_t expected, size_t bytes) {
    if (!r || r->running || !valid_key(key) ||
        (input != PURE_INPUT_READ && input != PURE_INPUT_WRITE) ||
        (input == PURE_INPUT_READ ? bytes != 0 : expected < 0)) return PURE_INVALID;
    if (!r->accepting) return PURE_CLOSED;
    bool missing_effect = input == PURE_INPUT_READ ? !r->host.read : !r->host.write;
#ifdef PURE_ASYNC_EFFECTS
    if (missing_effect && !r->async_effects) return PURE_DENIED;
#else
    if (missing_effect) return PURE_DENIED;
#endif
    if (r->free_head == NONE) return PURE_BUSY;
    if (r->serial == UINT64_MAX) return PURE_LIMIT;
    if (bytes > r->budget - r->retained) { METRIC(++r->budget_rejections); return PURE_BUDGET; }
    return PURE_OK;
}

static PureTicket occupy(PureRuntime *r, PureInput input, const char *key,
                          int64_t expected, size_t bytes, PureResource resource) {
    size_t slot = r->free_head;
    PureWork *w = &r->work[slot];
    r->free_head = w->next;
    *w = (PureWork){.generation = ++r->serial, .next = NONE, .charged = bytes,
        .state = RECEIVING, .held = true, .input = input, .expected = expected,
        .resource = resource};
    memcpy(w->key, key, strlen(key) + 1);
    ++r->active;
    METRIC(++r->accepted);
    r->retained += bytes;
    METRIC(if (r->active > r->peak_active) r->peak_active = r->active);
    METRIC(if (r->retained > r->peak_retained) r->peak_retained = r->retained);
    return (PureTicket){r, slot, w->generation};
}

bool pure_runtime_ready(PureTicket ticket) {
    PureWork *w = lookup(ticket, true);
    if (!w || ticket.runtime->running || w->state != RECEIVING) return false;
    PureRuntime *r = ticket.runtime;
    w->state = READY;
    if (r->ready_tail == NONE) r->ready_head = ticket.slot;
    else r->work[r->ready_tail].next = ticket.slot;
    r->ready_tail = ticket.slot;
    return true;
}

PureResult pure_runtime_accept(PureRuntime *r, PureInput input, const char *key,
                               int64_t expected, size_t size, PureTicket *out) {
    if (!out) return PURE_INVALID;
    PureResult status = admission(r, input, key, expected, size);
    if (status != PURE_OK) return status;
    PureResource resource = {0};
    if (size) {
        if (!r->host.allocate) return PURE_ALLOCATION;
        r->running = true;
        bool ok = r->host.allocate(r->host.context, size, &resource);
        r->running = false;
        if (!ok || !resource.body.data || resource.body.size != size || !resource.release) {
            dispose(r, &resource);
            return PURE_ALLOCATION;
        }
    }
    *out = occupy(r, input, key, expected, size, resource);
    if (input == PURE_INPUT_READ) pure_runtime_ready(*out);
    return PURE_OK;
}

unsigned char *pure_runtime_ingress(PureTicket ticket, size_t *size) {
    PureWork *w = lookup(ticket, true);
    if (size) *size = 0;
    if (!w || ticket.runtime->running || w->state != RECEIVING ||
        w->input != PURE_INPUT_WRITE || w->parent.runtime) return NULL;
    if (size) *size = w->resource.body.size;
    return (unsigned char *)w->resource.body.data;
}

PureResult pure_runtime_grow(PureTicket ticket, size_t size) {
    PureWork *w = lookup(ticket, true);
    PureRuntime *r = ticket.runtime;
    if (!w || r->running || w->state != RECEIVING || w->input != PURE_INPUT_WRITE || w->parent.runtime)
        return PURE_INVALID;
    size_t had = w->resource.body.size;
    if (size <= had) return PURE_OK;
    if (size - had > r->budget - r->retained) {
        METRIC(++r->budget_rejections);
        return PURE_BUDGET;
    }
    if (!r->host.allocate) return PURE_ALLOCATION;
    PureResource grown = {0};
    r->running = true;
    bool ok = r->host.allocate(r->host.context, size, &grown);
    r->running = false;
    if (!ok || !grown.body.data || grown.body.size != size || !grown.release) {
        dispose(r, &grown);
        return PURE_ALLOCATION;
    }
    if (had) memcpy((unsigned char *)grown.body.data, w->resource.body.data, had);
    dispose(r, &w->resource);
    w->resource = grown;
    w->charged += size - had;
    r->retained += size - had;
    METRIC(if (r->retained > r->peak_retained) r->peak_retained = r->retained);
    return PURE_OK;
}

bool pure_runtime_step(PureRuntime *r) {
    if (!r || r->running || r->ready_head == NONE) return false;
#ifdef PURE_ASYNC_EFFECTS
    PureWork *head = &r->work[r->ready_head];
    if (head->input == PURE_INPUT_READ ? !r->host.read : !r->host.write) return false;
#endif
    size_t slot = r->ready_head;
    PureWork *w = &r->work[slot];
    r->ready_head = w->next;
    if (r->ready_head == NONE) r->ready_tail = NONE;
    w->next = NONE;
    w->state = RUNNING;
    r->running = true;
    PureOutcome outcome = {0};
    if (w->input == PURE_INPUT_WRITE) {
        PureSpan reply = {0};
        outcome.status = r->host.write(r->host.context, r->definition, w->key,
                                       w->expected, w->resource.body, &reply,
                                       &outcome.revision);
        if (outcome.status == PURE_OK) {
            if (borrowed_reply(w->resource.body, reply)) outcome.body = reply;
            else outcome.status = PURE_STORE_ERROR;
        }
        METRIC(if (outcome.status == PURE_OK) ++r->commits;
               else if (outcome.status == PURE_CONFLICT) ++r->conflicts);
    } else {
        outcome.status = r->host.read(r->host.context, r->definition, w->key,
            r->budget - r->retained, &w->resource, &outcome.revision);
        if (outcome.status == PURE_OK) {
            if (w->resource.body.size > r->budget - r->retained) outcome.status = PURE_BUDGET;
            else if (w->resource.body.size && !w->resource.release)
                outcome.status = PURE_STORE_ERROR;
            else {
                w->charged = w->resource.body.size;
                r->retained += w->charged;
                METRIC(if (r->retained > r->peak_retained) r->peak_retained = r->retained);
                outcome.body = w->resource.body;
                METRIC(++r->gets);
            }
        }
        METRIC(if (outcome.status == PURE_BUDGET) ++r->budget_rejections);
    }
    if (outcome.status != PURE_OK || !outcome.body.size) {
        outcome.body = (PureSpan){0};
        payload_release(r, w);
    }
    METRIC(r->borrowed_output_bytes += outcome.body.size);
    w->outcome = outcome;
    w->state = DONE;
    r->running = false;
    return true;
}

#ifdef PURE_ASYNC_EFFECTS
bool pure_runtime_enable_async(PureRuntime *r) {
    if (!r || r->running || r->active) return false;
    r->async_effects = true;
    return true;
}

PureTake pure_runtime_take(PureRuntime *r, PureEffect *out) {
    if (!r || !out || !r->async_effects || r->running || r->ready_head == NONE)
        return PURE_TAKE_EMPTY;
    size_t slot = r->ready_head;
    PureWork *w = &r->work[slot];
    r->ready_head = w->next;
    if (r->ready_head == NONE) r->ready_tail = NONE;
    w->next = NONE;
    w->state = RUNNING;
    *out = (PureEffect){.ticket = {r, slot, w->generation}, .input = w->input,
                        .key = w->key, .expected = w->expected,
                        .value = w->resource.body,
                        .available = r->budget - r->retained};
    return PURE_TAKE_EFFECT;
}

bool pure_runtime_complete(PureTicket ticket, PureResult status,
                           PureResource *read_resource, PureSpan write_reply,
                           int64_t revision) {
    PureWork *w = lookup(ticket, false);
    PureRuntime *r = ticket.runtime;
    if (!w || r->running || w->state != RUNNING ||
        (w->input == PURE_INPUT_WRITE && read_resource) ||
        (w->input == PURE_INPUT_READ && write_reply.size) ||
        (w->input == PURE_INPUT_WRITE && status == PURE_OK &&
         !borrowed_reply(w->resource.body, write_reply)) ||
        (read_resource && !read_resource->release &&
         (read_resource->body.size || read_resource->allocation))) return false;
    r->running = true;
    PureOutcome outcome = {.status = status, .revision = revision};
    if (w->input == PURE_INPUT_READ) {
        PureResource acquired = {0};
        if (read_resource) {
            acquired = *read_resource;
            *read_resource = (PureResource){0};
        }
        if (outcome.status == PURE_OK) {
            if (acquired.body.size > r->budget - r->retained)
                outcome.status = PURE_BUDGET;
            else if (acquired.body.size && !acquired.release)
                outcome.status = PURE_STORE_ERROR;
            else {
                w->resource = acquired;
                acquired = (PureResource){0};
                w->charged = w->resource.body.size;
                r->retained += w->charged;
                METRIC(if (r->retained > r->peak_retained) r->peak_retained = r->retained);
                outcome.body = w->resource.body;
                METRIC(++r->gets);
            }
        }
        dispose(r, &acquired);
        METRIC(if (outcome.status == PURE_BUDGET) ++r->budget_rejections);
    } else {
        if (status == PURE_OK) outcome.body = write_reply;
        METRIC(if (outcome.status == PURE_OK) ++r->commits;
               else if (outcome.status == PURE_CONFLICT) ++r->conflicts);
    }
    if (outcome.status != PURE_OK || !outcome.body.size) {
        outcome.body = (PureSpan){0};
        payload_release(r, w);
    }
    METRIC(r->borrowed_output_bytes += outcome.body.size);
    w->outcome = outcome;
    w->state = DONE;
    r->running = false;
    if (!w->held) retire(r, w);
    return true;
}
#endif

bool pure_runtime_result(PureTicket ticket, PureOutcome *out) {
    PureWork *w = lookup(ticket, true);
    if (!out || !w || ticket.runtime->running || w->state != DONE) return false;
    *out = w->outcome;
    return true;
}

PureResult pure_runtime_forward(PureTicket source, PureRuntime *r,
                                const char *key, int64_t expected, PureTicket *out) {
    PureWork *parent = lookup(source, true);
    if (!out || !parent || source.runtime->running || parent->state != DONE ||
        parent->outcome.status != PURE_OK || (parent->outcome.body.size && !parent->outcome.body.data))
        return PURE_INVALID;
    PureResult status = admission(r, PURE_INPUT_WRITE, key, expected, parent->outcome.body.size);
    if (status != PURE_OK) return status;
    if (parent->pins == SIZE_MAX) return PURE_LIMIT;
    PureTicket ticket = occupy(r, PURE_INPUT_WRITE, key, expected, parent->outcome.body.size,
                              (PureResource){.body = parent->outcome.body});
    ++parent->pins;
    r->work[ticket.slot].parent = source;
    pure_runtime_ready(ticket);
    *out = ticket;
    return PURE_OK;
}

static void unlink_ready(PureRuntime *r, size_t slot) {
    size_t previous = NONE;
    for (size_t at = r->ready_head; at != NONE; at = r->work[at].next) {
        if (at == slot) {
            if (previous == NONE) r->ready_head = r->work[at].next;
            else r->work[previous].next = r->work[at].next;
            if (r->ready_tail == at) r->ready_tail = previous;
            r->work[at].next = NONE;
            return;
        }
        previous = at;
    }
}

bool pure_runtime_cancel(PureTicket ticket) {
    PureWork *w = lookup(ticket, true);
    if (!w || ticket.runtime->running || (w->state != RECEIVING && w->state != READY)) return false;
    if (w->state == READY) unlink_ready(ticket.runtime, ticket.slot);
    payload_release(ticket.runtime, w);
    w->outcome = (PureOutcome){.status = PURE_CANCELLED};
    w->state = DONE;
    return true;
}

bool pure_runtime_release(PureTicket ticket) {
    PureWork *w = lookup(ticket, true);
    if (!w || ticket.runtime->running) return false;
    if (w->state == READY) unlink_ready(ticket.runtime, ticket.slot);
    w->held = false;
#ifdef PURE_ASYNC_EFFECTS
    if (w->state == RUNNING) return true;
#endif
    retire(ticket.runtime, w);
    return true;
}
