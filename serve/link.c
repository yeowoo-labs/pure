#define _GNU_SOURCE
/* Links without connections (serve/link.h, docs/design/011-pure-transport.md). */
#include "link.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

enum { VERSION = 0x70, HEADER = 21, DATAGRAM = 1200, BODY = DATAGRAM - HEADER - PURE_AEAD_TAG, PIECE = 1000 };
enum { F_STREAM = 1, F_RECORD = 2, F_ACK = 3, F_STORED = 4, F_PING = 5, STORED_GAP = 1 };
enum { RANGES = 32, RUNS = 4, UNITS = 64, CREDIT = 1024, BATCH = 256, RECORD_HEAD = 19 };
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
static PureAead *derive(const unsigned char secret[32], const unsigned char from[PURE_SEAL_KEY],
                        const unsigned char to[PURE_SEAL_KEY], uint64_t from_run, uint64_t to_run,
                        unsigned char base[PURE_AEAD_NONCE]) {
    unsigned char info[16 + 2 * PURE_SEAL_KEY + 16], out[PURE_AEAD_KEY + PURE_AEAD_NONCE];
    memcpy(info, "pure link 0 keys", 16);
    memcpy(info + 16, from, PURE_SEAL_KEY);
    memcpy(info + 16 + PURE_SEAL_KEY, to, PURE_SEAL_KEY);
    put64(info + 16 + 2 * PURE_SEAL_KEY, from_run);
    put64(info + 24 + 2 * PURE_SEAL_KEY, to_run);
    if (!pure_link_derive(secret, info, sizeof(info), out, sizeof(out))) return NULL;
    memcpy(base, out + PURE_AEAD_KEY, PURE_AEAD_NONCE);
    PureAead *aead = pure_aead(out);
    explicit_bzero(out, sizeof(out));
    return aead;
}

static void nonce_of(unsigned char out[PURE_AEAD_NONCE], const unsigned char base[PURE_AEAD_NONCE], uint64_t pn) {
    memcpy(out, base, PURE_AEAD_NONCE);
    for (int i = 0; i < 8; ++i) out[4 + i] ^= (unsigned char)(pn >> (56 - 8 * i));
}

/* A packet: the header, then body sealed; its size, or 0. */
static size_t seal_packet(PureAead *aead, const unsigned char base[PURE_AEAD_NONCE], uint32_t id, uint64_t run,
                          uint64_t pn, const unsigned char *body, size_t size, unsigned char *out) {
    unsigned char nonce[PURE_AEAD_NONCE];
    out[0] = VERSION;
    put32(out + 1, id);
    put64(out + 5, run);
    put64(out + 13, pn);
    nonce_of(nonce, base, pn);
    return pure_aead_seal(aead, nonce, out, HEADER, body, size, out + HEADER) ? HEADER + size + PURE_AEAD_TAG : 0;
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
    if (ok) {
        *record_at(l, l->count++) = r;
        l->queued = seq;
    }
    pthread_mutex_unlock(&l->lock);
    if (!ok) {
        free(r.bytes);
        free(r.piece);
        return false;
    }
    wake(l->wake);
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
            p->state = UNSENT;
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
                    r->piece[f->units[u].piece].state = RECEIVED;
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
            free(r->bytes);
            free(r->piece);
            l->head = (l->head + 1) % l->capacity;
            --l->count;
        }
        l->stored = heard;
        l->stored_changed = now;
    }
    if (flags & STORED_GAP) /* it did not keep what came after a gap: send it again */
        for (size_t k = 0; k < l->count; ++k) {
            Record *r = record_at(l, k);
            for (uint32_t i = 0; i < r->pieces; ++i)
                if (r->piece[i].state == RECEIVED) r->piece[i].state = UNSENT;
        }
    l->credit = credit ? least(credit, 65536) : 1;
}

static void take_answers(PureLink *l, uint64_t now) {
    unsigned char packet[DATAGRAM + 64], plain[DATAGRAM];
    for (unsigned tries = 0; tries < 256; ++tries) {
        ssize_t n = recv(l->fd, packet, sizeof(packet), MSG_DONTWAIT);
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        if (n < HEADER + PURE_AEAD_TAG || (size_t)n > DATAGRAM || packet[0] != VERSION || get32(packet + 1) != l->peer_id)
            continue; /* not ours, or an error from an earlier send (nobody listening) */
        uint64_t run = get64(packet + 5), pn = get64(packet + 13);
        if (!l->in || l->in_run != run) {
            pure_aead_free(l->in);
            l->in = derive(l->secret, l->peer, l->own_public, run, l->run, l->in_base);
            l->in_run = run;
            if (!l->in) continue;
        }
        unsigned char nonce[PURE_AEAD_NONCE];
        nonce_of(nonce, l->in_base, pn);
        size_t size = (size_t)n - HEADER - PURE_AEAD_TAG;
        if (!pure_aead_open(l->in, nonce, packet, HEADER, packet + HEADER, (size_t)n - HEADER, plain)) continue;
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
            } else if (type == F_PING)
                continue;
            else
                break;
        }
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
    for (size_t k = 0; k < l->count && f->count < UNITS; ++k) {
        Record *r = record_at(l, k);
        if (r->seq > base + l->credit) break;
        for (uint32_t i = 0; i < r->pieces && f->count < UNITS; ++i) {
            if (r->piece[i].state != UNSENT) continue;
            uint32_t offset = i * PIECE, length = (uint32_t)least(PIECE, r->size - offset);
            if (at + RECORD_HEAD + length > BODY) return f->count ? at : 0;
            body[at] = F_RECORD;
            put64(body + at + 1, r->seq);
            put32(body + at + 9, r->size);
            put32(body + at + 13, offset);
            put16(body + at + 17, length);
            if (length) memcpy(body + at + RECORD_HEAD, r->bytes + offset, length);
            at += RECORD_HEAD + length;
            r->piece[i].state = FLYING;
            r->piece[i].pn = f->pn;
            f->units[f->count++] = (Unit){r->seq, i};
            ++l->counts.records;
        }
    }
    return f->count ? at : 0;
}

/* Seal and send f with body; its number is spent even when the socket
 * refuses it (what it carried is then marked unsent again). */
static bool transmit(PureLink *l, Flight *f, const unsigned char *body, size_t size, uint64_t now) {
    unsigned char packet[DATAGRAM];
    size_t total = seal_packet(l->out, l->out_base, l->own_id, l->run, f->pn, body, size, packet);
    ++l->next_pn;
    bool sent = total && (send(l->fd, packet, total, 0) >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK &&
                                                                  errno != ENOBUFS && errno != EINTR));
    f->sent = now;
    f->bytes = (uint32_t)total;
    if (!sent) {
        for (uint16_t u = 0; u < f->count; ++u) {
            Record *r = record_of(l, f->units[u].seq);
            if (r && r->piece[f->units[u].piece].pn == f->pn) r->piece[f->units[u].piece].state = UNSENT;
        }
        f->done = true;
        f->eliciting = false;
        f->count = 0;
    }
    if (!push_flight(l, f)) return false;
    if (!sent) return false;
    ++l->counts.packets;
    if (f->eliciting) {
        ++l->eliciting;
        l->in_flight += f->bytes;
        l->last_eliciting = now;
    }
    return true;
}

static void send_packets(PureLink *l, uint64_t now) {
    unsigned char body[BODY];
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
}

static uint64_t pto_period(const PureLink *l) {
    uint64_t smoothed = l->sampled ? l->smoothed_rtt : FIRST_RTT, var = l->sampled ? l->rtt_var : FIRST_RTT / 2;
    uint64_t period = smoothed + most(4 * var, GRANULARITY) + ACK_DELAY_MOST;
    for (unsigned i = 0; i < l->pto_count && period < PTO_MOST; ++i) period *= 2;
    return least(period, PTO_MOST);
}

static bool waiting_for_save(PureLink *l) {
    for (size_t k = 0; k < l->count; ++k) {
        Record *r = record_at(l, k);
        for (uint32_t i = 0; i < r->pieces; ++i)
            if (r->piece[i].state == RECEIVED) return true;
    }
    return false;
}

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
                r->piece[i].state = UNSENT;
                ++l->counts.resent;
                break;
            }
        }
    }
    if (waiting_for_save(l) && l->stored_changed + STORED_WAIT <= now) {
        /* Received long ago and still not stored: the receiver lost them (it
         * restarted) or its word got lost. Sent again: it takes a number once. */
        for (size_t k = 0; k < l->count; ++k) {
            Record *r = record_at(l, k);
            for (uint32_t i = 0; i < r->pieces; ++i)
                if (r->piece[i].state == RECEIVED) r->piece[i].state = UNSENT, ++l->counts.resent;
        }
        l->stored_changed = now;
    }
}

static void *run_link(void *context) {
    PureLink *l = context;
    for (;;) {
        pthread_mutex_lock(&l->lock);
        uint64_t now = now_ns(), due = next_timer(l);
        bool stopping = l->stopping;
        pthread_mutex_unlock(&l->lock);
        if (stopping) break;
        int timeout = due ? (int)((due > now ? due - now : 0) / MS) + 1 : -1;
        struct pollfd waits[2] = {{l->fd, POLLIN, 0}, {l->wake, POLLIN, 0}};
        if (poll(waits, 2, timeout) < 0 && errno != EINTR) break;
        if (waits[1].revents & POLLIN) {
            uint64_t signals;
            if (read(l->wake, &signals, sizeof(signals)) < 0) { /* cleared already */ }
        }
        pthread_mutex_lock(&l->lock);
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
    l->run = random64();
    l->window = WINDOW_FIRST;
    l->threshold = UINT64_MAX;
    l->credit = CREDIT;
    ok = ok && pure_link_secret(l->own, l->peer, l->secret) &&
         (l->out = derive(l->secret, l->own_public, l->peer, l->run, 0, l->out_base)) &&
         (l->wake = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)) >= 0;
    l->notify = notify;
    l->context = context;
    if (!ok || pthread_create(&l->thread, NULL, run_link, l)) {
        snprintf(error, size, "pure://%s:%s/%s: cannot start (a socket, the keys)", l->host, l->port, l->input);
        return false;
    }
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
    pure_aead_free(l->out);
    pure_aead_free(l->in);
    explicit_bzero(l->secret, sizeof(l->secret));
    if (l->fd >= 0) close(l->fd);
    if (l->wake >= 0) close(l->wake);
    pthread_mutex_destroy(&l->lock);
    free(l);
}

/* ---- The receiving side */

typedef struct { uint64_t first, last; } Range;
typedef struct {
    uint64_t run, seen; /* the sender's run; when it last sent */
    bool used, ack_due;
    PureAead *in, *out;
    unsigned char in_base[PURE_AEAD_NONCE], out_base[PURE_AEAD_NONCE];
    uint64_t next_pn, largest_at;
    struct sockaddr_storage address;
    socklen_t address_size;
    Range got[RANGES]; /* the numbers received, newest first */
    unsigned got_count;
} Run;
typedef struct {
    unsigned char key[PURE_SEAL_KEY], secret[32];
    uint32_t id;
    Run runs[RUNS];
} Peer;
typedef struct {
    uint64_t seq;
    unsigned char *bytes;
    uint32_t size, pieces, got;
    uint8_t *have;
    bool handed;
} Held;
typedef struct Stream {
    Peer *peer;
    char source[PURE_SOURCE + 1];
    uint32_t input;
    uint64_t heard;              /* the last number heard (stored), once known */
    bool known, out, stored_due, gap;
    Held *held;                  /* records not yet stored, by number */
    size_t held_count, held_capacity, held_bytes;
    Run *run;                    /* the run that sent to it last: where STORED goes */
    uint64_t run_id;
} Stream;
typedef struct { Stream *stream; uint64_t heard; bool stored, gap; } Done;
typedef struct { char source[PURE_SOURCE + 1]; size_t peer; } Allowed;

struct PureLinkListener {
    const PureModule *module;
    const PureSealKey *own;
    unsigned char own_public[PURE_SEAL_KEY];
    uint32_t own_id;
    unsigned port;
    uint64_t run;
    int fd, wake;
    pthread_t thread;
    bool started;
    pthread_mutex_t lock; /* the done list and stopping */
    Done *done;
    size_t done_count, done_capacity;
    bool stopping;
    Peer *peers;
    size_t peer_count;
    Allowed *allowed;
    size_t allowed_count;
    Stream **streams;
    size_t stream_count, stream_capacity;
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
    s->deliver = deliver;
    s->context = context;
    s->fd = s->wake = -1;
    return s;
}

bool pure_link_allow(PureLinkListener *s, const char *source, const unsigned char key[PURE_SEAL_KEY], char *error,
                     size_t size) {
    size_t p = 0;
    while (p < s->peer_count && memcmp(s->peers[p].key, key, PURE_SEAL_KEY)) ++p;
    for (size_t k = 0; p == s->peer_count && k < s->peer_count; ++k)
        if (s->peers[k].id == key_id(key)) {
            snprintf(error, size, "two senders' keys have the same id: give one another key");
            return false;
        }
    if (p == s->peer_count) {
        Peer *grown = realloc(s->peers, (s->peer_count + 1) * sizeof(*grown));
        if (!grown) {
            snprintf(error, size, "out of memory");
            return false;
        }
        s->peers = grown;
        Peer *peer = &s->peers[s->peer_count];
        *peer = (Peer){.id = key_id(key)};
        memcpy(peer->key, key, PURE_SEAL_KEY);
        if (!pure_link_secret(s->own, key, peer->secret)) {
            snprintf(error, size, "no secret shared with that key");
            return false;
        }
        ++s->peer_count;
    }
    Allowed *grown = realloc(s->allowed, (s->allowed_count + 1) * sizeof(*grown));
    if (!grown) {
        snprintf(error, size, "out of memory");
        return false;
    }
    s->allowed = grown;
    snprintf(s->allowed[s->allowed_count].source, sizeof(s->allowed[0].source), "%s", source);
    s->allowed[s->allowed_count++].peer = p;
    return true;
}

static Peer *peer_of(PureLinkListener *s, uint32_t id) {
    for (size_t p = 0; p < s->peer_count; ++p)
        if (s->peers[p].id == id) return &s->peers[p];
    return NULL;
}

static Run *run_found(Peer *peer, uint64_t run) {
    for (unsigned i = 0; i < RUNS; ++i)
        if (peer->runs[i].used && peer->runs[i].run == run) return &peer->runs[i];
    return NULL;
}

/* A new run's keys, in the slot unused longest: only once its first packet
 * opened with them, so a forged one displaces nothing. */
static Run *install_run(Peer *peer, const Run *made) {
    Run *slot = &peer->runs[0];
    for (unsigned i = 0; i < RUNS; ++i) {
        Run *r = &peer->runs[i];
        if (!r->used || (slot->used && r->seen < slot->seen)) slot = r;
    }
    pure_aead_free(slot->in);
    pure_aead_free(slot->out);
    *slot = *made;
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

static bool allowed(PureLinkListener *s, const Peer *peer, const char *source) {
    for (size_t k = 0; k < s->allowed_count; ++k)
        if (&s->peers[s->allowed[k].peer] == peer && !strcmp(s->allowed[k].source, source)) return true;
    return false;
}

/* The stream of peer's source to input, made the first time: NULL when the
 * source is not allowed from peer, or input takes no such messages. */
static Stream *stream_of(PureLinkListener *s, Peer *peer, const char *source, const char *input, uint32_t shape) {
    int index = pure_module_input(s->module, input);
    if (index < 0) return NULL;
    for (size_t k = 0; k < s->stream_count; ++k) {
        Stream *st = s->streams[k];
        if (st->peer == peer && st->input == (uint32_t)index && !strcmp(st->source, source)) return st;
    }
    const PureInputShape *in = &s->module->input[index];
    if (!allowed(s, peer, source) || in->kind != PURE_KIND_MESSAGE || in->keyed || shape_of(s->module, (uint32_t)index) != shape)
        return NULL;
    if (s->stream_count == s->stream_capacity) {
        size_t capacity = s->stream_capacity ? 2 * s->stream_capacity : 8;
        Stream **grown = realloc(s->streams, capacity * sizeof(*grown));
        if (!grown) return NULL;
        s->streams = grown;
        s->stream_capacity = capacity;
    }
    Stream *st = calloc(1, sizeof(*st));
    if (!st) return NULL;
    st->peer = peer;
    st->input = (uint32_t)index;
    snprintf(st->source, sizeof(st->source), "%s", source);
    s->streams[s->stream_count++] = st;
    return st;
}

static void drop_held(Stream *st, size_t k) {
    st->held_bytes -= st->held[k].size;
    free(st->held[k].bytes);
    free(st->held[k].have);
    memmove(&st->held[k], &st->held[k + 1], (st->held_count - k - 1) * sizeof(Held));
    --st->held_count;
}

/* A record's piece, kept until the record is whole and handed over. */
static void hold(Stream *st, uint64_t seq, uint32_t size, uint32_t offset, const unsigned char *bytes, uint32_t length) {
    if (st->known && seq <= st->heard) {
        st->stored_due = true; /* sent again: say again how far it is stored */
        return;
    }
    if ((st->known && seq > st->heard + CREDIT) || size > RECORD_MOST) return;
    size_t k = 0;
    while (k < st->held_count && st->held[k].seq < seq) ++k;
    if (k == st->held_count || st->held[k].seq != seq) {
        if (st->held_bytes + size > HELD_MOST) return;
        if (st->held_count == st->held_capacity) {
            size_t capacity = st->held_capacity ? 2 * st->held_capacity : 16;
            Held *grown = realloc(st->held, capacity * sizeof(*grown));
            if (!grown) return;
            st->held = grown;
            st->held_capacity = capacity;
        }
        Held h = {.seq = seq, .size = size, .pieces = size ? (size + PIECE - 1) / PIECE : 1};
        if (!(h.bytes = malloc(size ? size : 1)) || !(h.have = calloc(h.pieces, 1))) {
            free(h.bytes);
            return;
        }
        memmove(&st->held[k + 1], &st->held[k], (st->held_count - k) * sizeof(Held));
        st->held[k] = h;
        ++st->held_count;
        st->held_bytes += size;
    }
    Held *h = &st->held[k];
    uint32_t piece = offset / PIECE;
    if (h->size != size || offset % PIECE || piece >= h->pieces || length != least(PIECE, size - offset) || h->have[piece])
        return;
    if (length) memcpy(h->bytes + offset, bytes, length);
    h->have[piece] = 1;
    ++h->got;
}

/* Whole records from the one after the last heard, in order, to the host:
 * one delivery a stream at a time. Before the last heard is known (after a
 * restart) they go from the first held: the host says how far it heard. */
static void hand_over(PureLinkListener *s, Stream *st) {
    if (st->out) return;
    while (st->known && st->held_count && st->held[0].seq <= st->heard) drop_held(st, 0);
    if (!st->held_count || (st->known && st->held[0].seq != st->heard + 1)) return;
    size_t n = 0, bytes = 0;
    while (n < st->held_count && n < BATCH && st->held[n].got == st->held[n].pieces &&
           st->held[n].seq == st->held[0].seq + n)
        bytes += 4 + st->held[n++].size;
    if (!n) return;
    if (bytes > s->batch_capacity) {
        unsigned char *grown = realloc(s->batch, bytes);
        if (!grown) return;
        s->batch = grown;
        s->batch_capacity = bytes;
    }
    size_t at = 0;
    for (size_t k = 0; k < n; ++k) {
        put32(s->batch + at, st->held[k].size);
        memcpy(s->batch + at + 4, st->held[k].bytes, st->held[k].size);
        at += 4 + st->held[k].size;
    }
    PureLinkDelivery d = {st->input, st->source, st->held[0].seq, (unsigned)n, s->batch, bytes, st};
    if (!s->deliver(s->context, &d)) return;
    st->out = true;
    for (size_t k = 0; k < n; ++k) st->held[k].handed = true;
    ++s->counts.deliveries;
}

static void take_packet(PureLinkListener *s, unsigned char *packet, size_t n, const struct sockaddr_storage *from,
                        socklen_t from_size, uint64_t now) {
    if (n < HEADER + PURE_AEAD_TAG || n > DATAGRAM || packet[0] != VERSION) return;
    Peer *peer = peer_of(s, get32(packet + 1));
    if (!peer) return;
    uint64_t pn = get64(packet + 13), run = get64(packet + 5);
    Run *r = run_found(peer, run), made = {.run = run, .seen = now, .used = true};
    if (r && had(r, pn)) {
        r->ack_due = true; /* its acknowledgment was lost, perhaps */
        return;
    }
    if (!r) {
        made.in = derive(peer->secret, peer->key, s->own_public, run, 0, made.in_base);
        made.out = derive(peer->secret, s->own_public, peer->key, s->run, run, made.out_base);
    }
    unsigned char plain[DATAGRAM], nonce[PURE_AEAD_NONCE];
    nonce_of(nonce, r ? r->in_base : made.in_base, pn);
    size_t size = n - HEADER - PURE_AEAD_TAG;
    PureAead *in = r ? r->in : made.in;
    if (!in || (!r && !made.out) || !pure_aead_open(in, nonce, packet, HEADER, packet + HEADER, n - HEADER, plain)) {
        pure_aead_free(made.in);
        pure_aead_free(made.out);
        return;
    }
    if (!r) r = install_run(peer, &made);
    r->seen = now;
    note(r, pn, now);
    memcpy(&r->address, from, from_size); /* where it is now: answers go there */
    r->address_size = from_size;
    ++s->counts.received;
    Stream *st = NULL;
    bool eliciting = false;
    for (size_t at = 0; at < size;) {
        unsigned type = plain[at++];
        if (type == F_STREAM) {
            char source[PURE_SOURCE + 1], input[PURE_MODULE_NAME];
            size_t names = get_names(plain + at, size - at, source, input);
            if (!names || at + names + 4 > size) break;
            st = stream_of(s, peer, source, input, get32(plain + at + names));
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
            if (st) hold(st, get64(plain + at), get32(plain + at + 8), get32(plain + at + 12), plain + at + 18, length);
            at += RECORD_HEAD - 1 + length;
            eliciting = true;
        } else if (type == F_PING) {
            eliciting = true;
        } else
            break;
    }
    if (eliciting) r->ack_due = true;
}

/* Acknowledgments and how far each stream is stored, a packet to each run
 * that has any. How far a stream is stored goes with every acknowledgment
 * to its run, so a lost word is said again at the next packet. */
static void answer(PureLinkListener *s, uint64_t now) {
    unsigned char body[BODY], packet[DATAGRAM];
    for (size_t p = 0; p < s->peer_count; ++p)
        for (unsigned i = 0; i < RUNS; ++i) {
            Run *r = &s->peers[p].runs[i];
            if (!r->used) continue;
            size_t at = 0;
            if (r->ack_due && r->got_count) {
                body[at++] = F_ACK;
                put64(body + at, r->got[0].last);
                put32(body + at + 8, (uint32_t)least((now - r->largest_at) / 1000, UINT32_MAX));
                body[at + 12] = (unsigned char)r->got_count;
                at += 13;
                for (unsigned k = 0; k < r->got_count; ++k, at += 16) {
                    put64(body + at, r->got[k].first);
                    put64(body + at + 8, r->got[k].last);
                }
            }
            for (size_t k = 0; k < s->stream_count; ++k) {
                Stream *st = s->streams[k];
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
            size_t total = seal_packet(r->out, r->out_base, s->own_id, s->run, r->next_pn++, body, at, packet);
            if (total && sendto(s->fd, packet, total, 0, (const struct sockaddr *)&r->address, r->address_size) >= 0)
                ++s->counts.packets;
        }
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
        for (size_t h = 0; h < st->held_count; ++h) st->held[h].handed = false;
        if (!done[k].stored) continue; /* not saved: handed over again */
        if (!st->known || done[k].heard > st->heard) st->heard = done[k].heard;
        st->known = true;
        st->gap = done[k].gap;
        st->stored_due = true;
        s->counts.stored = st->heard;
    }
    free(done);
}

static void *run_listener(void *context) {
    PureLinkListener *s = context;
    unsigned char packet[DATAGRAM + 64];
    for (;;) {
        struct pollfd waits[2] = {{s->fd, POLLIN, 0}, {s->wake, POLLIN, 0}};
        if (poll(waits, 2, -1) < 0 && errno != EINTR) break;
        pthread_mutex_lock(&s->lock);
        bool stopping = s->stopping;
        pthread_mutex_unlock(&s->lock);
        if (stopping) break;
        if (waits[1].revents & POLLIN) {
            uint64_t signals;
            if (read(s->wake, &signals, sizeof(signals)) < 0) { /* cleared already */ }
        }
        uint64_t now = now_ns();
        for (unsigned k = 0; k < 256; ++k) {
            struct sockaddr_storage from;
            socklen_t from_size = sizeof(from);
            ssize_t n = recvfrom(s->fd, packet, sizeof(packet), MSG_DONTWAIT, (struct sockaddr *)&from, &from_size);
            if (n < 0) break;
            take_packet(s, packet, (size_t)n, &from, from_size, now);
        }
        take_done(s);
        for (size_t k = 0; k < s->stream_count; ++k) hand_over(s, s->streams[k]);
        answer(s, now);
    }
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
    s->run = random64();
    s->wake = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (s->wake < 0 || pthread_create(&s->thread, NULL, run_listener, s)) {
        snprintf(error, size, "--link-port %u: cannot start", s->port);
        return false;
    }
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
    if (counts) *counts = s->counts;
    for (size_t k = 0; k < s->stream_count; ++k) {
        while (s->streams[k]->held_count) drop_held(s->streams[k], 0);
        free(s->streams[k]->held);
        free(s->streams[k]);
    }
    for (size_t p = 0; p < s->peer_count; ++p) {
        for (unsigned i = 0; i < RUNS; ++i) {
            pure_aead_free(s->peers[p].runs[i].in);
            pure_aead_free(s->peers[p].runs[i].out);
        }
        explicit_bzero(s->peers[p].secret, sizeof(s->peers[p].secret));
    }
    free(s->streams);
    free(s->peers);
    free(s->allowed);
    free(s->batch);
    free(s->done);
    if (s->fd >= 0) close(s->fd);
    if (s->wake >= 0) close(s->wake);
    pthread_mutex_destroy(&s->lock);
    free(s);
}
