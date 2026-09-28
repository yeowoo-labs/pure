#define _GNU_SOURCE
/* Many senders to one Pure node over links without connections
 * (docs/design/011-pure-transport.md, M1). Written from the packet format in
 * serve/link.h alone, with OpenSSL: a second implementation of it.
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
 *       devices FIRST.. each make a record every PERIOD_MS for SECONDS and
 *       send what is not stored (again after 200 ms, doubling to a second,
 *       without an answer), however the node comes and goes; then wait until
 *       all is stored and write a line of JSON: records made and stored,
 *       the records stored in each 100 ms, and the time from a record made
 *       to it stored. */
#include "module.h"
#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <netdb.h>
#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

enum { KEY = 32, HEADER = 21, TAG = 16, DATAGRAM = 1200, NONCE = 12 };

static void put16(unsigned char *p, uint32_t v) { p[0] = (unsigned char)v, p[1] = (unsigned char)(v >> 8); }
static void put32(unsigned char *p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = (unsigned char)(v >> 8 * i); }
static void put64(unsigned char *p, uint64_t v) { for (int i = 0; i < 8; ++i) p[i] = (unsigned char)(v >> 8 * i); }
static uint32_t get32(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t get64(const unsigned char *p) { return (uint64_t)get32(p) | (uint64_t)get32(p + 4) << 32; }

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

/* ---- Keys of a link (serve/link.h): X25519, HKDF-SHA256, AES-128-GCM */

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

static EVP_KDF *hkdf;

typedef struct { EVP_CIPHER_CTX *seal, *open; unsigned char base[NONCE]; } Aead;

static bool derive(const unsigned char secret[32], const unsigned char from[KEY], const unsigned char to[KEY],
                   uint64_t from_run, uint64_t to_run, Aead *out) {
    unsigned char info[16 + 2 * KEY + 16], key[16 + NONCE];
    memcpy(info, "pure link 0 keys", 16);
    memcpy(info + 16, from, KEY);
    memcpy(info + 16 + KEY, to, KEY);
    put64(info + 16 + 2 * KEY, from_run);
    put64(info + 24 + 2 * KEY, to_run);
    static const char salt[] = "pure link 0";
    EVP_KDF_CTX *c = EVP_KDF_CTX_new(hkdf);
    OSSL_PARAM params[] = {OSSL_PARAM_construct_utf8_string(OSSL_KDF_PARAM_DIGEST, (char *)"SHA256", 0),
                           OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_KEY, (void *)secret, 32),
                           OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_SALT, (void *)salt, sizeof(salt) - 1),
                           OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_INFO, info, sizeof(info)),
                           OSSL_PARAM_construct_end()};
    bool ok = c && EVP_KDF_derive(c, key, sizeof(key), params) == 1;
    EVP_KDF_CTX_free(c);
    if (!ok) return false;
    memcpy(out->base, key + 16, NONCE);
    out->seal = EVP_CIPHER_CTX_new();
    out->open = EVP_CIPHER_CTX_new();
    return out->seal && out->open && EVP_EncryptInit_ex(out->seal, EVP_aes_128_gcm(), NULL, key, NULL) == 1 &&
           EVP_DecryptInit_ex(out->open, EVP_aes_128_gcm(), NULL, key, NULL) == 1;
}

static void aead_free(Aead *a) {
    EVP_CIPHER_CTX_free(a->seal);
    EVP_CIPHER_CTX_free(a->open);
    *a = (Aead){0};
}

static void nonce_of(unsigned char out[NONCE], const unsigned char base[NONCE], uint64_t pn) {
    memcpy(out, base, NONCE);
    for (int i = 0; i < 8; ++i) out[4 + i] ^= (unsigned char)(pn >> (56 - 8 * i));
}

static size_t seal(Aead *a, uint32_t id, uint64_t run, uint64_t pn, const unsigned char *body, size_t size,
                   unsigned char *out) {
    unsigned char nonce[NONCE];
    int n = 0;
    out[0] = 0x70;
    put32(out + 1, id);
    put64(out + 5, run);
    put64(out + 13, pn);
    nonce_of(nonce, a->base, pn);
    if (EVP_EncryptInit_ex(a->seal, NULL, NULL, NULL, nonce) != 1 || EVP_EncryptUpdate(a->seal, NULL, &n, out, HEADER) != 1 ||
        EVP_EncryptUpdate(a->seal, out + HEADER, &n, body, (int)size) != 1 ||
        EVP_EncryptFinal_ex(a->seal, out + HEADER + n, &n) != 1 ||
        EVP_CIPHER_CTX_ctrl(a->seal, EVP_CTRL_GCM_GET_TAG, TAG, out + HEADER + size) != 1)
        return 0;
    return HEADER + size + TAG;
}

static bool open_packet(Aead *a, const unsigned char *packet, size_t n, unsigned char *plain) {
    unsigned char nonce[NONCE], tag[TAG];
    int length = 0, rest = 0;
    size_t size = n - HEADER - TAG;
    memcpy(tag, packet + n - TAG, TAG);
    nonce_of(nonce, a->base, get64(packet + 13));
    return EVP_DecryptInit_ex(a->open, NULL, NULL, NULL, nonce) == 1 &&
           EVP_DecryptUpdate(a->open, NULL, &length, packet, HEADER) == 1 &&
           EVP_DecryptUpdate(a->open, plain, &length, packet + HEADER, (int)size) == 1 &&
           EVP_CIPHER_CTX_ctrl(a->open, EVP_CTRL_GCM_SET_TAG, TAG, tag) == 1 &&
           EVP_DecryptFinal_ex(a->open, plain + length, &rest) == 1;
}

/* ---- What is sent: an order as the receiving input stores it */

static PureModule module;

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

typedef struct {
    size_t device;
    int fd;
    uint64_t run, pn, started, sent, in_run;
    Aead out, in;
    unsigned char secret[32];
    bool busy, in_ready;
    unsigned tries;
} Slot;

static unsigned char *all_keys, node_key[KEY], payload[512];
static size_t payload_size;
static uint32_t shape, node_id;
static const char *input_name;
static uint64_t seq;
static double *waited;
static size_t waited_count, resent, failed;

static uint64_t random64(void) {
    uint64_t v = 0;
    while (getrandom(&v, sizeof(v), 0) != sizeof(v) || !v) {}
    return v;
}

static void send_record(Slot *s) {
    unsigned char body[DATAGRAM], packet[DATAGRAM];
    char source[16];
    snprintf(source, sizeof(source), "d%07zu", s->device);
    size_t at = 0, a = strlen(source), b = strlen(input_name);
    body[at++] = 1; /* STREAM */
    body[at++] = (unsigned char)a;
    memcpy(body + at, source, a);
    at += a;
    body[at++] = (unsigned char)b;
    memcpy(body + at, input_name, b);
    at += b;
    put32(body + at, shape);
    at += 4;
    body[at++] = 2; /* RECORD */
    put64(body + at, seq);
    put32(body + at + 8, (uint32_t)payload_size);
    put32(body + at + 12, 0);
    put16(body + at + 16, (uint32_t)payload_size);
    memcpy(body + at + 18, payload, payload_size);
    at += 18 + payload_size;
    const unsigned char *own = all_keys + s->device * 2 * KEY;
    size_t n = seal(&s->out, fnv(2166136261u, own + KEY, KEY), s->run, s->pn++, body, at, packet);
    if (!n || send(s->fd, packet, n, 0) < 0) { /* sent again at its time */ }
    s->sent = now_ns();
}

static void start_device(Slot *s, size_t device) {
    const unsigned char *own = all_keys + device * 2 * KEY;
    s->device = device;
    s->run = random64();
    s->pn = 0;
    s->tries = 0;
    s->busy = true;
    s->in_ready = false;
    s->started = now_ns();
    if (!secret_of(own, node_key, s->secret) || !derive(s->secret, own + KEY, node_key, s->run, 0, &s->out))
        fail("cannot make a device's keys");
    send_record(s);
}

/* An answer: done once it says the record is stored. */
static bool take_answer(Slot *s, const unsigned char *packet, size_t n) {
    unsigned char plain[DATAGRAM];
    if (n < HEADER + TAG || packet[0] != 0x70 || get32(packet + 1) != node_id) return false;
    const unsigned char *own = all_keys + s->device * 2 * KEY;
    uint64_t run = get64(packet + 5);
    if (!s->in_ready || s->in_run != run) {
        aead_free(&s->in);
        if (!derive(s->secret, node_key, own + KEY, run, s->run, &s->in)) return false;
        s->in_ready = true;
        s->in_run = run;
    }
    if (!open_packet(&s->in, packet, n, plain)) return false;
    size_t size = n - HEADER - TAG;
    for (size_t at = 0; at < size;) {
        unsigned type = plain[at++];
        if (type == 3) { /* ACK */
            if (at + 13 > size) return false;
            at += 13 + 16u * plain[at + 12];
        } else if (type == 4) { /* STORED */
            if (at + 1 > size) return false;
            size_t a = plain[at], b = at + 1 + a < size ? plain[at + 1 + a] : 0;
            if (at + 2 + a + b + 13 > size) return false;
            uint64_t heard = get64(plain + at + 2 + a + b);
            at += 2 + a + b + 13;
            if (heard >= seq) return true;
        } else
            return false;
    }
    return false;
}

static int by_value(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

static int send_all(int argc, char **argv) {
    const char *path = argv[2], *module_path = argv[3];
    input_name = argv[4];
    size_t first = strtoull(argv[8], NULL, 10), count = strtoull(argv[9], NULL, 10), window = argc > 11 ? strtoull(argv[11], NULL, 10) : 256;
    seq = strtoull(argv[10], NULL, 10);
    char error[256];
    if (pure_module_read(module_path, &module, error, sizeof(error)) != PURE_OK) fail(error);
    int input = pure_module_input(&module, input_name);
    if (input < 0) fail("no such input");
    shape = shape_of(&module, (uint32_t)input);
    payload_size = order(&module, (uint32_t)input, payload);
    for (int i = 0; i < KEY; ++i) sscanf(argv[7] + 2 * i, "%2hhx", &node_key[i]);
    node_id = fnv(2166136261u, node_key, KEY);
    FILE *file = fopen(path, "rb");
    if (!file) fail("cannot read the keys");
    fseek(file, 0, SEEK_END);
    size_t n = (size_t)ftell(file) / (2 * KEY);
    if (first + count > n) fail("not so many keys");
    all_keys = malloc(n * 2 * KEY);
    fseek(file, 0, SEEK_SET);
    if (!all_keys || fread(all_keys, 2 * KEY, n, file) != n) fail("cannot read the keys");
    fclose(file);
    hkdf = EVP_KDF_fetch(NULL, "HKDF", NULL);
    struct addrinfo hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_DGRAM}, *found;
    if (getaddrinfo(argv[5], argv[6], &hints, &found)) fail("no such host");
    if (window > count) window = count;
    Slot *slots = calloc(window, sizeof(*slots));
    waited = malloc(count * sizeof(*waited));
    int poller = epoll_create1(0);
    for (size_t k = 0; k < window; ++k) {
        slots[k].fd = socket(found->ai_family, SOCK_DGRAM | SOCK_NONBLOCK, 0);
        if (slots[k].fd < 0 || connect(slots[k].fd, found->ai_addr, found->ai_addrlen)) fail("no socket");
        struct epoll_event e = {.events = EPOLLIN, .data.u64 = k};
        epoll_ctl(poller, EPOLL_CTL_ADD, slots[k].fd, &e);
    }
    uint64_t began = now_ns();
    size_t next = 0, done = 0;
    for (size_t k = 0; k < window; ++k) start_device(&slots[k], first + next++);
    struct epoll_event events[256];
    while (done < count) {
        int ready = epoll_wait(poller, events, 256, 20);
        uint64_t now = now_ns();
        for (int e = 0; e < ready; ++e) {
            Slot *s = &slots[events[e].data.u64];
            unsigned char packet[DATAGRAM + 64];
            for (ssize_t got; s->busy && (got = recv(s->fd, packet, sizeof(packet), 0)) > 0;) {
                if (!take_answer(s, packet, (size_t)got)) continue;
                waited[waited_count++] = (double)(now_ns() - s->started) / 1e6;
                aead_free(&s->out);
                aead_free(&s->in);
                s->busy = false;
                ++done;
                if (next < count) start_device(s, first + next++);
            }
        }
        for (size_t k = 0; k < window; ++k) { /* no answer: sent again, as a link does */
            Slot *s = &slots[k];
            if (!s->busy || now < s->sent + 200000000u) continue;
            if (++s->tries > 50) {
                ++failed, ++done;
                aead_free(&s->out);
                aead_free(&s->in);
                s->busy = false;
                if (next < count) start_device(s, first + next++);
                continue;
            }
            ++resent;
            send_record(s);
        }
    }
    double seconds = (double)(now_ns() - began) / 1e9;
    qsort(waited, waited_count, sizeof(*waited), by_value);
    double p50 = waited_count ? waited[waited_count / 2] : 0, p99 = waited_count ? waited[waited_count * 99 / 100] : 0;
    printf("{\"devices\":%zu,\"stored\":%zu,\"failed\":%zu,\"resent\":%zu,\"seconds\":%.3f,\"per_second\":%.0f,"
           "\"stored_ms_p50\":%.3f,\"stored_ms_p99\":%.3f,\"stored_ms_max\":%.3f}\n",
           count, waited_count, failed, resent, seconds, (double)count / seconds, p50, p99,
           waited_count ? waited[waited_count - 1] : 0);
    freeaddrinfo(found);
    return failed ? 1 : 0;
}

/* ---- Devices that keep sending: an outbox each */

enum { OUTBOX = 64, BATCH = 8 };
typedef struct {
    size_t device;
    int fd;
    uint64_t run, pn, in_run, made, stored; /* records made and stored: numbers 1..made, 1..stored */
    uint64_t made_at[OUTBOX];                /* when each record not stored was made (ns), by number % OUTBOX */
    uint64_t next_make, sent, wait;          /* when the next record is made; when last sent; the wait for an answer */
    Aead out, in;
    unsigned char secret[32];
    bool in_ready;
} Device;

static void send_outbox(Device *d, uint64_t now) {
    unsigned char body[DATAGRAM], packet[DATAGRAM];
    char source[16];
    snprintf(source, sizeof(source), "d%07zu", d->device);
    size_t at = 0, a = strlen(source), b = strlen(input_name);
    body[at++] = 1; /* STREAM */
    body[at++] = (unsigned char)a;
    memcpy(body + at, source, a);
    at += a;
    body[at++] = (unsigned char)b;
    memcpy(body + at, input_name, b);
    at += b;
    put32(body + at, shape);
    at += 4;
    for (uint64_t seq = d->stored + 1; seq <= d->made && seq <= d->stored + BATCH; ++seq) {
        body[at++] = 2; /* RECORD */
        put64(body + at, seq);
        put32(body + at + 8, (uint32_t)payload_size);
        put32(body + at + 12, 0);
        put16(body + at + 16, (uint32_t)payload_size);
        memcpy(body + at + 18, payload, payload_size);
        at += 18 + payload_size;
    }
    const unsigned char *own = all_keys + d->device * 2 * KEY;
    size_t n = seal(&d->out, fnv(2166136261u, own + KEY, KEY), d->run, d->pn++, body, at, packet);
    if (!n || send(d->fd, packet, n, 0) < 0) { /* sent again at its time */ }
    d->sent = now;
}

/* An answer: how far the node stored (0 when it says nothing of it). */
static uint64_t stored_in(Device *d, const unsigned char *packet, size_t n) {
    unsigned char plain[DATAGRAM];
    if (n < HEADER + TAG || packet[0] != 0x70 || get32(packet + 1) != node_id) return 0;
    const unsigned char *own = all_keys + d->device * 2 * KEY;
    uint64_t run = get64(packet + 5), heard = 0;
    if (!d->in_ready || d->in_run != run) { /* the node started again: its answers come in another run */
        Aead in = {0};
        if (!derive(d->secret, node_key, own + KEY, run, d->run, &in)) return 0;
        if (!open_packet(&in, packet, n, plain)) {
            aead_free(&in);
            return 0;
        }
        aead_free(&d->in);
        d->in = in;
        d->in_ready = true;
        d->in_run = run;
    } else if (!open_packet(&d->in, packet, n, plain))
        return 0;
    size_t size = n - HEADER - TAG;
    for (size_t at = 0; at < size;) {
        unsigned type = plain[at++];
        if (type == 3) {
            if (at + 13 > size) break;
            at += 13 + 16u * plain[at + 12];
        } else if (type == 4) {
            if (at + 1 > size) break;
            size_t a = plain[at], b = at + 1 + a < size ? plain[at + 1 + a] : 0;
            if (at + 2 + a + b + 13 > size) break;
            uint64_t said = get64(plain + at + 2 + a + b);
            if (said > heard) heard = said;
            at += 2 + a + b + 13;
        } else
            break;
    }
    return heard;
}

static int run_all(char **argv) {
    const char *path = argv[2], *module_path = argv[3];
    input_name = argv[4];
    size_t first = strtoull(argv[8], NULL, 10), count = strtoull(argv[9], NULL, 10);
    uint64_t period = strtoull(argv[10], NULL, 10) * 1000000u, seconds = strtoull(argv[11], NULL, 10);
    char error[256];
    if (pure_module_read(module_path, &module, error, sizeof(error)) != PURE_OK) fail(error);
    int input = pure_module_input(&module, input_name);
    if (input < 0) fail("no such input");
    shape = shape_of(&module, (uint32_t)input);
    payload_size = order(&module, (uint32_t)input, payload);
    for (int i = 0; i < KEY; ++i) sscanf(argv[7] + 2 * i, "%2hhx", &node_key[i]);
    node_id = fnv(2166136261u, node_key, KEY);
    FILE *file = fopen(path, "rb");
    if (!file) fail("cannot read the keys");
    fseek(file, 0, SEEK_END);
    size_t n = (size_t)ftell(file) / (2 * KEY);
    if (first + count > n) fail("not so many keys");
    all_keys = malloc(n * 2 * KEY);
    fseek(file, 0, SEEK_SET);
    if (!all_keys || fread(all_keys, 2 * KEY, n, file) != n) fail("cannot read the keys");
    fclose(file);
    hkdf = EVP_KDF_fetch(NULL, "HKDF", NULL);
    struct addrinfo hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_DGRAM}, *found;
    if (getaddrinfo(argv[5], argv[6], &hints, &found)) fail("no such host");
    Device *devices = calloc(count, sizeof(*devices));
    int poller = epoll_create1(0);
    uint64_t began = now_ns(), end = began + seconds * 1000000000u;
    for (size_t k = 0; k < count; ++k) {
        Device *d = &devices[k];
        const unsigned char *own = all_keys + (first + k) * 2 * KEY;
        d->device = first + k;
        d->run = random64();
        d->fd = socket(found->ai_family, SOCK_DGRAM | SOCK_NONBLOCK, 0);
        if (d->fd < 0 || connect(d->fd, found->ai_addr, found->ai_addrlen)) fail("no socket");
        if (!secret_of(own, node_key, d->secret) || !derive(d->secret, own + KEY, node_key, d->run, 0, &d->out))
            fail("cannot make a device's keys");
        d->next_make = began + period * k / count; /* spread over the period */
        d->wait = 200000000u;
        struct epoll_event e = {.events = EPOLLIN, .data.u64 = k};
        epoll_ctl(poller, EPOLL_CTL_ADD, d->fd, &e);
    }
    began = now_ns(); /* the keys took their time: the run starts now */
    end = began + seconds * 1000000000u;
    for (size_t k = 0; k < count; ++k) devices[k].next_make = began + period * k / count;
    size_t buckets = (size_t)(seconds + 60) * 10;
    uint32_t *timeline = calloc(buckets, sizeof(*timeline));
    size_t latency_capacity = 1024, latency_count = 0;
    double *latency = malloc(latency_capacity * sizeof(*latency));
    uint64_t made = 0, stored = 0, sends = 0, again = 0, last_report = began, last_scan = 0;
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
            unsigned char packet[DATAGRAM + 64];
            for (ssize_t got; (got = recv(d->fd, packet, sizeof(packet), 0)) > 0;) {
                uint64_t heard = stored_in(d, packet, (size_t)got);
                if (heard > d->made) heard = d->made;
                for (uint64_t seq = d->stored + 1; seq <= heard; ++seq) {
                    size_t bucket = (size_t)((now - began) / 100000000u);
                    if (bucket < buckets) ++timeline[bucket];
                    if (latency_count == latency_capacity) latency = realloc(latency, (latency_capacity *= 2) * sizeof(*latency));
                    latency[latency_count++] = (double)(now - d->made_at[seq % OUTBOX]) / 1e6;
                    ++stored;
                }
                if (heard > d->stored) {
                    d->stored = heard;
                    d->wait = 200000000u;
                }
            }
        }
        if (now - last_scan < 5000000u) continue; /* the devices' clocks, every 5 ms */
        last_scan = now;
        for (size_t k = 0; k < count; ++k) {
            Device *d = &devices[k];
            if (making && now >= d->next_make && d->made - d->stored < OUTBOX) {
                d->made_at[++d->made % OUTBOX] = now;
                ++made;
                d->next_make += period;
                send_outbox(d, now);
                ++sends;
            } else if (d->stored < d->made && now >= d->sent + d->wait) { /* no answer: again, waiting longer */
                send_outbox(d, now);
                ++sends, ++again;
                d->wait = d->wait * 2 > 1000000000u ? 1000000000u : d->wait * 2;
            }
        }
        if (now - last_report >= 1000000000u) {
            last_report = now;
            fprintf(stderr, "{\"t\":%.1f,\"made\":%" PRIu64 ",\"stored\":%" PRIu64 "}\n", (double)(now - began) / 1e9, made, stored);
        }
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
