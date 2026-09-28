#define _GNU_SOURCE
/* Links without connections (serve/link.h, docs/design/011-pure-transport.md). */
#include "link.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/udp.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

enum { VERSION = 0x70, HEADER = 21, DATAGRAM = 1200, BODY = DATAGRAM - HEADER - PURE_AEAD_TAG, PIECE = 1000 };
enum { F_PADDING = 0, F_STREAM = 1, F_RECORD = 2, F_ACK = 3, F_STORED = 4, F_PING = 5, F_RECORDS = 6, STORED_GAP = 1 };
enum { RECORDS_HEAD = 10 }; /* type, the first number, the count */
/* Packets sent at once with UDP GSO: each but the last is a whole datagram
 * (a full one padded), and 54 of them fit a UDP datagram's 65,507 bytes. */
enum { BURST = 54 };
#ifndef UDP_SEGMENT
#define UDP_SEGMENT 103
#endif
#ifndef UDP_GRO
#define UDP_GRO 104
#endif
enum { RANGES = 32, RUNS = 2, UNITS = 64, CREDIT = 1024, BATCH = 256, RECORD_HEAD = 19 };
enum { WINDOW_FIRST = 10 * DATAGRAM, WINDOW_LEAST = 2 * DATAGRAM };
#define MS UINT64_C(1000000)
/* RFC 9002's values, but a first RTT of 100 ms (a LAN's, not the 333 ms of
 * the Internet at large); a probe at least every second while a receiver is
 * away; a record received but not stored after a second is sent again. */
static const uint64_t FIRST_RTT = 100 * MS, ACK_DELAY_MOST = 25 * MS, GRANULARITY = 1 * MS, PTO_MOST = 1000 * MS,
                      STORED_WAIT = 1000 * MS, RECORD_MOST = UINT64_C(64) << 20, HELD_MOST = UINT64_C(16) << 20;

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

static uint64_t now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

static uint32_t fnv(uint32_t h, const void *bytes, size_t size) {
    const unsigned char *p = bytes;
    for (size_t i = 0; i < size; ++i) h = (h ^ p[i]) * 16777619u;
    return h;
}

static uint32_t key_id(const unsigned char key[PURE_SEAL_KEY]) { return fnv(2166136261u, key, PURE_SEAL_KEY); }

/* What a message of input looks like: its fields' names, types and places. */
static uint32_t shape_of(const PureModule *m, uint32_t input) {
    const PureInputShape *s = &m->input[input];
    uint32_t h = fnv(2166136261u, &s->prefix, sizeof(s->prefix));
    for (uint32_t f = 0; f < s->field_count; ++f) {
        const PureField *field = &m->field[s->field_start + f];
        unsigned char fixed[7] = {field->type, (unsigned char)field->rows, (unsigned char)(field->rows >> 8)};
        put32(fixed + 3, field->offset);
        h = fnv(fnv(h, field->name, strlen(field->name) + 1), fixed, sizeof(fixed));
    }
    return h;
}

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
static size_t get_names(const unsigned char *p, size_t size, char source[PURE_SOURCE + 1], char input[PURE_MODULE_NAME]) {
    if (size < 1 || !p[0] || p[0] > PURE_SOURCE || size < 2u + p[0]) return 0;
    size_t a = p[0], b = p[1 + a];
    if (!b || b >= PURE_MODULE_NAME || size < 2 + a + b) return 0;
    memcpy(source, p + 1, a);
    source[a] = 0;
    memcpy(input, p + 2 + a, b);
    input[b] = 0;
    return 2 + a + b;
}

/* Datagrams taken from a socket at once (recvmmsg). */
enum { TAKE = 64, SLOT = DATAGRAM + 64 };
typedef struct {
    unsigned char bytes[TAKE][SLOT];
    struct sockaddr_storage from[TAKE];
    struct iovec parts[TAKE];
    struct mmsghdr messages[TAKE];
} Taking;

/* With UDP GRO the kernel hands a listener the datagrams of one sender that
 * came one after another as one buffer, each the segment's size but the
 * last (a burst sent with GSO comes whole, not cut up): up to GATHER such
 * buffers at once. */
enum { GATHER = 8, GATHERED = 65536 };
typedef struct {
    unsigned char bytes[GATHER][GATHERED];
    struct sockaddr_storage from[GATHER];
    struct iovec parts[GATHER];
    struct mmsghdr messages[GATHER];
    _Alignas(8) unsigned char control[GATHER][64]; /* a cmsghdr's alignment */
} Gathering;

static int gather(int fd, Gathering *g) {
    for (unsigned k = 0; k < GATHER; ++k) {
        g->parts[k] = (struct iovec){g->bytes[k], GATHERED};
        g->messages[k] = (struct mmsghdr){.msg_hdr = {.msg_name = &g->from[k], .msg_namelen = sizeof(g->from[k]),
                                                      .msg_iov = &g->parts[k], .msg_iovlen = 1,
                                                      .msg_control = g->control[k],
                                                      .msg_controllen = sizeof(g->control[k])}};
    }
    int n = recvmmsg(fd, g->messages, GATHER, MSG_DONTWAIT, NULL);
    return n > 0 ? n : 0;
}

/* The size of each datagram in a buffer gathered (all of it when one). */
static size_t segment_of(struct msghdr *m, size_t length) {
    for (struct cmsghdr *c = CMSG_FIRSTHDR(m); c; c = CMSG_NXTHDR(m, c))
        if (c->cmsg_level == SOL_UDP && c->cmsg_type == UDP_GRO) {
            int size;
            memcpy(&size, CMSG_DATA(c), sizeof(size));
            if (size > 0) return (size_t)size;
        }
    return length ? length : 1;
}

/* Up to TAKE datagrams from fd: how many (0 when none waits). */
static int take_datagrams(int fd, Taking *t) {
    for (unsigned k = 0; k < TAKE; ++k) {
        t->parts[k] = (struct iovec){t->bytes[k], SLOT};
        t->messages[k] = (struct mmsghdr){.msg_hdr = {.msg_name = &t->from[k], .msg_namelen = sizeof(t->from[k]),
                                                      .msg_iov = &t->parts[k], .msg_iovlen = 1}};
    }
    int n = recvmmsg(fd, t->messages, TAKE, MSG_DONTWAIT, NULL);
    return n > 0 ? n : 0;
}

/* ---- The sending side */

enum { UNSENT, FLYING, RECEIVED };
typedef struct { uint64_t pn; uint8_t state; } Piece;
typedef struct { uint64_t seq; unsigned char *bytes; uint32_t size, pieces; Piece *piece; } Record;
typedef struct { uint64_t seq; uint32_t piece; } Unit;
typedef struct {
    uint64_t pn, sent; /* its number; when it went (ns) */
    uint32_t bytes;
    bool eliciting, done; /* acknowledged or lost */
    uint16_t count;
    Unit units[UNITS];
} Flight;

struct PureLink {
    const PureModule *module;
    uint32_t output, shape, own_id, peer_id;
    char host[256], port[8], input[PURE_MODULE_NAME], source[PURE_SOURCE + 1];
    const PureSealKey *own;
    unsigned char own_public[PURE_SEAL_KEY], peer[PURE_SEAL_KEY], secret[32];
    bool peered;
    pthread_mutex_t lock;
    Record *records; /* the sends not yet stored, oldest first (a ring of consecutive numbers) */
    size_t head, count, capacity;
    uint64_t queued, stored, told; /* the last queued; saved by the receiver; told the owner */
    bool stopping, started;
    /* The thread sleeps (in poll), and has been woken since; records were
     * queued since it last sent. A queue wakes it only when it sleeps, once. */
    bool sleeping, signaled, queued_new;
    int fd, wake;
    pthread_t thread;
    void (*notify)(void *);
    void *context;
    /* The thread's own, under the lock. */
    uint64_t run, next_pn, in_run;
    PureAead *out, *in;
    unsigned char out_base[PURE_AEAD_NONCE], in_base[PURE_AEAD_NONCE];
    Flight *flights; /* one for each number sent, oldest first (a ring) */
    size_t flight_head, flight_count, flight_capacity;
    uint64_t in_flight, window, threshold, recovery_start, eliciting;
    uint64_t largest_acked, latest_rtt, smoothed_rtt, rtt_var, min_rtt;
    bool acked_any, sampled;
    uint64_t loss_time, last_eliciting, credit, stored_changed;
    uint64_t received;    /* pieces received and not yet stored */
    uint64_t unsent_from; /* no record before this number has a piece unsent */
    Taking *taking;       /* answers, received at once */
    unsigned char *burst; /* packets sealed and not yet sent (BURST datagrams) */
    size_t burst_size;
    unsigned burst_count;
    uint64_t burst_pn[BURST];
    bool gso;
    unsigned pto_count, probes;
    PureLinkCounts counts;
};

PureLink *pure_link_open(const PureModule *m, const char *output, const char *url, const char *source,
                         const PureSealKey *own, const unsigned char own_public[PURE_SEAL_KEY], char *error,
                         size_t size) {
    int index = pure_module_input(m, output);
    if (index < 0 || m->input[index].kind != PURE_KIND_OUTPUT) {
        snprintf(error, size, "no output '%s'", output);
        return NULL;
    }
    if (!own) {
        snprintf(error, size, "a pure:// link needs the node's --key");
        return NULL;
    }
    size_t length = strlen(source);
    const char *host = url + 7, *slash = strchr(host, '/'), *colon = slash ? memchr(host, ':', (size_t)(slash - host)) : NULL;
    if (!length || length > PURE_SOURCE || strncmp(url, "pure://", 7) || !slash || !colon || colon == host ||
        (size_t)(colon - host) >= 256 || slash - colon - 1 < 1 || slash - colon - 1 > 5 || !slash[1] ||
        strlen(slash + 1) >= PURE_MODULE_NAME) {
        snprintf(error, size, "a link is pure://host:port/INPUT");
        return NULL;
    }
    PureLink *l = calloc(1, sizeof(*l));
    if (!l || pthread_mutex_init(&l->lock, NULL)) {
        free(l);
        snprintf(error, size, "out of memory");
        return NULL;
    }
    l->module = m;
    l->output = (uint32_t)index;
    l->shape = shape_of(m, (uint32_t)index);
    memcpy(l->host, host, (size_t)(colon - host));
    memcpy(l->port, colon + 1, (size_t)(slash - colon - 1));
    strcpy(l->input, slash + 1);
    strcpy(l->source, source);
    l->own = own;
    memcpy(l->own_public, own_public, PURE_SEAL_KEY);
    l->own_id = key_id(own_public);
    l->fd = l->wake = -1;
    return l;
}

void pure_link_peer(PureLink *l, const unsigned char peer[PURE_SEAL_KEY]) {
    memcpy(l->peer, peer, PURE_SEAL_KEY);
    l->peer_id = key_id(peer);
    l->peered = true;
}

uint32_t pure_link_output(const PureLink *l) { return l->output; }

static Record *record_at(PureLink *l, size_t k) { return &l->records[(l->head + k) % l->capacity]; }

/* The queued record numbered seq, or NULL (stored already, or not queued). */
static Record *record_of(PureLink *l, uint64_t seq) {
    if (!l->count || seq < record_at(l, 0)->seq || seq - record_at(l, 0)->seq >= l->count) return NULL;
    return record_at(l, (size_t)(seq - record_at(l, 0)->seq));
}

/* Every change of a piece's state goes through here: the link knows at once
 * how many pieces wait to be stored, and where the unsent ones begin. */
static void set_piece(PureLink *l, Record *r, uint32_t i, uint8_t state) {
    uint8_t was = r->piece[i].state;
    if (was == state) return;
    if (was == RECEIVED) --l->received;
    if (state == RECEIVED) ++l->received;
    if (state == UNSENT && r->seq < l->unsent_from) l->unsent_from = r->seq;
    r->piece[i].state = state;
}

static void wake(int fd) {
    uint64_t one = 1;
    if (fd >= 0 && write(fd, &one, sizeof(one)) != sizeof(one)) { /* full: it is readable already */ }
}

bool pure_link_queue(PureLink *l, uint64_t seq, PureSpan payload) {
    Record r = {.seq = seq, .size = (uint32_t)payload.size};
    r.pieces = payload.size ? (uint32_t)((payload.size + PIECE - 1) / PIECE) : 1;
    if (payload.size > RECORD_MOST || !(r.bytes = malloc(payload.size ? payload.size : 1)) ||
        !(r.piece = calloc(r.pieces, sizeof(*r.piece)))) {
        free(r.bytes);
        return false;
    }
    if (payload.size) memcpy(r.bytes, payload.data, payload.size);
    pthread_mutex_lock(&l->lock);
    bool ok = seq > l->queued && (!l->count || seq == l->queued + 1);
    if (ok && l->count == l->capacity) {
        size_t capacity = l->capacity ? 2 * l->capacity : 64;
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
    bool signal = false;
    if (ok) {
        *record_at(l, l->count++) = r;
        l->queued = seq;
        l->queued_new = true;
        signal = l->sleeping && !l->signaled;
        l->signaled = l->signaled || signal;
    }
    pthread_mutex_unlock(&l->lock);
    if (!ok) {
        free(r.bytes);
        free(r.piece);
        return false;
    }
    if (signal) wake(l->wake);
    return true;
}

uint64_t pure_link_queued(PureLink *l) {
    pthread_mutex_lock(&l->lock);
    uint64_t queued = l->queued;
    pthread_mutex_unlock(&l->lock);
    return queued;
}

uint64_t pure_link_delivered(PureLink *l) {
    pthread_mutex_lock(&l->lock);
    uint64_t stored = l->stored;
    pthread_mutex_unlock(&l->lock);
    return stored;
}

static Flight *flight_of(PureLink *l, uint64_t pn) {
    if (!l->flight_count) return NULL;
    uint64_t first = l->flights[l->flight_head].pn;
    if (pn < first || pn - first >= l->flight_count) return NULL;
    return &l->flights[(l->flight_head + (size_t)(pn - first)) % l->flight_capacity];
}

static bool push_flight(PureLink *l, const Flight *f) {
    if (l->flight_count == l->flight_capacity) {
        size_t capacity = l->flight_capacity ? 2 * l->flight_capacity : 256;
        Flight *grown = malloc(capacity * sizeof(*grown));
        if (!grown) return false;
        for (size_t k = 0; k < l->flight_count; ++k) grown[k] = l->flights[(l->flight_head + k) % l->flight_capacity];
        free(l->flights);
        l->flights = grown;
        l->flight_capacity = capacity;
        l->flight_head = 0;
    }
    l->flights[(l->flight_head + l->flight_count++) % l->flight_capacity] = *f;
    return true;
}

static void settle(PureLink *l, Flight *f) {
    f->done = true;
    if (f->eliciting) {
        --l->eliciting;
        l->in_flight -= f->bytes;
    }
}

/* A packet lost: what it carried and was not received otherwise goes again. */
static void lose(PureLink *l, Flight *f, uint64_t now) {
    settle(l, f);
    ++l->counts.lost;
    for (uint16_t u = 0; u < f->count; ++u) {
        Record *r = record_of(l, f->units[u].seq);
        Piece *p = r ? &r->piece[f->units[u].piece] : NULL;
        if (p && p->state == FLYING && p->pn == f->pn) {
            set_piece(l, r, f->units[u].piece, UNSENT);
            ++l->counts.resent;
        }
    }
    if (f->sent > l->recovery_start) { /* one congestion event per round trip */
        l->recovery_start = now;
        l->window = most(l->window / 2, WINDOW_LEAST);
        l->threshold = l->window;
    }
}

static void detect_lost(PureLink *l, uint64_t now) {
    l->loss_time = 0;
    if (!l->acked_any) return;
    uint64_t rtt = most(l->latest_rtt, l->sampled ? l->smoothed_rtt : FIRST_RTT);
    uint64_t delay = most(rtt * 9 / 8, GRANULARITY);
    for (size_t k = 0; k < l->flight_count; ++k) {
        Flight *f = &l->flights[(l->flight_head + k) % l->flight_capacity];
        if (f->pn >= l->largest_acked) break;
        if (f->done) continue;
        if (f->sent + delay <= now || l->largest_acked >= f->pn + 3) lose(l, f, now);
        else if (!l->loss_time || f->sent + delay < l->loss_time) l->loss_time = f->sent + delay;
    }
}

static void sample_rtt(PureLink *l, uint64_t latest, uint64_t ack_delay) {
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

static void on_ack(PureLink *l, const unsigned char *p, size_t size, uint64_t now) {
    if (size < 13 || size < 13 + 16u * p[12]) return;
    uint64_t largest = get64(p), delay = (uint64_t)get32(p + 8) * 1000;
    Flight *top = NULL;
    for (unsigned k = 0; k < p[12]; ++k) {
        uint64_t first = get64(p + 13 + 16 * k), last = get64(p + 21 + 16 * k);
        if (first > last || !l->flight_count) continue;
        uint64_t oldest = l->flights[l->flight_head].pn, newest = oldest + l->flight_count - 1;
        for (uint64_t pn = most(first, oldest); pn <= least(last, newest); ++pn) {
            Flight *f = flight_of(l, pn);
            if (!f || f->done) continue;
            settle(l, f);
            ++l->counts.acknowledged;
            for (uint16_t u = 0; u < f->count; ++u) {
                Record *r = record_of(l, f->units[u].seq);
                if (r && r->piece[f->units[u].piece].state != RECEIVED) {
                    set_piece(l, r, f->units[u].piece, RECEIVED);
                    l->stored_changed = now; /* waits for its save from now */
                }
            }
            if (f->eliciting && f->sent > l->recovery_start)
                l->window += l->window < l->threshold ? f->bytes : (uint64_t)DATAGRAM * f->bytes / l->window;
            if (pn == largest) top = f;
        }
    }
    if (!l->acked_any || largest > l->largest_acked) l->largest_acked = largest;
    l->acked_any = true;
    if (top && top->eliciting) sample_rtt(l, now - top->sent, delay);
    l->pto_count = 0;
    detect_lost(l, now);
    while (l->flight_count && l->flights[l->flight_head].done) {
        l->flight_head = (l->flight_head + 1) % l->flight_capacity;
        --l->flight_count;
    }
}

static void on_stored(PureLink *l, const unsigned char *p, size_t size, uint64_t now) {
    char source[PURE_SOURCE + 1], input[PURE_MODULE_NAME];
    size_t names = get_names(p, size, source, input);
    if (!names || size < names + 13 || strcmp(source, l->source) || strcmp(input, l->input)) return;
    uint64_t heard = get64(p + names);
    unsigned flags = p[names + 8];
    uint32_t credit = get32(p + names + 9);
    if (heard > l->stored) {
        while (l->count && record_at(l, 0)->seq <= heard) {
            Record *r = record_at(l, 0);
            for (uint32_t i = 0; i < r->pieces; ++i) l->received -= r->piece[i].state == RECEIVED;
            free(r->bytes);
            free(r->piece);
            l->head = (l->head + 1) % l->capacity;
            --l->count;
        }
        l->stored = heard;
        l->stored_changed = now;
    }
    if (flags & STORED_GAP) /* it did not keep what came after a gap: send it again */
        for (size_t k = 0; k < l->count && l->received; ++k) {
            Record *r = record_at(l, k);
            for (uint32_t i = 0; i < r->pieces; ++i)
                if (r->piece[i].state == RECEIVED) set_piece(l, r, i, UNSENT);
        }
    l->credit = credit ? least(credit, 65536) : 1;
}

static void take_answer(PureLink *l, unsigned char *packet, ssize_t n, uint64_t now);

static void take_answers(PureLink *l, uint64_t now) {
    for (unsigned rounds = 0, n = TAKE; rounds < 4 && n == TAKE; ++rounds) {
        n = (unsigned)take_datagrams(l->fd, l->taking);
        for (unsigned k = 0; k < n; ++k) take_answer(l, l->taking->bytes[k], l->taking->messages[k].msg_len, now);
    }
}

static void take_answer(PureLink *l, unsigned char *packet, ssize_t n, uint64_t now) {
    unsigned char plain[DATAGRAM], nonce[PURE_AEAD_NONCE];
    if (n < HEADER + PURE_AEAD_TAG || (size_t)n > DATAGRAM || packet[0] != VERSION || get32(packet + 1) != l->peer_id)
        return; /* not ours, or an error from an earlier send (nobody listening) */
    uint64_t run = get64(packet + 5), pn = get64(packet + 13);
    if (!l->in || l->in_run != run) {
        pure_aead_free(l->in);
        l->in = aead_of(l->secret, l->peer, l->own_public, run, l->run, l->in_base);
        l->in_run = run;
        if (!l->in) return;
    }
    nonce_of(nonce, l->in_base, pn);
    size_t size = (size_t)n - HEADER - PURE_AEAD_TAG;
    if (!pure_aead_open(l->in, NULL, nonce, packet, HEADER, packet + HEADER, (size_t)n - HEADER, plain)) return;
    ++l->counts.received;
    for (size_t at = 0; at < size;) {
        unsigned type = plain[at++];
        if (type == F_ACK && at + 13 <= size && at + 13 + 16u * plain[at + 12] <= size) {
            on_ack(l, plain + at, size - at, now);
            at += 13 + 16u * plain[at + 12];
        } else if (type == F_STORED) {
            char source[PURE_SOURCE + 1], input[PURE_MODULE_NAME];
            size_t names = get_names(plain + at, size - at, source, input);
            if (!names || at + names + 13 > size) break;
            on_stored(l, plain + at, size - at, now);
            at += names + 13;
        } else if (type != F_PING)
            break; /* padding, or what this side does not know: the rest */
    }
}

/* A packet of pieces not sent yet, in order, within the receiver's credit:
 * its body's size, or 0 when there is none. */
static size_t fill(PureLink *l, unsigned char *body, Flight *f) {
    size_t at = 0;
    body[at++] = F_STREAM;
    at += put_names(body + at, l->source, l->input);
    put32(body + at, l->shape);
    at += 4;
    /* The credit counts from the oldest record not stored: after a restart
     * the sender does not know yet how far the receiver stored. */
    uint64_t base = l->count ? record_at(l, 0)->seq - 1 : l->stored;
    /* From the first record that may have an unsent piece: the ones before
     * it are sent, and every one found sent moves that mark on. */
    size_t k = l->count && l->unsent_from > base + 1 ? (size_t)(l->unsent_from - base - 1) : 0;
    bool before = true;
    /* Whole records whose numbers follow on share a RECORDS frame (its count
     * at run); a piece of a larger one goes alone (RECORD). */
    size_t run = 0;
    uint64_t next = 0;
    for (; k < l->count && f->count < UNITS; ++k) {
        Record *r = record_at(l, k);
        if (r->seq > base + l->credit) break;
        bool unsent = false;
        for (uint32_t i = 0; i < r->pieces; ++i) {
            if (r->piece[i].state != UNSENT) continue;
            uint32_t offset = i * PIECE, length = (uint32_t)least(PIECE, r->size - offset);
            bool whole = r->pieces == 1, joins = whole && run && r->seq == next;
            size_t need = whole ? (joins ? 0 : RECORDS_HEAD) + varint_size(length) + length : RECORD_HEAD + length;
            if (f->count == UNITS || at + need > BODY) {
                unsent = true;
                break;
            }
            if (whole) {
                if (!joins) {
                    body[at] = F_RECORDS;
                    put64(body + at + 1, r->seq);
                    body[at + 9] = 0;
                    run = at + 9;
                    at += RECORDS_HEAD;
                }
                at += put_varint(body + at, length);
                if (length) memcpy(body + at, r->bytes, length);
                at += length;
                ++body[run];
                next = r->seq + 1;
            } else {
                body[at] = F_RECORD;
                put64(body + at + 1, r->seq);
                put32(body + at + 9, r->size);
                put32(body + at + 13, offset);
                put16(body + at + 17, length);
                if (length) memcpy(body + at + RECORD_HEAD, r->bytes + offset, length);
                at += RECORD_HEAD + length;
                run = 0;
            }
            set_piece(l, r, i, FLYING);
            r->piece[i].pn = f->pn;
            f->units[f->count++] = (Unit){r->seq, i};
            ++l->counts.records;
        }
        if (before && !unsent) l->unsent_from = r->seq + 1;
        before = before && !unsent;
        if (unsent) { /* the packet is full: whole, so it can go in a burst */
            if (l->gso && f->count) {
                memset(body + at, F_PADDING, BODY - at);
                at = BODY;
            }
            break;
        }
    }
    return f->count ? at : 0;
}

/* Seal and send f with body; its number is spent even when the socket
 * refuses it (what it carried is then marked unsent again). */
/* A packet the socket refused: what it carried is unsent again, and it
 * counts as never sent. Its number is spent all the same. */
static void refused(PureLink *l, uint64_t pn) {
    Flight *f = flight_of(l, pn);
    if (!f || f->done) return;
    for (uint16_t u = 0; u < f->count; ++u) {
        Record *r = record_of(l, f->units[u].seq);
        if (r && r->piece[f->units[u].piece].pn == pn) set_piece(l, r, f->units[u].piece, UNSENT);
    }
    settle(l, f);
    f->eliciting = false;
    f->count = 0;
    --l->counts.packets;
}

static bool transient(void) { return errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOBUFS || errno == EINTR; }

/* The packets of the burst to the socket (each but the last a whole
 * datagram): with GSO in one call, else one by one. */
static void flush(PureLink *l) {
    unsigned n = l->burst_count, sent = 0;
    bool whole = n > 1 && l->gso;
    if (whole && send(l->fd, l->burst, l->burst_size, 0) >= 0) sent = n;
    else if (whole && !transient()) l->gso = false, whole = false; /* no GSO here: one by one from now on */
    for (size_t at = 0; !whole && sent < n; ++sent) {
        size_t size = sent + 1 < n ? DATAGRAM : l->burst_size - at;
        if (send(l->fd, l->burst + at, size, 0) < 0 && transient()) break;
        at += size;
    }
    for (unsigned k = sent; k < n; ++k) refused(l, l->burst_pn[k]);
    l->burst_count = 0;
    l->burst_size = 0;
}

/* Seal f with body into the burst (a whole datagram each but the last), and
 * count it sent; its number is spent even when the socket then refuses it. */
static bool transmit(PureLink *l, Flight *f, const unsigned char *body, size_t size, uint64_t now) {
    if (l->burst_count && l->burst_size != (size_t)l->burst_count * DATAGRAM) flush(l); /* the last was short */
    size_t total = seal_packet(l->out, NULL, l->out_base, l->own_id, l->run, f->pn, body, size, l->burst + l->burst_size);
    ++l->next_pn;
    f->sent = now;
    f->bytes = (uint32_t)total;
    if (!total) { /* not sealed: what it carried is unsent again */
        for (uint16_t u = 0; u < f->count; ++u) {
            Record *r = record_of(l, f->units[u].seq);
            if (r && r->piece[f->units[u].piece].pn == f->pn) set_piece(l, r, f->units[u].piece, UNSENT);
        }
        f->done = true;
        f->eliciting = false;
        f->count = 0;
        push_flight(l, f);
        return false;
    }
    if (!push_flight(l, f)) return false;
    l->burst_pn[l->burst_count++] = f->pn;
    l->burst_size += total;
    ++l->counts.packets;
    if (f->eliciting) {
        ++l->eliciting;
        l->in_flight += f->bytes;
        l->last_eliciting = now;
    }
    if (l->burst_count == BURST) flush(l);
    return true;
}

static void send_packets(PureLink *l, uint64_t now) {
    unsigned char body[BODY];
    l->queued_new = false; /* all queued is looked at now */
    for (unsigned n = 0; n < 128; ++n) {
        bool probe = l->probes > 0;
        if (!probe && l->in_flight + DATAGRAM > l->window) break;
        Flight f = {.pn = l->next_pn, .eliciting = true};
        size_t size = fill(l, body, &f);
        if (!size) {
            if (!probe) break;
            body[0] = F_PING; /* nothing to send again: a probe all the same */
            size = 1;
        }
        if (!transmit(l, &f, body, size, now)) break;
        if (probe) --l->probes;
    }
    flush(l);
}

static uint64_t pto_period(const PureLink *l) {
    uint64_t smoothed = l->sampled ? l->smoothed_rtt : FIRST_RTT, var = l->sampled ? l->rtt_var : FIRST_RTT / 2;
    uint64_t period = smoothed + most(4 * var, GRANULARITY) + ACK_DELAY_MOST;
    for (unsigned i = 0; i < l->pto_count && period < PTO_MOST; ++i) period *= 2;
    return least(period, PTO_MOST);
}

static bool waiting_for_save(PureLink *l) { return l->received > 0; }

/* The next time a timer is due (ns), or 0. */
static uint64_t next_timer(PureLink *l) {
    uint64_t due = l->loss_time;
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

static void run_timers(PureLink *l, uint64_t now) {
    if (l->loss_time && l->loss_time <= now) detect_lost(l, now);
    else if (l->eliciting && l->last_eliciting + pto_period(l) <= now) {
        /* Nothing heard for a while: a probe, what flies longest sent again. */
        ++l->pto_count;
        l->probes = 1;
        for (size_t k = 0; k < l->count; ++k) {
            Record *r = record_at(l, k);
            uint32_t i = 0;
            while (i < r->pieces && r->piece[i].state != FLYING) ++i;
            if (i < r->pieces) {
                set_piece(l, r, i, UNSENT);
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
            for (uint32_t i = 0; i < r->pieces; ++i)
                if (r->piece[i].state == RECEIVED) set_piece(l, r, i, UNSENT), ++l->counts.resent;
        }
        l->stored_changed = now;
    }
}

static void *run_link(void *context) {
    PureLink *l = context;
    for (;;) {
        pthread_mutex_lock(&l->lock);
        uint64_t now = now_ns(), due = next_timer(l);
        bool stopping = l->stopping, more = l->queued_new; /* queued while it sent: no sleep */
        l->sleeping = !more;
        pthread_mutex_unlock(&l->lock);
        if (stopping) break;
        int timeout = more ? 0 : due ? (int)((due > now ? due - now : 0) / MS) + 1 : -1;
        struct pollfd waits[2] = {{l->fd, POLLIN, 0}, {l->wake, POLLIN, 0}};
        if (poll(waits, 2, timeout) < 0 && errno != EINTR) break;
        if (waits[1].revents & POLLIN) {
            uint64_t signals;
            if (read(l->wake, &signals, sizeof(signals)) < 0) { /* cleared already */ }
        }
        pthread_mutex_lock(&l->lock);
        l->sleeping = l->signaled = false;
        now = now_ns();
        if (waits[0].revents) take_answers(l, now);
        run_timers(l, now);
        send_packets(l, now);
        bool told = l->stored != l->told;
        l->told = l->stored;
        pthread_mutex_unlock(&l->lock);
        if (told && l->notify) l->notify(l->context);
    }
    return NULL;
}

bool pure_link_start(PureLink *l, void (*notify)(void *), void *context, char *error, size_t size) {
    if (!l->peered) {
        snprintf(error, size, "pure://%s:%s/%s: no receiver's key (--seal)", l->host, l->port, l->input);
        return false;
    }
    struct addrinfo hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_DGRAM}, *found = NULL;
    int resolved = getaddrinfo(l->host, l->port, &hints, &found);
    if (resolved) {
        snprintf(error, size, "%s: %s", l->host, gai_strerror(resolved));
        return false;
    }
    l->fd = socket(found->ai_family, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    bool ok = l->fd >= 0 && !connect(l->fd, found->ai_addr, found->ai_addrlen);
    freeaddrinfo(found);
    int buffer = 4 << 20;
    if (ok) {
        setsockopt(l->fd, SOL_SOCKET, SO_SNDBUF, &buffer, sizeof(buffer));
        setsockopt(l->fd, SOL_SOCKET, SO_RCVBUF, &buffer, sizeof(buffer));
    }
    int segment = DATAGRAM;
    l->gso = ok && !setsockopt(l->fd, SOL_UDP, UDP_SEGMENT, &segment, sizeof(segment));
    ok = ok && (l->burst = malloc((size_t)BURST * DATAGRAM)) && (l->taking = malloc(sizeof(*l->taking)));
    l->run = random64();
    l->window = WINDOW_FIRST;
    l->threshold = UINT64_MAX;
    l->credit = CREDIT;
    ok = ok && pure_link_secret(l->own, l->peer, l->secret) &&
         (l->out = aead_of(l->secret, l->own_public, l->peer, l->run, 0, l->out_base)) &&
         (l->wake = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)) >= 0;
    l->notify = notify;
    l->context = context;
    if (!ok || pthread_create(&l->thread, NULL, run_link, l)) {
        snprintf(error, size, "pure://%s:%s/%s: cannot start (a socket, the keys)", l->host, l->port, l->input);
        return false;
    }
    pthread_setname_np(l->thread, "pure-link"); /* a link's CPU, seen apart */
    l->started = true;
    return true;
}

void pure_link_counts(PureLink *l, PureLinkCounts *out) {
    pthread_mutex_lock(&l->lock);
    *out = l->counts;
    out->stored = l->stored;
    pthread_mutex_unlock(&l->lock);
}

void pure_link_close(PureLink *l) {
    if (!l) return;
    if (l->started) {
        pthread_mutex_lock(&l->lock);
        l->stopping = true;
        pthread_mutex_unlock(&l->lock);
        wake(l->wake);
        pthread_join(l->thread, NULL);
    }
    for (size_t k = 0; k < l->count; ++k) {
        free(record_at(l, k)->bytes);
        free(record_at(l, k)->piece);
    }
    free(l->records);
    free(l->flights);
    free(l->burst);
    free(l->taking);
    pure_aead_free(l->out);
    pure_aead_free(l->in);
    explicit_bzero(l->secret, sizeof(l->secret));
    if (l->fd >= 0) close(l->fd);
    if (l->wake >= 0) close(l->wake);
    pthread_mutex_destroy(&l->lock);
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
    bool used, ack_due, due;
    unsigned char in_key[PURE_AEAD_KEY], out_key[PURE_AEAD_KEY], in_base[PURE_AEAD_NONCE], out_base[PURE_AEAD_NONCE];
    uint64_t next_pn, largest_at;
    struct sockaddr_in6 address; /* or an IPv4 one */
    socklen_t address_size;
    Range got[RANGES]; /* the numbers received, newest first */
    unsigned got_count;
} Run;
typedef struct {
    uint64_t seq;
    unsigned char *bytes; /* its size, then a byte a piece: had */
    uint32_t size, pieces, got;
} Held;
typedef struct Stream {
    Active *active;
    char source[PURE_SOURCE + 1];
    uint32_t input;
    uint64_t heard;              /* the last number heard (stored), once known */
    bool known, out, stored_due, gap, ready;
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
typedef struct { Stream *stream; uint64_t heard; bool stored, gap; } Done;

/* A peer quiet this long lets its active state go; the active are looked at
 * that often; what they held goes back to the system once TRIM_AFTER have
 * gone, or TRIM_WAIT after any went. */
static const uint64_t IDLE_FIRST = 30000 * MS, SWEEP_EVERY = 250 * MS, TRIM_WAIT = 10000 * MS;
enum { TRIM_AFTER = 1024 };

struct PureLinkListener {
    const PureModule *module;
    const PureSealKey *own;
    unsigned char own_public[PURE_SEAL_KEY];
    uint32_t own_id;
    unsigned port;
    uint64_t run, idle, swept, trimmed, untrimmed;
    PureAead *aead; /* keyed by each packet's run */
    unsigned char open_key[PURE_AEAD_KEY], seal_key[PURE_AEAD_KEY]; /* the keys it has now */
    bool open_keyed, seal_keyed;
    unsigned char *memo; /* the secrets remembered (pure_link_remember), mapped */
    size_t memo_size;
    int fd, wake;
    pthread_t thread;
    bool started;
    pthread_mutex_t lock; /* the done list and stopping */
    Done *done;
    size_t done_count, done_capacity;
    bool stopping;
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
    bool (*deliver)(void *, const PureLinkDelivery *);
    void *context;
    unsigned char *batch;
    size_t batch_capacity;
    PureLinkCounts counts;
};

PureLinkListener *pure_link_listen(const PureModule *m, unsigned port, const PureSealKey *own,
                                   const unsigned char own_public[PURE_SEAL_KEY],
                                   bool (*deliver)(void *, const PureLinkDelivery *), void *context, char *error,
                                   size_t size) {
    PureLinkListener *s = own ? calloc(1, sizeof(*s)) : NULL;
    if (!s || pthread_mutex_init(&s->lock, NULL)) {
        free(s);
        snprintf(error, size, own ? "out of memory" : "--link-port needs the node's --key");
        return NULL;
    }
    s->module = m;
    s->own = own;
    memcpy(s->own_public, own_public, PURE_SEAL_KEY);
    s->own_id = key_id(own_public);
    s->port = port;
    s->idle = IDLE_FIRST;
    s->deliver = deliver;
    s->context = context;
    s->fd = s->wake = -1;
    return s;
}

void pure_link_idle(PureLinkListener *s, uint64_t ms) { s->idle = ms * MS; }

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
static bool grow_index(PureLinkListener *s) {
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

static bool same_name(const PureLinkListener *s, uint32_t name, const char *source, size_t length) {
    return s->names[name] == length && !memcmp(s->names + name + 1, source, length);
}

/* Keys are found by their ids, which are 32 bits: two keys may share one. */
bool pure_link_allow(PureLinkListener *s, const char *source, const unsigned char key[PURE_SEAL_KEY], char *error,
                     size_t size) {
    size_t length = strlen(source), p = SIZE_MAX;
    uint32_t id = key_id(key);
    if (!length || length > PURE_SOURCE) {
        snprintf(error, size, "a source's name is 1 to %d bytes", PURE_SOURCE);
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

/* The first record held let go: the ones after it stay where they are. */
static void drop_first(Stream *st) {
    st->held_bytes -= held_at(st, 0)->size;
    free(held_at(st, 0)->bytes);
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

bool pure_link_remember(PureLinkListener *s, const char *path, char *error, size_t size) {
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
static Active *activate(PureLinkListener *s, Peer *peer, uint64_t now) {
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
    } else if (pure_link_secret(s->own, peer->key, a->secret)) {
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

static void let_go(PureLinkListener *s, Active *a) {
    for (unsigned k = 0; k < a->stream_count; ++k) {
        while (a->streams[k]->held_count) drop_first(a->streams[k]);
        free(a->streams[k]->held);
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
        if (a->streams[k]->out || a->streams[k]->held_count || a->streams[k]->ready) return true;
    for (unsigned i = 0; i < RUNS; ++i)
        if (a->runs[i].due) return true;
    return false;
}

/* Let go the active state of peers quiet for the idle period; what idle
 * links held goes back to the system. */
static void sweep(PureLinkListener *s, uint64_t now) {
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

static void mark_ready(PureLinkListener *s, Stream *st) {
    Stream **ready = st->ready ? NULL : room(s->ready, &s->ready_capacity, s->ready_count, sizeof(*ready), 64);
    if (!ready) return;
    s->ready = ready;
    s->ready[s->ready_count++] = st;
    st->ready = true;
}

static void mark_due(PureLinkListener *s, Run *r) {
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

static bool allowed(const PureLinkListener *s, const Peer *peer, const char *source) {
    for (uint32_t a = peer->allowed; a; a = s->allowed[a - 1].next)
        if (same_name(s, s->allowed[a - 1].name, source, strlen(source))) return true;
    return false;
}

/* The stream of a peer's source to input, made the first time: NULL when the
 * source is not allowed from the peer, or input takes no such messages. */
static Stream *stream_of(PureLinkListener *s, Active *a, const char *source, const char *input, uint32_t shape) {
    int index = pure_module_input(s->module, input);
    if (index < 0) return NULL;
    for (unsigned k = 0; k < a->stream_count; ++k) {
        Stream *st = a->streams[k];
        if (st->input == (uint32_t)index && !strcmp(st->source, source)) return st;
    }
    const PureInputShape *in = &s->module->input[index];
    if (!allowed(s, &s->peers[a->peer], source) || in->kind != PURE_KIND_MESSAGE || in->keyed ||
        shape_of(s->module, (uint32_t)index) != shape)
        return NULL;
    size_t capacity = a->stream_capacity;
    Stream **streams = room(a->streams, &capacity, a->stream_count, sizeof(*streams), 2), *st;
    if (!streams) return NULL;
    a->streams = streams;
    a->stream_capacity = (unsigned)capacity;
    if (!(st = calloc(1, sizeof(*st)))) return NULL;
    st->active = a;
    st->input = (uint32_t)index;
    snprintf(st->source, sizeof(st->source), "%s", source);
    a->streams[a->stream_count++] = st;
    return st;
}

/* A record's piece, kept until the record is whole and handed over. */
static void hold(Stream *st, uint64_t seq, uint32_t size, uint32_t offset, const unsigned char *bytes, uint32_t length) {
    if (st->known && seq <= st->heard) {
        st->stored_due = true; /* sent again: say again how far it is stored */
        return;
    }
    if ((st->known && seq > st->heard + CREDIT) || size > RECORD_MOST) return;
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
        if (st->held_bytes + size > HELD_MOST) return;
        if (st->held_first && st->held_first + st->held_count == st->held_capacity) { /* the front is free: used again */
            memmove(st->held, held_at(st, 0), st->held_count * sizeof(Held));
            st->held_first = 0;
        }
        Held *kept = room(st->held, &st->held_capacity, st->held_first + st->held_count, sizeof(Held), 16);
        if (!kept) return;
        st->held = kept;
        Held h = {.seq = seq, .size = size, .pieces = size ? (size + PIECE - 1) / PIECE : 1};
        if (!(h.bytes = malloc(size + h.pieces))) return;
        memset(h.bytes + size, 0, h.pieces);
        memmove(held_at(st, k + 1), held_at(st, k), (st->held_count - k) * sizeof(Held));
        *held_at(st, k) = h;
        ++st->held_count;
        st->held_bytes += size;
    }
    Held *h = held_at(st, k);
    uint32_t piece = offset / PIECE;
    if (h->size != size || offset % PIECE || piece >= h->pieces || length != least(PIECE, size - offset) ||
        h->bytes[size + piece])
        return;
    if (length) memcpy(h->bytes + offset, bytes, length);
    h->bytes[size + piece] = 1;
    ++h->got;
}

/* Whole records from the one after the last heard, in order, to the host:
 * one delivery a stream at a time. Before the last heard is known (after a
 * restart) they go from the first held: the host says how far it heard.
 * False when the host could not take them now. */
static bool hand_over(PureLinkListener *s, Stream *st) {
    if (st->out) return true;
    while (st->known && st->held_count && held_at(st, 0)->seq <= st->heard) drop_first(st);
    if (!st->held_count || (st->known && held_at(st, 0)->seq != st->heard + 1)) return true;
    size_t n = 0, bytes = 0;
    while (n < st->held_count && n < BATCH && held_at(st, n)->got == held_at(st, n)->pieces &&
           held_at(st, n)->seq == held_at(st, 0)->seq + n)
        bytes += 4 + held_at(st, n++)->size;
    if (!n) return true;
    if (bytes > s->batch_capacity) {
        unsigned char *grown = realloc(s->batch, bytes);
        if (!grown) return false;
        s->batch = grown;
        s->batch_capacity = bytes;
    }
    size_t at = 0;
    for (size_t k = 0; k < n; ++k) {
        put32(s->batch + at, held_at(st, k)->size);
        memcpy(s->batch + at + 4, held_at(st, k)->bytes, held_at(st, k)->size);
        at += 4 + held_at(st, k)->size;
    }
    PureLinkDelivery d = {st->input, st->source, held_at(st, 0)->seq, (unsigned)n, s->batch, bytes, st};
    if (!s->deliver(s->context, &d)) return false;
    st->out = true;
    ++s->counts.deliveries;
    return true;
}

/* A packet from peer, if it opens with the keys of peer's run (made for a
 * run not seen yet, and for a peer at rest its active state): its frames. */
static bool take_from(PureLinkListener *s, Peer *peer, unsigned char *packet, size_t n,
                      const struct sockaddr_storage *from, socklen_t from_size, uint64_t now) {
    uint64_t pn = get64(packet + 13), run = get64(packet + 5);
    Active *a = peer->active;
    Run *r = a ? run_found(a, run) : NULL;
    if (r && had(r, pn)) {
        r->ack_due = true; /* its acknowledgment was lost, perhaps */
        mark_due(s, r);
        return true;
    }
    bool woke = !a;
    if (woke && !(a = activate(s, peer, now))) return false;
    Run made = {.active = a, .run = run, .seen = now, .used = true};
    bool keyed = r || (derive(a->secret, peer->key, s->own_public, run, 0, made.in_key, made.in_base) &&
                       derive(a->secret, s->own_public, peer->key, s->run, run, made.out_key, made.out_base));
    unsigned char plain[DATAGRAM], nonce[PURE_AEAD_NONCE];
    nonce_of(nonce, r ? r->in_base : made.in_base, pn);
    size_t size = n - HEADER - PURE_AEAD_TAG;
    const unsigned char *key = r ? r->in_key : made.in_key, *give = key;
    if (s->open_keyed && !memcmp(s->open_key, key, PURE_AEAD_KEY)) give = NULL; /* the key it has */
    else if (keyed) memcpy(s->open_key, key, PURE_AEAD_KEY), s->open_keyed = true;
    if (!keyed || !pure_aead_open(s->aead, give, nonce, packet, HEADER, packet + HEADER, n - HEADER, plain)) {
        explicit_bzero(&made, sizeof(made));
        if (woke) let_go(s, a);
        return false;
    }
    if (woke) ++s->counts.woken;
    if (!r) r = install_run(a, &made);
    a->seen = r->seen = now;
    note(r, pn, now);
    r->address_size = from_size <= sizeof(r->address) ? from_size : sizeof(r->address);
    memcpy(&r->address, from, r->address_size); /* where it is now: answers go there */
    ++s->counts.received;
    Stream *st = NULL;
    bool eliciting = false;
    for (size_t at = 0; at < size;) {
        unsigned type = plain[at++];
        if (type == F_STREAM) {
            char source[PURE_SOURCE + 1], input[PURE_MODULE_NAME];
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
                              length <= PIECE && length <= size - at - used; ++i) {
                if (st) hold(st, first + i, (uint32_t)length, 0, plain + at + used, (uint32_t)length);
                at += used + length;
            }
            if (st) mark_ready(s, st);
            eliciting = true;
            if (i < count) break; /* cut short: the rest is not read */
        } else if (type == F_PING) {
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

static void take_packet(PureLinkListener *s, unsigned char *packet, size_t n, const struct sockaddr_storage *from,
                        socklen_t from_size, uint64_t now) {
    if (n < HEADER + PURE_AEAD_TAG || n > DATAGRAM || packet[0] != VERSION || !s->index_size) return;
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
static void answer(PureLinkListener *s, uint64_t now) {
    unsigned char body[BODY], packet[DATAGRAM];
    for (size_t k = 0; k < s->due_count; ++k) {
        Run *r = s->due[k];
        Active *a = r->active;
        r->due = false;
        if (!r->used) continue;
        size_t at = 0;
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
            if (at + 2 + PURE_SOURCE + PURE_MODULE_NAME + 14 > BODY) break;
            body[at++] = F_STORED;
            at += put_names(body + at, st->source, s->module->input_names[st->input]);
            put64(body + at, st->heard);
            body[at + 8] = st->gap ? STORED_GAP : 0;
            size_t room = st->held_bytes < HELD_MOST ? CREDIT : 1;
            put32(body + at + 9, (uint32_t)room);
            at += 13;
            st->stored_due = st->gap = false;
        }
        r->ack_due = false;
        if (!at) continue;
        const unsigned char *give = r->out_key;
        if (s->seal_keyed && !memcmp(s->seal_key, r->out_key, PURE_AEAD_KEY)) give = NULL; /* the key it has */
        else memcpy(s->seal_key, r->out_key, PURE_AEAD_KEY), s->seal_keyed = true;
        size_t total = seal_packet(s->aead, give, r->out_base, s->own_id, s->run, r->next_pn++, body, at, packet);
        if (total && sendto(s->fd, packet, total, 0, (const struct sockaddr *)&r->address, r->address_size) >= 0)
            ++s->counts.packets;
    }
    s->due_count = 0;
}

static void take_done(PureLinkListener *s) {
    pthread_mutex_lock(&s->lock);
    Done *done = s->done;
    size_t count = s->done_count;
    s->done = NULL;
    s->done_count = s->done_capacity = 0;
    pthread_mutex_unlock(&s->lock);
    for (size_t k = 0; k < count; ++k) {
        Stream *st = done[k].stream;
        st->out = false;
        mark_ready(s, st);
        if (!done[k].stored) continue; /* not saved: handed over again */
        if (!st->known || done[k].heard > st->heard) st->heard = done[k].heard;
        st->known = true;
        st->gap = done[k].gap;
        st->stored_due = true;
        if (st->run && st->run->used && st->run_id == st->run->run) mark_due(s, st->run);
        s->counts.stored = st->heard;
    }
    free(done);
}

/* The streams ready, each handed over; one the host could not take now is
 * tried again at the next turn. */
static void hand_over_ready(PureLinkListener *s) {
    size_t count = s->ready_count;
    for (size_t k = 0; k < count; ++k) {
        Stream *st = s->ready[k];
        st->ready = false;
        if (!hand_over(s, st)) mark_ready(s, st);
    }
    memmove(s->ready, s->ready + count, (s->ready_count - count) * sizeof(*s->ready));
    s->ready_count -= count;
}

static void *run_listener(void *context) {
    PureLinkListener *s = context;
    Gathering *gathering = malloc(sizeof(*gathering));
    if (!gathering) return NULL;
    for (;;) {
        struct pollfd waits[2] = {{s->fd, POLLIN, 0}, {s->wake, POLLIN, 0}};
        int timeout = s->active_count || s->ready_count ? (int)(SWEEP_EVERY / MS) : -1;
        if (poll(waits, 2, timeout) < 0 && errno != EINTR) break;
        pthread_mutex_lock(&s->lock);
        bool stopping = s->stopping;
        pthread_mutex_unlock(&s->lock);
        if (stopping) break;
        if (waits[1].revents & POLLIN) {
            uint64_t signals;
            if (read(s->wake, &signals, sizeof(signals)) < 0) { /* cleared already */ }
        }
        uint64_t now = now_ns();
        for (unsigned rounds = 0, n = GATHER; rounds < 8 && n == GATHER; ++rounds) {
            n = (unsigned)gather(s->fd, gathering);
            for (unsigned k = 0; k < n; ++k) {
                struct mmsghdr *m = &gathering->messages[k];
                size_t length = m->msg_len, step = segment_of(&m->msg_hdr, length);
                for (size_t at = 0; at < length; at += step)
                    take_packet(s, gathering->bytes[k] + at, least(step, length - at), &gathering->from[k],
                                m->msg_hdr.msg_namelen, now);
            }
        }
        take_done(s);
        hand_over_ready(s);
        answer(s, now);
        sweep(s, now);
    }
    free(gathering);
    return NULL;
}

bool pure_link_serve(PureLinkListener *s, char *error, size_t size) {
    struct sockaddr_in6 six = {.sin6_family = AF_INET6, .sin6_port = htons((uint16_t)s->port), .sin6_addr = in6addr_any};
    int off = 0, buffer = 4 << 20;
    s->fd = socket(AF_INET6, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    bool bound = s->fd >= 0 && !setsockopt(s->fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off)) &&
                 !bind(s->fd, (struct sockaddr *)&six, sizeof(six));
    if (!bound) { /* no IPv6 here */
        if (s->fd >= 0) close(s->fd);
        struct sockaddr_in four = {.sin_family = AF_INET, .sin_port = htons((uint16_t)s->port), .sin_addr = {INADDR_ANY}};
        s->fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        bound = s->fd >= 0 && !bind(s->fd, (struct sockaddr *)&four, sizeof(four));
    }
    if (!bound) {
        snprintf(error, size, "--link-port %u: %s", s->port, strerror(errno));
        return false;
    }
    setsockopt(s->fd, SOL_SOCKET, SO_RCVBUF, &buffer, sizeof(buffer));
    setsockopt(s->fd, SOL_SOCKET, SO_SNDBUF, &buffer, sizeof(buffer));
    int one = 1; /* without GRO, a buffer is a datagram */
    setsockopt(s->fd, SOL_UDP, UDP_GRO, &one, sizeof(one));
    s->run = random64();
    s->swept = s->trimmed = now_ns();
    s->wake = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    unsigned char key[PURE_AEAD_KEY] = {0}; /* each packet brings its run's */
    if (s->wake < 0 || !(s->aead = pure_aead(key)) || pthread_create(&s->thread, NULL, run_listener, s)) {
        snprintf(error, size, "--link-port %u: cannot start", s->port);
        return false;
    }
    pthread_setname_np(s->thread, "pure-listen");
    s->started = true;
    return true;
}

void pure_link_done(PureLinkListener *s, void *token, uint64_t heard, bool stored, bool gap) {
    pthread_mutex_lock(&s->lock);
    if (s->done_count == s->done_capacity) {
        size_t capacity = s->done_capacity ? 2 * s->done_capacity : 16;
        Done *grown = realloc(s->done, capacity * sizeof(*grown));
        if (!grown) abort(); /* a delivery's word cannot be lost */
        s->done = grown;
        s->done_capacity = capacity;
    }
    s->done[s->done_count++] = (Done){token, heard, stored, gap};
    pthread_mutex_unlock(&s->lock);
    wake(s->wake);
}

void pure_link_listener_close(PureLinkListener *s, PureLinkCounts *counts) {
    if (!s) return;
    if (s->started) {
        pthread_mutex_lock(&s->lock);
        s->stopping = true;
        pthread_mutex_unlock(&s->lock);
        wake(s->wake);
        pthread_join(s->thread, NULL);
    }
    if (counts) {
        *counts = s->counts;
        counts->active = s->active_count;
    }
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
    free(s->done);
    if (s->fd >= 0) close(s->fd);
    if (s->wake >= 0) close(s->wake);
    pthread_mutex_destroy(&s->lock);
    free(s);
}
