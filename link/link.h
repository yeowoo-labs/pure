#ifndef PURE_LINK_H
#define PURE_LINK_H
#include "link-protocol.h"

/* Links over UDP: the two sides of link/link-protocol.h (what goes on the
 * wire, and what each side keeps) with sockets and threads. A sender sends
 * on a thread of its own, packets in bursts with UDP GSO; a listener takes
 * a receiver's packets on a port, with UDP GRO. */

/* The sending side: one output's saved sends to url, pure://host:port/INPUT,
 * as source (a Pure receiver's name for it), their messages of shape (the
 * receiver's input must have the same: core/module.h's pure_module_shape),
 * from own (this node's key, whose public half is own_public) to the
 * receiver's public key, given by pure_link_peer before pure_link_start.
 * The owner queues and trims as it does a wire's (serve/deliver.h): a send
 * counts as delivered once the receiver says it saved it. */
typedef struct PureLink PureLink;
PureLink *pure_link_open(const char *url, const char *source, uint32_t shape, const PureSealKey *own,
                         const unsigned char own_public[PURE_SEAL_KEY], char *error, size_t size);
void pure_link_peer(PureLink *, const unsigned char peer[PURE_SEAL_KEY]);
/* The fields its sends are laid out with (core/module.h's
 * pure_module_fields): a receiver whose input takes another shape may take
 * them by name (docs/design/014-fields-by-name.md). */
bool pure_link_fields(PureLink *, const unsigned char *fields, size_t size);
bool pure_link_queue(PureLink *, uint64_t seq, const unsigned char *bytes, size_t size);
uint64_t pure_link_queued(PureLink *);
/* The last number the receiver saved; any thread. */
uint64_t pure_link_delivered(PureLink *);
/* The bytes of the sends queued and not yet saved; any thread. */
size_t pure_link_held(PureLink *);
/* Packets go in bursts with UDP GSO where the system has it (at first);
 * off, each in a call of its own: for a path whose queues take a burst as
 * one packet (netem does). Before pure_link_start. */
void pure_link_gso(PureLink *, bool on);
/* Send on a thread of the link's own, calling notify when more was saved. */
bool pure_link_start(PureLink *, void (*notify)(void *), void *context, char *error, size_t size);
void pure_link_counts(PureLink *, PureLinkCounts *);
void pure_link_close(PureLink *);

/* The receiving side: a receiver's packets on a UDP port, taken on a thread
 * of the listener's own, where the host's input and deliver are called. The
 * listener takes the receiver: closing it frees the receiver, and gives its
 * counts. pure_link_done (any thread) says how a delivery's save went. */
typedef struct PureLinkListener PureLinkListener;
PureLinkListener *pure_link_listen(PureLinkReceiver *, unsigned port, char *error, size_t size);
bool pure_link_serve(PureLinkListener *, char *error, size_t size);
/* How a delivery went: link-protocol.h's PureLinkDone, with its token. */
void pure_link_done(PureLinkListener *, PureLinkDone);
void pure_link_listener_close(PureLinkListener *, PureLinkCounts *counts);
#endif
