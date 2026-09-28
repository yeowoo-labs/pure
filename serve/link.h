#ifndef PURE_LINK_H
#define PURE_LINK_H
#include "module.h"
#include "seal.h"

/* Links without connections (docs/design/011-pure-transport.md): one node's
 * saved sends to another node's input over UDP, sealed with a key the two
 * nodes' keys share, with no handshake: the first packet carries records.
 * A link's lasting state is the apps' own: the sender's outbox (what to send
 * and send again) and the receiver's heard numbers (what it applied, saved
 * with the events). What either side keeps in memory is a cache, so either
 * may restart and the other goes on with the next packet.
 *
 * A packet is a header in the clear, then frames sealed with AES-128-GCM
 * (the header as associated data):
 *   u8 0x70 (version 0), u32le the key id of the node that sent it (FNV-1a
 *   of its public key; two keys may share an id, and a receiver tries the
 *   keys it allows with it), u64le that node's run (random, new each time it
 *   starts, so no nonce repeats under a key across restarts), u64le the
 *   packet's number in its run.
 * Keys come from the secret the two nodes' keys share (X25519) by
 * HKDF-SHA256, with both public keys and the sending node's run: a sender's
 * packets use its own run; a receiver's answers add the run they answer,
 * so an answer opens only in that run. The nonce is the key's base with the
 * packet's number in its last eight bytes (XOR, big-endian, as QUIC does).
 *
 * Frames (numbers little-endian):
 *   0x01 STREAM  source (u8 length, bytes), input (u8 length, bytes), u32 the
 *                shape of the messages (the sender's output and the
 *                receiver's input must have the same): the stream the
 *                records after it are of
 *   0x02 RECORD  u64 number, u32 size, u32 offset, u16 length, bytes: a saved
 *                send as stored, or a piece of one
 *   0x03 ACK     u64 largest number, u32 delay (µs), u8 ranges, each as u64
 *                first and u64 last, newest first
 *   0x04 STORED  source, input (as STREAM), u64 the last number heard
 *                (applied and saved), u8 flags (1: a record after a gap was
 *                not taken: send what follows it again), u32 credit (records
 *                it takes past that)
 *   0x05 PING    to be acknowledged
 * Losses are detected as RFC 9002 says; congestion control is its NewReno
 * (RFC 9002 §7). */

/* The sending side: one output's saved sends to url, pure://host:port/INPUT,
 * as source (a Pure receiver's name for it), from own (this node's key,
 * whose public half is own_public) to the receiver's public key, given by
 * pure_link_peer before pure_link_start. The owner queues and trims as it
 * does a wire's (serve/deliver.h): a send counts as delivered once the
 * receiver says it saved it. */
typedef struct PureLink PureLink;
PureLink *pure_link_open(const PureModule *m, const char *output, const char *url, const char *source,
                         const PureSealKey *own, const unsigned char own_public[PURE_SEAL_KEY], char *error,
                         size_t size);
void pure_link_peer(PureLink *, const unsigned char peer[PURE_SEAL_KEY]);
uint32_t pure_link_output(const PureLink *);
bool pure_link_queue(PureLink *, uint64_t seq, PureSpan payload);
uint64_t pure_link_queued(PureLink *);
/* The last number the receiver saved; any thread. */
uint64_t pure_link_delivered(PureLink *);
/* Send on a thread of the link's own, calling notify when more was saved. */
bool pure_link_start(PureLink *, void (*notify)(void *), void *context, char *error, size_t size);
void pure_link_close(PureLink *);

/* The receiving side: packets on a UDP port, from the senders allowed (a
 * source name and the key it seals with, a few bytes each however many
 * there are: see pure_link_idle). Whole records of a stream, in
 * order, go to deliver (on the listener's thread) as one delivery; the host
 * applies them from the one after the source's last heard number, stops at
 * a gap, saves, and then calls pure_link_done (any thread) with the last
 * number heard. Records wait here until then, and only the saved ones are
 * acknowledged as stored. */
typedef struct {
    uint32_t input;
    const char *source;
    uint64_t first;
    unsigned count;
    const unsigned char *records; /* count of: u32le size, bytes */
    size_t size;
    void *token; /* for pure_link_done */
} PureLinkDelivery;
typedef struct PureLinkListener PureLinkListener;
PureLinkListener *pure_link_listen(const PureModule *m, unsigned port, const PureSealKey *own,
                                   const unsigned char own_public[PURE_SEAL_KEY],
                                   bool (*deliver)(void *, const PureLinkDelivery *), void *context, char *error,
                                   size_t size);
bool pure_link_allow(PureLinkListener *, const char *source, const unsigned char key[PURE_SEAL_KEY], char *error,
                     size_t size);
/* A sender quiet for ms (30 s at first) with nothing of it waiting lets its
 * keys, addresses and streams go: an idle link costs the listener only its
 * allowing, and the sender's next packet makes them again (one X25519). */
void pure_link_idle(PureLinkListener *, uint64_t ms);
bool pure_link_serve(PureLinkListener *, char *error, size_t size);
/* stored: the delivery's save succeeded; gap: it stopped at a gap. */
void pure_link_done(PureLinkListener *, void *token, uint64_t heard, bool stored, bool gap);

/* Counts for a host's report: packets and records sent, records sent again,
 * packets lost and acknowledged, packets received, deliveries handed over,
 * the last number stored; a listener's senders woken (their active state
 * made), let go when idle, and active when it closed. A listener's are
 * given when it closes. */
typedef struct {
    uint64_t packets, records, resent, lost, acknowledged, received, deliveries, stored, woken, swept, active;
} PureLinkCounts;
void pure_link_counts(PureLink *, PureLinkCounts *);
void pure_link_listener_close(PureLinkListener *, PureLinkCounts *counts);
#endif
