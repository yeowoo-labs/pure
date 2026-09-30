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
 *   u8 0x70 (version 0) with the low four bits of the epoch it is sealed
 *   in, u32le the key id of the node that sent it (FNV-1a of its public
 *   key; two keys may share an id, and a receiver tries the keys it allows
 *   with it), u64le that node's run (random, new each time it starts, so no
 *   nonce repeats under a key across restarts), u64le the packet's number in
 *   its run.
 * Keys come from an epoch's secret by HKDF-SHA256, with both public keys and
 * the sending node's run: a sender's packets use its own run. A receiver's
 * answers use a run drawn for each sender's run it keeps (made again when a
 * sender at rest comes back, so no nonce repeats), and add the run they
 * answer, so an answer opens only in that run. The nonce is the key's base
 * with the packet's number in its last eight bytes (XOR, big-endian, as QUIC
 * does).
 *
 * Epoch 0's secret is the one the two nodes' keys share (X25519). A sender
 * offers the public half of a pair made for the next epoch (OFFER) in its
 * packets until the receiver accepts it with a half of its own (ACCEPT); the
 * X25519 of the two halves and the epoch's secret make the next one's
 * (HKDF, "pure link 0 epoch" and its number), and the private halves are let
 * go. The sender seals in it from the ACCEPT on; the receiver takes it once
 * a packet of it opens, and opens the late ones of the epoch before. So what
 * went in epoch 1 and on cannot be opened by one who later has both nodes'
 * keys: only a run's first packets, before its first epoch, are sealed
 * under the static keys. A new epoch every hour or GiB
 * (pure_link_sender_epochs). A receiver that remembers (pure_link_remember)
 * writes the epoch it accepted for each sender's run, and goes on in it after
 * it starts again. One that does not know a run's epoch (it forgot) answers
 * its packet with LOST, under the nodes' keys, naming the packet: its
 * sender, if the packet is one it has out, begins again as a run begins, in
 * epoch 0 with records until an epoch is accepted. A sender that hears
 * nothing for 2 s over 4 probes (an attacker may keep the answers from it)
 * offers again in epoch 0 with no records, and the chain begins again from
 * the nodes' keys.
 *
 * What an attacker on the path may do, and what it gets (link/test-link.c
 * does each):
 *   - change a packet: it does not open, and it is answered by nothing but,
 *     when its run is one the receiver does not know in an epoch past 0, a
 *     LOST (as QUIC's stateless reset: smaller than the packet, LOSTS a
 *     second at most, telling nothing but that).
 *   - send a packet of the past again: its records are applied once (the
 *     heard numbers), its word is taken only as the most said (STORED's
 *     limit), and it moves nothing: answers go where the newest packet of
 *     the run came from. A new address is sent a challenge the sender says
 *     back (the attacker cannot); until then answers there are at most
 *     three times the bytes that came from it (RFC 9000 §8).
 *   - send as a key or a source not allowed: nothing opens or is delivered.
 *     (Anyone who has the receiver's private key can seal as any sender to
 *     it, in a run of its own: static-static keys, as Noise KK's first
 *     message. Epochs do not change this; they keep it out of others'
 *     runs.)
 *   - keep what went, and later get both nodes' keys: it opens only the
 *     packets of epoch 0.
 *   - flood made-up packets under a known key's id, runs never seen: keys
 *     made for a run that did not open cost a token, 256 a second, so at
 *     most that many HKDFs a second; X25519 once a sender (remembered).
 *
 * Frames (numbers little-endian):
 *   0x01 STREAM  source (u8 length, bytes), input (u8 length, bytes), u32 the
 *                shape of the messages (the sender's output and the
 *                receiver's input must have the same): the stream the
 *                records after it are of
 *   0x02 RECORD  u64 number, u32 size, u32 offset, u16 length, bytes: part
 *                of a saved send (a send too large for what is left of a
 *                packet goes in parts, cut where the packet is full)
 *   0x03 ACK     u64 largest number, u32 delay (µs), u8 ranges, each as u64
 *                first and u64 last, newest first
 *   0x04 STORED  source, input (as STREAM), u64 the last number heard
 *                (applied and saved), u8 flags (1: a record after a gap was
 *                not taken: send what follows it again), u32 credit (records
 *                it takes past that: the sender sends up to the most heard
 *                and credit it was told)
 *   0x05 PING    to be acknowledged
 *   0x06 RECORDS u64 the first one's number, u8 count, then whole records one
 *                after another (numbers following on), each its size (a
 *                varint: LEB128) and its bytes: a send that fits the packet
 *                whole, with 1 to 3 bytes of framing, not the 19 of RECORD
 *   0x07 CHALLENGE u64: a receiver's, until its sender says it back from
 *                the address the answers go to
 *   0x08 RESPONSE u64: a challenge said back (the last few had), in the next
 *                packet that goes (records, or a probe)
 *   0x09 OFFER   u32 epoch, a public key (X25519): the sender's half of that
 *                epoch's secret, until it is accepted
 *   0x0a ACCEPT  u32 epoch, a public key: the receiver's half, until a
 *                packet of that epoch opens (the same offer again gets the
 *                same; one a second for a sender), then the first four
 *                bytes of the key offered it answers (a sender that took
 *                its epoch for lost offers epoch 1 again with another)
 *   0x0b LOST    u64 a packet's number: a receiver knows no epoch of its run
 *                (sealed in epoch 0, in a run of answers of its own)
 *   0x0c REFUSED source, input (as STREAM), u8 why (PURE_LINK_NO_INPUT and
 *                on): the receiver takes no records of that stream; its
 *                sender sends one packet of them a wait from then (a
 *                second, twice as long each time, 64 s at most) until one
 *                is stored
 *   0x00 PADDING to the end of the packet: a full packet sent in a burst is
 *                a whole datagram
 * Losses are detected as RFC 9002 says, with RACK's patience for packets
 * that come late; congestion control is CUBIC (RFC 9438), paced. */

/* A packet is PURE_LINK_DATAGRAM bytes at most at first, and up to
 * PURE_LINK_DATAGRAM_MOST once the path is found to carry that many (a
 * probe of that size acknowledged: DPLPMTUD, RFC 8899; a link probes once
 * a packet of its was full, else a larger one would carry nothing more, and
 * goes back to the first size when the larger stop coming, §4.3); a
 * source's or an input's name is 1 to PURE_LINK_NAME bytes; a receiver
 * keeps a packet's path (where it came from, and the address of its own it
 * came to: link/link.c answers from there) in PURE_LINK_ADDRESS bytes; a
 * send is PURE_LINK_RECORD_MOST bytes at most (what a receiver holds of a
 * stream not yet stored). */
enum { PURE_LINK_DATAGRAM = 1200, PURE_LINK_DATAGRAM_MOST = 1452, PURE_LINK_NAME = 64, PURE_LINK_ADDRESS = 48,
       PURE_LINK_RECORD_MOST = 16 << 20 };
/* Why a receiver refuses a stream (REFUSED): no input of that name takes
 * messages, the input takes messages of another shape, or the sender's key
 * may not send as that source. */
enum { PURE_LINK_NO_INPUT = 1, PURE_LINK_OTHER_SHAPE = 2, PURE_LINK_NOT_ALLOWED = 3 };

/* Counts for a host's report: packets and records sent, records sent again,
 * packets lost and acknowledged, packets received, deliveries handed over,
 * the last number stored; packets called lost that came late, and
 * congestion events undone for it; runs keyed (keys made from a secret),
 * secrets made (X25519), epochs begun, and a sender's epochs taken for lost
 * (begun again from the nodes' keys); streams refused (a receiver's said, a
 * sender's heard); a receiver's senders woken (their active state made), of
 * them with a secret remembered (no X25519), let go when idle, and active
 * now. */
typedef struct {
    uint64_t packets, records, resent, lost, acknowledged, received, deliveries, stored, late, undone, keyed, secrets,
        epochs, fallbacks, refused, woken, remembered, swept, active;
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
 * waits); false when it cannot be kept (or is larger than
 * PURE_LINK_RECORD_MOST). */
bool pure_link_sender_queue(PureLinkSender *, uint64_t seq, const unsigned char *bytes, size_t size);
/* A packet that came (the receiver's answers; others are let be). */
void pure_link_sender_take(PureLinkSender *, const unsigned char *packet, size_t size, uint64_t now);
/* The timers due by now: losses found, probes, records sent again. */
void pure_link_sender_tick(PureLinkSender *, uint64_t now);
/* The next packet to send into out, and its number: its size, or 0 when
 * there is none (nothing to send, or the window is full). whole: a full
 * packet is padded to the sender's datagram (so packets may go in a burst
 * of whole datagrams, as UDP GSO sends them). A probe of a larger size is
 * larger than the datagram: it goes alone. */
size_t pure_link_sender_packet(PureLinkSender *, unsigned char out[PURE_LINK_DATAGRAM_MOST], bool whole, uint64_t now,
                               uint64_t *number);
/* The size of a full packet now. */
size_t pure_link_sender_datagram(const PureLinkSender *);
/* A packet made but not sent (the socket refused it): what it carried is
 * sent again, and it counts as never sent. */
void pure_link_sender_refused(PureLinkSender *, uint64_t number);
/* When the next timer is due (ns), or 0 for none. */
uint64_t pure_link_sender_due(const PureLinkSender *);
/* Probes in a row that nothing answered (0 once anything is): the path, or
 * the receiver where it was, may be gone. */
unsigned pure_link_sender_probes(const PureLinkSender *);
/* Why the receiver refuses the stream (PURE_LINK_NO_INPUT and on), or 0:
 * cleared once it stores one of its records. */
unsigned pure_link_sender_refusal(const PureLinkSender *);
/* How often it moves on to a new epoch (ns), and after how many bytes sent
 * (an hour and a GiB at first). */
void pure_link_sender_epochs(PureLinkSender *, uint64_t every_ns, uint64_t bytes);
/* The last number queued; the last the receiver stored. */
uint64_t pure_link_sender_queued(const PureLinkSender *);
uint64_t pure_link_sender_stored(const PureLinkSender *);
/* The bytes of the sends queued and not yet stored. */
size_t pure_link_sender_held(const PureLinkSender *);
void pure_link_sender_counts(const PureLinkSender *, PureLinkCounts *);
void pure_link_sender_free(PureLinkSender *);

/* ---- The receiving side: packets from the senders allowed (a source name
 * and the key it seals with, a few bytes each however many there are: see
 * pure_link_idle). A stream (a source to an input) goes to the host's input
 * its name and shape find: its number, or -PURE_LINK_OTHER_SHAPE when it
 * takes another shape, or -1 (PURE_LINK_NO_INPUT) when none takes messages
 * by that name; a stream refused is said to be (REFUSED). Whole
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
    const unsigned char *records; /* count of: u32le size, bytes (the receiver's: the host's during the call) */
    size_t size;
    void *token; /* for its done */
} PureLinkDelivery;
typedef struct {
    int (*input)(void *context, const char *name, uint32_t shape);
    bool (*deliver)(void *context, const PureLinkDelivery *);
    void *context;
} PureLinkHost;
/* A delivery's end: heard, the last number heard; stored, its save
 * succeeded (else it is handed over again); gap, it stopped at a gap; full,
 * the host has no room for more now (its app's memory budget): what it did
 * not take is kept, and handed over again after a pause, 100 ms doubling to
 * half a second while it stays full (its sender told so meanwhile). */
typedef struct { void *token; uint64_t heard; bool stored, gap, full; } PureLinkDone;
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
 * before, and goes on in the epoch of each of its runs (a key's runs that
 * send beside its first, as a node's outputs to this one do, in 1,024
 * places beside). After the senders are allowed, before the first packet. */
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
