#ifndef PURE_DELIVER_H
#define PURE_DELIVER_H
#include "module.h"

/* Delivering an app's saved sends over HTTP (docs/research/083-delivery-over-http.md).
 * A wire takes one output's sends in order: each goes as a JSON object of
 * its fields (groups as objects inside) by POST to a URL, with the headers
 * X-Pure-Source (the sender's name for the output), X-Pure-Sequence and
 * Idempotency-Key, one at a time, again and again until it is delivered:
 * a 2xx answer, or a Pure receiver's refusal (a 4xx with X-Pure-Error
 * naming a fault: it heard the message and refused it). 408, 429, 5xx and
 * no answer are tried again after a pause (0.1 s doubling to 10 s); another
 * 4xx is logged and tried again every 10 s. Nothing is ever dropped: what is
 * not delivered stays in the app's outbox, and a receiver applies a number
 * once, so a send delivered twice (its answer lost) is harmless. */
typedef struct PureWire PureWire;

/* A wire for the output named output of m to url (http[s]://host[:port]/path),
 * as source; NULL with why in error when it cannot be. An https receiver's
 * certificate must be trusted (the system's CAs, or the file given to
 * pure_wire_trust before the first https wire) and name its host. */
void pure_wire_trust(const char *ca_file);
PureWire *pure_wire_open(const PureModule *m, const char *output, const char *url, const char *source,
                         char *error, size_t size);
uint32_t pure_wire_output(const PureWire *);
/* Seal the wire's sends to the receiver's public key (serve/seal.h): each
 * request's body, bound to its source, first number and count, goes with
 * X-Pure-Seal naming the suite. Before pure_wire_start. */
void pure_wire_seal(PureWire *, const unsigned char peer[32]);
/* The owner's side: queue a saved send numbered after the last queued one
 * (it is encoded now); false when it cannot be (a bytes field that is not
 * UTF-8 has no JSON form), and the wire then stops at it. */
bool pure_wire_queue(PureWire *, uint64_t seq, PureSpan payload);
uint64_t pure_wire_queued(const PureWire *);
/* The last number delivered; any thread. */
uint64_t pure_wire_delivered(PureWire *);
/* Deliver on a thread of the wire's own, calling notify after each delivery. */
bool pure_wire_start(PureWire *, void (*notify)(void *), void *context);
/* Stop (a delivery under way is finished or abandoned) and free. */
void pure_wire_close(PureWire *);
/* Answers as events: the final answer to each send (a 2xx, or a 4xx but 408
 * and 429) becomes a message event of input (a message without a key), in
 * the order sent: its fields come from the answer's JSON object by name
 * (other members are skipped), a field the answer lacks from the send's
 * field of that name and type (as a request's id), and a u32 field named
 * http_status is the answer's status. An answer that makes no message is
 * logged and the send tried again every 10 s. Answers go one per request. */
bool pure_wire_reply(PureWire *, uint32_t input, char *error, size_t size);
/* The owner's side: the next answer in order, or false. Taking it counts
 * its send delivered (the owner applies the answer, then trims); what out
 * points to is the wire's until the next call. The answer is applied with
 * pure_app_deliver from the wire's source with the send's number, so an
 * answer that comes again after a restart is applied once. */
typedef struct {
    uint32_t input;
    uint64_t seq;
    PureSpan payload;
    const uint32_t *placed;
    const char *source;
} PureWireAnswer;
bool pure_wire_answer(PureWire *, PureWireAnswer *out);
/* Counts for a host's report. */
void pure_wire_counts(PureWire *, uint64_t *delivered, uint64_t *attempts);
#endif
