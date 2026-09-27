#include "flow.h"
#include <assert.h>

void pure_transfer_abort(PureTransfer *t) {
    if (!t->active) return;
    PureRelease release = t->release;
    void *owner = t->owner;
    *t = (PureTransfer){0};
    if (release) release(owner);
}

static void advance(PureTransfer *t) {
    while (t->active && t->part < 2 && t->offset == t->spans[t->part].size) {
        t->part++;
        t->offset = 0;
    }
    if (t->active && t->part == 2) pure_transfer_abort(t);
}

void pure_transfer_init(PureTransfer *t, PureSpan header, PureSpan body,
                        void *owner, PureRelease release) {
    assert(!t->active);
    assert((!header.size || header.data) && (!body.size || body.data));
    *t = (PureTransfer){.spans = {header, body}, .owner = owner,
                       .release = release, .active = true};
    advance(t);
}

PureSpan pure_transfer_next(const PureTransfer *t, size_t limit) {
    if (!t->active) return (PureSpan){0};
    PureSpan span = t->spans[t->part];
    size_t remaining = span.size - t->offset;
    return (PureSpan){span.data + t->offset, remaining < limit ? remaining : limit};
}

bool pure_transfer_consume(PureTransfer *t, size_t bytes) {
    if (!t->active || bytes > t->spans[t->part].size - t->offset) return false;
    t->offset += bytes;
    advance(t);
    return true;
}
