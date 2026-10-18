#define _POSIX_C_SOURCE 200809L
#include "deliver.h"
#include <inttypes.h>
#include "seal.h"
#include "sending.h"
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
#include <sys/epoll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

enum { TIMEOUT_S = 10, FIRST_PAUSE_MS = 100, LONGEST_PAUSE_MS = 10000, GROUPS = PURE_MODULE_NAME / 2, BATCH_MOST = 256,
       ANSWER_MOST = 1 << 20, HEAD_MOST = 64 << 10 };
enum { DELIVERED, AGAIN, REFUSED_BY_PEER, TLS_FAILED, ANSWER_UNREAD };
#define MS UINT64_C(1000000)

typedef struct { char *bytes; size_t size, capacity; bool failed; } Text;

typedef struct { char *body; size_t size; uint64_t seq; unsigned char *sent; size_t sent_size; } Pending;
/* An answer made into its input's message: a table of the fields' starts and
 * sizes (u32le), then the payload from its prefix on. */
typedef struct { uint64_t seq; unsigned char *bytes; size_t size, table; } Answer;

/* Where a request is: none under way, to connect, connecting, in its TLS
 * handshake, written, its answer read. */
enum { IDLE, CONNECT, CONNECTING, HANDSHAKE, WRITING, READING };

struct PureWire {
    PureSending sending; /* its place in the sending loop (link/sending.h) */
    const PureModule *module;
    uint32_t output;
    char host[256], port[8], path[1024], source[PURE_SOURCE + 1];
    pthread_mutex_t lock;
    Pending *queue; /* the sends to deliver, in order, from head */
    size_t held;    /* their bytes */
    size_t head, count, capacity;
    uint64_t queued, delivered, attempts, deliveries;
    unsigned batch; /* how many a request may carry, as the receiver said (X-Pure-Batch); 0: one */
    bool started, stuck;
    void (*notify)(void *);
    void *context;
    int fd;           /* the connection kept for the next send, or -1 */
    bool tls;         /* https: the connection is TLS */
    PureTls *session; /* its TLS session */
    int answer_input; /* the input answers become events of, or -1 */
    Answer *answers;  /* made, for the owner to take in order */
    size_t answer_head, answer_count, answer_capacity;
    uint32_t placed[2 * PURE_MESSAGE_FIELDS];
    /* Sealed sends: to peer's key, authenticated by the node's own key when
     * it has one (its public half is from), in one session made at the first
     * seal (and again after one fails); enc opens it. */
    bool sealed;
    unsigned char peer[PURE_SEAL_KEY], from[PURE_SEAL_KEY], enc[PURE_SEAL_KEY];
    const PureSealKey *own;
    PureSealer *sealer;
    /* Its rounds' own, on the sending loop: the request under way. */
    int phase;
    struct addrinfo *addresses, *trying; /* the receiver's, while it connects */
    bool fresh;                          /* its connection was made for it */
    char *request;
    size_t request_size, written;
    Pending first;     /* the first send it carries */
    unsigned carried;  /* how many */
    uint64_t last;     /* the last one's number */
    uint64_t deadline; /* nothing moved by then: it failed */
    /* Its answer as it comes: got, its head (got_head bytes) and a chunked
     * body after it; what the head says; the body kept when answers are
     * wired. */
    Text got, body;
    size_t got_head;
    bool headed, keep, refused, sized, chunked;
    int status;
    unsigned batch_said;
    uint64_t left; /* a sized body's bytes to come */
    /* After a request failed: none till then. */
    uint64_t pause_until;
    long pause_ms;
    uint64_t reported; /* the first send the last failure was said of */
};

/* TLS verifies a receiver's certificate against the system's CAs or the
 * file pure_wire_trust named (link/tls.h). */
static const char *tls_ca;

void pure_wire_trust(const char *ca_file) { tls_ca = ca_file; }

/* ---- A send as JSON: its fields in order, a group's fields as an object. */

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
        } else if (field->type == PURE_FIELD_BOOL) {
            put(t, payload.data[field->offset] ? "true" : "false", payload.data[field->offset] ? 4 : 5);
        } else if (field->type == PURE_FIELD_I64) { /* every digit */
            uint64_t bits = read_u32(payload.data + field->offset) | (uint64_t)read_u32(payload.data + field->offset + 4) << 32;
            put(t, digits, (size_t)snprintf(digits, sizeof(digits), "%" PRId64, (int64_t)bits));
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

static uint64_t wire_round(PureSending *, uint32_t ready);

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
    pure_sending_init(&w->sending, wire_round);
    return w;
}

uint32_t pure_wire_output(const PureWire *w) { return w->output; }

bool pure_wire_reply(PureWire *w, uint32_t input, char *error, size_t size) {
    const PureModule *m = w->module;
    if (input >= m->inputs || m->input[input].kind != PURE_KIND_MESSAGE) {
        snprintf(error, size, "an answer goes to a message input");
        return false;
    }
    w->answer_input = (int)input;
    return true;
}

bool pure_wire_answer(PureWire *w, PureWireAnswer *out) {
    pthread_mutex_lock(&w->lock);
    bool some = w->answer_count > 0;
    Answer next = some ? w->answers[w->answer_head] : (Answer){0}; /* its bytes stay put while the array moves */
    pthread_mutex_unlock(&w->lock);
    if (!some) return false;
    const PureInputShape *in = &w->module->input[w->answer_input];
    for (size_t i = 0; i < 2 * (size_t)in->field_count; ++i) w->placed[i] = read_u32(next.bytes + 4 * i);
    *out = (PureWireAnswer){(uint32_t)w->answer_input, next.seq, {next.bytes + next.table, next.size - next.table},
                            w->placed, w->source};
    return true;
}

void pure_wire_answered(PureWire *w) {
    pthread_mutex_lock(&w->lock);
    Answer done = w->answers[w->answer_head++];
    --w->answer_count;
    w->delivered = done.seq; /* trimmed after the owner applies it */
    pthread_mutex_unlock(&w->lock);
    free(done.bytes);
}
uint64_t pure_wire_queued(const PureWire *w) { return w->queued; }

uint64_t pure_wire_delivered(PureWire *w) {
    pthread_mutex_lock(&w->lock);
    uint64_t delivered = w->delivered;
    pthread_mutex_unlock(&w->lock);
    return delivered;
}

size_t pure_wire_held(PureWire *w) {
    pthread_mutex_lock(&w->lock);
    size_t held = w->held;
    pthread_mutex_unlock(&w->lock);
    return held;
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
    w->held += t.size + (sent ? payload.size : 0);
    pthread_mutex_unlock(&w->lock);
    return true;
}

void pure_wire_push(PureWire *w) { pure_sending_push(&w->sending); }

/* ---- Requests. */

/* A fault a Pure receiver answers when its app refused the message: any
 * but memory (its budget: it may take the message later). */
static bool refusal(const char *error) {
    for (unsigned f = PURE_FAULT_DIVIDE; f < PURE_FAULT_COUNT; ++f)
        if (f != PURE_FAULT_MEMORY && !strcmp(error, pure_fault_name((PureFault)f))) return true;
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
        if (pure_field_fixed(field->type))
            memcpy(prefix + field->offset, p->sent + from->offset, pure_field_size(field->type));
        else {
            write_u32(bytes + 8 * f, (uint32_t)(copy + start - prefix));
            write_u32(bytes + 8 * f + 4, (uint32_t)length);
        }
    }
    *out = (Answer){p->seq, bytes, total, table};
    return true;
}

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
 * an array of them for a receiver that takes batches; NULL when it cannot
 * be made (memory, a seal). */
static char *make_request(PureWire *w, const Pending *p, unsigned count, size_t *size) {
    size_t body = count > 1 ? count + 1 : 0;
    for (unsigned i = 0; i < count; ++i) body += p[i].size;
    size_t length = body + (w->sealed ? PURE_SEAL_OVERHEAD : 0);
    char batch[32] = "", seal[256] = "";
    if (count > 1) snprintf(batch, sizeof(batch), "X-Pure-Count: %u\r\n", count);
    unsigned char *sealed = w->sealed ? seal_body(w, p, count, body, seal, sizeof(seal)) : NULL;
    if (w->sealed && !sealed) return NULL;
    /* The request in one piece, its head and then a batch in its brackets:
     * one write where there were two a send (a batch drained at 91k sends a
     * second, and at 470k in one piece). */
    enum { HEAD = 2048 }; /* the longest path, host, source and seal leave room to spare */
    char *request = malloc(HEAD + length);
    if (!request) {
        free(sealed);
        return NULL;
    }
    int head = snprintf(request, HEAD,
                        "POST %s HTTP/1.1\r\nHost: %s:%s\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n"
                        "X-Pure-Source: %s\r\nX-Pure-Sequence: %llu\r\n%s%sIdempotency-Key: %s-%llu\r\n\r\n",
                        w->path, w->host, w->port, length, w->source, (unsigned long long)p->seq, batch, seal, w->source,
                        (unsigned long long)p->seq);
    if (sealed) memcpy(request + head, sealed, length);
    else put_body((unsigned char *)request + head, p, count);
    free(sealed);
    *size = (size_t)head + length;
    return request;
}

/* ---- HTTP/1.1, one request at a time on a kept connection, in the wire's
 * rounds on the sending loop: each goes as far as its socket lets it, and
 * then waits for the socket (or its deadline, TIMEOUT_S after the last
 * bytes moved). */

/* Where the head ends in b (after its blank line), looking from from; 0
 * while it has not. */
static size_t head_end(const char *b, size_t size, size_t from) {
    for (const char *at = memchr(b + from, '\n', size - from); at; at = memchr(at + 1, '\n', (size_t)(b + size - at - 1))) {
        size_t i = (size_t)(at - b);
        if (i + 1 < size && b[i + 1] == '\n') return i + 2;
        if (i + 2 < size && b[i + 1] == '\r' && b[i + 2] == '\n') return i + 3;
    }
    return 0;
}

/* The head's lines (size bytes of b): the status, and the headers a wire
 * heeds. False when it is no HTTP/1 answer. */
static bool read_head(PureWire *w, const char *b, size_t size) {
    w->sized = w->chunked = w->refused = false;
    w->left = 0;
    w->batch_said = 0;
    for (size_t at = 0, n = 0; at < size; ++n) {
        const char *end = memchr(b + at, '\n', size - at);
        if (!end) return false;
        size_t length = (size_t)(end - (b + at));
        if (length && end[-1] == '\r') --length;
        char line[1024];
        if (length >= sizeof(line)) return false;
        memcpy(line, b + at, length);
        line[length] = 0;
        at = (size_t)(end - b) + 1;
        if (!n) {
            if (length < 12 || strncmp(line, "HTTP/1.", 7)) return false;
            w->status = atoi(line + 9);
            w->keep = line[7] == '1';
            continue;
        }
        if (!*line) break;
        char *value = strchr(line, ':');
        if (!value) return false;
        *value++ = 0;
        while (*value == ' ' || *value == '\t') ++value;
        if (!strcasecmp(line, "Content-Length")) { w->left = strtoull(value, NULL, 10); w->sized = true; }
        else if (!strcasecmp(line, "Transfer-Encoding")) w->chunked = !strcasecmp(value, "chunked");
        else if (!strcasecmp(line, "Connection")) w->keep = strcasecmp(value, "close") != 0 && w->keep;
        else if (!strcasecmp(line, "X-Pure-Error")) w->refused = refusal(value);
        else if (!strcasecmp(line, "X-Pure-Batch")) w->batch_said = (unsigned)strtoul(value, NULL, 10);
    }
    return true;
}

/* A chunked body after the head, as far as it came (closed: no more
 * comes), made again from its start each time (such answers are few and
 * small): 1 once it is whole (kept when answers are wired), 0 while more
 * must come, -1 when it cannot be. */
static int chunks(PureWire *w, bool closed) {
    const char *b = w->got.bytes + w->got_head;
    size_t size = w->got.size - w->got_head, at = 0;
    w->body.size = 0;
    for (;;) {
        const char *end = memchr(b + at, '\n', size - at);
        if (!end) return closed ? -1 : 0;
        uint64_t chunk = strtoull(b + at, NULL, 16);
        at = (size_t)(end - b) + 1;
        if (!chunk) break;
        if (chunk > size - at) return closed ? -1 : 0;
        if (w->answer_input >= 0) {
            if (chunk > ANSWER_MOST - w->body.size) return -1;
            put(&w->body, b + at, (size_t)chunk);
        }
        at += (size_t)chunk;
        if (!(end = memchr(b + at, '\n', size - at))) return closed ? -1 : 0;
        at = (size_t)(end - b) + 1;
    }
    for (;;) { /* its trailers, to a blank line (or the close) */
        const char *end = memchr(b + at, '\n', size - at);
        if (!end) return closed ? 1 : 0;
        size_t length = (size_t)(end - (b + at));
        at = (size_t)(end - b) + 1;
        if (!length || (length == 1 && end[-1] == '\r')) return w->body.failed ? -1 : 1;
    }
}

/* Bytes of the answer as they come (closed: no more come): 1 once it is
 * whole, 0 while more must come, -1 when it is no answer or too large. */
static int answer_bytes(PureWire *w, const char *bytes, size_t n, bool closed) {
    if (!w->headed) {
        size_t before = w->got.size;
        put(&w->got, bytes, n);
        size_t end = w->got.failed ? 0 : head_end(w->got.bytes, w->got.size, before >= 2 ? before - 2 : 0);
        if (!end) return closed || w->got.failed || w->got.size > HEAD_MOST ? -1 : 0;
        if (!read_head(w, w->got.bytes, end)) return -1;
        w->headed = true;
        w->got_head = end;
        if (w->status == 204 || w->status == 304 || w->status < 200) return 1;
        if (w->chunked) return chunks(w, closed);
        bytes = w->got.bytes + end; /* the body's first bytes */
        n = w->got.size - end;
    } else if (w->chunked) {
        put(&w->got, bytes, n);
        return w->got.failed || w->got.size - w->got_head > ANSWER_MOST + HEAD_MOST ? -1 : chunks(w, closed);
    }
    size_t taken = w->sized && n > w->left ? (size_t)w->left : n;
    if (w->answer_input >= 0) {
        if (taken > ANSWER_MOST - w->body.size) return -1;
        put(&w->body, bytes, taken);
        if (w->body.failed) return -1;
    }
    if (w->sized) {
        w->left -= taken;
        return !w->left ? 1 : closed ? -1 : 0;
    }
    w->keep = false; /* the body runs to the close */
    return closed ? 1 : 0;
}

static void drop(PureWire *w) {
    pure_sending_watch(&w->sending, -1, 0); /* before its socket closes */
    pure_tls_close(w->session);
    w->session = NULL;
    if (w->fd >= 0) close(w->fd);
    w->fd = -1;
}

/* The request is done with: delivered, or to be tried again after a pause
 * (0.1 s doubling to 10 s; 10 s for one the receiver does not take). */
static void finish(PureWire *w, int outcome, Answer *answer) {
    free(w->request);
    w->request = NULL;
    w->phase = IDLE;
    w->headed = false;
    w->got.size = w->body.size = 0;
    w->got.failed = w->body.failed = false;
    if (w->addresses) freeaddrinfo(w->addresses);
    w->addresses = w->trying = NULL;
    if (w->fd >= 0) pure_sending_watch(&w->sending, w->fd, EPOLLIN); /* kept: heard if it closes */
    pthread_mutex_lock(&w->lock);
    if (outcome == DELIVERED && answer && answer->bytes) { /* for the owner, who then counts it delivered */
        if (w->answer_head + w->answer_count == w->answer_capacity) {
            if (w->answer_head) {
                memmove(w->answers, w->answers + w->answer_head, w->answer_count * sizeof(*w->answers));
                w->answer_head = 0;
            } else {
                size_t capacity = w->answer_capacity ? 2 * w->answer_capacity : 16;
                Answer *grown = realloc(w->answers, capacity * sizeof(*grown));
                if (!grown) { free(answer->bytes); outcome = AGAIN; }
                else { w->answers = grown; w->answer_capacity = capacity; }
            }
        }
        if (outcome == DELIVERED) w->answers[w->answer_head + w->answer_count++] = *answer;
    }
    if (outcome == DELIVERED) {
        for (unsigned i = 0; i < w->carried; ++i) {
            const Pending *p = &w->queue[w->head + i];
            w->held -= p->size + (p->sent ? p->sent_size : 0);
            free(p->body);
            free(p->sent);
        }
        w->head += w->carried;
        w->count -= w->carried;
        if (!answer || !answer->bytes) w->delivered = w->last;
        w->deliveries += w->carried;
        w->pause_ms = 0;
        pthread_mutex_unlock(&w->lock);
        if (w->notify) w->notify(w->context);
        return;
    }
    pthread_mutex_unlock(&w->lock);
    if (outcome != AGAIN && w->reported != w->first.seq) {
        fprintf(stderr, "{\"wire\":\"%s\",\"seq\":%llu,\"error\":\"%s\"}\n", w->source, (unsigned long long)w->first.seq,
                outcome == TLS_FAILED ? "the receiver's certificate is not trusted; trying again" :
                outcome == ANSWER_UNREAD ? "its answer does not make the answer input's message; trying again" :
                "the receiver does not take it; trying again");
        w->reported = w->first.seq;
    }
    w->pause_ms = outcome != AGAIN ? LONGEST_PAUSE_MS : w->pause_ms ? 2 * w->pause_ms : FIRST_PAUSE_MS;
    if (w->pause_ms > LONGEST_PAUSE_MS) w->pause_ms = LONGEST_PAUSE_MS;
    w->pause_until = pure_sending_now() + (uint64_t)w->pause_ms * MS;
}

/* The request failed on its connection: the connection goes, and a request
 * on one kept from before is tried once more on a new one. */
static void failed(PureWire *w, int outcome) {
    bool again = outcome == AGAIN && !w->fresh;
    drop(w);
    if (!again) {
        finish(w, outcome, NULL);
        return;
    }
    w->phase = CONNECT;
    w->written = 0;
    w->headed = false;
    w->got.size = w->body.size = 0;
    w->got.failed = w->body.failed = false;
}

/* The next request, from the sends queued (as many as the receiver takes
 * at once: their bodies stay put while the queue only grows behind them):
 * false when none waits. */
static bool begin(PureWire *w, uint64_t now) {
    Pending next[BATCH_MOST];
    pthread_mutex_lock(&w->lock);
    unsigned count = w->batch > 1 ? (unsigned)(w->count < w->batch ? w->count : w->batch) : w->count ? 1 : 0;
    if (count > BATCH_MOST) count = BATCH_MOST;
    memcpy(next, w->queue + w->head, count * sizeof(*next));
    if (count) ++w->attempts;
    pthread_mutex_unlock(&w->lock);
    if (!count) return false;
    w->first = next[0];
    w->carried = count;
    w->last = next[count - 1].seq;
    w->written = 0;
    w->deadline = now + TIMEOUT_S * 1000 * MS;
    if (!(w->request = make_request(w, next, count, &w->request_size))) finish(w, AGAIN, NULL);
    else w->phase = w->fd >= 0 ? WRITING : CONNECT;
    w->fresh = w->fd < 0;
    return true;
}

/* A socket connecting to the next of the receiver's addresses (watched to
 * be writable: connected); false when none is left. */
static bool connect_next(PureWire *w) {
    for (; w->trying; w->trying = w->trying->ai_next) {
        struct addrinfo *a = w->trying;
        int fd = socket(a->ai_family, a->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC, a->ai_protocol), one = 1;
        if (fd < 0) continue;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        if (!connect(fd, a->ai_addr, a->ai_addrlen) || errno == EINPROGRESS) {
            w->fd = fd;
            pure_sending_watch(&w->sending, fd, EPOLLOUT);
            return true;
        }
        close(fd);
    }
    return false;
}

/* The request's bytes to the socket as far as it takes them: 1 all
 * written, 0 when it waits for the socket, -1 when it failed. */
static int write_some(PureWire *w) {
    while (w->written < w->request_size) {
        size_t left = w->request_size - w->written;
        ssize_t n;
        if (w->session) {
            int moved = pure_tls_write(w->session, w->request + w->written, left < INT32_MAX ? (int)left : INT32_MAX);
            if (moved == PURE_TLS_WANTS_READ || moved == PURE_TLS_WANTS_WRITE) {
                pure_sending_watch(&w->sending, w->fd, moved == PURE_TLS_WANTS_READ ? EPOLLIN : EPOLLOUT);
                return 0;
            }
            n = moved;
        } else {
            n = send(w->fd, w->request + w->written, left, MSG_NOSIGNAL);
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                pure_sending_watch(&w->sending, w->fd, EPOLLOUT);
                return 0;
            }
        }
        if (n <= 0) return -1;
        w->written += (size_t)n;
        w->deadline = pure_sending_now() + TIMEOUT_S * 1000 * MS;
    }
    return 1;
}

/* The answer's bytes from the socket as far as they came: 1 once it is
 * whole, 0 when it waits for more, -1 when it failed. */
static int read_some(PureWire *w) {
    char bytes[16384];
    for (;;) {
        ssize_t n;
        if (w->session) {
            int moved = pure_tls_read(w->session, bytes, (int)sizeof(bytes));
            if (moved == PURE_TLS_WANTS_READ || moved == PURE_TLS_WANTS_WRITE) {
                pure_sending_watch(&w->sending, w->fd, moved == PURE_TLS_WANTS_READ ? EPOLLIN : EPOLLOUT);
                return 0;
            }
            n = moved; /* closed, or failed: as the close */
        } else {
            n = recv(w->fd, bytes, sizeof(bytes), 0);
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                pure_sending_watch(&w->sending, w->fd, EPOLLIN);
                return 0;
            }
        }
        if (n > 0) {
#ifdef TCP_QUICKACK
            /* Acknowledge at once: a server that writes an answer in pieces
             * holds the next piece until the first is acknowledged (Nagle),
             * which a delayed acknowledgement would stall for 40 ms. The flag
             * does not stay set. */
            int one = 1;
            setsockopt(w->fd, IPPROTO_TCP, TCP_QUICKACK, &one, sizeof(one));
#endif
            w->deadline = pure_sending_now() + TIMEOUT_S * 1000 * MS;
        }
        int whole = answer_bytes(w, bytes, n > 0 ? (size_t)n : 0, n <= 0);
        if (whole || n <= 0) return whole ? whole : -1;
    }
}

/* The whole answer: what came of the request. */
static void answered(PureWire *w) {
    if (w->batch_said && w->answer_input < 0) w->batch = w->batch_said;
    if (!w->keep) drop(w);
    int status = w->status, outcome;
    Answer answer = {0};
    if (status == 408 || status == 429 || status >= 500) outcome = AGAIN;
    else if (w->answer_input >= 0 && status >= 200 && status < 500) /* a final answer: the app hears it */
        outcome = form_answer(w, status, &w->body, &w->first, &answer) ? DELIVERED : ANSWER_UNREAD;
    else if ((status >= 200 && status < 300) || (status >= 400 && status < 500 && w->refused)) outcome = DELIVERED;
    else outcome = REFUSED_BY_PEER;
    finish(w, outcome, &answer);
}

/* A round: the request under way taken as far as it goes, and the next
 * begun when it is done. When the wire wants its next round. */
static uint64_t wire_round(PureSending *s, uint32_t ready) {
    PureWire *w = (PureWire *)s;
    for (;;) {
        uint64_t now = pure_sending_now();
        if (w->phase >= CONNECTING && !ready && now >= w->deadline) { /* nothing moved for TIMEOUT_S */
            failed(w, AGAIN);
            continue;
        }
        switch (w->phase) {
        case IDLE:
            if ((ready & (EPOLLIN | EPOLLHUP | EPOLLERR)) && w->fd >= 0) drop(w); /* a kept connection that speaks unasked is closing */
            ready = 0;
            if (now < w->pause_until) return w->pause_until;
            if (!begin(w, now)) return 0;
            continue;
        case CONNECT:
            w->fresh = true;
            if (!w->addresses) {
                struct addrinfo hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM};
                if (getaddrinfo(w->host, w->port, &hints, &w->addresses)) w->addresses = NULL; /* a name looked up waits here */
                w->trying = w->addresses;
            }
            if (!connect_next(w)) {
                finish(w, AGAIN, NULL);
                continue;
            }
            w->phase = CONNECTING;
            w->deadline = now + TIMEOUT_S * 1000 * MS;
            return w->deadline;
        case CONNECTING: {
            if (!ready) return w->deadline;
            ready = 0;
            int error = 0;
            socklen_t size = sizeof(error);
            if (getsockopt(w->fd, SOL_SOCKET, SO_ERROR, &error, &size) || error) { /* the next address */
                drop(w);
                w->trying = w->trying->ai_next;
                w->phase = CONNECT;
                continue;
            }
            freeaddrinfo(w->addresses);
            w->addresses = w->trying = NULL;
            pure_sending_watch(s, w->fd, EPOLLIN);
            w->deadline = now + TIMEOUT_S * 1000 * MS;
            if (!w->tls) w->phase = WRITING;
            else if ((w->session = pure_tls_open(w->host, w->fd))) w->phase = HANDSHAKE; /* the certificate must name the host */
            else failed(w, TLS_FAILED);
            continue;
        }
        case HANDSHAKE: {
            ready = 0;
            int step = pure_tls_step(w->session);
            if (step == PURE_TLS_WANTS_READ || step == PURE_TLS_WANTS_WRITE) {
                pure_sending_watch(s, w->fd, step == PURE_TLS_WANTS_READ ? EPOLLIN : EPOLLOUT);
                return w->deadline;
            }
            if (step != 1) {
                failed(w, TLS_FAILED);
                continue;
            }
            pure_sending_watch(s, w->fd, EPOLLIN);
            w->phase = WRITING;
            continue;
        }
        case WRITING: {
            ready = 0;
            int written = write_some(w);
            if (written < 0) failed(w, AGAIN);
            else if (!written) return w->deadline;
            else { /* the answer, once the socket has it */
                pure_sending_watch(s, w->fd, EPOLLIN);
                w->phase = READING;
                return w->deadline;
            }
            continue;
        }
        case READING: {
            if (!ready) return w->deadline;
            ready = 0;
            int whole = read_some(w);
            if (whole < 0) failed(w, AGAIN);
            else if (!whole) return w->deadline;
            else answered(w);
            continue;
        }
        }
    }
}

bool pure_wire_start(PureWire *w, void (*notify)(void *), void *context) {
    w->notify = notify;
    w->context = context;
    char why[128];
    w->started = pure_sending_join(&w->sending, why, sizeof(why));
    return w->started;
}

void pure_wire_close(PureWire *w) {
    if (!w) return;
    pure_sending_leave(&w->sending); /* no round of it after */
    drop(w);
    if (w->addresses) freeaddrinfo(w->addresses);
    free(w->request);
    free(w->got.bytes);
    free(w->body.bytes);
    pure_sealer_free(w->sealer);
    for (size_t i = 0; i < w->count; ++i) {
        free(w->queue[w->head + i].body);
        free(w->queue[w->head + i].sent);
    }
    for (size_t i = 0; i < w->answer_count; ++i) free(w->answers[w->answer_head + i].bytes);
    free(w->answers);
    free(w->queue);
    pthread_mutex_destroy(&w->lock);
    free(w);
}
