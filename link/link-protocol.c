#define _GNU_SOURCE
/* Links without connections as state alone (link/link-protocol.h,
 * docs/design/011-pure-transport.md). */
#include "link-protocol.h"
#include <fcntl.h>
#include <malloc.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>

enum { VERSION = 0x70, HEADER = 21, DATAGRAM = PURE_LINK_DATAGRAM, BODY = DATAGRAM - HEADER - PURE_AEAD_TAG };
enum { DATAGRAM_MOST = PURE_LINK_DATAGRAM_MOST, BODY_MOST = DATAGRAM_MOST - HEADER - PURE_AEAD_TAG };
/* A record that does not fit what is left of a packet goes in part, when
 * at least this much of it fits. */
enum { PART_LEAST = 64 };
enum { F_PADDING = 0, F_STREAM = 1, F_RECORD = 2, F_ACK = 3, F_STORED = 4, F_PING = 5, F_RECORDS = 6, F_CHALLENGE = 7,
       F_RESPONSE = 8, STORED_GAP = 1 };
enum { RECORDS_HEAD = 10 }; /* type, the first number, the count */
enum { RANGES = 32, RUNS = 2, UNITS = 64, CREDIT = 1024, BATCH = 256, RECORD_HEAD = 19 };
enum { WINDOW_FIRST = 10 * DATAGRAM, WINDOW_LEAST = 2 * DATAGRAM };
/* Reordering awaited (see PureLinkSender): at first and at most; congestion
 * events with no late packet before the patience halves. */
enum { REORDER_FIRST = 3, REORDER_PACKETS_MOST = 256, REORDER_QUARTERS_MOST = 8, CALM = 16 };
/* A probe of a larger datagram lost is tried again a second on, three
 * times, then ten minutes on. */
enum { REACH_TRIES = 3 };
/* Challenges a sender keeps to say back, the newest first. */
enum { RESPONSES = 4 };
enum { KEYINGS = 256 };
/* CUBIC's constants (RFC 9438): the cubic's scale, and the window kept at a
 * congestion event. */
static const double CUBIC_C = 0.4, CUBIC_BETA = 0.7, RENO_PACE = 3 * (1 - 0.7) / (1 + 0.7);
#define MS UINT64_C(1000000)
/* RFC 9002's values, but a first RTT of 100 ms (a LAN's, not the 333 ms of
 * the Internet at large); a probe at least every second while a receiver is
 * away; a record received but not stored after a second is sent again. */
static const uint64_t FIRST_RTT = 100 * MS, ACK_DELAY_MOST = 25 * MS, GRANULARITY = 1 * MS, PTO_MOST = 1000 * MS,
                      STORED_WAIT = 1000 * MS, RECORD_MOST = UINT64_C(64) << 20, HELD_MOST = UINT64_C(16) << 20,
                      REACH_AGAIN = 1000 * MS, REACH_RARELY = 600000 * MS;

static void put16(unsigned char *p, uint32_t v) { p[0] = (unsigned char)v, p[1] = (unsigned char)(v >> 8); }
static void put32(unsigned char *p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = (unsigned char)(v >> 8 * i); }
static void put64(unsigned char *p, uint64_t v) { for (int i = 0; i < 8; ++i) p[i] = (unsigned char)(v >> 8 * i); }
static uint32_t get16(const unsigned char *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t get32(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t get64(const unsigned char *p) { return (uint64_t)get32(p) | (uint64_t)get32(p + 4) << 32; }
/* LEB128: seven bits a byte, low first. */
static size_t put_varint(unsigned char *p, uint64_t v) {
    size_t n = 0;
    for (; v >= 0x80; v >>= 7) p[n++] = (unsigned char)(v | 0x80);
    p[n++] = (unsigned char)v;
    return n;
}
static size_t varint_size(uint64_t v) {
    size_t n = 1;
    for (; v >= 0x80; v >>= 7) ++n;
    return n;
}
/* A varint from p (size bytes): how many bytes it took, or 0. */
static size_t get_varint(const unsigned char *p, size_t size, uint64_t *v) {
    *v = 0;
    for (size_t n = 0; n < size && n < 10; ++n) {
        *v |= (uint64_t)(p[n] & 0x7f) << (7 * n);
        if (!(p[n] & 0x80)) return n + 1;
    }
    return 0;
}

static uint64_t least(uint64_t a, uint64_t b) { return a < b ? a : b; }
static uint64_t most(uint64_t a, uint64_t b) { return a > b ? a : b; }

static uint32_t fnv(uint32_t h, const void *bytes, size_t size) {
    const unsigned char *p = bytes;
    for (size_t i = 0; i < size; ++i) h = (h ^ p[i]) * 16777619u;
    return h;
}

static uint32_t key_id(const unsigned char key[PURE_SEAL_KEY]) { return fnv(2166136261u, key, PURE_SEAL_KEY); }

static uint64_t random64(void) {
    uint64_t value = 0;
    while (getrandom(&value, sizeof(value), 0) != sizeof(value) || !value) {}
    return value;
}

/* The key and nonce base of packets from `from` (in its run from_run) to
 * `to`, answering to_run (0 for none). */
static bool derive(const unsigned char secret[32], const unsigned char from[PURE_SEAL_KEY],
                   const unsigned char to[PURE_SEAL_KEY], uint64_t from_run, uint64_t to_run,
                   unsigned char key[PURE_AEAD_KEY], unsigned char base[PURE_AEAD_NONCE]) {
    unsigned char info[16 + 2 * PURE_SEAL_KEY + 16], out[PURE_AEAD_KEY + PURE_AEAD_NONCE];
    memcpy(info, "pure link 0 keys", 16);
    memcpy(info + 16, from, PURE_SEAL_KEY);
    memcpy(info + 16 + PURE_SEAL_KEY, to, PURE_SEAL_KEY);
    put64(info + 16 + 2 * PURE_SEAL_KEY, from_run);
    put64(info + 24 + 2 * PURE_SEAL_KEY, to_run);
    if (!pure_link_derive(secret, info, sizeof(info), out, sizeof(out))) return false;
    memcpy(key, out, PURE_AEAD_KEY);
    memcpy(base, out + PURE_AEAD_KEY, PURE_AEAD_NONCE);
    explicit_bzero(out, sizeof(out));
    return true;
}

/* The same as a context of its own, for a sender's one link. */
static PureAead *aead_of(const unsigned char secret[32], const unsigned char from[PURE_SEAL_KEY],
                         const unsigned char to[PURE_SEAL_KEY], uint64_t from_run, uint64_t to_run,
                         unsigned char base[PURE_AEAD_NONCE]) {
    unsigned char key[PURE_AEAD_KEY];
    PureAead *aead = derive(secret, from, to, from_run, to_run, key, base) ? pure_aead(key) : NULL;
    explicit_bzero(key, sizeof(key));
    return aead;
}

static void nonce_of(unsigned char out[PURE_AEAD_NONCE], const unsigned char base[PURE_AEAD_NONCE], uint64_t pn) {
    memcpy(out, base, PURE_AEAD_NONCE);
    for (int i = 0; i < 8; ++i) out[4 + i] ^= (unsigned char)(pn >> (56 - 8 * i));
}

/* A packet: the header, then body sealed (with key, or the context's own
 * when NULL); its size, or 0. */
static size_t seal_packet(PureAead *aead, const unsigned char *key, const unsigned char base[PURE_AEAD_NONCE], uint32_t id,
                          uint64_t run, uint64_t pn, const unsigned char *body, size_t size, unsigned char *out) {
    unsigned char nonce[PURE_AEAD_NONCE];
    out[0] = VERSION;
    put32(out + 1, id);
    put64(out + 5, run);
    put64(out + 13, pn);
    nonce_of(nonce, base, pn);
    return pure_aead_seal(aead, key, nonce, out, HEADER, body, size, out + HEADER) ? HEADER + size + PURE_AEAD_TAG : 0;
}

static size_t put_names(unsigned char *p, const char *source, const char *input) {
    size_t a = strlen(source), b = strlen(input);
    p[0] = (unsigned char)a;
    memcpy(p + 1, source, a);
    p[1 + a] = (unsigned char)b;
    memcpy(p + 2 + a, input, b);
    return 2 + a + b;
}

/* A source and an input's names from p (size bytes); how many it takes, or 0. */
static size_t get_names(const unsigned char *p, size_t size, char source[PURE_LINK_NAME + 1],
                        char input[PURE_LINK_NAME + 1]) {
    if (size < 1 || !p[0] || p[0] > PURE_LINK_NAME || size < 2u + p[0]) return 0;
    size_t a = p[0], b = p[1 + a];
    if (!b || b > PURE_LINK_NAME || size < 2 + a + b) return 0;
    memcpy(source, p + 1, a);
    source[a] = 0;
    memcpy(input, p + 2 + a, b);
    input[b] = 0;
    return 2 + a + b;
}

/* ---- The sending side */

enum { UNSENT, FLYING, RECEIVED };
/* Part of a record's bytes, gone whole in one packet (numbered pn): a
 * record is one part until a packet has room for only some of it. */
typedef struct { uint64_t pn; uint32_t offset, length; uint8_t state; } Part;
/* A record queued: its bytes at `at` in the link's byte ring, its parts in
 * order (the one here, or all of them beside, room for room). */
typedef struct { uint64_t seq; size_t at; uint32_t size, parts, room; Part *more, one; } Record;
static Part *part_of(Record *r, uint32_t i) { return r->more ? &r->more[i] : &r->one; }
/* What a packet carried of a record: its bytes from offset, whole parts. */
typedef struct { uint64_t seq; uint32_t offset, length; } Unit;
typedef struct {
    uint64_t pn, sent; /* its number; when it went (ns) */
    uint64_t largest_then; /* lost: the largest number acknowledged when it was called lost */
    uint64_t episode;      /* lost: the congestion episode it counted in */
    uint32_t bytes;
    bool eliciting, done; /* acknowledged or lost */
    bool reach;           /* a probe of a larger datagram */
    bool lost, by_time;   /* called lost (by time, or by later ones acknowledged), and not acknowledged since */
    uint16_t count;
    Unit units[UNITS];
} Flight;

struct PureLinkSender {
    uint32_t shape, own_id, peer_id;
    char input[PURE_LINK_NAME + 1], source[PURE_LINK_NAME + 1];
    const PureSealKey *own;
    unsigned char own_public[PURE_SEAL_KEY], peer[PURE_SEAL_KEY], secret[32];
    Record *records; /* the sends not yet stored, oldest first (a ring of consecutive numbers) */
    size_t head, count, capacity;
    /* Their bytes, one after another as queued (a record's never cut by the
     * ring's end): taken back in order as they are stored. */
    unsigned char *ring;
    size_t ring_capacity;
    uint64_t queued, stored; /* the last queued; saved by the receiver */
    uint64_t run, next_pn, in_run;
    PureAead *out, *in; /* made once the receiver's key is known */
    unsigned char out_base[PURE_AEAD_NONCE], in_base[PURE_AEAD_NONCE];
    Flight *flights; /* one for each number sent, oldest first (a ring) */
    size_t flight_head, flight_count, flight_capacity;
    uint64_t in_flight, window, threshold, recovery_start, eliciting;
    uint64_t largest_acked, latest_rtt, smoothed_rtt, rtt_var, min_rtt;
    bool acked_any, sampled;
    /* limit: the highest number the receiver takes (the last it stored and
     * its credit past that, the most it said: a word of the past sent again
     * lowers nothing; 0 before it said any). Challenges to say back in the
     * next packet (the receiver checks the path so): the last few, as an
     * answer of the past sent again may bring an old one. */
    uint64_t loss_time, last_eliciting, limit, stored_changed;
    uint64_t respond[RESPONSES];
    unsigned responding;
    /* Reordering: later packets acknowledged before a packet is lost (3 at
     * first), and the time past the RTT, in quarters of the least RTT (0:
     * RFC 9002's eighth of the RTT); both grow when a packet called lost
     * turns out late. Congestion events one after another while losses of
     * the first are still unsettled make an episode; one whose losses all
     * turn out late is undone. Its window, threshold and recovery before
     * it; its losses unsettled; whether none of them was real. */
    uint64_t reorder_packets, reorder_quarters, calm_events;
    uint64_t episode, undo_window, undo_threshold, undo_recovery, undo_losses;
    bool undo_ok;
    /* Congestion avoidance is CUBIC (RFC 9438), in datagrams and seconds:
     * the window before the last event, the time the cubic takes to reach
     * it again, when the epoch began (0: none yet), and what Reno would have
     * (the window never grows slower). As they were before an episode, for
     * its undoing. */
    double cubic_max, cubic_k, cubic_reno;
    uint64_t cubic_epoch;
    double undo_cubic_max, undo_cubic_k, undo_cubic_reno;
    uint64_t undo_cubic_epoch;
    /* The window went unused (nothing to send, or no credit) when the
     * sender last looked: it does not grow then (RFC 9002 §7.8). */
    bool unused;
    /* Pacing (RFC 9002 §7.7): bytes it may send now, filled as time goes
     * at the window an RTT (a quarter more; twice in slow start), up to 2 ms
     * of it (10 to 256 datagrams); when that was last done; when there will
     * be enough for the next packet, while it waits for that. */
    double pace_bytes;
    uint64_t pace_at, pace_due;
    /* The datagram's size now; a probe of the largest flies; when the next
     * may go, and how many were lost (RFC 8899). */
    uint32_t datagram;
    bool reaching, filled; /* filled: a packet was full with more to go (a larger one would carry more) */
    uint64_t reach_at;
    unsigned reach_lost;
    uint64_t received;    /* parts received and not yet stored */
    uint64_t unsent_from; /* no record before this number has a part unsent */
    unsigned pto_count, probes;
    PureLinkCounts counts;
};

static Record *record_at(PureLinkSender *l, size_t k) { return &l->records[(l->head + k) % l->capacity]; }

/* The queued record numbered seq, or NULL (stored already, or not queued). */
static Record *record_of(PureLinkSender *l, uint64_t seq) {
    if (!l->count || seq < record_at(l, 0)->seq || seq - record_at(l, 0)->seq >= l->count) return NULL;
    return record_at(l, (size_t)(seq - record_at(l, 0)->seq));
}

/* Every change of a part's state goes through here: the link knows at once
 * how many parts wait to be stored, and where the unsent ones begin. */
static void set_part(PureLinkSender *l, Record *r, Part *p, uint8_t state) {
    if (p->state == state) return;
    if (p->state == RECEIVED) --l->received;
    if (state == RECEIVED) ++l->received;
    if (state == UNSENT && r->seq < l->unsent_from) l->unsent_from = r->seq;
    p->state = state;
}

/* Part i of r cut after length bytes: the rest is part i + 1, as it
 * stood. Cut only before it goes, so what a packet carried is whole parts,
 * and cuts stay. False when memory runs out. */
static bool cut(Record *r, uint32_t i, uint32_t length) {
    if (!r->more) {
        if (!(r->more = malloc(4 * sizeof(Part)))) return false;
        r->more[0] = r->one;
        r->room = 4;
    } else if (r->parts == r->room) {
        Part *grown = realloc(r->more, 2 * (size_t)r->room * sizeof(Part));
        if (!grown) return false;
        r->more = grown;
        r->room *= 2;
    }
    memmove(&r->more[i + 2], &r->more[i + 1], (r->parts - i - 1) * sizeof(Part));
    r->more[i + 1] = r->more[i];
    r->more[i + 1].offset += length;
    r->more[i + 1].length -= length;
    r->more[i].length = length;
    ++r->parts;
    return true;
}

/* The first part of r at or after offset. */
static uint32_t part_at(Record *r, uint32_t offset) {
    uint32_t low = 0, high = r->parts;
    while (low < high) {
        uint32_t middle = low + (high - low) / 2;
        if (part_of(r, middle)->offset < offset) low = middle + 1;
        else high = middle;
    }
    return low;
}

/* What became of the parts unit u carried in packet pn: acknowledged
 * (received, whatever went since), or not (the ones still flying in pn are
 * unsent again). How many went unsent. */
static unsigned carried(PureLinkSender *l, const Unit *u, uint64_t pn, bool acknowledged, uint64_t now) {
    Record *r = record_of(l, u->seq);
    unsigned unsent = 0;
    for (uint32_t i = r ? part_at(r, u->offset) : 0; r && i < r->parts; ++i) {
        Part *p = part_of(r, i);
        if (p->offset >= u->offset + u->length && u->length) break;
        if (acknowledged && p->state != RECEIVED) {
            set_part(l, r, p, RECEIVED);
            l->stored_changed = now; /* waits for its save from now */
        } else if (!acknowledged && p->state == FLYING && p->pn == pn) {
            set_part(l, r, p, UNSENT);
            ++unsent;
        }
        if (!u->length) break; /* an empty record's one part */
    }
    return unsent;
}

/* Where size bytes go at the end of the byte ring (after the newest record,
 * or from its start when that is free and the end is not), or SIZE_MAX. */
static size_t ring_place(PureLinkSender *l, size_t size) {
    if (!l->count) return size <= l->ring_capacity ? 0 : SIZE_MAX;
    Record *oldest = record_at(l, 0), *newest = record_at(l, l->count - 1);
    size_t head = oldest->at, tail = newest->at + newest->size;
    if (newest->at >= head) /* the bytes are [head, tail): after them, or before them from the start */
        return l->ring_capacity - tail >= size ? tail : size <= head ? 0 : SIZE_MAX;
    return head - tail >= size ? tail : SIZE_MAX; /* around the end: between the newest and the oldest */
}

/* A ring room for size bytes more: twice as large (or more), the records
 * moved into it one after another. */
static bool ring_grow(PureLinkSender *l, size_t size) {
    size_t used = 0;
    for (size_t k = 0; k < l->count; ++k) used += record_at(l, k)->size;
    size_t capacity = l->ring_capacity ? 2 * l->ring_capacity : 4 << 10;
    while (capacity < used + size + 1) capacity *= 2;
    unsigned char *ring = malloc(capacity);
    if (!ring) return false;
    for (size_t k = 0, at = 0; k < l->count; ++k) {
        Record *r = record_at(l, k);
        if (r->size) memcpy(ring + at, l->ring + r->at, r->size);
        r->at = at;
        at += r->size;
    }
    free(l->ring);
    l->ring = ring;
    l->ring_capacity = capacity;
    return true;
}

bool pure_link_sender_queue(PureLinkSender *l, uint64_t seq, const unsigned char *bytes, size_t size) {
    Record r = {.seq = seq, .size = (uint32_t)size, .parts = 1, .one = {.length = (uint32_t)size}};
    if (size > RECORD_MOST) return false;
    bool ok = seq > l->queued && (!l->count || seq == l->queued + 1);
    size_t at = ok ? ring_place(l, size) : SIZE_MAX;
    if (ok && at == SIZE_MAX) ok = ring_grow(l, size) && (at = ring_place(l, size)) != SIZE_MAX;
    if (ok) {
        r.at = at;
        if (size) memcpy(l->ring + at, bytes, size);
    }
    if (ok && l->count == l->capacity) {
        size_t capacity = l->capacity ? 2 * l->capacity : 16;
        Record *grown = malloc(capacity * sizeof(*grown));
        if (grown) {
            for (size_t k = 0; k < l->count; ++k) grown[k] = *record_at(l, k);
            free(l->records);
            l->records = grown;
            l->capacity = capacity;
            l->head = 0;
        } else
            ok = false;
    }
    if (!ok) return false;
    *record_at(l, l->count++) = r;
    l->queued = seq;
    return true;
}

static Flight *flight_of(PureLinkSender *l, uint64_t pn) {
    if (!l->flight_count) return NULL;
    uint64_t first = l->flights[l->flight_head].pn;
    if (pn < first || pn - first >= l->flight_count) return NULL;
    return &l->flights[(l->flight_head + (size_t)(pn - first)) % l->flight_capacity];
}

/* Room for one more flight: made before a packet is, so every number sent
 * has its flight (they are found by number). */
static bool flight_room(PureLinkSender *l) {
    if (l->flight_count < l->flight_capacity) return true;
    size_t capacity = l->flight_capacity ? 2 * l->flight_capacity : 8;
    Flight *grown = malloc(capacity * sizeof(*grown));
    if (!grown) return false;
    for (size_t k = 0; k < l->flight_count; ++k) grown[k] = l->flights[(l->flight_head + k) % l->flight_capacity];
    free(l->flights);
    l->flights = grown;
    l->flight_capacity = capacity;
    l->flight_head = 0;
    return true;
}

static void push_flight(PureLinkSender *l, const Flight *f) {
    l->flights[(l->flight_head + l->flight_count++) % l->flight_capacity] = *f;
}

static void settle(PureLinkSender *l, Flight *f) {
    f->done = true;
    if (f->eliciting) {
        --l->eliciting;
        l->in_flight -= f->bytes;
    }
}

/* A packet lost: what it carried and was not received otherwise goes again. */
static void lose(PureLinkSender *l, Flight *f, uint64_t now, bool by_time) {
    settle(l, f);
    if (f->reach) { /* the path does not carry it (yet): tried again a while on, three times, then rarely */
        l->reaching = false;
        l->reach_at = now + (++l->reach_lost < REACH_TRIES ? REACH_AGAIN : REACH_RARELY);
        return;
    }
    f->lost = true;
    f->by_time = by_time;
    f->largest_then = l->largest_acked;
    ++l->counts.lost;
    for (uint16_t u = 0; u < f->count; ++u) l->counts.resent += carried(l, &f->units[u], f->pn, false, now);
    if (f->sent > l->recovery_start) { /* one congestion event per round trip */
        if (!l->undo_losses) { /* an episode begins */
            ++l->episode;
            l->undo_window = l->window;
            l->undo_threshold = l->threshold;
            l->undo_recovery = l->recovery_start;
            l->undo_cubic_max = l->cubic_max, l->undo_cubic_k = l->cubic_k, l->undo_cubic_reno = l->cubic_reno;
            l->undo_cubic_epoch = l->cubic_epoch;
            l->undo_ok = true;
        }
        l->recovery_start = now;
        double w = (double)l->window / l->datagram;
        l->cubic_max = w < l->cubic_max ? w * (1 + CUBIC_BETA) / 2 : w; /* fast convergence: another flow came */
        l->window = most((uint64_t)((double)l->window * CUBIC_BETA), WINDOW_LEAST);
        l->threshold = l->window;
        l->cubic_k = cbrt(l->cubic_max * (1 - CUBIC_BETA) / CUBIC_C);
        l->cubic_epoch = now;
        l->cubic_reno = (double)l->window / l->datagram;
        if (++l->calm_events >= CALM) { /* many events, none of them late packets: less patience */
            l->reorder_packets = most(REORDER_FIRST, l->reorder_packets / 2);
            l->reorder_quarters /= 2;
            l->calm_events = 0;
        }
    }
    f->episode = l->episode;
    ++l->undo_losses;
}

/* A loss of the episode settled: it came late, or it was real. */
static void settle_loss(PureLinkSender *l, const Flight *f, bool real) {
    if (f->episode != l->episode || !l->undo_losses) return;
    if (real) l->undo_ok = false;
    if (!--l->undo_losses && l->undo_ok) {
        l->window = most(l->window, l->undo_window);
        l->threshold = l->undo_threshold;
        l->recovery_start = l->undo_recovery;
        l->cubic_max = l->undo_cubic_max, l->cubic_k = l->undo_cubic_k, l->cubic_reno = l->undo_cubic_reno;
        l->cubic_epoch = l->undo_cubic_epoch;
        ++l->counts.undone;
    }
}

/* A packet called lost acknowledged after all: it came late. What it
 * carried was received (not sent again, if it waits to be); later packets
 * are awaited longer; and when every loss of the last congestion event
 * came late, the event is undone. */
static void came_late(PureLinkSender *l, Flight *f, uint64_t now) {
    f->lost = false;
    ++l->counts.late;
    for (uint16_t u = 0; u < f->count; ++u) carried(l, &f->units[u], f->pn, true, now);
    if (f->by_time) l->reorder_quarters = least(l->reorder_quarters + 1, REORDER_QUARTERS_MOST);
    else l->reorder_packets = least(most(l->reorder_packets, l->largest_acked - f->pn + 1), REORDER_PACKETS_MOST);
    l->calm_events = 0;
    settle_loss(l, f, false);
}

/* How long past the RTT a packet may come before it is called lost. */
static uint64_t reorder_time(const PureLinkSender *l, uint64_t rtt) {
    uint64_t wait = rtt / 8;
    if (l->reorder_quarters && l->sampled) wait = most(wait, l->min_rtt / 4 * l->reorder_quarters);
    return least(wait, rtt);
}

static void detect_lost(PureLinkSender *l, uint64_t now) {
    l->loss_time = 0;
    if (!l->acked_any) return;
    uint64_t rtt = most(l->latest_rtt, l->sampled ? l->smoothed_rtt : FIRST_RTT);
    uint64_t delay = most(rtt + reorder_time(l, rtt), GRANULARITY);
    for (size_t k = 0; k < l->flight_count; ++k) {
        Flight *f = &l->flights[(l->flight_head + k) % l->flight_capacity];
        if (f->pn >= l->largest_acked) break;
        if (f->done) continue;
        bool late = f->sent + delay <= now;
        if (late || l->largest_acked >= f->pn + l->reorder_packets) lose(l, f, now, late);
        else if (!l->loss_time || f->sent + delay < l->loss_time) l->loss_time = f->sent + delay;
    }
}

static void sample_rtt(PureLinkSender *l, uint64_t latest, uint64_t ack_delay) {
    l->latest_rtt = latest;
    if (!l->sampled) {
        l->min_rtt = l->smoothed_rtt = latest;
        l->rtt_var = latest / 2;
        l->sampled = true;
        return;
    }
    l->min_rtt = least(l->min_rtt, latest);
    ack_delay = least(ack_delay, ACK_DELAY_MOST);
    uint64_t adjusted = latest >= l->min_rtt + ack_delay ? latest - ack_delay : latest;
    uint64_t difference = l->smoothed_rtt > adjusted ? l->smoothed_rtt - adjusted : adjusted - l->smoothed_rtt;
    l->rtt_var = (3 * l->rtt_var + difference) / 4;
    l->smoothed_rtt = (7 * l->smoothed_rtt + adjusted) / 8;
}

/* The RTT as the window's pace takes it: never under a microsecond (a
 * sample of 0, as on a path within one clock tick, would make it infinite). */
static uint64_t rtt_now(const PureLinkSender *l) { return most(l->sampled ? l->smoothed_rtt : FIRST_RTT, 1000); }

/* The window after bytes acknowledged: slow start below the threshold,
 * then CUBIC. The cubic's target is where it will be an RTT on, and the
 * window closes on it by the share of the window acknowledged. */
static void grow(PureLinkSender *l, uint64_t bytes, uint64_t now) {
    if (l->window < l->threshold) {
        l->window += bytes;
        return;
    }
    double w = (double)l->window / l->datagram, acked = (double)bytes / l->datagram;
    if (!l->cubic_epoch) { /* no event yet: the cubic starts where the window is */
        l->cubic_epoch = now;
        l->cubic_max = l->cubic_reno = w;
        l->cubic_k = 0;
    }
    double t = (double)(now - l->cubic_epoch) / 1e9, rtt = (double)rtt_now(l) / 1e9;
    /* Reno's pace, 0.53 datagrams an RTT as the CUBIC deployed (Linux's,
     * quinn's) keeps it, not RFC 9438's whole datagram once past the window
     * before the event: as the acknowledgments come, or as time goes (RFC
     * 8312), whichever is ahead, so a link its app holds back does not fall
     * behind it. */
    l->cubic_reno += RENO_PACE * acked / w;
    double by_time = l->cubic_max * CUBIC_BETA + RENO_PACE * t / rtt;
    if (by_time > l->cubic_reno) l->cubic_reno = by_time;
    double d = t - l->cubic_k, at = CUBIC_C * d * d * d + l->cubic_max;
    d += rtt;
    double target = CUBIC_C * d * d * d + l->cubic_max;
    target = target < w ? w : target > 1.5 * w ? 1.5 * w : target;
    double next = at < l->cubic_reno ? l->cubic_reno : w + (target - w) * acked / w;
    l->window = most(l->window, (uint64_t)(next * l->datagram));
}

static void on_ack(PureLinkSender *l, const unsigned char *p, size_t size, uint64_t now) {
    if (size < 13 || size < 13 + 16u * p[12]) return;
    uint64_t largest = get64(p), delay = (uint64_t)get32(p + 8) * 1000;
    Flight *top = NULL;
    if (!l->acked_any || largest > l->largest_acked) l->largest_acked = largest;
    for (unsigned k = 0; k < p[12]; ++k) {
        uint64_t first = get64(p + 13 + 16 * k), last = get64(p + 21 + 16 * k);
        if (first > last || !l->flight_count) continue;
        uint64_t oldest = l->flights[l->flight_head].pn, newest = oldest + l->flight_count - 1;
        for (uint64_t pn = most(first, oldest); pn <= least(last, newest); ++pn) {
            Flight *f = flight_of(l, pn);
            if (f && f->lost) came_late(l, f, now);
            if (!f || f->done) continue;
            settle(l, f);
            ++l->counts.acknowledged;
            if (f->reach) l->datagram = DATAGRAM_MOST, l->reaching = false; /* the path carries it */
            for (uint16_t u = 0; u < f->count; ++u) carried(l, &f->units[u], f->pn, true, now);
            if (f->eliciting && f->sent > l->recovery_start && !l->unused) grow(l, f->bytes, now);
            if (pn == largest) top = f;
        }
    }
    l->acked_any = true;
    if (top && top->eliciting) sample_rtt(l, now - top->sent, delay);
    l->pto_count = 0;
    detect_lost(l, now);
    /* Done ones go; one called lost stays a while, so it is known if it
     * comes late. */
    uint64_t keep = 2 * most(l->latest_rtt, l->sampled ? l->smoothed_rtt : FIRST_RTT);
    while (l->flight_count) {
        Flight *f = &l->flights[l->flight_head];
        if (!f->done || (f->lost && f->sent + keep > now)) break;
        if (f->lost) settle_loss(l, f, true); /* not late: lost */
        l->flight_head = (l->flight_head + 1) % l->flight_capacity;
        --l->flight_count;
    }
}

static void on_stored(PureLinkSender *l, const unsigned char *p, size_t size, uint64_t now) {
    char source[PURE_LINK_NAME + 1], input[PURE_LINK_NAME + 1];
    size_t names = get_names(p, size, source, input);
    if (!names || size < names + 13 || strcmp(source, l->source) || strcmp(input, l->input)) return;
    uint64_t heard = get64(p + names);
    unsigned flags = p[names + 8];
    uint32_t credit = get32(p + names + 9);
    if (heard > l->stored) {
        while (l->count && record_at(l, 0)->seq <= heard) {
            Record *r = record_at(l, 0);
            for (uint32_t i = 0; i < r->parts; ++i) l->received -= part_of(r, i)->state == RECEIVED;
            free(r->more); /* its bytes go back with the ring's head */
            l->head = (l->head + 1) % l->capacity;
            --l->count;
        }
        l->stored = heard;
        l->stored_changed = now;
    }
    if ((flags & STORED_GAP) && heard >= l->stored) /* it did not keep what came after a gap: send it again */
        for (size_t k = 0; k < l->count && l->received; ++k) {
            Record *r = record_at(l, k);
            for (uint32_t i = 0; i < r->parts; ++i)
                if (part_of(r, i)->state == RECEIVED) set_part(l, r, part_of(r, i), UNSENT);
        }
    l->limit = most(l->limit, heard + (credit ? least(credit, 65536) : 1));
}

void pure_link_sender_take(PureLinkSender *l, const unsigned char *packet, size_t n, uint64_t now) {
    unsigned char plain[DATAGRAM_MOST], nonce[PURE_AEAD_NONCE];
    if (!l->out || n < HEADER + PURE_AEAD_TAG || n > DATAGRAM_MOST || packet[0] != VERSION || get32(packet + 1) != l->peer_id)
        return; /* not ours, or an error from an earlier send (nobody listening) */
    uint64_t run = get64(packet + 5), pn = get64(packet + 13);
    if (!l->in || l->in_run != run) {
        pure_aead_free(l->in);
        l->in = aead_of(l->secret, l->peer, l->own_public, run, l->run, l->in_base);
        ++l->counts.keyed;
        l->in_run = run;
        if (!l->in) return;
    }
    nonce_of(nonce, l->in_base, pn);
    size_t size = n - HEADER - PURE_AEAD_TAG;
    if (!pure_aead_open(l->in, NULL, nonce, packet, HEADER, packet + HEADER, n - HEADER, plain)) return;
    ++l->counts.received;
    for (size_t at = 0; at < size;) {
        unsigned type = plain[at++];
        if (type == F_ACK && at + 13 <= size && at + 13 + 16u * plain[at + 12] <= size) {
            on_ack(l, plain + at, size - at, now);
            at += 13 + 16u * plain[at + 12];
        } else if (type == F_STORED) {
            char source[PURE_LINK_NAME + 1], input[PURE_LINK_NAME + 1];
            size_t names = get_names(plain + at, size - at, source, input);
            if (!names || at + names + 13 > size) break;
            on_stored(l, plain + at, size - at, now);
            at += names + 13;
        } else if (type == F_CHALLENGE) { /* said back in the next packet */
            if (at + 8 > size) break;
            uint64_t challenge = get64(plain + at);
            unsigned k = 0;
            while (k < l->responding && l->respond[k] != challenge) ++k;
            if (k == l->responding) { /* not had yet: first, the oldest let go */
                memmove(&l->respond[1], &l->respond[0], (RESPONSES - 1) * sizeof(uint64_t));
                l->respond[0] = challenge;
                if (l->responding < RESPONSES) ++l->responding;
            }
            at += 8;
        } else if (type != F_PING)
            break; /* padding, or what this side does not know: the rest */
    }
}

/* A packet of what is not sent yet, in order, within the receiver's credit
 * (or a probe's, which goes all the same): its body's size, or 0 when there
 * is none. */
static size_t fill(PureLinkSender *l, unsigned char *body, Flight *f, bool pad, bool probe) {
    size_t at = 0, most = l->datagram - HEADER - PURE_AEAD_TAG;
    body[at++] = F_STREAM;
    at += put_names(body + at, l->source, l->input);
    put32(body + at, l->shape);
    at += 4;
    /* The receiver's challenges, said back in the next packet that goes,
     * never in one of their own: that would be a packet more for each
     * address checked. Until then the answers there are at most three
     * times what came from it, and the answers to a packet are less; when
     * they are held back, a probe or a record sent again says it back. */
    for (unsigned k = 0; k < l->responding; ++k, at += 9) { /* let go once the packet goes */
        body[at] = F_RESPONSE;
        put64(body + at + 1, l->respond[k]);
    }
    /* The credit counts from the oldest record not stored: after a restart
     * the sender does not know yet how far the receiver stored. */
    uint64_t base = l->count ? record_at(l, 0)->seq - 1 : l->stored;
    /* From the first record that may have a part unsent: the ones before
     * it are sent, and every one found sent moves that mark on. */
    size_t k = l->count && l->unsent_from > base + 1 ? (size_t)(l->unsent_from - base - 1) : 0;
    bool before = true;
    /* A record none of which went yet goes whole when it fits what is left
     * of the packet: whole records whose numbers follow on share a RECORDS
     * frame (its count at run). Else its unsent parts go, each in a RECORD
     * frame, the last cut to what is left (when at least PART_LEAST is). */
    size_t run = 0;
    uint64_t next = 0;
    for (; k < l->count && f->count < UNITS; ++k) {
        Record *r = record_at(l, k);
        if (r->seq > (l->limit ? l->limit : base + CREDIT)) break;
        bool full = false, fresh = !r->more && r->one.state == UNSENT, joins = run && r->seq == next;
        size_t whole = (joins ? 0 : RECORDS_HEAD) + varint_size(r->size) + r->size;
        if (fresh && at + whole <= most) {
            if (!joins) {
                body[at] = F_RECORDS;
                put64(body + at + 1, r->seq);
                body[at + 9] = 0;
                run = at + 9;
                at += RECORDS_HEAD;
            }
            at += put_varint(body + at, r->size);
            if (r->size) memcpy(body + at, l->ring + r->at, r->size);
            at += r->size;
            ++body[run];
            next = r->seq + 1;
            set_part(l, r, &r->one, FLYING);
            r->one.pn = f->pn;
            f->units[f->count++] = (Unit){r->seq, 0, r->size};
            ++l->counts.records;
        } else
            for (uint32_t i = 0; i < r->parts; ++i) {
                Part *p = part_of(r, i);
                if (p->state != UNSENT) continue;
                size_t room = at + RECORD_HEAD < most ? most - at - RECORD_HEAD : 0;
                if (f->count == UNITS || (p->length > room && (room < PART_LEAST || !cut(r, i, (uint32_t)room)))) {
                    full = true;
                    break;
                }
                p = part_of(r, i); /* where it is after a cut */
                body[at] = F_RECORD;
                put64(body + at + 1, r->seq);
                put32(body + at + 9, r->size);
                put32(body + at + 13, p->offset);
                put16(body + at + 17, p->length);
                if (p->length) memcpy(body + at + RECORD_HEAD, l->ring + r->at + p->offset, p->length);
                at += RECORD_HEAD + p->length;
                run = 0;
                set_part(l, r, p, FLYING);
                p->pn = f->pn;
                f->units[f->count++] = (Unit){r->seq, p->offset, p->length};
                ++l->counts.records;
            }
        if (before && !full) l->unsent_from = r->seq + 1;
        before = before && !full;
        if (full) { /* the packet is full: whole, so it can go in a burst */
            l->filled = true;
            if (pad && f->count) {
                memset(body + at, F_PADDING, most - at);
                at = most;
            }
            break;
        }
    }
    if (!f->count && !(probe && l->responding)) return 0;
    l->responding = 0;
    return at;
}

/* Its number is spent all the same. */
void pure_link_sender_refused(PureLinkSender *l, uint64_t pn) {
    Flight *f = flight_of(l, pn);
    if (!f || f->done) return;
    for (uint16_t u = 0; u < f->count; ++u) carried(l, &f->units[u], pn, false, 0);
    settle(l, f);
    f->eliciting = false;
    f->count = 0;
    --l->counts.packets;
}

/* Seal f with body into out, and count it sent: its size, or 0 (what it
 * carried is then unsent again). Its number is spent either way. */
static size_t transmit(PureLinkSender *l, Flight *f, const unsigned char *body, size_t size, unsigned char *out,
                       uint64_t now) {
    size_t total = seal_packet(l->out, NULL, l->out_base, l->own_id, l->run, f->pn, body, size, out);
    ++l->next_pn;
    f->sent = now;
    f->bytes = (uint32_t)total;
    if (!total) { /* not sealed: what it carried is unsent again */
        for (uint16_t u = 0; u < f->count; ++u) carried(l, &f->units[u], f->pn, false, now);
        f->done = true;
        f->eliciting = false;
        f->count = 0;
        push_flight(l, f);
        return 0;
    }
    push_flight(l, f);
    ++l->counts.packets;
    l->pace_bytes = l->pace_bytes > (double)total ? l->pace_bytes - (double)total : 0;
    if (f->eliciting) {
        ++l->eliciting;
        l->in_flight += f->bytes;
        l->last_eliciting = now;
    }
    return total;
}

/* Whether pacing lets a packet go now (else when it will, in pace_due). */
static bool paced(PureLinkSender *l, uint64_t now) {
    double rtt = (double)rtt_now(l);
    double rate = (double)l->window * (l->window < l->threshold ? 2.0 : 1.25) / rtt; /* bytes a ns */
    double most = rate * 2e6, least = 10.0 * l->datagram, top = 256.0 * l->datagram;
    most = most < least ? least : most > top ? top : most;
    l->pace_bytes += rate * (double)(now - l->pace_at);
    if (l->pace_bytes > most) l->pace_bytes = most;
    l->pace_at = now;
    l->pace_due = 0;
    if (l->pace_bytes >= l->datagram) return true;
    l->pace_due = now + (uint64_t)(((double)l->datagram - l->pace_bytes) / rate) + 1;
    return false;
}

size_t pure_link_sender_packet(PureLinkSender *l, unsigned char out[PURE_LINK_DATAGRAM_MOST], bool whole, uint64_t now,
                               uint64_t *number) {
    bool probe = l->probes > 0;
    l->pace_due = 0; /* set again only when pacing holds a packet back */
    if (!probe && l->in_flight + l->datagram > l->window) l->unused = false;
    if (!l->out || (!probe && l->in_flight + l->datagram > l->window) || !flight_room(l)) return 0;
    if (!probe && !paced(l, now)) return 0;
    unsigned char body[BODY_MOST];
    Flight f = {.pn = l->next_pn, .eliciting = true};
    if (l->datagram < DATAGRAM_MOST && l->filled && !l->reaching && l->sampled && now >= l->reach_at &&
        l->in_flight + DATAGRAM_MOST <= l->window) { /* the path may carry more: a PING of the most, padded */
        body[0] = F_PING;
        memset(body + 1, F_PADDING, BODY_MOST - 1);
        f.reach = true;
        size_t total = transmit(l, &f, body, BODY_MOST, out, now);
        if (!total) return 0;
        l->reaching = true;
        *number = f.pn;
        return total;
    }
    size_t size = fill(l, body, &f, whole, probe);
    if (!size) {
        l->unused = true;
        if (!probe) return 0;
        body[0] = F_PING; /* nothing to send again: a probe all the same */
        size = 1;
    }
    size_t total = transmit(l, &f, body, size, out, now);
    if (!total) return 0;
    if (probe) --l->probes;
    *number = f.pn;
    return total;
}

static uint64_t pto_period(const PureLinkSender *l) {
    uint64_t smoothed = l->sampled ? l->smoothed_rtt : FIRST_RTT, var = l->sampled ? l->rtt_var : FIRST_RTT / 2;
    uint64_t period = smoothed + most(4 * var, GRANULARITY) + ACK_DELAY_MOST;
    for (unsigned i = 0; i < l->pto_count && period < PTO_MOST; ++i) period *= 2;
    return least(period, PTO_MOST);
}

static bool waiting_for_save(const PureLinkSender *l) { return l->received > 0; }

uint64_t pure_link_sender_due(const PureLinkSender *l) {
    uint64_t due = l->loss_time;
    if (l->pace_due && (!due || l->pace_due < due)) due = l->pace_due;
    if (l->eliciting) {
        uint64_t pto = l->last_eliciting + pto_period(l);
        if (!due || pto < due) due = pto;
    }
    if (waiting_for_save(l)) {
        uint64_t stored = l->stored_changed + STORED_WAIT;
        if (!due || stored < due) due = stored;
    }
    return due;
}

void pure_link_sender_tick(PureLinkSender *l, uint64_t now) {
    if (l->loss_time && l->loss_time <= now) detect_lost(l, now);
    else if (l->eliciting && l->last_eliciting + pto_period(l) <= now) {
        /* Nothing heard for a while: a probe, what flies longest sent again. */
        ++l->pto_count;
        l->probes = 1;
        for (size_t k = 0; k < l->count; ++k) {
            Record *r = record_at(l, k);
            uint32_t i = 0;
            while (i < r->parts && part_of(r, i)->state != FLYING) ++i;
            if (i < r->parts) {
                set_part(l, r, part_of(r, i), UNSENT);
                ++l->counts.resent;
                break;
            }
        }
    }
    if (waiting_for_save(l) && l->stored_changed + STORED_WAIT <= now) {
        /* Received long ago and still not stored: the receiver lost them (it
         * restarted) or its word got lost. Sent again: it takes a number once. */
        for (size_t k = 0; k < l->count && l->received; ++k) {
            Record *r = record_at(l, k);
            for (uint32_t i = 0; i < r->parts; ++i)
                if (part_of(r, i)->state == RECEIVED) set_part(l, r, part_of(r, i), UNSENT), ++l->counts.resent;
        }
        l->stored_changed = now;
    }
}

PureLinkSender *pure_link_sender(const char *source, const char *input, uint32_t shape, const PureSealKey *own,
                                 const unsigned char own_public[PURE_SEAL_KEY]) {
    size_t a = strlen(source), b = strlen(input);
    PureLinkSender *l = a && a <= PURE_LINK_NAME && b && b <= PURE_LINK_NAME ? calloc(1, sizeof(*l)) : NULL;
    if (!l) return NULL;
    memcpy(l->source, source, a);
    memcpy(l->input, input, b);
    l->shape = shape;
    l->own = own;
    memcpy(l->own_public, own_public, PURE_SEAL_KEY);
    l->own_id = key_id(own_public);
    l->run = random64();
    l->window = WINDOW_FIRST;
    l->threshold = UINT64_MAX;
    l->reorder_packets = REORDER_FIRST;
    l->datagram = DATAGRAM;
    return l;
}

/* The receiver's key, with the secret made (or given) into l->secret: the
 * keys of packets to it. */
static bool keyed(PureLinkSender *l, const unsigned char peer[PURE_SEAL_KEY], const unsigned char *secret) {
    memcpy(l->peer, peer, PURE_SEAL_KEY);
    l->peer_id = key_id(peer);
    pure_aead_free(l->out);
    pure_aead_free(l->in);
    l->out = l->in = NULL;
    if (secret) memcpy(l->secret, secret, 32);
    if (!secret) ++l->counts.secrets;
    return (secret || (l->own && pure_link_secret(l->own, l->peer, l->secret))) &&
           (l->out = aead_of(l->secret, l->own_public, l->peer, l->run, 0, l->out_base)) && ++l->counts.keyed;
}

bool pure_link_sender_peer(PureLinkSender *l, const unsigned char peer[PURE_SEAL_KEY]) { return keyed(l, peer, NULL); }

bool pure_link_sender_shared(PureLinkSender *l, const unsigned char peer[PURE_SEAL_KEY], const unsigned char secret[32]) {
    return keyed(l, peer, secret);
}

uint64_t pure_link_sender_queued(const PureLinkSender *l) { return l->queued; }

size_t pure_link_sender_datagram(const PureLinkSender *l) { return l->datagram; }

uint64_t pure_link_sender_stored(const PureLinkSender *l) { return l->stored; }

void pure_link_sender_counts(const PureLinkSender *l, PureLinkCounts *out) {
    *out = l->counts;
    out->stored = l->stored;
}

void pure_link_sender_free(PureLinkSender *l) {
    if (!l) return;
    for (size_t k = 0; k < l->count; ++k) free(record_at(l, k)->more);
    free(l->records);
    free(l->ring);
    free(l->flights);
    pure_aead_free(l->out);
    pure_aead_free(l->in);
    explicit_bzero(l->secret, sizeof(l->secret));
    free(l);
}

/* ---- The receiving side
 *
 * A sender allowed is a peer: its key and the sources it may send as, a few
 * bytes each, found by its key's id. What a sending peer needs besides (the
 * secret its key shares with the node's, its runs' keys and addresses, its
 * streams) is its active state: made at its first packet (one X25519), let
 * go once the peer has been quiet for the idle period with nothing of it
 * waiting here. So a link at rest costs the node its allowing, and the next
 * packet goes on without a handshake. */

typedef struct { uint64_t first, last; } Range;
typedef struct Active Active;
typedef struct {
    Active *active;
    uint64_t run, seen; /* the sender's run; when it last sent */
    /* The run its answers are sealed in, drawn when the slot is made: a
     * slot made again for the same sender's run (after it rested) never
     * seals under a key and nonce used before. */
    uint64_t answer_run;
    bool used, ack_due, due;
    /* The address answers go to is checked once the sender says back the
     * challenge sent there; until then they are at most three times the
     * bytes that came from it (RFC 9000 §8). */
    bool checked;
    uint64_t challenge, bytes_in, bytes_out;
    unsigned char in_key[PURE_AEAD_KEY], out_key[PURE_AEAD_KEY], in_base[PURE_AEAD_NONCE], out_base[PURE_AEAD_NONCE];
    uint64_t next_pn, largest_at;
    _Alignas(8) unsigned char address[PURE_LINK_ADDRESS]; /* where it sends from: a socket's address */
    size_t address_size;
    Range got[RANGES]; /* the numbers received, newest first */
    unsigned got_count;
} Run;
typedef struct { uint32_t from, to; } Span; /* bytes [from, to) */
typedef struct {
    uint64_t seq;
    unsigned char *bytes;
    uint32_t size, spans, room; /* the spans of its bytes had, in order, none touching (room for room) */
    Span *span;
    bool whole;
} Held;
typedef struct Stream {
    Active *active;
    char source[PURE_LINK_NAME + 1], input_name[PURE_LINK_NAME + 1];
    uint32_t input, shape; /* the host's number for the input; the messages' shape */
    uint64_t heard;              /* the last number heard (stored), once known */
    bool known, out, stored_due, gap, ready;
    /* The line: once the last heard is known, whole records from the one
     * after it on, in order, as a delivery carries them (u32 size, bytes),
     * from line_head: handed over as they are, let go by moving the head.
     * The record at its end may come in parts, in order from its start:
     * the tail, its room kept after the line's records, tail_got bytes of
     * its tail_size had. Records that come early or out of order, or before
     * the last heard is known, wait in held until the line reaches them. */
    unsigned char *line;
    size_t line_head, line_size, line_capacity;
    uint64_t line_first;
    unsigned line_count;
    bool tail;
    uint32_t tail_size, tail_got;
    Held *held;                  /* records not yet stored, by number: held_count from held_first */
    size_t held_first, held_count, held_capacity, held_bytes;
    Run *run;                    /* the run that sent to it last: where STORED goes */
    uint64_t run_id;
} Stream;
struct Active {
    uint32_t peer;               /* its place among the peers */
    unsigned char secret[32];
    uint64_t seen;
    size_t place;                /* among the active */
    Run runs[RUNS];
    Stream **streams;
    unsigned stream_count, stream_capacity;
};
typedef struct {
    unsigned char key[PURE_SEAL_KEY];
    uint32_t id, allowed; /* allowed: the place + 1 of its first source (0: none) */
    Active *active;
} Peer;
typedef struct { uint32_t name, next; } Allowed; /* name: where in names (u8 length, bytes); next: a place + 1 */

/* A peer quiet this long lets its active state go; the active are looked at
 * that often; what they held goes back to the system once TRIM_AFTER have
 * gone, or TRIM_WAIT after any went. */
static const uint64_t IDLE_FIRST = 30000 * MS, SWEEP_EVERY = 250 * MS, TRIM_WAIT = 10000 * MS;
enum { TRIM_AFTER = 1024 };

struct PureLinkReceiver {
    PureLinkHost host;
    const PureSealKey *own;
    unsigned char own_public[PURE_SEAL_KEY];
    uint32_t own_id;
    uint64_t idle, swept, trimmed, untrimmed;
    /* Keys made for a run that then did not open cost a token; tokens come
     * back KEYINGS a second, up to KEYINGS. Under a flood of made-up
     * packets keys are made for that many a second at most, while senders
     * whose packets open (a thousand waking at once) cost none. */
    double key_tokens;
    uint64_t key_at;
    PureAead *aead; /* keyed by each packet's run */
    unsigned char open_key[PURE_AEAD_KEY], seal_key[PURE_AEAD_KEY]; /* the keys it has now */
    bool open_keyed, seal_keyed;
    unsigned char *memo; /* the secrets remembered (pure_link_remember), mapped */
    size_t memo_size;
    Peer *peers;
    size_t peer_count, peer_capacity;
    uint32_t *index; /* the peers by key id: open addressing, a place + 1 (0: empty) */
    size_t index_size;
    Allowed *allowed;
    size_t allowed_count, allowed_capacity;
    unsigned char *names;
    size_t names_size, names_capacity;
    Active **actives;
    size_t active_count, active_capacity;
    Stream **ready; /* streams that may have whole records to hand over */
    size_t ready_count, ready_capacity;
    Run **due; /* runs to answer */
    size_t due_count, due_capacity;
    unsigned char *batch;
    size_t batch_capacity;
    PureLinkCounts counts;
};

PureLinkReceiver *pure_link_receiver(const PureSealKey *own, const unsigned char own_public[PURE_SEAL_KEY],
                                     const PureLinkHost *host, char *error, size_t size) {
    if (!own) {
        snprintf(error, size, "a receiver needs the node's key");
        return NULL;
    }
    if (!pure_seal_ready(error, size)) return NULL;
    PureLinkReceiver *s = calloc(1, sizeof(*s));
    unsigned char key[PURE_AEAD_KEY] = {0}; /* each packet brings its run's */
    if (!s || !(s->aead = pure_aead(key))) {
        free(s);
        snprintf(error, size, "out of memory");
        return NULL;
    }
    s->host = *host;
    s->own = own;
    memcpy(s->own_public, own_public, PURE_SEAL_KEY);
    s->own_id = key_id(own_public);
    s->idle = IDLE_FIRST;
    return s;
}

void pure_link_idle(PureLinkReceiver *s, uint64_t ms) { s->idle = ms * MS; }

/* items (count used of capacity) with room for one more: the same or grown,
 * or NULL when memory runs out (items stay as they were). */
static void *room(void *items, size_t *capacity, size_t count, size_t item_size, size_t first) {
    if (count < *capacity) return items;
    size_t more = *capacity ? 2 * *capacity : first;
    void *grown = realloc(items, more * item_size);
    if (grown) *capacity = more;
    return grown;
}

/* The peers by key id again, in an index twice as large. */
static bool grow_index(PureLinkReceiver *s) {
    size_t size = s->index_size ? 2 * s->index_size : 1024;
    uint32_t *index = calloc(size, sizeof(*index));
    if (!index) return false;
    for (size_t p = 0; p < s->peer_count; ++p) {
        size_t slot = s->peers[p].id & (size - 1);
        while (index[slot]) slot = (slot + 1) & (size - 1);
        index[slot] = (uint32_t)p + 1;
    }
    free(s->index);
    s->index = index;
    s->index_size = size;
    return true;
}

static bool same_name(const PureLinkReceiver *s, uint32_t name, const char *source, size_t length) {
    return s->names[name] == length && !memcmp(s->names + name + 1, source, length);
}

/* Keys are found by their ids, which are 32 bits: two keys may share one. */
bool pure_link_allow(PureLinkReceiver *s, const char *source, const unsigned char key[PURE_SEAL_KEY], char *error,
                     size_t size) {
    size_t length = strlen(source), p = SIZE_MAX;
    uint32_t id = key_id(key);
    if (!length || length > PURE_LINK_NAME) {
        snprintf(error, size, "a source's name is 1 to %d bytes", PURE_LINK_NAME);
        return false;
    }
    for (size_t slot = s->index_size ? id & (s->index_size - 1) : 0; s->index_size && s->index[slot];
         slot = (slot + 1) & (s->index_size - 1)) {
        const Peer *peer = &s->peers[s->index[slot] - 1];
        if (peer->id == id && !memcmp(peer->key, key, PURE_SEAL_KEY)) {
            p = s->index[slot] - 1;
            break;
        }
    }
    if (p == SIZE_MAX) {
        Peer *peers = s->peer_count < UINT32_MAX - 1 && ((s->peer_count + 1) * 2 <= s->index_size || grow_index(s)) ?
                      room(s->peers, &s->peer_capacity, s->peer_count, sizeof(Peer), 64) : NULL;
        if (!peers) goto full;
        s->peers = peers;
        p = s->peer_count++;
        s->peers[p] = (Peer){.id = id};
        memcpy(s->peers[p].key, key, PURE_SEAL_KEY);
        size_t slot = id & (s->index_size - 1);
        while (s->index[slot]) slot = (slot + 1) & (s->index_size - 1);
        s->index[slot] = (uint32_t)p + 1;
    }
    for (uint32_t a = s->peers[p].allowed; a; a = s->allowed[a - 1].next)
        if (same_name(s, s->allowed[a - 1].name, source, length)) return true;
    Allowed *kept = s->allowed_count < UINT32_MAX - 1 && s->names_size + 1 + length <= UINT32_MAX ?
                    room(s->allowed, &s->allowed_capacity, s->allowed_count, sizeof(Allowed), 64) : NULL;
    if (!kept) goto full;
    s->allowed = kept;
    if (s->names_size + 1 + length > s->names_capacity) {
        size_t more = s->names_capacity ? 2 * s->names_capacity : 4096;
        unsigned char *names = realloc(s->names, more);
        if (!names) goto full;
        s->names = names;
        s->names_capacity = more;
    }
    s->names[s->names_size] = (unsigned char)length;
    memcpy(s->names + s->names_size + 1, source, length);
    s->allowed[s->allowed_count] = (Allowed){(uint32_t)s->names_size, s->peers[p].allowed};
    s->peers[p].allowed = (uint32_t)++s->allowed_count;
    s->names_size += 1 + length;
    return true;
full:
    snprintf(error, size, "out of memory");
    return false;
}

static Held *held_at(Stream *st, size_t k) { return &st->held[st->held_first + k]; }

static uint64_t line_end(const Stream *st) { return st->line_first + st->line_count; }

/* Room for more bytes at the line's end: its front let go given back
 * first when that is at least half of it (so no byte moves twice in a
 * doubling), else a longer line. */
static bool line_room(Stream *st, size_t more) {
    if (st->line_size + more <= st->line_capacity) return true;
    if (st->line_head && st->line_head >= st->line_capacity / 2) {
        memmove(st->line, st->line + st->line_head, st->line_size - st->line_head);
        st->line_size -= st->line_head;
        st->line_head = 0;
        if (st->line_size + more <= st->line_capacity) return true;
    }
    size_t capacity = st->line_capacity ? st->line_capacity : 4096;
    while (capacity < st->line_size + more) capacity *= 2;
    unsigned char *grown = realloc(st->line, capacity);
    if (!grown) return false;
    st->line = grown;
    st->line_capacity = capacity;
    return true;
}

static bool line_append(Stream *st, const unsigned char *bytes, uint32_t size) {
    if (!line_room(st, 4 + (size_t)size)) return false;
    put32(st->line + st->line_size, size);
    if (size) memcpy(st->line + st->line_size + 4, bytes, size);
    st->line_size += 4 + (size_t)size;
    ++st->line_count;
    return true;
}

/* The line's records up to heard let go (all of them, when heard is past it). */
static void line_drop(Stream *st, uint64_t heard) {
    if (heard < st->line_first) return;
    if (heard >= line_end(st)) {
        st->line_head = st->line_size = 0;
        st->line_count = 0;
        st->line_first = heard + 1;
        st->tail = false;
        return;
    }
    for (uint64_t n = heard - st->line_first + 1; n; --n, --st->line_count, ++st->line_first)
        st->line_head += 4 + (size_t)get32(st->line + st->line_head);
}

/* Records held that the line reached, moved onto it; ones behind it let go. */
static void drop_first(Stream *st);
static void line_pull(Stream *st) {
    while (st->held_count) {
        Held *h = held_at(st, 0);
        if (h->seq < line_end(st)) drop_first(st);
        else if (h->seq == line_end(st) && h->whole && line_append(st, h->bytes, h->size)) drop_first(st);
        else break;
    }
}

/* The first record held let go: the ones after it stay where they are. */
static void drop_first(Stream *st) {
    st->held_bytes -= held_at(st, 0)->size;
    free(held_at(st, 0)->bytes);
    free(held_at(st, 0)->span);
    ++st->held_first;
    if (!--st->held_count) st->held_first = 0;
}

/* The memo of secrets (pure_link_remember): a header, then a place a peer
 * in the order allowed, each the first 28 bytes of its key, a check (FNV-1a
 * of its whole key and the secret) and the secret. A place whose check
 * fails (never made, another key, cut short by a crash) is made again. */
enum { MEMO_HEAD = 64, MEMO_PLACE = 64 };
static const unsigned char memo_magic[8] = {'P', 'L', 'M', '1', 0, 0, 0, 0};

static uint32_t memo_check(const unsigned char key[PURE_SEAL_KEY], const unsigned char secret[32]) {
    return fnv(fnv(2166136261u, key, PURE_SEAL_KEY), secret, 32);
}

bool pure_link_remember(PureLinkReceiver *s, const char *path, char *error, size_t size) {
    int fd = open(path, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    struct stat info;
    unsigned char head[MEMO_HEAD];
    size_t needed = MEMO_HEAD + (s->peer_count ? s->peer_count : 1) * MEMO_PLACE;
    bool ok = fd >= 0 && !fstat(fd, &info) && S_ISREG(info.st_mode) && !(info.st_mode & 077);
    /* Another node's key, or none yet: begun again. */
    bool ours = ok && pread(fd, head, MEMO_HEAD, 0) == MEMO_HEAD && !memcmp(head, memo_magic, 8) &&
                !memcmp(head + 8, s->own_public, PURE_SEAL_KEY);
    if (ok && !ours) {
        memset(head, 0, MEMO_HEAD);
        memcpy(head, memo_magic, 8);
        memcpy(head + 8, s->own_public, PURE_SEAL_KEY);
        ok = !ftruncate(fd, 0) && pwrite(fd, head, MEMO_HEAD, 0) == MEMO_HEAD;
    }
    ok = ok && (ours && (size_t)info.st_size >= needed ? true : !ftruncate(fd, (off_t)needed));
    void *map = ok ? mmap(NULL, needed, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0) : MAP_FAILED;
    if (fd >= 0) close(fd);
    if (map == MAP_FAILED) {
        snprintf(error, size, "%s: cannot keep secrets there (a file only its owner may read)", path);
        return false;
    }
    s->memo = map;
    s->memo_size = needed;
    return true;
}

/* A peer's active state, made at its first packet: the secret its key
 * shares with ours, remembered or made (one X25519, then remembered: the
 * secret first, the key that makes the place its own last). */
static Active *activate(PureLinkReceiver *s, Peer *peer, uint64_t now) {
    Active *a = calloc(1, sizeof(*a)), **actives = room(s->actives, &s->active_capacity, s->active_count, sizeof(*actives), 64);
    if (actives) s->actives = actives;
    if (!a || !actives) {
        free(a);
        return NULL;
    }
    size_t index = (size_t)(peer - s->peers);
    unsigned char *place = s->memo && MEMO_HEAD + (index + 1) * MEMO_PLACE <= s->memo_size ?
                           s->memo + MEMO_HEAD + index * MEMO_PLACE : NULL;
    if (place && !memcmp(place, peer->key, 28) && get32(place + 28) == memo_check(peer->key, place + 32)) {
        memcpy(a->secret, place + 32, 32);
        ++s->counts.remembered;
    } else if (++s->counts.secrets && pure_link_secret(s->own, peer->key, a->secret)) {
        if (place) {
            memset(place, 0, 28);
            __atomic_thread_fence(__ATOMIC_RELEASE);
            memcpy(place + 32, a->secret, 32);
            put32(place + 28, memo_check(peer->key, a->secret));
            __atomic_thread_fence(__ATOMIC_RELEASE);
            memcpy(place, peer->key, 28);
        }
    } else {
        explicit_bzero(a, sizeof(*a));
        free(a);
        return NULL;
    }
    a->peer = (uint32_t)(peer - s->peers);
    a->seen = now;
    a->place = s->active_count;
    s->actives[s->active_count++] = a;
    peer->active = a;
    return a;
}

static void let_go(PureLinkReceiver *s, Active *a) {
    for (unsigned k = 0; k < a->stream_count; ++k) {
        while (a->streams[k]->held_count) drop_first(a->streams[k]);
        free(a->streams[k]->held);
        free(a->streams[k]->line);
        free(a->streams[k]);
    }
    free(a->streams);
    s->peers[a->peer].active = NULL;
    s->actives[a->place] = s->actives[--s->active_count];
    s->actives[a->place]->place = a->place;
    explicit_bzero(a, sizeof(*a));
    free(a);
}

/* Whether any of a peer's records or answers waits here. */
static bool busy(const Active *a) {
    for (unsigned k = 0; k < a->stream_count; ++k)
        if (a->streams[k]->out || a->streams[k]->held_count || a->streams[k]->line_count || a->streams[k]->tail ||
            a->streams[k]->ready)
            return true;
    for (unsigned i = 0; i < RUNS; ++i)
        if (a->runs[i].due) return true;
    return false;
}

/* Let go the active state of peers quiet for the idle period; what idle
 * links held goes back to the system. */
static void sweep(PureLinkReceiver *s, uint64_t now) {
    if (now - s->swept < SWEEP_EVERY) return;
    s->swept = now;
    size_t freed = 0;
    for (size_t k = 0; k < s->active_count;) {
        Active *a = s->actives[k];
        if (now - a->seen < s->idle || busy(a)) ++k;
        else let_go(s, a), ++freed;
    }
    s->counts.swept += freed;
    s->untrimmed += freed;
    if (s->untrimmed >= TRIM_AFTER || (s->untrimmed && now - s->trimmed >= TRIM_WAIT)) {
        malloc_trim(0);
        s->untrimmed = 0;
        s->trimmed = now;
    }
}

static void mark_ready(PureLinkReceiver *s, Stream *st) {
    Stream **ready = st->ready ? NULL : room(s->ready, &s->ready_capacity, s->ready_count, sizeof(*ready), 64);
    if (!ready) return;
    s->ready = ready;
    s->ready[s->ready_count++] = st;
    st->ready = true;
}

static void mark_due(PureLinkReceiver *s, Run *r) {
    Run **due = r->due ? NULL : room(s->due, &s->due_capacity, s->due_count, sizeof(*due), 64);
    if (!due) return;
    s->due = due;
    s->due[s->due_count++] = r;
    r->due = true;
}

static Run *run_found(Active *a, uint64_t run) {
    for (unsigned i = 0; i < RUNS; ++i)
        if (a->runs[i].used && a->runs[i].run == run) return &a->runs[i];
    return NULL;
}

/* A new run's keys, in the slot unused longest: only once its first packet
 * opened with them, so a forged one displaces nothing. */
static Run *install_run(Active *a, const Run *made) {
    Run *slot = &a->runs[0];
    for (unsigned i = 0; i < RUNS; ++i) {
        Run *r = &a->runs[i];
        if (!r->used || (slot->used && r->seen < slot->seen)) slot = r;
    }
    bool due = slot->due; /* the answer list may hold the slot */
    *slot = *made;
    slot->due = due;
    return slot;
}

static bool had(const Run *r, uint64_t pn) {
    for (unsigned i = 0; i < r->got_count; ++i)
        if (pn >= r->got[i].first && pn <= r->got[i].last) return true;
    return false;
}

static void note(Run *r, uint64_t pn, uint64_t now) {
    unsigned i = 0, n = r->got_count;
    while (i < n && r->got[i].first > pn + 1) ++i;
    if (i < n && r->got[i].first == pn + 1) { /* joins the range above it */
        r->got[i].first = pn;
        if (i + 1 < n && r->got[i + 1].last + 1 == pn) {
            r->got[i].first = r->got[i + 1].first;
            memmove(&r->got[i + 1], &r->got[i + 2], (n - i - 2) * sizeof(Range));
            --r->got_count;
        }
    } else if (i < n && r->got[i].last + 1 == pn) {
        r->got[i].last = pn;
    } else if (i < RANGES) {
        if (n == RANGES) --n; /* the oldest range is forgotten */
        memmove(&r->got[i + 1], &r->got[i], (n - i) * sizeof(Range));
        r->got[i] = (Range){pn, pn};
        r->got_count = n + 1;
    }
    if (r->got_count && r->got[0].last == pn) r->largest_at = now;
}

static bool allowed(const PureLinkReceiver *s, const Peer *peer, const char *source) {
    for (uint32_t a = peer->allowed; a; a = s->allowed[a - 1].next)
        if (same_name(s, s->allowed[a - 1].name, source, strlen(source))) return true;
    return false;
}

/* The stream of a peer's source to input, made the first time: NULL when the
 * source is not allowed from the peer, or the host has no input that takes
 * such messages. */
static Stream *stream_of(PureLinkReceiver *s, Active *a, const char *source, const char *input, uint32_t shape) {
    for (unsigned k = 0; k < a->stream_count; ++k) {
        Stream *st = a->streams[k];
        if (!strcmp(st->input_name, input) && !strcmp(st->source, source)) return st->shape == shape ? st : NULL;
    }
    int index = allowed(s, &s->peers[a->peer], source) ? s->host.input(s->host.context, input, shape) : -1;
    if (index < 0) return NULL;
    size_t capacity = a->stream_capacity;
    Stream **streams = room(a->streams, &capacity, a->stream_count, sizeof(*streams), 2), *st;
    if (!streams) return NULL;
    a->streams = streams;
    a->stream_capacity = (unsigned)capacity;
    if (!(st = calloc(1, sizeof(*st)))) return NULL;
    st->active = a;
    st->input = (uint32_t)index;
    st->shape = shape;
    snprintf(st->source, sizeof(st->source), "%s", source);
    snprintf(st->input_name, sizeof(st->input_name), "%s", input);
    a->streams[a->stream_count++] = st;
    return st;
}

/* Bytes [from, to) of h had: the spans that touch them become one. False
 * when memory runs out. */
static bool had_bytes(Held *h, uint32_t from, uint32_t to) {
    uint32_t i = 0, j;
    while (i < h->spans && h->span[i].to < from) ++i;
    for (j = i; j < h->spans && h->span[j].from <= to; ++j) {
        if (h->span[j].from < from) from = h->span[j].from;
        if (h->span[j].to > to) to = h->span[j].to;
    }
    if (j == i) { /* touches none: one more */
        if (h->spans == h->room) {
            uint32_t more = h->room ? 2 * h->room : 4;
            Span *grown = realloc(h->span, more * sizeof(Span));
            if (!grown) return false;
            h->span = grown;
            h->room = more;
        }
        memmove(&h->span[i + 1], &h->span[i], (h->spans - i) * sizeof(Span));
        ++h->spans;
    } else { /* spans i to j - 1 become one */
        memmove(&h->span[i + 1], &h->span[j], (h->spans - j) * sizeof(Span));
        h->spans -= j - i - 1;
    }
    h->span[i] = (Span){from, to};
    h->whole = h->spans == 1 && !h->span[0].from && h->span[0].to == h->size;
    return true;
}

/* The record held numbered seq, made when there is none (NULL when there
 * is no room, or it was said to be of another size). */
static Held *held_for(Stream *st, uint64_t seq, uint32_t size) {
    /* In order it goes last; out of order, its place is found by halving. */
    size_t k = st->held_count;
    if (k && held_at(st, k - 1)->seq >= seq) {
        size_t low = 0;
        while (low < k) {
            size_t middle = low + (k - low) / 2;
            if (held_at(st, middle)->seq < seq) low = middle + 1;
            else k = middle;
        }
    }
    if (k == st->held_count || held_at(st, k)->seq != seq) {
        if (st->held_bytes + size > HELD_MOST) return NULL;
        if (st->held_first && st->held_first + st->held_count == st->held_capacity) { /* the front is free: used again */
            memmove(st->held, held_at(st, 0), st->held_count * sizeof(Held));
            st->held_first = 0;
        }
        Held *kept = room(st->held, &st->held_capacity, st->held_first + st->held_count, sizeof(Held), 16);
        if (!kept) return NULL;
        st->held = kept;
        Held h = {.seq = seq, .size = size};
        if (!(h.bytes = malloc(size ? size : 1))) return NULL;
        memmove(held_at(st, k + 1), held_at(st, k), (st->held_count - k) * sizeof(Held));
        *held_at(st, k) = h;
        ++st->held_count;
        st->held_bytes += size;
    }
    Held *h = held_at(st, k);
    return h->size == size ? h : NULL;
}

static bool is_held(Stream *st, uint64_t seq) {
    size_t k = 0;
    while (k < st->held_count && held_at(st, k)->seq < seq) ++k;
    return k < st->held_count && held_at(st, k)->seq == seq;
}

/* A part of the record at the line's end, onto the line when it comes in
 * order: whole, or as the tail, part after part from its start. True when
 * it is taken (or let be: no room, had already); false when it waits in
 * held instead (it came out of order: then so does the tail so far). */
static bool on_line(Stream *st, uint32_t size, uint32_t offset, const unsigned char *bytes, uint32_t length) {
    bool whole = !offset && length == size;
    if (st->tail) {
        unsigned char *at = st->line + st->line_size + 4;
        if (size != st->tail_size || (!whole && offset + length <= st->tail_got)) return true;
        if (!whole && offset != st->tail_got) { /* out of order: held, with what the tail had */
            uint64_t seq = line_end(st);
            st->tail = false;
            Held *h = held_for(st, seq, size);
            if (h && st->tail_got) {
                memcpy(h->bytes, at, st->tail_got);
                had_bytes(h, 0, st->tail_got);
            }
            return false;
        }
        memcpy(at + offset, bytes, length);
        st->tail_got = whole ? size : st->tail_got + length;
        if (st->tail_got == size) { /* whole now: a record of the line */
            st->line_size += 4 + (size_t)size;
            ++st->line_count;
            st->tail = false;
            line_pull(st);
        }
        return true;
    }
    if (st->held_bytes + (st->line_size - st->line_head) + size > HELD_MOST) return true;
    if (whole) { /* a part of it held goes as the line passes it */
        if (line_append(st, bytes, size)) line_pull(st);
        return true;
    }
    if (offset || is_held(st, line_end(st)) || !line_room(st, 4 + (size_t)size)) return false;
    put32(st->line + st->line_size, size);
    memcpy(st->line + st->line_size + 4, bytes, length);
    st->tail = true;
    st->tail_size = size;
    st->tail_got = length;
    return true;
}

/* A record's part, kept until the record is whole and handed over. */
static void hold(Stream *st, uint64_t seq, uint32_t size, uint32_t offset, const unsigned char *bytes, uint32_t length) {
    if (st->known && seq <= st->heard) {
        st->stored_due = true; /* sent again: say again how far it is stored */
        return;
    }
    if ((st->known && seq > st->heard + CREDIT) || size > RECORD_MOST) return;
    if (st->known && seq < line_end(st)) return; /* on the line already */
    if ((!length && offset) || offset > size || length > size - offset) return;
    if (st->known && seq == line_end(st) && on_line(st, size, offset, bytes, length)) return;
    Held *h = held_for(st, seq, size);
    if (!h || h->whole) return;
    if (!offset && length == size) { /* whole at once */
        if (size) memcpy(h->bytes, bytes, size);
        h->whole = true;
        return;
    }
    if (!length) return;
    memcpy(h->bytes + offset, bytes, length);
    had_bytes(h, offset, offset + length);
}

/* Whole records from the one after the last heard, in order, to the host:
 * one delivery a stream at a time. Before the last heard is known (after a
 * restart) they go from the first held: the host says how far it heard.
 * False when the host could not take them now. */
static bool hand_over(PureLinkReceiver *s, Stream *st) {
    if (st->out) return true;
    if (st->known) { /* the line as it is, up to BATCH records */
        line_pull(st);
        if (!st->line_count) return true;
        unsigned n = 0;
        size_t bytes = 0;
        for (; n < st->line_count && n < BATCH; ++n) bytes += 4 + (size_t)get32(st->line + st->line_head + bytes);
        PureLinkDelivery d = {st->input, st->source, st->line_first, n, st->line + st->line_head, bytes, st};
        if (!s->host.deliver(s->host.context, &d)) return false;
        st->out = true;
        ++s->counts.deliveries;
        return true;
    }
    /* From the first whole record held: one held before it in part may be
     * of the past (a packet sent again) and never come whole; the host's
     * word on this delivery says how far it heard, and lets that go. */
    size_t first = 0;
    while (first < st->held_count && !held_at(st, first)->whole) ++first;
    size_t n = 0, bytes = 0;
    while (first + n < st->held_count && n < BATCH && held_at(st, first + n)->whole &&
           held_at(st, first + n)->seq == held_at(st, first)->seq + n)
        bytes += 4 + held_at(st, first + n++)->size;
    if (!n) return true;
    if (bytes > s->batch_capacity) {
        unsigned char *grown = realloc(s->batch, bytes);
        if (!grown) return false;
        s->batch = grown;
        s->batch_capacity = bytes;
    }
    size_t at = 0;
    for (size_t k = first; k < first + n; ++k) {
        put32(s->batch + at, held_at(st, k)->size);
        memcpy(s->batch + at + 4, held_at(st, k)->bytes, held_at(st, k)->size);
        at += 4 + held_at(st, k)->size;
    }
    PureLinkDelivery d = {st->input, st->source, held_at(st, first)->seq, (unsigned)n, s->batch, bytes, st};
    if (!s->host.deliver(s->host.context, &d)) return false;
    st->out = true;
    ++s->counts.deliveries;
    return true;
}

/* A packet from peer, if it opens with the keys of peer's run (made for a
 * run not seen yet, and for a peer at rest its active state): its frames. */
/* Whether keys may be made for a run not seen (a token is there). */
static bool may_key(PureLinkReceiver *s, uint64_t now) {
    if (!s->key_at) s->key_tokens = KEYINGS;
    s->key_tokens += (double)(now - s->key_at) * KEYINGS / 1e9;
    if (s->key_tokens > KEYINGS) s->key_tokens = KEYINGS;
    s->key_at = now;
    return s->key_tokens >= 1;
}

static bool take_from(PureLinkReceiver *s, Peer *peer, const unsigned char *packet, size_t n, const void *from,
                      size_t from_size, uint64_t now) {
    uint64_t pn = get64(packet + 13), run = get64(packet + 5);
    Active *a = peer->active;
    Run *r = a ? run_found(a, run) : NULL;
    if (!r && !may_key(s, now)) return false;
    bool woke = !a;
    if (woke && !(a = activate(s, peer, now))) return false;
    Run made = {.active = a, .run = run, .seen = now, .used = true};
    if (!r) made.answer_run = random64();
    bool keyed = r || (++s->counts.keyed && derive(a->secret, peer->key, s->own_public, run, 0, made.in_key, made.in_base) &&
                       derive(a->secret, s->own_public, peer->key, made.answer_run, run, made.out_key, made.out_base));
    unsigned char plain[DATAGRAM_MOST], nonce[PURE_AEAD_NONCE];
    nonce_of(nonce, r ? r->in_base : made.in_base, pn);
    size_t size = n - HEADER - PURE_AEAD_TAG;
    const unsigned char *key = r ? r->in_key : made.in_key, *give = key;
    if (s->open_keyed && !memcmp(s->open_key, key, PURE_AEAD_KEY)) give = NULL; /* the key it has */
    else if (keyed) memcpy(s->open_key, key, PURE_AEAD_KEY), s->open_keyed = true;
    if (!keyed || !pure_aead_open(s->aead, give, nonce, packet, HEADER, packet + HEADER, n - HEADER, plain)) {
        explicit_bzero(&made, sizeof(made));
        if (!r) s->key_tokens = s->key_tokens > 1 ? s->key_tokens - 1 : 0; /* keys for nothing */
        if (woke) let_go(s, a);
        return false;
    }
    if (woke) ++s->counts.woken;
    bool fresh = !r;
    if (!r) r = install_run(a, &made);
    if (!fresh && had(r, pn)) { /* again (opened first: nothing unsealed answers): its acknowledgment was lost, perhaps */
        r->ack_due = true;
        mark_due(s, r);
        return true;
    }
    bool newest = fresh || !r->got_count || pn > r->got[0].last;
    a->seen = r->seen = now;
    note(r, pn, now);
    /* Where it is now, if this is its newest packet (one of the past sent
     * again moves nothing): answers go there, the address checked first. */
    size_t at_size = from_size <= sizeof(r->address) ? from_size : sizeof(r->address);
    bool elsewhere = r->address_size != at_size || memcmp(r->address, from, at_size);
    if (elsewhere && newest) {
        memcpy(r->address, from, at_size);
        r->address_size = at_size;
        r->checked = false;
        r->challenge = random64();
        r->bytes_in = r->bytes_out = 0;
    }
    if (!elsewhere || newest) r->bytes_in += n;
    ++s->counts.received;
    Stream *st = NULL;
    bool eliciting = false;
    for (size_t at = 0; at < size;) {
        unsigned type = plain[at++];
        if (type == F_STREAM) {
            char source[PURE_LINK_NAME + 1], input[PURE_LINK_NAME + 1];
            size_t names = get_names(plain + at, size - at, source, input);
            if (!names || at + names + 4 > size) break;
            st = stream_of(s, a, source, input, get32(plain + at + names));
            if (st) {
                st->run = r;
                st->run_id = r->run;
            }
            at += names + 4;
            eliciting = true;
        } else if (type == F_RECORD) {
            if (at + RECORD_HEAD - 1 > size) break;
            uint32_t length = get16(plain + at + 16);
            if (at + RECORD_HEAD - 1 + length > size) break;
            if (st) {
                hold(st, get64(plain + at), get32(plain + at + 8), get32(plain + at + 12), plain + at + 18, length);
                mark_ready(s, st);
            }
            at += RECORD_HEAD - 1 + length;
            eliciting = true;
        } else if (type == F_RECORDS) {
            if (at + RECORDS_HEAD - 1 > size) break;
            uint64_t first = get64(plain + at), length = 0;
            unsigned count = plain[at + 8];
            at += RECORDS_HEAD - 1;
            unsigned i = 0;
            for (size_t used; i < count && (used = get_varint(plain + at, size - at, &length)) &&
                              length <= size - at - used; ++i) {
                if (st) hold(st, first + i, (uint32_t)length, 0, plain + at + used, (uint32_t)length);
                at += used + length;
            }
            if (st) mark_ready(s, st);
            eliciting = true;
            if (i < count) break; /* cut short: the rest is not read */
        } else if (type == F_PING) {
            eliciting = true;
        } else if (type == F_RESPONSE) { /* the challenge said back: the address is the sender's */
            if (at + 8 > size) break;
            if (!r->checked && get64(plain + at) == r->challenge) r->checked = true;
            at += 8;
            eliciting = true;
        } else
            break; /* padding, or what this side does not know: the rest */
    }
    if (eliciting) {
        r->ack_due = true;
        mark_due(s, r);
    }
    return true;
}

void pure_link_receiver_take(PureLinkReceiver *s, const unsigned char *packet, size_t n, const void *from,
                             size_t from_size, uint64_t now) {
    if (n < HEADER + PURE_AEAD_TAG || n > DATAGRAM_MOST || packet[0] != VERSION || !s->index_size) return;
    uint32_t id = get32(packet + 1);
    size_t mask = s->index_size - 1;
    uint64_t run = get64(packet + 5);
    /* The peers with the packet's key id: first one whose run is known (no
     * key to make), then any. */
    for (size_t slot = id & mask; s->index[slot]; slot = (slot + 1) & mask) {
        Peer *peer = &s->peers[s->index[slot] - 1];
        if (peer->id == id && peer->active && run_found(peer->active, run) &&
            take_from(s, peer, packet, n, from, from_size, now))
            return;
    }
    for (size_t slot = id & mask; s->index[slot]; slot = (slot + 1) & mask) {
        Peer *peer = &s->peers[s->index[slot] - 1];
        if (peer->id == id && !(peer->active && run_found(peer->active, run)) &&
            take_from(s, peer, packet, n, from, from_size, now))
            return;
    }
}

/* Acknowledgments and how far each stream is stored, a packet to each run
 * that has any. How far a stream is stored goes with every acknowledgment
 * to its run, so a lost word is said again at the next packet. */
static void answer(PureLinkReceiver *s, uint64_t now, PureLinkSend *send, void *context) {
    unsigned char body[BODY], packet[DATAGRAM];
    for (size_t k = 0; k < s->due_count; ++k) {
        Run *r = s->due[k];
        Active *a = r->active;
        r->due = false;
        if (!r->used) continue;
        size_t at = 0;
        if (!r->checked) { /* the address not checked yet: a challenge */
            body[at++] = F_CHALLENGE;
            put64(body + at, r->challenge);
            at += 8;
        }
        if (r->ack_due && r->got_count) {
            body[at++] = F_ACK;
            put64(body + at, r->got[0].last);
            put32(body + at + 8, (uint32_t)least((now - r->largest_at) / 1000, UINT32_MAX));
            body[at + 12] = (unsigned char)r->got_count;
            at += 13;
            for (unsigned i = 0; i < r->got_count; ++i, at += 16) {
                put64(body + at, r->got[i].first);
                put64(body + at + 8, r->got[i].last);
            }
        }
        for (unsigned j = 0; j < a->stream_count; ++j) {
            Stream *st = a->streams[j];
            if (!(st->stored_due || r->ack_due) || st->run != r || st->run_id != r->run || !st->known) continue;
            if (at + strlen(st->source) + strlen(st->input_name) + 16 > BODY) break; /* type, names, 13 */
            body[at++] = F_STORED;
            at += put_names(body + at, st->source, st->input_name);
            put64(body + at, st->heard);
            body[at + 8] = st->gap ? STORED_GAP : 0;
            size_t room = st->held_bytes + (st->line_size - st->line_head) < HELD_MOST ? CREDIT : 1;
            put32(body + at + 9, (uint32_t)room);
            at += 13;
            st->stored_due = st->gap = false;
        }
        r->ack_due = false;
        if (!at) continue;
        if (!r->checked && r->bytes_out + HEADER + at + PURE_AEAD_TAG > 3 * r->bytes_in) continue; /* no amplifier */
        const unsigned char *give = r->out_key;
        if (s->seal_keyed && !memcmp(s->seal_key, r->out_key, PURE_AEAD_KEY)) give = NULL; /* the key it has */
        else memcpy(s->seal_key, r->out_key, PURE_AEAD_KEY), s->seal_keyed = true;
        size_t total = seal_packet(s->aead, give, r->out_base, s->own_id, r->answer_run, r->next_pn++, body, at, packet);
        if (total && send(context, packet, total, r->address, r->address_size)) {
            ++s->counts.packets;
            if (!r->checked) r->bytes_out += total;
        }
    }
    s->due_count = 0;
}

/* What the host said of its deliveries: the saved records let go, and how
 * far they are stored said. */
static void apply_done(PureLinkReceiver *s, const PureLinkDone *done, size_t count) {
    for (size_t k = 0; k < count; ++k) {
        Stream *st = done[k].token;
        st->out = false;
        mark_ready(s, st);
        if (!done[k].stored) continue; /* not saved: handed over again */
        if (!st->known || done[k].heard > st->heard) st->heard = done[k].heard;
        if (!st->known) st->line_first = st->heard + 1; /* the line begins */
        line_drop(st, st->heard);
        while (st->held_count && held_at(st, 0)->seq <= st->heard) drop_first(st);
        st->known = true;
        st->gap = done[k].gap;
        st->stored_due = true;
        if (st->run && st->run->used && st->run_id == st->run->run) mark_due(s, st->run);
        s->counts.stored = st->heard;
    }
}

/* The streams ready, each handed over; one the host could not take now is
 * tried again at the next turn. */
static void hand_over_ready(PureLinkReceiver *s) {
    size_t count = s->ready_count;
    for (size_t k = 0; k < count; ++k) {
        Stream *st = s->ready[k];
        st->ready = false;
        if (!hand_over(s, st)) mark_ready(s, st);
    }
    memmove(s->ready, s->ready + count, (s->ready_count - count) * sizeof(*s->ready));
    s->ready_count -= count;
}

void pure_link_receiver_turn(PureLinkReceiver *s, const PureLinkDone *done, size_t count, uint64_t now,
                             PureLinkSend *send, void *context) {
    if (!s->swept) s->swept = s->trimmed = now;
    apply_done(s, done, count);
    hand_over_ready(s);
    answer(s, now, send, context);
    sweep(s, now);
}

uint64_t pure_link_receiver_due(const PureLinkReceiver *s) {
    return s->active_count || s->ready_count ? s->swept + SWEEP_EVERY : 0;
}

void pure_link_receiver_counts(const PureLinkReceiver *s, PureLinkCounts *counts) {
    *counts = s->counts;
    counts->active = s->active_count;
}

void pure_link_receiver_free(PureLinkReceiver *s) {
    if (!s) return;
    while (s->active_count) let_go(s, s->actives[0]);
    pure_aead_free(s->aead);
    if (s->memo) munmap(s->memo, s->memo_size);
    free(s->actives);
    free(s->peers);
    free(s->index);
    free(s->allowed);
    free(s->names);
    free(s->ready);
    free(s->due);
    free(s->batch);
    free(s);
}
