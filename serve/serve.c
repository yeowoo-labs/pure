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
 * message. HTTP/1.1 connections persist and may pipeline; an epoll thread
 * accepts, reads and writes without blocking. Requests go through the
 * runtime core (a work slot per connection, the payload budget, ingress
 * owned by the core, write replies borrowed from the request). After each
 * round of events every ready request is applied to the module with
 * deferred saving, one save stores the burst, and only then are its
 * requests answered: an answer is durable, and a failed save fails the
 * whole burst, reads included.
 *
 * With --threads N each thread has its own listener on the same port
 * (SO_REUSEPORT), epoll set and runtime domain; the kernel spreads
 * connections over them. A module of keyed documents alone runs one app per
 * thread: they share nothing but the store, whose commits take turns and
 * whose revisions settle concurrent edits of a key. States have one owner:
 * for a module with them, one more thread owns the app, and the connection
 * threads hand it their ready requests and answer what it hands back. Its
 * bursts take the requests of every thread, one save each. */
#include "deliver.h"
#include "seal.h"
#include "message-json.h"
#include "module-store.h"
#include "module-text.h"
#include "runtime.h"
#include "store.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <poll.h>
#include <sched.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

enum { HEADER_LIMIT = 4096, EVENTS = 256, BATCH = 256 };
typedef enum { CLOSED, READING_HEAD, READING_BODY, WAITING, WRITING } Phase;
typedef struct Host Host;

/* What a request names: a document, a view (a table's rows from, count) or
 * an input. Kept per work slot, so an accepted event runs even if its
 * connection is gone before the burst. */
typedef enum { ROUTE_NONE, ROUTE_OBJECT, ROUTE_VIEW, ROUTE_INPUT } RouteKind;
typedef struct { RouteKind kind; uint32_t target; size_t from, count; } Route;

/* This node's key, for deliveries sealed to it (--key), and whether it
 * takes only those (--sealed-only). */
static PureSealKey *seal_key;
static bool sealed_only;

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
    /* An app input's message: its fixed fields from the query, which fields
     * were given there, and whether the body is a JSON object of the rest.
     * Its ingress is [u32le payload size][payload]; with JSON it is [u32le
     * payload size | PLACED][u32le start and size of each field][payload: the
     * prefix, then the JSON as it came], its strings read where they are. */
    unsigned char fields[8 * PURE_MODULE_FIELDS];
    size_t field_bytes;
    bool given[PURE_MODULE_FIELDS], json;
    uint32_t input;
    unsigned char *ingress;
    char source[PURE_SOURCE + 1]; /* a delivered message's sender (X-Pure-Source), or empty */
    uint64_t sequence;            /* and its number (X-Pure-Sequence) */
    unsigned batch;               /* how many numbered messages the body holds (X-Pure-Count) */
    /* A sealed delivery: the suite it names (X-Pure-Seal), what the seal is
     * bound to (source, number, count), and how many plain bytes the body
     * holds once opened, cleared when its ticket goes. */
    char seal[40], aad[PURE_SOURCE + 48];
    size_t opened;
    int64_t expected;
    size_t body_size, received, in_used;
    unsigned char *body;
    PureTicket ticket;
    struct iovec *out, small[4]; /* the answer: its head, then its body's extents */
    size_t out_count, out_at;
    char head[320];
    char in[HEADER_LIMIT]; /* the header being read, then pipelined bytes */
} Connection;

/* One request applied in a burst, for the connection thread that answers
 * it. A read answered from the store lends its extents: they point into a
 * store mapping, which the resource holds and the runtime charges and
 * releases with the ticket. */
typedef struct {
    Host *host;
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

/* The app and its store. A connection thread runs it itself (an app of
 * documents only), or one owner thread runs it for all connection threads.
 * The owner publishes a snapshot of the states after each save, and the
 * connection threads show views from it themselves: reads never wait for
 * the owner, which may be applying events or waiting for a sync. */
typedef struct {
    const PureModule *module;
    PureStore *store;
    PureApp app;
    uint32_t show, edit;
    uint64_t bursts, burst_events;
    Applied *applied; /* a round's requests, grown to fit */
    size_t applied_capacity;
    bool wrote;       /* the last round saved an event or an edit */
    bool reads_first; /* the owner's: a round's reads before its writes */
    double next[PURE_MODULE_INPUTS]; /* when each timer ticks next (wall clock, ms) */
    uint64_t ticks;
    /* The owner's: the last save's snapshot (NULL: the owner shows views),
     * taken under the lock, and the app's changes it shows. */
    bool publishes;
    pthread_mutex_t published_lock;
    PureSnapshot *published;
    uint64_t published_changes, publications;
    /* The owner's: outputs wired to HTTP receivers, and how far each was trimmed. */
    PureWire *wires[PURE_MODULE_INPUTS];
    unsigned wire_count;
    uint64_t trimmed[PURE_MODULE_INPUTS], answers;
    bool held[PURE_MODULE_INPUTS]; /* an answer failed to apply: the wire's sends stay until a restart */
} Executor;

/* A request made ready: its effect, what it names, and the thread that answers it. */
typedef struct { Host *host; PureEffect effect; Route route; } Job;

/* The owner of an app with states served by several connection threads:
 * their requests wait here, and it runs them in bursts, one save each
 * however many threads they came from, handing each thread back its
 * applied requests to answer. */
typedef struct {
    Executor executor;
    pthread_mutex_t lock;
    pthread_cond_t waiting;
    Job *jobs;
    size_t count, capacity;
    uint64_t arrivals; /* requests handed to the owner, ever */
    bool stopping;
    bool delivered; /* a wire delivered a send: trim it */
    Host *hosts;
    unsigned host_count;
} Owner;

struct Host {
    const PureModule *module;
    bool documents; /* the module serves /objects: it has show and edit */
    Executor local; /* the app, when this thread runs it */
    Owner *owner;   /* or the owner that runs it */
    PureApp reader; /* shows the owner's snapshot here */
    Applied *views; /* a round's views shown here, answered together */
    size_t views_capacity;
    uint64_t shown; /* views shown from snapshots */
    Route *routes;  /* by work slot */
    PureRuntime runtime;
    PureWork *work;
    Connection **table;   /* open connections by index */
    Connection **by_slot; /* the connection waiting on a work slot */
    Connection *spare, *closed;
    Job *jobs; /* a round's requests: at most one per work slot */
    size_t capacity, active;
    int epoll, listener;
    uint64_t timeout_ms;
    uint64_t accepted, requests, expired;
    /* What the owner hands back to answer (swapped for answering), and the
     * eventfd that says so; pending counts requests at the owner. */
    pthread_mutex_t lock;
    Applied *done, *answering;
    size_t done_count, done_capacity, answering_capacity, pending;
    int wake;
};

/* Grow an array to hold `need` items; false when memory runs out. */
static bool grow(void *array, size_t *capacity, size_t need, size_t size) {
    if (need <= *capacity) return true;
    size_t next = *capacity ? *capacity : 64;
    while (next < need) next *= 2;
    void *grown = realloc(*(void **)array, next * size);
    if (!grown) return false;
    *(void **)array = grown;
    *capacity = next;
    return true;
}

static atomic_bool stopping; /* lock-free, so a signal handler may set it */
static void stop(int signal_number) { (void)signal_number; atomic_store(&stopping, true); }

static uint64_t now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}

/* The time events see: the wall clock, in milliseconds since 1970 (UTC). */
static double wall_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    return floor((double)t.tv_sec * 1000 + (double)t.tv_nsec / 1e6);
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
    if (c->opened && c->body) explicit_bzero(c->body, c->opened); /* an opened seal's plain text is gone with it */
    c->opened = 0;
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
    /* A delivery's answer tells how many numbered messages one request may carry. */
    char batch[32] = "";
    if (*c->source) snprintf(batch, sizeof(batch), "X-Pure-Batch: %d\r\n", BATCH);
    int length = snprintf(c->head, sizeof(c->head),
        "HTTP/1.1 %d %s\r\nContent-Length: %zu\r\n%sContent-Type: %s\r\n"
        "Cache-Control: no-store\r\nX-Pure-Error: %s\r\n%s%s\r\n",
        status, reason, size, c->keep_alive ? "" : "Connection: close\r\n", type, error, tag, batch);
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
    c->source[0] = 0;
    c->sequence = 0;
    c->batch = 1;
    c->seal[0] = 0;
    bool counted = false;
    c->body_size = 0;
    c->json = false;
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
        } else if (!strcasecmp(line, "Content-Type")) {
            c->json = !strncasecmp(value, "application/json", 16);
        } else if (!strcasecmp(line, "Connection")) {
            if (!strcasecmp(value, "close")) c->keep_alive = false;
            else if (strcasecmp(value, "keep-alive")) return false;
        } else if (!strcasecmp(line, "X-Pure-Source")) {
            /* A message delivered from another app's output: applied once per number. */
            size_t length = strlen(value);
            if (*c->source || !length || length > PURE_SOURCE) return false;
            for (size_t i = 0; i < length; ++i) if (value[i] <= ' ' || value[i] > '~') return false;
            memcpy(c->source, value, length + 1);
        } else if (!strcasecmp(line, "X-Pure-Sequence")) {
            if (c->sequence || !number(value, UINT64_MAX, &n) || !n) return false;
            c->sequence = n;
        } else if (!strcasecmp(line, "X-Pure-Seal")) {
            /* A delivery sealed to this node's key, in the suite named. */
            if (*c->seal || strlen(value) >= sizeof(c->seal)) return false;
            strcpy(c->seal, value);
        } else if (!strcasecmp(line, "X-Pure-Count")) {
            /* Numbered messages from X-Pure-Sequence on, as a JSON array. */
            if (counted || !number(value, BATCH, &n) || !n) return false;
            c->batch = (unsigned)n;
            counted = true;
        } else if (!strcasecmp(line, "Transfer-Encoding") || !strcasecmp(line, "Expect")) return false;
        line = next + 2;
    }
    if (!*c->source != !c->sequence || (*c->source && c->route != ROUTE_INPUT) || (counted && !*c->source) ||
        c->sequence > UINT64_MAX - c->batch) return false;
    if (c->put) return host_seen && length_seen && condition_seen;
    if (c->post) return host_seen && !condition_seen; /* no Content-Length: no body */
    return host_seen && !condition_seen && c->body_size == 0;
}

static void put_u32(unsigned char *p, uint32_t v) { for (unsigned i = 0; i < 4; ++i) p[i] = (unsigned char)(v >> (8 * i)); }
static void put_u64(unsigned char *p, uint64_t v) { for (unsigned i = 0; i < 8; ++i) p[i] = (unsigned char)(v >> (8 * i)); }
static uint32_t get_u32(const unsigned char *p) { return p[0] | p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t get_u64(const unsigned char *p) { return get_u32(p) | (uint64_t)get_u32(p + 4) << 32; }

/* What an app route names in the module, with a message's fixed fields
 * taken from the query into c->fields. NULL, or the error to answer (404
 * for a name the module lacks, 400 otherwise). */
static const char *resolve(const Host *host, Connection *c, char *query, Route *route, int *status) {
    const PureModule *m = host->module;
    *status = 404;
    c->field_bytes = 0;
    memset(c->given, 0, sizeof(c->given));
    if (route->kind == ROUTE_NONE || (route->kind == ROUTE_OBJECT && !host->documents)) return "route";
    if (route->kind == ROUTE_OBJECT) return NULL;
    int found = route->kind == ROUTE_VIEW ? pure_module_view(m, c->key) : pure_module_input(m, c->key);
    /* An output is the app's to send and a timer ticks by itself: neither is posted. */
    if (found >= 0 && route->kind == ROUTE_INPUT &&
        (m->input[found].kind == PURE_KIND_OUTPUT || m->input[found].kind == PURE_KIND_TIMER)) found = -1;
    if (found < 0) return route->kind == ROUTE_VIEW ? "no_view" : "no_input";
    *status = 400;
    route->target = (uint32_t)found;
    route->from = 0;
    route->count = SIZE_MAX;
    const PureInputShape *shape = route->kind == ROUTE_INPUT ? &m->input[found] : NULL;
    bool message = shape && shape->kind == PURE_KIND_MESSAGE;
    if (message && shape->keyed) return "keyed";
    if (*c->source && !message) return "delivery";
    if (!host->owner) c->source[0] = 0; /* an app without states or outputs applies it as a message */
    if (shape) {
        /* A body is a text input's text, a message's one variable field, or a
         * JSON object of a message's fields (two variable fields need one). */
        unsigned variables = 0;
        for (uint32_t f = 0; message && f < shape->field_count; ++f)
            variables += m->field[shape->field_start + f].type == PURE_FIELD_BYTES ||
                         m->field[shape->field_start + f].type == PURE_FIELD_TEXT;
        if (c->json && !message) return "json";
        if (!c->json && (variables > 1 || (c->body_size && shape->kind == PURE_KIND_KEY) ||
                         (c->body_size && message && !variables))) return variables > 1 ? "json" : "body";
        c->input = (uint32_t)found;
        c->field_bytes = message ? shape->prefix : 0;
    }
    /* name=value pairs: a table's from and count, a message's fixed fields. */
    for (char *pair = query; pair && *pair;) {
        char *end = strchr(pair, '&'), *value = strchr(pair, '=');
        if (end) *end = 0;
        if (!value || (end && value > end)) return "query";
        *value++ = 0;
        if (route->kind == ROUTE_VIEW) {
            uint64_t n;
            bool from = !strcmp(pair, "from");
            if (!number(value, SIZE_MAX, &n) || m->view_type[found] != PURE_VIEW_TABLE || (!from && strcmp(pair, "count")))
                return "query";
            if (from) route->from = (size_t)n;
            else route->count = (size_t)n;
        } else {
            uint32_t f = 0;
            while (f < (message ? shape->field_count : 0) && strcmp(m->field[shape->field_start + f].name, pair)) ++f;
            const PureField *field = message && f < shape->field_count ? &m->field[shape->field_start + f] : NULL;
            if (!field || c->given[f] || (field->type != PURE_FIELD_U32 && field->type != PURE_FIELD_NUMBER) ||
                !pure_json_fixed(field, (const unsigned char *)value, strlen(value), c->fields)) return "query";
            c->given[f] = true;
        }
        pair = end ? end + 1 : NULL;
    }
    for (uint32_t f = 0; message && !c->json && f < shape->field_count; ++f) {
        uint8_t type = m->field[shape->field_start + f].type;
        if ((type == PURE_FIELD_U32 || type == PURE_FIELD_NUMBER) && !c->given[f]) return "missing_field";
    }
    for (uint32_t f = 0; c->batch > 1 && f < shape->field_count; ++f)
        if (c->given[f]) return "delivery"; /* a batch's fields are all in its objects */
    if (c->batch > 1 && !c->json) return "delivery";
    if (!*c->source) c->batch = 1;
    return NULL;
}

/* Parsing a JSON body in place: the payload it is in, the placed fields'
 * table, and which fields it gave. */
#define PLACED (UINT32_C(1) << 31)
/* After the size: a delivered message's number (u64le), how many follow it
 * (u32le: a batch) and its source (u8 length and the bytes), in DELIVERY
 * bytes before the tables; then a table for each message, a prefix for each
 * and the JSON. */
#define DELIVERED (UINT32_C(1) << 30)
enum { DELIVERY = 80 };
/* An app input's JSON body becomes its message: an object with a member for
 * each field the query did not give (a u32, a number, or a string for bytes
 * and text), groups as objects inside it, and nothing else. Fixed fields go
 * into the prefix; strings stay in the body, and the table says where.
 * NULL, or the error to answer. */
static const char *convert(Connection *c) {
    const PureInputShape *shape = &c->host->module->input[c->input];
    uint32_t delivered = get_u32(c->ingress) & DELIVERED;
    size_t table = 8 * (size_t)shape->field_count;
    unsigned char *tables = c->ingress + 4 + (delivered ? DELIVERY : 0), *prefixes = tables + c->batch * table;
    unsigned char *json = c->body;
    size_t size = c->body_size, at = pure_json_space(json, 0, size);
    /* A batch is an array of its messages' objects. */
    if (c->batch > 1) {
        if (at == size || json[at++] != '[') return "json";
        at = pure_json_space(json, at, size);
    }
    for (unsigned i = 0; i < c->batch; ++i) {
        if (i) {
            if (at == size || json[at++] != ',') return "json";
            at = pure_json_space(json, at, size);
        }
        PureJsonMessage b = {.module = c->host->module, .shape = shape, .table = tables + i * table,
                             .payload = prefixes + i * c->field_bytes, .given = c->given};
        const char *why = pure_json_message(&b, json, size, &at);
        if (why) return why;
        at = pure_json_space(json, at, size);
        for (uint32_t f = 0; f < shape->field_count; ++f)
            if (!b.seen[f] && !c->given[f]) return "missing_field";
    }
    if (c->batch > 1 && (at == size || json[at++] != ']')) return "json";
    if (pure_json_space(json, at, size) != size) return "json";
    put_u32(c->ingress, (uint32_t)(c->batch * c->field_bytes + c->body_size) | PLACED | delivered);
    return NULL;
}

/* A sealed body opened where it lies, or false (another key, another
 * delivery's binding, or changed on the way). */
static bool open_seal(Connection *c) {
    if (c->body_size < PURE_SEAL_OVERHEAD) return false;
    size_t size = c->body_size - PURE_SEAL_OVERHEAD;
    unsigned char *plain = malloc(size ? size : 1);
    bool opened = plain && pure_unseal(seal_key, c->seal, c->aad, c->body, c->body_size, plain);
    if (opened) {
        memcpy(c->body, plain, size);
        c->body_size = size;
        c->opened = size;
    }
    if (plain) explicit_bzero(plain, size);
    free(plain);
    return opened;
}

/* A request's body is in: an app input's JSON becomes its payload, and the
 * request is ready, or it is answered 400 and never runs. */
static void body_done(Connection *c) {
    if (*c->seal && !open_seal(c)) {
        release_ticket(c); /* never made ready: it never runs */
        respond(c, 400, "Bad Request", "seal", 0, (PureSpan){0});
        return;
    }
    const char *why = c->route == ROUTE_INPUT && c->json ? convert(c) : NULL;
    if (why) {
        release_ticket(c); /* never made ready: it never runs */
        respond(c, 400, "Bad Request", why, 0, (PureSpan){0});
        return;
    }
    if (!pure_runtime_ready(c->ticket)) abort();
    c->phase = WAITING;
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
    /* A node that keeps its deliveries sealed refuses the others; a seal
     * needs this node's key, and binds the delivery's source, number and count. */
    if (*c->source && sealed_only && !*c->seal) { refuse(c, 403, "Forbidden", "unsealed"); return; }
    if (*c->seal) {
        if (!*c->source || !seal_key || !c->json) { refuse(c, 400, "Bad Request", "seal"); return; }
        snprintf(c->aad, sizeof(c->aad), "%s %llu %u", c->source, (unsigned long long)c->sequence, c->batch);
    }
    if (c->body_size > PURE_OBJECT_LIMIT) { refuse(c, 413, "Content Too Large", "limit"); return; }
    ++host->requests;
    Route route = {.kind = c->route};
    int status;
    const char *why = resolve(host, c, query, &route, &status);
    if (why) { reject(c, status, status == 404 ? "Not Found" : "Bad Request", why); return; }
    /* A document's PUT carries its body; an input's event its payload's size,
     * the payload (the fixed fields, then the body), and room for its JSON. */
    bool write = route.kind == ROUTE_INPUT || c->put;
    size_t table = route.kind == ROUTE_INPUT && c->json ? 8 * (size_t)host->module->input[c->input].field_count : 0;
    size_t lead = 4 + (route.kind == ROUTE_INPUT && *c->source ? DELIVERY : 0);
    /* A batch has a table and a prefix for each of its messages. */
    size_t input_size = route.kind == ROUTE_INPUT ? lead + c->batch * (table + c->field_bytes) + c->body_size :
                        write ? c->body_size : 0;
    PureResult admitted = pure_runtime_accept(&host->runtime, write ? PURE_INPUT_WRITE : PURE_INPUT_READ, c->key,
                                              route.kind == ROUTE_INPUT ? 0 : c->expected, input_size, &c->ticket);
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
    unsigned char *ingress = c->ingress = pure_runtime_ingress(c->ticket, NULL); /* NULL for no bytes */
    c->body = ingress;
    if (route.kind == ROUTE_INPUT) {
        put_u32(ingress, (uint32_t)(c->batch * c->field_bytes + c->body_size) | (lead > 4 ? DELIVERED : 0));
        if (lead > 4) {
            size_t length = strlen(c->source);
            put_u64(ingress + 4, c->sequence);
            put_u32(ingress + 12, c->batch);
            ingress[16] = (unsigned char)length;
            memcpy(ingress + 17, c->source, length);
        }
        if (c->field_bytes) memcpy(ingress + lead + c->batch * table, c->fields, c->field_bytes);
        c->body = ingress + lead + c->batch * (table + c->field_bytes);
    }
    size_t take = c->in_used < c->body_size ? c->in_used : c->body_size;
    if (take) memcpy(c->body, c->in, take);
    memmove(c->in, c->in + take, c->in_used - take);
    c->in_used -= take;
    c->received = take;
    c->phase = READING_BODY;
    if (c->received == c->body_size) body_done(c);
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
                if (c->received == c->body_size) body_done(c);
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

/* A value's extents, if the store mapping holds every one. */
typedef struct { struct iovec *extents; size_t count, capacity; const PureMapping *mapping; bool held; } Gather;

static bool gather_extents(void *context, PureSpan span) {
    Gather *g = context;
    if (!span.size) return true;
    g->held = g->held && pure_mapping_holds(g->mapping, span) &&
              grow(&g->extents, &g->capacity, g->count + 1, sizeof(*g->extents));
    if (g->held) g->extents[g->count++] = (struct iovec){(void *)span.data, span.size};
    return g->held;
}

/* A gathered value goes out as the mapping's extents, not copied: the answer
 * holds the mapping (charged to the budget) until it is sent and its ticket
 * released. false (and nothing held) when some extent was not stored. */
static bool lend(Applied *out, PureMapping *mapping, Gather *gather, bool read, size_t size) {
    if (!read || !gather->held) {
        free(gather->extents);
        pure_mapping_release(mapping);
        return false;
    }
    out->extents = gather->extents;
    out->extent_count = gather->count;
    out->resource = (PureResource){.body = {NULL, size}, .allocation = mapping, .allocation_size = size,
                                   .release = release_loan};
    return true;
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
static PureResult show_view(PureApp *app, const Route *route, size_t available, Applied *answer) {
    const PureModule *m = app->module;
    uint32_t view = route->target;
    PureResource *out = &answer->resource;
    const char **type = &answer->type;
    /* Room for a page of short rows at once. */
    Output o = {0};
    PureResult status = PURE_OK;
    double value;
    if (m->view_type[view] == PURE_VIEW_TABLE && !(o.bytes = malloc(o.capacity = 1024))) return PURE_ALLOCATION;
    if (m->view_type[view] == PURE_VIEW_NUM) {
        *type = "text/plain; charset=utf-8";
        if (pure_app_view(app, view, &value)) put_number(&o, value);
        else status = PURE_NOT_FOUND;
    } else if (m->view_type[view] == PURE_VIEW_TEXT) {
        *type = "text/plain; charset=utf-8";
        /* A text state shown as it is goes out as its stored extents. */
        int state = pure_module_text_state(m, view);
        PureDraft *text = state >= 0 ? app->texts[state] : NULL;
        PureVersion version = text ? pure_draft_version(text) : (PureVersion){0, 0};
        if (version.bytes > available) return PURE_BUDGET;
        PureMapping *mapping = version.bytes ? pure_draft_mapping(text) : NULL;
        if (mapping) {
            Gather gather = {NULL, 0, 0, mapping, true};
            bool read = pure_draft_read_version(text, version, 0, (size_t)version.bytes, gather_extents, &gather) == PURE_OK;
            if (lend(answer, mapping, &gather, read, (size_t)version.bytes)) return PURE_OK;
        }
        status = pure_app_view_text(app, view, put_span, &o);
    } else {
        *type = "application/json";
        size_t rows = pure_app_rows(app, view), from = route->from < rows ? route->from : rows;
        size_t end = from + (route->count < rows - from ? route->count : rows - from);
        /* Each column's key, ,"name": (the first row's first without the comma), made once. */
        struct { uint32_t view; size_t length; char key[PURE_MODULE_NAME + 4]; } columns[PURE_MODULE_VIEWS];
        unsigned count = 0;
        for (uint32_t column = 0; column < m->views; ++column)
            if (m->view_table[column] == view + 1) {
                columns[count].view = column;
                columns[count].length = (size_t)snprintf(columns[count].key, sizeof(columns[count].key), ",\"%s\":",
                                                         m->view_names[column]);
                ++count;
            }
        char head[80];
        put_bytes(&o, head, (size_t)snprintf(head, sizeof(head), "{\"rows\":%zu,\"from\":%zu,\"items\":[", rows, from));
        for (size_t row = from; row < end && !o.failed; ++row) {
            put_bytes(&o, ",{" + (row == from), 2 - (row == from));
            for (unsigned k = 0; k < count; ++k) {
                put_bytes(&o, columns[k].key + !k, columns[k].length - !k);
                if (m->view_type[columns[k].view] == PURE_VIEW_NUM) {
                    if (pure_app_cell(app, columns[k].view, row, &value)) put_number(&o, value);
                    else put_bytes(&o, "null", 4);
                    continue;
                }
                size_t mark = o.size;
                put_bytes(&o, "\"", 1);
                if (pure_app_cell_text(app, columns[k].view, row, put_json_span, &o) == PURE_OK) put_bytes(&o, "\"", 1);
                else if (!o.failed) {
                    o.size = mark;
                    put_bytes(&o, "null", 4);
                }
            }
            put_bytes(&o, "}", 1);
        }
        put_bytes(&o, "]}", 2);
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
    uint32_t header = get_u32(effect->value.data);
    const unsigned char *delivery = effect->value.data + 4;
    unsigned batch = header & DELIVERED ? get_u32(delivery + 8) : 1;
    size_t lead = 4 + (header & DELIVERED ? DELIVERY : 0), table = header & PLACED ? 8 * (size_t)shape->field_count : 0;
    PureSpan payload = {effect->value.data + lead + batch * table, header & ~(PLACED | DELIVERED)};
    PureResult status;
    if (shape->kind == PURE_KIND_TEXT) {
        status = pure_app_text(app, route->target, payload);
    } else if (shape->kind == PURE_KIND_KEY) {
        status = pure_app_input(app, route->target, PURE_EDGE_DOWN);
        if (status == PURE_OK) status = pure_app_input(app, route->target, PURE_EDGE_UP);
    } else {
        uint32_t placed[2 * PURE_MODULE_FIELDS];
        for (size_t i = 0; table && i < 2 * (size_t)shape->field_count; ++i)
            placed[i] = get_u32(effect->value.data + lead + 4 * i);
        if (header & DELIVERED) {
            char source[PURE_SOURCE + 1];
            memcpy(source, delivery + 13, delivery[12]);
            source[delivery[12]] = 0;
            status = pure_app_deliver(app, route->target, source, get_u64(delivery), payload, table ? placed : NULL);
            /* The rest of a batch, in order: an app's refusal was heard and the
             * batch goes on; another failure stops it, to be sent again. */
            for (unsigned i = 1; i < batch && (status == PURE_OK || app->fault != PURE_FAULT_NONE); ++i) {
                const unsigned char *at = effect->value.data + lead + i * table;
                for (size_t k = 0; k < 2 * (size_t)shape->field_count; ++k) placed[k] = get_u32(at + 4 * k);
                PureSpan next = {payload.data + i * shape->prefix, payload.size - i * shape->prefix};
                status = pure_app_deliver(app, route->target, source, get_u64(delivery) + i, next, placed);
            }
            if (batch > 1 && app->fault != PURE_FAULT_NONE) status = PURE_OK;
        } else
            status = table ? pure_app_message_fields(app, route->target, NULL, -1, payload, placed) :
                             pure_app_message(app, route->target, NULL, -1, payload);
        Reply reply = {0};
        if (status == PURE_OK) pure_app_reply(app, measure_reply, &reply);
        out->reply = (PureSpan){reply.size ? reply.first : NULL, reply.size};
    }
    out->fault = app->fault;
    return status;
}

static void apply_effect(Executor *ex, const Job *job, Applied *out) {
    PureApp *app = &ex->app;
    const PureEffect *effect = &job->effect;
    app->now = wall_ms();
    *out = (Applied){.host = job->host, .ticket = effect->ticket, .input = effect->input, .route = job->route.kind};
    memcpy(out->key, effect->key, strlen(effect->key) + 1);
    if (job->route.kind == ROUTE_VIEW) {
        out->status = show_view(app, &job->route, effect->available, out);
        return;
    }
    if (job->route.kind == ROUTE_INPUT) {
        out->status = apply_input(app, &job->route, effect, out);
        if (out->status == PURE_OK) ++ex->burst_events;
        return;
    }
    bool read = effect->input == PURE_INPUT_READ;
    out->status = pure_app_message(app, read ? ex->show : ex->edit, effect->key,
                                   read ? -1 : effect->expected, read ? (PureSpan){0} : effect->value);
    out->revision = app->revision;
    if (out->status != PURE_OK) return;
    ++ex->burst_events;
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
        Gather gather = {NULL, 0, 0, mapping, true};
        bool read = pure_app_reply(app, gather_extents, &gather) == PURE_OK;
        if (lend(out, mapping, &gather, read, reply.size)) return;
    }
    unsigned char *copy = malloc(reply.size);
    if (!copy) { out->status = PURE_ALLOCATION; return; }
    reply = (Reply){.bytes = copy};
    pure_app_reply(app, copy_reply, &reply);
    out->resource = (PureResource){.body = {copy, reply.size}, .allocation = copy, .allocation_size = reply.size,
                                   .release = release_copy};
}

/* After a save, before its answers go: from now on views show what it
 * saved, so a read made after a write's answer sees the write. A failed save
 * returns the app to the last save's states, which the snapshot still shows. */
static void publish(Executor *ex) {
    if (!ex->publishes || (ex->published && ex->published_changes == ex->app.changes)) return;
    PureSnapshot *made = pure_app_snapshot(&ex->app), *old;
    pthread_mutex_lock(&ex->published_lock);
    old = ex->published;
    ex->published = made;
    pthread_mutex_unlock(&ex->published_lock);
    pure_snapshot_release(old);
    ex->published_changes = ex->app.changes;
    ++ex->publications;
}

/* The saved sends of wired outputs not queued yet go to their wires, in
 * order; a wire that cannot take one takes the rest at a later save. The
 * outbox is looked at newest first, only as far back as the new sends go,
 * however many older ones wait for their receivers. */
typedef struct { uint32_t wire; uint64_t seq; PureSpan payload; } Fresh;
typedef struct { Executor *executor; Fresh *fresh; size_t count, capacity; unsigned done; bool failed; } Queueing;

static bool find_fresh(void *context, uint32_t output, uint64_t seq, PureSpan payload) {
    Queueing *q = context;
    Executor *ex = q->executor;
    for (unsigned i = 0; i < ex->wire_count; ++i) {
        if (pure_wire_output(ex->wires[i]) != output || (q->done >> i & 1)) continue;
        if (seq <= pure_wire_queued(ex->wires[i])) {
            q->done |= 1u << i; /* older ones were queued before */
            break;
        }
        if (q->count == q->capacity) {
            size_t capacity = q->capacity ? 2 * q->capacity : 64;
            Fresh *grown = realloc(q->fresh, capacity * sizeof(*grown));
            if (!grown) { q->failed = true; return false; }
            q->fresh = grown;
            q->capacity = capacity;
        }
        q->fresh[q->count++] = (Fresh){i, seq, payload};
        break;
    }
    return q->done != (ex->wire_count >= 32 ? UINT32_MAX : (1u << ex->wire_count) - 1);
}

static void after_save(Executor *ex) {
    publish(ex);
    if (!ex->wire_count) return;
    Queueing q = {.executor = ex};
    pure_app_outbox_newest(&ex->app, find_fresh, &q);
    bool blocked[PURE_MODULE_INPUTS] = {false};
    for (size_t k = q.failed ? 0 : q.count; k-- > 0;) {
        const Fresh *f = &q.fresh[k];
        if (!blocked[f->wire]) blocked[f->wire] = !pure_wire_queue(ex->wires[f->wire], f->seq, f->payload);
    }
    free(q.fresh);
}

/* The answers the wires made become events, in order; then what the wires
 * delivered leaves the outbox (with the next save). True when an answer was
 * applied (the caller saves). */
static bool take_deliveries(Executor *ex) {
    bool answered = false;
    for (unsigned i = 0; i < ex->wire_count; ++i) {
        PureWireAnswer answer;
        while (pure_wire_answer(ex->wires[i], &answer)) {
            ex->app.now = wall_ms();
            PureResult result = pure_app_deliver(&ex->app, answer.input, answer.source, answer.seq, answer.payload,
                                                 answer.placed);
            if (result != PURE_OK && ex->app.fault == PURE_FAULT_NONE) {
                ex->held[i] = true; /* not applied and not refused: sent again after a restart */
                fprintf(stderr, "{\"wire\":\"%s\",\"seq\":%" PRIu64 ",\"error\":\"its answer was not applied\"}\n",
                        answer.source, answer.seq);
            }
            pure_app_finish(&ex->app);
            answered = true;
            ++ex->answers;
        }
        uint64_t delivered = pure_wire_delivered(ex->wires[i]);
        if (delivered <= ex->trimmed[i] || ex->held[i]) continue;
        pure_app_trim(&ex->app, pure_wire_output(ex->wires[i]), delivered);
        ex->trimmed[i] = delivered;
    }
    return answered;
}

/* One save for a burst: its requests are durable with it or fail with it. */
static void save_burst(Executor *ex, Applied *applied, size_t count) {
    if (!count) return;
    for (size_t i = 0; i < count; ++i)
        ex->wrote = ex->wrote || (applied[i].status == PURE_OK && applied[i].input == PURE_INPUT_WRITE);
    PureResult saved = pure_app_save(&ex->app);
    ++ex->bursts;
    if (saved == PURE_OK) after_save(ex);
    for (size_t i = 0; i < count; ++i)
        if (applied[i].status == PURE_OK) applied[i].status = saved;
}

/* Run a round's requests in bursts. A burst ends before a document's key
 * repeats or before its documents would outgrow one save, so every request
 * is stored by its burst's save or fails with it; views and events of the
 * app's states join any burst. Each group of answers goes to finish.
 *
 * The owner takes a round's reads first and answers them at once: a
 * round's requests come from different connections (each has one request
 * at a time), so nothing orders them, and placed before the writes the
 * reads see only saved states (a round starts from a save). With writes
 * going on this halves the reads' wait and doubles both rates. A single
 * thread does not: it waits out each save itself, and reads answered first
 * come back first, splitting its next writes over more saves (the owner
 * gathers after a save instead). */
static void execute(Executor *ex, const Job *jobs, size_t count, void (*finish)(void *, Applied *, size_t),
                    void *context) {
    if (!count) return;
    if (!grow(&ex->applied, &ex->applied_capacity, count, sizeof(*ex->applied))) abort();
    Applied *applied = ex->applied;
    /* One look at the store per round: a request made after another shard's
     * answer was sent is read after that answer's commit. */
    bool pinned = pure_store_pin(ex->store, true) == PURE_OK;
    size_t reads = 0;
    for (size_t i = 0; ex->reads_first && i < count; ++i)
        if (jobs[i].effect.input == PURE_INPUT_READ) apply_effect(ex, &jobs[i], &applied[reads++]);
    if (reads) finish(context, applied, reads);
    size_t n = reads, start = reads, keys = 0;
    for (size_t i = 0; i < count; ++i) {
        if (ex->reads_first && jobs[i].effect.input == PURE_INPUT_READ) continue;
        /* A document joins a burst once (its revision moves once a save). */
        bool document = jobs[i].route.kind == ROUTE_OBJECT;
        if (document && (keys == PURE_APP_PENDING || pure_app_pending(&ex->app, jobs[i].effect.key))) {
            save_burst(ex, applied + start, n - start);
            finish(context, applied + start, n - start);
            start = n;
            keys = 0;
        }
        apply_effect(ex, &jobs[i], &applied[n++]);
        keys += document;
    }
    save_burst(ex, applied + start, n - start);
    finish(context, applied + start, n - start);
    if (pinned) pure_store_pin(ex->store, false);
}

/* Tick every timer that is due, with one save for them (an app with states
 * only: a timer can change nothing else). The milliseconds until the next
 * is due, or -1 without timers. Missed ticks are not made up. */
static long run_timers(Executor *ex) {
    const PureModule *m = ex->module;
    if (!m->states) return -1;
    double now = wall_ms(), soonest = -1;
    bool ticked = false;
    for (uint32_t i = 0; i < m->inputs; ++i) {
        if (m->input[i].kind != PURE_KIND_TIMER) continue;
        if (!ex->next[i]) ex->next[i] = now + m->input[i].period; /* the first period starts now */
        if (ex->next[i] <= now) {
            ex->app.now = now;
            if (pure_app_tick(&ex->app, i) == PURE_OK) ++ex->ticks;
            ticked = true;
            ex->next[i] = now + m->input[i].period;
        }
        if (soonest < 0 || ex->next[i] - now < soonest) soonest = ex->next[i] - now;
    }
    if (ticked && pure_app_save(&ex->app) == PURE_OK) after_save(ex); /* a failed save undoes the ticks */
    return soonest < 0 ? -1 : (long)ceil(soonest);
}

/* On the request's connection thread: complete its ticket and answer it if
 * its connection still waits for it. */
static void answer_applied(Host *host, Applied *a) {
    if (!pure_runtime_complete(a->ticket, a->status, a->input == PURE_INPUT_READ ? &a->resource : NULL,
                               a->status == PURE_OK ? a->reply : (PureSpan){0}, a->revision)) abort();
    Connection *c = host->by_slot[a->ticket.slot];
    if (c && c->phase == WAITING && c->ticket.generation == a->ticket.generation) {
        PureOutcome result;
        if (!pure_runtime_result(a->ticket, &result)) abort();
        /* A lent answer's body is its extents (the runtime charged them). */
        if (result.status == PURE_OK && a->extents)
            respond_typed(c, 200, "OK", "none", result.revision, a->type ? a->type : "application/octet-stream",
                          a->extents, a->extent_count, result.body.size);
        else if (a->route != ROUTE_OBJECT) answer_app(c, result, a->route == ROUTE_VIEW, a->fault, a->type);
        else answer(c, result);
        drive(c); /* most answers fit the socket buffer at once */
    }
    free(a->extents);
}

static void answer_burst(void *context, Applied *applied, size_t count) {
    (void)context;
    for (size_t i = 0; i < count; ++i) answer_applied(applied[i].host, &applied[i]);
}

/* On the owner: give each connection thread its applied requests, and wake it. */
static void hand_back(void *context, Applied *applied, size_t count) {
    Owner *owner = context;
    for (unsigned h = 0; h < owner->host_count; ++h) {
        Host *host = &owner->hosts[h];
        size_t mine = 0;
        for (size_t i = 0; i < count; ++i) mine += applied[i].host == host;
        if (!mine) continue;
        pthread_mutex_lock(&host->lock);
        if (!grow(&host->done, &host->done_capacity, host->done_count + mine, sizeof(*host->done))) abort();
        for (size_t i = 0; i < count; ++i)
            if (applied[i].host == host) host->done[host->done_count++] = applied[i];
        pthread_mutex_unlock(&host->lock);
        uint64_t one = 1;
        if (write(host->wake, &one, sizeof(one)) != sizeof(one)) abort();
    }
}

/* On a connection thread: answer what the owner handed back. */
static void answer_handed(Host *host) {
    uint64_t signals;
    if (read(host->wake, &signals, sizeof(signals)) < 0 && errno != EAGAIN) abort();
    pthread_mutex_lock(&host->lock);
    Applied *done = host->done;
    size_t count = host->done_count, capacity = host->done_capacity;
    host->done = host->answering;
    host->done_capacity = host->answering_capacity;
    host->done_count = 0;
    pthread_mutex_unlock(&host->lock);
    host->answering = done;
    host->answering_capacity = capacity;
    for (size_t i = 0; i < count; ++i) answer_applied(host, &done[i]);
    host->pending -= count;
}

/* The owner's newest snapshot, held, shown by this thread's reader; NULL
 * when there is none. Taken for each round, after the round's answers from
 * the owner: those were handed back after their saves' snapshots. */
static PureSnapshot *read_published(Host *host) {
    Executor *ex = &host->owner->executor;
    pthread_mutex_lock(&ex->published_lock);
    PureSnapshot *snapshot = ex->published;
    if (snapshot) pure_snapshot_hold(snapshot);
    pthread_mutex_unlock(&ex->published_lock);
    if (snapshot) pure_app_read(&host->reader, snapshot);
    return snapshot;
}

/* Every request made ready since the last round: run here, or handed to the
 * owner. Views are shown here from the snapshot when there is one, after the
 * rest went to the owner, which meanwhile applies and saves them. Answering
 * may make more ready; they form the next round. */
static void serve_bursts(Host *host) {
    for (;;) {
        size_t count = 0;
        PureEffect effect;
        while (pure_runtime_take(&host->runtime, &effect) == PURE_TAKE_EFFECT)
            host->jobs[count++] = (Job){host, effect, host->routes[effect.ticket.slot]};
        if (!count) return;
        if (!host->owner) {
            execute(&host->local, host->jobs, count, answer_burst, NULL);
            continue;
        }
        PureSnapshot *snapshot = NULL;
        size_t views = 0;
        for (size_t i = 0; i < count; ++i) views += host->jobs[i].route.kind == ROUTE_VIEW;
        if (views && !(snapshot = read_published(host))) views = 0; /* the owner shows them */
        size_t handed = count - views;
        if (handed) {
            Owner *owner = host->owner;
            pthread_mutex_lock(&owner->lock);
            if (!grow(&owner->jobs, &owner->capacity, owner->count + handed, sizeof(*owner->jobs))) abort();
            for (size_t i = 0; i < count; ++i)
                if (!snapshot || host->jobs[i].route.kind != ROUTE_VIEW) owner->jobs[owner->count++] = host->jobs[i];
            owner->arrivals += handed;
            pthread_cond_signal(&owner->waiting);
            pthread_mutex_unlock(&owner->lock);
            host->pending += handed;
        }
        if (!views) return;
        /* Shown first and answered after, as a burst is. */
        if (!grow(&host->views, &host->views_capacity, views, sizeof(*host->views))) abort();
        size_t shown = 0;
        for (size_t i = 0; i < count; ++i) {
            const Job *job = &host->jobs[i];
            if (job->route.kind != ROUTE_VIEW) continue;
            Applied *a = &host->views[shown++];
            *a = (Applied){.host = host, .ticket = job->effect.ticket, .input = job->effect.input, .route = ROUTE_VIEW};
            a->status = show_view(&host->reader, &job->route, job->effect.available, a);
        }
        pure_snapshot_release(snapshot); /* what answers lend holds its own mapping */
        for (size_t i = 0; i < shown; ++i) answer_applied(host, &host->views[i]);
        host->shown += shown;
    }
}

/* The owner's thread: take every waiting request of every connection thread
 * as one round, until they have all stopped. */
static struct timespec later(struct timespec t, long nanoseconds) {
    t.tv_sec += nanoseconds / 1000000000;
    t.tv_nsec += nanoseconds % 1000000000;
    if (t.tv_nsec >= 1000000000) { ++t.tv_sec; t.tv_nsec -= 1000000000; }
    return t;
}

static bool before(struct timespec a, struct timespec b) {
    return a.tv_sec < b.tv_sec || (a.tv_sec == b.tv_sec && a.tv_nsec < b.tv_nsec);
}

static void *run_owner(void *context) {
    Owner *owner = context;
    Job *spare = NULL;
    size_t spare_capacity = 0;
    bool gather = false;
    uint64_t answered_at = 0; /* arrivals when the last round's answers were handed back */
    for (;;) {
        pthread_mutex_lock(&owner->lock);
        /* Wait for requests, or until the next timer is due; trim what the
         * wires delivered meanwhile. */
        for (;;) {
            bool delivered = owner->delivered;
            owner->delivered = false;
            pthread_mutex_unlock(&owner->lock);
            long due = run_timers(&owner->executor);
            bool answered = delivered && take_deliveries(&owner->executor);
            pthread_mutex_lock(&owner->lock);
            /* Answers are saved with the next round, or now if none waits. */
            if (answered && !owner->count && !owner->stopping) {
                pthread_mutex_unlock(&owner->lock);
                if (pure_app_save(&owner->executor.app) == PURE_OK) after_save(&owner->executor);
                pthread_mutex_lock(&owner->lock);
            }
            if (owner->count || owner->stopping) break;
            if (owner->delivered) continue;
            if (due < 0) {
                pthread_cond_wait(&owner->waiting, &owner->lock);
                continue;
            }
            struct timespec wake;
            clock_gettime(CLOCK_REALTIME, &wake);
            wake = later(wake, due * 1000000L);
            pthread_cond_timedwait(&owner->waiting, &owner->lock, &wake);
        }
        /* After a round that saved, its answers bring requests back from the
         * connection threads: gather them until none has come for 100 µs (a
         * millisecond at most), so they share one save as they would on a
         * single thread, where answering takes that long anyway. The quiet
         * counts only once a request came after the answers: requests that
         * waited out the save alone would otherwise start the next round
         * without the clients just answered, and the clients would settle
         * into two halves taking turns, each save carrying half of them. */
        struct timespec now, end;
        clock_gettime(CLOCK_REALTIME, &now);
        end = later(now, 1000000);
        while (gather && owner->count && !owner->stopping && before(now, end)) {
            size_t seen = owner->count;
            bool returned = owner->arrivals != answered_at;
            struct timespec quiet = later(now, 100000);
            pthread_cond_timedwait(&owner->waiting, &owner->lock, returned && before(quiet, end) ? &quiet : &end);
            if (returned && owner->count == seen) break;
            clock_gettime(CLOCK_REALTIME, &now);
        }
        if (!owner->count) {
            pthread_mutex_unlock(&owner->lock);
            break;
        }
        Job *jobs = owner->jobs;
        size_t count = owner->count, capacity = owner->capacity;
        owner->jobs = spare;
        owner->capacity = spare_capacity;
        owner->count = 0;
        pthread_mutex_unlock(&owner->lock);
        owner->executor.wrote = false;
        execute(&owner->executor, jobs, count, hand_back, owner);
        gather = owner->executor.wrote;
        pthread_mutex_lock(&owner->lock);
        answered_at = owner->arrivals;
        pthread_mutex_unlock(&owner->lock);
        spare = jobs;
        spare_capacity = capacity;
    }
    free(spare);
    return NULL;
}

/* A write's reply is borrowed from its request, so the handler of an input
 * served by PUT or POST may reply only a field of its message. */
static bool replies_borrow(const PureModule *m, uint32_t input) {
    for (uint32_t h = 0; h < m->handlers; ++h) {
        if (m->handler[h].event != 1 + 2 * input) continue;
        const PureBlock *code = &m->handler[h].code;
        /* A field replied as it is, or a text field as its bytes. */
        for (uint32_t pc = code->start; pc < code->start + code->length; ++pc) {
            if (m->code[pc].op != PURE_OP_REPLY) continue;
            uint32_t from = pc > code->start && m->code[pc - 1].op == PURE_OP_BYTES_FROM ? pc - 1 : pc;
            if (from == code->start || m->code[from - 1].op != PURE_OP_FIELD) return false;
        }
    }
    return true;
}

/* ---- Main ------------------------------------------------------------- */

static bool load_module(PureModule *m, const char *path, bool *documents, uint32_t *show_out, uint32_t *edit_out) {
    char error[160];
    if (pure_module_read(path, m, error, sizeof(error)) != PURE_OK) {
        fprintf(stderr, "Module: %s\n", error);
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
        c->opened = 0;
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

static bool start_executor(Executor *ex, const PureModule *module, uint32_t show, uint32_t edit, const char *store) {
    ex->module = module;
    ex->show = show;
    ex->edit = edit;
    if (pure_store_open(store, &ex->store) != PURE_OK) { fputs("Cannot open the store\n", stderr); return false; }
    if (pure_app_start(&ex->app, module, pure_store_data(ex->store)) != PURE_OK || !pure_app_defer(&ex->app, true)) {
        fputs("Cannot start the module on the store\n", stderr);
        return false;
    }
    return true;
}

static void stop_executor(Executor *ex) {
    pure_snapshot_release(ex->published);
    pure_app_stop(&ex->app);
    pure_store_close(ex->store);
    free(ex->applied);
}

/* One connection thread: its own listener on the shared port, epoll set,
 * runtime domain and, unless an owner runs the app for all of them, app and
 * store handle. */
static bool start_shard(Host *host, const PureModule *module, bool documents, uint32_t show, uint32_t edit,
                        const char *store, size_t budget, unsigned *port) {
    host->module = module;
    host->documents = documents;
    host->wake = -1;
    if (!host->owner && !start_executor(&host->local, module, show, edit, store)) return false;
    host->work = calloc(host->capacity, sizeof(*host->work));
    host->table = calloc(host->capacity, sizeof(*host->table));
    host->by_slot = calloc(host->capacity, sizeof(*host->by_slot));
    host->jobs = calloc(host->capacity, sizeof(*host->jobs));
    host->routes = calloc(host->capacity, sizeof(*host->routes));
    if (!host->work || !host->table || !host->by_slot || !host->jobs || !host->routes) {
        fputs("Cannot allocate connections\n", stderr);
        return false;
    }
    if (!pure_runtime_init(&host->runtime, host->work, host->capacity, host, (PureHost){.allocate = allocate_ingress},
                           budget) ||
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
    /* The owner says it handed requests back through an eventfd. */
    if (host->owner) {
        struct epoll_event wake_event = {.events = EPOLLIN, .data.ptr = &host->wake};
        if (pthread_mutex_init(&host->lock, NULL) || (host->wake = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)) < 0 ||
            epoll_ctl(host->epoll, EPOLL_CTL_ADD, host->wake, &wake_event)) {
            perror("eventfd");
            return false;
        }
    }
    return true;
}

static void *run_shard(void *context) {
    Host *host = context;
    struct epoll_event events[EVENTS];
    uint64_t next_sweep = now_ms() + 1000;
    while (!atomic_load(&stopping)) {
        /* A thread that runs the app ticks its timers on time. */
        long due = host->owner ? -1 : run_timers(&host->local);
        int ready = epoll_wait(host->epoll, events, EVENTS, due >= 0 && due < 1000 ? (int)due : 1000);
        for (int e = 0; e < ready; ++e) {
            Connection *c = events[e].data.ptr;
            if (!c) { accept_all(host); continue; }
            if (events[e].data.ptr == &host->wake) { answer_handed(host); continue; }
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
    /* What the owner still runs is answered before the connections close. */
    while (host->pending) {
        struct pollfd wake = {host->wake, POLLIN, 0};
        if (poll(&wake, 1, -1) > 0) answer_handed(host);
    }
    for (size_t i = 0; i < host->capacity; ++i) if (host->table[i]) close_connection(host->table[i]);
    return NULL;
}

static void stop_shard(Host *host) {
    if (!host->owner) stop_executor(&host->local);
    pure_app_stop(&host->reader);
    if (host->listener >= 0) close(host->listener);
    if (host->epoll >= 0) close(host->epoll);
    if (host->wake >= 0) close(host->wake);
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
    free(host->jobs);
    free(host->routes);
    free(host->done);
    free(host->answering);
    free(host->views);
}

/* A wire delivered a send: the owner trims it. */
static void wire_delivered(void *context) {
    Owner *owner = context;
    pthread_mutex_lock(&owner->lock);
    owner->delivered = true;
    pthread_cond_signal(&owner->waiting);
    pthread_mutex_unlock(&owner->lock);
}

/* --send OUTPUT=URL: that output's saved sends go to url, from NAME.OUTPUT. */
static bool open_wires(Executor *ex, const PureModule *m, const char *const *sends, unsigned count, const char *name) {
    for (unsigned i = 0; i < count; ++i) {
        const char *equals = strchr(sends[i], '=');
        char output[PURE_MODULE_NAME], source[PURE_SOURCE + 2], error[160];
        size_t length = equals ? (size_t)(equals - sends[i]) : 0;
        if (!length || length >= sizeof(output)) {
            fprintf(stderr, "--send takes OUTPUT=URL, not %s\n", sends[i]);
            return false;
        }
        memcpy(output, sends[i], length);
        output[length] = 0;
        int written = snprintf(source, sizeof(source), "%s.%s", name, output);
        PureWire *wire = written > 0 && written <= PURE_SOURCE ? pure_wire_open(m, output, equals + 1, source, error, sizeof(error)) : NULL;
        if (!wire) {
            fprintf(stderr, "--send %s: %s\n", sends[i], written > PURE_SOURCE ? "the name and output are longer than 64 bytes" : error);
            return false;
        }
        for (unsigned k = 0; k < ex->wire_count; ++k)
            if (pure_wire_output(ex->wires[k]) == pure_wire_output(wire)) {
                fprintf(stderr, "--send %s: the output is wired already\n", sends[i]);
                pure_wire_close(wire);
                return false;
            }
        ex->wires[ex->wire_count++] = wire;
    }
    return true;
}

/* --seal OUTPUT=KEY: that output's sends sealed to the receiver's public key (hex). */
static bool wire_seals(Executor *ex, const PureModule *m, const char *const *seals, unsigned count) {
    for (unsigned i = 0; i < count; ++i) {
        const char *equals = strchr(seals[i], '=');
        unsigned char peer[PURE_SEAL_KEY];
        char output[PURE_MODULE_NAME];
        size_t length = equals ? (size_t)(equals - seals[i]) : 0;
        PureWire *wire = NULL;
        if (length && length < sizeof(output)) {
            memcpy(output, seals[i], length);
            output[length] = 0;
            for (unsigned k = 0; k < ex->wire_count; ++k)
                if (!strcmp(m->input_names[pure_wire_output(ex->wires[k])], output)) wire = ex->wires[k];
        }
        if (!wire || !pure_seal_peer(equals + 1, peer)) {
            fprintf(stderr, "--seal takes OUTPUT=KEY for an output given to --send, the key as 64 hex digits: %s\n", seals[i]);
            return false;
        }
        pure_wire_seal(wire, peer);
    }
    return true;
}

/* --reply OUTPUT=INPUT: the answers to that output's sends become INPUT's events. */
static bool wire_replies(Executor *ex, const PureModule *m, const char *const *replies, unsigned count) {
    for (unsigned i = 0; i < count; ++i) {
        const char *equals = strchr(replies[i], '=');
        char output[PURE_MODULE_NAME], error[160] = "";
        size_t length = equals ? (size_t)(equals - replies[i]) : 0;
        int input = equals ? pure_module_input(m, equals + 1) : -1;
        PureWire *wire = NULL;
        if (length && length < sizeof(output)) {
            memcpy(output, replies[i], length);
            output[length] = 0;
            int index = pure_module_input(m, output);
            for (unsigned k = 0; k < ex->wire_count && index >= 0; ++k)
                if (pure_wire_output(ex->wires[k]) == (uint32_t)index) wire = ex->wires[k];
        }
        if (!wire || input < 0 || !pure_wire_reply(wire, (uint32_t)input, error, sizeof(error))) {
            fprintf(stderr, "--reply %s: %s\n", replies[i], !wire ? "takes OUTPUT=INPUT of an output given --send" :
                                                         input < 0 ? "no such input" : error);
            return false;
        }
    }
    return true;
}

/* --threads auto: a connection thread for each CPU this process may run on,
 * less the one the owner of an app with states takes. */
static unsigned auto_threads(bool owner) {
    cpu_set_t set;
    long cpus = sched_getaffinity(0, sizeof(set), &set) ? sysconf(_SC_NPROCESSORS_ONLN) : CPU_COUNT(&set);
    if (cpus < 1) cpus = 1;
    if (owner && cpus > 1) --cpus;
    return cpus > 256 ? 256 : (unsigned)cpus;
}

int main(int argc, char **argv) {
    static PureModule module;
    static Owner owner;
    const char *module_path = NULL, *store = NULL, *name = NULL, *sends[PURE_MODULE_INPUTS], *replies[PURE_MODULE_INPUTS];
    const char *seals[PURE_MODULE_INPUTS], *key_path = NULL;
    unsigned send_count = 0, reply_count = 0, seal_count = 0;
    unsigned port = 8767, threads = 1;
    size_t budget = 64u << 20, capacity = 1024;
    uint64_t timeout_ms = 10000;
    for (int i = 1; i < argc; i += 2) {
        uint64_t value;
        if (!strcmp(argv[i], "--sealed-only")) { sealed_only = true; --i; continue; }
        if (i + 1 == argc) goto usage;
        if (!strcmp(argv[i], "--module")) module_path = argv[i + 1];
        else if (!strcmp(argv[i], "--key")) key_path = argv[i + 1];
        else if (!strcmp(argv[i], "--seal") && seal_count < PURE_MODULE_INPUTS) seals[seal_count++] = argv[i + 1];
        else if (!strcmp(argv[i], "--store")) store = argv[i + 1];
        else if (!strcmp(argv[i], "--send") && send_count < PURE_MODULE_INPUTS) sends[send_count++] = argv[i + 1];
        else if (!strcmp(argv[i], "--name")) name = argv[i + 1];
        else if (!strcmp(argv[i], "--send-ca")) pure_wire_trust(argv[i + 1]);
        else if (!strcmp(argv[i], "--reply") && reply_count < PURE_MODULE_INPUTS) replies[reply_count++] = argv[i + 1];
        else if (!strcmp(argv[i], "--port") && number(argv[i + 1], 65535, &value)) port = (unsigned)value;
        else if (!strcmp(argv[i], "--budget") && number(argv[i + 1], SIZE_MAX, &value) && value) budget = (size_t)value;
        else if (!strcmp(argv[i], "--connections") && number(argv[i + 1], 1u << 20, &value) && value) capacity = (size_t)value;
        else if (!strcmp(argv[i], "--threads") && !strcmp(argv[i + 1], "auto")) threads = 0;
        else if (!strcmp(argv[i], "--threads") && number(argv[i + 1], 256, &value) && value) threads = (unsigned)value;
        else if (!strcmp(argv[i], "--timeout-ms") && number(argv[i + 1], 3600000, &value) && value) timeout_ms = value;
        else goto usage;
    }
    uint32_t show, edit;
    bool documents;
    if (!module_path || !store || !load_module(&module, module_path, &documents, &show, &edit)) goto usage;
    if (!threads) threads = auto_threads(module.states);
    char public_key[2 * PURE_SEAL_KEY + 1] = "";
    if (key_path) {
        char error[256];
        if (!(seal_key = pure_seal_key(key_path, public_key, error, sizeof(error)))) { fprintf(stderr, "--key %s\n", error); return 2; }
    }
    if (sealed_only && !seal_key) { fputs("--sealed-only needs --key\n", stderr); return 2; }
    if (pure_store_init(store) != PURE_OK) { fputs("Cannot initialize the store directory\n", stderr); return 1; }
    signal(SIGPIPE, SIG_IGN);
    struct sigaction stop_action = {.sa_handler = stop};
    sigemptyset(&stop_action.sa_mask);
    sigaction(SIGINT, &stop_action, NULL);
    sigaction(SIGTERM, &stop_action, NULL);
    Host *hosts = calloc(threads, sizeof(*hosts));
    pthread_t *thread = calloc(threads, sizeof(*thread)), owner_thread;
    if (!hosts || !thread) return 1;
    /* States (and outputs, whose sends are the app's state) have one owner on
     * a thread of its own, whose snapshots the connection threads read. */
    bool outputs = false;
    for (uint32_t i = 0; i < module.inputs; ++i) outputs = outputs || module.input[i].kind == PURE_KIND_OUTPUT;
    bool owned = module.states || outputs;
    /* The sender's name: --name, or the module file's name without .pmod. */
    char stem[PURE_SOURCE + 1];
    if (!name) {
        const char *base = strrchr(module_path, '/') ? strrchr(module_path, '/') + 1 : module_path;
        size_t length = strlen(base);
        if (length > 5 && !strcmp(base + length - 5, ".pmod")) length -= 5;
        snprintf(stem, sizeof(stem), "%.*s", (int)length, base);
        name = stem;
    }
    if (owned) {
        if (!start_executor(&owner.executor, &module, show, edit, store) || pthread_mutex_init(&owner.lock, NULL) ||
            pthread_cond_init(&owner.waiting, NULL) || pthread_mutex_init(&owner.executor.published_lock, NULL))
            return 1;
        owner.executor.reads_first = true;
        owner.executor.publishes = true;
        if (!open_wires(&owner.executor, &module, sends, send_count, name) ||
            !wire_replies(&owner.executor, &module, replies, reply_count) ||
            !wire_seals(&owner.executor, &module, seals, seal_count)) return 2;
        after_save(&owner.executor); /* the snapshot, and the sends stored before */
        for (unsigned i = 0; i < owner.executor.wire_count; ++i)
            if (!pure_wire_start(owner.executor.wires[i], wire_delivered, &owner)) { fputs("Cannot start a wire\n", stderr); return 1; }
        owner.hosts = hosts;
        owner.host_count = threads;
    }
    for (unsigned i = 0; i < threads; ++i) {
        hosts[i].capacity = capacity;
        hosts[i].timeout_ms = timeout_ms;
        hosts[i].listener = hosts[i].epoll = -1;
        hosts[i].owner = owned ? &owner : NULL;
        if (!start_shard(&hosts[i], &module, documents, show, edit, store, budget, &port)) return 1;
    }
    if (owned && pthread_create(&owner_thread, NULL, run_owner, &owner)) { fputs("Cannot start the owner\n", stderr); return 1; }
    if (*public_key) printf("{\"port\":%u,\"threads\":%u,\"public_key\":\"%s\"}\n", port, threads, public_key);
    else printf("{\"port\":%u,\"threads\":%u}\n", port, threads);
    fflush(stdout);
    for (unsigned i = 1; i < threads; ++i)
        if (pthread_create(&thread[i], NULL, run_shard, &hosts[i])) { fputs("Cannot start a thread\n", stderr); return 1; }
    run_shard(&hosts[0]);
    for (unsigned i = 1; i < threads; ++i) pthread_join(thread[i], NULL);
    if (owned) {
        pthread_mutex_lock(&owner.lock);
        owner.stopping = true;
        pthread_cond_signal(&owner.waiting);
        pthread_mutex_unlock(&owner.lock);
        pthread_join(owner_thread, NULL);
    }
    size_t active = 0, retained = 0;
    uint64_t accepted = 0, requests = 0, bursts = owner.executor.bursts, events = owner.executor.burst_events, expired = 0,
             ticks = owner.executor.ticks, shown = 0;
    for (unsigned i = 0; i < threads; ++i) {
        shown += hosts[i].shown;
        active += hosts[i].active;
        retained += hosts[i].runtime.retained;
        accepted += hosts[i].accepted;
        requests += hosts[i].requests;
        bursts += hosts[i].local.bursts;
        events += hosts[i].local.burst_events;
        ticks += hosts[i].local.ticks;
        expired += hosts[i].expired;
        stop_shard(&hosts[i]);
    }
    uint64_t delivered = 0, attempts = 0;
    for (unsigned i = 0; i < owner.executor.wire_count; ++i) {
        uint64_t d, a;
        pure_wire_counts(owner.executor.wires[i], &d, &a);
        delivered += d;
        attempts += a;
        pure_wire_close(owner.executor.wires[i]); /* what was not delivered stays in the outbox */
    }
    if (owned) {
        stop_executor(&owner.executor);
        free(owner.jobs);
    }
    unsigned long groups, grouped;
    pure_store_group_counts(&groups, &grouped);
    fprintf(stderr, "{\"active\":%zu,\"retained_payload_bytes\":%zu,\"accepted\":%" PRIu64 ",\"requests\":%" PRIu64
            ",\"bursts\":%" PRIu64 ",\"burst_events\":%" PRIu64 ",\"ticks\":%" PRIu64 ",\"expired\":%" PRIu64
            ",\"threads\":%u,\"owner\":%s,\"snapshots\":%" PRIu64 ",\"views_from_snapshots\":%" PRIu64
            ",\"delivered\":%" PRIu64 ",\"delivery_attempts\":%" PRIu64 ",\"answers\":%" PRIu64
            ",\"commit_groups\":%lu,\"grouped_commits\":%lu}\n",
            active, retained, accepted, requests, bursts, events, ticks, expired, threads, owned ? "true" : "false",
            owner.executor.publications, shown, delivered, attempts, owner.executor.answers, groups, grouped);
    free(hosts);
    free(thread);
    return 0;
usage:
    fputs("pure-serve --module FILE.pmod --store DIRECTORY [--port 8767] [--threads 1|N|auto] [--budget BYTES] "
          "[--connections 1024] [--timeout-ms 10000] [--send OUTPUT=http[s]://host:port/path ...] [--name NAME] "
          "[--send-ca FILE] [--reply OUTPUT=INPUT ...] [--seal OUTPUT=KEY ...] [--key FILE] [--sealed-only]\n", stderr);
    return 2;
}
