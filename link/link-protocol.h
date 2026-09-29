#ifndef PURE_LINK_PROTOCOL_H
#define PURE_LINK_PROTOCOL_H
#include "seal.h"

/* Links without connections (docs/design/011-pure-transport.md), as state
 * alone: no socket, thread or clock of their own. The owner gives each side
 * the packets that came and the time (ns, monotonic), and sends the packets
 * each side makes; link/link.h is such an owner over UDP. So one thread may
 * carry many links, and a test may carry both sides through a network of its
 * own making.
 *
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
 *   0x02 RECORD  u64 number, u32 size, u32 offset, u16 length, bytes: a piece
 *                of a saved send (pieces are 1000 bytes, the last shorter)
 *   0x03 ACK     u64 largest number, u32 delay (µs), u8 ranges, each as u64
 *                first and u64 last, newest first
 *   0x04 STORED  source, input (as STREAM), u64 the last number heard
 *                (applied and saved), u8 flags (1: a record after a gap was
 *                not taken: send what follows it again), u32 credit (records
 *                it takes past that)
 *   0x05 PING    to be acknowledged
 *   0x06 RECORDS u64 the first one's number, u8 count, then whole records one
 *                after another (numbers following on), each its size (a
 *                varint: LEB128) and its bytes: a send that fits the packet
 *                whole, with 1 to 3 bytes of framing, not the 19 of RECORD
 *   0x00 PADDING to the end of the packet: a full packet sent in a burst is
 *                a whole datagram
 * Losses are detected as RFC 9002 says; congestion control is its NewReno
 * (RFC 9002 §7). */

/* A packet is at most PURE_LINK_DATAGRAM bytes; a source's or an input's
 * name is 1 to PURE_LINK_NAME bytes; a receiver keeps where a packet came
 * from (the socket's address) in PURE_LINK_ADDRESS bytes. */
enum { PURE_LINK_DATAGRAM = 1200, PURE_LINK_NAME = 64, PURE_LINK_ADDRESS = 28 };

/* Counts for a host's report: packets and records sent, records sent again,
 * packets lost and acknowledged, packets received, deliveries handed over,
 * the last number stored; a receiver's senders woken (their active state
 * made), of them with a secret remembered (no X25519), let go when idle,
 * and active now. */
typedef struct {
    uint64_t packets, records, resent, lost, acknowledged, received, deliveries, stored, woken, remembered, swept, active;
} PureLinkCounts;

/* ---- The sending side: one output's saved sends to a receiver's input, as
 * source (the receiver's name for it), their messages of shape (the
 * receiver's input must have the same: core/module.h's pure_module_shape),
 * from own (this node's key, whose public half is own_public; NULL when
 * the secret comes from elsewhere). NULL when the names are not 1 to
 * PURE_LINK_NAME bytes, or memory runs out. */
typedef struct PureLinkSender PureLinkSender;
PureLinkSender *pure_link_sender(const char *source, const char *input, uint32_t shape, const PureSealKey *own,
                                 const unsigned char own_public[PURE_SEAL_KEY]);
/* The receiver's public key: the keys made from it (one X25519). A sender
 * makes no packet before it has one. */
bool pure_link_sender_peer(PureLinkSender *, const unsigned char peer[PURE_SEAL_KEY]);
/* The same with the secret own's key shares with peer's made elsewhere (by
 * a key kept in hardware, say, or a test's many keys): no X25519 here. */
bool pure_link_sender_shared(PureLinkSender *, const unsigned char peer[PURE_SEAL_KEY], const unsigned char secret[32]);
/* A saved send, numbered one past the last queued (any number when none
 * waits); false when it cannot be kept. */
bool pure_link_sender_queue(PureLinkSender *, uint64_t seq, const unsigned char *bytes, size_t size);
/* A packet that came (the receiver's answers; others are let be). */
void pure_link_sender_take(PureLinkSender *, const unsigned char *packet, size_t size, uint64_t now);
/* The timers due by now: losses found, probes, records sent again. */
void pure_link_sender_tick(PureLinkSender *, uint64_t now);
/* The next packet to send into out, and its number: its size, or 0 when
 * there is none (nothing to send, or the window is full). whole: a full
 * packet is padded to PURE_LINK_DATAGRAM (so packets may go in a burst of
 * whole datagrams, as UDP GSO sends them). */
size_t pure_link_sender_packet(PureLinkSender *, unsigned char out[PURE_LINK_DATAGRAM], bool whole, uint64_t now,
                               uint64_t *number);
/* A packet made but not sent (the socket refused it): what it carried is
 * sent again, and it counts as never sent. */
void pure_link_sender_refused(PureLinkSender *, uint64_t number);
/* When the next timer is due (ns), or 0 for none. */
uint64_t pure_link_sender_due(const PureLinkSender *);
/* The last number queued; the last the receiver stored. */
uint64_t pure_link_sender_queued(const PureLinkSender *);
uint64_t pure_link_sender_stored(const PureLinkSender *);
void pure_link_sender_counts(const PureLinkSender *, PureLinkCounts *);
void pure_link_sender_free(PureLinkSender *);

/* ---- The receiving side: packets from the senders allowed (a source name
 * and the key it seals with, a few bytes each however many there are: see
 * pure_link_idle). A stream (a source to an input) goes to the host's input
 * its name and shape find (its number, or -1: the stream is refused). Whole
 * records of a stream, in order, go to deliver as one delivery; the host
 * applies them from the one after the source's last heard number, stops at
 * a gap, saves, and then says so with a done (pure_link_receiver_turn).
 * Records wait here until then, and only the saved ones are acknowledged as
 * stored. */
typedef struct {
    uint32_t input;
    const char *source;
    uint64_t first;
    unsigned count;
    const unsigned char *records; /* count of: u32le size, bytes */
    size_t size;
    void *token; /* for its done */
} PureLinkDelivery;
typedef struct {
    int (*input)(void *context, const char *name, uint32_t shape);
    bool (*deliver)(void *context, const PureLinkDelivery *);
    void *context;
} PureLinkHost;
/* A delivery's end: heard, the last number heard; stored, its save
 * succeeded (else it is handed over again); gap, it stopped at a gap. */
typedef struct { void *token; uint64_t heard; bool stored, gap; } PureLinkDone;
/* A packet to send to an address a packet came from: whether it went. */
typedef bool PureLinkSend(void *context, const unsigned char *packet, size_t size, const unsigned char *to,
                          size_t to_size);

typedef struct PureLinkReceiver PureLinkReceiver;
PureLinkReceiver *pure_link_receiver(const PureSealKey *own, const unsigned char own_public[PURE_SEAL_KEY],
                                     const PureLinkHost *host, char *error, size_t size);
bool pure_link_allow(PureLinkReceiver *, const char *source, const unsigned char key[PURE_SEAL_KEY], char *error,
                     size_t size);
/* A sender quiet for ms (30 s at first) with nothing of it waiting lets its
 * keys, addresses and streams go: an idle link costs the receiver only its
 * allowing, and the sender's next packet makes them again (one X25519). */
void pure_link_idle(PureLinkReceiver *, uint64_t ms);
/* Remember the secret each sender's key shares with the node's in a file
 * (path, 0600; begun again for another node's key): a place a sender in
 * the order allowed, mapped, so a sender at rest takes no memory for it.
 * A node that starts again then makes no X25519 for a sender it heard from
 * before. After the senders are allowed, before the first packet. */
bool pure_link_remember(PureLinkReceiver *, const char *path, char *error, size_t size);
/* A packet that came from an address (from_size bytes, at most
 * PURE_LINK_ADDRESS kept): answers to its sender go there. */
void pure_link_receiver_take(PureLinkReceiver *, const unsigned char *packet, size_t size, const void *from,
                             size_t from_size, uint64_t now);
/* After the packets that came: the dones (count of them) applied, whole
 * records handed over, answers sent through send, and senders at rest let
 * go. */
void pure_link_receiver_turn(PureLinkReceiver *, const PureLinkDone *done, size_t count, uint64_t now,
                             PureLinkSend *send, void *context);
/* When a turn is due with no packet (ns), or 0 for none. */
uint64_t pure_link_receiver_due(const PureLinkReceiver *);
void pure_link_receiver_counts(const PureLinkReceiver *, PureLinkCounts *);
void pure_link_receiver_free(PureLinkReceiver *);
#endif
