#define _GNU_SOURCE
/* A link's two sides (link/link-protocol.h) through a network of the test's
 * own: time is the test's, and packets are lost, held back (so they come out
 * of order) and doubled at random, from a seed. The receiving host applies a
 * record once, from the one after its last heard, and says so a turn later,
 * now and then failing a save; the receiver and the sender start again
 * midway, each from what its app kept. Every record must be applied once,
 * in order, as it was sent. */
#include "link-protocol.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MS UINT64_C(1000000)
enum { PACKETS = 1 << 16 };

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

typedef struct {
    /* The network: packets on their way, each arriving at its time. */
    Packet *packets;
    size_t count;
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

static bool answer(void *context, const unsigned char *packet, size_t size, const unsigned char *to, size_t to_size) {
    CHECK(to_size == 6 && !memcmp(to, "sender", 6), "an answer to an address no packet came from");
    put_packet(context, false, packet, size);
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
} Case;

typedef struct { PureLinkCounts sent, heard; uint64_t packets, seconds_ms; } Outcome;

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
               .spread_ms = c->spread_ms, .fail_every = c->fail_every};
    if (!w.packets) exit(1);
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
        for (size_t size; (size = pure_link_sender_packet(s, out, false, w.now, &number));)
            put_packet(&w, true, out, size);
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
            if (p->to_receiver) pure_link_receiver_take(r, p->bytes, p->size, "sender", 6, w.now);
            else pure_link_sender_take(s, p->bytes, p->size, w.now);
            *p = w.packets[--w.count];
        }
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
    return o;
}

int main(void) {
    char dir[] = "/tmp/pure-test-link-XXXXXX", error[256];
    if (!mkdtemp(dir) || !pure_seal_ready(error, sizeof(error))) {
        fprintf(stderr, "test-link: %s\n", error);
        return 1;
    }
    /* A clean network: nothing is sent twice. */
    Case clean = {"clean", 3000, 1, 0, 0, 0, 0, 0, 0, 0, 0};
    Outcome o = run(&clean, dir);
    CHECK(o.sent.resent == 0 && o.sent.lost == 0, "clean: %llu sent again, %llu lost", (unsigned long long)o.sent.resent,
          (unsigned long long)o.sent.lost);
    /* Loss, packets out of order and doubled, failed saves, a pause long
     * enough for the receiver to let the sender's state go, and both sides
     * starting again: the receiver remembers its secret, so it wakes the
     * sender without an X25519. */
    for (uint64_t s = 1; s <= 4; ++s) {
        Case rough = {"rough", 3000, s * 7919, 10, 3, 20, 37, 1500, 800, 2200, 300};
        o = run(&rough, dir);
        CHECK(o.sent.resent > 0 && o.heard.swept >= 1 && o.heard.woken >= 1 && o.heard.remembered >= 1,
              "rough: sent again %llu, swept %llu, woken %llu, remembered %llu", (unsigned long long)o.sent.resent,
              (unsigned long long)o.heard.swept, (unsigned long long)o.heard.woken,
              (unsigned long long)o.heard.remembered);
    }
    char command[600];
    snprintf(command, sizeof(command), "rm -rf %s", dir);
    if (system(command)) { /* left behind in /tmp */ }
    if (failures) fprintf(stderr, "test-link: %d failed\n", failures);
    return failures ? 1 : 0;
}
