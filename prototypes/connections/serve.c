#define _GNU_SOURCE
/* pure-serve: the HTTP host for aup modules (docs/research/075).
 *
 * An app's views and inputs are its HTTP interface, with no routes to write:
 * GET /views/NAME answers a view (a number or text as text/plain, a table as
 * JSON rows, ?from=N&count=M for a page of them) and POST /inputs/NAME
 * delivers an event (a text input's text is the body; a key input is
 * pressed, down then up; a message takes its u32 fields from the query and
 * its bytes field from the body, and answers what its handler replies, a
 * field of the message). A failed event answers with its fault.
 *
 * A module with keyed messages named show and edit also serves the document
 * contract of server.c (GET and PUT /objects/KEY, If-Match or If-None-Match:
 * *, ETag = revision, X-Pure-Error): GET is its show message and PUT its edit
 * message. HTTP/1.1 connections persist
 * and may pipeline; one epoll owner thread accepts, reads and writes without
 * blocking. Requests go through the runtime core (a work slot per
 * connection, the payload budget, ingress owned by the core, PUT replies
 * borrowed from the request). After each round of events every ready
 * request is applied to the module with deferred saving, one save stores
 * the burst, and only then are its requests answered: an answer is durable,
 * and a failed save fails the whole burst, reads included.
 *
 * With --threads N each thread is a shard with its own listener on the same
 * port (SO_REUSEPORT), epoll set, runtime domain, app and store handle; the
 * kernel spreads connections over them. Shards share nothing but the store,
 * whose commits take turns and whose revisions settle concurrent edits of a
 * key. A module with states (not only keyed documents) runs on one thread. */
#include "module-store.h"
#include "module-text.h"
#include "runtime.h"
#include "store.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

enum { HEADER_LIMIT = 4096, EVENTS = 256 };
typedef enum { CLOSED, READING_HEAD, READING_BODY, WAITING, WRITING } Phase;
typedef struct Host Host;

/* What a request names: a document, a view (a table's rows from, count) or
 * an input. Kept per work slot, so an accepted event runs even if its
 * connection is gone before the burst. */
typedef enum { ROUTE_NONE, ROUTE_OBJECT, ROUTE_VIEW, ROUTE_INPUT } RouteKind;
typedef struct { RouteKind kind; uint32_t target; size_t from, count; } Route;

/* Allocated when accepted and reused after closing: memory follows the
 * connections that are open, not the configured maximum. */
typedef struct Connection {
    Host *host;
    struct Connection *next; /* on the spare or the closed list */
    size_t index;            /* in the host's table */
    int fd;
    Phase phase;
    bool readable, writable, keep_alive, put, post;
    uint64_t deadline;
    char key[65]; /* the document's key, or the view's or input's name */
    RouteKind route;
    unsigned char fields[4 * PURE_MODULE_FIELDS]; /* a message's u32 fields, from the query */
    size_t field_bytes;
    int64_t expected;
    size_t body_size, received, in_used;
    unsigned char *body;
    PureTicket ticket;
    struct iovec *out, small[4]; /* the answer: its head, then its body's extents */
    size_t out_count, out_at;
    char head[320];
    char in[HEADER_LIMIT]; /* the header being read, then pipelined bytes */
} Connection;

/* One request applied in the current burst. A read answered from the store
 * lends its extents: they point into a store mapping, which the resource holds
 * and the runtime charges and releases with the ticket. */
typedef struct {
    Connection *connection;
    PureTicket ticket;
    PureInput input;
    PureResult status;
    int64_t revision;
    PureResource resource;
    PureSpan reply;
    struct iovec *extents;
    size_t extent_count;
    char key[65];
    RouteKind route;
    PureFault fault;  /* a failed event's */
    const char *type; /* a view's content type */
} Applied;

struct Host {
    const PureModule *module;
    PureStore *store;
    PureApp app;
    uint32_t show, edit;
    bool documents; /* the module serves /objects: it has show and edit */
    Route *routes;  /* by work slot */
    PureRuntime runtime;
    PureWork *work;
    Connection **table;   /* open connections by index */
    Connection **by_slot; /* the connection waiting on a work slot */
    Connection *spare, *closed;
    Applied *applied;
    size_t capacity, active;
    int epoll, listener;
    uint64_t timeout_ms;
    uint64_t accepted, requests, bursts, burst_events, expired;
};

static atomic_bool stopping; /* lock-free, so a signal handler may set it */
static void stop(int signal_number) { (void)signal_number; atomic_store(&stopping, true); }

static uint64_t now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}

static bool number(const char *text, uint64_t maximum, uint64_t *value) {
    if (!*text) return false;
    uint64_t n = 0;
    for (; *text; ++text) {
        if (*text < '0' || *text > '9') return false;
        unsigned digit = (unsigned)(*text - '0');
        if (n > (maximum - digit) / 10) return false;
        n = n * 10 + digit;
    }
    *value = n;
    return true;
}

/* ---- Connections ------------------------------------------------------ */

static void release_ticket(void *owner) {
    Connection *c = owner;
    if (c->ticket.runtime && !pure_runtime_release(c->ticket)) abort();
    c->ticket = (PureTicket){0};
    c->body = NULL;
}

/* Everything an answer borrowed goes back with its ticket: the request's
 * payload, a copied reply or the lent store extents. */
static void finish_output(Connection *c) {
    release_ticket(c);
    if (c->out != c->small) free(c->out);
    c->out = NULL;
    c->out_count = c->out_at = 0;
}

/* Events of this round may still name c: it is reused after the round. */
static void close_connection(Connection *c) {
    if (c->phase == CLOSED) return;
    Host *host = c->host;
    finish_output(c);
    close(c->fd); /* also leaves the epoll set */
    c->phase = CLOSED;
    host->table[c->index] = NULL;
    c->next = host->closed;
    host->closed = c;
    --host->active;
}

/* Answer with a body of `count` extents (`size` bytes in all); the ticket
 * keeps them alive. One writev sends as much as the socket takes. */
static void respond_typed(Connection *c, int status, const char *reason, const char *error, int64_t revision,
                          const char *type, const struct iovec *extents, size_t count, size_t size) {
    char tag[48] = "";
    if (revision > 0) snprintf(tag, sizeof(tag), "ETag: \"%" PRId64 "\"\r\n", revision);
    int length = snprintf(c->head, sizeof(c->head),
        "HTTP/1.1 %d %s\r\nContent-Length: %zu\r\n%sContent-Type: %s\r\n"
        "Cache-Control: no-store\r\nX-Pure-Error: %s\r\n%s\r\n",
        status, reason, size, c->keep_alive ? "" : "Connection: close\r\n", type, error, tag);
    c->out = count < sizeof(c->small) / sizeof(*c->small) ? c->small : malloc((count + 1) * sizeof(*c->out));
    c->phase = WRITING;
    if (!c->out) { c->keep_alive = false; c->out = c->small; count = 0; } /* cannot send it: the close tells */
    c->out[0] = (struct iovec){c->head, (size_t)length};
    if (count) memcpy(c->out + 1, extents, count * sizeof(*extents));
    c->out_count = count + 1;
    c->out_at = 0;
}

static void respond_extents(Connection *c, int status, const char *reason, const char *error, int64_t revision,
                            const struct iovec *extents, size_t count, size_t size) {
    respond_typed(c, status, reason, error, revision, "application/octet-stream", extents, count, size);
}

static void respond(Connection *c, int status, const char *reason, const char *error, int64_t revision,
                    PureSpan body) {
    struct iovec extent = {(void *)body.data, body.size};
    respond_extents(c, status, reason, error, revision, &extent, body.size ? 1 : 0, body.size);
}

/* A request refused before it has a ticket; a body it came with is not read. */
static void reject(Connection *c, int status, const char *reason, const char *error) {
    if (c->body_size) c->keep_alive = false;
    respond(c, status, reason, error, 0, (PureSpan){0});
}

/* A request the host cannot read further: answer and close. */
static void refuse(Connection *c, int status, const char *reason, const char *error) {
    release_ticket(c);
    c->keep_alive = false;
    respond(c, status, reason, error, 0, (PureSpan){0});
}

static void answer(Connection *c, PureOutcome result) {
    switch (result.status) {
    case PURE_OK: respond(c, 200, "OK", "none", result.revision, result.body); return;
    case PURE_CONFLICT: respond(c, 412, "Precondition Failed", "conflict", result.revision, (PureSpan){0}); return;
    case PURE_NOT_FOUND: respond(c, 404, "Not Found", "not_found", 0, (PureSpan){0}); break;
    case PURE_BUSY: respond(c, 503, "Service Unavailable", "store_busy", 0, (PureSpan){0}); break;
    case PURE_BUDGET: respond(c, 503, "Service Unavailable", "payload_budget", 0, (PureSpan){0}); break;
    case PURE_ALLOCATION: respond(c, 503, "Service Unavailable", "allocation", 0, (PureSpan){0}); break;
    case PURE_CLOSED: respond(c, 503, "Service Unavailable", "stopped", 0, (PureSpan){0}); break;
    case PURE_DENIED: respond(c, 403, "Forbidden", "denied", 0, (PureSpan){0}); break;
    case PURE_LIMIT: respond(c, 413, "Content Too Large", "limit", 0, (PureSpan){0}); break;
    case PURE_UNCERTAIN: respond(c, 500, "Internal Server Error", "durability_unknown", 0, (PureSpan){0}); break;
    default: respond(c, 500, "Internal Server Error", "storage", 0, (PureSpan){0}); break;
    }
    /* A failed request carries no body: the payload can go now. */
    if (!result.body.size) release_ticket(c);
}

static const char *const fault_names[] = {"none", "divide", "non_finite", "clamp", "limit", "not_found", "encoding",
                                          "exists"};

/* An app route's answer: a view's value, or what an event replied. A failed
 * event names its fault: 404 not_found, 409 exists, 422 for the others;
 * a view without a value is 404 no_value. */
static void answer_app(Connection *c, PureOutcome result, bool view, PureFault fault, const char *type) {
    const char *named = fault != PURE_FAULT_NONE ? fault_names[fault] : NULL;
    if (result.status == PURE_OK) {
        struct iovec extent = {(void *)result.body.data, result.body.size};
        respond_typed(c, 200, "OK", "none", 0, type ? type : "application/octet-stream", &extent,
                      result.body.size ? 1 : 0, result.body.size);
    } else if (result.status == PURE_NOT_FOUND) {
        respond(c, 404, "Not Found", view ? "no_value" : named ? named : "not_found", 0, (PureSpan){0});
    } else if (result.status == PURE_CONFLICT) {
        respond(c, 409, "Conflict", named ? named : "conflict", 0, (PureSpan){0});
    } else if (named && (result.status == PURE_LIMIT || result.status == PURE_INVALID)) {
        respond(c, 422, "Unprocessable Content", named, 0, (PureSpan){0});
    } else if (result.status == PURE_INVALID) {
        respond(c, 400, "Bad Request", "invalid", 0, (PureSpan){0}); /* such as a text that is not UTF-8 */
    } else {
        answer(c, result);
        return;
    }
    if (!result.body.size) release_ticket(c);
}

/* Parse one request head of `size` bytes (through the blank line): the same
 * bounded HTTP/1.1 subset as server.c, plus Connection, POST and the app
 * routes. A query stays in head for start_request; an unknown path is
 * answered 404 once its head is read. */
static bool parse_head(Connection *c, char *head, size_t size, char **query) {
    for (size_t i = 0; i < size; ++i) {
        unsigned char b = (unsigned char)head[i];
        if ((b < 32 && b != '\r' && b != '\n' && b != '\t') || b == 127) return false;
    }
    head[size - 2] = 0; /* keep the last CRLF of the last header */
    char *line = head, *next = strstr(line, "\r\n");
    if (!next) return false;
    *next = 0;
    char *path = strchr(line, ' ');
    if (!path) return false;
    *path++ = 0;
    c->put = !strcmp(line, "PUT");
    c->post = !strcmp(line, "POST");
    if (!c->put && !c->post && strcmp(line, "GET")) return false;
    char *version = strchr(path, ' ');
    if (!version) return false;
    *version++ = 0;
    if (strcmp(version, "HTTP/1.1")) return false;
    if ((*query = strchr(path, '?'))) *(*query)++ = 0;
    static const struct { const char *prefix; RouteKind kind; } routes[] = {
        {"/objects/", ROUTE_OBJECT}, {"/views/", ROUTE_VIEW}, {"/inputs/", ROUTE_INPUT}};
    const char *key = "";
    c->route = ROUTE_NONE;
    for (size_t i = 0; i < sizeof(routes) / sizeof(*routes) && c->route == ROUTE_NONE; ++i)
        if (!strncmp(path, routes[i].prefix, strlen(routes[i].prefix))) {
            c->route = routes[i].kind;
            key = path + strlen(routes[i].prefix);
        }
    size_t key_size = strlen(key);
    if (c->route != ROUTE_NONE && (!key_size || key_size > 64)) return false;
    for (size_t i = 0; i < key_size; ++i) {
        char b = key[i];
        if (!((b >= 'a' && b <= 'z') || (b >= 'A' && b <= 'Z') || (b >= '0' && b <= '9') || b == '-' || b == '_'))
            return false;
    }
    memcpy(c->key, key, key_size + 1);
    /* Each route has its method: documents GET and PUT, views GET, inputs POST. */
    if ((c->route == ROUTE_OBJECT && (c->post || *query)) || (c->route == ROUTE_VIEW && (c->put || c->post)) ||
        (c->route == ROUTE_INPUT && !c->post)) return false;
    bool length_seen = false, condition_seen = false, host_seen = false;
    c->body_size = 0;
    c->expected = -1;
    c->keep_alive = true;
    line = next + 2;
    while (*line) {
        next = strstr(line, "\r\n");
        if (!next) return false;
        *next = 0;
        char *value = strchr(line, ':');
        if (!value || value == line) return false;
        *value++ = 0;
        for (char *p = line; *p; ++p)
            if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '-'))
                return false;
        while (*value == ' ' || *value == '\t') ++value;
        char *tail = value + strlen(value);
        while (tail > value && (tail[-1] == ' ' || tail[-1] == '\t')) *--tail = 0;
        uint64_t n;
        if (!strcasecmp(line, "Content-Length")) {
            if (length_seen || !number(value, SIZE_MAX, &n)) return false;
            length_seen = true;
            c->body_size = (size_t)n;
        } else if (!strcasecmp(line, "Host")) {
            if (host_seen || !*value) return false;
            host_seen = true;
        } else if (!strcasecmp(line, "If-Match")) {
            if (condition_seen || tail - value < 3 || *value != '"' || tail[-1] != '"') return false;
            tail[-1] = 0;
            if (!number(value + 1, INT64_MAX, &n) || !n) return false;
            c->expected = (int64_t)n;
            condition_seen = true;
        } else if (!strcasecmp(line, "If-None-Match")) {
            if (condition_seen || strcmp(value, "*")) return false;
            c->expected = 0;
            condition_seen = true;
        } else if (!strcasecmp(line, "Connection")) {
            if (!strcasecmp(value, "close")) c->keep_alive = false;
            else if (strcasecmp(value, "keep-alive")) return false;
        } else if (!strcasecmp(line, "Transfer-Encoding") || !strcasecmp(line, "Expect")) return false;
        line = next + 2;
    }
    if (c->put) return host_seen && length_seen && condition_seen;
    if (c->post) return host_seen && !condition_seen; /* no Content-Length: no body */
    return host_seen && !condition_seen && c->body_size == 0;
}

/* What an app route names in the module, with a message's u32 fields taken
 * from the query into c->fields. NULL, or the error to answer (404 for a
 * name the module lacks, 400 otherwise). */
static const char *resolve(const Host *host, Connection *c, char *query, Route *route, int *status) {
    const PureModule *m = host->module;
    *status = 404;
    c->field_bytes = 0;
    if (route->kind == ROUTE_NONE || (route->kind == ROUTE_OBJECT && !host->documents)) return "route";
    if (route->kind == ROUTE_OBJECT) return NULL;
    int found = route->kind == ROUTE_VIEW ? pure_module_view(m, c->key) : pure_module_input(m, c->key);
    if (found < 0) return route->kind == ROUTE_VIEW ? "no_view" : "no_input";
    *status = 400;
    route->target = (uint32_t)found;
    route->from = 0;
    route->count = SIZE_MAX;
    const PureInputShape *shape = route->kind == ROUTE_INPUT ? &m->input[found] : NULL;
    bool message = shape && shape->kind == PURE_KIND_MESSAGE;
    if (message && shape->keyed) return "keyed";
    if (shape) {
        const PureField *last = message && shape->field_count ? &m->field[shape->field_start + shape->field_count - 1] : NULL;
        if (c->body_size && shape->kind != PURE_KIND_TEXT && !(last && last->type == PURE_FIELD_BYTES)) return "body";
        c->field_bytes = message ? shape->prefix : 0;
    }
    /* name=value pairs: a table's from and count, a message's u32 fields. */
    bool given[PURE_MODULE_FIELDS] = {false};
    for (char *pair = query; pair && *pair;) {
        char *end = strchr(pair, '&'), *value = strchr(pair, '=');
        if (end) *end = 0;
        if (!value || (end && value > end)) return "query";
        *value++ = 0;
        uint64_t n;
        if (!number(value, route->kind == ROUTE_VIEW ? SIZE_MAX : UINT32_MAX, &n)) return "query";
        if (route->kind == ROUTE_VIEW) {
            bool from = !strcmp(pair, "from");
            if (m->view_type[found] != PURE_VIEW_TABLE || (!from && strcmp(pair, "count"))) return "query";
            if (from) route->from = (size_t)n;
            else route->count = (size_t)n;
        } else {
            uint32_t f = 0;
            while (f < (message ? shape->field_count : 0) && strcmp(m->field[shape->field_start + f].name, pair)) ++f;
            const PureField *field = message && f < shape->field_count ? &m->field[shape->field_start + f] : NULL;
            if (!field || field->type != PURE_FIELD_U32 || given[f]) return "query";
            given[f] = true;
            for (unsigned i = 0; i < 4; ++i) c->fields[field->offset + i] = (unsigned char)(n >> (8 * i));
        }
        pair = end ? end + 1 : NULL;
    }
    for (uint32_t f = 0; message && f < shape->field_count; ++f)
        if (m->field[shape->field_start + f].type == PURE_FIELD_U32 && !given[f]) return "missing_field";
    return NULL;
}

/* A whole head is in `in`: admit the request and take any body bytes that
 * came with it; later bytes stay for the next request. */
static void start_request(Connection *c, size_t head_size) {
    Host *host = c->host;
    char head[HEADER_LIMIT], *query = NULL;
    memcpy(head, c->in, head_size);
    size_t extra = c->in_used - head_size;
    memmove(c->in, c->in + head_size, extra);
    c->in_used = extra;
    if (!parse_head(c, head, head_size, &query)) { refuse(c, 400, "Bad Request", "request"); return; }
    if (c->body_size > PURE_OBJECT_LIMIT) { refuse(c, 413, "Content Too Large", "limit"); return; }
    ++host->requests;
    Route route = {.kind = c->route};
    int status;
    const char *why = resolve(host, c, query, &route, &status);
    if (why) { reject(c, status, status == 404 ? "Not Found" : "Bad Request", why); return; }
    /* A document's PUT and an input's event carry a payload: the message's fields, then the body. */
    bool write = route.kind == ROUTE_INPUT || c->put;
    PureResult admitted = pure_runtime_accept(&host->runtime, write ? PURE_INPUT_WRITE : PURE_INPUT_READ, c->key,
                                              route.kind == ROUTE_INPUT ? 0 : c->expected,
                                              write ? c->field_bytes + c->body_size : 0, &c->ticket);
    if (admitted != PURE_OK) {
        c->ticket = (PureTicket){0};
        /* An unread body would desynchronize the stream. */
        if (c->body_size) c->keep_alive = false;
        answer(c, (PureOutcome){.status = admitted});
        return;
    }
    host->by_slot[c->ticket.slot] = c;
    host->routes[c->ticket.slot] = route;
    if (!write) { c->phase = WAITING; return; }
    unsigned char *ingress = pure_runtime_ingress(c->ticket, NULL);
    memcpy(ingress, c->fields, c->field_bytes);
    c->body = ingress + c->field_bytes;
    size_t take = c->in_used < c->body_size ? c->in_used : c->body_size;
    if (take) memcpy(c->body, c->in, take);
    memmove(c->in, c->in + take, c->in_used - take);
    c->in_used -= take;
    c->received = take;
    c->phase = READING_BODY;
    if (c->received == c->body_size) {
        if (!pure_runtime_ready(c->ticket)) abort();
        c->phase = WAITING;
    }
}

static size_t head_end(const Connection *c) {
    for (size_t i = 3; i < c->in_used; ++i)
        if (!memcmp(c->in + i - 3, "\r\n\r\n", 4)) return i + 1;
    return 0;
}

/* Advance a connection as far as its socket allows without blocking. */
static void drive(Connection *c) {
    for (;;) {
        if (c->phase == READING_HEAD) {
            size_t end = head_end(c);
            if (end) { start_request(c, end); continue; }
            if (c->in_used == HEADER_LIMIT) { refuse(c, 431, "Request Header Fields Too Large", "headers"); continue; }
            if (!c->readable) return;
            ssize_t n = recv(c->fd, c->in + c->in_used, HEADER_LIMIT - c->in_used, 0);
            if (n > 0) { c->in_used += (size_t)n; c->deadline = now_ms() + c->host->timeout_ms; continue; }
            if (n < 0 && errno == EAGAIN) { c->readable = false; return; }
            if (n < 0 && errno == EINTR) continue;
            close_connection(c);
            return;
        }
        if (c->phase == READING_BODY) {
            if (!c->readable) return;
            ssize_t n = recv(c->fd, c->body + c->received, c->body_size - c->received, 0);
            if (n > 0) {
                c->received += (size_t)n;
                c->deadline = now_ms() + c->host->timeout_ms;
                if (c->received == c->body_size) {
                    if (!pure_runtime_ready(c->ticket)) abort();
                    c->phase = WAITING;
                }
                continue;
            }
            if (n < 0 && errno == EAGAIN) { c->readable = false; return; }
            if (n < 0 && errno == EINTR) continue;
            close_connection(c);
            return;
        }
        if (c->phase == WRITING) {
            if (!c->writable) return;
            size_t left = c->out_count - c->out_at;
            ssize_t n = writev(c->fd, c->out + c->out_at, (int)(left < IOV_MAX ? left : IOV_MAX));
            if (n < 0 && errno == EAGAIN) { c->writable = false; return; }
            if (n < 0 && errno == EINTR) continue;
            if (n < 0) { close_connection(c); return; }
            c->deadline = now_ms() + c->host->timeout_ms; /* a slow reader is fine while it reads */
            for (size_t sent = (size_t)n; c->out_at < c->out_count;) {
                struct iovec *v = &c->out[c->out_at];
                if (sent < v->iov_len) {
                    v->iov_base = (char *)v->iov_base + sent;
                    v->iov_len -= sent;
                    break;
                }
                sent -= v->iov_len;
                ++c->out_at;
            }
            if (c->out_at < c->out_count) continue;
            /* Answered: the next request may already be here. */
            finish_output(c);
            c->deadline = now_ms() + c->host->timeout_ms;
            if (!c->keep_alive) { close_connection(c); return; }
            c->phase = READING_HEAD;
            continue;
        }
        return; /* WAITING: the next burst answers */
    }
}

/* ---- Bursts ----------------------------------------------------------- */

static void release_copy(void *allocation, size_t size) {
    (void)size;
    free(allocation);
}

static void release_loan(void *allocation, size_t size) {
    (void)size;
    pure_mapping_release(allocation);
}

static bool allocate_ingress(void *context, size_t size, PureResource *out) {
    (void)context;
    void *bytes = malloc(size ? size : 1); /* a key press or an empty text has no payload */
    if (!bytes) return false;
    *out = (PureResource){.body = {bytes, size}, .allocation = bytes, .allocation_size = size, .release = release_copy};
    return true;
}

typedef struct { unsigned char *bytes; size_t size; const unsigned char *first; size_t spans; } Reply;

static bool measure_reply(void *context, PureSpan span) {
    Reply *r = context;
    if (!r->spans++) r->first = span.data;
    r->size += span.size;
    return true;
}

/* The reply's extents, if the store mapping holds every one. */
typedef struct { struct iovec *extents; size_t count; const PureMapping *mapping; bool held; } Gather;

static bool gather_extents(void *context, PureSpan span) {
    Gather *g = context;
    if (!span.size) return true;
    g->held = g->held && pure_mapping_holds(g->mapping, span);
    if (g->held) g->extents[g->count++] = (struct iovec){(void *)span.data, span.size};
    return g->held;
}

static bool copy_reply(void *context, PureSpan span) {
    Reply *r = context;
    if (span.size) memcpy(r->bytes + r->size, span.data, span.size);
    r->size += span.size;
    return true;
}

/* ---- App routes -------------------------------------------------------- */

typedef struct { unsigned char *bytes; size_t size, capacity; bool failed; } Output;

static bool put_bytes(Output *o, const void *bytes, size_t size) {
    if (o->failed) return false;
    if (size > o->capacity - o->size) {
        size_t capacity = o->capacity ? o->capacity : 256;
        while (capacity - o->size < size) {
            if (capacity > SIZE_MAX / 2) { o->failed = true; return false; }
            capacity *= 2;
        }
        unsigned char *grown = realloc(o->bytes, capacity);
        if (!grown) { o->failed = true; return false; }
        o->bytes = grown;
        o->capacity = capacity;
    }
    if (size) memcpy(o->bytes + o->size, bytes, size);
    o->size += size;
    return true;
}

static bool put_text(Output *o, const char *text) { return put_bytes(o, text, strlen(text)); }

static bool put_span(void *context, PureSpan span) { return put_bytes(context, span.data, span.size); }

/* A JSON string's contents: quotes, backslashes and control bytes escaped,
 * other bytes (UTF-8, which Pure keeps valid) as they are. */
static bool put_json_span(void *context, PureSpan span) {
    Output *o = context;
    const unsigned char *p = span.data;
    size_t plain = 0;
    for (size_t i = 0; i < span.size; ++i) {
        unsigned char b = p[i];
        if (b >= 0x20 && b != '"' && b != '\\') continue;
        char escape[8];
        int length = b == '"' || b == '\\' ? snprintf(escape, sizeof(escape), "\\%c", b) :
                                               snprintf(escape, sizeof(escape), "\\u%04x", b);
        put_bytes(o, p + plain, i - plain);
        put_bytes(o, escape, (size_t)length);
        plain = i + 1;
    }
    return put_bytes(o, p + plain, span.size - plain);
}

static void put_number(Output *o, double value) {
    char digits[32];
    put_bytes(o, digits, pure_format_number(value, digits));
}

/* A view's value: a number in Pure's shortest form or a text, as text; a
 * table as {"rows":N,"from":F,"items":[{column: value}...]} for the rows
 * asked, each cell computed only for them (a cell without a value is null). */
static PureResult show_view(PureApp *app, const Route *route, size_t available, PureResource *out, const char **type) {
    const PureModule *m = app->module;
    uint32_t view = route->target;
    Output o = {0};
    PureResult status = PURE_OK;
    double value;
    if (m->view_type[view] == PURE_VIEW_NUM) {
        *type = "text/plain; charset=utf-8";
        if (pure_app_view(app, view, &value)) put_number(&o, value);
        else status = PURE_NOT_FOUND;
    } else if (m->view_type[view] == PURE_VIEW_TEXT) {
        *type = "text/plain; charset=utf-8";
        status = pure_app_view_text(app, view, put_span, &o);
    } else {
        *type = "application/json";
        size_t rows = pure_app_rows(app, view), from = route->from < rows ? route->from : rows;
        size_t end = from + (route->count < rows - from ? route->count : rows - from);
        char head[80];
        snprintf(head, sizeof(head), "{\"rows\":%zu,\"from\":%zu,\"items\":[", rows, from);
        put_text(&o, head);
        for (size_t row = from; row < end && !o.failed; ++row) {
            put_text(&o, row > from ? ",{" : "{");
            bool first = true;
            for (uint32_t column = 0; column < m->views; ++column) {
                if (m->view_table[column] != view + 1) continue;
                put_text(&o, first ? "\"" : ",\"");
                put_text(&o, m->view_names[column]);
                put_text(&o, "\":");
                first = false;
                if (m->view_type[column] == PURE_VIEW_NUM) {
                    if (pure_app_cell(app, column, row, &value)) put_number(&o, value);
                    else put_text(&o, "null");
                    continue;
                }
                size_t mark = o.size;
                put_text(&o, "\"");
                if (pure_app_cell_text(app, column, row, put_json_span, &o) == PURE_OK) put_text(&o, "\"");
                else if (!o.failed) {
                    o.size = mark;
                    put_text(&o, "null");
                }
            }
            put_text(&o, "}");
        }
        put_text(&o, "]}");
    }
    if (status == PURE_OK && !o.bytes && !(o.bytes = malloc(1))) o.failed = true; /* an empty value has a buffer too */
    if (status == PURE_OK && o.failed) status = PURE_ALLOCATION;
    if (status == PURE_OK && o.size > available) status = PURE_BUDGET;
    if (status != PURE_OK) {
        free(o.bytes);
        return status;
    }
    *out = (PureResource){.body = {o.bytes, o.size}, .allocation = o.bytes, .allocation_size = o.size,
                          .release = release_copy};
    return PURE_OK;
}

/* An input's event: a text, a key pressed (down, then up), or a message
 * whose reply (a field of it) is borrowed from the request. */
static PureResult apply_input(PureApp *app, const Route *route, const PureEffect *effect, Applied *out) {
    const PureInputShape *shape = &app->module->input[route->target];
    PureResult status;
    if (shape->kind == PURE_KIND_TEXT) {
        status = pure_app_text(app, route->target, effect->value);
    } else if (shape->kind == PURE_KIND_KEY) {
        status = pure_app_input(app, route->target, PURE_EDGE_DOWN);
        if (status == PURE_OK) status = pure_app_input(app, route->target, PURE_EDGE_UP);
    } else {
        status = pure_app_message(app, route->target, NULL, -1, effect->value);
        Reply reply = {0};
        if (status == PURE_OK) pure_app_reply(app, measure_reply, &reply);
        out->reply = (PureSpan){reply.size ? reply.first : NULL, reply.size};
    }
    out->fault = app->fault;
    return status;
}

static void apply_effect(Host *host, const PureEffect *effect, Applied *out) {
    PureApp *app = &host->app;
    *out = (Applied){.ticket = effect->ticket, .input = effect->input};
    memcpy(out->key, effect->key, strlen(effect->key) + 1);
    Connection *c = host->by_slot[effect->ticket.slot];
    if (c && c->phase == WAITING && c->ticket.generation == effect->ticket.generation) out->connection = c;
    const Route *route = &host->routes[effect->ticket.slot];
    out->route = route->kind;
    if (route->kind == ROUTE_VIEW) {
        out->status = show_view(app, route, effect->available, &out->resource, &out->type);
        return;
    }
    if (route->kind == ROUTE_INPUT) {
        out->status = apply_input(app, route, effect, out);
        if (out->status == PURE_OK) ++host->burst_events;
        return;
    }
    bool read = effect->input == PURE_INPUT_READ;
    out->status = pure_app_message(app, read ? host->show : host->edit, effect->key,
                                   read ? -1 : effect->expected, read ? (PureSpan){0} : effect->value);
    out->revision = app->revision;
    if (out->status != PURE_OK) return;
    ++host->burst_events;
    Reply reply = {0};
    pure_app_reply(app, measure_reply, &reply);
    if (!read) {
        /* Checked at start: an edit replies a field of its request. */
        out->reply = (PureSpan){reply.size ? reply.first : NULL, reply.size};
        return;
    }
    if (reply.size > effect->available) { out->status = PURE_BUDGET; return; }
    if (!reply.size) return;
    /* A stored document is lent, extent by extent, not copied. */
    PureMapping *mapping = app->document ? pure_draft_mapping(app->document) : NULL;
    if (mapping) {
        Gather gather = {malloc(reply.spans * sizeof(struct iovec)), 0, mapping, true};
        if (gather.extents && pure_app_reply(app, gather_extents, &gather) == PURE_OK && gather.held) {
            out->extents = gather.extents;
            out->extent_count = gather.count;
            out->resource = (PureResource){.body = {NULL, reply.size}, .allocation = mapping,
                                           .allocation_size = reply.size, .release = release_loan};
            return;
        }
        free(gather.extents);
        pure_mapping_release(mapping);
    }
    unsigned char *copy = malloc(reply.size);
    if (!copy) { out->status = PURE_ALLOCATION; return; }
    reply = (Reply){.bytes = copy};
    pure_app_reply(app, copy_reply, &reply);
    out->resource = (PureResource){.body = {copy, reply.size}, .allocation = copy, .allocation_size = reply.size,
                                   .release = release_copy};
}

static void finish_burst(Host *host, size_t count) {
    if (!count) return;
    PureResult saved = pure_app_save(&host->app);
    ++host->bursts;
    for (size_t i = 0; i < count; ++i) {
        Applied *a = &host->applied[i];
        PureResult status = a->status == PURE_OK ? saved : a->status;
        if (!pure_runtime_complete(a->ticket, status, a->input == PURE_INPUT_READ ? &a->resource : NULL,
                                   status == PURE_OK ? a->reply : (PureSpan){0}, a->revision)) abort();
        Connection *c = a->connection;
        if (c && c->phase == WAITING && c->ticket.generation == a->ticket.generation) {
            PureOutcome result;
            if (!pure_runtime_result(a->ticket, &result)) abort();
            /* A lent answer's body is its extents (the runtime charged them). */
            if (result.status == PURE_OK && a->extents)
                respond_extents(c, 200, "OK", "none", result.revision, a->extents, a->extent_count, result.body.size);
            else if (a->route != ROUTE_OBJECT) answer_app(c, result, a->route == ROUTE_VIEW, a->fault, a->type);
            else answer(c, result);
            drive(c); /* most answers fit the socket buffer at once */
        }
        free(a->extents);
    }
}

/* Every request made ready since the last burst. A burst ends before a
 * document's key repeats or before its documents would outgrow one save, so
 * every request in it is stored by its own save or fails with it. Views and
 * events of the app's states join any burst. */
static void serve_bursts(Host *host) {
    size_t count = 0, keys = 0;
    PureEffect effect;
    /* One look at the store per burst: a request made after another shard's
     * answer was sent is read after that answer's commit. */
    bool pinned = false;
    while (pure_runtime_take(&host->runtime, &effect) == PURE_TAKE_EFFECT) {
        if (!pinned) pinned = pure_store_pin(host->store, true) == PURE_OK;
        bool document = host->routes[effect.ticket.slot].kind == ROUTE_OBJECT, repeated = false;
        for (size_t i = 0; i < count && document && !repeated; ++i)
            repeated = host->applied[i].route == ROUTE_OBJECT && !strcmp(host->applied[i].key, effect.key);
        if (repeated || (document && keys == PURE_APP_PENDING)) {
            finish_burst(host, count);
            count = keys = 0;
        }
        apply_effect(host, &effect, &host->applied[count++]);
        keys += document;
    }
    finish_burst(host, count);
    if (pinned) pure_store_pin(host->store, false);
}

/* A write's reply is borrowed from its request, so the handler of an input
 * served by PUT or POST may reply only a field of its message. */
static bool replies_borrow(const PureModule *m, uint32_t input) {
    for (uint32_t h = 0; h < m->handlers; ++h) {
        if (m->handler[h].event != 1 + 2 * input) continue;
        const PureBlock *code = &m->handler[h].code;
        for (uint32_t pc = code->start; pc < code->start + code->length; ++pc)
            if (m->code[pc].op == PURE_OP_REPLY && (pc == code->start || m->code[pc - 1].op != PURE_OP_FIELD))
                return false;
    }
    return true;
}

/* ---- Main ------------------------------------------------------------- */

static bool load_module(PureModule *m, const char *path, bool *documents, uint32_t *show_out, uint32_t *edit_out) {
    static unsigned char bytes[1 << 17];
    char error[128];
    FILE *file = fopen(path, "rb");
    size_t size = file ? fread(bytes, 1, sizeof(bytes), file) : 0;
    bool read_ok = file && !ferror(file) && size < sizeof(bytes);
    if (file) fclose(file);
    if (!read_ok || pure_module_load(bytes, size, m, error, sizeof(error)) != PURE_OK) {
        fprintf(stderr, "Module: %s\n", read_ok ? error : "cannot read");
        return false;
    }
    /* Documents are served when the module has show and edit, as keyed messages. */
    int show = pure_module_input(m, "show"), edit = pure_module_input(m, "edit");
    *documents = show >= 0 || edit >= 0;
    if (*documents && (show < 0 || edit < 0 || m->input[show].kind != PURE_KIND_MESSAGE || !m->input[show].keyed ||
                       m->input[edit].kind != PURE_KIND_MESSAGE || !m->input[edit].keyed)) {
        fputs("Module: documents need keyed messages named show and edit\n", stderr);
        return false;
    }
    for (uint32_t i = 0; i < m->inputs; ++i) {
        bool served = (*documents && i == (uint32_t)edit) || (m->input[i].kind == PURE_KIND_MESSAGE && !m->input[i].keyed);
        if (served && !replies_borrow(m, i)) {
            fprintf(stderr, "Module: %s may reply only a field of its message\n", m->input_names[i]);
            return false;
        }
    }
    *show_out = (uint32_t)(show >= 0 ? show : 0);
    *edit_out = (uint32_t)(edit >= 0 ? edit : 0);
    return true;
}

static void accept_all(Host *host) {
    for (;;) {
        int fd = accept4(host->listener, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) return;
        if (host->active == host->capacity) { close(fd); continue; } /* full: the peer sees the close */
        Connection *c = host->spare;
        if (c) host->spare = c->next;
        else if (!(c = malloc(sizeof(*c)))) { close(fd); continue; }
        size_t index = 0;
        while (host->table[index]) ++index; /* a free index exists: active < capacity */
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        c->host = host;
        c->index = index;
        c->fd = fd;
        c->phase = READING_HEAD;
        c->readable = c->writable = c->keep_alive = true;
        c->deadline = now_ms() + host->timeout_ms;
        c->ticket = (PureTicket){0};
        c->out = NULL;
        c->out_count = c->out_at = 0;
        c->body = NULL;
        c->in_used = 0;
        struct epoll_event event = {.events = EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLET, .data.ptr = c};
        if (epoll_ctl(host->epoll, EPOLL_CTL_ADD, fd, &event)) {
            close(fd);
            c->next = host->spare;
            host->spare = c;
            continue;
        }
        host->table[index] = c;
        ++host->active;
        ++host->accepted;
        drive(c);
    }
}

/* One shard: its own listener on the shared port, epoll set, runtime domain,
 * app and store handle. */
static bool start_shard(Host *host, const PureModule *module, bool documents, uint32_t show, uint32_t edit,
                        const char *store, size_t budget, unsigned *port) {
    host->module = module;
    host->documents = documents;
    host->show = show;
    host->edit = edit;
    if (pure_store_open(store, &host->store) != PURE_OK) { fputs("Cannot open the store\n", stderr); return false; }
    if (pure_app_start(&host->app, module, pure_store_data(host->store)) != PURE_OK || !pure_app_defer(&host->app, true)) {
        fputs("Cannot start the module on the store\n", stderr);
        return false;
    }
    host->work = calloc(host->capacity, sizeof(*host->work));
    host->table = calloc(host->capacity, sizeof(*host->table));
    host->by_slot = calloc(host->capacity, sizeof(*host->by_slot));
    host->applied = calloc(host->capacity, sizeof(*host->applied));
    host->routes = calloc(host->capacity, sizeof(*host->routes));
    if (!host->work || !host->table || !host->by_slot || !host->applied || !host->routes) {
        fputs("Cannot allocate connections\n", stderr);
        return false;
    }
    if (!pure_runtime_init(&host->runtime, host->work, host->capacity, &host->app,
                           (PureHost){.allocate = allocate_ingress}, budget) ||
        !pure_runtime_enable_async(&host->runtime)) return false;
    host->listener = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    int one = 1;
    setsockopt(host->listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    setsockopt(host->listener, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons((uint16_t)*port),
                                  .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    socklen_t address_size = sizeof(address);
    if (host->listener < 0 || bind(host->listener, (struct sockaddr *)&address, sizeof(address)) ||
        listen(host->listener, 4096) || getsockname(host->listener, (struct sockaddr *)&address, &address_size)) {
        perror("listen");
        return false;
    }
    *port = ntohs(address.sin_port); /* the first shard picks it, the others join it */
    host->epoll = epoll_create1(EPOLL_CLOEXEC);
    struct epoll_event listen_event = {.events = EPOLLIN, .data.ptr = NULL};
    if (host->epoll < 0 || epoll_ctl(host->epoll, EPOLL_CTL_ADD, host->listener, &listen_event)) {
        perror("epoll");
        return false;
    }
    return true;
}

static void *run_shard(void *context) {
    Host *host = context;
    struct epoll_event events[EVENTS];
    uint64_t next_sweep = now_ms() + 1000;
    while (!atomic_load(&stopping)) {
        int ready = epoll_wait(host->epoll, events, EVENTS, 1000);
        for (int e = 0; e < ready; ++e) {
            Connection *c = events[e].data.ptr;
            if (!c) { accept_all(host); continue; }
            if (c->phase == CLOSED) continue;
            if (events[e].events & EPOLLERR) { close_connection(c); continue; }
            if (events[e].events & (EPOLLIN | EPOLLRDHUP | EPOLLHUP)) c->readable = true;
            if (events[e].events & EPOLLOUT) c->writable = true;
            drive(c);
        }
        serve_bursts(host);
        uint64_t now = now_ms();
        if (now >= next_sweep) {
            next_sweep = now + 1000;
            for (size_t i = 0; i < host->capacity; ++i) {
                Connection *c = host->table[i];
                if (c && c->phase != WAITING && now >= c->deadline) { ++host->expired; close_connection(c); }
            }
        }
        /* The round is over: closed connections can be reused. */
        while (host->closed) {
            Connection *c = host->closed;
            host->closed = c->next;
            c->next = host->spare;
            host->spare = c;
        }
    }
    pure_runtime_stop(&host->runtime);
    serve_bursts(host);
    for (size_t i = 0; i < host->capacity; ++i) if (host->table[i]) close_connection(host->table[i]);
    return NULL;
}

static void stop_shard(Host *host) {
    pure_app_stop(&host->app);
    pure_store_close(host->store);
    if (host->listener >= 0) close(host->listener);
    if (host->epoll >= 0) close(host->epoll);
    while (host->closed) {
        Connection *c = host->closed;
        host->closed = c->next;
        c->next = host->spare;
        host->spare = c;
    }
    for (Connection *c = host->spare, *next; c; c = next) { next = c->next; free(c); }
    free(host->work);
    free(host->table);
    free(host->by_slot);
    free(host->applied);
    free(host->routes);
}

int main(int argc, char **argv) {
    static PureModule module;
    const char *module_path = NULL, *store = NULL;
    unsigned port = 8767, threads = 1;
    size_t budget = 64u << 20, capacity = 1024;
    uint64_t timeout_ms = 10000;
    for (int i = 1; i < argc; i += 2) {
        uint64_t value;
        if (i + 1 == argc) goto usage;
        if (!strcmp(argv[i], "--module")) module_path = argv[i + 1];
        else if (!strcmp(argv[i], "--store")) store = argv[i + 1];
        else if (!strcmp(argv[i], "--port") && number(argv[i + 1], 65535, &value)) port = (unsigned)value;
        else if (!strcmp(argv[i], "--budget") && number(argv[i + 1], SIZE_MAX, &value) && value) budget = (size_t)value;
        else if (!strcmp(argv[i], "--connections") && number(argv[i + 1], 1u << 20, &value) && value) capacity = (size_t)value;
        else if (!strcmp(argv[i], "--threads") && number(argv[i + 1], 256, &value) && value) threads = (unsigned)value;
        else if (!strcmp(argv[i], "--timeout-ms") && number(argv[i + 1], 3600000, &value) && value) timeout_ms = value;
        else goto usage;
    }
    uint32_t show, edit;
    bool documents;
    if (!module_path || !store || !load_module(&module, module_path, &documents, &show, &edit)) goto usage;
    if (threads > 1 && module.states) {
        fputs("Module: its states have one owner, so it runs with --threads 1\n", stderr);
        return 2;
    }
    if (pure_store_init(store) != PURE_OK) { fputs("Cannot initialize the store directory\n", stderr); return 1; }
    signal(SIGPIPE, SIG_IGN);
    struct sigaction stop_action = {.sa_handler = stop};
    sigemptyset(&stop_action.sa_mask);
    sigaction(SIGINT, &stop_action, NULL);
    sigaction(SIGTERM, &stop_action, NULL);
    Host *hosts = calloc(threads, sizeof(*hosts));
    pthread_t *thread = calloc(threads, sizeof(*thread));
    if (!hosts || !thread) return 1;
    for (unsigned i = 0; i < threads; ++i) {
        hosts[i].capacity = capacity;
        hosts[i].timeout_ms = timeout_ms;
        hosts[i].listener = hosts[i].epoll = -1;
        if (!start_shard(&hosts[i], &module, documents, show, edit, store, budget, &port)) return 1;
    }
    printf("{\"port\":%u,\"threads\":%u}\n", port, threads);
    fflush(stdout);
    for (unsigned i = 1; i < threads; ++i)
        if (pthread_create(&thread[i], NULL, run_shard, &hosts[i])) { fputs("Cannot start a thread\n", stderr); return 1; }
    run_shard(&hosts[0]);
    for (unsigned i = 1; i < threads; ++i) pthread_join(thread[i], NULL);
    size_t active = 0, retained = 0;
    uint64_t accepted = 0, requests = 0, bursts = 0, events = 0, expired = 0;
    for (unsigned i = 0; i < threads; ++i) {
        active += hosts[i].active;
        retained += hosts[i].runtime.retained;
        accepted += hosts[i].accepted;
        requests += hosts[i].requests;
        bursts += hosts[i].bursts;
        events += hosts[i].burst_events;
        expired += hosts[i].expired;
        stop_shard(&hosts[i]);
    }
    unsigned long groups, grouped;
    pure_store_group_counts(&groups, &grouped);
    fprintf(stderr, "{\"active\":%zu,\"retained_payload_bytes\":%zu,\"accepted\":%" PRIu64 ",\"requests\":%" PRIu64
            ",\"bursts\":%" PRIu64 ",\"burst_events\":%" PRIu64 ",\"expired\":%" PRIu64 ",\"threads\":%u"
            ",\"commit_groups\":%lu,\"grouped_commits\":%lu}\n",
            active, retained, accepted, requests, bursts, events, expired, threads, groups, grouped);
    free(hosts);
    free(thread);
    return 0;
usage:
    fputs("pure-serve --module FILE.pmod --store DIRECTORY [--port 8767] [--threads 1] [--budget BYTES] "
          "[--connections 1024] [--timeout-ms 10000]\n", stderr);
    return 2;
}
