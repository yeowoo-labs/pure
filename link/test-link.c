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
 * keys made). Under all of it no key and nonce seals twice. */
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
    uint64_t attacker_in, attacker_out;
    unsigned loss, doubled, spread_ms; /* percent, percent, the most a packet is held back */
    uint64_t now, sent;
    /* The receiving app: its last heard (kept across the receiver's
     * restarts), what it said of deliveries (given at the next turn). */
    uint64_t heard, applied, deliveries;
    PureLinkDone done[64], told[64];
    size_t done_count;
    unsigned fail_every; /* a delivery's save fails every so many (0: never) */
} World;

static void put_packet(World *w, bool to_receiver, const unsigned char *bytes, size_t size) {
    ++w->sent;
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
    (void)context;
    return !strcmp(name, "order") && shape == 0x5eed ? 3 : -1;
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
} Case;

static bool nothing_sent(void *context, const unsigned char *packet, size_t size, const unsigned char *to, size_t to_size) {
    (void)packet, (void)size, (void)to, (void)to_size;
    ++*(unsigned *)context;
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
        unsigned answers = 0;
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
        CHECK(after.received == before.received && !answers, "a packet changed on the way (%d, to the %s) %s", change,
              p->to_receiver ? "receiver" : "sender", answers ? "made an answer" : "opened");
    }
}

/* A packet of the past, again: to the receiver from the sender's address or
 * the attacker's, or to the sender. */
static void replay(World *w, PureLinkSender *s, PureLinkReceiver *r, const char *from) {
    const Packet *h = &w->history[next_random() % (w->history_count < HISTORY ? w->history_count : HISTORY)];
    const char *only = getenv("TEST_LINK_REPLAY");
    if (only && strcmp(only, h->to_receiver ? (chance(50) ? "receiver-sender" : "receiver-attacker") : "sender")) return;
    if (!h->to_receiver) pure_link_sender_take(s, h->bytes, h->size, w->now);
    else if (only ? !strcmp(only, "receiver-sender") : chance(50)) pure_link_receiver_take(r, h->bytes, h->size, from, 7, w->now);
    else {
        w->attacker_in += h->size;
        pure_link_receiver_take(r, h->bytes, h->size, "attacker", 8, w->now);
    }
}

typedef struct { PureLinkCounts sent, heard; uint64_t packets, seconds_ms; } Outcome;

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
        !pure_link_remember(r, memo, error, sizeof(error))) {
        fprintf(stderr, "test-link: %s\n", error);
        exit(1);
    }
    if (idle_ms) pure_link_idle(r, idle_ms);
    return r;
}

/* The sender as its app makes it: the sends its outbox keeps, from the one
 * after the last stored, up to limit. */
static PureLinkSender *sender_of(const PureSealKey *a, const unsigned char *public_a, const unsigned char *public_b,
                                 uint64_t from, uint64_t limit) {
    PureLinkSender *s = pure_link_sender("test.ordered", "order", 0x5eed, a, public_a);
    if (!s || !pure_link_sender_peer(s, public_b)) {
        fprintf(stderr, "test-link: cannot make a sender\n");
        exit(1);
    }
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
               .history = malloc(HISTORY * sizeof(Packet))};
    if (!w.packets || !w.history) exit(1);
    char from[8] = "sender0";
    seed = c->seed;
    uint64_t queued = c->pause_after ? c->pause_after : c->records, resumed = 0;
    PureLinkSender *s = sender_of(a, public_a, public_b, 1, queued);
    PureLinkReceiver *r = receiver_of(&w, b, public_b, public_a, memo, c->idle_ms);
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
            put_packet(&w, true, out, size);
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
            if (w.tamper) tamper(&w, s, r, p, from);
            if (p->to_receiver) pure_link_receiver_take(r, p->bytes, p->size, from, 7, w.now);
            else pure_link_sender_take(s, p->bytes, p->size, w.now);
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
            r = receiver_of(&w, b, public_b, public_a, memo, c->idle_ms);
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
    Outcome o = {.packets = w.sent, .seconds_ms = w.now / MS};
    pure_link_sender_counts(s, &o.sent);
    pure_link_receiver_counts(r, &o.heard);
    o.sent.packets += sent_before.packets, o.sent.resent += sent_before.resent;
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
    printf("{\"case\":\"%s\",\"records\":%llu,\"virtual_ms\":%llu,\"packets\":%llu,\"resent\":%llu,\"lost\":%llu,"
           "\"deliveries\":%llu,\"woken\":%llu,\"remembered\":%llu,\"swept\":%llu}\n",
           c->name, (unsigned long long)c->records, (unsigned long long)o.seconds_ms, (unsigned long long)o.sent.packets,
           (unsigned long long)o.sent.resent, (unsigned long long)o.sent.lost, (unsigned long long)w.deliveries,
           (unsigned long long)o.heard.woken, (unsigned long long)o.heard.remembered, (unsigned long long)o.heard.swept);
    pure_link_sender_free(s);
    pure_link_receiver_free(r);
    pure_seal_key_free(a);
    pure_seal_key_free(b);
    free(w.packets);
    free(w.history);
    free(w.nonces);
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
        packet[0] = 0x70;
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
    CHECK(flooded.keyed <= 300 && flooded.secrets <= 1 && !w.attacker_out, /* 256 at once, 256 a second after */
          "flood: %llu runs keyed, %llu secrets made, %llu bytes to the attacker", (unsigned long long)flooded.keyed,
          (unsigned long long)flooded.secrets, (unsigned long long)w.attacker_out);
    CHECK(w.heard == 200, "flood: the sender heard after it: %llu of 200", (unsigned long long)w.heard);
    if (getenv("TEST_LINK_DEBUG")) {
        PureLinkCounts rc, sc;
        pure_link_receiver_counts(r, &rc), pure_link_sender_counts(s, &sc);
        fprintf(stderr, "receiver received %llu woken %llu deliveries %llu keyed %llu active %llu; sender packets %llu received %llu stored %llu\n",
                (unsigned long long)rc.received, (unsigned long long)rc.woken, (unsigned long long)rc.deliveries,
                (unsigned long long)rc.keyed, (unsigned long long)rc.active, (unsigned long long)sc.packets,
                (unsigned long long)sc.received, (unsigned long long)sc.stored);
    }
    printf("{\"case\":\"flood\",\"packets\":10000,\"keyed\":%llu,\"secrets\":%llu,\"heard_after\":%llu}\n",
           (unsigned long long)flooded.keyed, (unsigned long long)flooded.secrets, (unsigned long long)w.heard);
    pure_link_sender_free(s);
    pure_link_receiver_free(r);
    pure_seal_key_free(a), pure_seal_key_free(b);
    free(w.packets);
}

int main(void) {
    char dir[] = "/tmp/pure-test-link-XXXXXX", error[256];
    if (!mkdtemp(dir) || !pure_seal_ready(error, sizeof(error))) {
        fprintf(stderr, "test-link: %s\n", error);
        return 1;
    }
    /* A clean network: nothing is sent twice. */
    Case clean = {"clean", 3000, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, false};
    Outcome o = run(&clean, dir);
    CHECK(o.sent.resent == 0 && o.sent.lost == 0, "clean: %llu sent again, %llu lost", (unsigned long long)o.sent.resent,
          (unsigned long long)o.sent.lost);
    /* Loss, packets out of order and doubled, failed saves, a pause long
     * enough for the receiver to let the sender's state go, and both sides
     * starting again: the receiver remembers its secret, so it wakes the
     * sender without an X25519. */
    for (uint64_t s = 1; s <= 4; ++s) {
        Case rough = {"rough", 3000, s * 7919, 10, 3, 20, 37, 1500, 800, 2200, 300, 0, false};
        o = run(&rough, dir);
        CHECK(o.sent.resent > 0 && o.heard.swept >= 1 && o.heard.woken >= 1 && o.heard.remembered >= 1,
              "rough: sent again %llu, swept %llu, woken %llu, remembered %llu", (unsigned long long)o.sent.resent,
              (unsigned long long)o.heard.swept, (unsigned long long)o.heard.woken,
              (unsigned long long)o.heard.remembered);
    }
    /* An attacker on the path: packets changed on the way, and packets of
     * the past sent again, from the sender's address and its own, across the
     * same restarts and pause. */
    Case changed = {"changed", 1500, 31, 3, 0, 5, 0, 0, 0, 0, 0, 0, true};
    run(&changed, dir);
    for (uint64_t s = 1; s <= 2; ++s) {
        Case again = {"replayed", 3000, s * 104729, 5, 3, 20, 37, 1500, 800, 2200, 300, 25, false};
        run(&again, dir);
    }
    impostors(dir);
    flood(dir);
    char command[600];
    snprintf(command, sizeof(command), "rm -rf %s", dir);
    if (system(command)) { /* left behind in /tmp */ }
    if (failures) fprintf(stderr, "test-link: %d failed\n", failures);
    return failures ? 1 : 0;
}
