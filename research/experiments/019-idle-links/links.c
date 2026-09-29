#define _GNU_SOURCE
/* Many senders to one Pure node over links without connections
 * (docs/design/011-pure-transport.md, M1 and M2): each device is a link's
 * sending side as link/link-protocol.h keeps it, on a socket of its own,
 * all of them on this one thread. The devices' keys are made here with
 * OpenSSL; each device's secret with the node's key too (X25519), given to
 * its sender.
 *
 *   links keys N FILE
 *       N X25519 keys into FILE (each a private then a public half, 32 bytes
 *       each) and FILE.senders, a line each for pure-serve --senders:
 *       dNNNNNNN KEY
 *   links send FILE MODULE INPUT HOST PORT NODE_KEY FIRST COUNT SEQ [WINDOW]
 *       devices FIRST to FIRST + COUNT - 1 each send record SEQ (an order)
 *       to INPUT (whose shape MODULE says) and wait until the node says it
 *       stored it, WINDOW devices at a time, each on a socket of its own;
 *       then a line of JSON: how many, how long, and the time from each
 *       device's first packet to its record stored.
 *   links run FILE MODULE INPUT HOST PORT NODE_KEY FIRST COUNT PERIOD_MS SECONDS
 *       devices FIRST.. each make a record every PERIOD_MS for SECONDS, their
 *       senders sending what is not stored however the node comes and goes;
 *       then wait until all is stored and write a line of JSON: records made
 *       and stored, the records stored in each 100 ms, and the time from a
 *       record made to it stored. */
#include "link-protocol.h"
#include "module.h"
#include <errno.h>
#include <inttypes.h>
#include <netdb.h>
#include <openssl/evp.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

enum { KEY = 32 };

static void put32(unsigned char *p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = (unsigned char)(v >> 8 * i); }

static uint64_t now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

static void fail(const char *what) {
    fprintf(stderr, "links: %s\n", what);
    exit(1);
}

/* ---- Keys */

static void make_key(unsigned char out[2 * KEY]) {
    EVP_PKEY *key = EVP_PKEY_Q_keygen(NULL, NULL, "X25519");
    size_t a = KEY, b = KEY;
    if (!key || EVP_PKEY_get_raw_private_key(key, out, &a) != 1 || EVP_PKEY_get_raw_public_key(key, out + KEY, &b) != 1)
        fail("cannot make a key");
    EVP_PKEY_free(key);
}

typedef struct { unsigned char *keys; size_t first, count; } Making;

static void *make_keys(void *context) {
    Making *m = context;
    for (size_t i = 0; i < m->count; ++i) make_key(m->keys + (m->first + i) * 2 * KEY);
    return NULL;
}

static int keys(size_t n, const char *path) {
    unsigned char *all = malloc(n * 2 * KEY);
    if (!all) fail("out of memory");
    pthread_t threads[4];
    Making parts[4];
    for (int t = 0; t < 4; ++t) {
        parts[t] = (Making){all, n * t / 4, n * (t + 1) / 4 - n * t / 4};
        pthread_create(&threads[t], NULL, make_keys, &parts[t]);
    }
    for (int t = 0; t < 4; ++t) pthread_join(threads[t], NULL);
    FILE *file = fopen(path, "wb");
    if (!file || fwrite(all, 2 * KEY, n, file) != n || fclose(file)) fail("cannot write the keys");
    char senders[4096];
    snprintf(senders, sizeof(senders), "%s.senders", path);
    FILE *lines = fopen(senders, "w");
    if (!lines) fail("cannot write the senders");
    for (size_t i = 0; i < n; ++i) {
        fprintf(lines, "d%07zu ", i);
        for (int b = 0; b < KEY; ++b) fprintf(lines, "%02x", all[i * 2 * KEY + KEY + b]);
        fputc('\n', lines);
    }
    if (fclose(lines)) fail("cannot write the senders");
    free(all);
    return 0;
}

/* The secret a device's key shares with the node's (X25519). */
static bool secret_of(const unsigned char own[2 * KEY], const unsigned char peer[KEY], unsigned char out[32]) {
    EVP_PKEY *mine = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, NULL, own, KEY);
    EVP_PKEY *theirs = EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, NULL, peer, KEY);
    EVP_PKEY_CTX *c = mine && theirs ? EVP_PKEY_CTX_new(mine, NULL) : NULL;
    size_t length = 32;
    bool ok = c && EVP_PKEY_derive_init(c) == 1 && EVP_PKEY_derive_set_peer(c, theirs) == 1 &&
              EVP_PKEY_derive(c, out, &length) == 1 && length == 32;
    EVP_PKEY_CTX_free(c);
    EVP_PKEY_free(mine);
    EVP_PKEY_free(theirs);
    return ok;
}

/* ---- What is sent: an order as the receiving input stores it */

static PureModule module;

/* A message of input: each u32 field 1, each number 0, each text or bytes "pen". */
static size_t order(const PureModule *m, uint32_t input, unsigned char *out) {
    const PureInputShape *s = &m->input[input];
    memset(out, 0, s->prefix);
    size_t at = s->prefix;
    for (uint32_t f = 0; f < s->field_count; ++f) {
        const PureField *field = &m->field[s->field_start + f];
        if (field->type == PURE_FIELD_U32) put32(out + field->offset, 1);
        else if (field->type == PURE_FIELD_TEXT || field->type == PURE_FIELD_BYTES) {
            if (field->offset != PURE_FIELD_REST) put32(out + field->offset, 3);
            memcpy(out + at, "pen", 3);
            at += 3;
        }
    }
    return at;
}

/* ---- Devices */

static unsigned char *all_keys, node_key[KEY], payload[512];
static size_t payload_size;
static uint32_t shape;
static const char *input_name;
static uint64_t sends; /* packets the devices sent */

/* What the devices have in common: the keys, the order, the node. */
static struct addrinfo *setup(char **argv, size_t first, size_t count) {
    const char *path = argv[2], *module_path = argv[3];
    input_name = argv[4];
    char error[256];
    if (pure_module_read(module_path, &module, error, sizeof(error)) != PURE_OK) fail(error);
    int input = pure_module_input(&module, input_name);
    if (input < 0) fail("no such input");
    shape = pure_module_shape(&module, (uint32_t)input);
    payload_size = order(&module, (uint32_t)input, payload);
    for (int i = 0; i < KEY; ++i) sscanf(argv[7] + 2 * i, "%2hhx", &node_key[i]);
    FILE *file = fopen(path, "rb");
    if (!file) fail("cannot read the keys");
    fseek(file, 0, SEEK_END);
    size_t n = (size_t)ftell(file) / (2 * KEY);
    if (first + count > n) fail("not so many keys");
    all_keys = malloc(n * 2 * KEY);
    fseek(file, 0, SEEK_SET);
    if (!all_keys || fread(all_keys, 2 * KEY, n, file) != n) fail("cannot read the keys");
    fclose(file);
    if (!pure_seal_ready(error, sizeof(error))) fail(error);
    struct addrinfo hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_DGRAM}, *found;
    if (getaddrinfo(argv[5], argv[6], &hints, &found)) fail("no such host");
    return found;
}

/* Device's sender, keyed to the node. */
static PureLinkSender *sender_of(size_t device) {
    const unsigned char *own = all_keys + device * 2 * KEY;
    char source[16];
    unsigned char secret[32];
    snprintf(source, sizeof(source), "d%07zu", device);
    PureLinkSender *s = pure_link_sender(source, input_name, shape, NULL, own + KEY);
    if (!s || !secret_of(own, node_key, secret) || !pure_link_sender_shared(s, node_key, secret))
        fail("cannot make a device's sender");
    return s;
}

/* What a sender has to send, to its socket. */
static void send_packets(PureLinkSender *s, int fd, uint64_t now) {
    unsigned char packet[PURE_LINK_DATAGRAM_MOST];
    uint64_t number;
    for (size_t size; (size = pure_link_sender_packet(s, packet, false, now, &number));) {
        if (send(fd, packet, size, 0) < 0 && (errno == EAGAIN || errno == ENOBUFS)) {
            pure_link_sender_refused(s, number);
            break;
        }
        ++sends;
    }
}

/* Answers waiting on fd, to the sender. */
static void take_answers(PureLinkSender *s, int fd, uint64_t now) {
    unsigned char packet[PURE_LINK_DATAGRAM_MOST + 64];
    for (ssize_t got; (got = recv(fd, packet, sizeof(packet), 0)) > 0;) pure_link_sender_take(s, packet, (size_t)got, now);
}

static int by_value(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

typedef struct {
    PureLinkSender *sender;
    int fd;
    uint64_t started;
} Slot;

/* The devices sending one record each, window of them at a time. */
typedef struct {
    size_t first, count, next, done, failed;
    uint64_t seq, resent;
} Sending;

/* The next device in slot s: its record queued and sent. */
static void start(Sending *g, Slot *s) {
    s->sender = sender_of(g->first + g->next++);
    s->started = now_ns();
    if (!pure_link_sender_queue(s->sender, g->seq, payload, payload_size)) fail("cannot queue");
    send_packets(s->sender, s->fd, s->started);
}

/* Slot s's device done (its record stored, or given up): the next in it. */
static void finish(Sending *g, Slot *s) {
    PureLinkCounts c;
    pure_link_sender_counts(s->sender, &c);
    g->resent += c.resent;
    pure_link_sender_free(s->sender);
    s->sender = NULL;
    ++g->done;
    if (g->next < g->count) start(g, s);
}

static int send_all(int argc, char **argv) {
    Sending g = {.first = strtoull(argv[8], NULL, 10), .count = strtoull(argv[9], NULL, 10), .seq = strtoull(argv[10], NULL, 10)};
    size_t window = argc > 11 ? strtoull(argv[11], NULL, 10) : 256;
    struct addrinfo *found = setup(argv, g.first, g.count);
    if (window > g.count) window = g.count;
    Slot *slots = calloc(window, sizeof(*slots));
    double *waited = malloc(g.count * sizeof(*waited));
    size_t waited_count = 0;
    int poller = epoll_create1(0);
    for (size_t k = 0; k < window; ++k) {
        slots[k].fd = socket(found->ai_family, SOCK_DGRAM | SOCK_NONBLOCK, 0);
        if (slots[k].fd < 0 || connect(slots[k].fd, found->ai_addr, found->ai_addrlen)) fail("no socket");
        struct epoll_event e = {.events = EPOLLIN, .data.u64 = k};
        epoll_ctl(poller, EPOLL_CTL_ADD, slots[k].fd, &e);
    }
    uint64_t began = now_ns();
    for (size_t k = 0; k < window; ++k) start(&g, &slots[k]);
    struct epoll_event events[256];
    while (g.done < g.count) {
        int ready = epoll_wait(poller, events, 256, 5);
        uint64_t now = now_ns();
        for (int e = 0; e < ready; ++e) {
            Slot *s = &slots[events[e].data.u64];
            if (!s->sender) continue;
            take_answers(s->sender, s->fd, now);
            if (pure_link_sender_stored(s->sender) >= g.seq) {
                waited[waited_count++] = (double)(now_ns() - s->started) / 1e6;
                finish(&g, s);
            }
        }
        for (size_t k = 0; k < window; ++k) { /* the senders' timers: sent again as they say */
            Slot *s = &slots[k];
            if (!s->sender) continue;
            uint64_t due = pure_link_sender_due(s->sender);
            if (due && due <= now) {
                pure_link_sender_tick(s->sender, now);
                send_packets(s->sender, s->fd, now);
            }
            if (s->sender && now > s->started + 30000000000u) ++g.failed, finish(&g, s); /* half a minute: not stored */
        }
    }
    double seconds = (double)(now_ns() - began) / 1e9;
    qsort(waited, waited_count, sizeof(*waited), by_value);
    double p50 = waited_count ? waited[waited_count / 2] : 0, p99 = waited_count ? waited[waited_count * 99 / 100] : 0;
    printf("{\"devices\":%zu,\"stored\":%zu,\"failed\":%zu,\"resent\":%" PRIu64 ",\"packets\":%" PRIu64
           ",\"seconds\":%.3f,\"per_second\":%.0f,\"stored_ms_p50\":%.3f,\"stored_ms_p99\":%.3f,\"stored_ms_max\":%.3f}\n",
           g.count, waited_count, g.failed, g.resent, sends, seconds, (double)g.count / seconds, p50, p99,
           waited_count ? waited[waited_count - 1] : 0);
    freeaddrinfo(found);
    return g.failed ? 1 : 0;
}

/* ---- Devices that keep sending: an outbox each */

enum { OUTBOX = 64 };
typedef struct {
    PureLinkSender *sender;
    int fd;
    uint64_t made, stored;    /* records made and stored: numbers 1..made, 1..stored */
    uint64_t next_make;       /* when the next record is made */
    uint64_t due;             /* its sender's next timer (0: none), as it was when last touched */
    uint64_t made_at[OUTBOX]; /* when each record not stored was made (ns), by number % OUTBOX */
} Device;

static int run_all(char **argv) {
    size_t first = strtoull(argv[8], NULL, 10), count = strtoull(argv[9], NULL, 10);
    uint64_t period = strtoull(argv[10], NULL, 10) * 1000000u, seconds = strtoull(argv[11], NULL, 10);
    struct addrinfo *found = setup(argv, first, count);
    Device *devices = calloc(count, sizeof(*devices));
    int poller = epoll_create1(0);
    for (size_t k = 0; k < count; ++k) {
        Device *d = &devices[k];
        d->sender = sender_of(first + k);
        d->fd = socket(found->ai_family, SOCK_DGRAM | SOCK_NONBLOCK, 0);
        if (d->fd < 0 || connect(d->fd, found->ai_addr, found->ai_addrlen)) fail("no socket");
        struct epoll_event e = {.events = EPOLLIN, .data.u64 = k};
        epoll_ctl(poller, EPOLL_CTL_ADD, d->fd, &e);
    }
    uint64_t began = now_ns(), end = began + seconds * 1000000000u; /* the keys took their time: the run starts now */
    for (size_t k = 0; k < count; ++k) devices[k].next_make = began + period * k / count; /* spread over the period */
    size_t buckets = (size_t)(seconds + 60) * 10;
    uint32_t *timeline = calloc(buckets, sizeof(*timeline));
    size_t latency_capacity = 1024, latency_count = 0;
    double *latency = malloc(latency_capacity * sizeof(*latency));
    uint32_t *made_second = malloc(latency_capacity * sizeof(*made_second)); /* when each was made */
    uint64_t made = 0, stored = 0, last_report = began, last_scan = 0;
    struct epoll_event events[256];
    for (;;) {
        uint64_t now = now_ns();
        bool making = now < end;
        if (!making && stored == made) break;
        if (now > end + 60000000000u) break; /* a minute past the end: what is left was not stored */
        int ready = epoll_wait(poller, events, 256, 5);
        now = now_ns();
        for (int e = 0; e < ready; ++e) {
            Device *d = &devices[events[e].data.u64];
            take_answers(d->sender, d->fd, now);
            uint64_t heard = pure_link_sender_stored(d->sender);
            for (uint64_t seq = d->stored + 1; seq <= heard; ++seq) {
                size_t bucket = (size_t)((now - began) / 100000000u);
                if (bucket < buckets) ++timeline[bucket];
                if (latency_count == latency_capacity) {
                    latency = realloc(latency, (latency_capacity *= 2) * sizeof(*latency));
                    made_second = realloc(made_second, latency_capacity * sizeof(*made_second));
                }
                made_second[latency_count] = (uint32_t)((d->made_at[seq % OUTBOX] - began) / 1000000000u);
                latency[latency_count++] = (double)(now - d->made_at[seq % OUTBOX]) / 1e6;
                ++stored;
            }
            if (heard > d->stored) d->stored = heard;
            send_packets(d->sender, d->fd, now); /* what the answer let go */
            d->due = pure_link_sender_due(d->sender);
        }
        if (now - last_scan < 5000000u) continue; /* the devices' clocks, every 5 ms */
        last_scan = now;
        for (size_t k = 0; k < count; ++k) {
            Device *d = &devices[k];
            bool more = false;
            if (making && now >= d->next_make && d->made - d->stored < OUTBOX) {
                d->made_at[++d->made % OUTBOX] = now;
                ++made;
                d->next_make += period;
                if (!pure_link_sender_queue(d->sender, d->made, payload, payload_size)) fail("cannot queue");
                more = true;
            }
            /* A device's own timer: its sender's, which changes only when the
             * sender is touched (so ten thousand at rest cost a glance each). */
            if (d->due && d->due <= now) pure_link_sender_tick(d->sender, now), more = true;
            if (more) {
                send_packets(d->sender, d->fd, now);
                d->due = pure_link_sender_due(d->sender);
            }
        }
        if (now - last_report >= 1000000000u) {
            last_report = now;
            fprintf(stderr, "{\"t\":%.1f,\"made\":%" PRIu64 ",\"stored\":%" PRIu64 "}\n", (double)(now - began) / 1e9, made, stored);
        }
    }
    uint64_t again = 0, keyed = 0, secrets = 0, epochs = 0;
    for (size_t k = 0; k < count; ++k) {
        PureLinkCounts c;
        pure_link_sender_counts(devices[k].sender, &c);
        again += c.resent, keyed += c.keyed, secrets += c.secrets, epochs += c.epochs;
    }
    /* The p50 and p99 of the records made in each second: where the slow ones are. */
    size_t seconds_made = (size_t)seconds + 1;
    double *p50_by_second = calloc(seconds_made, sizeof(double)), *p99_by_second = calloc(seconds_made, sizeof(double)),
           *some = malloc(latency_count * sizeof(double));
    for (size_t second = 0; second < seconds_made; ++second) {
        size_t n = 0;
        for (size_t i = 0; i < latency_count; ++i)
            if (made_second[i] == second) some[n++] = latency[i];
        qsort(some, n, sizeof(*some), by_value);
        p50_by_second[second] = n ? some[n / 2] : 0;
        p99_by_second[second] = n ? some[n * 99 / 100] : 0;
    }
    qsort(latency, latency_count, sizeof(*latency), by_value);
    /* The longest time without a record stored, from the first to the last one stored. */
    size_t last = 0, longest = 0, gap = 0, since = SIZE_MAX;
    for (size_t b = 0; b < buckets; ++b)
        if (timeline[b]) {
            if (since != SIZE_MAX && b - since - 1 > longest) longest = b - since - 1, gap = since + 1;
            since = b;
            last = b;
        }
    printf("{\"devices\":%zu,\"made\":%" PRIu64 ",\"stored\":%" PRIu64 ",\"sends\":%" PRIu64 ",\"again\":%" PRIu64
           ",\"stored_ms_p50\":%.3f,\"stored_ms_p99\":%.3f,\"stored_ms_max\":%.3f,\"longest_gap_ms\":%zu,\"gap_from_ms\":%zu,"
           "\"timeline_100ms\":[",
           count, made, stored, sends, again, latency_count ? latency[latency_count / 2] : 0,
           latency_count ? latency[latency_count * 99 / 100] : 0, latency_count ? latency[latency_count - 1] : 0,
           longest * 100, gap * 100);
    for (size_t b = 0; b <= last; ++b) printf("%s%u", b ? "," : "", timeline[b]);
    printf("],\"keyed\":%" PRIu64 ",\"secrets\":%" PRIu64 ",\"epochs\":%" PRIu64 ",\"p50_ms_by_second_made\":[", keyed,
           secrets, epochs);
    for (size_t second = 0; second < seconds_made; ++second) printf("%s%.2f", second ? "," : "", p50_by_second[second]);
    printf("],\"p99_ms_by_second_made\":[");
    for (size_t second = 0; second < seconds_made; ++second) printf("%s%.2f", second ? "," : "", p99_by_second[second]);
    printf("]}\n");
    freeaddrinfo(found);
    return stored == made ? 0 : 1;
}

int main(int argc, char **argv) {
    if (argc == 4 && !strcmp(argv[1], "keys")) return keys(strtoull(argv[2], NULL, 10), argv[3]);
    if ((argc == 11 || argc == 12) && !strcmp(argv[1], "send")) return send_all(argc, argv);
    if (argc == 12 && !strcmp(argv[1], "run")) return run_all(argv);
    fputs("links keys N FILE | links send FILE MODULE INPUT HOST PORT NODE_KEY FIRST COUNT SEQ [WINDOW]\n"
          "      | links run FILE MODULE INPUT HOST PORT NODE_KEY FIRST COUNT PERIOD_MS SECONDS\n", stderr);
    return 2;
}
