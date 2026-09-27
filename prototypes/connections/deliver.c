#define _POSIX_C_SOURCE 200809L
#include "deliver.h"
#include "module-text.h"
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

enum { TIMEOUT_S = 10, FIRST_PAUSE_MS = 100, LONGEST_PAUSE_MS = 10000, GROUPS = PURE_MODULE_NAME / 2, BATCH_MOST = 256 };
enum { DELIVERED, AGAIN, REFUSED_BY_PEER, TLS_FAILED };

typedef struct { char *body; size_t size; uint64_t seq; } Pending;

struct PureWire {
    const PureModule *module;
    uint32_t output;
    char host[256], port[8], path[1024], source[PURE_SOURCE + 1];
    pthread_mutex_t lock;
    pthread_cond_t changed;
    Pending *queue; /* the sends to deliver, in order, from head */
    size_t head, count, capacity;
    uint64_t queued, delivered, attempts, deliveries;
    unsigned batch; /* how many a request may carry, as the receiver said (X-Pure-Batch); 0: one */
    bool stopping, started, stuck;
    pthread_t thread;
    void (*notify)(void *);
    void *context;
    int fd;   /* the connection kept for the next send, or -1; changed under the lock */
    bool tls; /* https: the connection is TLS */
    SSL *ssl; /* its TLS session */
};

/* ---- TLS: one client context, verifying the receiver's certificate against
 * the system's CAs or the file pure_wire_trust named, and its name. */
static pthread_once_t tls_once = PTHREAD_ONCE_INIT;
static SSL_CTX *tls_context;
static const char *tls_ca;

void pure_wire_trust(const char *ca_file) { tls_ca = ca_file; }

static void make_tls_context(void) {
    SSL_CTX *context = SSL_CTX_new(TLS_client_method());
    if (!context) return;
    if (!SSL_CTX_set_min_proto_version(context, TLS1_2_VERSION) ||
        !(tls_ca ? SSL_CTX_load_verify_locations(context, tls_ca, NULL) : SSL_CTX_set_default_verify_paths(context))) {
        SSL_CTX_free(context);
        return;
    }
    SSL_CTX_set_verify(context, SSL_VERIFY_PEER, NULL);
    SSL_CTX_set_mode(context, SSL_MODE_AUTO_RETRY);
    tls_context = context;
}

/* ---- A send as JSON: its fields in order, a group's fields as an object. */

typedef struct { char *bytes; size_t size, capacity; bool failed; } Text;

static void put(Text *t, const void *bytes, size_t size) {
    if (t->failed) return;
    if (size > t->capacity - t->size) {
        size_t capacity = t->capacity ? t->capacity : 256;
        while (size > capacity - t->size) capacity *= 2;
        char *grown = realloc(t->bytes, capacity);
        if (!grown) { t->failed = true; return; }
        t->bytes = grown;
        t->capacity = capacity;
    }
    memcpy(t->bytes + t->size, bytes, size);
    t->size += size;
}

static void put_string(Text *t, const unsigned char *bytes, size_t size) {
    put(t, "\"", 1);
    size_t run = 0;
    for (size_t i = 0; i < size; ++i) {
        unsigned char b = bytes[i];
        if (b != '"' && b != '\\' && b >= 0x20) continue;
        put(t, bytes + run, i - run);
        char escaped[8];
        if (b == '"' || b == '\\') put(t, (char[]){'\\', (char)b}, 2);
        else put(t, escaped, (size_t)snprintf(escaped, sizeof(escaped), "\\u%04x", b));
        run = i + 1;
    }
    put(t, bytes + run, size - run);
    put(t, "\"", 1);
}

static uint32_t read_u32(const unsigned char *p) { return p[0] | p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

/* The group a field is in: the length of its name before the last dot (0: none). */
static size_t group_of(const char *name) {
    const char *dot = strrchr(name, '.');
    return dot ? (size_t)(dot - name) : 0;
}

/* Whether the group path open (length open_size) holds the group of name (length size). */
static bool holds(const char *open, size_t open_size, const char *name, size_t size) {
    return open_size <= size && !memcmp(open, name, open_size) && (open_size == size || !open_size || name[open_size] == '.');
}

static bool encode(const PureWire *w, PureSpan payload, Text *t) {
    const PureModule *m = w->module;
    const PureInputShape *shape = &m->input[w->output];
    if (payload.size < shape->prefix) return false;
    char open[PURE_MODULE_NAME] = "";
    size_t open_size = 0, at = shape->prefix;
    unsigned depth = 0;
    bool first[GROUPS + 1] = {true};
    put(t, "{", 1);
    for (uint32_t f = 0; f < shape->field_count; ++f) {
        const PureField *field = &m->field[shape->field_start + f];
        size_t group = group_of(field->name);
        while (depth && !holds(open, open_size, field->name, group)) {
            put(t, "}", 1);
            --depth;
            open_size = group_of(open) < open_size ? group_of(open) : 0;
            open[open_size] = 0;
        }
        while (open_size < group) {
            size_t start = open_size ? open_size + 1 : 0, end = start;
            while (end < group && field->name[end] != '.') ++end;
            if (!first[depth]) put(t, ",", 1);
            first[depth] = false;
            put_string(t, (const unsigned char *)field->name + start, end - start);
            put(t, ":{", 2);
            first[++depth] = true;
            memcpy(open, field->name, end);
            open[open_size = end] = 0;
        }
        if (!first[depth]) put(t, ",", 1);
        first[depth] = false;
        const char *key = field->name + (group ? group + 1 : 0);
        put_string(t, (const unsigned char *)key, strlen(key));
        put(t, ":", 1);
        char digits[32];
        if (field->type == PURE_FIELD_U32) {
            put(t, digits, (size_t)snprintf(digits, sizeof(digits), "%u", (unsigned)read_u32(payload.data + field->offset)));
        } else if (field->type == PURE_FIELD_NUMBER) {
            uint64_t bits = read_u32(payload.data + field->offset) | (uint64_t)read_u32(payload.data + field->offset + 4) << 32;
            double value;
            memcpy(&value, &bits, sizeof(value));
            put(t, digits, pure_format_number(value, digits));
        } else {
            size_t size = field->offset == PURE_FIELD_REST ? payload.size - at : read_u32(payload.data + field->offset);
            if (size > payload.size - at) return false;
            PureSpan value = {payload.data + at, size};
            if (!pure_utf8_valid(value)) return false; /* bytes that are not text have no JSON form */
            put_string(t, value.data, size);
            at += size;
        }
    }
    while (depth--) put(t, "}", 1);
    put(t, "}", 1);
    return !t->failed;
}

/* ---- The wire. */

PureWire *pure_wire_open(const PureModule *m, const char *output, const char *url, const char *source,
                         char *error, size_t size) {
    int index = pure_module_input(m, output);
    if (index < 0 || m->input[index].kind != PURE_KIND_OUTPUT) {
        snprintf(error, size, "no output '%s'", output);
        return NULL;
    }
    /* A group's fields are together, so each group is one object. */
    const PureInputShape *shape = &m->input[index];
    for (uint32_t f = 1; f < shape->field_count; ++f) {
        const char *name = m->field[shape->field_start + f].name;
        for (uint32_t e = 0; e + 1 < f; ++e) {
            const char *earlier = m->field[shape->field_start + e].name, *between = m->field[shape->field_start + f - 1].name;
            size_t shared = 0;
            while (earlier[shared] && earlier[shared] == name[shared]) ++shared;
            while (shared && earlier[shared - 1] != '.') --shared; /* the groups both are in */
            if (shared && strncmp(between, name, shared)) {
                snprintf(error, size, "the fields of output '%s' in one group are not together", output);
                return NULL;
            }
        }
    }
    size_t length = strlen(source);
    if (!length || length > PURE_SOURCE) {
        snprintf(error, size, "a source name has 1 to %d bytes", PURE_SOURCE);
        return NULL;
    }
    bool tls = !strncmp(url, "https://", 8);
    if (!tls && strncmp(url, "http://", 7)) {
        snprintf(error, size, "a URL is http[s]://host[:port]/path");
        return NULL;
    }
    if (tls) {
        pthread_once(&tls_once, make_tls_context);
        if (!tls_context) {
            snprintf(error, size, "no TLS context: %s cannot be read", tls_ca ? tls_ca : "the system's CAs");
            return NULL;
        }
    }
    const char *host = url + (tls ? 8 : 7), *slash = strchr(host, '/'), *colon = memchr(host, ':', slash ? (size_t)(slash - host) : strlen(host));
    const char *host_end = colon ? colon : slash ? slash : host + strlen(host);
    const char *port = colon ? colon + 1 : tls ? "443" : "80";
    size_t port_size = colon ? (size_t)((slash ? slash : host + strlen(host)) - port) : strlen(port);
    if (host_end == host || (size_t)(host_end - host) >= 256 || !port_size || port_size >= 8 ||
        (slash && strlen(slash) >= 1024)) {
        snprintf(error, size, "a URL is http://host[:port]/path");
        return NULL;
    }
    PureWire *w = calloc(1, sizeof(*w));
    if (!w) { snprintf(error, size, "out of memory"); return NULL; }
    w->module = m;
    w->output = (uint32_t)index;
    memcpy(w->host, host, (size_t)(host_end - host));
    memcpy(w->port, port, port_size);
    snprintf(w->path, sizeof(w->path), "%s", slash ? slash : "/");
    memcpy(w->source, source, length + 1);
    w->fd = -1;
    w->tls = tls;
    pthread_mutex_init(&w->lock, NULL);
    pthread_cond_init(&w->changed, NULL);
    return w;
}

uint32_t pure_wire_output(const PureWire *w) { return w->output; }
uint64_t pure_wire_queued(const PureWire *w) { return w->queued; }

uint64_t pure_wire_delivered(PureWire *w) {
    pthread_mutex_lock(&w->lock);
    uint64_t delivered = w->delivered;
    pthread_mutex_unlock(&w->lock);
    return delivered;
}

void pure_wire_counts(PureWire *w, uint64_t *delivered, uint64_t *attempts) {
    pthread_mutex_lock(&w->lock);
    *delivered = w->deliveries;
    *attempts = w->attempts;
    pthread_mutex_unlock(&w->lock);
}

bool pure_wire_queue(PureWire *w, uint64_t seq, PureSpan payload) {
    if (w->stuck) return false;
    Text t = {0};
    if (!encode(w, payload, &t)) {
        free(t.bytes);
        w->stuck = true;
        fprintf(stderr, "{\"wire\":\"%s\",\"seq\":%llu,\"error\":\"no JSON form: its bytes are not text\"}\n", w->source,
                (unsigned long long)seq);
        return false;
    }
    pthread_mutex_lock(&w->lock);
    if (w->head + w->count == w->capacity) {
        if (w->head) {
            memmove(w->queue, w->queue + w->head, w->count * sizeof(*w->queue));
            w->head = 0;
        } else {
            size_t capacity = w->capacity ? 2 * w->capacity : 16;
            Pending *grown = realloc(w->queue, capacity * sizeof(*grown));
            if (!grown) {
                pthread_mutex_unlock(&w->lock);
                free(t.bytes);
                return false; /* it stays in the outbox: queued after the next save */
            }
            w->queue = grown;
            w->capacity = capacity;
        }
    }
    w->queue[w->head + w->count++] = (Pending){t.bytes, t.size, seq};
    w->queued = seq;
    pthread_cond_signal(&w->changed);
    pthread_mutex_unlock(&w->lock);
    return true;
}

/* ---- HTTP/1.1, one request at a time on a kept connection. */

typedef struct { PureWire *wire; char bytes[4096]; size_t at, end; } Reader;

static bool fill(Reader *r) {
    if (r->at == r->end) r->at = r->end = 0;
    if (r->end == sizeof(r->bytes)) return false;
    PureWire *w = r->wire;
    ssize_t n;
    if (w->ssl) n = SSL_read(w->ssl, r->bytes + r->end, (int)(sizeof(r->bytes) - r->end));
    else do n = recv(w->fd, r->bytes + r->end, sizeof(r->bytes) - r->end, 0); while (n < 0 && errno == EINTR);
    if (n <= 0) return false;
    r->end += (size_t)n;
    return true;
}

/* A line without its CRLF, in line (at most size - 1 bytes). */
static bool read_line(Reader *r, char *line, size_t size) {
    for (;;) {
        char *end = memchr(r->bytes + r->at, '\n', r->end - r->at);
        if (end) {
            size_t length = (size_t)(end - (r->bytes + r->at));
            if (length && end[-1] == '\r') --length;
            if (length >= size) return false;
            memcpy(line, r->bytes + r->at, length);
            line[length] = 0;
            r->at = (size_t)(end - r->bytes) + 1;
            return true;
        }
        if (r->at) {
            memmove(r->bytes, r->bytes + r->at, r->end - r->at);
            r->end -= r->at;
            r->at = 0;
        }
        if (!fill(r)) return false;
    }
}

static bool skip(Reader *r, uint64_t size) {
    while (size) {
        if (r->at == r->end && !fill(r)) return false;
        size_t n = r->end - r->at < size ? r->end - r->at : (size_t)size;
        r->at += n;
        size -= n;
    }
    return true;
}

static bool send_all(PureWire *w, const char *bytes, size_t size) {
    while (size) {
        ssize_t n = w->ssl ? SSL_write(w->ssl, bytes, (int)(size < INT32_MAX ? size : INT32_MAX)) :
                             send(w->fd, bytes, size, MSG_NOSIGNAL);
        if (n < 0 && !w->ssl && errno == EINTR) continue;
        if (n <= 0) return false;
        bytes += n;
        size -= (size_t)n;
    }
    return true;
}

/* TLS over a connected socket: the receiver's certificate must name the host
 * (a DNS name, or an IP address written as one). */
static SSL *start_tls(const PureWire *w, int fd) {
    SSL *ssl = SSL_new(tls_context);
    if (!ssl) return NULL;
    unsigned char address[16];
    bool literal = inet_pton(AF_INET, w->host, address) == 1 || inet_pton(AF_INET6, w->host, address) == 1;
    bool named = literal ? X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(ssl), w->host) == 1 :
                           SSL_set_tlsext_host_name(ssl, w->host) == 1 && SSL_set1_host(ssl, w->host) == 1;
    if (!named || SSL_set_fd(ssl, fd) != 1 || SSL_connect(ssl) != 1 || SSL_get_verify_result(ssl) != X509_V_OK) {
        SSL_free(ssl);
        return NULL;
    }
    return ssl;
}

static int connect_to(const PureWire *w) {
    struct addrinfo hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM}, *list;
    if (getaddrinfo(w->host, w->port, &hints, &list)) return -1;
    int fd = -1;
    for (struct addrinfo *a = list; a && fd < 0; a = a->ai_next) {
        fd = socket(a->ai_family, a->ai_socktype | SOCK_CLOEXEC, a->ai_protocol);
        if (fd < 0) continue;
        struct timeval limit = {TIMEOUT_S, 0};
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &limit, sizeof(limit));
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &limit, sizeof(limit));
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        if (connect(fd, a->ai_addr, a->ai_addrlen)) {
            close(fd);
            fd = -1;
        }
    }
    freeaddrinfo(list);
    return fd;
}

static void drop_connection(PureWire *w) {
    pthread_mutex_lock(&w->lock);
    if (w->ssl) SSL_free(w->ssl);
    if (w->fd >= 0) close(w->fd);
    w->ssl = NULL;
    w->fd = -1;
    pthread_mutex_unlock(&w->lock);
}

/* A fault a Pure receiver answers when its app refused the message. */
static bool refusal(const char *error) {
    static const char *const faults[] = {"divide", "non_finite", "clamp", "limit", "not_found", "encoding", "exists"};
    for (size_t i = 0; i < sizeof(faults) / sizeof(*faults); ++i) if (!strcmp(error, faults[i])) return true;
    return false;
}

/* The answer to one request: its status, whether a Pure receiver refused it,
 * and whether the connection may carry the next. */
static bool read_answer(PureWire *w, int *status, bool *refused, bool *keep, unsigned *batch) {
    Reader r = {.wire = w};
    char line[1024];
    if (!read_line(&r, line, sizeof(line)) || strncmp(line, "HTTP/1.", 7) || strlen(line) < 12) return false;
    *status = atoi(line + 9);
    *keep = line[7] == '1';
    *refused = false;
    uint64_t length = 0;
    bool sized = false, chunked = false;
    while (read_line(&r, line, sizeof(line))) {
        if (!*line) {
            if (chunked) {
                for (;;) {
                    uint64_t chunk;
                    if (!read_line(&r, line, sizeof(line))) return false;
                    chunk = strtoull(line, NULL, 16);
                    if (!chunk) break;
                    if (!skip(&r, chunk) || !read_line(&r, line, sizeof(line))) return false;
                }
                while (read_line(&r, line, sizeof(line)) && *line) {}
                return true;
            }
            if (*status == 204 || *status == 304 || *status < 200) return true;
            if (sized) return skip(&r, length);
            *keep = false; /* the body runs to the close */
            while (fill(&r)) r.at = r.end;
            return true;
        }
        char *value = strchr(line, ':');
        if (!value) return false;
        *value++ = 0;
        while (*value == ' ' || *value == '\t') ++value;
        if (!strcasecmp(line, "Content-Length")) { length = strtoull(value, NULL, 10); sized = true; }
        else if (!strcasecmp(line, "Transfer-Encoding")) chunked = !strcasecmp(value, "chunked");
        else if (!strcasecmp(line, "Connection")) *keep = strcasecmp(value, "close") != 0 && *keep;
        else if (!strcasecmp(line, "X-Pure-Error")) *refused = refusal(value);
        else if (!strcasecmp(line, "X-Pure-Batch")) *batch = (unsigned)strtoul(value, NULL, 10);
    }
    return false;
}

/* One request for count sends from p (numbered in a row): one object, or
 * an array of them for a receiver that takes batches. */
static int post(PureWire *w, const Pending *p, unsigned count) {
    size_t body = count > 1 ? count + 1 : 0;
    for (unsigned i = 0; i < count; ++i) body += p[i].size;
    char head[2048], batch[32] = "";
    if (count > 1) snprintf(batch, sizeof(batch), "X-Pure-Count: %u\r\n", count);
    int size = snprintf(head, sizeof(head),
                        "POST %s HTTP/1.1\r\nHost: %s:%s\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n"
                        "X-Pure-Source: %s\r\nX-Pure-Sequence: %llu\r\n%sIdempotency-Key: %s-%llu\r\n\r\n",
                        w->path, w->host, w->port, body, w->source, (unsigned long long)p->seq, batch, w->source,
                        (unsigned long long)p->seq);
    /* A kept connection the peer closed is tried once more on a new one. */
    for (int attempt = 0; attempt < 2; ++attempt) {
        bool fresh = w->fd < 0;
        if (fresh) {
            int fd = connect_to(w);
            if (fd < 0) return AGAIN;
            pthread_mutex_lock(&w->lock);
            w->fd = fd;
            bool stopping = w->stopping;
            pthread_mutex_unlock(&w->lock);
            if (stopping) return AGAIN;
            SSL *ssl = w->tls ? start_tls(w, fd) : NULL;
            if (w->tls && !ssl) {
                drop_connection(w);
                return TLS_FAILED;
            }
            pthread_mutex_lock(&w->lock);
            w->ssl = ssl;
            pthread_mutex_unlock(&w->lock);
        }
        int status;
        bool refused, keep, sent = send_all(w, head, (size_t)size);
        unsigned batch_size = 0;
        for (unsigned i = 0; sent && i < count; ++i)
            sent = (!i || count == 1 || send_all(w, ",", 1)) && (i || count == 1 || send_all(w, "[", 1)) &&
                   send_all(w, p[i].body, p[i].size);
        if (sent && count > 1) sent = send_all(w, "]", 1);
        if (!sent || !read_answer(w, &status, &refused, &keep, &batch_size)) {
            drop_connection(w);
            if (fresh) return AGAIN;
            continue;
        }
        if (!keep) drop_connection(w);
        if (batch_size) w->batch = batch_size;
        if ((status >= 200 && status < 300) || (status >= 400 && status < 500 && refused)) return DELIVERED;
        if (status == 408 || status == 429 || status >= 500) return AGAIN;
        return REFUSED_BY_PEER;
    }
    return AGAIN;
}

static void *deliver(void *context) {
    PureWire *w = context;
    long pause_ms = 0;
    uint64_t reported = 0;
    pthread_mutex_lock(&w->lock);
    for (;;) {
        while (!w->stopping && !w->count) pthread_cond_wait(&w->changed, &w->lock);
        if (w->stopping) break;
        /* As many as the receiver takes at once; their bodies stay put while
         * the queue only grows behind them. */
        unsigned count = w->batch > 1 ? (unsigned)(w->count < w->batch ? w->count : w->batch) : 1;
        Pending next[BATCH_MOST];
        if (count > BATCH_MOST) count = BATCH_MOST;
        memcpy(next, w->queue + w->head, count * sizeof(*next));
        ++w->attempts;
        pthread_mutex_unlock(&w->lock);
        int outcome = post(w, next, count);
        pthread_mutex_lock(&w->lock);
        if (outcome == DELIVERED) {
            for (unsigned i = 0; i < count; ++i) free(w->queue[w->head + i].body);
            w->head += count;
            w->count -= count;
            w->delivered = next[count - 1].seq;
            w->deliveries += count;
            pause_ms = 0;
            pthread_mutex_unlock(&w->lock);
            if (w->notify) w->notify(w->context);
            pthread_mutex_lock(&w->lock);
            continue;
        }
        if ((outcome == REFUSED_BY_PEER || outcome == TLS_FAILED) && reported != next[0].seq) {
            fprintf(stderr, "{\"wire\":\"%s\",\"seq\":%llu,\"error\":\"%s\"}\n", w->source,
                    (unsigned long long)next[0].seq, outcome == TLS_FAILED ? "the receiver's certificate is not trusted; trying again" :
                    "the receiver does not take it; trying again");
            reported = next[0].seq;
        }
        pause_ms = outcome != AGAIN ? LONGEST_PAUSE_MS : pause_ms ? 2 * pause_ms : FIRST_PAUSE_MS;
        if (pause_ms > LONGEST_PAUSE_MS) pause_ms = LONGEST_PAUSE_MS;
        struct timespec until;
        clock_gettime(CLOCK_REALTIME, &until);
        until.tv_sec += pause_ms / 1000;
        until.tv_nsec += (pause_ms % 1000) * 1000000;
        if (until.tv_nsec >= 1000000000) { ++until.tv_sec; until.tv_nsec -= 1000000000; }
        while (!w->stopping && pthread_cond_timedwait(&w->changed, &w->lock, &until) == 0) {}
    }
    pthread_mutex_unlock(&w->lock);
    return NULL;
}

bool pure_wire_start(PureWire *w, void (*notify)(void *), void *context) {
    w->notify = notify;
    w->context = context;
    w->started = pthread_create(&w->thread, NULL, deliver, w) == 0;
    return w->started;
}

void pure_wire_close(PureWire *w) {
    if (!w) return;
    pthread_mutex_lock(&w->lock);
    w->stopping = true;
    if (w->fd >= 0) shutdown(w->fd, SHUT_RDWR); /* a send or an answer waited for ends now */
    pthread_cond_broadcast(&w->changed);
    pthread_mutex_unlock(&w->lock);
    if (w->started) pthread_join(w->thread, NULL);
    if (w->ssl) SSL_free(w->ssl);
    if (w->fd >= 0) close(w->fd);
    for (size_t i = 0; i < w->count; ++i) free(w->queue[w->head + i].body);
    free(w->queue);
    pthread_cond_destroy(&w->changed);
    pthread_mutex_destroy(&w->lock);
    free(w);
}
