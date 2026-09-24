#ifndef PURE_FLOW_H
#define PURE_FLOW_H
#include <stdbool.h>
#include <stddef.h>

typedef struct { const unsigned char *data; size_t size; } PureSpan;
typedef void (*PureRelease)(void *owner);

/* A borrowed body stays alive until every byte is consumed or the transfer is
 * aborted. A successful send() consumes bytes; peer receipt is a separate fact.
 * Single owner thread; callbacks must not reenter or reuse this transfer. */
typedef struct {
    PureSpan spans[2];
    size_t part, offset;
    void *owner;
    PureRelease release;
    bool active;
} PureTransfer;

void pure_transfer_init(PureTransfer *, PureSpan header, PureSpan body,
                        void *owner, PureRelease);
PureSpan pure_transfer_next(const PureTransfer *, size_t limit);
bool pure_transfer_consume(PureTransfer *, size_t bytes);
void pure_transfer_abort(PureTransfer *);
#endif
