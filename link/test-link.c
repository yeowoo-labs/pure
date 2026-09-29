#define _GNU_SOURCE
/* A link's two sides (link/link-protocol.h) through a network of the test's
 * own: time is the test's, and packets are lost, held back (so they come out
 * of order) and doubled at random, from a seed. The receiving host applies a
 * record once, from the one after its last heard, and says so a turn later,
 * now and then failing a save; the receiver and the sender start again
 * midway, each from what its app kept. Every record must be applied once,
 * in order, as it was sent.
 *
 * And an attacker on the path: packets changed on the way (none may open),
 * packets of the past sent again, from the sender's address or the
 * attacker's own (none applied twice; nothing sent to the attacker beyond
 * three times what it sent), keys not allowed and sources not allowed
 * (nothing delivered), and a flood of made-up packets for a known key (few
 * keys made). Under all of it no key and nonce seals twice.
 *
 * And later: one who then has both nodes' private keys, and every packet
 * the sender sent. Once an epoch has begun, nothing it sent opens under
 * the keys of the nodes' static secret, and no record went under them
 * after that (forward secrecy), however often the epochs change and
 * whether the receiver starts again with its memo or without. */
#include "link-protocol.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MS UINT64_C(1000000)
enum { PACKETS = 1 << 16, HISTORY = 4096 };

static uint64_t seed;
static uint64_t next_random(void) {
    seed ^= seed << 13, seed ^= seed >> 7, seed ^= seed << 17;
    return seed;
}
static bool chance(unsigned percent) { return next_random() % 100 < percent; }

static int failures;
#define CHECK(condition, ...)                                                                                          \
    do {                                                                                                               \
        if (!(condition)) {                                                                                            \
            fprintf(stderr, "test-link: " __VA_ARGS__);                                                                \
            fputc('\n', stderr);                                                                                       \
            ++failures;                                                                                                \
        }                                                                                                              \
    } while (0)

/* A record's size and bytes, from its number: small ones, ones of a packet or
 * so, and some of many pieces. */
static size_t record_size(uint64_t seq) {
    uint64_t h = seq * 0x9e3779b97f4a7c15u;
    unsigned kind = (unsigned)(h >> 60) % 10;
    return kind < 3 ? (size_t)(h >> 8) % 61 : kind < 8 ? 61 + (size_t)(h >> 8) % 1040 : 1101 + (size_t)(h >> 8) % 19000;
}
static unsigned char record_byte(uint64_t seq, size_t i) { return (unsigned char)(seq * 31 + i * 7 + (i >> 8)); }

typedef struct {
    uint64_t at;
    bool to_receiver;
    size_t size;
    unsigned char bytes[PURE_LINK_DATAGRAM_MOST];
} Packet;

/* A nonce used: whose key (0: the sender's; 1 + n: answers to the sender's
 * n-th start), the packet's run and number. */
typedef struct { uint64_t whose, run, pn; } Nonce;

typedef struct {
    /* The network: packets on their way, each arriving at its time. */
    Packet *packets;
    size_t count;
    /* The sender's start (its address is "sender" and it); the nonces used;
     * packets of the past (a ring), sent again now and then (percent a
     * step); packets changed on the way; the attacker's bytes, in and out. */
    unsigned gen, replay;
    bool tamper;
    Nonce *nonces;
    size_t nonce_count, nonce_capacity;
    Packet *history;
    size_t history_count;
    Packet *sent_log; /* every packet the sender sent, for one who later has the keys */
    size_t sent_log_count, sent_log_capacity;
    uint64_t attacker_in, attacker_out, bent, replayed;
    unsigned loss, doubled, spread_ms; /* percent, percent, the most a packet is held back */
    uint64_t now, sent;
    /* The path cut (nothing passes) until then: cuts times, 3 s each, the
     * moment the sender begins an epoch (before the receiver has seen it). */
    uint64_t cut_until;
    unsigned cuts;
    /* A black hole: from when the sender's datagram grew, the path carries
     * nothing larger than PURE_LINK_DATAGRAM (and says nothing of it). */
    bool black_hole, hole;
    uint64_t forgot_at; /* when the receiver started again with no memo (it lost its epochs) */
    /* The receiving app: its last heard (kept across the receiver's
     * restarts), what it said of deliveries (given at the next turn). */
    uint64_t heard, applied, deliveries;
    PureLinkDone done[64], told[64];
    size_t done_count;
    unsigned fail_every; /* a delivery's save fails every so many (0: never) */
    bool later_open;     /* the app has an input "later" (as "order") */
} World;

static void put_packet(World *w, bool to_receiver, const unsigned char *bytes, size_t size) {
    ++w->sent;
    if (w->hole && size > PURE_LINK_DATAGRAM) return;
    for (int copies = chance(w->doubled) ? 2 : 1; copies--;) {
        if (chance(w->loss) || w->count == PACKETS) continue;
        Packet *p = &w->packets[w->count++];
        p->at = w->now + 5 * MS + (w->spread_ms ? next_random() % (w->spread_ms * MS) : 0);
        p->to_receiver = to_receiver;
        p->size = size;
        memcpy(p->bytes, bytes, size);
    }
}

static uint64_t get64le(const unsigned char *p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = v << 8 | p[i];
    return v;
}

static void used(World *w, uint64_t whose, const unsigned char *packet) {
    if (w->nonce_count == w->nonce_capacity) {
        w->nonce_capacity = w->nonce_capacity ? 2 * w->nonce_capacity : 4096;
        if (!(w->nonces = realloc(w->nonces, w->nonce_capacity * sizeof(Nonce)))) exit(1);
    }
    w->nonces[w->nonce_count++] = (Nonce){whose, get64le(packet + 5), get64le(packet + 13)};
}

/* An answer: to the sender's start it names (an earlier start's goes
 * nowhere), or to the attacker. */
static bool answer(void *context, const unsigned char *packet, size_t size, const unsigned char *to, size_t to_size) {
    World *w = context;
    if (to_size == 8 && !memcmp(to, "attacker", 8)) {
        w->attacker_out += size;
        return true;
    }
    CHECK(to_size == 7 && !memcmp(to, "sender", 6), "an answer to an address no packet came from");
    unsigned gen = (unsigned)(to[6] - '0');
    used(w, 1 + gen, packet);
    if (gen == w->gen) put_packet(w, false, packet, size);
    return true;
}

static int input_of(void *context, const char *name, uint32_t shape) {
    const World *w = context;
    if (strcmp(name, "order") && !(w && w->later_open && !strcmp(name, "later"))) return -PURE_LINK_NO_INPUT;
    return shape == 0x5eed ? 3 : -PURE_LINK_OTHER_SHAPE;
}

/* Applied from the one after the last heard, in order; a gap stops it. */
static bool deliver(void *context, const PureLinkDelivery *d) {
    World *w = context;
    CHECK(d->input == 3 && !strcmp(d->source, "test.ordered"), "a delivery to input %u from %s", d->input, d->source);
    if (w->done_count == sizeof(w->done) / sizeof(w->done[0])) return false; /* busy: tried again */
    ++w->deliveries;
    bool fail = w->fail_every && w->deliveries % w->fail_every == 0, gap = false;
    uint64_t heard = w->heard;
    const unsigned char *at = d->records;
    for (unsigned k = 0; k < d->count; ++k) {
        uint32_t size = (uint32_t)at[0] | (uint32_t)at[1] << 8 | (uint32_t)at[2] << 16 | (uint32_t)at[3] << 24;
        uint64_t seq = d->first + k;
        if (seq == heard + 1) {
            bool same = size == record_size(seq);
            for (size_t i = 0; same && i < size; ++i) same = at[4 + i] == record_byte(seq, i);
            CHECK(same, "record %llu is not as it was sent", (unsigned long long)seq);
            heard = seq;
        } else if (seq > heard + 1) {
            gap = true;
            break;
        }
        at += 4 + size;
    }
    if (!fail) {
        w->applied += heard - w->heard;
        w->heard = heard;
    }
    w->done[w->done_count++] = (PureLinkDone){d->token, w->heard, !fail, gap};
    return true;
}

static PureSealKey *key(const char *dir, const char *name, unsigned char public_key[PURE_SEAL_KEY]) {
    char path[512], hex[2 * PURE_SEAL_KEY + 1], error[256];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    PureSealKey *k = pure_seal_key(path, hex, error, sizeof(error));
    if (!k || !pure_seal_peer(hex, public_key)) {
        fprintf(stderr, "test-link: %s\n", error);
        exit(1);
    }
    return k;
}

typedef struct {
    const char *name;
    uint64_t records, seed;
    unsigned loss, doubled, spread_ms, fail_every;
    uint64_t pause_after, receiver_restart_after, sender_restart_after, idle_ms;
    unsigned replay; /* percent of steps that send a packet of the past again */
    bool tamper;     /* every packet comes changed three ways first */
    uint64_t epoch_ms; /* epochs this long (0: an hour) */
    bool forgetful;    /* the receiver keeps no memo: it starts again knowing no epoch */
    unsigned cuts;     /* the path cut for 3 s so many times, as an epoch begins */
    bool black_hole;   /* larger packets lost from when the path was found to carry them */
} Case;

/* Answers to a packet no one should answer: counted, but a LOST (a packet
 * of 46 bytes: a run unknown here, in an epoch past 0, as QUIC's stateless
 * reset) apart. */
typedef struct { unsigned answers, lost; size_t lost_bytes; } Answers;
static bool nothing_sent(void *context, const unsigned char *packet, size_t size, const unsigned char *to, size_t to_size) {
    (void)packet, (void)to, (void)to_size;
    Answers *a = context;
    if (size == 46) ++a->lost, a->lost_bytes += size;
    else ++a->answers;
    return true;
}

/* A packet changed on the way (a byte, cut short, one byte more) must not
 * open, and must make no answer. */
static void tamper(World *w, PureLinkSender *s, PureLinkReceiver *r, const Packet *p, const char *from) {
    unsigned char bent[PURE_LINK_DATAGRAM_MOST + 1];
    pure_link_receiver_turn(r, NULL, 0, w->now, answer, w); /* what is due goes first */
    for (int change = 0; change < 3; ++change) {
        size_t size = p->size;
        memcpy(bent, p->bytes, size);
        if (change == 0) bent[next_random() % size] ^= (unsigned char)(1 + next_random() % 255);
        else if (change == 1) size -= 1 + next_random() % 16;
        else bent[size++] = (unsigned char)next_random();
        PureLinkCounts before, after;
        Answers answers = {0};
        ++w->bent;
        if (p->to_receiver) {
            pure_link_receiver_counts(r, &before);
            pure_link_receiver_take(r, bent, size, from, 7, w->now);
            pure_link_receiver_turn(r, NULL, 0, w->now, nothing_sent, &answers);
            pure_link_receiver_counts(r, &after);
        } else {
            pure_link_sender_counts(s, &before);
            pure_link_sender_take(s, bent, size, w->now);
            pure_link_sender_counts(s, &after);
        }
        CHECK(after.received == before.received && !answers.answers && answers.lost_bytes < size,
              "a packet changed on the way (%d, to the %s) %s", change, p->to_receiver ? "receiver" : "sender",
              answers.answers || answers.lost_bytes >= size ? "made an answer" : "opened");
    }
}

/* A packet of the past, again: to the receiver from the sender's address or
 * the attacker's, or to the sender. */
static void replay(World *w, PureLinkSender *s, PureLinkReceiver *r, const char *from) {
    const Packet *h = &w->history[next_random() % (w->history_count < HISTORY ? w->history_count : HISTORY)];
    const char *only = getenv("TEST_LINK_REPLAY");
    if (only && strcmp(only, h->to_receiver ? (chance(50) ? "receiver-sender" : "receiver-attacker") : "sender")) return;
    ++w->replayed;
    if (!h->to_receiver) pure_link_sender_take(s, h->bytes, h->size, w->now);
    else if (only ? !strcmp(only, "receiver-sender") : chance(50)) pure_link_receiver_take(r, h->bytes, h->size, from, 7, w->now);
    else {
        w->attacker_in += h->size;
        pure_link_receiver_take(r, h->bytes, h->size, "attacker", 8, w->now);
    }
}

typedef struct {
    PureLinkCounts sent, heard;
    uint64_t packets, seconds_ms;
    size_t datagram; /* the sender's at the end */
    bool hole;       /* a black hole opened */
} Outcome;

/* One who later has both nodes' private keys: the keys of the sender's
 * packets under the nodes' static secret (link-protocol.c's derive). */
static bool static_keys(const unsigned char secret[32], const unsigned char *from, const unsigned char *to, uint64_t run,
                        unsigned char key[PURE_AEAD_KEY], unsigned char base[PURE_AEAD_NONCE]) {
    unsigned char info[16 + 2 * PURE_SEAL_KEY + 16], out[PURE_AEAD_KEY + PURE_AEAD_NONCE];
    memcpy(info, "pure link 0 keys", 16);
    memcpy(info + 16, from, PURE_SEAL_KEY);
    memcpy(info + 16 + PURE_SEAL_KEY, to, PURE_SEAL_KEY);
    for (int i = 0; i < 8; ++i) info[16 + 2 * PURE_SEAL_KEY + i] = (unsigned char)(run >> 8 * i);
    memset(info + 24 + 2 * PURE_SEAL_KEY, 0, 8);
    if (!pure_link_derive(secret, info, sizeof(info), out, sizeof(out))) return false;
    memcpy(key, out, PURE_AEAD_KEY);
    memcpy(base, out + PURE_AEAD_KEY, PURE_AEAD_NONCE);
    return true;
}

/* Each packet the sender sent, tried under the static keys: one of an epoch
 * past 0 must not open, and one of epoch 0 sent after its run's first packet
 * of a later epoch may carry no record. But for this: once the receiver
 * started again with no memo (and said it knows no epoch), the run begins
 * again, in epoch 0, until its next packet of a later epoch. The number
 * that opened. */
static unsigned later_with_keys(World *w, const PureSealKey *a, const PureSealKey *b, const unsigned char *public_a,
                                const unsigned char *public_b, const char *name) {
    unsigned char secret[32], again[32], key[PURE_AEAD_KEY], base[PURE_AEAD_NONCE], nonce[PURE_AEAD_NONCE],
        plain[PURE_LINK_DATAGRAM_MOST];
    if (!pure_link_secret(a, public_b, secret) || !pure_link_secret(b, public_a, again) || memcmp(secret, again, 32)) exit(1);
    PureAead *aead = pure_aead(secret);
    unsigned opened = 0;
    for (size_t k = 0; k < w->sent_log_count; ++k) {
        const Packet *p = &w->sent_log[k];
        /* Its run's first packet of a later epoch; after the receiver forgot,
         * its first packet of epoch 0 (begun again), and the first of a later
         * epoch after that one. */
        uint64_t run = get64le(p->bytes + 5), pn = get64le(p->bytes + 13), first_later = UINT64_MAX,
                 again = UINT64_MAX, again_later = UINT64_MAX;
        for (size_t j = 0; j < w->sent_log_count; ++j) {
            const Packet *q = &w->sent_log[j];
            uint64_t n = get64le(q->bytes + 13);
            if (get64le(q->bytes + 5) != run) continue;
            if ((q->bytes[0] & 15) && n < first_later) first_later = n;
            if (w->forgot_at && q->at > w->forgot_at && !(q->bytes[0] & 15) && n < again) again = n;
        }
        for (size_t j = 0; j < w->sent_log_count; ++j) {
            const Packet *q = &w->sent_log[j];
            uint64_t n = get64le(q->bytes + 13);
            if (get64le(q->bytes + 5) == run && (q->bytes[0] & 15) && n > again && n < again_later) again_later = n;
        }
        if (!static_keys(secret, public_a, public_b, run, key, base)) exit(1);
        memcpy(nonce, base, PURE_AEAD_NONCE);
        for (int i = 0; i < 8; ++i) nonce[4 + i] ^= (unsigned char)(pn >> (56 - 8 * i));
        if (!pure_aead_open(aead, key, nonce, p->bytes, 21, p->bytes + 21, p->size - 21, plain)) continue;
        ++opened;
        CHECK(!(p->bytes[0] & 15), "%s: a packet of epoch %u opened under the static keys", name, p->bytes[0] & 15);
        if (pn < first_later || (pn >= again && pn < again_later)) continue;
        size_t size = p->size - 21 - 16;
        for (size_t at = 0; at < size;) { /* its frames: none a record */
            unsigned type = plain[at++];
            if (type == 1) at += 2 + plain[at] + plain[at + 1 + plain[at]] + 4; /* STREAM */
            else if (type == 8) at += 8;                                      /* RESPONSE */
            else if (type == 9) at += 36;                                     /* OFFER */
            else {
                CHECK(type != 2 && type != 6, "%s: a record under the static keys after the epoch began (packet %llu)", name,
                      (unsigned long long)pn);
                break;
            }
        }
    }
    pure_aead_free(aead);
    explicit_bzero(secret, sizeof(secret)), explicit_bzero(again, sizeof(again));
    return opened;
}

static int by_nonce(const void *x, const void *y) {
    const Nonce *a = x, *b = y;
    return a->whose != b->whose ? (a->whose < b->whose ? -1 : 1)
           : a->run != b->run   ? (a->run < b->run ? -1 : 1)
           : a->pn != b->pn     ? (a->pn < b->pn ? -1 : 1)
                                : 0;
}

static PureLinkReceiver *receiver_of(World *w, const PureSealKey *b, const unsigned char *public_b,
                                     const unsigned char *public_a, const char *memo, uint64_t idle_ms) {
    char error[256];
    PureLinkReceiver *r = pure_link_receiver(b, public_b, &(PureLinkHost){input_of, deliver, w}, error, sizeof(error));
    if (!r || !pure_link_allow(r, "test.ordered", public_a, error, sizeof(error)) ||
        (memo && !pure_link_remember(r, memo, error, sizeof(error)))) {
        fprintf(stderr, "test-link: %s\n", error);
        exit(1);
    }
    if (idle_ms) pure_link_idle(r, idle_ms);
    return r;
}

/* The sender as its app makes it: the sends its outbox keeps, from the one
 * after the last stored, up to limit. */
static uint64_t epoch_ms; /* the senders' epochs, as the case says */

static PureLinkSender *sender_of(const PureSealKey *a, const unsigned char *public_a, const unsigned char *public_b,
                                 uint64_t from, uint64_t limit) {
    PureLinkSender *s = pure_link_sender("test.ordered", "order", 0x5eed, a, public_a);
    if (!s || !pure_link_sender_peer(s, public_b)) {
        fprintf(stderr, "test-link: cannot make a sender\n");
        exit(1);
    }
    if (epoch_ms) pure_link_sender_epochs(s, epoch_ms * MS, UINT64_C(1) << 30);
    static unsigned char bytes[20200];
    for (uint64_t seq = from; seq <= limit; ++seq) {
        size_t size = record_size(seq);
        for (size_t i = 0; i < size; ++i) bytes[i] = record_byte(seq, i);
        if (!pure_link_sender_queue(s, seq, bytes, size)) {
            fprintf(stderr, "test-link: cannot queue %llu\n", (unsigned long long)seq);
            exit(1);
        }
    }
    return s;
}

static Outcome run(const Case *c, const char *dir) {
    unsigned char public_a[PURE_SEAL_KEY], public_b[PURE_SEAL_KEY];
    PureSealKey *a = key(dir, "a.key", public_a), *b = key(dir, "b.key", public_b);
    char memo[512];
    snprintf(memo, sizeof(memo), "%s/b.key.links", dir);
    unlink(memo);
    World w = {.packets = malloc(PACKETS * sizeof(Packet)), .loss = c->loss, .doubled = c->doubled,
               .spread_ms = c->spread_ms, .fail_every = c->fail_every, .replay = c->replay, .tamper = c->tamper,
               .cuts = c->cuts, .black_hole = c->black_hole, .history = malloc(HISTORY * sizeof(Packet))};
    if (!w.packets || !w.history) exit(1);
    char from[8] = "sender0";
    epoch_ms = c->epoch_ms;
    const char *keep = c->forgetful ? NULL : memo;
    seed = c->seed;
    uint64_t queued = c->pause_after ? c->pause_after : c->records, resumed = 0;
    PureLinkSender *s = sender_of(a, public_a, public_b, 1, queued);
    PureLinkReceiver *r = receiver_of(&w, b, public_b, public_a, keep, c->idle_ms);
    PureLinkCounts sent_before = {0}, heard_before = {0};
    bool receiver_restarted = !c->receiver_restart_after, sender_restarted = !c->sender_restart_after;
    unsigned char out[PURE_LINK_DATAGRAM_MOST];
    while (w.heard < c->records || pure_link_sender_stored(s) < c->records) {
        CHECK(w.now < 600000 * MS, "%s: not done after ten minutes (heard %llu of %llu)", c->name,
              (unsigned long long)w.heard, (unsigned long long)c->records);
        if (w.now >= 600000 * MS) break;
        uint64_t number;
        for (size_t size; (size = pure_link_sender_packet(s, out, false, w.now, &number));) {
            used(&w, 0, out);
            if (w.sent_log_count == w.sent_log_capacity) {
                w.sent_log_capacity = w.sent_log_capacity ? 2 * w.sent_log_capacity : 1024;
                if (!(w.sent_log = realloc(w.sent_log, w.sent_log_capacity * sizeof(Packet)))) exit(1);
            }
            w.sent_log[w.sent_log_count] = (Packet){.at = w.now, .size = size}; /* at: when it was sent */
            memcpy(w.sent_log[w.sent_log_count++].bytes, out, size);
            put_packet(&w, true, out, size);
            if (w.black_hole && pure_link_sender_datagram(s) > PURE_LINK_DATAGRAM) w.hole = true;
        }
        /* Then the next thing to happen. */
        uint64_t next = UINT64_MAX, due = pure_link_sender_due(s), turn = pure_link_receiver_due(r);
        for (size_t k = 0; k < w.count; ++k)
            if (w.packets[k].at < next) next = w.packets[k].at;
        if (due && due < next) next = due;
        if (turn && turn < next) next = turn;
        if (w.done_count && w.now + MS < next) next = w.now + MS; /* the host's word, a turn later */
        if (queued < c->records && pure_link_sender_stored(s) >= queued && !resumed) resumed = w.now + 2000 * MS;
        if (resumed && queued < c->records && resumed < next) next = resumed;
        if (next == UINT64_MAX) {
            CHECK(false, "%s: nothing more happens (heard %llu of %llu)", c->name, (unsigned long long)w.heard,
                  (unsigned long long)c->records);
            break;
        }
        w.now = next > w.now ? next : w.now;
        if (resumed && resumed <= w.now && queued < c->records) { /* after a pause: the rest */
            static unsigned char bytes[20200];
            for (uint64_t seq = queued + 1; seq <= c->records; ++seq) {
                size_t size = record_size(seq);
                for (size_t i = 0; i < size; ++i) bytes[i] = record_byte(seq, i);
                CHECK(pure_link_sender_queue(s, seq, bytes, size), "%s: cannot queue %llu", c->name, (unsigned long long)seq);
            }
            queued = c->records;
        }
        for (size_t k = 0; k < w.count;) {
            Packet *p = &w.packets[k];
            if (p->at > w.now) {
                ++k;
                continue;
            }
            if (w.now < w.cut_until) { /* the path is cut */
                *p = w.packets[--w.count];
                continue;
            }
            if (w.tamper) tamper(&w, s, r, p, from);
            if (p->to_receiver) pure_link_receiver_take(r, p->bytes, p->size, from, 7, w.now);
            else {
                PureLinkCounts before, after;
                pure_link_sender_counts(s, &before);
                pure_link_sender_take(s, p->bytes, p->size, w.now);
                pure_link_sender_counts(s, &after);
                if (w.cuts && after.epochs > before.epochs) --w.cuts, w.cut_until = w.now + 3000 * MS;
            }
            w.history[w.history_count++ % HISTORY] = *p;
            *p = w.packets[--w.count];
        }
        if (w.replay && w.history_count && chance(w.replay)) replay(&w, s, r, from);
        pure_link_sender_tick(s, w.now);
        size_t told = w.done_count;
        memcpy(w.told, w.done, told * sizeof(*w.done));
        w.done_count = 0;
        pure_link_receiver_turn(r, w.told, told, w.now, answer, &w);
        if (!receiver_restarted && w.heard >= c->receiver_restart_after) { /* kill -9: what it held is gone */
            receiver_restarted = true;
            pure_link_receiver_counts(r, &heard_before);
            pure_link_receiver_free(r);
            w.done_count = 0;
            r = receiver_of(&w, b, public_b, public_a, keep, c->idle_ms);
            if (!keep) w.forgot_at = w.now;
        }
        if (!sender_restarted && pure_link_sender_stored(s) >= c->sender_restart_after) {
            sender_restarted = true;
            pure_link_sender_counts(s, &sent_before);
            uint64_t stored = pure_link_sender_stored(s);
            pure_link_sender_free(s);
            s = sender_of(a, public_a, public_b, stored + 1, queued);
            snprintf(from, sizeof(from), "sender%u", ++w.gen); /* a new process: another address */
        }
    }
    Outcome o = {.packets = w.sent, .seconds_ms = w.now / MS, .datagram = pure_link_sender_datagram(s), .hole = w.hole};
    pure_link_sender_counts(s, &o.sent);
    pure_link_receiver_counts(r, &o.heard);
    o.sent.packets += sent_before.packets, o.sent.resent += sent_before.resent, o.sent.epochs += sent_before.epochs;
    o.sent.fallbacks += sent_before.fallbacks;
    o.heard.woken += heard_before.woken, o.heard.swept += heard_before.swept;
    CHECK(w.heard == c->records && w.applied == c->records, "%s: applied %llu, heard %llu of %llu", c->name,
          (unsigned long long)w.applied, (unsigned long long)w.heard, (unsigned long long)c->records);
    CHECK(pure_link_sender_stored(s) == c->records, "%s: the sender knows %llu stored of %llu", c->name,
          (unsigned long long)pure_link_sender_stored(s), (unsigned long long)c->records);
    /* No key sealed two packets with one nonce. */
    qsort(w.nonces, w.nonce_count, sizeof(Nonce), by_nonce);
    for (size_t k = 1; k < w.nonce_count; ++k)
        if (!by_nonce(&w.nonces[k - 1], &w.nonces[k])) {
            CHECK(false, "%s: a nonce used twice (%s, run %llx, number %llu)", c->name,
                  w.nonces[k].whose ? "an answer" : "the sender", (unsigned long long)w.nonces[k].run,
                  (unsigned long long)w.nonces[k].pn);
            break;
        }
    CHECK(w.attacker_out <= 3 * w.attacker_in, "%s: %llu bytes to the attacker for its %llu", c->name,
          (unsigned long long)w.attacker_out, (unsigned long long)w.attacker_in);
    unsigned opened_later = later_with_keys(&w, a, b, public_a, public_b, c->name);
    printf("{\"case\":\"%s\",\"records\":%llu,\"virtual_ms\":%llu,\"packets\":%llu,\"resent\":%llu,\"lost\":%llu,"
           "\"deliveries\":%llu,\"woken\":%llu,\"remembered\":%llu,\"swept\":%llu,\"changed\":%llu,\"replayed\":%llu,"
           "\"attacker_in\":%llu,\"attacker_out\":%llu,\"epochs\":%llu,\"opened_later\":%u}\n",
           c->name, (unsigned long long)c->records, (unsigned long long)o.seconds_ms, (unsigned long long)o.sent.packets,
           (unsigned long long)o.sent.resent, (unsigned long long)o.sent.lost, (unsigned long long)w.deliveries,
           (unsigned long long)o.heard.woken, (unsigned long long)o.heard.remembered, (unsigned long long)o.heard.swept,
           (unsigned long long)w.bent, (unsigned long long)w.replayed, (unsigned long long)w.attacker_in,
           (unsigned long long)w.attacker_out, (unsigned long long)o.sent.epochs, opened_later);
    pure_link_sender_free(s);
    pure_link_receiver_free(r);
    pure_seal_key_free(a);
    pure_seal_key_free(b);
    free(w.packets);
    free(w.history);
    free(w.nonces);
    free(w.sent_log);
    return o;
}

static uint32_t key_id(const unsigned char key[PURE_SEAL_KEY]) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < PURE_SEAL_KEY; ++i) h = (h ^ key[i]) * 16777619u;
    return h;
}

/* Senders the receiver must not hear: a key it does not allow, a packet of
 * that key under an allowed key's id, and an allowed key sending as a
 * source it is not allowed. Nothing opens but the last's, and nothing is
 * delivered (deliver would say whose). */
static void impostors(const char *dir) {
    unsigned char public_a[PURE_SEAL_KEY], public_b[PURE_SEAL_KEY], public_c[PURE_SEAL_KEY], out[PURE_LINK_DATAGRAM_MOST];
    PureSealKey *a = key(dir, "a.key", public_a), *b = key(dir, "b.key", public_b), *c = key(dir, "c.key", public_c);
    char memo[512];
    snprintf(memo, sizeof(memo), "%s/b.key.links", dir);
    unlink(memo);
    World w = {0};
    PureLinkReceiver *r = receiver_of(&w, b, public_b, public_a, memo, 0);
    PureLinkSender *stranger = sender_of(c, public_c, public_b, 1, 20);
    PureLinkSender *other = pure_link_sender("other.source", "order", 0x5eed, a, public_a);
    unsigned char bytes[64] = {0};
    if (!other || !pure_link_sender_peer(other, public_b)) exit(1);
    for (uint64_t seq = 1; seq <= 20; ++seq) pure_link_sender_queue(other, seq, bytes, sizeof(bytes));
    uint32_t id = key_id(public_a);
    uint64_t number, opened = 0;
    unsigned answers = 0;
    for (int round = 0; round < 50; ++round, w.now += 10 * MS) {
        PureLinkCounts before, after;
        for (size_t size; (size = pure_link_sender_packet(stranger, out, false, w.now, &number));) {
            pure_link_receiver_take(r, out, size, "sender0", 7, w.now);
            for (int i = 0; i < 4; ++i) out[1 + i] = (unsigned char)(id >> 8 * i); /* an allowed key's id */
            pure_link_receiver_take(r, out, size, "sender0", 7, w.now);
        }
        pure_link_receiver_counts(r, &before);
        CHECK(before.received == opened, "a key not allowed was heard (%llu packets)",
              (unsigned long long)(before.received - opened));
        for (size_t size; (size = pure_link_sender_packet(other, out, false, w.now, &number));)
            pure_link_receiver_take(r, out, size, "sender0", 7, w.now);
        pure_link_receiver_counts(r, &after);
        opened = after.received;
        pure_link_receiver_turn(r, NULL, 0, w.now, nothing_sent, &answers);
        pure_link_sender_tick(stranger, w.now), pure_link_sender_tick(other, w.now);
    }
    CHECK(!w.deliveries, "impostors: %llu deliveries", (unsigned long long)w.deliveries);
    printf("{\"case\":\"impostors\",\"opened\":%llu,\"answers\":%u,\"deliveries\":%llu}\n", (unsigned long long)opened,
           answers, (unsigned long long)w.deliveries);
    pure_link_sender_free(stranger);
    pure_link_sender_free(other);
    pure_link_receiver_free(r);
    pure_seal_key_free(a), pure_seal_key_free(b), pure_seal_key_free(c);
}

/* Made-up packets under an allowed key's id, each of a run never seen, as
 * fast as 100,000 a second: they cost the receiver few keys (a rate
 * limited), no X25519 past the first, and no answer; and the sender, heard
 * after, is heard all the same. */
static void flood(const char *dir) {
    unsigned char public_a[PURE_SEAL_KEY], public_b[PURE_SEAL_KEY], packet[PURE_LINK_DATAGRAM_MOST];
    PureSealKey *a = key(dir, "a.key", public_a), *b = key(dir, "b.key", public_b);
    char memo[512];
    snprintf(memo, sizeof(memo), "%s/b.key.links", dir);
    unlink(memo);
    World w = {.packets = malloc(PACKETS * sizeof(Packet))};
    if (!w.packets) exit(1);
    PureLinkReceiver *r = receiver_of(&w, b, public_b, public_a, memo, 0);
    uint32_t id = key_id(public_a);
    for (int i = 0; i < (getenv("TEST_LINK_NO_FLOOD") ? 0 : 10000); ++i, w.now += 10000) {
        size_t size = 64 + next_random() % 1100;
        for (size_t k = 0; k < size; ++k) packet[k] = (unsigned char)next_random();
        packet[0] = (unsigned char)(0x70 | (i & 1)); /* half in an epoch past 0: a LOST each, while tokens last */
        for (int j = 0; j < 4; ++j) packet[1 + j] = (unsigned char)(id >> 8 * j);
        w.attacker_in += size;
        pure_link_receiver_take(r, packet, size, "attacker", 8, w.now);
        if (i % 64 == 0) pure_link_receiver_turn(r, NULL, 0, w.now, answer, &w);
    }
    PureLinkCounts flooded;
    pure_link_receiver_counts(r, &flooded);
    /* Then the sender: its records all delivered. */
    PureLinkSender *s = sender_of(a, public_a, public_b, 1, 200);
    unsigned char out[PURE_LINK_DATAGRAM_MOST];
    uint64_t number;
    for (int step = 0; step < 20000 && w.heard < 200; ++step, w.now += MS) {
        for (size_t size; (size = pure_link_sender_packet(s, out, false, w.now, &number));)
            pure_link_receiver_take(r, out, size, "sender0", 7, w.now);
        size_t told = w.done_count;
        memcpy(w.told, w.done, told * sizeof(*w.done));
        w.done_count = 0;
        pure_link_receiver_turn(r, w.told, told, w.now, answer, &w);
        for (size_t k = 0; k < w.count; ++k) pure_link_sender_take(s, w.packets[k].bytes, w.packets[k].size, w.now);
        w.count = 0;
        pure_link_sender_tick(s, w.now);
        if (getenv("TEST_LINK_DEBUG") && step < 40) {
            PureLinkCounts rc, sc;
            pure_link_receiver_counts(r, &rc), pure_link_sender_counts(s, &sc);
            fprintf(stderr, "step %d: sent %llu answers %llu received %llu delivered %llu heard %llu due %llu\n", step,
                    (unsigned long long)sc.packets, (unsigned long long)sc.received, (unsigned long long)rc.received,
                    (unsigned long long)rc.deliveries, (unsigned long long)w.heard,
                    (unsigned long long)pure_link_sender_due(s));
        }
    }
    CHECK(flooded.keyed <= 300 && flooded.secrets <= 1 && w.attacker_out <= 46 * (4096 + 410) &&
              w.attacker_out < w.attacker_in, /* keys: 256 at once, 256 a second; LOSTs: 4,096 at once, 4,096 a second */
          "flood: %llu runs keyed, %llu secrets made, %llu bytes to the attacker for its %llu",
          (unsigned long long)flooded.keyed, (unsigned long long)flooded.secrets, (unsigned long long)w.attacker_out,
          (unsigned long long)w.attacker_in);
    CHECK(w.heard == 200, "flood: the sender heard after it: %llu of 200", (unsigned long long)w.heard);
    if (getenv("TEST_LINK_DEBUG")) {
        PureLinkCounts rc, sc;
        pure_link_receiver_counts(r, &rc), pure_link_sender_counts(s, &sc);
        fprintf(stderr, "receiver received %llu woken %llu deliveries %llu keyed %llu active %llu; sender packets %llu received %llu stored %llu\n",
                (unsigned long long)rc.received, (unsigned long long)rc.woken, (unsigned long long)rc.deliveries,
                (unsigned long long)rc.keyed, (unsigned long long)rc.active, (unsigned long long)sc.packets,
                (unsigned long long)sc.received, (unsigned long long)sc.stored);
    }
    printf("{\"case\":\"flood\",\"packets\":10000,\"keyed\":%llu,\"secrets\":%llu,\"to_attacker\":%llu,\"heard_after\":%llu}\n",
           (unsigned long long)flooded.keyed, (unsigned long long)flooded.secrets, (unsigned long long)w.attacker_out,
           (unsigned long long)w.heard);
    pure_link_sender_free(s);
    pure_link_receiver_free(r);
    pure_seal_key_free(a), pure_seal_key_free(b);
    free(w.packets);
}

/* Three outputs of one node to this one: three senders with one key, each a
 * run of its own, sending at once. Each run keeps a place of its own (none
 * pushes another out, which sent its answers to the wrong run), each keyed
 * once, and every stream's records are delivered once, in order. Then,
 * each in an epoch past 0, the receiver starts again (kill -9) with its
 * memo: every run goes on in its epoch, none told LOST, no X25519 made. */
typedef struct {
    uint64_t now, heard[3], queued;
    PureLinkSender *senders[3];
    PureLinkDone done[64];
    size_t done_count;
} Outputs;
static int output_input(void *context, const char *name, uint32_t shape) {
    (void)context;
    return shape != 0x5eed ? -1 : !strcmp(name, "one") ? 0 : !strcmp(name, "two") ? 1 : !strcmp(name, "three") ? 2 : -1;
}
static bool output_deliver(void *context, const PureLinkDelivery *d) {
    Outputs *o = context;
    if (o->done_count == sizeof(o->done) / sizeof(o->done[0])) return false;
    uint64_t heard = o->heard[d->input];
    bool gap = false;
    const unsigned char *at = d->records;
    for (unsigned k = 0; k < d->count; ++k) {
        uint32_t size = (uint32_t)at[0] | (uint32_t)at[1] << 8 | (uint32_t)at[2] << 16 | (uint32_t)at[3] << 24;
        uint64_t seq = d->first + k;
        if (seq > heard + 1) {
            gap = true;
            break;
        }
        if (seq == heard + 1) {
            bool same = size == record_size(seq);
            for (size_t i = 0; same && i < size; ++i) same = at[4 + i] == record_byte(seq, i);
            CHECK(same, "outputs: record %llu of input %u is not as it was sent", (unsigned long long)seq, d->input);
            heard = seq;
        }
        at += 4 + size;
    }
    o->heard[d->input] = heard;
    o->done[o->done_count++] = (PureLinkDone){d->token, heard, true, gap};
    return true;
}
static bool output_answer(void *context, const unsigned char *packet, size_t size, const unsigned char *to,
                          size_t to_size) {
    (void)to, (void)to_size;
    Outputs *o = context;
    for (unsigned i = 0; i < 3; ++i) pure_link_sender_take(o->senders[i], packet, size, o->now); /* its run's opens */
    return true;
}
static PureLinkReceiver *outputs_receiver(Outputs *o, const PureSealKey *b, const unsigned char *public_b,
                                          const unsigned char *public_a, const char *memo) {
    char error[256];
    PureLinkReceiver *r = pure_link_receiver(b, public_b, &(PureLinkHost){output_input, output_deliver, o}, error,
                                             sizeof(error));
    if (!r || !pure_link_allow(r, "test.ordered", public_a, error, sizeof(error)) ||
        !pure_link_remember(r, memo, error, sizeof(error))) {
        fprintf(stderr, "test-link: %s\n", error);
        exit(1);
    }
    return r;
}
/* Each sender's records to last queued. */
static void outputs_queue(Outputs *o, uint64_t last) {
    static unsigned char bytes[20200];
    for (unsigned i = 0; i < 3; ++i)
        for (uint64_t seq = o->queued + 1; seq <= last; ++seq) {
            size_t size = record_size(seq);
            for (size_t k = 0; k < size; ++k) bytes[k] = record_byte(seq, k);
            if (!pure_link_sender_queue(o->senders[i], seq, bytes, size)) exit(1);
        }
    o->queued = last;
}
/* Until each sender's records are stored, or a minute; when epochs, a record
 * more each 50 ms until each is in an epoch past 0 (an offer goes in a
 * packet of records). */
static void outputs_until(Outputs *o, PureLinkReceiver *r, bool epochs) {
    unsigned char out[PURE_LINK_DATAGRAM_MOST];
    uint64_t number, end = o->now + 60000 * MS;
    for (bool all = false; o->now < end && !all; o->now += MS) {
        if (epochs && o->now % (50 * MS) == 0) outputs_queue(o, o->queued + 1);
        for (unsigned i = 0; i < 3; ++i)
            for (size_t size; (size = pure_link_sender_packet(o->senders[i], out, false, o->now, &number));)
                pure_link_receiver_take(r, out, size, "sender0", 7, o->now);
        PureLinkDone told[64];
        size_t count = o->done_count;
        memcpy(told, o->done, count * sizeof(*told));
        o->done_count = 0;
        pure_link_receiver_turn(r, told, count, o->now, output_answer, o);
        all = true;
        for (unsigned i = 0; i < 3; ++i) {
            PureLinkCounts c;
            pure_link_sender_tick(o->senders[i], o->now);
            pure_link_sender_counts(o->senders[i], &c);
            all = all && pure_link_sender_stored(o->senders[i]) == o->queued && (!epochs || c.epochs >= 1);
        }
    }
}
static void outputs(const char *dir) {
    unsigned char public_a[PURE_SEAL_KEY], public_b[PURE_SEAL_KEY];
    char memo[512];
    PureSealKey *a = key(dir, "a.key", public_a), *b = key(dir, "b.key", public_b);
    snprintf(memo, sizeof(memo), "%s/b.key.links", dir);
    unlink(memo);
    Outputs o = {0};
    PureLinkReceiver *r = outputs_receiver(&o, b, public_b, public_a, memo);
    const char *inputs[3] = {"one", "two", "three"};
    for (unsigned i = 0; i < 3; ++i)
        if (!(o.senders[i] = pure_link_sender("test.ordered", inputs[i], 0x5eed, a, public_a)) ||
            !pure_link_sender_peer(o.senders[i], public_b))
            exit(1);
    outputs_queue(&o, 500);
    outputs_until(&o, r, false);
    PureLinkCounts first;
    pure_link_receiver_counts(r, &first);
    uint64_t took = o.now / MS;
    outputs_until(&o, r, true); /* a new epoch a second for a peer: the third's after two */
    pure_link_receiver_free(r); /* kill -9: what it held is gone, the memo stays */
    o.done_count = 0;
    r = outputs_receiver(&o, b, public_b, public_a, memo);
    outputs_queue(&o, 1000);
    outputs_until(&o, r, false);
    PureLinkCounts again, sent = {0};
    pure_link_receiver_counts(r, &again);
    for (unsigned i = 0; i < 3; ++i) {
        PureLinkCounts c;
        pure_link_sender_counts(o.senders[i], &c);
        sent.fallbacks += c.fallbacks;
        CHECK(pure_link_sender_stored(o.senders[i]) == 1000 && o.heard[i] == 1000, "outputs: %u stored %llu, heard %llu",
              i, (unsigned long long)pure_link_sender_stored(o.senders[i]), (unsigned long long)o.heard[i]);
    }
    CHECK(first.keyed == 3 && took < 1000, "outputs: %llu runs keyed (3 runs), stored in %llu ms",
          (unsigned long long)first.keyed, (unsigned long long)took);
    CHECK(!sent.fallbacks && !again.secrets && again.keyed == 3,
          "outputs, started again: %llu epochs taken for lost, %llu secrets made, %llu runs keyed",
          (unsigned long long)sent.fallbacks, (unsigned long long)again.secrets, (unsigned long long)again.keyed);
    printf("{\"case\":\"outputs\",\"runs\":3,\"records\":3000,\"stored_ms\":%llu,\"keyed\":%llu,"
           "\"keyed_again\":%llu,\"secrets_again\":%llu,\"fallbacks\":%llu}\n",
           (unsigned long long)took, (unsigned long long)first.keyed, (unsigned long long)again.keyed,
           (unsigned long long)again.secrets, (unsigned long long)sent.fallbacks);
    for (unsigned i = 0; i < 3; ++i) pure_link_sender_free(o.senders[i]);
    pure_link_receiver_free(r);
    pure_seal_key_free(a), pure_seal_key_free(b);
}

/* Streams a receiver refuses: a source its key may not send as, an input it
 * does not have, one of another shape. Each sender hears why, and then
 * tries a packet a wait (1, 2, 4 s ...) instead of sending its records
 * again every second; and one refused until the receiver's app has the
 * input goes on then, all its records delivered. */
static void refusals(const char *dir) {
    unsigned char public_a[PURE_SEAL_KEY], public_b[PURE_SEAL_KEY], out[PURE_LINK_DATAGRAM_MOST];
    PureSealKey *a = key(dir, "a.key", public_a), *b = key(dir, "b.key", public_b);
    char memo[512];
    snprintf(memo, sizeof(memo), "%s/b.key.links", dir);
    unlink(memo);
    World w = {.packets = malloc(PACKETS * sizeof(Packet))};
    if (!w.packets) exit(1);
    /* Its senders let go only after two minutes quiet: else one of the runs
     * the memo does not keep is answered LOST at a try (a minute apart). */
    PureLinkReceiver *r = receiver_of(&w, b, public_b, public_a, memo, 120000);
    struct { const char *source, *input; uint32_t shape; unsigned why; PureLinkSender *s; uint64_t packets; } links[] = {
        {"other.source", "order", 0x5eed, PURE_LINK_NOT_ALLOWED, NULL, 0},
        {"test.ordered", "nothing", 0x5eed, PURE_LINK_NO_INPUT, NULL, 0},
        {"test.ordered", "order", 0x5eee, PURE_LINK_OTHER_SHAPE, NULL, 0},
        {"test.ordered", "later", 0x5eed, 0, NULL, 0}, /* no such input until 10 s on */
    };
    enum { LINKS = sizeof(links) / sizeof(links[0]) };
    static unsigned char bytes[20200];
    for (unsigned i = 0; i < LINKS; ++i) {
        PureLinkSender *s = links[i].s = pure_link_sender(links[i].source, links[i].input, links[i].shape, a, public_a);
        if (!s || !pure_link_sender_peer(s, public_b)) exit(1);
        for (uint64_t seq = 1; seq <= 200; ++seq) {
            size_t size = record_size(seq);
            for (size_t k = 0; k < size; ++k) bytes[k] = record_byte(seq, k);
            if (!pure_link_sender_queue(s, seq, bytes, size)) exit(1);
        }
    }
    uint64_t number;
    for (; w.now < 90000 * MS; w.now += MS) {
        if (w.now == 10000 * MS) w.later_open = true;
        for (unsigned i = 0; i < LINKS; ++i)
            for (size_t size; (size = pure_link_sender_packet(links[i].s, out, false, w.now, &number)); ++links[i].packets)
                pure_link_receiver_take(r, out, size, "sender0", 7, w.now);
        size_t told = w.done_count;
        memcpy(w.told, w.done, told * sizeof(*w.done));
        w.done_count = 0;
        pure_link_receiver_turn(r, w.told, told, w.now, answer, &w);
        for (size_t k = 0; k < w.count;) { /* each answer to all: only its sender's run opens it */
            if (w.packets[k].at > w.now) {
                ++k;
                continue;
            }
            for (unsigned i = 0; i < LINKS; ++i) pure_link_sender_take(links[i].s, w.packets[k].bytes, w.packets[k].size, w.now);
            w.packets[k] = w.packets[--w.count];
        }
        for (unsigned i = 0; i < LINKS; ++i) pure_link_sender_tick(links[i].s, w.now);
    }
    PureLinkCounts heard;
    pure_link_receiver_counts(r, &heard);
    for (unsigned i = 0; i < LINKS; ++i) {
        PureLinkCounts c;
        pure_link_sender_counts(links[i].s, &c);
        unsigned why = pure_link_sender_refusal(links[i].s);
        printf("{\"case\":\"refused\",\"source\":\"%s\",\"input\":\"%s\",\"why\":%u,\"refused\":%llu,\"packets\":%llu,"
               "\"stored\":%llu}\n", links[i].source, links[i].input, why, (unsigned long long)c.refused,
               (unsigned long long)links[i].packets, (unsigned long long)pure_link_sender_stored(links[i].s));
        if (links[i].why) /* refused at once, then at 1, 3, 7, 15, 31 and 63 s: a packet each */
            CHECK(why == links[i].why && c.refused == 7 && links[i].packets <= 30,
                  "refused %s to %s: why %u (not %u), refused %llu times, %llu packets", links[i].source,
                  links[i].input, why, links[i].why, (unsigned long long)c.refused, (unsigned long long)links[i].packets);
        else /* taken at the try after 10 s (15 s on) */
            CHECK(!why && c.refused == 4 && pure_link_sender_stored(links[i].s) == 200 && w.heard == 200,
                  "refused until taken: why %u, refused %llu times, %llu stored, %llu heard", why,
                  (unsigned long long)c.refused, (unsigned long long)pure_link_sender_stored(links[i].s),
                  (unsigned long long)w.heard);
        pure_link_sender_free(links[i].s);
    }
    CHECK(heard.refused >= 3 * 7 + 4, "refused: the receiver said so %llu times", (unsigned long long)heard.refused);
    pure_link_receiver_free(r);
    pure_seal_key_free(a), pure_seal_key_free(b);
    free(w.packets);
}

/* The largest send (PURE_LINK_RECORD_MOST) goes whole, over a receiver that
 * never heard of the stream before; one byte more is not taken to send. */
typedef struct { World *w; PureLinkSender *s; } Largest;
static bool deliver_largest(void *context, const PureLinkDelivery *d) {
    World *w = context;
    uint32_t size = (uint32_t)d->records[0] | (uint32_t)d->records[1] << 8 | (uint32_t)d->records[2] << 16 |
                    (uint32_t)d->records[3] << 24;
    bool same = d->first == 1 && d->count == 1 && size == PURE_LINK_RECORD_MOST;
    for (size_t i = 0; same && i < size; i += 4099) same = d->records[4 + i] == record_byte(1, i);
    CHECK(same, "largest: a delivery of %u records from %llu, the first %u bytes", d->count,
          (unsigned long long)d->first, size);
    w->heard = same ? 1 : w->heard;
    w->done[w->done_count++] = (PureLinkDone){d->token, 1, true, false};
    return true;
}
static bool answer_largest(void *context, const unsigned char *packet, size_t size, const unsigned char *to,
                           size_t to_size) {
    (void)to, (void)to_size;
    Largest *l = context;
    pure_link_sender_take(l->s, packet, size, l->w->now);
    return true;
}
static void largest(const char *dir) {
    unsigned char public_a[PURE_SEAL_KEY], public_b[PURE_SEAL_KEY], out[PURE_LINK_DATAGRAM_MOST];
    char error[256];
    PureSealKey *a = key(dir, "a.key", public_a), *b = key(dir, "b.key", public_b);
    World w = {0};
    PureLinkReceiver *r = pure_link_receiver(b, public_b, &(PureLinkHost){input_of, deliver_largest, &w}, error,
                                             sizeof(error));
    PureLinkSender *s = pure_link_sender("test.ordered", "order", 0x5eed, a, public_a);
    unsigned char *bytes = malloc(PURE_LINK_RECORD_MOST + 1);
    if (!r || !s || !bytes || !pure_link_allow(r, "test.ordered", public_a, error, sizeof(error)) ||
        !pure_link_sender_peer(s, public_b))
        exit(1);
    for (size_t i = 0; i <= PURE_LINK_RECORD_MOST; ++i) bytes[i] = record_byte(1, i);
    CHECK(!pure_link_sender_queue(s, 1, bytes, PURE_LINK_RECORD_MOST + 1), "largest: a send past the most was taken");
    CHECK(pure_link_sender_queue(s, 1, bytes, PURE_LINK_RECORD_MOST), "largest: the most a send may be was not taken");
    free(bytes);
    Largest l = {&w, s};
    uint64_t number;
    for (; w.now < 60000 * MS && pure_link_sender_stored(s) < 1; w.now += MS) {
        for (size_t size; (size = pure_link_sender_packet(s, out, false, w.now, &number));)
            pure_link_receiver_take(r, out, size, "sender0", 7, w.now);
        size_t told = w.done_count;
        memcpy(w.told, w.done, told * sizeof(*w.done));
        w.done_count = 0;
        pure_link_receiver_turn(r, w.told, told, w.now, answer_largest, &l);
        pure_link_sender_tick(s, w.now);
    }
    CHECK(w.heard == 1 && pure_link_sender_stored(s) == 1, "largest: heard %llu, stored %llu after %llu ms",
          (unsigned long long)w.heard, (unsigned long long)pure_link_sender_stored(s), (unsigned long long)(w.now / MS));
    printf("{\"case\":\"largest\",\"bytes\":%d,\"virtual_ms\":%llu}\n", PURE_LINK_RECORD_MOST,
           (unsigned long long)(w.now / MS));
    pure_link_sender_free(s);
    pure_link_receiver_free(r);
    pure_seal_key_free(a), pure_seal_key_free(b);
}

int main(void) {
    char dir[] = "/tmp/pure-test-link-XXXXXX", error[256];
    if (!mkdtemp(dir) || !pure_seal_ready(error, sizeof(error))) {
        fprintf(stderr, "test-link: %s\n", error);
        return 1;
    }
    /* A clean network: nothing is sent twice. */
    Case clean = {"clean", 3000, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, false, 0, false, 0, false};
    Outcome o = run(&clean, dir);
    CHECK(o.sent.resent == 0 && o.sent.lost == 0, "clean: %llu sent again, %llu lost", (unsigned long long)o.sent.resent,
          (unsigned long long)o.sent.lost);
    /* Loss, packets out of order and doubled, failed saves, a pause long
     * enough for the receiver to let the sender's state go, and both sides
     * starting again: the receiver remembers its secret, so it wakes the
     * sender without an X25519. */
    for (uint64_t s = 1; s <= 4; ++s) {
        Case rough = {"rough", 3000, s * 7919, 10, 3, 20, 37, 1500, 800, 2200, 300, 0, false, 0, false, 0, false};
        o = run(&rough, dir);
        CHECK(o.sent.resent > 0 && o.heard.swept >= 1 && o.heard.woken >= 1 && o.heard.remembered >= 1,
              "rough: sent again %llu, swept %llu, woken %llu, remembered %llu", (unsigned long long)o.sent.resent,
              (unsigned long long)o.heard.swept, (unsigned long long)o.heard.woken,
              (unsigned long long)o.heard.remembered);
    }
    /* An attacker on the path: packets changed on the way, and packets of
     * the past sent again, from the sender's address and its own, across the
     * same restarts and pause. */
    Case changed = {"changed", 1500, 31, 3, 0, 5, 0, 0, 0, 0, 0, 0, true, 0, false, 0, false};
    run(&changed, dir);
    for (uint64_t s = 1; s <= 2; ++s) {
        Case again = {"replayed", 3000, s * 104729, 5, 3, 20, 37, 1500, 800, 2200, 300, 25, false, 0, false, 0, false};
        run(&again, dir);
    }
    /* Epochs a fifth of a second long (a new one a second, as the receiver
     * accepts), and a receiver that starts again with no memo: it knows no
     * epoch, and the sender begins them again. */
    Case epochs = {"epochs", 3000, 271, 5, 3, 20, 37, 1500, 800, 2200, 300, 10, false, 200, false, 0, false};
    o = run(&epochs, dir);
    CHECK(o.sent.epochs >= 10, "epochs: %llu begun", (unsigned long long)o.sent.epochs);
    Case forgetful = {"forgetful", 3000, 409, 5, 3, 20, 37, 0, 800, 0, 0, 0, false, 0, true, 0, false};
    o = run(&forgetful, dir);
    CHECK(o.sent.epochs >= 2, "forgetful: %llu epochs begun (none again)", (unsigned long long)o.sent.epochs);
    /* The path cut for 3 s the moment the sender begins an epoch, three
     * times: the receiver never saw the epoch used, and the sender, hearing
     * nothing, offers another epoch 1 with a new key. The accept of the old
     * offer must not begin it. */
    Case cut = {"cut", 3000, 919, 0, 0, 20, 37, 0, 0, 0, 0, 0, false, 200, false, 3, false};
    o = run(&cut, dir);
    CHECK(o.sent.epochs >= 4 && o.sent.fallbacks == 3, "cut: %llu epochs begun, %llu taken for lost (3 cuts)",
          (unsigned long long)o.sent.epochs, (unsigned long long)o.sent.fallbacks);
    /* A path that carried 1,452-byte packets and then, from the moment the
     * link found it did, carries none larger than 1,200 (a route changed,
     * and no ICMP says so): the link must see it and go back. */
    Case hole = {"black hole", 3000, 1601, 0, 0, 0, 0, 0, 0, 0, 0, 0, false, 0, false, 0, true};
    o = run(&hole, dir);
    CHECK(o.hole && o.datagram == PURE_LINK_DATAGRAM, "black hole: %s, a datagram of %zu at the end",
          o.hole ? "opened" : "never opened", o.datagram);
    impostors(dir);
    flood(dir);
    outputs(dir);
    refusals(dir);
    largest(dir);
    char command[600];
    snprintf(command, sizeof(command), "rm -rf %s", dir);
    if (system(command)) { /* left behind in /tmp */ }
    if (failures) fprintf(stderr, "test-link: %d failed\n", failures);
    return failures ? 1 : 0;
}
