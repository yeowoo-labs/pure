#include "runtime.h"
#include <string.h>

#define NONE SIZE_MAX
enum { FREE, RECEIVING, READY, RUNNING, DONE };

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

static void dispose(PureResource *resource) {
    PureResource old = *resource;
    *resource = (PureResource){0};
    if (old.release) old.release(old.allocation, old.allocation_size);
}

static void payload_release(PureRuntime *r, PureWork *w);

/* Iteratively retire a chain of forwarded values. An arbitrarily long chain
 * must not turn completion into recursive stack growth. */
static void retire(PureRuntime *r, PureWork *w) {
    while (w && !w->held && !w->pins) {
        PureTicket parent = w->parent;
        bool owned = w->charged || w->resource.release || parent.runtime;
        dispose(&w->resource);
        r->retained -= w->charged;
        if (owned) ++r->releases;
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
    bool owned = w->charged || w->resource.release || parent.runtime;
    dispose(&w->resource);
    r->retained -= w->charged;
    w->charged = 0;
    w->parent = (PureTicket){0};
    if (owned) ++r->releases;
    if (parent.runtime) {
        PureWork *source = lookup(parent, false);
        if (source && source->pins) { --source->pins; retire(parent.runtime, source); }
    }
}

bool pure_runtime_init(PureRuntime *r, PureWork *work, size_t capacity,
                       const PureProgram *program, PureHost host, size_t budget) {
    if (!r || !work || !capacity || !program) return false;
    *r = (PureRuntime){.program = program, .host = host, .work = work,
        .capacity = capacity, .free_head = 0, .ready_head = NONE, .ready_tail = NONE,
        .budget = budget, .accepting = true};
    for (size_t i = 0; i < capacity; ++i)
        work[i] = (PureWork){.next = i + 1 < capacity ? i + 1 : NONE};
    return true;
}

bool pure_runtime_restart(PureRuntime *r, const PureProgram *p, PureHost host, size_t budget) {
    if (!r || !p || r->running || r->active) return false;
    r->program = p;
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
    if (input == PURE_INPUT_READ ? !r->host.read : !r->host.write) return PURE_DENIED;
    if (r->free_head == NONE) return PURE_BUSY;
    if (r->serial == UINT64_MAX) return PURE_LIMIT;
    if (bytes > r->budget - r->retained) { ++r->budget_rejections; return PURE_BUDGET; }
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
    ++r->accepted;
    r->retained += bytes;
    if (r->active > r->peak_active) r->peak_active = r->active;
    if (r->retained > r->peak_retained) r->peak_retained = r->retained;
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
            dispose(&resource);
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

bool pure_runtime_step(PureRuntime *r) {
    if (!r || r->running || r->ready_head == NONE) return false;
    size_t slot = r->ready_head;
    PureWork *w = &r->work[slot];
    r->ready_head = w->next;
    if (r->ready_head == NONE) r->ready_tail = NONE;
    w->next = NONE;
    w->state = RUNNING;
    r->running = true;
    PureOutcome outcome = {0};
    if (w->input == PURE_INPUT_WRITE) {
        PureSpan value;
        if (!pure_program_input(r->program, w->resource.body, &value, &outcome.body))
            outcome.status = PURE_LIMIT;
        else outcome.status = r->host.write(r->host.context, w->key, w->expected,
                                            value, &outcome.revision);
        if (outcome.status == PURE_OK) ++r->commits;
        else if (outcome.status == PURE_CONFLICT) ++r->conflicts;
    } else {
        outcome.status = r->host.read(r->host.context, w->key, r->program->read,
            r->program->read_required, r->budget - r->retained, &w->resource, &outcome.revision);
        if (outcome.status == PURE_OK) {
            if (w->resource.body.size > r->budget - r->retained) outcome.status = PURE_BUDGET;
            else if (w->resource.body.size && (!w->resource.body.data || !w->resource.release))
                outcome.status = PURE_STORE_ERROR;
            else {
                w->charged = w->resource.body.size;
                r->retained += w->charged;
                if (r->retained > r->peak_retained) r->peak_retained = r->retained;
                outcome.body = w->resource.body;
                ++r->gets;
            }
        }
        if (outcome.status == PURE_BUDGET) ++r->budget_rejections;
    }
    if (outcome.status != PURE_OK || !outcome.body.size) {
        outcome.body = (PureSpan){0};
        payload_release(r, w);
    }
    r->borrowed_output_bytes += outcome.body.size;
    w->outcome = outcome;
    w->state = DONE;
    r->running = false;
    return true;
}

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
        parent->outcome.status != PURE_OK) return PURE_INVALID;
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
    retire(ticket.runtime, w);
    return true;
}
