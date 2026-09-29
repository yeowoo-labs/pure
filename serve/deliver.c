#define _POSIX_C_SOURCE 200809L
#include "deliver.h"
#include "seal.h"
#include "message-json.h"
#include "module-text.h"
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include "tls.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

enum { TIMEOUT_S = 10, FIRST_PAUSE_MS = 100, LONGEST_PAUSE_MS = 10000, GROUPS = PURE_MODULE_NAME / 2, BATCH_MOST = 256,
       ANSWER_MOST = 1 << 20 };
enum { DELIVERED, AGAIN, REFUSED_BY_PEER, TLS_FAILED, ANSWER_UNREAD };

typedef struct { char *body; size_t size; uint64_t seq; unsigned char *sent; size_t sent_size; } Pending;
/* An answer made into its input's message: a table of the fields' starts and
 * sizes (u32le), then the payload from its prefix on. */
typedef struct { uint64_t seq; unsigned char *bytes; size_t size, table; } Answer;

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
    bool tls;         /* https: the connection is TLS */
    PureTls *session; /* its TLS session */
    int answer_input; /* the input answers become events of, or -1 */
    Answer *answers;  /* made, for the owner to take in order */
    size_t answer_head, answer_count, answer_capacity;
    Answer taken;     /* the one the owner took last, kept until it takes the next */
    uint32_t placed[2 * PURE_MODULE_FIELDS];
    /* Sealed sends: to peer's key, authenticated by the node's own key when
     * it has one (its public half is from), in one session made at the first
     * seal (and again after one fails); enc opens it. */
    bool sealed;
    unsigned char peer[PURE_SEAL_KEY], from[PURE_SEAL_KEY], enc[PURE_SEAL_KEY];
    const PureSealKey *own;
    PureSealer *sealer;
};

/* TLS verifies a receiver's certificate against the system's CAs or the
 * file pure_wire_trust named (link/tls.h). */
static const char *tls_ca;

void pure_wire_trust(const char *ca_file) { tls_ca = ca_file; }

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
    if (tls && !pure_tls_start(tls_ca, error, size)) return NULL;
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
    w->answer_input = -1;
    pthread_mutex_init(&w->lock, NULL);
    pthread_cond_init(&w->changed, NULL);
    return w;
}

uint32_t pure_wire_output(const PureWire *w) { return w->output; }

bool pure_wire_reply(PureWire *w, uint32_t input, char *error, size_t size) {
    const PureModule *m = w->module;
    if (input >= m->inputs || m->input[input].kind != PURE_KIND_MESSAGE || m->input[input].keyed) {
        snprintf(error, size, "an answer goes to a message input without a key");
        return false;
    }
    w->answer_input = (int)input;
    return true;
}

bool pure_wire_answer(PureWire *w, PureWireAnswer *out) {
    free(w->taken.bytes);
    w->taken = (Answer){0};
    pthread_mutex_lock(&w->lock);
    bool some = w->answer_count > 0;
    if (some) {
        w->taken = w->answers[w->answer_head++];
        --w->answer_count;
        w->delivered = w->taken.seq; /* trimmed after the owner applies it */
    }
    pthread_mutex_unlock(&w->lock);
    if (!some) return false;
    const PureInputShape *in = &w->module->input[w->answer_input];
    for (size_t i = 0; i < 2 * (size_t)in->field_count; ++i) w->placed[i] = read_u32(w->taken.bytes + 4 * i);
    *out = (PureWireAnswer){(uint32_t)w->answer_input, w->taken.seq,
                            {w->taken.bytes + w->taken.table, w->taken.size - w->taken.table}, w->placed, w->source};
    return true;
}
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
    unsigned char *sent = NULL;
    if (w->answer_input >= 0 && !(sent = malloc(payload.size ? payload.size : 1))) {
        pthread_mutex_unlock(&w->lock);
        free(t.bytes);
        return false;
    }
    if (sent && payload.size) memcpy(sent, payload.data, payload.size);
    w->queue[w->head + w->count++] = (Pending){t.bytes, t.size, seq, sent, payload.size};
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
    if (w->session) n = pure_tls_read(w->session, r->bytes + r->end, (int)(sizeof(r->bytes) - r->end));
    else do n = recv(w->fd, r->bytes + r->end, sizeof(r->bytes) - r->end, 0); while (n < 0 && errno == EINTR);
    if (n <= 0) return false;
#ifdef TCP_QUICKACK
    /* Acknowledge at once: a server that writes an answer in pieces holds the
     * next piece until the first is acknowledged (Nagle), which a delayed
     * acknowledgement would stall for 40 ms. The flag does not stay set. */
    int one = 1;
    setsockopt(w->fd, IPPROTO_TCP, TCP_QUICKACK, &one, sizeof(one));
#endif
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

/* size bytes of the body: kept in into (up to ANSWER_MOST), or skipped when it is NULL. */
static bool take(Reader *r, uint64_t size, Text *into) {
    if (into && size > ANSWER_MOST - into->size) return false;
    while (size) {
        if (r->at == r->end && !fill(r)) return false;
        size_t n = r->end - r->at < size ? r->end - r->at : (size_t)size;
        if (into) put(into, r->bytes + r->at, n);
        r->at += n;
        size -= n;
    }
    return !into || !into->failed;
}

static bool send_all(PureWire *w, const char *bytes, size_t size) {
    while (size) {
        ssize_t n = w->session ? pure_tls_write(w->session, bytes, (int)(size < INT32_MAX ? size : INT32_MAX)) :
                                 send(w->fd, bytes, size, MSG_NOSIGNAL);
        if (n < 0 && !w->session && errno == EINTR) continue;
        if (n <= 0) return false;
        bytes += n;
        size -= (size_t)n;
    }
    return true;
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
    pure_tls_close(w->session);
    if (w->fd >= 0) close(w->fd);
    w->session = NULL;
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
static bool read_answer(PureWire *w, int *status, bool *refused, bool *keep, unsigned *batch, Text *body) {
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
                    if (!take(&r, chunk, body) || !read_line(&r, line, sizeof(line))) return false;
                }
                while (read_line(&r, line, sizeof(line)) && *line) {}
                return true;
            }
            if (*status == 204 || *status == 304 || *status < 200) return true;
            if (sized) return take(&r, length, body);
            *keep = false; /* the body runs to the close */
            while (fill(&r)) {
                if (body && !take(&r, r.end - r.at, body)) return false;
                r.at = r.end;
            }
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

static void write_u32(unsigned char *p, uint32_t v) { for (unsigned i = 0; i < 4; ++i) p[i] = (unsigned char)(v >> (8 * i)); }

/* The answer (status, body) to the send p as a message of the answer input:
 * fields from the body's JSON object by name, others skipped; a field the
 * body lacks from the send's field of that name and type; a u32 field named
 * http_status from the status. False when a field has none of these. */
static bool form_answer(const PureWire *w, int status, const Text *body, const Pending *p, Answer *out) {
    const PureModule *m = w->module;
    const PureInputShape *in = &m->input[w->answer_input], *sent = &m->input[w->output];
    size_t table = 8 * (size_t)in->field_count, total = table + in->prefix + body->size + p->sent_size;
    unsigned char *bytes = calloc(1, total ? total : 1);
    if (!bytes) return false;
    unsigned char *prefix = bytes + table, *text = prefix + in->prefix, *copy = text + body->size;
    if (body->size) memcpy(text, body->bytes, body->size);
    if (p->sent_size) memcpy(copy, p->sent, p->sent_size);
    /* Rows the answer gives go after the send's copy. */
    PureJsonMessage message = {.module = m, .shape = in, .payload = prefix, .table = bytes, .lenient = true,
                               .rows_base = in->prefix + body->size + p->sent_size};
    size_t at = pure_json_space(text, 0, body->size);
    if (at < body->size && text[at] == '{' && pure_json_message(&message, text, body->size, &at)) {
        free(message.rows);
        free(bytes);
        return false;
    }
    if (message.rows_size) {
        unsigned char *grown = realloc(bytes, total + message.rows_size);
        if (!grown) {
            free(message.rows);
            free(bytes);
            return false;
        }
        memcpy(grown + total, message.rows, message.rows_size);
        bytes = grown;
        prefix = bytes + table;
        copy = prefix + in->prefix + body->size;
        total += message.rows_size;
    }
    free(message.rows);
    for (uint32_t f = 0; f < in->field_count; ++f) {
        if (message.seen[f]) continue;
        const PureField *field = &m->field[in->field_start + f];
        if (field->type == PURE_FIELD_ROWS) { /* none given: none */
            write_u32(bytes + 8 * f, 0);
            write_u32(bytes + 8 * f + 4, 0);
            continue;
        }
        if (field->type == PURE_FIELD_U32 && !strcmp(field->name, "http_status")) {
            write_u32(prefix + field->offset, (uint32_t)status);
            continue;
        }
        /* The send's field of the same name and type, and where its bytes are. */
        size_t start = sent->prefix, length = 0;
        const PureField *from = NULL;
        for (uint32_t g = 0; g < sent->field_count && !from; ++g) {
            const PureField *other = &m->field[sent->field_start + g];
            bool variable = other->type == PURE_FIELD_BYTES || other->type == PURE_FIELD_TEXT;
            if (variable) length = other->offset == PURE_FIELD_REST ? p->sent_size - start : read_u32(p->sent + other->offset);
            if (!strcmp(other->name, field->name) && other->type == field->type) from = other;
            else if (variable) start += length;
        }
        bool variable = field->type == PURE_FIELD_BYTES || field->type == PURE_FIELD_TEXT;
        if (!from || (variable && (start > p->sent_size || length > p->sent_size - start))) {
            free(bytes);
            return false;
        }
        if (field->type == PURE_FIELD_U32 || field->type == PURE_FIELD_NUMBER)
            memcpy(prefix + field->offset, p->sent + from->offset, field->type == PURE_FIELD_U32 ? 4 : 8);
        else {
            write_u32(bytes + 8 * f, (uint32_t)(copy + start - prefix));
            write_u32(bytes + 8 * f + 4, (uint32_t)length);
        }
    }
    *out = (Answer){p->seq, bytes, total, table};
    return true;
}

static int send_request(PureWire *w, const char *request, size_t size, const Pending *p, Answer *answer);

void pure_wire_seal(PureWire *w, const unsigned char peer[PURE_SEAL_KEY], const PureSealKey *own,
                    const unsigned char *from) {
    memcpy(w->peer, peer, PURE_SEAL_KEY);
    w->own = own;
    if (own) memcpy(w->from, from, PURE_SEAL_KEY);
    w->sealed = true;
}

/* count sends from p as one body: one object, or an array of them. */
static void put_body(unsigned char *at, const Pending *p, unsigned count) {
    for (unsigned i = 0; i < count; ++i) {
        if (count > 1) *at++ = i ? ',' : '[';
        memcpy(at, p[i].body, p[i].size);
        at += p[i].size;
    }
    if (count > 1) *at = ']';
}

static void hex(const unsigned char *bytes, size_t size, char *out) {
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < size; ++i) out[2 * i] = digits[bytes[i] >> 4], out[2 * i + 1] = digits[bytes[i] & 15];
    out[2 * size] = 0;
}

/* The body sealed whole in the wire's session, bound to its source, first
 * number and count; the X-Pure-Seal line that opens it (suite, session,
 * number, and the sender's key when it sealed with its own) into line. */
static unsigned char *seal_body(PureWire *w, const Pending *p, unsigned count, size_t body, char *line, size_t size) {
    unsigned char *plain = malloc(body), *sealed = malloc(body + PURE_SEAL_OVERHEAD);
    char aad[PURE_SOURCE + 48];
    snprintf(aad, sizeof(aad), "%s %llu %u", w->source, (unsigned long long)p->seq, count);
    uint64_t n = 0;
    if (plain && sealed) {
        put_body(plain, p, count);
        if (!w->sealer) w->sealer = pure_sealer(w->peer, w->own, w->enc);
    }
    bool ok = plain && sealed && w->sealer && pure_seal(w->sealer, aad, plain, body, sealed, &n);
    free(plain);
    if (!ok) {
        pure_sealer_free(w->sealer); /* the next try makes a new session */
        w->sealer = NULL;
        free(sealed);
        return NULL;
    }
    char enc[2 * PURE_SEAL_KEY + 1], from[2 * PURE_SEAL_KEY + 2] = "";
    hex(w->enc, PURE_SEAL_KEY, enc);
    if (w->own) from[0] = ' ', hex(w->from, PURE_SEAL_KEY, from + 1);
    snprintf(line, size, "X-Pure-Seal: %s %s %llu%s\r\n", pure_seal_suite(), enc, (unsigned long long)n, from);
    return sealed;
}

/* One request for count sends from p (numbered in a row): one object, or
 * an array of them for a receiver that takes batches. A final answer to a
 * send whose answers are wired is made into *answer. */
static int post(PureWire *w, const Pending *p, unsigned count, Answer *answer) {
    size_t body = count > 1 ? count + 1 : 0;
    for (unsigned i = 0; i < count; ++i) body += p[i].size;
    size_t length = body + (w->sealed ? PURE_SEAL_OVERHEAD : 0);
    char batch[32] = "", seal[256] = "";
    if (count > 1) snprintf(batch, sizeof(batch), "X-Pure-Count: %u\r\n", count);
    unsigned char *sealed = w->sealed ? seal_body(w, p, count, body, seal, sizeof(seal)) : NULL;
    if (w->sealed && !sealed) return AGAIN;
    /* The request in one piece, its head and then a batch in its brackets:
     * one write where there were two a send (a batch drained at 91k sends a
     * second, and at 470k in one piece). */
    enum { HEAD = 2048 }; /* the longest path, host, source and seal leave room to spare */
    char *request = malloc(HEAD + length);
    if (!request) {
        free(sealed);
        return AGAIN;
    }
    int head = snprintf(request, HEAD,
                        "POST %s HTTP/1.1\r\nHost: %s:%s\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n"
                        "X-Pure-Source: %s\r\nX-Pure-Sequence: %llu\r\n%s%sIdempotency-Key: %s-%llu\r\n\r\n",
                        w->path, w->host, w->port, length, w->source, (unsigned long long)p->seq, batch, seal, w->source,
                        (unsigned long long)p->seq);
    if (sealed) memcpy(request + head, sealed, length);
    else put_body((unsigned char *)request + head, p, count);
    free(sealed);
    int result = send_request(w, request, (size_t)head + length, p, answer);
    free(request);
    return result;
}

static int send_request(PureWire *w, const char *request, size_t size, const Pending *p, Answer *answer) {
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
            /* The receiver's certificate must name the host. */
            PureTls *session = w->tls ? pure_tls_open(w->host, fd) : NULL;
            if (w->tls && !session) {
                drop_connection(w);
                return TLS_FAILED;
            }
            pthread_mutex_lock(&w->lock);
            w->session = session;
            pthread_mutex_unlock(&w->lock);
        }
        int status;
        bool refused, keep, sent = send_all(w, request, size);
        unsigned batch_size = 0;
        Text body = {0};
        if (!sent || !read_answer(w, &status, &refused, &keep, &batch_size, w->answer_input >= 0 ? &body : NULL)) {
            free(body.bytes);
            drop_connection(w);
            if (fresh) return AGAIN;
            continue;
        }
        if (!keep) drop_connection(w);
        if (batch_size && w->answer_input < 0) w->batch = batch_size;
        if (status == 408 || status == 429 || status >= 500) {
            free(body.bytes);
            return AGAIN;
        }
        if (w->answer_input >= 0 && status >= 200 && status < 500) { /* a final answer: the app hears it */
            bool formed = form_answer(w, status, &body, p, answer);
            free(body.bytes);
            return formed ? DELIVERED : ANSWER_UNREAD;
        }
        free(body.bytes);
        if ((status >= 200 && status < 300) || (status >= 400 && status < 500 && refused)) return DELIVERED;
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
        Answer answer = {0};
        int outcome = post(w, next, count, &answer);
        pthread_mutex_lock(&w->lock);
        if (outcome == DELIVERED && answer.bytes) { /* for the owner, who then counts it delivered */
            if (w->answer_head + w->answer_count == w->answer_capacity) {
                if (w->answer_head) {
                    memmove(w->answers, w->answers + w->answer_head, w->answer_count * sizeof(*w->answers));
                    w->answer_head = 0;
                } else {
                    size_t capacity = w->answer_capacity ? 2 * w->answer_capacity : 16;
                    Answer *grown = realloc(w->answers, capacity * sizeof(*grown));
                    if (!grown) { free(answer.bytes); outcome = AGAIN; }
                    else { w->answers = grown; w->answer_capacity = capacity; }
                }
            }
            if (outcome == DELIVERED) w->answers[w->answer_head + w->answer_count++] = answer;
        }
        if (outcome == DELIVERED) {
            for (unsigned i = 0; i < count; ++i) {
                free(w->queue[w->head + i].body);
                free(w->queue[w->head + i].sent);
            }
            w->head += count;
            w->count -= count;
            if (!answer.bytes) w->delivered = next[count - 1].seq;
            w->deliveries += count;
            pause_ms = 0;
            pthread_mutex_unlock(&w->lock);
            if (w->notify) w->notify(w->context);
            pthread_mutex_lock(&w->lock);
            continue;
        }
        if (outcome != AGAIN && reported != next[0].seq) {
            fprintf(stderr, "{\"wire\":\"%s\",\"seq\":%llu,\"error\":\"%s\"}\n", w->source,
                    (unsigned long long)next[0].seq, outcome == TLS_FAILED ? "the receiver's certificate is not trusted; trying again" :
                    outcome == ANSWER_UNREAD ? "its answer does not make the answer input's message; trying again" :
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
    pure_tls_close(w->session);
    pure_sealer_free(w->sealer);
    if (w->fd >= 0) close(w->fd);
    for (size_t i = 0; i < w->count; ++i) {
        free(w->queue[w->head + i].body);
        free(w->queue[w->head + i].sent);
    }
    for (size_t i = 0; i < w->answer_count; ++i) free(w->answers[w->answer_head + i].bytes);
    free(w->answers);
    free(w->taken.bytes);
    free(w->queue);
    pthread_cond_destroy(&w->changed);
    pthread_mutex_destroy(&w->lock);
    free(w);
}
