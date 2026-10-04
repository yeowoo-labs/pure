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
#include "wait.h"
#include "link.h"
#include "seal.h"
#include "surface.h"
#include "message-json.h"
#include "sha256.h"
#include "module-store.h"
#include "module-text.h"
#include "runtime.h"
#include "store.h"
#include <arpa/inet.h>
#include <dlfcn.h>
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
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

enum { HEADER_LIMIT = 4096, EVENTS = 256, BATCH = 256 };
typedef enum { CLOSED, READING_HEAD, READING_BODY, WAITING, WRITING } Phase;
typedef struct Host Host;

/* What a request names: a document, a view (a table's rows from, count) or
 * an input. Kept per work slot, so an accepted event runs even if its
 * connection is gone before the burst. */
typedef enum { ROUTE_NONE, ROUTE_OBJECT, ROUTE_VIEW, ROUTE_INPUT, ROUTE_QUERY } RouteKind;
/* A view's (or a query's) arguments come as the request's query: argued
 * says so, and given is its text, kept on the request's work slot. */
typedef struct App App;
/* And the module it was resolved with: not applied under another (its app
 * changed module meanwhile, docs/design/017's stage 3). */
typedef struct {
    RouteKind kind;
    uint32_t target;
    size_t from, count;
    bool argued;
    const char *given;
    App *app;
    const PureModule *module;
} Route;

/* An app's tokens (docs/design/017's 3b): what each grants, by its token's
 * SHA-256. Made whole when its file is read and never changed: a reading
 * puts a new one in its place (the one before kept as long as the host
 * runs: a request may be checking it). */
enum { GRANT_READ = 1, GRANT_WRITE = 2, GRANT_INPUT = 3, BEARER = 256 };
typedef struct {
    unsigned count;
    struct Grant {
        unsigned char digest[32];
        unsigned char kind;
        char inputs[256]; /* GRANT_INPUT: the inputs' names, comma-separated */
    } grant[];
} Tokens;
static struct RetiredTokens { Tokens *tokens; struct RetiredTokens *next; } *retired_tokens;

/* This node's key, for deliveries sealed to it (--key), and whether it
 * takes only those (--sealed-only), or only those from the senders named,
 * each by the key it seals with (--sender NAME=KEY). */
static PureSealKey *seal_key;
static bool sealed_only;
static struct Sender { char name[PURE_SOURCE + 1]; unsigned char key[PURE_SEAL_KEY]; } senders[PURE_MODULE_INPUTS];
static unsigned sender_count;
static size_t memory_budget; /* --memory: the app's budget of what its states hold (0: none) */
static uint64_t work_budget;  /* --work: what an event or a view's evaluation may run (0: none) */

/* A delivery's X-Pure-Seal: the suite, the sender's session (its
 * encapsulated key), the seal's number in it and, when the sender sealed
 * with a key of its own, that key. */
typedef struct {
    char suite[32]; /* empty: not sealed */
    unsigned char enc[PURE_SEAL_KEY], from[PURE_SEAL_KEY];
    uint64_t n;
    bool authenticated;
} Seal;

/* Sessions a connection thread has opened, the least used replaced first:
 * a sender's key agreement is made once a session, not once a request. */
enum { SESSIONS = 16 };
typedef struct { Seal seal; PureOpener *opener; uint64_t used; } Session;

/* Allocated when accepted and reused after closing: memory follows the
 * connections that are open, not the configured maximum. */
typedef struct Connection {
    Host *host;
    struct Connection *next; /* on the spare or the closed list */
    size_t index;            /* in the host's table */
    int fd;
    Phase phase;
    bool readable, writable, keep_alive, put, post;
    bool hung_up; /* the peer closed its side: read until the end, never stop at a short read */
    bool events;  /* its request asks for an event stream (Accept: text/event-stream) */
    bool settled; /* on its app's home thread, or staying here (a host of apps moves it once, at its first request) */
    uint64_t deadline;
    char key[65]; /* the document's key, or the view's or input's name */
    RouteKind route;
    App *app; /* the app the path names (NULL: none of the host's) */
    const PureModule *module; /* the module its request was resolved with */
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
    /* Its request's token (Authorization: Bearer), and the digest of the
     * last token it hashed: the same again is not hashed again. */
    char bearer[BEARER + 1], hashed[BEARER + 1];
    size_t bearer_size, hashed_size;
    bool bearer_given, hashed_valid;
    unsigned char hashed_digest[32];
    char source[PURE_SOURCE + 1]; /* a delivered message's sender (X-Pure-Source), or empty */
    uint64_t sequence;            /* and its number (X-Pure-Sequence) */
    unsigned batch;               /* how many numbered messages the body holds (X-Pure-Count) */
    /* A sealed delivery: its seal (X-Pure-Seal), what the seal is bound to
     * (source, number, count), and how many plain bytes the body holds once
     * opened, cleared when its ticket goes. */
    Seal seal;
    char aad[PURE_SOURCE + 48];
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
/* A surface's event, waiting for its save: done, and how it went. */
typedef struct { PureResult status; bool done; } Waiter;

typedef struct {
    Host *host;       /* NULL for a surface's event */
    Waiter *waiter;   /* and then its waiter */
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
    struct LinkJob *link; /* records a link delivered (link/link.h) */
    const char *refused;  /* refused by the host (503 and this name), not applied */
    uint64_t heard;       /* then the source's last number heard after them */
    bool gap;             /* and whether they stopped at a gap */
    struct Group *group;  /* a push group's evaluation, by a view worker (docs/design/020) */
    uint64_t changes;     /* and the snapshot it was of */
} Applied;

/* The last save's snapshot an owner published, the connection threads'
 * to show views from (NULL: none, or its app does not run). The owner's,
 * so it outlasts the executors its app runs on. */
typedef struct Published {
    pthread_mutex_t lock;
    PureSnapshot *snapshot;
    _Atomic unsigned subscribers; /* streams of its views on any thread (docs/design/019): publishing wakes their threads */
} Published;

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
    uint64_t save_waited; /* ns the last save waited for the device (a sync), not working */
    bool reads_first; /* the owner's: a round's reads before its writes */
    uint64_t next[PURE_MODULE_INPUTS]; /* when each timer ticks next (monotonic clock, ms; 0: not yet begun) */
    uint64_t ticks;
    uint64_t memory_refused; /* events refused for the app's memory budget */
    bool answers_wait; /* an answer the app had no room for: tried again after ANSWER_WAIT_MS */
    /* The owner's: where its snapshots go (NULL: it shows its views
     * itself), and the app's changes and module the last one shows. */
    Published *published;
    uint64_t published_changes, publications;
    const PureModule *published_module;
    struct PureSurfaceHost *surface; /* told of each snapshot published */
    /* The owner's: outputs wired to HTTP receivers, and how far each was trimmed. */
    PureWire *wires[PURE_MODULE_INPUTS];
    unsigned wire_count;
    uint64_t trimmed[PURE_MODULE_INPUTS], answers;
    bool held[PURE_MODULE_INPUTS]; /* an answer failed to apply: the wire's sends stay until a restart */
    /* The owner's: outputs linked to Pure receivers (pure://), which output
     * each is, and how far each was trimmed. */
    PureLink *links[PURE_MODULE_INPUTS];
    unsigned link_count;
    uint32_t link_output[PURE_MODULE_INPUTS];
    uint64_t link_trimmed[PURE_MODULE_INPUTS];
    /* The owner's: outputs handed to other apps of the host (send=OUTPUT=APP/INPUT), and how far each was trimmed. */
    struct Local *locals[PURE_MODULE_INPUTS];
    unsigned local_count;
    uint64_t local_trimmed[PURE_MODULE_INPUTS], handed;
    /* Each outlet's place in the outbox, wires first, then links, then
     * locals: its sends before it are queued (feed_outlets); and the app's
     * sends_gone when they were last moved back. How often an outlet's
     * window was full. */
    size_t outlet_from[3 * PURE_MODULE_INPUTS];
    uint64_t gone_seen, windows_full;
    struct Writer *writer; /* the thread writing the app's checkpoints, once one was due */
    uint64_t checkpoints;  /* images it wrote */
} Executor;

/* The images of the app's lists are written on a thread of their own
 * (pure_app_checkpoint): the owner hands one over after a save, and takes
 * it back, written, after a later one. */
typedef struct Writer {
    pthread_mutex_t lock;
    pthread_cond_t wake;
    pthread_t thread;
    PureCheckpoint *taken, *written;
    PureResult result;
    bool stopping;
} Writer;

static void *run_writer(void *context) {
    Writer *w = context;
    pthread_mutex_lock(&w->lock);
    for (;;) {
        while (!w->taken && !w->stopping) pthread_cond_wait(&w->wake, &w->lock);
        if (!w->taken) break;
        PureCheckpoint *c = w->taken;
        pthread_mutex_unlock(&w->lock);
        PureResult result = pure_checkpoint_write(c);
        pthread_mutex_lock(&w->lock);
        w->taken = NULL;
        w->written = c;
        w->result = result;
    }
    pthread_mutex_unlock(&w->lock);
    return NULL;
}

/* After a save: a checkpoint written is handed back, and one due handed over. */
static void checkpoint_round(Executor *ex) {
    Writer *w = ex->writer;
    if (w) {
        pthread_mutex_lock(&w->lock);
        PureCheckpoint *written = w->written;
        PureResult result = w->result;
        w->written = NULL;
        pthread_mutex_unlock(&w->lock);
        if (written) {
            pure_app_checkpointed(&ex->app, written, result);
            ex->checkpoints += result == PURE_OK;
        }
    }
    PureCheckpoint *due = pure_app_checkpoint(&ex->app);
    if (!due) return;
    if (!w) {
        w = calloc(1, sizeof(*w));
        if (!w || pthread_mutex_init(&w->lock, NULL) || pthread_cond_init(&w->wake, NULL) ||
            pthread_create(&w->thread, NULL, run_writer, w)) {
            free(w); /* written here instead */
            pure_app_checkpointed(&ex->app, due, pure_checkpoint_write(due));
            return;
        }
        pthread_setname_np(w->thread, "pure-checkpoint"); /* its CPU, seen apart */
        ex->writer = w;
    }
    pthread_mutex_lock(&w->lock);
    w->taken = due;
    pthread_cond_signal(&w->wake);
    pthread_mutex_unlock(&w->lock);
}

/* The writer finishes the image it writes, which goes back to the app. */
static void stop_writer(Executor *ex) {
    Writer *w = ex->writer;
    if (!w) return;
    pthread_mutex_lock(&w->lock);
    w->stopping = true;
    pthread_cond_signal(&w->wake);
    pthread_mutex_unlock(&w->lock);
    pthread_join(w->thread, NULL);
    if (w->written) {
        pure_app_checkpointed(&ex->app, w->written, w->result);
        ex->checkpoints += w->result == PURE_OK;
    }
    pthread_mutex_destroy(&w->lock);
    pthread_cond_destroy(&w->wake);
    free(w);
    ex->writer = NULL;
}

/* A request made ready: its effect, what it names, and the thread that
 * answers it (or, for a surface's event, the waiter it wakes). */
typedef struct LinkJob { PureLinkListener *listener; void *token; unsigned char *ingress; struct Local *local; } LinkJob;
typedef struct { Host *host; PureEffect effect; Route route; Waiter *waiter; LinkJob *link; } Job;

/* The owner of an app with states served by several connection threads:
 * their requests wait here, and a worker runs them in bursts, one save each
 * however many threads they came from, handing each thread back its
 * applied requests to answer (and a surface its events). */
typedef struct Owner {
    Executor executor;
    pthread_mutex_t lock;
    pthread_cond_t waiting;
    bool made;      /* its locks, made once (its app may start again) */
    Published published;
    /* Under lock: the module its executor runs (NULL: none). A request
     * resolved with another is not taken (503 stopped). */
    const PureModule *module;
    Job *jobs;
    size_t count, capacity;
    uint64_t arrivals; /* requests handed to the owner, ever */
    bool stopping;
    bool delivered; /* a wire delivered a send: trim it */
    Host *hosts;
    unsigned host_count;
    /* Its turns on the workers (docs/design/017-apps-on-one-host.md): queued
     * with work to do, or running on one; again when work came while it ran;
     * due when its next timer is (the monotonic clock in ms, 0: none). */
    struct Owner *next;
    bool live; /* it takes turns: its app runs */
    bool queued, running, again;
    uint64_t due, turns;
    size_t queue; /* how many requests may wait for its turn (0: any number) */
    uint64_t busy; /* requests refused for that */
    /* Between its turns: whether the last one saved (it then gathers the
     * clients it answered), the arrivals when it handed them back, and the
     * jobs' spare array. */
    bool gather;
    uint64_t answered_at;
    Job *spare;
    size_t spare_capacity;
} Owner;

/* An app of the host (docs/design/017-apps-on-one-host.md): its module,
 * store and budgets, and the owner that applies its events on the workers,
 * taking turns with the host's other apps. The one app of a host started
 * with --module has no name (its routes start /views/ and so on); each of
 * --apps has its own, the first segment of its routes. */
enum { APP_NAME = 32 };
/* What an app did, added up over its starts (its executor's are its present run's). */
typedef struct {
    uint64_t bursts, events, ticks, memory_refused, windows_full, publications, answers, handed, checkpoints, delivered,
        attempts;
    PureLinkCounts sent;
} Counts;
struct App {
    char name[APP_NAME + 1]; /* never changed once the app is in its place: threads find it by name */
    PureModule *module; /* the module it runs, or ran or was given last (NULL: none loaded) */
    /* The module its requests are resolved with, any thread's to read: NULL
     * while it does not run (its routes answer 503 stopped, 404 no_app once
     * gone). A module an app ran is kept as long as the host runs: requests
     * and snapshots may still point to it. */
    _Atomic(const PureModule *) live;
    _Atomic bool gone;
    char *module_path; /* its module's file as its line gives it (a host of apps), and that file's digest */
    uint64_t digest;
    /* Its tokens (NULL: its routes are open), from tokens_path (its file's digest when read). */
    _Atomic(const Tokens *) tokens;
    char *tokens_path;
    uint64_t tokens_digest;
    const char *store;
    bool documents; /* the module serves /objects: it has show and edit */
    bool owned;     /* an owner runs it (states or outputs, or any of --apps); else the connection threads */
    uint32_t show, edit;
    _Atomic size_t memory; /* its budgets (0: none): what its states hold (read by its views' readers too), */
    _Atomic uint64_t work; /* what an event or a view's evaluation may run */
    size_t queue;   /* the requests that may wait for its turn, and its views' with the view workers */
    _Atomic size_t viewing;
    bool running;   /* started (the main thread's word); else why says why */
    char why[256];
    unsigned home;  /* the connection thread its connections move to (a host of apps) */
    /* Its wiring in --apps (send=, seal=, reply=), as --send, --seal and --reply give the one app's. */
    const char *sends[PURE_MODULE_INPUTS], *seals[PURE_MODULE_INPUTS], *replies[PURE_MODULE_INPUTS];
    unsigned send_count, seal_count, reply_count;
    Counts past; /* its runs' before the present one */
    Owner owner;
};
static App *apps;
/* The places taken: an app's place is its own, never another name's, so a
 * thread finds an app by name while others start and stop (docs/design/017's
 * stage 3). app_slots is how many there are, made when the host starts. */
static _Atomic unsigned app_count;
static unsigned app_slots = 1;
static unsigned places(void) { return atomic_load_explicit(&app_count, memory_order_acquire); }
/* The modules apps ran before the one they run, kept as long as the host runs. */
static struct Retired { PureModule *module; struct Retired *next; } *retired_modules;
static bool named_apps; /* --apps: a route's first segment names its app */
static Host *all_hosts;  /* the connection threads, for moving a connection to its app's home */
static unsigned host_count;
static bool moved_home(Connection *c, size_t head_size);
static void schedule(Owner *owner);
static void subscribe(Connection *c, const Route *route, const char *raw, size_t raw_size);
static void wake_pushers(void);

struct Host {
    Executor local;   /* the one app of the host, when this thread runs it (documents alone) */
    PureApp *readers; /* by app: shows its owner's snapshots here */
    PureSnapshot **snapshots; /* by app: the snapshot a round reads views from, once */
    uint64_t round, *read_in, *handed_in; /* by app: the round it was read in, its requests handed in */
    unsigned *touched; /* the apps read in this round */
    Applied *views; /* a round's views shown here, answered together */
    size_t views_capacity;
    uint64_t shown, pooled; /* views shown from snapshots; of them, by the view workers */
    Route *routes;  /* by work slot */
    struct Given { char *text; size_t capacity; } *given; /* a view's arguments as asked, by work slot */
    PureRuntime runtime;
    PureWork *work;
    Connection **table;   /* open connections by index */
    Connection **by_slot; /* the connection waiting on a work slot */
    Connection *spare, *closed;
    Job *jobs; /* a round's requests: at most one per work slot */
    size_t capacity, active;
    int epoll, listener;
    uint64_t timeout_ms;
    uint64_t now; /* the monotonic clock in ms, read once a round: deadlines count from it */
    uint64_t accepted, requests, expired, moved_away, unauthorized, forbidden;
    Session sessions[SESSIONS];
    uint64_t session_uses;
    /* What the owner hands back to answer (swapped for answering), and the
     * eventfd that says so; pending counts requests at the owner. */
    pthread_mutex_t lock;
    Applied *done, *answering;
    size_t done_count, done_capacity, answering_capacity, pending;
    int wake;
    /* Connections other threads moved here, their first request's bytes
     * read already (under lock, woken by wake too). */
    struct Moved { int fd; bool hung_up; size_t used; char in[HEADER_LIMIT]; } *moved;
    size_t moved_count, moved_capacity;
    /* Its views' subscribers (docs/design/019), in groups that share an
     * evaluation; push_due: an owner published since its last pass. */
    struct Group *groups;
    size_t subscribers, subscribers_at_stop;
    _Atomic bool push_due;
    bool push_waiting;  /* a pass is due but --push-every-ms has not passed since the last */
    uint64_t pushed_at; /* when the last pass ran (ms) */
    uint64_t pushes, pushed_events, replaced, subscribed, unsubscribed;
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

/* An answer's head is written in place, part by part: printf's scanning of
 * its format took a sixth of a small read's time. Clipped at the end of the
 * buffer, which its parts, all short, never reach. */
typedef struct { char *at, *end; } Head;

static void put_text(Head *h, const char *text) {
    size_t length = strlen(text), room = (size_t)(h->end - h->at);
    if (length > room) length = room;
    memcpy(h->at, text, length);
    h->at += length;
}

static void put_decimal(Head *h, uint64_t value) {
    char digits[20];
    size_t count = 0;
    do digits[count++] = (char)('0' + value % 10); while (value /= 10);
    while (count && h->at < h->end) *h->at++ = digits[--count];
}

/* Answer with a body of `count` extents (`size` bytes in all); the ticket
 * keeps them alive. One writev sends as much as the socket takes. */
static void respond_typed(Connection *c, int status, const char *reason, const char *error, int64_t revision,
                          const char *type, const struct iovec *extents, size_t count, size_t size) {
    Head h = {c->head, c->head + sizeof(c->head)};
    put_text(&h, "HTTP/1.1 ");
    put_decimal(&h, (uint64_t)status);
    put_text(&h, " ");
    put_text(&h, reason);
    put_text(&h, "\r\nContent-Length: ");
    put_decimal(&h, size);
    put_text(&h, c->keep_alive ? "\r\n" : "\r\nConnection: close\r\n");
    put_text(&h, "Content-Type: ");
    put_text(&h, type);
    put_text(&h, "\r\nCache-Control: no-store\r\nX-Pure-Error: ");
    put_text(&h, error);
    put_text(&h, status == 401 ? "\r\nWWW-Authenticate: Bearer\r\n" : "\r\n");
    if (revision > 0) {
        put_text(&h, "ETag: \"");
        put_decimal(&h, (uint64_t)revision);
        put_text(&h, "\"\r\n");
    }
    /* A delivery's answer tells how many numbered messages one request may carry. */
    if (*c->source) {
        put_text(&h, "X-Pure-Batch: ");
        put_decimal(&h, BATCH);
        put_text(&h, "\r\n");
    }
    put_text(&h, "\r\n");
    size_t length = (size_t)(h.at - c->head);
    c->out = count < sizeof(c->small) / sizeof(*c->small) ? c->small : malloc((count + 1) * sizeof(*c->out));
    c->phase = WRITING;
    if (!c->out) { c->keep_alive = false; c->out = c->small; count = 0; } /* cannot send it: the close tells */
    c->out[0] = (struct iovec){c->head, length};
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

/* An app route's answer: a view's value, or what an event replied. A failed
 * event names its fault: 404 not_found, 409 exists, 503 memory (the app's
 * budget: again later), 422 for the others; a view without a value is 404
 * no_value. */
static void answer_app(Connection *c, PureOutcome result, bool view, PureFault fault, const char *type) {
    const char *named = fault != PURE_FAULT_NONE ? pure_fault_name(fault) : NULL;
    if (result.status == PURE_OK) {
        struct iovec extent = {(void *)result.body.data, result.body.size};
        respond_typed(c, 200, "OK", "none", 0, type ? type : "application/octet-stream", &extent,
                      result.body.size ? 1 : 0, result.body.size);
    } else if (result.status == PURE_NOT_FOUND) {
        respond(c, 404, "Not Found", view ? "no_value" : named ? named : "not_found", 0, (PureSpan){0});
    } else if (result.status == PURE_CONFLICT) {
        respond(c, 409, "Conflict", named ? named : "conflict", 0, (PureSpan){0});
    } else if (fault == PURE_FAULT_MEMORY) {
        respond(c, 503, "Service Unavailable", named, 0, (PureSpan){0});
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

/* "SUITE ENC N[ FROM]": the keys as 64 hex digits, the number in decimal. */
static bool read_seal(const char *value, Seal *seal) {
    char copy[256], *rest, *suite = NULL, *enc = NULL, *n = NULL, *from = NULL, *extra = NULL;
    if (strlen(value) >= sizeof(copy)) return false;
    strcpy(copy, value);
    suite = strtok_r(copy, " ", &rest);
    if (suite) enc = strtok_r(NULL, " ", &rest);
    if (enc) n = strtok_r(NULL, " ", &rest);
    if (n) from = strtok_r(NULL, " ", &rest);
    if (from) extra = strtok_r(NULL, " ", &rest);
    uint64_t sequence;
    if (!n || extra || strlen(suite) >= sizeof(seal->suite) || !pure_seal_peer(enc, seal->enc) ||
        !number(n, UINT64_MAX, &sequence) || (from && !pure_seal_peer(from, seal->from)))
        return false;
    strcpy(seal->suite, suite);
    seal->n = sequence;
    seal->authenticated = from != NULL;
    return true;
}

/* A sender this node names, sealing with the key it was named with. */
static bool known_sender(const char *source, const Seal *seal) {
    for (unsigned i = 0; i < sender_count; ++i)
        if (!strcmp(senders[i].name, source))
            return seal->authenticated && !memcmp(senders[i].key, seal->from, PURE_SEAL_KEY);
    return false;
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
    /* A host of apps (--apps): the path's first segment names one, and the rest routes in it. */
    c->app = named_apps ? NULL : &apps[0];
    if (named_apps && *path == '/') {
        const char *end = strchr(path + 1, '/');
        size_t length = end ? (size_t)(end - path - 1) : 0;
        for (unsigned i = 0, count = places(); end && i < count && !c->app; ++i)
            if (strlen(apps[i].name) == length && !memcmp(apps[i].name, path + 1, length)) c->app = &apps[i];
        if (end) path += 1 + length;
    }
    static const struct { const char *prefix; RouteKind kind; } routes[] = {
        {"/objects/", ROUTE_OBJECT}, {"/views/", ROUTE_VIEW}, {"/inputs/", ROUTE_INPUT}, {"/queries/", ROUTE_QUERY}};
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
    /* Each route has its method: documents GET and PUT, views GET, inputs
     * POST, queries PUT (registered) and GET (shown). */
    if ((c->route == ROUTE_OBJECT && (c->post || *query)) || (c->route == ROUTE_VIEW && (c->put || c->post)) ||
        (c->route == ROUTE_INPUT && !c->post) || (c->route == ROUTE_QUERY && (c->post || (c->put && *query))))
        return false;
    bool length_seen = false, condition_seen = false, host_seen = false;
    c->source[0] = 0;
    c->sequence = 0;
    c->batch = 1;
    c->seal.suite[0] = 0;
    c->bearer_given = false;
    c->events = false;
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
        } else if (!strcasecmp(line, "Accept")) {
            c->events = strcasestr(value, "text/event-stream") != NULL; /* a view's stream (docs/design/019) */
        } else if (!strcasecmp(line, "Authorization")) {
            size_t length = strlen(value);
            if (c->bearer_given || length < 8 || strncasecmp(value, "Bearer ", 7) || length - 7 > BEARER) return false;
            memcpy(c->bearer, value + 7, length - 6);
            c->bearer_size = length - 7;
            c->bearer_given = true;
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
            /* A delivery sealed to this node's key: suite, session, number[, sender]. */
            if (*c->seal.suite || !read_seal(value, &c->seal)) return false;
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
    /* A document's PUT names the revision it was made against; a query's replaces what was registered. */
    if (c->put) return host_seen && length_seen && condition_seen == (c->route != ROUTE_QUERY);
    if (c->post) return host_seen && !condition_seen; /* no Content-Length: no body */
    return host_seen && !condition_seen && c->body_size == 0;
}

static void put_u32(unsigned char *p, uint32_t v) { for (unsigned i = 0; i < 4; ++i) p[i] = (unsigned char)(v >> (8 * i)); }
static void put_u64(unsigned char *p, uint64_t v) { for (unsigned i = 0; i < 8; ++i) p[i] = (unsigned char)(v >> (8 * i)); }
static uint32_t get_u32(const unsigned char *p) { return p[0] | p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t get_u64(const unsigned char *p) { return get_u32(p) | (uint64_t)get_u32(p + 4) << 32; }

static int hex_value(char c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* %XX decoded in place (never longer): the length after, or -1 for a bad escape. */
static long percent_decoded(char *text, size_t length) {
    size_t o = 0;
    for (size_t i = 0; i < length; ++i) {
        if (text[i] != '%') { text[o++] = text[i]; continue; }
        int high = i + 2 < length ? hex_value(text[i + 1]) : -1, low = high >= 0 ? hex_value(text[i + 2]) : -1;
        if (low < 0) return -1;
        text[o++] = (char)(high << 4 | low);
        i += 2;
    }
    return (long)o;
}

/* What an app route names in the module, with a message's fixed fields
 * taken from the query (%XX decoded, as a view's arguments are) into
 * c->fields. NULL, or the error to answer (404 for a name the module lacks,
 * 400 otherwise). */
static const char *resolve(Connection *c, char *query, Route *route, int *status) {
    App *app = c->app;
    *status = 404;
    c->field_bytes = 0;
    memset(c->given, 0, sizeof(c->given));
    if (!app) return "no_app";
    const PureModule *m = atomic_load_explicit(&app->live, memory_order_acquire);
    if (!m && atomic_load_explicit(&app->gone, memory_order_relaxed)) return "no_app";
    if (!m) {
        *status = 503;
        return "stopped";
    }
    route->app = app;
    route->module = c->module = m;
    if (route->kind == ROUTE_NONE || (route->kind == ROUTE_OBJECT && !m->collections) ||
        (route->kind == ROUTE_QUERY && !app->owned)) return "route"; /* a query reads an app's states */
    if (route->kind == ROUTE_OBJECT) return NULL;
    if (route->kind == ROUTE_QUERY) { /* the view is the query module's, found when shown */
        route->from = 0;
        route->count = SIZE_MAX;
        *status = 400;
        for (char *pair = query; pair && *pair;) {
            char *end = strchr(pair, '&'), *value = strchr(pair, '=');
            if (end) *end = 0;
            uint64_t n;
            if (!value || (end && value > end)) return "query";
            *value++ = 0;
            bool from = !strcmp(pair, "from"), paging = from || !strcmp(pair, "count");
            if (paging && !number(value, SIZE_MAX, &n)) return "query";
            if (!paging) route->argued = true; /* the query's view reads it, if it takes arguments */
            else if (from) route->from = (size_t)n;
            else route->count = (size_t)n;
            pair = end ? end + 1 : NULL;
        }
        return NULL;
    }
    int found = route->kind == ROUTE_VIEW ? pure_module_view(m, c->key) : pure_module_input(m, c->key);
    /* An output is the app's to send and a timer ticks by itself: neither is posted. */
    if (found >= 0 && route->kind == ROUTE_INPUT &&
        (m->input[found].kind == PURE_KIND_OUTPUT || m->input[found].kind == PURE_KIND_TIMER ||
         m->input[found].kind == PURE_KIND_ARGUMENTS)) found = -1;
    if (found < 0) return route->kind == ROUTE_VIEW ? "no_view" : "no_input";
    *status = 400;
    route->target = (uint32_t)found;
    route->from = 0;
    route->count = SIZE_MAX;
    const PureInputShape *shape = route->kind == ROUTE_INPUT ? &m->input[found] : NULL;
    bool message = shape && shape->kind == PURE_KIND_MESSAGE;
    if (message && shape->keyed) return "keyed";
    if (*c->source && !message) return "delivery";
    if (!app->owned) c->source[0] = 0; /* an app without states or outputs applies it as a message */
    if (shape) {
        /* A body is a text input's text, a message's one variable field (rows
         * in their list's stored form), or a JSON object of a message's fields
         * (two variable fields need one). */
        unsigned variables = 0;
        for (uint32_t f = 0; message && f < shape->field_count; ++f)
            variables += m->field[shape->field_start + f].type == PURE_FIELD_BYTES ||
                         m->field[shape->field_start + f].type == PURE_FIELD_TEXT ||
                         m->field[shape->field_start + f].type == PURE_FIELD_ROWS;
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
            bool from = !strcmp(pair, "from"), paging = from || !strcmp(pair, "count");
            if (!paging && m->view_arguments[found]) route->argued = true; /* an argument, read when shown */
            else if (!number(value, SIZE_MAX, &n) || m->view_type[found] != PURE_VIEW_TABLE || !paging) return "query";
            else if (from) route->from = (size_t)n;
            else route->count = (size_t)n;
        } else {
            uint32_t f = 0;
            while (f < (message ? shape->field_count : 0) && strcmp(m->field[shape->field_start + f].name, pair)) ++f;
            const PureField *field = message && f < shape->field_count ? &m->field[shape->field_start + f] : NULL;
            long length = percent_decoded(value, strlen(value)); /* as a view's arguments are */
            if (!field || c->given[f] || !pure_field_fixed(field->type) ||
                length < 0 || !pure_json_fixed(field, (const unsigned char *)value, (size_t)length, c->fields))
                return "query";
            c->given[f] = true;
        }
        pair = end ? end + 1 : NULL;
    }
    for (uint32_t f = 0; message && !c->json && f < shape->field_count; ++f) {
        uint8_t type = m->field[shape->field_start + f].type;
        if (pure_field_fixed(type) && !c->given[f]) return "missing_field";
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
/* With DELIVERED: the batch's messages are records a link delivered, each
 * as stored (u32le size, then its bytes), applied in order from the one
 * after the source's last heard number, stopping at a gap. */
#define STORED_RECORDS (UINT32_C(1) << 29)
enum { DELIVERY = 80 };
/* An app input's JSON body becomes its message: an object with a member for
 * each field the query did not give (a u32, a number, or a string for bytes
 * and text), groups as objects inside it, and nothing else. Fixed fields go
 * into the prefix; strings stay in the body, and the table says where.
 * NULL, or the error to answer. */
static const char *convert(Connection *c) {
    const PureInputShape *shape = &c->module->input[c->input];
    uint32_t delivered = get_u32(c->ingress) & DELIVERED;
    size_t table = 8 * (size_t)shape->field_count, payload = c->batch * c->field_bytes + c->body_size;
    unsigned char *tables = c->ingress + 4 + (delivered ? DELIVERY : 0), *prefixes = tables + c->batch * table;
    unsigned char *json = c->body;
    size_t size = c->body_size, at = pure_json_space(json, 0, size);
    /* A message's rows go after the body: they are encoded, then placed. */
    PureJsonMessage b = {.module = c->module, .shape = shape, .given = c->given, .rows_base = payload};
    const char *why = NULL;
    /* A batch is an array of its messages' objects. */
    if (c->batch > 1) {
        if (at == size || json[at++] != '[') return "json";
        at = pure_json_space(json, at, size);
    }
    for (unsigned i = 0; i < c->batch && !why; ++i) {
        if (i && (at == size || json[at++] != ',')) why = "json";
        at = pure_json_space(json, at, size);
        b.table = tables + i * table;
        b.payload = prefixes + i * c->field_bytes;
        memset(b.seen, 0, sizeof(b.seen));
        if (!why) why = pure_json_message(&b, json, size, &at);
        at = pure_json_space(json, at, size);
        /* A delivery from another node may lack fields its sender's module has
         * not (docs/design/014-fields-by-name.md): they are 0 or empty. A
         * member naming no field is refused all the same: it would be dropped. */
        for (uint32_t f = 0; !why && f < shape->field_count; ++f) {
            if (b.seen[f] || c->given[f]) continue;
            const PureField *field = &c->module->field[shape->field_start + f];
            if (!delivered || field->type == PURE_FIELD_ROWS) why = "missing_field";
            else if (pure_field_fixed(field->type))
                memset(b.payload + field->offset, 0, pure_field_size(field->type));
            else put_u32(b.table + 8 * f, 0), put_u32(b.table + 8 * f + 4, 0);
        }
        if (!why && c->batch > 1 && b.rows_size) why = "delivery"; /* an output sends no rows */
    }
    if (!why && c->batch > 1 && (at == size || json[at++] != ']')) why = "json";
    if (!why && pure_json_space(json, at, size) != size) why = "json";
    if (!why && b.rows_size) {
        size_t body = (size_t)(c->body - c->ingress);
        PureResult grown = pure_runtime_grow(c->ticket, body + c->body_size + b.rows_size);
        if (grown != PURE_OK) why = grown == PURE_BUDGET ? "payload_budget" : "memory";
        else {
            c->ingress = pure_runtime_ingress(c->ticket, NULL);
            c->body = c->ingress + body;
            memcpy(c->body + c->body_size, b.rows, b.rows_size);
        }
    }
    free(b.rows);
    if (why) return why;
    put_u32(c->ingress, (uint32_t)(payload + b.rows_size) | PLACED | delivered);
    return NULL;
}

/* The session a seal was made in: one this thread opened before, or opened
 * now in place of the least used; NULL when it does not open. */
static PureOpener *session(Host *host, const Seal *seal) {
    Session *least = &host->sessions[0];
    for (unsigned i = 0; i < SESSIONS; ++i) {
        Session *k = &host->sessions[i];
        if (k->opener && !memcmp(k->seal.enc, seal->enc, PURE_SEAL_KEY) && !strcmp(k->seal.suite, seal->suite) &&
            k->seal.authenticated == seal->authenticated &&
            (!seal->authenticated || !memcmp(k->seal.from, seal->from, PURE_SEAL_KEY))) {
            k->used = ++host->session_uses;
            return k->opener;
        }
        if (k->used < least->used) least = k;
    }
    PureOpener *opener = pure_opener(seal_key, seal->suite, seal->enc, seal->authenticated ? seal->from : NULL);
    if (!opener) return NULL;
    pure_opener_free(least->opener);
    *least = (Session){*seal, opener, ++host->session_uses};
    return opener;
}

/* A sealed body opened where it lies, or false (another key, another
 * session or sender, another delivery's binding, or changed on the way). */
static bool open_seal(Connection *c) {
    if (c->body_size < PURE_SEAL_OVERHEAD) return false;
    PureOpener *opener = session(c->host, &c->seal);
    size_t size = c->body_size - PURE_SEAL_OVERHEAD;
    unsigned char *plain = opener ? malloc(size ? size : 1) : NULL;
    bool opened = plain && pure_unseal(opener, c->seal.n, c->aad, c->body, c->body_size, plain);
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
    if (*c->seal.suite && !open_seal(c)) {
        release_ticket(c); /* never made ready: it never runs */
        respond(c, 400, "Bad Request", "seal", 0, (PureSpan){0});
        return;
    }
    const char *why = c->route == ROUTE_INPUT && c->json ? convert(c) : NULL;
    if (why) {
        release_ticket(c); /* never made ready: it never runs */
        if (!strcmp(why, "payload_budget") || !strcmp(why, "memory")) respond(c, 503, "Service Unavailable", why, 0, (PureSpan){0});
        else respond(c, 400, "Bad Request", why, 0, (PureSpan){0});
        return;
    }
    if (!pure_runtime_ready(c->ticket)) abort();
    c->phase = WAITING;
}

/* Whether a request may go its route (docs/design/017's 3b): 0, or 401 (no
 * token, or one the app does not know) or 403 (one that does not grant
 * it). An app without tokens takes any. A delivery another node sealed
 * with a key this node names needs none: its key said who sent it. The
 * digests are compared in constant time, all of them. */
static int authorize(Connection *c, const App *app, RouteKind kind) {
    const Tokens *tokens = atomic_load_explicit(&app->tokens, memory_order_acquire);
    if (!tokens) return 0;
    if (kind == ROUTE_INPUT && *c->source && *c->seal.suite && sender_count) return 0; /* known_sender held */
    if (!c->bearer_given) return 401;
    if (!c->hashed_valid || c->hashed_size != c->bearer_size || memcmp(c->hashed, c->bearer, c->bearer_size)) {
        pure_sha256(c->bearer, c->bearer_size, c->hashed_digest);
        memcpy(c->hashed, c->bearer, c->bearer_size);
        c->hashed_size = c->bearer_size;
        c->hashed_valid = true;
    }
    int found = -1;
    for (unsigned g = 0; g < tokens->count; ++g) {
        unsigned char differ = 0;
        for (unsigned i = 0; i < 32; ++i) differ |= (unsigned char)(tokens->grant[g].digest[i] ^ c->hashed_digest[i]);
        if (!differ) found = (int)g;
    }
    if (found < 0) return 401;
    const struct Grant *grant = &tokens->grant[found];
    bool write = kind == ROUTE_INPUT || c->put;
    if (grant->kind == GRANT_WRITE || (grant->kind == GRANT_READ && !write)) return 0;
    if (grant->kind != GRANT_INPUT || kind != ROUTE_INPUT) return 403;
    size_t length = strlen(c->key);
    for (const char *at = grant->inputs; *at;) { /* name,name,... */
        size_t part = strcspn(at, ",");
        if (part == length && !memcmp(at, c->key, length)) return 0;
        at += part + (at[part] == ',');
    }
    return 403;
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
    /* The query as it came: resolving takes it apart, and a view's arguments are read from it when shown. */
    char raw[HEADER_LIMIT];
    size_t raw_size = query ? strlen(query) : 0;
    if (raw_size) memcpy(raw, query, raw_size + 1);
    /* A node that keeps its deliveries sealed refuses the others, and one
     * that names its senders refuses any other sender; a seal needs this
     * node's key, and binds the delivery's source, number and count. */
    if (*c->source && (sealed_only || sender_count) && !*c->seal.suite) { refuse(c, 403, "Forbidden", "unsealed"); return; }
    if (*c->seal.suite) {
        if (!*c->source || !seal_key || !c->json) { refuse(c, 400, "Bad Request", "seal"); return; }
        if (sender_count && !known_sender(c->source, &c->seal)) { refuse(c, 403, "Forbidden", "sender"); return; }
        snprintf(c->aad, sizeof(c->aad), "%s %llu %u", c->source, (unsigned long long)c->sequence, c->batch);
    }
    if (c->body_size > PURE_OBJECT_LIMIT) { refuse(c, 413, "Content Too Large", "limit"); return; }
    ++host->requests;
    Route route = {.kind = c->route};
    int status;
    const char *why = resolve(c, query, &route, &status);
    if (why) {
        reject(c, status, status == 404 ? "Not Found" : status == 503 ? "Service Unavailable" : "Bad Request", why);
        return;
    }
    int denied = authorize(c, c->app, route.kind);
    if (denied) {
        host->unauthorized += denied == 401, host->forbidden += denied == 403;
        reject(c, denied, denied == 401 ? "Unauthorized" : "Forbidden", denied == 401 ? "unauthorized" : "forbidden");
        return;
    }
    /* A view's stream (docs/design/019): pushed from snapshots, so an owned app's. */
    if (c->events && route.kind == ROUTE_VIEW && route.app->owned) { subscribe(c, &route, raw, raw_size); return; }
    /* A document's PUT carries its body; an input's event its payload's size,
     * the payload (the fixed fields, then the body), and room for its JSON. */
    bool write = route.kind == ROUTE_INPUT || c->put;
    size_t table = route.kind == ROUTE_INPUT && c->json ? 8 * (size_t)route.module->input[c->input].field_count : 0;
    size_t lead = 4 + (route.kind == ROUTE_INPUT && *c->source ? DELIVERY : 0);
    /* A batch has a table and a prefix for each of its messages. */
    size_t input_size = route.kind == ROUTE_INPUT ? lead + c->batch * (table + c->field_bytes) + c->body_size :
                        write ? c->body_size : 0;
    PureResult admitted = pure_runtime_accept(&host->runtime, write ? PURE_INPUT_WRITE : PURE_INPUT_READ, c->key,
                                              route.kind == ROUTE_INPUT || route.kind == ROUTE_QUERY ? 0 : c->expected,
                                              input_size, &c->ticket);
    if (admitted != PURE_OK) {
        c->ticket = (PureTicket){0};
        /* An unread body would desynchronize the stream. */
        if (c->body_size) c->keep_alive = false;
        answer(c, (PureOutcome){.status = admitted});
        return;
    }
    host->by_slot[c->ticket.slot] = c;
    if (route.argued) {
        struct Given *given = &host->given[c->ticket.slot];
        if (given->capacity <= raw_size) {
            char *grown = realloc(given->text, raw_size + 1);
            if (!grown) abort();
            given->text = grown;
            given->capacity = raw_size + 1;
        }
        memcpy(given->text, raw, raw_size + 1);
        route.given = given->text;
    }
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
            if (end && !c->settled && named_apps && all_hosts && moved_home(c, end)) return;
            if (end) { start_request(c, end); continue; }
            if (c->in_used == HEADER_LIMIT) { refuse(c, 431, "Request Header Fields Too Large", "headers"); continue; }
            if (!c->readable) return;
            size_t room = HEADER_LIMIT - c->in_used;
            ssize_t n = recv(c->fd, c->in + c->in_used, room, 0);
            if (n > 0) {
                c->in_used += (size_t)n;
                c->deadline = c->host->now + c->host->timeout_ms;
                /* A short read emptied the socket; the edge-triggered epoll
                 * tells when more comes. Not asking again spares a recv that
                 * would fail (EAGAIN) after every request. */
                if ((size_t)n < room && !c->hung_up) c->readable = false;
                continue;
            }
            if (n < 0 && errno == EAGAIN) { c->readable = false; return; }
            if (n < 0 && errno == EINTR) continue;
            close_connection(c);
            return;
        }
        if (c->phase == READING_BODY) {
            if (!c->readable) return;
            size_t room = c->body_size - c->received;
            ssize_t n = recv(c->fd, c->body + c->received, room, 0);
            if (n > 0) {
                c->received += (size_t)n;
                c->deadline = c->host->now + c->host->timeout_ms;
                if ((size_t)n < room && !c->hung_up) c->readable = false;
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
            c->deadline = c->host->now + c->host->timeout_ms; /* a slow reader is fine while it reads */
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
            c->deadline = c->host->now + c->host->timeout_ms;
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

/* An i64's every digit (a JSON number too: a reader keeping binary64s loses those past 2^53). */
static void put_integer(Output *o, int64_t value) {
    char digits[24];
    put_bytes(o, digits, (size_t)snprintf(digits, sizeof(digits), "%" PRId64, value));
}

/* A view's arguments from a query's name=value pairs (values %XX decoded;
 * from and count page a table): a payload of their input's shape, the
 * fixed fields in its prefix, the texts and bytes after it in order, each's
 * length in the prefix but the last's. NULL for a pair naming no field or
 * given twice, a fixed field missing or not of its type, or no memory. */
static unsigned char *arguments_payload(const PureModule *m, int input, const char *given, size_t *size) {
    const PureInputShape *shape = &m->input[input];
    size_t length = given ? strlen(given) : 0;
    char *copy = malloc(length + 1);
    unsigned char *payload = malloc(shape->prefix + length + 1);
    const char *values[PURE_MODULE_FIELDS] = {0};
    long lengths[PURE_MODULE_FIELDS] = {0};
    bool ok = copy && payload;
    if (ok) {
        memcpy(copy, given ? given : "", length + 1);
        memset(payload, 0, shape->prefix);
    }
    for (char *pair = copy; ok && pair && *pair;) {
        char *end = strchr(pair, '&'), *value = strchr(pair, '=');
        if (end) *end = 0;
        if (!value || (end && value > end)) { ok = false; break; }
        *value++ = 0;
        uint32_t f = 0;
        while (f < shape->field_count && strcmp(m->field[shape->field_start + f].name, pair)) ++f;
        if (f == shape->field_count) ok = !strcmp(pair, "from") || !strcmp(pair, "count");
        else if (values[f]) ok = false;
        else if ((lengths[f] = percent_decoded(value, strlen(value))) < 0) ok = false;
        else values[f] = value;
        pair = end ? end + 1 : NULL;
    }
    size_t at = shape->prefix;
    for (uint32_t f = 0; ok && f < shape->field_count; ++f) {
        const PureField *field = &m->field[shape->field_start + f];
        if (pure_field_fixed(field->type)) {
            ok = values[f] && pure_json_fixed(field, (const unsigned char *)values[f], (size_t)lengths[f], payload);
            continue;
        }
        if (field->offset != PURE_FIELD_REST) put_u32(payload + field->offset, (uint32_t)lengths[f]);
        if (lengths[f]) memcpy(payload + at, values[f], (size_t)lengths[f]);
        at += (size_t)lengths[f];
    }
    free(copy);
    if (!ok) {
        free(payload);
        return NULL;
    }
    *size = at;
    return payload;
}

static PureResult show_given(PureApp *app, const Route *route, size_t available, Applied *answer);

/* A view shown with its arguments, when it takes some (they are the request's to give). */
static PureResult show_view(PureApp *app, const Route *route, size_t available, Applied *answer) {
    int input = pure_module_arguments(app->module, route->target);
    if (input < 0) return route->argued ? PURE_INVALID : show_given(app, route, available, answer);
    size_t size;
    unsigned char *payload = arguments_payload(app->module, input, route->given, &size);
    if (!payload) return PURE_INVALID;
    PureResult given = pure_app_arguments(app, route->target, (PureSpan){payload, size}, NULL);
    PureResult status = given == PURE_OK ? show_given(app, route, available, answer) : PURE_INVALID;
    free(payload);
    return status;
}

/* A view's value: a number in Pure's shortest form or a text, as text; a
 * table as {"rows":N,"from":F,"items":[{column: value}...]} for the rows
 * asked, each cell computed only for them (a cell without a value is null). */
static PureResult show_given(PureApp *app, const Route *route, size_t available, Applied *answer) {
    const PureModule *m = app->module;
    uint32_t view = route->target;
    PureResource *out = &answer->resource;
    const char **type = &answer->type;
    /* Room for a page of short rows at once. */
    Output o = {0};
    PureResult status = PURE_OK;
    double value;
    if (m->view_type[view] == PURE_VIEW_TABLE && !(o.bytes = malloc(o.capacity = 1024))) return PURE_ALLOCATION;
    if (m->view_type[view] == PURE_VIEW_I64) {
        int64_t integer;
        *type = "text/plain; charset=utf-8";
        if (!pure_app_view_i64(app, view, &integer)) status = PURE_NOT_FOUND;
        else put_integer(&o, integer);
    } else if (m->view_type[view] == PURE_VIEW_NUM || m->view_type[view] == PURE_VIEW_BOOL) {
        *type = "text/plain; charset=utf-8";
        if (!pure_app_view(app, view, &value)) status = PURE_NOT_FOUND;
        else if (m->view_type[view] == PURE_VIEW_BOOL) put_bytes(&o, value ? "true" : "false", value ? 4 : 5);
        else put_number(&o, value);
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
        if (pure_app_view_fault(app) == PURE_FAULT_WORK) status = PURE_LIMIT, rows = from = 0; /* its filter's */
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
            put_bytes(&o, row == from ? "{" : ",{", row == from ? 1 : 2);
            for (unsigned k = 0; k < count; ++k) {
                put_bytes(&o, columns[k].key + !k, columns[k].length - !k);
                uint8_t kind = m->view_type[columns[k].view];
                bool valued;
                if (kind == PURE_VIEW_I64) {
                    int64_t integer;
                    if ((valued = pure_app_cell_i64(app, columns[k].view, row, &integer))) put_integer(&o, integer);
                } else if (kind == PURE_VIEW_NUM || kind == PURE_VIEW_BOOL) {
                    if (!(valued = pure_app_cell(app, columns[k].view, row, &value))) {
                    } else if (kind == PURE_VIEW_BOOL) put_bytes(&o, value ? "true" : "false", value ? 4 : 5);
                    else put_number(&o, value);
                } else {
                    size_t mark = o.size;
                    put_bytes(&o, "\"", 1);
                    if ((valued = pure_app_cell_text(app, columns[k].view, row, put_json_span, &o) == PURE_OK)) put_bytes(&o, "\"", 1);
                    else o.size = o.failed ? o.size : mark;
                }
                if (valued) continue;
                if (pure_app_view_fault(app) == PURE_FAULT_WORK) { /* a cell past the work budget fails the table */
                    status = PURE_LIMIT;
                    end = row;
                    break;
                }
                put_bytes(&o, "null", 4); /* a cell without a value */
            }
            put_bytes(&o, "}", 1);
        }
        put_bytes(&o, "]}", 2);
    }
    if (status != PURE_OK && pure_app_view_fault(app) == PURE_FAULT_WORK) { /* past the work budget: 422 work */
        answer->fault = PURE_FAULT_WORK;
        status = PURE_LIMIT;
    } else if (status != PURE_OK && pure_app_view_fault(app) == PURE_FAULT_MEMORY) { /* a body's lists past it: 503 memory */
        answer->fault = PURE_FAULT_MEMORY;
        status = PURE_BUDGET;
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

/* The app refused a message it was given (a fault of its own): it was
 * delivered, and is heard. Refused for memory it was not. */
static bool app_refused(const PureApp *app) { return app->fault != PURE_FAULT_NONE && app->fault != PURE_FAULT_MEMORY; }

/* An input's event: a text, a key pressed (down, then up), or a message
 * whose reply (a field of it) is borrowed from the request. */
static PureResult apply_input(PureApp *app, const Route *route, const PureEffect *effect, Applied *out) {
    const PureInputShape *shape = &app->module->input[route->target];
    uint32_t header = get_u32(effect->value.data);
    const unsigned char *delivery = effect->value.data + 4;
    if (header & STORED_RECORDS) {
        char source[PURE_SOURCE + 1];
        memcpy(source, delivery + 13, delivery[12]);
        source[delivery[12]] = 0;
        uint64_t first = get_u64(delivery), heard = pure_app_heard(app, source);
        unsigned count = get_u32(delivery + 8);
        const unsigned char *at = effect->value.data + 4 + DELIVERY;
        /* Records laid out with other fields (named after them) are laid out anew for the input. */
        size_t records = header & (STORED_RECORDS - 1), named = effect->value.size - 4 - DELIVERY - records;
        const unsigned char *fields = named ? at + records : NULL;
        out->gap = false;
        out->fault = PURE_FAULT_NONE;
        for (unsigned i = 0; i < count; ++i) {
            uint32_t size = get_u32(at);
            PureSpan record = {at + 4, size};
            at += 4 + size;
            if (first + i <= heard) continue;
            if (first + i > heard + 1) { /* one before it is missing: the sender sends from there again */
                out->gap = true;
                break;
            }
            unsigned char *recast = NULL;
            size_t recast_size = 0;
            if (fields && pure_module_recast(app->module, route->target, fields, named, record, &recast, &recast_size) !=
                              PURE_OK)
                break; /* not laid out as its fields were named: not applied, sent again */
            PureResult status = pure_app_deliver(app, route->target, source, first + i,
                                                 recast ? (PureSpan){recast, recast_size} : record, NULL);
            free(recast);
            if (status != PURE_OK && !app_refused(app)) { /* not applied: sent again */
                out->fault = app->fault;
                break;
            }
            heard = first + i;
        }
        out->heard = pure_app_heard(app, source);
        return PURE_OK; /* the save says whether they are stored */
    }
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
             * batch goes on; another failure (the budget's too) stops it, to
             * be sent again. */
            for (unsigned i = 1; i < batch && (status == PURE_OK || app_refused(app)); ++i) {
                const unsigned char *at = effect->value.data + lead + i * table;
                for (size_t k = 0; k < 2 * (size_t)shape->field_count; ++k) placed[k] = get_u32(at + 4 * k);
                PureSpan next = {payload.data + i * shape->prefix, payload.size - i * shape->prefix};
                status = pure_app_deliver(app, route->target, source, get_u64(delivery) + i, next, placed);
            }
            if (batch > 1 && app_refused(app)) status = PURE_OK;
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
    *out = (Applied){.host = job->host, .waiter = job->waiter, .ticket = effect->ticket, .input = effect->input,
                     .route = job->route.kind, .link = job->link};
    memcpy(out->key, effect->key, strlen(effect->key) + 1);
    if (job->route.kind == ROUTE_VIEW) {
        out->status = show_view(app, &job->route, effect->available, out);
        return;
    }
    if (job->route.kind == ROUTE_INPUT) {
        out->status = apply_input(app, &job->route, effect, out);
        if (out->status == PURE_OK) ++ex->burst_events;
        ex->memory_refused += out->fault == PURE_FAULT_MEMORY;
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

/* ---- A surface in the node (serve/surface.h): one, run in its own thread,
 * reading published snapshots and giving events to the owner. */
struct PureSurfaceHost {
    struct Owner *owner;
    pthread_mutex_t lock;
    pthread_cond_t applied; /* a waiter's event was applied and saved (or not) */
    PureSurface table;
    PureSurfaceRun run;
    int argc, result;
    char **argv;
    pthread_t thread;
};

static void surface_changed(PureSurfaceHost *s) {
    uint64_t one = 1;
    if (write(s->table.changes, &one, sizeof(one)) != sizeof(one)) { /* full: it is readable already */ }
}

/* After a save, before its answers go: from now on views show what it
 * saved, so a read made after a write's answer sees the write. A failed save
 * returns the app to the last save's states, which the snapshot still shows. */
static void publish(Executor *ex) {
    if (!ex->published || (ex->publications && ex->published_changes == ex->app.changes && ex->published_module == ex->module))
        return;
    PureSnapshot *made = pure_app_snapshot(&ex->app), *old;
    pthread_mutex_lock(&ex->published->lock);
    old = ex->published->snapshot;
    ex->published->snapshot = made;
    pthread_mutex_unlock(&ex->published->lock);
    pure_snapshot_release(old);
    ex->published_changes = ex->app.changes;
    ex->published_module = ex->module;
    ++ex->publications;
    if (ex->surface) surface_changed(ex->surface);
    if (atomic_load_explicit(&ex->published->subscribers, memory_order_relaxed)) wake_pushers();
}

/* An output's outlets: its wires first, then its links. Each holds a
 * window of the output's saved sends, WINDOW_SENDS and --window bytes past
 * what its receiver has (at least one): the outbox keeps the rest, and the
 * outlet takes them as its receiver takes what it holds. */
enum { WINDOW_SENDS = 65536 };
static size_t window_bytes = 4u << 20;

/* An output's sends handed to another app of the host (send=OUTPUT=APP/INPUT,
 * docs/design/017's stage 2): no socket, seal or packet. The sender's turn
 * queues its saved sends here (feed_outlets) and hands them, a batch at a
 * time, to the receiving app's queue as records a link delivers; the
 * receiver's turn applies them after its last number heard and, its save
 * over, says what it heard (local_done); the sender's next turn trims its
 * outbox so far. What the receiver did not store is queued again from the
 * outbox, after 100 ms when it had no room. */
enum { LOCAL_RETRY_MS = 100 };
typedef struct Local {
    Owner *from, *to;
    uint32_t output, input; /* the sender's output, the receiver's input */
    const PureModule *sender, *module; /* the sender's module; the receiver's, which input is in */
    char input_name[PURE_MODULE_NAME + 1], why[160]; /* the input by name; why it holds its sends, as last said */
    char source[PURE_SOURCE + 1];
    unsigned char fields[PURE_FIELDS_NAMED]; /* the sender's, when the input takes them by name */
    size_t fields_size;
    pthread_mutex_t lock; /* the receiver's turn says what it stored */
    uint64_t queued, delivered; /* the last number queued, the last the receiver stored */
    unsigned char *batch; /* queued, not handed over yet: a u32le size and the bytes of each */
    size_t batch_size, batch_capacity, out_bytes;
    uint64_t first, out_last; /* the batch's first number; the last of the one handed over */
    unsigned count;
    bool out, rewind; /* a batch is with the receiver; it did not store all of one */
    uint64_t retry;   /* when one it had no room for goes again (the monotonic clock in ms) */
} Local;

static uint64_t local_queued(Local *l) {
    pthread_mutex_lock(&l->lock);
    uint64_t queued = l->queued;
    pthread_mutex_unlock(&l->lock);
    return queued;
}

static uint64_t local_delivered(Local *l) {
    pthread_mutex_lock(&l->lock);
    uint64_t delivered = l->delivered;
    pthread_mutex_unlock(&l->lock);
    return delivered;
}

static size_t local_held(Local *l) {
    pthread_mutex_lock(&l->lock);
    size_t held = l->batch_size + l->out_bytes;
    pthread_mutex_unlock(&l->lock);
    return held;
}

static bool local_queue(Local *l, uint64_t seq, PureSpan payload) {
    pthread_mutex_lock(&l->lock);
    bool room = grow(&l->batch, &l->batch_capacity, l->batch_size + 4 + payload.size, 1);
    if (room) {
        put_u32(l->batch + l->batch_size, (uint32_t)payload.size);
        if (payload.size) memcpy(l->batch + l->batch_size + 4, payload.data, payload.size);
        if (!l->count) l->first = seq;
        l->batch_size += 4 + payload.size;
        ++l->count;
        l->queued = seq;
    }
    pthread_mutex_unlock(&l->lock);
    return room;
}

/* The receiver's input the outlet hands to, found by name in its module r:
 * one taking messages of the output's shape or, by name, its fields. False,
 * why in error, when r has none. */
static bool local_bind(Local *l, const PureModule *r, char *error, size_t size) {
    int input = pure_module_input(r, l->input_name);
    if (input < 0 || r->input[input].kind != PURE_KIND_MESSAGE || r->input[input].keyed) {
        snprintf(error, size, "no input '%s' taking messages", l->input_name);
        return false;
    }
    size_t fields_size = 0;
    if (pure_module_shape(r, (uint32_t)input) != pure_module_shape(l->sender, l->output)) { /* taken by name, or not at all */
        fields_size = pure_module_fields(l->sender, l->output, l->fields);
        if (!pure_module_takes(r, (uint32_t)input, l->fields, fields_size)) {
            snprintf(error, size, "its input %s does not take the output's fields", l->input_name);
            return false;
        }
    }
    l->input = (uint32_t)input;
    l->fields_size = fields_size;
    l->module = r;
    return true;
}

/* The batch queued goes to the receiver, unless one is there or it waits
 * to try again; as a link's delivery is laid out (link_deliver). A
 * receiver that does not run holds it (LOCAL_RETRY_MS), and one that runs
 * another module now has its input found again first. */
static void local_hand(Executor *ex, Local *l, uint64_t now) {
    pthread_mutex_lock(&l->from->lock);
    bool leaving = l->from->stopping; /* its app stops: no batch more */
    pthread_mutex_unlock(&l->from->lock);
    pthread_mutex_lock(&l->lock);
    bool waits = leaving || l->out || !l->count || (l->retry && now < l->retry);
    pthread_mutex_unlock(&l->lock);
    if (waits) return;
    Owner *to = l->to;
    pthread_mutex_lock(&to->lock);
    const PureModule *runs = to->stopping ? NULL : to->module;
    pthread_mutex_unlock(&to->lock);
    char error[160] = "its receiver does not run";
    if (!runs || (runs != l->module && !local_bind(l, runs, error, sizeof(error)))) {
        if (strcmp(error, l->why)) { /* said once a cause */
            snprintf(l->why, sizeof(l->why), "%s", error);
            fprintf(stderr, "{\"local\":\"%s\",\"input\":\"%s\",\"error\":\"%s; its sends wait\"}\n", l->source,
                    l->input_name, error);
        }
        pthread_mutex_lock(&l->lock);
        l->retry = now + LOCAL_RETRY_MS;
        pthread_mutex_unlock(&l->lock);
        return;
    }
    l->why[0] = 0;
    pthread_mutex_lock(&l->lock);
    size_t length = strlen(l->source), size = 4 + DELIVERY + l->batch_size + l->fields_size;
    unsigned char *ingress = malloc(size);
    LinkJob *link = malloc(sizeof(*link));
    if (!ingress || !link) {
        pthread_mutex_unlock(&l->lock);
        free(ingress);
        free(link);
        return;
    }
    memset(ingress, 0, 4 + DELIVERY);
    put_u32(ingress, (uint32_t)l->batch_size | DELIVERED | STORED_RECORDS);
    put_u64(ingress + 4, l->first);
    put_u32(ingress + 12, l->count);
    ingress[16] = (unsigned char)length;
    memcpy(ingress + 17, l->source, length);
    memcpy(ingress + 4 + DELIVERY, l->batch, l->batch_size);
    if (l->fields_size) memcpy(ingress + 4 + DELIVERY + l->batch_size, l->fields, l->fields_size);
    *link = (LinkJob){.ingress = ingress, .local = l};
    l->out = true;
    l->out_bytes = l->batch_size;
    l->out_last = l->first + l->count - 1;
    l->batch_size = 0;
    l->count = 0;
    l->retry = 0;
    pthread_mutex_unlock(&l->lock);
    Job job = {.route = {.kind = ROUTE_INPUT, .target = l->input, .module = l->module}, .link = link,
               .effect = {.input = PURE_INPUT_WRITE, .key = "", .expected = -1, .value = {ingress, size}}};
    pthread_mutex_lock(&to->lock);
    bool queued = !to->stopping && to->module == l->module && grow(&to->jobs, &to->capacity, to->count + 1, sizeof(*to->jobs));
    if (queued) {
        to->jobs[to->count++] = job;
        ++to->arrivals;
        pthread_cond_signal(&to->waiting);
    }
    pthread_mutex_unlock(&to->lock);
    if (queued) {
        ++ex->handed;
        schedule(to);
        return;
    }
    free(ingress);
    free(link);
    pthread_mutex_lock(&l->lock);
    l->out = false;
    l->rewind = true; /* queued again from the outbox, after a pause */
    l->retry = now + LOCAL_RETRY_MS;
    pthread_mutex_unlock(&l->lock);
}

/* The receiver's turn, its save over: what it heard from the batch. All of
 * it, and the sender trims so far; else what it did not store is queued
 * again (after LOCAL_RETRY_MS when it had no room). The sender's turn
 * comes either way. */
static void local_done(Local *l, uint64_t heard, bool stored, bool full) {
    Owner *from = l->from; /* read first: with out false the sender may stop, and let l go */
    pthread_mutex_lock(&l->lock);
    l->out = false;
    l->out_bytes = 0;
    if (stored && heard > l->delivered) l->delivered = heard;
    if (!stored || heard < l->out_last) {
        l->rewind = true;
        if (full) l->retry = now_ms() + LOCAL_RETRY_MS;
    }
    pthread_mutex_unlock(&l->lock);
    pthread_mutex_lock(&from->lock);
    from->delivered = true;
    pthread_cond_signal(&from->waiting);
    pthread_mutex_unlock(&from->lock);
    schedule(from);
}

/* The milliseconds until a batch an app's local outlet holds back goes again (-1: none waits). */
static long locals_due(Executor *ex) {
    long due = -1;
    uint64_t now = now_ms();
    for (unsigned i = 0; i < ex->local_count; ++i) {
        Local *l = ex->locals[i];
        pthread_mutex_lock(&l->lock);
        if (l->retry && (l->count || l->rewind)) {
            long wait = l->retry > now ? (long)(l->retry - now) : 0;
            if (due < 0 || wait < due) due = wait;
        }
        pthread_mutex_unlock(&l->lock);
    }
    return due;
}

static uint32_t outlet_output(Executor *ex, unsigned i) {
    return i < ex->wire_count                 ? pure_wire_output(ex->wires[i])
           : i < ex->wire_count + ex->link_count ? ex->link_output[i - ex->wire_count]
                                                 : ex->locals[i - ex->wire_count - ex->link_count]->output;
}

/* The saved sends not yet queued go to their outlets, in order, as far as
 * each window goes; an outlet that cannot take one takes it later. Each
 * outlet looks on from its place in the outbox, moved back by the sends
 * that left the outbox since (they may have been before it), so the sends
 * it queued before are not looked at again, however many wait. */
static void feed_outlets(Executor *ex) {
    unsigned outlets = ex->wire_count + ex->link_count + ex->local_count;
    uint64_t gone = ex->app.sends_gone - ex->gone_seen;
    ex->gone_seen = ex->app.sends_gone;
    for (unsigned i = 0; i < outlets; ++i) {
        PureWire *wire = i < ex->wire_count ? ex->wires[i] : NULL;
        PureLink *link = !wire && i < ex->wire_count + ex->link_count ? ex->links[i - ex->wire_count] : NULL;
        Local *local = !wire && !link ? ex->locals[i - ex->wire_count - ex->link_count] : NULL;
        uint32_t output = outlet_output(ex, i);
        uint64_t queued = wire ? pure_wire_queued(wire) : link ? pure_link_queued(link) : local_queued(local),
                 delivered = wire ? pure_wire_delivered(wire) : link ? pure_link_delivered(link) : local_delivered(local);
        size_t held = wire ? pure_wire_held(wire) : link ? pure_link_held(link) : local_held(local), at = ex->outlet_from[i];
        at = at > gone ? at - (size_t)gone : 0;
        uint64_t before = queued;
        for (; at < ex->app.sent_saved; ++at) {
            uint32_t of;
            uint64_t seq;
            PureSpan payload;
            if (!pure_app_sent(&ex->app, at, &of, &seq, &payload) || of != output || seq <= queued) continue;
            if (queued > delivered && (queued - delivered >= WINDOW_SENDS || held + payload.size > window_bytes)) {
                ++ex->windows_full;
                break;
            }
            if (wire ? !pure_wire_queue(wire, seq, payload)
                     : link ? !pure_link_queue(link, seq, payload.data, payload.size) : !local_queue(local, seq, payload))
                break;
            queued = seq;
            held += payload.size;
        }
        if (queued != before && link) pure_link_push(link); /* the turn's together */
        else if (queued != before && wire) pure_wire_push(wire);
        ex->outlet_from[i] = at;
    }
    uint64_t now = ex->local_count ? now_ms() : 0;
    for (unsigned i = 0; i < ex->local_count; ++i) local_hand(ex, ex->locals[i], now);
}

static void after_save(Executor *ex) {
    publish(ex);
    checkpoint_round(ex);
    feed_outlets(ex);
}

enum { ANSWER_WAIT_MS = 100 };

/* The answers the wires made become events, in order, each taking its send
 * out of the outbox; one the app has no room for waits, with the wire's
 * after it, and is tried again after ANSWER_WAIT_MS. Then what the wires
 * delivered leaves the outbox (with the next save). True when an answer was
 * applied (the caller saves). */
static bool take_deliveries(Executor *ex) {
    bool answered = false;
    ex->answers_wait = false;
    for (unsigned i = 0; i < ex->wire_count; ++i) {
        PureWireAnswer answer;
        while (pure_wire_answer(ex->wires[i], &answer)) {
            ex->app.now = wall_ms();
            PureResult result = pure_app_answer(&ex->app, answer.input, pure_wire_output(ex->wires[i]), answer.source,
                                                answer.seq, answer.payload, answer.placed);
            if (result != PURE_OK && ex->app.fault == PURE_FAULT_MEMORY) {
                pure_app_finish(&ex->app);
                ++ex->memory_refused;
                ex->answers_wait = true;
                break;
            }
            if (result != PURE_OK && ex->app.fault == PURE_FAULT_NONE) {
                ex->held[i] = true; /* not applied and not refused: sent again after a restart */
                fprintf(stderr, "{\"wire\":\"%s\",\"seq\":%" PRIu64 ",\"error\":\"its answer was not applied\"}\n",
                        answer.source, answer.seq);
            }
            pure_wire_answered(ex->wires[i]);
            pure_app_finish(&ex->app);
            answered = true;
            ++ex->answers;
        }
        uint64_t delivered = pure_wire_delivered(ex->wires[i]);
        if (delivered <= ex->trimmed[i] || ex->held[i]) continue;
        pure_app_trim(&ex->app, pure_wire_output(ex->wires[i]), delivered);
        ex->trimmed[i] = delivered;
    }
    for (unsigned i = 0; i < ex->link_count; ++i) {
        uint64_t stored = pure_link_delivered(ex->links[i]);
        if (stored <= ex->link_trimmed[i]) continue;
        pure_app_trim(&ex->app, ex->link_output[i], stored);
        ex->link_trimmed[i] = stored;
    }
    for (unsigned i = 0; i < ex->local_count; ++i) {
        Local *l = ex->locals[i];
        pthread_mutex_lock(&l->lock);
        if (l->rewind && !l->out) { /* from what it stored, again */
            l->queued = l->delivered;
            l->batch_size = 0;
            l->count = 0;
            l->rewind = false;
            ex->outlet_from[ex->wire_count + ex->link_count + i] = 0;
        }
        uint64_t stored = l->delivered;
        pthread_mutex_unlock(&l->lock);
        if (stored <= ex->local_trimmed[i]) continue;
        pure_app_trim(&ex->app, l->output, stored);
        ex->local_trimmed[i] = stored;
    }
    feed_outlets(ex); /* their windows moved on */
    return answered;
}

/* One save for a burst: its requests are durable with it or fail with it. */
static void save_burst(Executor *ex, Applied *applied, size_t count) {
    if (!count) return;
    for (size_t i = 0; i < count; ++i)
        ex->wrote = ex->wrote || (applied[i].status == PURE_OK && applied[i].input == PURE_INPUT_WRITE);
    struct timespec began, ended, worked, work_ended;
    clock_gettime(CLOCK_MONOTONIC, &began);
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &worked);
    PureResult saved = pure_app_save(&ex->app);
    clock_gettime(CLOCK_MONOTONIC, &ended);
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &work_ended);
    int64_t waited = (int64_t)(ended.tv_sec - began.tv_sec) * 1000000000 + (ended.tv_nsec - began.tv_nsec) -
                     ((int64_t)(work_ended.tv_sec - worked.tv_sec) * 1000000000 + (work_ended.tv_nsec - worked.tv_nsec));
    ex->save_waited = waited > 0 ? (uint64_t)waited : 0;
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
    ex->app.now = wall_ms(); /* a round's events share its time */
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

/* Tick every timer that is due, with one save for them. A tick can change
 * states and send (the outbox is the app's state too): an app with neither
 * has nothing a tick could do, and runs on every thread, so it ticks none.
 * The milliseconds until the next is due, or -1 without timers. Missed
 * ticks are not made up. When a timer is due is the monotonic clock's
 * business (the wall clock may be set back); events see the wall clock. */
static long run_timers(Executor *ex) {
    const PureModule *m = ex->module;
    bool changes = m->states > 0;
    for (uint32_t i = 0; i < m->inputs && !changes; ++i) changes = m->input[i].kind == PURE_KIND_OUTPUT;
    if (!changes) return -1;
    uint64_t now = now_ms(), soonest = UINT64_MAX;
    bool ticked = false;
    for (uint32_t i = 0; i < m->inputs; ++i) {
        if (m->input[i].kind != PURE_KIND_TIMER) continue;
        if (!ex->next[i]) ex->next[i] = now + m->input[i].period; /* the first period starts now */
        if (ex->next[i] <= now) {
            ex->app.now = wall_ms();
            if (pure_app_tick(&ex->app, i) == PURE_OK) ++ex->ticks;
            ticked = true;
            ex->next[i] = now + m->input[i].period;
        }
        if (ex->next[i] - now < soonest) soonest = ex->next[i] - now;
    }
    if (ticked && pure_app_save(&ex->app) == PURE_OK) after_save(ex); /* a failed save undoes the ticks */
    return soonest == UINT64_MAX ? -1 : (long)soonest;
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
        else if (a->refused) {
            respond(c, 503, "Service Unavailable", a->refused, 0, (PureSpan){0});
            release_ticket(c);
        } else if (a->route != ROUTE_OBJECT) answer_app(c, result, a->route == ROUTE_VIEW, a->fault, a->type);
        else answer(c, result);
        drive(c); /* most answers fit the socket buffer at once */
    }
    free(a->extents);
}

static void answer_burst(void *context, Applied *applied, size_t count) {
    (void)context;
    for (size_t i = 0; i < count; ++i) answer_applied(applied[i].host, &applied[i]);
}

/* A connection thread's requests among those applied, given back to it (an
 * owner's or a view worker's), and it woken. */
static void give_back(Host *host, const Applied *applied, size_t count) {
    size_t mine = 0;
    for (size_t i = 0; i < count; ++i) mine += applied[i].host == host;
    if (!mine) return;
    pthread_mutex_lock(&host->lock);
    if (!grow(&host->done, &host->done_capacity, host->done_count + mine, sizeof(*host->done))) abort();
    for (size_t i = 0; i < count; ++i)
        if (applied[i].host == host) host->done[host->done_count++] = applied[i];
    pthread_mutex_unlock(&host->lock);
    uint64_t one = 1;
    if (write(host->wake, &one, sizeof(one)) != sizeof(one)) abort();
}

/* On the owner: give each connection thread its applied requests, and wake it. */
static void hand_back(void *context, Applied *applied, size_t count) {
    Owner *owner = context;
    PureSurfaceHost *surface = owner->executor.surface;
    bool events = false;
    for (size_t i = 0; i < count; ++i) {
        events = events || applied[i].waiter;
        LinkJob *link = applied[i].link;
        if (!link) continue; /* records a link (or another app of the host) delivered: stored now, or not */
        if (link->local) local_done(link->local, applied[i].heard, applied[i].status == PURE_OK, applied[i].fault == PURE_FAULT_MEMORY);
        else
            pure_link_done(link->listener, (PureLinkDone){link->token, applied[i].heard, applied[i].status == PURE_OK,
                                                         applied[i].gap, applied[i].fault == PURE_FAULT_MEMORY});
        free(link->ingress);
        free(link);
    }
    if (events) { /* a surface's: its waiter wakes */
        pthread_mutex_lock(&surface->lock);
        for (size_t i = 0; i < count; ++i)
            if (applied[i].waiter) applied[i].waiter->status = applied[i].status, applied[i].waiter->done = true;
        pthread_cond_broadcast(&surface->applied);
        pthread_mutex_unlock(&surface->lock);
    }
    for (unsigned h = 0; h < owner->host_count; ++h) give_back(&owner->hosts[h], applied, count);
}

static void group_evaluated(Host *host, Applied *a);

/* On a connection thread: answer what the owner and the view workers handed back. */
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
    for (size_t i = 0; i < count; ++i)
        if (done[i].group) group_evaluated(host, &done[i]);
        else answer_applied(host, &done[i]);
    host->pending -= count;
}

/* The owner's newest snapshot, held, shown by this thread's reader; NULL
 * when there is none. Taken for each round, after the round's answers from
 * the owner: those were handed back after their saves' snapshots. */
static PureSnapshot *read_newest(PureApp *reader, App *app) {
    Published *published = &app->owner.published;
    pthread_mutex_lock(&published->lock);
    PureSnapshot *snapshot = published->snapshot;
    if (snapshot) pure_snapshot_hold(snapshot);
    pthread_mutex_unlock(&published->lock);
    if (snapshot) pure_app_read(reader, snapshot);
    reader->work_budget = atomic_load_explicit(&app->work, memory_order_relaxed);
    reader->memory = atomic_load_explicit(&app->memory, memory_order_relaxed); /* what a view's body may add to its own lists */
    return snapshot;
}

static PureSnapshot *read_published(Host *host, App *app) { return read_newest(&host->readers[app - apps], app); }

/* ---- Views that compute, off the connection threads (docs/design/020's
 * stage 2). A view with a body may run long: its GET, and a push group's
 * evaluation of it, go to the view workers, which show it from its app's
 * newest snapshot on readers of their own and give the answer back to the
 * thread it came from as an owner does. That thread reads and writes its
 * other connections meanwhile. A view of one expression stays on it. */
typedef struct ViewJob {
    Host *host;
    Route route;         /* in the module it was resolved with: shown only from a snapshot of that one */
    PureTicket ticket;   /* a GET's, */
    PureInput input;
    size_t available;
    struct Group *group; /* or a push group's evaluation */
    struct ViewJob *next;
} ViewJob;
static struct {
    pthread_mutex_t lock;
    pthread_cond_t ready;
    ViewJob *first, *last;
    bool stopping;
} views_waiting = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, NULL, NULL, false};
static unsigned view_workers = UINT_MAX; /* --view-workers (UINT_MAX: a CPU each; 0: none, on the connection threads) */
static pthread_t *view_threads;
static unsigned view_thread_count;
static unsigned online_cpus(void);

/* Whether a view may run long: it has a body, or it is a table one of whose columns has. */
static bool computes(const PureModule *m, uint32_t view) {
    if (m->view_body[view]) return true;
    if (m->view_type[view] != PURE_VIEW_TABLE) return false;
    for (uint32_t column = 0; column < m->views; ++column)
        if (m->view_table[column] == view + 1 && m->view_body[column]) return true;
    return false;
}

/* A view for the workers to show: it comes back to host. */
static void hand_view(Host *host, const Route *route, PureTicket ticket, PureInput input, size_t available,
                      struct Group *group) {
    ViewJob *job = malloc(sizeof(*job));
    if (!job) abort();
    *job = (ViewJob){host, *route, ticket, input, available, group, NULL};
    ++host->pending;
    pthread_mutex_lock(&views_waiting.lock);
    if (views_waiting.last) views_waiting.last->next = job;
    else views_waiting.first = job;
    views_waiting.last = job;
    pthread_cond_signal(&views_waiting.ready);
    pthread_mutex_unlock(&views_waiting.lock);
}

static void *run_view_worker(void *unused) {
    (void)unused;
    PureApp *readers = calloc(app_slots, sizeof(*readers)); /* by app place, as a connection thread's */
    if (!readers) abort();
    for (;;) {
        pthread_mutex_lock(&views_waiting.lock);
        while (!views_waiting.first && !views_waiting.stopping) pthread_cond_wait(&views_waiting.ready, &views_waiting.lock);
        ViewJob *job = views_waiting.first;
        if (job && !(views_waiting.first = job->next)) views_waiting.last = NULL;
        pthread_mutex_unlock(&views_waiting.lock);
        if (!job) break;
        App *app = job->route.app;
        PureApp *reader = &readers[app - apps];
        PureSnapshot *snapshot = read_newest(reader, app);
        Applied a = {.host = job->host, .ticket = job->ticket, .input = job->input, .route = ROUTE_VIEW, .group = job->group};
        if (!snapshot || reader->module != job->route.module) { /* its app stopped, or runs another module now */
            a.status = PURE_CLOSED;
            a.refused = "stopped";
        } else {
            a.status = show_view(reader, &job->route, job->available, &a);
            a.changes = reader->changes;
            if (job->group && a.status != PURE_OK && a.fault == PURE_FAULT_NONE) a.fault = pure_app_view_fault(reader);
        }
        if (snapshot) pure_snapshot_release(snapshot); /* an answer holds what it lends */
        if (!job->group) atomic_fetch_sub_explicit(&app->viewing, 1, memory_order_relaxed);
        give_back(job->host, &a, 1);
        free(job);
    }
    for (unsigned i = 0, count = places(); i < count; ++i) pure_app_stop(&readers[i]);
    free(readers);
    return NULL;
}

/* Made before the connection threads, stopped after them (they wait for what they handed). */
static bool start_view_workers(void) {
    unsigned count = view_workers == UINT_MAX ? online_cpus() : view_workers;
    if (!count) return true;
    if (!(view_threads = calloc(count, sizeof(*view_threads)))) return false;
    for (; view_thread_count < count; ++view_thread_count) {
        if (pthread_create(&view_threads[view_thread_count], NULL, run_view_worker, NULL)) return false;
        pthread_setname_np(view_threads[view_thread_count], "pure-view"); /* their CPU, seen apart */
    }
    return true;
}

static void stop_view_workers(void) {
    pthread_mutex_lock(&views_waiting.lock);
    views_waiting.stopping = true;
    pthread_cond_broadcast(&views_waiting.ready);
    pthread_mutex_unlock(&views_waiting.lock);
    for (unsigned i = 0; i < view_thread_count; ++i) pthread_join(view_threads[i], NULL);
    free(view_threads);
}

/* ---- Pushing views (docs/design/019). A view's GET that asks for an event
 * stream makes its connection a subscriber: its socket, and the group of
 * this thread's subscribers to that view with those arguments. A pass, at
 * most every --push-every-ms, evaluates each group once on its app's
 * newest snapshot and sends the value to the group when it changed; a new
 * subscriber gets the value the group has. A subscriber still sending one
 * event keeps only the newest after it: a value, not every event. */
typedef struct Subscriber {
    int fd;
    struct Group *group;
    struct Subscriber *next; /* in its group */
    bool writable, fresh;    /* fresh: it has not had the group's value yet */
    char *out;               /* what is left of the event being sent (NULL: none), and the newest after it */
    size_t out_size, out_at;
    char *waiting;
    size_t waiting_size;
    uint64_t stalled;        /* since when out has not moved (ms) */
    const Tokens *checked;   /* the tokens it was let in by, and its token's digest (token: it gave one) */
    bool token;
    unsigned char digest[32];
} Subscriber;

typedef struct Group {
    App *app;
    Route route;             /* its view in the module it was resolved with; given: text */
    char name[PURE_MODULE_NAME];
    char *text;              /* the query as asked */
    bool computed, failed;   /* a value computed, from the snapshot of changes; failed: why holds a fault's name */
    uint64_t changes;
    char *value;             /* the value's bytes (or why it has none), and as an event */
    size_t value_size;
    char *event;
    size_t event_size;
    /* A view worker evaluates it (a view with a body): for the module asked
     * of; again: a newer snapshot came meanwhile. It stays until it is back. */
    bool evaluating, again;
    const PureModule *asked;
    Subscriber *first;
    struct Group *next;
} Group;

static size_t subscribers_most = 65536; /* --subscribers: a thread's */
static uint64_t push_every_ms = 10;     /* --push-every-ms */

/* An owner published (or its app stopped) and someone subscribes to its
 * views: each thread that has not been told since its last pass is. */
static void wake_pushers(void) {
    for (unsigned i = 0; i < host_count; ++i) {
        Host *h = &all_hosts[i];
        uint64_t one = 1;
        if (!atomic_exchange_explicit(&h->push_due, true, memory_order_acq_rel) && write(h->wake, &one, sizeof(one)) < 0) {
            /* full: it is readable already */
        }
    }
}

static char *copied(const char *bytes, size_t size) {
    char *copy = malloc(size ? size : 1);
    if (!copy) abort();
    if (size) memcpy(copy, bytes, size);
    return copy;
}

/* A subscriber's socket in the epoll set: told when it can be read (it
 * closed), and when it can be written only while it keeps something to
 * send (otherwise every acknowledgment would wake the thread). */
static void watch_subscriber(Host *host, Subscriber *s, int op) {
    struct epoll_event event = {.events = EPOLLIN | EPOLLRDHUP | EPOLLET | (s->out ? EPOLLOUT : 0),
                                .data.u64 = (uintptr_t)s | 1u};
    if (epoll_ctl(host->epoll, op, s->fd, &event)) abort();
}

/* An event to a subscriber: sent now as far as its socket takes it, the
 * rest kept; while one is kept, the newest waits after it, replacing what
 * waited. False when its socket failed. */
static bool subscriber_send(Host *host, Subscriber *s, const char *event, size_t size) {
    ++host->pushed_events;
    if (s->out) {
        if (s->waiting) { free(s->waiting); ++host->replaced; }
        s->waiting = copied(event, size);
        s->waiting_size = size;
        return true;
    }
    ssize_t n = s->writable ? send(s->fd, event, size, MSG_NOSIGNAL | MSG_DONTWAIT) : 0;
    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) return false;
    if (n < 0) n = 0;
    if ((size_t)n < size) {
        s->writable = false;
        s->out = copied(event + n, size - (size_t)n);
        s->out_size = size - (size_t)n;
        s->out_at = 0;
        s->stalled = host->now;
        watch_subscriber(host, s, EPOLL_CTL_MOD); /* and now when it can be written */
    }
    return true;
}

/* What a subscriber kept, sent as far as its socket takes it. False when its socket failed. */
static bool subscriber_flush(Host *host, Subscriber *s) {
    bool kept = s->out != NULL;
    while (s->out && s->writable) {
        ssize_t n = send(s->fd, s->out + s->out_at, s->out_size - s->out_at, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) return false;
        if (n <= 0) { s->writable = false; break; }
        s->out_at += (size_t)n;
        s->stalled = host->now;
        if (s->out_at < s->out_size) continue;
        free(s->out);
        s->out = s->waiting, s->out_size = s->waiting_size, s->out_at = 0;
        s->waiting = NULL;
    }
    if (kept && !s->out) watch_subscriber(host, s, EPOLL_CTL_MOD); /* all sent: no more told when it can be written */
    return true;
}

static void free_group(Group *g) {
    free(g->text);
    free(g->value);
    free(g->event);
    free(g);
}

static void unsubscribe(Host *host, Subscriber *s) {
    Group *g = s->group;
    for (Subscriber **at = &g->first; *at; at = &(*at)->next)
        if (*at == s) { *at = s->next; break; }
    close(s->fd); /* also leaves the epoll set */
    free(s->out);
    free(s->waiting);
    atomic_fetch_sub_explicit(&g->app->owner.published.subscribers, 1, memory_order_relaxed);
    --host->subscribers;
    ++host->unsubscribed;
    free(s);
    if (g->first) return;
    for (Group **at = &host->groups; *at; at = &(*at)->next)
        if (*at == g) { *at = g->next; break; }
    if (!g->evaluating) free_group(g); /* else when its evaluation is back */
}

static void close_group(Host *host, Group *g) {
    while (g->first && g->first->next) unsubscribe(host, g->first);
    unsubscribe(host, g->first); /* the last lets the group go */
}

/* Whether a subscriber's token still lets it read (tokens: the app's now). */
static bool still_reads(const Tokens *tokens, const Subscriber *s) {
    if (!tokens) return true;
    if (!s->token) return false;
    for (unsigned g = 0; g < tokens->count; ++g) {
        unsigned char differ = 0;
        for (unsigned i = 0; i < 32; ++i) differ |= (unsigned char)(tokens->grant[g].digest[i] ^ s->digest[i]);
        if (!differ) return tokens->grant[g].kind == GRANT_READ || tokens->grant[g].kind == GRANT_WRITE;
    }
    return false;
}

static void subscriber_event(Host *host, Subscriber *s, uint32_t events) {
    bool gone = events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP);
    if (events & EPOLLOUT) s->writable = true;
    if (events & EPOLLIN) { /* a stream reads nothing: what comes is let go, and its end ends it */
        char sink[512];
        ssize_t n;
        while ((n = recv(s->fd, sink, sizeof(sink), 0)) > 0) {}
        if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) gone = true;
    }
    if (gone || !subscriber_flush(host, s)) unsubscribe(host, s);
}

/* A group's value, as the view's GET answers it (a), or why it has none
 * (event none), from the snapshot of changes. An event made when it differs
 * from the last, for each subscriber. */
static void group_value(Group *g, Applied *a, uint64_t changes) {
    Output value = {0};
    bool failed = a->status != PURE_OK;
    if (failed) { /* why it has none */
        const char *why = a->status == PURE_NOT_FOUND && a->fault == PURE_FAULT_NONE ? "no_value" : pure_fault_name(a->fault);
        put_bytes(&value, why, strlen(why));
    } else if (a->extents) {
        for (size_t i = 0; i < a->extent_count; ++i) put_bytes(&value, a->extents[i].iov_base, a->extents[i].iov_len);
    } else put_bytes(&value, a->resource.body.data, a->resource.body.size);
    if (value.failed) abort();
    bool same = g->computed && failed == g->failed && value.size == g->value_size &&
                (!value.size || !memcmp(value.bytes, g->value, value.size));
    g->changes = changes;
    g->computed = true;
    if (!same) {
        free(g->value);
        g->value = (char *)value.bytes, g->value_size = value.size, g->failed = failed;
        value.bytes = NULL;
        Output event = {0}; /* id, an event's name if it has none, and a data line per line */
        char id[32];
        int length = snprintf(id, sizeof(id), "id: %" PRIu64 "\n", g->changes);
        put_bytes(&event, id, (size_t)length);
        if (failed) put_bytes(&event, "event: none\n", 12);
        for (size_t at = 0;;) { /* a data line per line of the value */
            const char *end = at < g->value_size ? memchr(g->value + at, '\n', g->value_size - at) : NULL;
            size_t line = end ? (size_t)(end - (g->value + at)) : g->value_size - at;
            put_bytes(&event, "data: ", 6);
            if (line) put_bytes(&event, g->value + at, line);
            put_bytes(&event, "\n", 1);
            if (!end) break;
            at += line + 1;
        }
        put_bytes(&event, "\n", 1);
        if (event.failed) abort();
        free(g->event);
        g->event = (char *)event.bytes, g->event_size = event.size;
        for (Subscriber *s = g->first; s; s = s->next) s->fresh = true; /* each gets it */
    }
    free(value.bytes);
}

static void let_go_applied(Applied *a) {
    if (a->resource.release) a->resource.release(a->resource.allocation, a->resource.allocation_size);
    free(a->extents);
}

/* Each subscriber that has not had the group's value gets it (none before
 * its first evaluation is back). False when the group went with the last. */
static bool group_send(Host *host, Group *g) {
    for (Subscriber *s = g->first, *after; g->event && s; s = after) {
        after = s->next;
        if (!s->fresh) continue;
        s->fresh = false;
        if (!subscriber_send(host, s, g->event, g->event_size)) {
            bool last = !s->next && g->first == s;
            unsubscribe(host, s);
            if (last) return false;
        }
    }
    return true;
}

/* A group's value from the snapshot its app's reader shows, when it is not
 * the one it has: here, or by a view worker for a view with a body (one at
 * a time; a newer snapshot meanwhile is evaluated after). Then each
 * subscriber gets it, and a fresh one the group's value at least. */
static void push_group(Host *host, Group *g) {
    App *app = g->app;
    if (atomic_load(&app->gone)) { close_group(host, g); return; }
    const Tokens *tokens = atomic_load_explicit(&app->tokens, memory_order_acquire);
    for (Subscriber *s = g->first, *after; s; s = after) { /* a tokens file changed: who may still read */
        after = s->next;
        if (s->checked == tokens) continue;
        if (!still_reads(tokens, s)) {
            if (!s->next && g->first == s) { unsubscribe(host, s); return; } /* the group goes with it */
            unsubscribe(host, s);
        } else s->checked = tokens;
    }
    /* Quiet (taking no request: its module changing, or stopping) it waits
     * for the next word; stopped (quiet and its snapshot let go) its streams end. */
    bool quiet = !atomic_load(&app->live);
    PureSnapshot *snapshot = read_published(host, app);
    if (quiet && !snapshot) { close_group(host, g); return; }
    if (quiet) { pure_snapshot_release(snapshot); return; }
    if (!snapshot) return; /* none yet: the next publication wakes this */
    PureApp *reader = &host->readers[app - apps];
    if (reader->module != g->route.module) { /* the app changed module: the view of that name, if it is the same kind */
        const PureModule *was = g->route.module, *now = reader->module;
        int view = pure_module_view(now, g->name);
        if (view < 0 || now->view_type[view] != was->view_type[g->route.target] ||
            (pure_module_arguments(now, (uint32_t)view) < 0) != (pure_module_arguments(was, g->route.target) < 0)) {
            pure_snapshot_release(snapshot);
            close_group(host, g);
            return;
        }
        g->route.module = now;
        g->route.target = (uint32_t)view;
        g->computed = false;
    }
    if (!g->computed || reader->changes != g->changes) {
        if (view_thread_count && computes(g->route.module, g->route.target)) {
            if (g->evaluating) g->again = true;
            else {
                g->evaluating = true;
                g->asked = g->route.module;
                hand_view(host, &g->route, (PureTicket){0}, PURE_INPUT_READ, PURE_OBJECT_LIMIT, g);
            }
        } else {
            Applied a = {0};
            a.status = show_view(reader, &g->route, PURE_OBJECT_LIMIT, &a);
            if (a.status != PURE_OK && a.fault == PURE_FAULT_NONE) a.fault = pure_app_view_fault(reader);
            group_value(g, &a, reader->changes);
            let_go_applied(&a);
        }
    }
    pure_snapshot_release(snapshot);
    group_send(host, g);
}

/* A view worker's evaluation of a group, back on its thread: its value,
 * unless the app stopped or changed module meanwhile (the next look says
 * which); then the newer snapshot's, if one came. */
static void group_evaluated(Host *host, Applied *a) {
    Group *g = a->group;
    g->evaluating = false;
    if (!g->first) { /* its subscribers all left meanwhile */
        let_go_applied(a);
        free_group(g);
        return;
    }
    if (a->refused || g->asked != g->route.module) g->again = true;
    else group_value(g, a, a->changes);
    let_go_applied(a);
    if (!group_send(host, g)) return;
    if (g->again && !atomic_load(&stopping)) {
        g->again = false;
        push_group(host, g);
    }
}

static void push_pass(Host *host) {
    host->pushed_at = host->now;
    ++host->pushes;
    for (Group *g = host->groups, *next; g; g = next) {
        next = g->next;
        push_group(host, g);
    }
}

/* A view's GET that asks for a stream: the connection becomes a subscriber
 * of its group (made if this thread has none of that view and those
 * arguments), its answer's head sent; the pass sends it the value. */
static void subscribe(Connection *c, const Route *route, const char *raw, size_t raw_size) {
    Host *host = c->host;
    if (c->in_used) { refuse(c, 400, "Bad Request", "request"); return; } /* no request follows a stream */
    if (host->subscribers >= subscribers_most) { reject(c, 503, "Service Unavailable", "busy"); return; }
    Group *g = host->groups;
    while (g && !(g->app == route->app && g->route.module == route->module && g->route.target == route->target &&
                  g->route.from == route->from && g->route.count == route->count && strlen(g->text) == raw_size &&
                  !memcmp(g->text, raw, raw_size)))
        g = g->next;
    if (!g) {
        if (!(g = calloc(1, sizeof(*g)))) abort();
        g->app = route->app;
        g->route = *route;
        g->text = copied(raw, raw_size + 1);
        g->text[raw_size] = 0;
        g->route.given = g->text;
        snprintf(g->name, sizeof(g->name), "%s", route->module->view_names[route->target]);
        g->next = host->groups;
        host->groups = g;
    }
    Subscriber *s = calloc(1, sizeof(*s));
    if (!s) abort();
    s->fd = c->fd;
    s->group = g;
    s->writable = true;
    s->fresh = true;
    s->checked = atomic_load_explicit(&route->app->tokens, memory_order_acquire);
    if ((s->token = c->bearer_given && c->hashed_valid)) memcpy(s->digest, c->hashed_digest, sizeof(s->digest));
    s->next = g->first;
    g->first = s;
    watch_subscriber(host, s, EPOLL_CTL_MOD);
    c->fd = -1; /* the socket is the subscriber's now */
    close_connection(c);
    atomic_fetch_add_explicit(&route->app->owner.published.subscribers, 1, memory_order_relaxed);
    ++host->subscribers;
    ++host->subscribed;
    static const char head[] = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-store\r\n"
                               "X-Pure-Error: none\r\n\r\nretry: 1000\n\n";
    --host->pushed_events; /* the head is not an event */
    if (!subscriber_send(host, s, head, sizeof(head) - 1)) { unsubscribe(host, s); return; }
    atomic_store_explicit(&host->push_due, true, memory_order_release); /* its value goes in this round's pass */
}

/* ---- Queries: PUT /queries/NAME registers a module of the app's states and
 * views of its own (aup query), checked when it comes: it must load (be
 * verified) with its states laid out as the app's, and have a view NAME.
 * GET /queries/NAME shows that view from the last save's snapshot, on the
 * connection thread, as the app's views are: a query never waits for, nor
 * holds up, the owner's writes. */
enum { QUERIES = 64 };
typedef struct { PureModule module; unsigned refs; } Held;
static struct { const App *app; char name[65]; Held *held; const PureModule *checked; } queries[QUERIES];
static pthread_mutex_t queries_lock = PTHREAD_MUTEX_INITIALIZER;

static void let_go_query(Held *held) {
    pthread_mutex_lock(&queries_lock);
    bool last = !--held->refs;
    pthread_mutex_unlock(&queries_lock);
    if (!last) return;
    pure_module_release(&held->module);
    free(held);
}

/* Whether a's states are laid out as b's: names, kinds, what each refers to,
 * keys, and the records and document collections behind them. */
static bool same_states(const PureModule *a, const PureModule *b) {
    if (a->states != b->states || a->records != b->records || a->record_fields != b->record_fields ||
        a->collections != b->collections)
        return false;
    for (uint32_t i = 0; i < a->states; ++i)
        if (strcmp(a->state_names[i], b->state_names[i]) || a->state_type[i] != b->state_type[i] ||
            a->state_ref[i] != b->state_ref[i] || a->state_flags[i] != b->state_flags[i] || a->state_key[i] != b->state_key[i])
            return false;
    for (uint32_t i = 0; i < a->records; ++i)
        if (strcmp(a->record_names[i], b->record_names[i]) || a->record[i].first != b->record[i].first ||
            a->record[i].count != b->record[i].count)
            return false;
    for (uint32_t i = 0; i < a->record_fields; ++i)
        if (strcmp(a->record_field_names[i], b->record_field_names[i]) || a->record_field_type[i] != b->record_field_type[i])
            return false;
    for (uint32_t i = 0; i < a->collections; ++i)
        if (strcmp(a->collection_names[i], b->collection_names[i])) return false;
    return true;
}

static PureResult register_query(const Job *job) {
    const App *app = job->route.app;
    Held *held = calloc(1, sizeof(*held));
    char error[128];
    if (!held) return PURE_ALLOCATION;
    if (pure_module_load(job->effect.value.data, job->effect.value.size, &held->module, error, sizeof(error)) != PURE_OK) {
        free(held);
        return PURE_INVALID;
    }
    PureResult status = !same_states(&held->module, job->route.module)     ? PURE_CONFLICT
                        : pure_module_view(&held->module, job->effect.key) < 0 ? PURE_NOT_FOUND
                                                                               : PURE_OK;
    Held *old = NULL;
    held->refs = 1;
    if (status == PURE_OK) {
        pthread_mutex_lock(&queries_lock);
        int slot = -1;
        for (int i = 0; i < QUERIES && slot < 0; ++i)
            if (queries[i].held && queries[i].app == app && !strcmp(queries[i].name, job->effect.key)) slot = i;
        for (int i = 0; i < QUERIES && slot < 0; ++i)
            if (!queries[i].held) slot = i;
        if (slot < 0) status = PURE_LIMIT;
        else {
            old = queries[slot].held;
            queries[slot].app = app;
            queries[slot].checked = job->route.module;
            snprintf(queries[slot].name, sizeof(queries[slot].name), "%s", job->effect.key);
            queries[slot].held = held;
        }
        pthread_mutex_unlock(&queries_lock);
    }
    if (status != PURE_OK) let_go_query(held);
    if (old) let_go_query(old);
    return status;
}

static PureResult run_query(const Job *job, PureSnapshot *snapshot, Applied *a) {
    Held *held = NULL;
    const PureModule *checked = NULL;
    pthread_mutex_lock(&queries_lock);
    for (int i = 0; i < QUERIES && !held; ++i)
        if (queries[i].held && queries[i].app == job->route.app && !strcmp(queries[i].name, job->effect.key)) {
            (held = queries[i].held)->refs++;
            checked = queries[i].checked;
        }
    pthread_mutex_unlock(&queries_lock);
    if (!held) return PURE_NOT_FOUND;
    if (!snapshot) {
        let_go_query(held);
        return PURE_BUSY;
    }
    PureApp reader = {0};
    pure_app_read(&reader, snapshot);
    if (reader.module != checked && !same_states(&held->module, reader.module)) { /* its app's states are laid out anew */
        pure_app_stop(&reader);
        let_go_query(held);
        return PURE_CONFLICT;
    }
    pure_app_work(&reader, atomic_load_explicit(&job->route.app->work, memory_order_relaxed));
    reader.memory = atomic_load_explicit(&job->route.app->memory, memory_order_relaxed);
    reader.module = &held->module; /* the app's states, the query's views */
    Route route = job->route;
    route.target = (uint32_t)pure_module_view(&held->module, job->effect.key);
    PureResult status = show_view(&reader, &route, job->effect.available, a);
    pure_app_stop(&reader);
    let_go_query(held);
    return status;
}

/* Whether a job of the round is shown on this thread: a query, or a view
 * whose app has a snapshot to show it from. */
static bool shown_here(const Host *host, const Job *job) {
    return job->route.kind == ROUTE_QUERY || (job->route.kind == ROUTE_VIEW && host->snapshots[job->route.app - apps]);
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
            host->jobs[count++] = (Job){host, effect, host->routes[effect.ticket.slot], NULL, NULL};
        if (!count) return;
        if (!apps[0].owned) { /* the one app of the host, of documents alone: run here */
            execute(&host->local, host->jobs, count, answer_burst, NULL);
            continue;
        }
        /* Each app with a view or a query in the round: its snapshot, read
         * once (NULL: none yet, and its owner shows its views). */
        ++host->round;
        size_t touched = 0;
        for (size_t i = 0; i < count; ++i) {
            const Route *route = &host->jobs[i].route;
            unsigned a = (unsigned)(route->app - apps);
            if ((route->kind != ROUTE_VIEW && route->kind != ROUTE_QUERY) || host->read_in[a] == host->round) continue;
            host->read_in[a] = host->round;
            host->snapshots[a] = read_published(host, route->app);
            host->touched[touched++] = a;
        }
        /* The rest go to their apps' owners, an owner's all at once; past an
         * app's queue budget they are refused (503 busy). */
        if (!grow(&host->views, &host->views_capacity, count, sizeof(*host->views))) abort();
        size_t answers = 0;
        for (size_t i = 0; i < count; ++i) {
            const Job *job = &host->jobs[i];
            App *app = job->route.app;
            unsigned a = (unsigned)(app - apps);
            if (shown_here(host, job) || host->handed_in[a] == host->round) continue;
            host->handed_in[a] = host->round;
            Owner *owner = &app->owner;
            size_t handed = 0;
            pthread_mutex_lock(&owner->lock);
            for (size_t k = i; k < count; ++k) {
                const Job *mine = &host->jobs[k];
                if (mine->route.app != app || shown_here(host, mine)) continue;
                if (owner->stopping || mine->route.module != owner->module) { /* its app stopped, or runs another module now */
                    host->views[answers++] = (Applied){.host = host, .ticket = mine->effect.ticket, .input = mine->effect.input,
                                                       .route = mine->route.kind, .status = PURE_CLOSED, .refused = "stopped"};
                    continue;
                }
                if (owner->queue && owner->count >= owner->queue) {
                    host->views[answers++] = (Applied){.host = host, .ticket = mine->effect.ticket, .input = mine->effect.input,
                                                       .route = mine->route.kind, .status = PURE_BUSY, .refused = "busy"};
                    ++owner->busy;
                    continue;
                }
                if (!grow(&owner->jobs, &owner->capacity, owner->count + 1, sizeof(*owner->jobs))) abort();
                owner->jobs[owner->count++] = *mine;
                ++handed;
            }
            owner->arrivals += handed;
            pthread_cond_signal(&owner->waiting); /* it may be gathering */
            pthread_mutex_unlock(&owner->lock);
            if (handed) schedule(owner);
            host->pending += handed;
        }
        /* Shown first and answered after, as a burst is. */
        size_t shown = 0;
        for (size_t i = 0; i < count; ++i) {
            const Job *job = &host->jobs[i];
            if (!shown_here(host, job)) continue;
            unsigned a = (unsigned)(job->route.app - apps);
            Applied *applied = &host->views[answers++];
            bool registering = job->route.kind == ROUTE_QUERY && job->effect.input == PURE_INPUT_WRITE;
            *applied = (Applied){.host = host, .ticket = job->effect.ticket, .input = job->effect.input,
                                 .route = registering ? ROUTE_QUERY : ROUTE_VIEW};
            if (job->route.kind == ROUTE_VIEW && host->readers[a].module != job->route.module) { /* another module's view now */
                applied->status = PURE_CLOSED;
                applied->refused = "stopped";
                continue;
            }
            if (job->route.kind == ROUTE_VIEW && view_thread_count && computes(job->route.module, job->route.target)) {
                App *app = job->route.app; /* a view with a body: the workers show it, past its app's queue 503 busy */
                if (app->queue && atomic_load_explicit(&app->viewing, memory_order_relaxed) >= app->queue) {
                    applied->status = PURE_BUSY;
                    applied->refused = "busy";
                    continue;
                }
                atomic_fetch_add_explicit(&app->viewing, 1, memory_order_relaxed);
                hand_view(host, &job->route, job->effect.ticket, job->effect.input, job->effect.available, NULL);
                --answers; /* answered when it is back */
                ++shown, ++host->pooled;
                continue;
            }
            applied->status = job->route.kind == ROUTE_VIEW ? show_view(&host->readers[a], &job->route, job->effect.available, applied)
                              : registering                ? register_query(job)
                                                           : run_query(job, host->snapshots[a], applied);
            ++shown;
        }
        for (size_t i = 0; i < touched; ++i) pure_snapshot_release(host->snapshots[host->touched[i]]); /* answers lend their own */
        if (!answers) return;
        for (size_t i = 0; i < answers; ++i) answer_applied(host, &host->views[i]);
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

/* The workers' queue of apps with work to do (docs/design/017): an app
 * takes its turns in order, on one worker at a time. */
static struct {
    pthread_mutex_t lock;
    pthread_cond_t wake; /* a worker waits for an app, or for the soonest timer due (on the monotonic clock) */
    Owner *head, *tail;
    bool stopping;
} turns = {.lock = PTHREAD_MUTEX_INITIALIZER};

static void enqueue(Owner *owner) {
    owner->queued = true;
    owner->next = NULL;
    if (turns.tail) turns.tail->next = owner;
    else turns.head = owner;
    turns.tail = owner;
    pthread_cond_signal(&turns.wake);
}

/* Work came for an app: a turn, unless it waits for one or has one (then
 * another after it). Any thread, holding none of the owner's locks. */
static void schedule(Owner *owner) {
    pthread_mutex_lock(&turns.lock);
    if (!owner->live) { /* its app stopped: what it did not take waits for its next start */ }
    else if (owner->running) owner->again = true;
    else if (!owner->queued) enqueue(owner);
    pthread_mutex_unlock(&turns.lock);
}

/* The most requests a turn takes when other apps wait for a worker (all
 * of them when none does): a connection thread's round. */
enum { TURN = BATCH };

/* An app's turn: its timers due, what its wires delivered, and the
 * requests waiting for it, applied and saved once, their answers handed
 * back. Alone (no other app waits for a worker), it first gathers the
 * clients its last save answered. Whether it has requests left at once;
 * due, its next timer's milliseconds (-1: none). */
static bool owner_turn(Owner *owner, bool alone, long *due) {
    Executor *ex = &owner->executor;
    pthread_mutex_lock(&owner->lock);
    bool delivered = owner->delivered;
    owner->delivered = false;
    pthread_mutex_unlock(&owner->lock);
    *due = run_timers(ex);
    bool answered = (delivered || ex->answers_wait) && take_deliveries(ex);
    if (ex->answers_wait && (*due < 0 || *due > ANSWER_WAIT_MS)) *due = ANSWER_WAIT_MS;
    long held_back = ex->local_count ? locals_due(ex) : -1;
    if (held_back >= 0 && (*due < 0 || held_back < *due)) *due = held_back;
    if (held_back == 0) feed_outlets(ex); /* its time came */
    pthread_mutex_lock(&owner->lock);
    /* Answers are saved with the requests waiting, or now if none waits. */
    if (answered && !owner->count) {
        pthread_mutex_unlock(&owner->lock);
        if (pure_app_save(&ex->app) == PURE_OK) after_save(ex);
        pthread_mutex_lock(&owner->lock);
    }
    /* After a turn that saved, its answers bring requests back from the
     * connection threads: gather them until none has come for 100 µs (a
     * millisecond at most), so they share one save as they would on a
     * single thread, where answering takes that long anyway. The quiet
     * counts only once a request came after the answers: requests that
     * waited out the save alone would otherwise start the next turn
     * without the clients just answered, and the clients would settle
     * into two halves taking turns, each save carrying half of them.
     * Gathering never outlasts the time the last save waited for its
     * device, nor does the quiet: waiting pays only while it costs less
     * than the wait a save of the late requests would take on its own (a
     * sync of milliseconds on a flash card; in RAM next to none, and there
     * gathering would only leave the worker idle). It gathers only when no
     * other app waits for the worker. */
    uint64_t waited = ex->save_waited, budget = waited < 1000000 ? waited : 1000000;
    struct timespec now, end;
    clock_gettime(PURE_WAIT_CLOCK, &now);
    end = later(now, (long)budget);
    while (alone && owner->gather && owner->count && !owner->stopping && before(now, end)) {
        size_t seen = owner->count;
        bool returned = owner->arrivals != owner->answered_at;
        struct timespec quiet = later(now, budget < 100000 ? (long)budget : 100000);
        pthread_cond_timedwait(&owner->waiting, &owner->lock, returned && before(quiet, end) ? &quiet : &end);
        if (returned && owner->count == seen) break;
        clock_gettime(PURE_WAIT_CLOCK, &now);
    }
    size_t count = owner->count, take = alone || count <= TURN ? count : TURN;
    if (!take) {
        pthread_mutex_unlock(&owner->lock);
        return false;
    }
    Job *jobs = owner->jobs;
    size_t capacity = owner->capacity;
    if (take == count) { /* all of them: the arrays trade places */
        owner->jobs = owner->spare;
        owner->capacity = owner->spare_capacity;
        owner->count = 0;
    } else { /* a turn's worth into the spare array; the rest wait for the next */
        if (!grow(&owner->spare, &owner->spare_capacity, take, sizeof(*owner->spare))) abort();
        memcpy(owner->spare, jobs, take * sizeof(*jobs));
        memmove(jobs, jobs + take, (count - take) * sizeof(*jobs));
        owner->count = count - take;
        jobs = owner->spare;
        capacity = owner->spare_capacity;
    }
    owner->spare = NULL; /* it is the turn's now, until it is over */
    owner->spare_capacity = 0;
    pthread_mutex_unlock(&owner->lock);
    ex->wrote = false;
    execute(ex, jobs, take, hand_back, owner);
    owner->gather = ex->wrote;
    pthread_mutex_lock(&owner->lock);
    owner->answered_at = owner->arrivals;
    owner->spare = jobs;
    owner->spare_capacity = capacity;
    bool more = owner->count > 0;
    pthread_mutex_unlock(&owner->lock);
    return more;
}

/* Apps whose timers are due join the queue (turns.lock held); the soonest
 * due of the others (UINT64_MAX: none). */
static uint64_t enqueue_due(uint64_t now) {
    uint64_t soonest = UINT64_MAX;
    for (unsigned i = 0, count = places(); i < count; ++i) {
        Owner *o = &apps[i].owner;
        if (!o->live || !o->due || o->queued || o->running) continue;
        if (o->due <= now) {
            o->due = 0;
            enqueue(o);
        } else if (o->due < soonest) soonest = o->due;
    }
    return soonest;
}

/* A worker: the app at the queue's head takes its turns here, one after
 * another while it has work and no other app waits (it stays on this
 * worker's core, and no other worker is woken for it); then it goes to the
 * queue's tail. An app whose timer is due joins the queue; with none, the
 * worker sleeps until the soonest is due. It stops once the host stops and
 * no app waits. */
static void *run_worker(void *context) {
    (void)context;
    pthread_mutex_lock(&turns.lock);
    for (;;) {
        uint64_t now = now_ms(), soonest = enqueue_due(now);
        Owner *owner = turns.head;
        if (!owner) {
            if (turns.stopping) break;
            if (soonest == UINT64_MAX) pthread_cond_wait(&turns.wake, &turns.lock);
            else {
                struct timespec wake = pure_wait_until((long long)(soonest - now) * 1000000LL);
                pthread_cond_timedwait(&turns.wake, &turns.lock, &wake);
            }
            continue;
        }
        turns.head = owner->next;
        if (!turns.head) turns.tail = NULL;
        else pthread_cond_signal(&turns.wake); /* more apps wait: a free worker takes the next */
        owner->queued = false;
        owner->running = true;
        for (;;) {
            owner->again = false;
            bool alone = !turns.head;
            pthread_mutex_unlock(&turns.lock);
            long due;
            bool more = owner_turn(owner, alone, &due);
            pthread_mutex_lock(&turns.lock);
            ++owner->turns;
            owner->due = due < 0 ? 0 : now_ms() + (uint64_t)due;
            if (!more && !owner->again) break;
            enqueue_due(now_ms());
            if (turns.head) { /* another app waits: this one to the tail, the head here next */
                owner->running = false;
                owner->queued = true;
                owner->next = NULL;
                turns.tail->next = owner;
                turns.tail = owner;
                break;
            }
        }
        owner->running = false;
    }
    pthread_cond_broadcast(&turns.wake); /* the other workers stop too */
    pthread_mutex_unlock(&turns.lock);
    return NULL;
}

/* The surface's table (serve/surface.h). */
static PureSnapshot *surface_read(PureSurfaceHost *s, PureApp *reader) {
    Published *published = &s->owner->published;
    pthread_mutex_lock(&published->lock);
    PureSnapshot *snapshot = published->snapshot;
    if (snapshot) pure_snapshot_hold(snapshot);
    pthread_mutex_unlock(&published->lock);
    if (snapshot) pure_app_read(reader, snapshot);
    return snapshot;
}

static void surface_release(PureSurfaceHost *s, PureSnapshot *snapshot) {
    (void)s;
    pure_snapshot_release(snapshot);
}

/* An event queued at the owner as a request is, waited for until its save. */
static PureResult surface_event(PureSurfaceHost *s, uint32_t input, PureSpan text) {
    Owner *owner = s->owner;
    const PureModule *m = owner->executor.module;
    if (input >= m->inputs || (m->input[input].kind != PURE_KIND_KEY && m->input[input].kind != PURE_KIND_TEXT) ||
        (text.size && !text.data))
        return PURE_INVALID;
    if (text.size > PURE_OBJECT_LIMIT) return PURE_LIMIT;
    unsigned char *value = malloc(4 + text.size); /* as a request's ingress: its size, then its bytes */
    if (!value) return PURE_ALLOCATION;
    put_u32(value, (uint32_t)text.size);
    if (text.size) memcpy(value + 4, text.data, text.size);
    Waiter waiter = {PURE_INVALID, false};
    Job job = {.route = {.kind = ROUTE_INPUT, .target = input}, .waiter = &waiter,
               .effect = {.input = PURE_INPUT_WRITE, .key = "", .expected = -1, .value = {value, 4 + text.size}}};
    pthread_mutex_lock(&owner->lock);
    bool queued = !owner->stopping && grow(&owner->jobs, &owner->capacity, owner->count + 1, sizeof(*owner->jobs));
    if (queued) {
        owner->jobs[owner->count++] = job;
        ++owner->arrivals;
        pthread_cond_signal(&owner->waiting);
    }
    pthread_mutex_unlock(&owner->lock);
    if (queued) schedule(owner);
    pthread_mutex_lock(&s->lock);
    while (queued && !waiter.done) pthread_cond_wait(&s->applied, &s->lock);
    pthread_mutex_unlock(&s->lock);
    free(value);
    return queued ? waiter.status : PURE_INVALID;
}

static PureResult surface_press(PureSurfaceHost *s, uint32_t input) {
    return input < s->owner->executor.module->inputs &&
                   s->owner->executor.module->input[input].kind == PURE_KIND_KEY
               ? surface_event(s, input, (PureSpan){0})
               : PURE_INVALID;
}

static PureResult surface_type(PureSurfaceHost *s, uint32_t input, PureSpan text) {
    return input < s->owner->executor.module->inputs &&
                   s->owner->executor.module->input[input].kind == PURE_KIND_TEXT
               ? surface_event(s, input, text)
               : PURE_INVALID;
}

static bool surface_stopping(PureSurfaceHost *s) {
    (void)s;
    return atomic_load(&stopping);
}

static void *run_surface(void *context) {
    PureSurfaceHost *s = context;
    s->result = s->run(&s->table, s->argc, s->argv);
    fprintf(stderr, "{\"surface\":\"%s\",\"returned\":%d}\n", s->argv[0], s->result);
    return NULL;
}

/* --surface LIBRARY [-- ARGUMENTS]: mapped now, run in its own thread. */
static bool start_surface(PureSurfaceHost *s, Owner *owner, int argc, char **argv) {
    void *library = dlopen(argv[0], RTLD_NOW | RTLD_LOCAL);
    PureSurfaceRun run = library ? (PureSurfaceRun)(uintptr_t)dlsym(library, "pure_surface_run") : NULL;
    if (!run) {
        fprintf(stderr, "--surface %s: %s\n", argv[0], library ? "no pure_surface_run in it" : dlerror());
        return false;
    }
    *s = (PureSurfaceHost){.owner = owner, .run = run, .argc = argc, .argv = argv};
    s->table = (PureSurface){PURE_SURFACE_VERSION, sizeof(PureApp), sizeof(PureModule), owner->executor.module, s,
                             eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC), surface_read, surface_release, surface_press,
                             surface_type, surface_stopping};
    if (s->table.changes < 0 || pthread_mutex_init(&s->lock, NULL) || pthread_cond_init(&s->applied, NULL)) return false;
    owner->executor.surface = s; /* before the thread: its first snapshot may come any time */
    if (pthread_create(&s->thread, NULL, run_surface, s)) {
        owner->executor.surface = NULL;
        return false;
    }
    return true;
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

/* A module from its file for an app, which serves documents when it keeps
 * a keyed collection; false, with why, when it does not load or cannot be
 * served. The module the app had is kept (see retired_modules). */
static bool load_module(App *app, const char *path) {
    PureModule *m = calloc(1, sizeof(*m));
    bool documents;
    char error[160];
    if (!m) {
        snprintf(app->why, sizeof(app->why), "Module: out of memory");
        return false;
    }
    if (pure_module_read(path, m, error, sizeof(error)) != PURE_OK) {
        snprintf(app->why, sizeof(app->why), "Module: %s", error);
        free(m);
        return false;
    }
    /* Documents are served when the module keeps a keyed collection: its keyed
     * messages show and edit read and write one by key. Without one, inputs
     * named show and edit are messages as any other. */
    int show = pure_module_input(m, "show"), edit = pure_module_input(m, "edit");
    documents = m->collections > 0;
    bool served = !documents || (show >= 0 && edit >= 0 && m->input[show].kind == PURE_KIND_MESSAGE && m->input[show].keyed &&
                                 m->input[edit].kind == PURE_KIND_MESSAGE && m->input[edit].keyed);
    if (!served) snprintf(app->why, sizeof(app->why), "Module: documents need keyed messages named show and edit");
    for (uint32_t i = 0; served && i < m->inputs; ++i) {
        bool replied = (documents && i == (uint32_t)edit) || (m->input[i].kind == PURE_KIND_MESSAGE && !m->input[i].keyed);
        if (replied && !replies_borrow(m, i)) {
            snprintf(app->why, sizeof(app->why), "Module: %s may reply only a field of its message", m->input_names[i]);
            served = false;
        }
    }
    struct Retired *kept = served && app->module ? malloc(sizeof(*kept)) : NULL;
    if (served && app->module && !kept) {
        snprintf(app->why, sizeof(app->why), "Module: out of memory");
        served = false;
    }
    if (!served) {
        pure_module_release(m);
        free(m);
        return false;
    }
    if (kept) { /* requests and snapshots may still point to it */
        *kept = (struct Retired){app->module, retired_modules};
        retired_modules = kept;
    }
    app->module = m;
    app->documents = documents;
    app->show = (uint32_t)(show >= 0 ? show : 0);
    app->edit = (uint32_t)(edit >= 0 ? edit : 0);
    return true;
}

/* A connection on this thread for fd (accepted, or moved here); NULL, fd
 * closed, when it is full (the peer sees the close) or out of memory. */
static Connection *adopt(Host *host, int fd) {
    if (host->active == host->capacity) { close(fd); return NULL; }
    Connection *c = host->spare;
    if (c) host->spare = c->next;
    else if (!(c = malloc(sizeof(*c)))) { close(fd); return NULL; }
    size_t index = 0;
    while (host->table[index]) ++index; /* a free index exists: active < capacity */
    c->host = host;
    c->index = index;
    c->fd = fd;
    c->phase = READING_HEAD;
    c->readable = c->writable = c->keep_alive = true;
    c->hung_up = c->settled = false;
    c->deadline = host->now + host->timeout_ms;
    c->ticket = (PureTicket){0};
    c->out = NULL;
    c->out_count = c->out_at = 0;
    c->body = NULL;
    c->opened = 0;
    c->in_used = 0;
    c->hashed_valid = false;
    struct epoll_event event = {.events = EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLET, .data.ptr = c};
    if (epoll_ctl(host->epoll, EPOLL_CTL_ADD, fd, &event)) {
        close(fd);
        c->next = host->spare;
        host->spare = c;
        return NULL;
    }
    host->table[index] = c;
    ++host->active;
    return c;
}

static void accept_all(Host *host) {
    for (;;) {
        int fd = accept4(host->listener, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) return;
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        Connection *c = adopt(host, fd);
        if (!c) continue;
        ++host->accepted;
        drive(c);
    }
}

/* In a host of apps, a connection's requests go to its app's home thread,
 * so an app's requests meet its owner from one thread, as they would from
 * a process of its own: each thread's round hands an owner all its
 * requests at once, and a turn hands its answers back to one thread. A
 * connection moves there once, at its first request (its head read, no
 * ticket taken): the thread gets its descriptor and the bytes read, and
 * reads the request again. Whether it moved. */
static bool moved_home(Connection *c, size_t head_size) {
    c->settled = true;
    const char *path = memchr(c->in, ' ', head_size);
    if (!path || *++path != '/') return false;
    const char *end = path + 1;
    while (end < c->in + head_size && *end != '/' && *end != ' ' && *end != '?') ++end;
    size_t length = (size_t)(end - path - 1);
    App *app = NULL;
    for (unsigned i = 0, count = places(); i < count && !app; ++i)
        if (strlen(apps[i].name) == length && !memcmp(apps[i].name, path + 1, length)) app = &apps[i];
    Host *home = app ? &all_hosts[app->home] : NULL;
    if (!home || home == c->host) return false;
    pthread_mutex_lock(&home->lock);
    bool room = grow(&home->moved, &home->moved_capacity, home->moved_count + 1, sizeof(*home->moved));
    if (room) {
        struct Moved *m = &home->moved[home->moved_count++];
        m->fd = c->fd;
        m->hung_up = c->hung_up;
        m->used = c->in_used;
        memcpy(m->in, c->in, c->in_used);
    }
    pthread_mutex_unlock(&home->lock);
    if (!room) return false;
    uint64_t one = 1;
    if (write(home->wake, &one, sizeof(one)) != sizeof(one)) abort();
    Host *host = c->host; /* here it is gone, its descriptor open there */
    epoll_ctl(host->epoll, EPOLL_CTL_DEL, c->fd, NULL);
    c->phase = CLOSED;
    host->table[c->index] = NULL;
    c->next = host->closed;
    host->closed = c;
    --host->active;
    ++host->moved_away;
    return true;
}

/* The connections moved here: each read again from its bytes. */
static void take_moved(Host *host) {
    pthread_mutex_lock(&host->lock);
    size_t count = host->moved_count;
    struct Moved *moved = count ? malloc(count * sizeof(*moved)) : NULL;
    if (moved) memcpy(moved, host->moved, count * sizeof(*moved));
    else count = 0;
    host->moved_count = 0;
    pthread_mutex_unlock(&host->lock);
    for (size_t i = 0; i < count; ++i) {
        Connection *c = adopt(host, moved[i].fd);
        if (!c) continue;
        c->settled = true;
        c->hung_up = moved[i].hung_up;
        c->in_used = moved[i].used;
        memcpy(c->in, moved[i].in, moved[i].used);
        drive(c);
    }
    free(moved);
}


/* An app's module on its store, with its budgets; false (why says why)
 * when the store does not open or the module does not start on it. */
static bool start_executor(Executor *ex, App *app) {
    ex->module = app->module;
    ex->show = app->show;
    ex->edit = app->edit;
    if (pure_store_open(app->store, &ex->store) != PURE_OK) {
        snprintf(app->why, sizeof(app->why), "Cannot open the store");
        return false;
    }
    if (pure_app_start(&ex->app, app->module, pure_store_data(ex->store)) != PURE_OK || !pure_app_defer(&ex->app, true)) {
        snprintf(app->why, sizeof(app->why), "Cannot start the module on the store%s%s", ex->app.why[0] ? ": " : "",
                 ex->app.why);
        pure_store_close(ex->store);
        ex->store = NULL;
        return false;
    }
    pure_app_background_checkpoints(&ex->app, true);
    pure_app_memory(&ex->app, app->memory);
    pure_app_work(&ex->app, app->work);
    return true;
}

static void stop_executor(Executor *ex) {
    stop_writer(ex);
    if (ex->published) { /* views are the owner's again, which shows none */
        pthread_mutex_lock(&ex->published->lock);
        PureSnapshot *last = ex->published->snapshot;
        ex->published->snapshot = NULL;
        pthread_mutex_unlock(&ex->published->lock);
        pure_snapshot_release(last);
        if (atomic_load_explicit(&ex->published->subscribers, memory_order_relaxed)) wake_pushers(); /* their streams end */
    }
    pure_app_stop(&ex->app);
    pure_store_close(ex->store);
    free(ex->applied);
}

/* One connection thread: its own listener on the shared port, epoll set,
 * runtime domain and, unless an owner runs the app for all of them, app and
 * store handle. */
static bool start_shard(Host *host, size_t budget, unsigned *port) {
    host->wake = -1;
    if (!apps[0].owned && !start_executor(&host->local, &apps[0])) {
        fprintf(stderr, "%s\n", apps[0].why);
        return false;
    }
    /* By app place: the places not taken are memory never touched. */
    host->readers = calloc(app_slots, sizeof(*host->readers));
    host->snapshots = calloc(app_slots, sizeof(*host->snapshots));
    host->read_in = calloc(app_slots, sizeof(*host->read_in));
    host->handed_in = calloc(app_slots, sizeof(*host->handed_in));
    host->touched = calloc(app_slots, sizeof(*host->touched));
    if (!host->readers || !host->snapshots || !host->read_in || !host->handed_in || !host->touched) return false;
    host->work = calloc(host->capacity, sizeof(*host->work));
    host->table = calloc(host->capacity, sizeof(*host->table));
    host->by_slot = calloc(host->capacity, sizeof(*host->by_slot));
    host->jobs = calloc(host->capacity, sizeof(*host->jobs));
    host->routes = calloc(host->capacity, sizeof(*host->routes));
    host->given = calloc(host->capacity, sizeof(*host->given));
    if (!host->work || !host->table || !host->by_slot || !host->jobs || !host->routes || !host->given) {
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
    /* The owners say they handed requests back through an eventfd. */
    if (apps[0].owned) {
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
    host->now = now_ms();
    uint64_t next_sweep = host->now + 1000;
    while (!atomic_load(&stopping)) {
        /* A thread that runs the app ticks its timers on time. */
        long due = apps[0].owned ? -1 : run_timers(&host->local);
        if (host->push_waiting) { /* a pass waits for --push-every-ms */
            uint64_t since = host->now - host->pushed_at;
            long left = since >= push_every_ms ? 0 : (long)(push_every_ms - since);
            if (due < 0 || left < due) due = left;
        }
        int ready = epoll_wait(host->epoll, events, EVENTS, due >= 0 && due < 1000 ? (int)due : 1000);
        host->now = now_ms();
        for (int e = 0; e < ready; ++e) {
            if (events[e].data.u64 & 1u) { /* a subscriber's socket */
                subscriber_event(host, (Subscriber *)(uintptr_t)(events[e].data.u64 & ~(uint64_t)1), events[e].events);
                continue;
            }
            Connection *c = events[e].data.ptr;
            if (!c) { accept_all(host); continue; }
            if (events[e].data.ptr == &host->wake) {
                answer_handed(host);
                take_moved(host);
                continue;
            }
            if (c->phase == CLOSED) continue;
            if (events[e].events & EPOLLERR) { close_connection(c); continue; }
            if (events[e].events & (EPOLLIN | EPOLLRDHUP | EPOLLHUP)) c->readable = true;
            if (events[e].events & (EPOLLRDHUP | EPOLLHUP)) c->hung_up = true;
            if (events[e].events & EPOLLOUT) c->writable = true;
            drive(c);
        }
        serve_bursts(host);
        /* At most one pass every --push-every-ms; the word stays given until
         * one runs, so owners write to this thread at most once a pass. */
        bool told = atomic_load_explicit(&host->push_due, memory_order_acquire);
        if (host->groups && (told || host->push_waiting)) {
            host->push_waiting = host->now - host->pushed_at < push_every_ms;
            if (!host->push_waiting) {
                atomic_store_explicit(&host->push_due, false, memory_order_release); /* a word given meanwhile runs the next */
                push_pass(host);
            }
        }
        uint64_t now = host->now;
        if (now >= next_sweep) {
            next_sweep = now + 1000;
            for (size_t i = 0; i < host->capacity; ++i) {
                Connection *c = host->table[i];
                if (c && c->phase != WAITING && now >= c->deadline) { ++host->expired; close_connection(c); }
            }
            /* A subscriber whose event has not moved for --timeout-ms is let go. */
            for (Group *g = host->groups, *next_group; g; g = next_group) {
                next_group = g->next;
                for (Subscriber *s = g->first, *after; s; s = after) {
                    after = s->next;
                    if (!s->out || now - s->stalled < host->timeout_ms) continue;
                    ++host->expired;
                    if (!s->next && g->first == s) { unsubscribe(host, s); break; }
                    unsubscribe(host, s);
                }
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
    host->subscribers_at_stop = host->subscribers;
    while (host->groups) close_group(host, host->groups);
    return NULL;
}

static void stop_shard(Host *host) {
    if (!apps[0].owned) stop_executor(&host->local);
    for (unsigned i = 0, count = places(); host->readers && i < count; ++i) pure_app_stop(&host->readers[i]);
    free(host->readers);
    free(host->snapshots);
    free(host->read_in);
    free(host->handed_in);
    free(host->touched);
    if (host->listener >= 0) close(host->listener);
    if (host->epoll >= 0) close(host->epoll);
    if (host->wake >= 0) close(host->wake);
    for (unsigned i = 0; i < SESSIONS; ++i) pure_opener_free(host->sessions[i].opener);
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
    for (size_t i = 0; host->given && i < host->capacity; ++i) free(host->given[i].text);
    free(host->given);
    free(host->done);
    free(host->answering);
    free(host->views);
    for (size_t i = 0; i < host->moved_count; ++i) close(host->moved[i].fd);
    free(host->moved);
}

/* The input a link's stream goes to: one taking messages (not keyed) of the
 * sender's shape, or, named its fields, one that takes them by name
 * (pure_module_takes: each record is laid out anew for it). */
/* In a host of apps the name is APP/INPUT. What it finds is a binding: the
 * app's place, the module it runs and the input in it, the listener
 * thread's alone. A delivery whose app runs another module now, or none, is
 * not taken: the receiver keeps it and finds the input again once told
 * (pure_link_listener_rebind, when the app starts). */
static struct Binding { unsigned app; const PureModule *module; uint32_t input; } *bindings;
static size_t binding_count, binding_capacity;
static int link_input(void *context, const char *name, uint32_t shape, const unsigned char *fields, size_t size) {
    (void)context;
    unsigned a = 0;
    if (named_apps) {
        const char *slash = strchr(name, '/');
        size_t length = slash ? (size_t)(slash - name) : 0;
        unsigned count = places();
        for (a = 0; a < count && (strlen(apps[a].name) != length || memcmp(apps[a].name, name, length)); ++a) {}
        if (a == count) return -PURE_LINK_NO_INPUT;
        name = slash + 1;
    }
    const PureModule *m = atomic_load_explicit(&apps[a].live, memory_order_acquire);
    int index = m ? pure_module_input(m, name) : -1;
    if (index < 0 || m->input[index].kind != PURE_KIND_MESSAGE || m->input[index].keyed) return -PURE_LINK_NO_INPUT;
    if (pure_module_shape(m, (uint32_t)index) != shape && !fields) return -PURE_LINK_OTHER_SHAPE;
    if (pure_module_shape(m, (uint32_t)index) != shape && !pure_module_takes(m, (uint32_t)index, fields, size))
        return -PURE_LINK_FIELDS_LACKED;
    for (size_t b = 0; b < binding_count; ++b)
        if (bindings[b].app == a && bindings[b].module == m && bindings[b].input == (uint32_t)index) return (int)b;
    if (binding_count > INT32_MAX - 1 || !grow(&bindings, &binding_capacity, binding_count + 1, sizeof(*bindings)))
        return -PURE_LINK_NO_INPUT;
    bindings[binding_count] = (struct Binding){a, m, (uint32_t)index};
    return (int)binding_count++;
}

/* Records a link delivered, queued at the owner as a request is: applied
 * with the next burst and done (pure_link_done) once its save is over. */
static PureLinkListener *link_listener;
static bool link_deliver(void *context, const PureLinkDelivery *d) {
    (void)context;
    const struct Binding *bound = &bindings[d->input];
    Owner *owner = &apps[bound->app].owner;
    size_t size = 4 + DELIVERY + d->size + d->fields_size, length = strlen(d->source);
    unsigned char *ingress = malloc(size);
    LinkJob *link = malloc(sizeof(*link));
    if (!ingress || !link || d->size > (STORED_RECORDS - 1) || length > PURE_SOURCE) {
        free(ingress);
        free(link);
        return false;
    }
    memset(ingress, 0, 4 + DELIVERY);
    put_u32(ingress, (uint32_t)d->size | DELIVERED | STORED_RECORDS);
    put_u64(ingress + 4, d->first);
    put_u32(ingress + 12, d->count);
    ingress[16] = (unsigned char)length;
    memcpy(ingress + 17, d->source, length);
    memcpy(ingress + 4 + DELIVERY, d->records, d->size);
    if (d->fields_size) memcpy(ingress + 4 + DELIVERY + d->size, d->fields, d->fields_size); /* laid out with those */
    *link = (LinkJob){.listener = link_listener, .token = d->token, .ingress = ingress};
    Job job = {.route = {.kind = ROUTE_INPUT, .target = bound->input, .module = bound->module}, .link = link,
               .effect = {.input = PURE_INPUT_WRITE, .key = "", .expected = -1, .value = {ingress, size}}};
    pthread_mutex_lock(&owner->lock);
    bool queued = !owner->stopping && owner->module == bound->module &&
                  grow(&owner->jobs, &owner->capacity, owner->count + 1, sizeof(*owner->jobs));
    if (queued) {
        owner->jobs[owner->count++] = job;
        ++owner->arrivals;
        pthread_cond_signal(&owner->waiting);
    }
    pthread_mutex_unlock(&owner->lock);
    if (!queued) {
        free(ingress);
        free(link);
    } else schedule(owner);
    return queued;
}

/* A wire delivered a send: the owner trims it. */
static void wire_delivered(void *context) {
    Owner *owner = context;
    pthread_mutex_lock(&owner->lock);
    owner->delivered = true;
    pthread_cond_signal(&owner->waiting);
    pthread_mutex_unlock(&owner->lock);
    schedule(owner);
}

/* --send OUTPUT=URL: that output's saved sends go to url, from NAME.OUTPUT. */
static unsigned char own_public[PURE_SEAL_KEY]; /* this node's key's public half, when it has one */
/* send=OUTPUT=APP/INPUT in a host of apps: that output's sends handed to
 * another app of it (Local), its input taking the output's shape or, by
 * name, its fields. */
static bool open_local(App *app, uint32_t output, const char *to, const char *source, char *error, size_t size) {
    *error = 0;
    const char *slash = strchr(to, '/');
    size_t length = slash ? (size_t)(slash - to) : 0;
    App *receiver = NULL;
    for (unsigned a = 0, count = places(); named_apps && slash && a < count && !receiver; ++a)
        if (strlen(apps[a].name) == length && !memcmp(apps[a].name, to, length) && !atomic_load(&apps[a].gone))
            receiver = &apps[a];
    if (!receiver || !receiver->owned || strlen(slash + 1) > PURE_MODULE_NAME) {
        snprintf(error, size, "no app '%.*s' in this host", (int)length, to);
        return false;
    }
    Executor *ex = &app->owner.executor;
    Local *l = calloc(1, sizeof(*l));
    if (!l) {
        snprintf(error, size, "no memory");
        return false;
    }
    *l = (Local){.from = &app->owner, .to = &receiver->owner, .output = output, .sender = app->module};
    strcpy(l->input_name, slash + 1);
    size_t named = strnlen(source, PURE_SOURCE);
    memcpy(l->source, source, named);
    l->source[named] = 0;
    /* A receiver that runs is checked now; one that does not, when it does
     * (its sends wait meanwhile, as a link's wait for its node). */
    if ((receiver->running && !local_bind(l, receiver->module, error, size)) || pthread_mutex_init(&l->lock, NULL)) {
        if (!*error) snprintf(error, size, "no lock");
        free(l);
        return false;
    }
    ex->locals[ex->local_count++] = l;
    return true;
}

static bool open_wires(App *app, const char *name) {
    Executor *ex = &app->owner.executor;
    const PureModule *m = app->module;
    const char *const *sends = app->sends;
    unsigned count = app->send_count;
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
        /* An output goes one way: its outbox is trimmed as far as its outlet's receiver stored. */
        int wired = pure_module_input(m, output);
        for (unsigned k = 0; wired >= 0 && k < ex->wire_count + ex->link_count + ex->local_count; ++k)
            if (outlet_output(ex, k) == (uint32_t)wired) {
                fprintf(stderr, "send=%s: the output is wired already\n", sends[i]);
                return false;
            }
        int written = snprintf(source, sizeof(source), "%s.%s", name, output);
        if (!strstr(equals + 1, "://")) { /* another app of the host: APP/INPUT */
            int index = pure_module_input(m, output);
            if (index < 0 || m->input[index].kind != PURE_KIND_OUTPUT) snprintf(error, sizeof(error), "no output '%s'", output);
            else if (written <= 0 || written > PURE_SOURCE) snprintf(error, sizeof(error), "the name and output are longer than 64 bytes");
            if (index < 0 || m->input[index].kind != PURE_KIND_OUTPUT || written <= 0 || written > PURE_SOURCE ||
                !open_local(app, (uint32_t)index, equals + 1, source, error, sizeof(error))) {
                fprintf(stderr, "send=%s: %s\n", sends[i], error);
                return false;
            }
            continue;
        }
        if (!strncmp(equals + 1, "pure://", 7)) { /* a link: no connection, sealed with the node's key */
            int index = pure_module_input(m, output);
            if (index < 0 || m->input[index].kind != PURE_KIND_OUTPUT) snprintf(error, sizeof(error), "no output '%s'", output);
            PureLink *link = index >= 0 && m->input[index].kind == PURE_KIND_OUTPUT && written > 0 && written <= PURE_SOURCE
                                 ? pure_link_open(equals + 1, source, pure_module_shape(m, (uint32_t)index), seal_key,
                                                  own_public, error, sizeof(error))
                                 : NULL;
            unsigned char named[PURE_FIELDS_NAMED];
            if (link && !pure_link_fields(link, named, pure_module_fields(m, (uint32_t)index, named))) {
                pure_link_close(link);
                link = NULL;
                snprintf(error, sizeof(error), "no memory");
            }
            if (!link) {
                fprintf(stderr, "--send %s: %s\n", sends[i], written > PURE_SOURCE ? "the name and output are longer than 64 bytes" : error);
                return false;
            }
            ex->link_output[ex->link_count] = (uint32_t)index;
            ex->links[ex->link_count++] = link;
            continue;
        }
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
static bool wire_seals(Executor *ex, const PureModule *m, const char *const *seals, unsigned count,
                       const unsigned char *own_key) {
    char error[512];
    if (count && !pure_seal_ready(error, sizeof(error))) {
        fprintf(stderr, "--seal: %s\n", error);
        return false;
    }
    for (unsigned i = 0; i < count; ++i) {
        const char *equals = strchr(seals[i], '=');
        unsigned char peer[PURE_SEAL_KEY];
        char output[PURE_MODULE_NAME];
        size_t length = equals ? (size_t)(equals - seals[i]) : 0;
        PureWire *wire = NULL;
        PureLink *link = NULL;
        if (length && length < sizeof(output)) {
            memcpy(output, seals[i], length);
            output[length] = 0;
            for (unsigned k = 0; k < ex->wire_count; ++k)
                if (!strcmp(m->input_names[pure_wire_output(ex->wires[k])], output)) wire = ex->wires[k];
            for (unsigned k = 0; k < ex->link_count; ++k)
                if (!strcmp(m->input_names[ex->link_output[k]], output)) link = ex->links[k];
        }
        if (link && pure_seal_peer(equals + 1, peer)) { /* the link's receiver */
            pure_link_peer(link, peer);
            continue;
        }
        if (!wire || !pure_seal_peer(equals + 1, peer)) {
            fprintf(stderr, "--seal takes OUTPUT=KEY for an output given to --send, the key as 64 hex digits: %s\n", seals[i]);
            return false;
        }
        pure_wire_seal(wire, peer, seal_key, own_key); /* sealed with this node's key when it has one */
    }
    return true;
}

/* --sender NAME=KEY: deliveries from source NAME are taken only sealed with
 * that key (hex); with any, only named senders are taken. */
static bool name_senders(const char *const *named, unsigned count) {
    for (unsigned i = 0; i < count; ++i) {
        const char *equals = strchr(named[i], '=');
        size_t length = equals ? (size_t)(equals - named[i]) : 0;
        struct Sender *sender = &senders[sender_count];
        if (!length || length > PURE_SOURCE || !pure_seal_peer(equals + 1, sender->key)) {
            fprintf(stderr, "--sender takes NAME=KEY, the key as 64 hex digits: %s\n", named[i]);
            return false;
        }
        memcpy(sender->name, named[i], length);
        sender->name[length] = 0;
        ++sender_count;
    }
    return true;
}

/* --senders FILE: senders a --link-port takes besides those named, a line
 * each (NAME KEY, the key as 64 hex digits), as many as memory holds. */
static bool allow_senders(PureLinkReceiver *receiver, const char *path, char *error, size_t size) {
    if (!path) return true;
    FILE *file = fopen(path, "r");
    if (!file) {
        snprintf(error, size, "--senders %s: %s", path, strerror(errno));
        return false;
    }
    char *line = NULL;
    size_t capacity = 0, count = 0;
    bool ok = true;
    for (ssize_t length; ok && (length = getline(&line, &capacity, file)) >= 0; ++count) {
        while (length && (line[length - 1] == '\n' || line[length - 1] == '\r')) line[--length] = 0;
        char *space = strchr(line, ' ');
        unsigned char key[PURE_SEAL_KEY];
        if (!length) continue;
        if (!space || space == line || (size_t)(space - line) > PURE_SOURCE || !pure_seal_peer(space + 1, key)) {
            snprintf(error, size, "--senders %s: line %zu is not NAME KEY", path, count + 1);
            ok = false;
            break;
        }
        *space = 0;
        for (const char *c = line; *c && ok; ++c) ok = *c > ' ' && *c <= '~';
        if (!ok) snprintf(error, size, "--senders %s: line %zu names no source", path, count + 1);
        else ok = pure_link_allow(receiver, line, key, error, size);
    }
    free(line);
    fclose(file);
    return ok;
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
/* An app's owner, its executor started on the app's store, its snapshot
 * published; false (why says why) when the app cannot start. */
/* An owner's locks, made once, with its app's place (other apps' outlets
 * lock them before the app starts, or when it never does). */
static bool make_owner(Owner *owner) {
    if (!owner->made && (pthread_mutex_init(&owner->lock, NULL) || pure_cond_monotonic(&owner->waiting) ||
                         pthread_mutex_init(&owner->published.lock, NULL)))
        return false;
    owner->made = true;
    return true;
}

static bool start_owner(App *app, Host *hosts, unsigned threads) {
    Owner *owner = &app->owner;
    if (!make_owner(owner)) {
        snprintf(app->why, sizeof(app->why), "Cannot make the owner's locks");
        return false;
    }
    if (!start_executor(&owner->executor, app)) return false;
    owner->executor.reads_first = true;
    owner->executor.published = &owner->published;
    owner->hosts = hosts;
    owner->host_count = threads;
    return true;
}

/* A line of --apps: an app as the file gives it, its strings its own. */
typedef struct {
    char name[APP_NAME + 1];
    char *module, *store, *tokens;
    uint64_t digest, tokens_digest; /* of the module's file, and the tokens', when they were read (0: they could not be) */
    size_t memory, queue;
    uint64_t work;
    char *sends[PURE_MODULE_INPUTS], *seals[PURE_MODULE_INPUTS], *replies[PURE_MODULE_INPUTS];
    unsigned send_count, seal_count, reply_count;
    bool kept;        /* its app runs as the line says: a reading leaves it be */
    bool tokens_only; /* and so but for its tokens: they are put in place, and it runs on */
} Line;

static void let_go_line(Line *line) {
    free(line->module);
    free(line->store);
    free(line->tokens);
    for (unsigned i = 0; i < line->send_count; ++i) free(line->sends[i]);
    for (unsigned i = 0; i < line->seal_count; ++i) free(line->seals[i]);
    for (unsigned i = 0; i < line->reply_count; ++i) free(line->replies[i]);
}

/* A file's bytes' digest (FNV-1a, 64 bits); 0 when it cannot be read. */
static uint64_t file_digest(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) return 0;
    uint64_t digest = 14695981039346656037u;
    unsigned char bytes[16384];
    for (size_t n; (n = fread(bytes, 1, sizeof(bytes), file));)
        for (size_t i = 0; i < n; ++i) digest = (digest ^ bytes[i]) * 1099511628211u;
    bool read = !ferror(file);
    fclose(file);
    return read && digest ? digest : 1;
}

/* --apps FILE: an app a line, NAME MODULE STORE, its budgets (memory=BYTES
 * work=N queue=N) and its wiring (send=, seal=, reply=), # beginning a
 * comment. False, why in error, when the file cannot be read, a line is not
 * an app, or none is. */
static bool parse_apps(const char *path, Line **out, unsigned *count, char *error, size_t size) {
    FILE *file = fopen(path, "r");
    if (!file) {
        snprintf(error, size, "--apps %s: %s", path, strerror(errno));
        return false;
    }
    Line *lines = NULL;
    unsigned n = 0, at = 0;
    char text[4096];
    bool ok = true;
    while (ok && fgets(text, sizeof(text), file)) {
        ++at;
        char *hash = strchr(text, '#'), *words[3 + 3 + 3 * PURE_MODULE_INPUTS], *rest = NULL;
        if (hash) *hash = 0;
        unsigned found = 0;
        for (char *word = strtok_r(text, " \t\r\n", &rest); word && found < sizeof(words) / sizeof(*words);
             word = strtok_r(NULL, " \t\r\n", &rest))
            words[found++] = word;
        if (!found) continue;
        Line *grown = found >= 3 ? realloc(lines, (n + 1) * sizeof(*lines)) : NULL;
        if (!grown) {
            snprintf(error, size, "--apps %s:%u: NAME MODULE STORE [memory=BYTES] [work=N] [queue=N] [send=OUTPUT=URL ...]",
                     path, at);
            ok = false;
            break;
        }
        lines = grown;
        Line *line = &lines[n];
        memset(line, 0, sizeof(*line));
        size_t length = strlen(words[0]);
        bool named = length <= APP_NAME;
        for (size_t i = 0; named && i < length; ++i) {
            char c = words[0][i];
            named = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        }
        for (unsigned i = 0; named && i < n; ++i) named = strcmp(lines[i].name, words[0]) != 0;
        if (!named) {
            snprintf(error, size, "--apps %s:%u: a name is 1-32 of a-z 0-9 _ -, each app's its own", path, at);
            ok = false;
            break;
        }
        memcpy(line->name, words[0], length + 1);
        ok = (line->module = strdup(words[1])) && (line->store = strdup(words[2]));
        for (unsigned w = 3; ok && w < found; ++w) {
            char *value = strchr(words[w], '=');
            uint64_t number_given;
            if (value) *value++ = 0;
            if (value && !strcmp(words[w], "memory") && number(value, SIZE_MAX, &number_given)) line->memory = (size_t)number_given;
            else if (value && !strcmp(words[w], "work") && number(value, UINT64_MAX, &number_given)) line->work = number_given;
            else if (value && !strcmp(words[w], "queue") && number(value, SIZE_MAX, &number_given)) line->queue = (size_t)number_given;
            else if (value && !strcmp(words[w], "send") && line->send_count < PURE_MODULE_INPUTS)
                ok = (line->sends[line->send_count++] = strdup(value)) != NULL;
            else if (value && !strcmp(words[w], "seal") && line->seal_count < PURE_MODULE_INPUTS)
                ok = (line->seals[line->seal_count++] = strdup(value)) != NULL;
            else if (value && !strcmp(words[w], "reply") && line->reply_count < PURE_MODULE_INPUTS)
                ok = (line->replies[line->reply_count++] = strdup(value)) != NULL;
            else if (value && !strcmp(words[w], "tokens") && !line->tokens && *value)
                ok = (line->tokens = strdup(value)) != NULL;
            else {
                snprintf(error, size, "--apps %s:%u: an app takes memory=BYTES, work=N, queue=N, send=OUTPUT=URL, "
                         "seal=OUTPUT=KEY, reply=OUTPUT=INPUT and tokens=FILE", path, at);
                ok = false;
            }
        }
        if (!ok && !*error) snprintf(error, size, "--apps %s:%u: out of memory", path, at);
        if (!ok) {
            let_go_line(line);
            break;
        }
        line->digest = file_digest(line->module);
        line->tokens_digest = line->tokens ? file_digest(line->tokens) : 0;
        ++n;
    }
    fclose(file);
    if (ok && !n) {
        snprintf(error, size, "--apps %s: no app", path);
        ok = false;
    }
    if (!ok) {
        for (unsigned i = 0; i < n; ++i) let_go_line(&lines[i]);
        free(lines);
        return false;
    }
    *out = lines;
    *count = n;
    return true;
}

/* An app takes its line: its module's file, store, budgets and wiring (the
 * strings move to it; those it had go). Its module is not read here. */
static void take_line(App *app, Line *line) {
    free(app->module_path);
    free(app->tokens_path);
    free((char *)app->store);
    for (unsigned i = 0; i < app->send_count; ++i) free((char *)app->sends[i]);
    for (unsigned i = 0; i < app->seal_count; ++i) free((char *)app->seals[i]);
    for (unsigned i = 0; i < app->reply_count; ++i) free((char *)app->replies[i]);
    app->module_path = line->module;
    app->digest = line->digest;
    app->tokens_path = line->tokens;
    app->tokens_digest = line->tokens_digest;
    app->store = line->store;
    app->memory = line->memory;
    atomic_store(&app->work, line->work);
    app->queue = line->queue;
    memcpy(app->sends, line->sends, sizeof(app->sends));
    memcpy(app->seals, line->seals, sizeof(app->seals));
    memcpy(app->replies, line->replies, sizeof(app->replies));
    app->send_count = line->send_count, app->seal_count = line->seal_count, app->reply_count = line->reply_count;
    *line = (Line){.kept = line->kept, .tokens_only = line->tokens_only};
    memcpy(line->name, app->name, sizeof(line->name));
}

static bool same_strings(const char *const *a, unsigned a_count, char *const *b, unsigned b_count) {
    for (unsigned i = 0; a_count == b_count && i < a_count; ++i)
        if (strcmp(a[i], b[i])) return false;
    return a_count == b_count;
}

/* Whether an app's tokens are as a line gives them: the same file, its bytes too. */
static bool same_tokens(const App *app, const Line *line) {
    return !app->tokens_path == !line->tokens &&
           (!line->tokens || (!strcmp(app->tokens_path, line->tokens) && app->tokens_digest == line->tokens_digest));
}

/* Whether an app runs as a line says: the same module (its file's bytes
 * too), store, budgets and wiring (its tokens apart: same_tokens). */
static bool same_line(const App *app, const Line *line) {
    return app->module_path && !strcmp(app->module_path, line->module) && app->digest == line->digest &&
           !strcmp(app->store, line->store) && app->memory == line->memory && atomic_load(&app->work) == line->work &&
           app->queue == line->queue && same_strings(app->sends, app->send_count, line->sends, line->send_count) &&
           same_strings(app->seals, app->seal_count, line->seals, line->seal_count) &&
           same_strings(app->replies, app->reply_count, line->replies, line->reply_count);
}

/* An app's tokens file (docs/design/017's 3b): a line a token, GRANT
 * DIGEST, # beginning a comment; GRANT is read, write or input=NAME[,NAME],
 * DIGEST the token's SHA-256 in 64 hex digits. NULL, why in error, when it
 * cannot be read or a line is not one. */
static Tokens *read_tokens(const char *path, char *error, size_t size) {
    FILE *file = fopen(path, "r");
    if (!file) {
        snprintf(error, size, "Tokens: %s: %s", path, strerror(errno));
        return NULL;
    }
    Tokens *tokens = calloc(1, sizeof(*tokens));
    char text[1024];
    unsigned at = 0;
    while (tokens && fgets(text, sizeof(text), file)) {
        ++at;
        char *hash = strchr(text, '#'), *rest = NULL;
        if (hash) *hash = 0;
        char *grant = strtok_r(text, " \t\r\n", &rest), *digest = grant ? strtok_r(NULL, " \t\r\n", &rest) : NULL;
        if (!grant) continue;
        struct Grant g = {0};
        bool ok = digest && strlen(digest) == 64 && !strtok_r(NULL, " \t\r\n", &rest);
        for (unsigned i = 0; ok && i < 32; ++i) {
            int high = hex_value(digest[2 * i]), low = hex_value(digest[2 * i + 1]);
            ok = high >= 0 && low >= 0;
            g.digest[i] = (unsigned char)(high << 4 | low);
        }
        if (ok && !strcmp(grant, "read")) g.kind = GRANT_READ;
        else if (ok && !strcmp(grant, "write")) g.kind = GRANT_WRITE;
        else if (ok && !strncmp(grant, "input=", 6) && grant[6] && strlen(grant + 6) < sizeof(g.inputs)) {
            g.kind = GRANT_INPUT;
            strcpy(g.inputs, grant + 6);
        } else {
            snprintf(error, size, "Tokens: %s:%u: a line is read, write or input=NAME[,NAME], then the token's SHA-256 "
                     "(64 hex digits)", path, at);
            free(tokens);
            tokens = NULL;
            break;
        }
        Tokens *grown = realloc(tokens, sizeof(*tokens) + (tokens->count + 1) * sizeof(struct Grant));
        if (!grown) {
            free(tokens);
            tokens = NULL;
            break;
        }
        tokens = grown;
        tokens->grant[tokens->count++] = g;
    }
    fclose(file);
    if (!tokens && !*error) snprintf(error, size, "Tokens: out of memory");
    return tokens;
}

/* An app's tokens as its tokens file says them now, in the place of those it
 * had (kept: see Tokens). False, why in app->why, when the file is not one. */
static bool give_tokens(App *app) {
    Tokens *tokens = NULL;
    char why[sizeof(app->why)] = "";
    if (app->tokens_path && !(tokens = read_tokens(app->tokens_path, why, sizeof(why)))) {
        snprintf(app->why, sizeof(app->why), "%s", why);
        return false;
    }
    const Tokens *before = atomic_exchange_explicit(&app->tokens, tokens, memory_order_acq_rel);
    struct RetiredTokens *kept = before ? malloc(sizeof(*kept)) : NULL;
    if (kept) *kept = (struct RetiredTokens){(Tokens *)before, retired_tokens}, retired_tokens = kept;
    if (all_hosts && atomic_load_explicit(&app->owner.published.subscribers, memory_order_relaxed))
        wake_pushers(); /* its streams are let in again by the new tokens, or end */
    return true;
}

/* A host of apps' first reading: each app in a place, room left for apps
 * started later (slots), its module read (an app whose module does not
 * load is kept, stopped, with why). False, why on stderr, when the file is
 * not one of apps. */
static bool read_apps(const char *path, unsigned slots) {
    Line *lines;
    unsigned count;
    char error[512];
    if (!parse_apps(path, &lines, &count, error, sizeof(error))) {
        fprintf(stderr, "%s\n", error);
        return false;
    }
    app_slots = slots > count ? slots : count;
    if (!(apps = calloc(app_slots, sizeof(*apps)))) return false;
    for (unsigned i = 0; i < count; ++i) {
        App *app = &apps[i];
        memcpy(app->name, lines[i].name, sizeof(app->name));
        app->owned = true; /* every app of a host of apps takes turns on the workers */
        if (!make_owner(&app->owner)) return false;
        take_line(app, &lines[i]);
        load_module(app, app->module_path); /* else stopped, with why */
    }
    atomic_store(&app_count, count);
    free(lines);
    return true;
}

/* A JSON string's quotes and escapes around text. */
static void print_json(FILE *out, const char *text) {
    fputc('"', out);
    for (const unsigned char *c = (const unsigned char *)text; *c; ++c)
        if (*c == '"' || *c == '\\') fprintf(out, "\\%c", *c);
        else if (*c < 0x20) fprintf(out, "\\u%04x", *c);
        else fputc(*c, out);
    fputc('"', out);
}

static unsigned online_cpus(void) {
    cpu_set_t set;
    long cpus = sched_getaffinity(0, sizeof(set), &set) ? sysconf(_SC_NPROCESSORS_ONLN) : CPU_COUNT(&set);
    return cpus < 1 ? 1 : cpus > 256 ? 256 : (unsigned)cpus;
}

static unsigned auto_threads(bool owner) {
    unsigned cpus = online_cpus();
    return owner && cpus > 1 ? cpus - 1 : cpus;
}


/* ---- Apps that start and stop while the host runs (docs/design/017's stage 3) */

/* An executor's counts, once it stopped, added to what its app did. */
static void fold_run(const Executor *ex, Counts *c) {
    c->bursts += ex->bursts, c->events += ex->burst_events, c->ticks += ex->ticks;
    c->memory_refused += ex->memory_refused, c->windows_full += ex->windows_full, c->publications += ex->publications;
    c->answers += ex->answers, c->handed += ex->handed, c->checkpoints += ex->checkpoints;
}

/* An executor's outlets closed, their counts added: what they did not
 * deliver stays in the outbox, and goes again when the app starts again. */
static void close_outlets(Executor *ex, Counts *c) {
    for (unsigned i = 0; i < ex->wire_count; ++i) {
        uint64_t delivered, attempts;
        pure_wire_counts(ex->wires[i], &delivered, &attempts);
        c->delivered += delivered, c->attempts += attempts;
        pure_wire_close(ex->wires[i]);
    }
    for (unsigned i = 0; i < ex->link_count; ++i) {
        PureLinkCounts k;
        pure_link_counts(ex->links[i], &k);
        c->sent.packets += k.packets, c->sent.records += k.records, c->sent.resent += k.resent, c->sent.lost += k.lost;
        c->sent.acknowledged += k.acknowledged, c->sent.received += k.received, c->sent.stored += k.stored;
        c->sent.refused += k.refused;
        pure_link_close(ex->links[i]);
    }
    for (unsigned i = 0; i < ex->local_count; ++i) {
        free(ex->locals[i]->batch);
        pthread_mutex_destroy(&ex->locals[i]->lock);
        free(ex->locals[i]);
    }
    ex->wire_count = ex->link_count = ex->local_count = 0;
}

/* The workers: an app's owner takes its turns on them, none its own
 * thread. One for the one app of a host; for apps, as many as run, at most
 * a CPU each (or --workers): made as apps start, never let go. The main
 * thread's, and the control's once the host runs. */
static pthread_t *workers_made;
static unsigned worker_count, worker_room, workers_asked;
static bool more_workers(void) {
    unsigned owners = 0, cpus = online_cpus();
    for (unsigned a = 0, count = places(); a < count; ++a) owners += apps[a].running && apps[a].owned;
    unsigned want = !owners ? 0 : workers_asked ? workers_asked : !named_apps ? 1 : owners < cpus ? owners : cpus;
    for (; worker_count < want; ++worker_count) {
        if (worker_count == worker_room) {
            unsigned room = worker_room ? 2 * worker_room : 4;
            pthread_t *grown = realloc(workers_made, room * sizeof(*grown));
            if (!grown) return false;
            workers_made = grown, worker_room = room;
        }
        if (pthread_create(&workers_made[worker_count], NULL, run_worker, NULL)) return false;
        pthread_setname_np(workers_made[worker_count], "pure-owner"); /* their CPU, seen apart */
    }
    return true;
}

/* An app whose executor runs its module takes turns and requests: its
 * outlets made for that module and its snapshot published; then the link
 * streams to its inputs find them again. False, why in app->why, when its
 * sends are wired wrong or an outlet does not start: it is then stopped,
 * its store closed. */
static bool resume_app(App *app) {
    Owner *o = &app->owner;
    Executor *ex = &o->executor;
    o->queue = app->queue;
    bool wired = open_wires(app, app->name) && wire_replies(ex, app->module, app->replies, app->reply_count) &&
                 wire_seals(ex, app->module, app->seals, app->seal_count, seal_key ? own_public : NULL);
    if (!wired) snprintf(app->why, sizeof(app->why), "Its sends are wired wrong (said above)");
    pthread_mutex_lock(&o->lock); /* it takes requests resolved with its module */
    o->module = wired ? app->module : NULL;
    o->stopping = !wired;
    pthread_mutex_unlock(&o->lock);
    if (wired) after_save(ex); /* the snapshot, and the sends stored before */
    char error[256] = "";
    for (unsigned i = 0; wired && i < ex->wire_count; ++i)
        if (!pure_wire_start(ex->wires[i], wire_delivered, o)) snprintf(error, sizeof(error), "Cannot start a wire");
    for (unsigned i = 0; wired && !*error && i < ex->link_count; ++i)
        pure_link_start(ex->links[i], wire_delivered, o, error, sizeof(error));
    if (!wired || *error) {
        if (*error) snprintf(app->why, sizeof(app->why), "%s", error);
        pthread_mutex_lock(&o->lock);
        o->module = NULL;
        o->stopping = true;
        pthread_mutex_unlock(&o->lock);
        close_outlets(ex, &app->past);
        stop_executor(ex);
        fold_run(ex, &app->past);
        return false;
    }
    pthread_mutex_lock(&turns.lock);
    o->live = true;
    pthread_mutex_unlock(&turns.lock);
    atomic_store(&app->gone, false);
    atomic_store_explicit(&app->live, app->module, memory_order_release);
    app->running = true;
    if (link_listener) {
        char prefix[APP_NAME + 2];
        snprintf(prefix, sizeof(prefix), "%s/", app->name);
        pure_link_listener_rebind(link_listener, prefix);
    }
    schedule(o); /* a first turn: its timers begin */
    return true;
}

/* An app of a host of apps starts in its place: its store (none another
 * running app has), its owner on it, then it resumes. False, why in
 * app->why, when it cannot: it stays stopped. */
static bool start_app(App *app) {
    struct stat mine, theirs;
    app->why[0] = 0;
    if (!give_tokens(app)) return false; /* its routes are never open for a tokens file that is not one */
    if (pure_store_init(app->store) != PURE_OK) snprintf(app->why, sizeof(app->why), "Cannot initialize the store directory");
    else if (stat(app->store, &mine)) snprintf(app->why, sizeof(app->why), "Cannot read the store directory");
    for (unsigned b = 0, count = places(); !app->why[0] && b < count; ++b)
        if (&apps[b] != app && apps[b].running && !stat(apps[b].store, &theirs) && theirs.st_dev == mine.st_dev &&
            theirs.st_ino == mine.st_ino)
            snprintf(app->why, sizeof(app->why), "The store is %s's", apps[b].name);
    if (app->why[0]) return false;
    Executor *ex = &app->owner.executor;
    *ex = (Executor){0};
    if (!start_owner(app, all_hosts, host_count)) return false;
    return resume_app(app);
}

/* An app goes quiet: it takes no request from now on (its routes answer
 * 503 stopped, link records and other apps' batches wait at their
 * senders), applies, saves and answers those it took, has its batches
 * with other apps back, then closes its outlets (what they did not deliver
 * stays in its outbox) and lets its checkpoint writer finish. Its owner
 * takes no turn after; its executor and store are as they were. */
static void quiet_app(App *app) {
    app->running = false;
    atomic_store_explicit(&app->live, NULL, memory_order_release);
    Owner *o = &app->owner;
    Executor *ex = &o->executor;
    pthread_mutex_lock(&o->lock);
    o->stopping = true;
    o->module = NULL;
    pthread_cond_signal(&o->waiting); /* it may be gathering */
    pthread_mutex_unlock(&o->lock);
    for (bool done = false; !done;) {
        pthread_mutex_lock(&o->lock);
        bool taken = o->count > 0;
        pthread_mutex_unlock(&o->lock);
        bool out = false;
        for (unsigned i = 0; i < ex->local_count; ++i) {
            pthread_mutex_lock(&ex->locals[i]->lock);
            out = out || ex->locals[i]->out;
            pthread_mutex_unlock(&ex->locals[i]->lock);
        }
        pthread_mutex_lock(&turns.lock);
        done = !taken && !out && !o->queued && !o->running;
        if (done) o->live = false;
        pthread_mutex_unlock(&turns.lock);
        if (!done) nanosleep(&(struct timespec){0, 1000000}, NULL);
    }
    close_outlets(ex, &app->past);
    stop_writer(ex);
    o->gather = false;
    o->due = 0;
}

/* An app stops: quiet, then its store closed. */
static void stop_app(App *app) {
    if (!app->running) return;
    quiet_app(app);
    Executor *ex = &app->owner.executor;
    stop_executor(ex);
    fold_run(ex, &app->past);
}

/* An app that runs changes in place, its line changed but not its store:
 * quiet, then its module replaced in memory when it is another
 * (pure_app_replace: its states, and the sends in its outbox, carried by
 * name; nothing read again from the store), its budgets as the line says,
 * and it resumes with outlets made anew. What its executor kept by input
 * or output number starts anew. A module its states do not go into stops
 * it, why in app->why (its store closed: a replace left uncertain starts
 * again from what the store has). */
static bool change_app(App *app) {
    Executor *ex = &app->owner.executor;
    if (!give_tokens(app)) { /* never open for a tokens file that is not one */
        char why[sizeof(app->why)];
        memcpy(why, app->why, sizeof(why));
        stop_app(app);
        memcpy(app->why, why, sizeof(why));
        return false;
    }
    quiet_app(app);
    if (ex->module != app->module && pure_app_replace(&ex->app, app->module) != PURE_OK) {
        snprintf(app->why, sizeof(app->why), "Its states do not go into the module%s%s", ex->app.why[0] ? ": " : "",
                 ex->app.why);
        stop_executor(ex);
        fold_run(ex, &app->past);
        return false;
    }
    ex->module = app->module;
    ex->show = app->show, ex->edit = app->edit;
    memset(ex->next, 0, sizeof(ex->next));
    memset(ex->trimmed, 0, sizeof(ex->trimmed));
    memset(ex->held, 0, sizeof(ex->held));
    memset(ex->link_output, 0, sizeof(ex->link_output));
    memset(ex->link_trimmed, 0, sizeof(ex->link_trimmed));
    memset(ex->local_trimmed, 0, sizeof(ex->local_trimmed));
    memset(ex->outlet_from, 0, sizeof(ex->outlet_from));
    ex->gone_seen = ex->app.sends_gone;
    ex->answers_wait = false;
    pure_app_memory(&ex->app, app->memory);
    pure_app_work(&ex->app, atomic_load(&app->work));
    return resume_app(app);
}

/* A reading of --apps on SIGHUP: the host made as the file says. Apps whose
 * lines are gone stop and their routes go (404 no_app); a running app whose
 * line changed but not its store changes in place (change_app); one whose
 * store changed, one a save of which is uncertain, and one that does not
 * run start (again) from their stores; new ones start in places of their
 * own; the others are left be. A line out for each reading: each app,
 * whether it runs (and why not), and what was done (kept, changed,
 * restarted, started, removed). */
static unsigned readings;
static void read_again(const char *path) {
    Line *lines = NULL;
    unsigned count = 0, before = places();
    char error[512];
    ++readings;
    const char **did = calloc(app_slots, sizeof(*did));
    if (!did || !parse_apps(path, &lines, &count, error, sizeof(error))) {
        if (!did) snprintf(error, sizeof(error), "out of memory");
        printf("{\"reading\":%u,\"error\":", readings);
        print_json(stdout, error);
        printf("}\n");
        fflush(stdout);
        free(did);
        return;
    }
    /* First what goes or changes stops: a store an app lets go another may take. */
    for (unsigned a = 0; a < before; ++a) {
        App *app = &apps[a];
        if (atomic_load(&app->gone)) continue;
        Line *line = NULL;
        for (unsigned i = 0; i < count && !line; ++i)
            if (!strcmp(lines[i].name, app->name)) line = &lines[i];
        if (!line) {
            stop_app(app);
            atomic_store(&app->gone, true);
            did[a] = "removed";
            continue;
        }
        bool uncertain = app->running && __atomic_load_n(&app->owner.executor.app.uncertain, __ATOMIC_RELAXED);
        if (app->running && !uncertain && same_line(app, line)) {
            did[a] = same_tokens(app, line) ? "kept" : "changed"; /* its tokens alone: put in place below */
            line->kept = same_tokens(app, line);
            line->tokens_only = !line->kept;
            continue;
        }
        if (app->running && !uncertain && !strcmp(app->store, line->store)) {
            did[a] = "changed"; /* in place, below */
            continue;
        }
        did[a] = app->running ? "restarted" : "started";
        stop_app(app);
    }
    /* Each new name a place, before any starts: an app may send to one
     * written after it. */
    for (unsigned i = 0; i < count; ++i) {
        unsigned a = 0, taken = places();
        while (a < taken && strcmp(apps[a].name, lines[i].name)) ++a;
        if (a < taken || taken == app_slots || !make_owner(&apps[a].owner)) continue;
        App *app = &apps[a];
        memcpy(app->name, lines[i].name, sizeof(app->name));
        app->owned = true;
        app->home = a % host_count;
        atomic_store(&app->gone, false);
        atomic_store_explicit(&app_count, taken + 1, memory_order_release); /* its name is there now */
        did[a] = "started";
    }
    for (unsigned i = 0; i < count; ++i) {
        Line *line = &lines[i];
        unsigned a = 0, taken = places();
        while (a < taken && strcmp(apps[a].name, line->name)) ++a;
        if (line->kept) continue;
        if (a == taken) {
            fprintf(stderr, "{\"app\":\"%s\",\"error\":\"no place for it (--app-slots %u)\"}\n", line->name, app_slots);
            continue;
        }
        App *app = &apps[a];
        if (line->tokens_only) { /* it runs on, its tokens new */
            take_line(app, line);
            app->why[0] = 0;
            if (!give_tokens(app)) {
                char why[sizeof(app->why)];
                memcpy(why, app->why, sizeof(why));
                stop_app(app);
                memcpy(app->why, why, sizeof(why));
            }
            continue;
        }
        bool same_module = app->module && app->module_path && !strcmp(app->module_path, line->module) &&
                           app->digest == line->digest && line->digest;
        bool in_place = app->running; /* left running by the first pass: its store stays */
        atomic_store(&app->gone, false); /* in the file again: stopped, not gone, until it starts */
        if (!did[a]) did[a] = "started";
        take_line(app, line);
        app->why[0] = 0;
        if (!same_module && !load_module(app, app->module_path)) { /* stopped, with why */
            if (in_place) stop_app(app);
            continue;
        }
        if (in_place) change_app(app);
        else start_app(app);
    }
    more_workers();
    printf("{\"reading\":%u,\"apps\":[", readings);
    bool first = true;
    for (unsigned a = 0, taken = places(); a < taken; ++a) {
        App *app = &apps[a];
        if (atomic_load(&app->gone) && !did[a]) continue;
        printf("%s{\"name\":", first ? "" : ",");
        first = false;
        print_json(stdout, app->name);
        printf(",\"running\":%s", app->running ? "true" : "false");
        if (did[a]) printf(",\"did\":\"%s\"", did[a]);
        if (!app->running && !atomic_load(&app->gone)) {
            printf(",\"why\":");
            print_json(stdout, app->why);
        }
        printf("}");
    }
    printf("]}\n");
    fflush(stdout);
    for (unsigned i = 0; i < count; ++i) let_go_line(&lines[i]);
    free(lines);
    free(did);
}

/* The thread that reads --apps again: on SIGHUP, which every other thread
 * blocks, until the host stops (it is sent one more then). */
static void *run_apps(void *context) {
    const char *path = context;
    sigset_t hangup;
    sigemptyset(&hangup);
    sigaddset(&hangup, SIGHUP);
    for (int signal_number; !sigwait(&hangup, &signal_number) && !atomic_load(&stopping);) read_again(path);
    return NULL;
}

/* pure-serve --make-token GRANT: a new token (32 random bytes, hex) and the
 * line of a tokens file that grants it GRANT (docs/design/017's 3b). */
static int make_token(const char *grant) {
    bool named = !strcmp(grant, "read") || !strcmp(grant, "write") || (!strncmp(grant, "input=", 6) && grant[6]);
    unsigned char bytes[32], digest[32];
    if (!named || strlen(grant) > 200 || strpbrk(grant, " \t\r\n#")) {
        fputs("--make-token read|write|input=NAME[,NAME]\n", stderr);
        return 2;
    }
    if (getrandom(bytes, sizeof(bytes), 0) != (ssize_t)sizeof(bytes)) {
        perror("getrandom");
        return 1;
    }
    char token[65], line[65];
    for (unsigned i = 0; i < 32; ++i) snprintf(token + 2 * i, 3, "%02x", bytes[i]);
    pure_sha256(token, 64, digest);
    for (unsigned i = 0; i < 32; ++i) snprintf(line + 2 * i, 3, "%02x", digest[i]);
    printf("{\"token\":\"%s\",\"line\":\"%s %s\"}\n", token, grant, line);
    return 0;
}

int main(int argc, char **argv) {
    static PureSurfaceHost surface;
    /* What follows -- is the surface's: its library first, then its arguments. */
    int options = argc, surface_argc = 0;
    char *surface_argv[64];
    for (int i = 1; i < argc; ++i)
        if (!strcmp(argv[i], "--")) {
            options = i;
            break;
        }
    const char *surface_path = NULL, *module_path = NULL, *store = NULL, *name = NULL, *sends[PURE_MODULE_INPUTS], *replies[PURE_MODULE_INPUTS];
    const char *seals[PURE_MODULE_INPUTS], *named[PURE_MODULE_INPUTS], *key_path = NULL, *senders_path = NULL, *apps_path = NULL;
    const char *tokens_path = NULL;
    if (argc == 3 && !strcmp(argv[1], "--make-token")) return make_token(argv[2]);
    unsigned send_count = 0, reply_count = 0, seal_count = 0, named_count = 0;
    unsigned port = 8767, threads = 1, link_port = 0, workers = 0, slots = 256;
    size_t budget = 64u << 20, capacity = 1024;
    uint64_t timeout_ms = 10000, link_idle_ms = 0;
    for (int i = 1; i < options; i += 2) {
        uint64_t value;
        if (!strcmp(argv[i], "--sealed-only")) { sealed_only = true; --i; continue; }
        if (i + 1 == options) goto usage;
        if (!strcmp(argv[i], "--module")) module_path = argv[i + 1];
        else if (!strcmp(argv[i], "--apps")) apps_path = argv[i + 1];
        else if (!strcmp(argv[i], "--tokens")) tokens_path = argv[i + 1];
        else if (!strcmp(argv[i], "--surface")) surface_path = argv[i + 1];
        else if (!strcmp(argv[i], "--key")) key_path = argv[i + 1];
        else if (!strcmp(argv[i], "--seal") && seal_count < PURE_MODULE_INPUTS) seals[seal_count++] = argv[i + 1];
        else if (!strcmp(argv[i], "--store")) store = argv[i + 1];
        else if (!strcmp(argv[i], "--send") && send_count < PURE_MODULE_INPUTS) sends[send_count++] = argv[i + 1];
        else if (!strcmp(argv[i], "--name")) name = argv[i + 1];
        else if (!strcmp(argv[i], "--send-ca")) pure_wire_trust(argv[i + 1]);
        else if (!strcmp(argv[i], "--reply") && reply_count < PURE_MODULE_INPUTS) replies[reply_count++] = argv[i + 1];
        else if (!strcmp(argv[i], "--sender") && named_count < PURE_MODULE_INPUTS) named[named_count++] = argv[i + 1];
        else if (!strcmp(argv[i], "--port") && number(argv[i + 1], 65535, &value)) port = (unsigned)value;
        else if (!strcmp(argv[i], "--link-port") && number(argv[i + 1], 65535, &value) && value) link_port = (unsigned)value;
        else if (!strcmp(argv[i], "--senders")) senders_path = argv[i + 1];
        else if (!strcmp(argv[i], "--link-idle-ms") && number(argv[i + 1], 86400000, &value) && value) link_idle_ms = value;
        else if (!strcmp(argv[i], "--budget") && number(argv[i + 1], SIZE_MAX, &value) && value) budget = (size_t)value;
        else if (!strcmp(argv[i], "--memory") && number(argv[i + 1], SIZE_MAX, &value) && value) memory_budget = (size_t)value;
        else if (!strcmp(argv[i], "--work") && number(argv[i + 1], UINT64_MAX, &value) && value) work_budget = value;
        else if (!strcmp(argv[i], "--window") && number(argv[i + 1], SIZE_MAX, &value) && value) window_bytes = (size_t)value;
        else if (!strcmp(argv[i], "--connections") && number(argv[i + 1], 1u << 20, &value) && value) capacity = (size_t)value;
        else if (!strcmp(argv[i], "--threads") && !strcmp(argv[i + 1], "auto")) threads = 0;
        else if (!strcmp(argv[i], "--threads") && number(argv[i + 1], 256, &value) && value) threads = (unsigned)value;
        else if (!strcmp(argv[i], "--workers") && number(argv[i + 1], 256, &value) && value) workers = (unsigned)value;
        else if (!strcmp(argv[i], "--app-slots") && number(argv[i + 1], 65536, &value) && value) slots = (unsigned)value;
        else if (!strcmp(argv[i], "--timeout-ms") && number(argv[i + 1], 3600000, &value) && value) timeout_ms = value;
        else if (!strcmp(argv[i], "--subscribers") && number(argv[i + 1], 1u << 22, &value)) subscribers_most = (size_t)value;
        else if (!strcmp(argv[i], "--push-every-ms") && number(argv[i + 1], 60000, &value)) push_every_ms = value;
        else if (!strcmp(argv[i], "--view-workers") && number(argv[i + 1], 256, &value)) view_workers = (unsigned)value;
        else goto usage;
    }
    if (surface_path) surface_argv[surface_argc++] = (char *)surface_path;
    for (int i = options + 1; i < argc && surface_argc < 63; ++i) surface_argv[surface_argc++] = argv[i];
    surface_argv[surface_argc] = NULL;
    if (options < argc && !surface_path) goto usage;
    if (apps_path) { /* a host of apps: each its own module, store and budgets; none sends yet (017, stage 1) */
        if (module_path || store || name || send_count || reply_count || seal_count || surface_path || memory_budget ||
            work_budget || tokens_path) {
            fputs("--apps gives each app its module, store, budgets, wiring (send=, seal=, reply=) and tokens (tokens=); "
                  "--name and --surface are for a host of one app (--module)\n", stderr);
            return 2;
        }
        /* SIGHUP is the reading thread's (run_apps): blocked before any thread is made, as they all inherit it. */
        sigset_t hangup;
        sigemptyset(&hangup);
        sigaddset(&hangup, SIGHUP);
        pthread_sigmask(SIG_BLOCK, &hangup, NULL);
        if (!read_apps(apps_path, slots)) return 2;
        named_apps = true;
    } else {
        if (!module_path || !store || !(apps = calloc(1, sizeof(*apps)))) goto usage;
        app_count = 1;
        apps[0].store = store;
        apps[0].memory = memory_budget;
        apps[0].work = work_budget;
        if (!load_module(&apps[0], module_path)) {
            fprintf(stderr, "%s\n", apps[0].why);
            goto usage;
        }
        /* States (and outputs, whose sends are the app's state) have one
         * owner, whose snapshots the connection threads read. */
        bool outputs = false;
        for (uint32_t i = 0; i < apps[0].module->inputs; ++i) outputs = outputs || apps[0].module->input[i].kind == PURE_KIND_OUTPUT;
        apps[0].owned = apps[0].module->states || outputs;
        apps[0].running = true;
        if (tokens_path && (!(apps[0].tokens_path = strdup(tokens_path)) || !give_tokens(&apps[0]))) {
            fprintf(stderr, "--tokens: %s\n", apps[0].why);
            return 2;
        }
    }
    if (!threads) threads = auto_threads(apps[0].owned);
    char public_key[2 * PURE_SEAL_KEY + 1] = "";
    if (key_path) {
        char error[256];
        if (!(seal_key = pure_seal_key(key_path, public_key, error, sizeof(error)))) { fprintf(stderr, "--key %s\n", error); return 2; }
    }
    if ((sealed_only || named_count) && !seal_key) { fputs("--sealed-only and --sender need --key\n", stderr); return 2; }
    if (!name_senders(named, named_count)) return 2;
    if (seal_key && !pure_seal_peer(public_key, own_public)) return 2;
    signal(SIGPIPE, SIG_IGN);
    struct sigaction stop_action = {.sa_handler = stop};
    sigemptyset(&stop_action.sa_mask);
    sigaction(SIGINT, &stop_action, NULL);
    sigaction(SIGTERM, &stop_action, NULL);
    Host *hosts = calloc(threads, sizeof(*hosts));
    pthread_t *thread = calloc(threads, sizeof(*thread));
    if (!hosts || !thread || pure_cond_monotonic(&turns.wake)) return 1;
    all_hosts = hosts;
    host_count = threads;
    for (unsigned a = 0; a < app_count; ++a) apps[a].home = a % threads; /* each app's connections meet on one thread */
    /* Each app's store, and its owner: an app that cannot start stops alone
     * (in a host of apps; the one app of a host stops the host). Two apps
     * never share a store. */
    for (unsigned a = 0; named_apps && a < app_count; ++a)
        if (apps[a].module) start_app(&apps[a]); /* else stopped, with why */
    for (unsigned a = 0; !named_apps && a < app_count; ++a) {
        App *app = &apps[a];
        if (!app->running) continue;
        struct stat mine, theirs;
        bool shared = false;
        if (pure_store_init(app->store) != PURE_OK) snprintf(app->why, sizeof(app->why), "Cannot initialize the store directory");
        else if (stat(app->store, &mine)) snprintf(app->why, sizeof(app->why), "Cannot read the store directory");
        else
            for (unsigned b = 0; b < a && !shared; ++b)
                if (apps[b].running && !stat(apps[b].store, &theirs) && theirs.st_dev == mine.st_dev && theirs.st_ino == mine.st_ino) {
                    snprintf(app->why, sizeof(app->why), "The store is %s's", apps[b].name);
                    shared = true;
                }
        app->running = !app->why[0] && (!app->owned || start_owner(app, hosts, threads));
        if (!app->running) {
            fprintf(stderr, "%s\n", app->why);
            return 1;
        }
    }
    /* The sender's name: --name, or the module file's name without .pmod. */
    char stem[PURE_SOURCE + 1];
    if (!named_apps && !name) {
        const char *base = strrchr(module_path, '/') ? strrchr(module_path, '/') + 1 : module_path;
        size_t length = strlen(base);
        if (length > 5 && !strcmp(base + length - 5, ".pmod")) length -= 5;
        snprintf(stem, sizeof(stem), "%.*s", (int)length, base);
        name = stem;
    }
    Owner *owner = &apps[0].owner; /* the one app's, in a host of one */
    if (!named_apps) { /* its --send, --seal and --reply */
        memcpy(apps[0].sends, sends, send_count * sizeof(*sends));
        memcpy(apps[0].seals, seals, seal_count * sizeof(*seals));
        memcpy(apps[0].replies, replies, reply_count * sizeof(*replies));
        apps[0].send_count = send_count, apps[0].seal_count = seal_count, apps[0].reply_count = reply_count;
    }
    /* The one app's wiring, its sends' source --name.OUTPUT (an app of
     * --apps wired itself as it started, APP.OUTPUT). */
    if (!named_apps && apps[0].owned &&
        (!open_wires(&apps[0], name) || !wire_replies(&owner->executor, apps[0].module, apps[0].replies, apps[0].reply_count) ||
         !wire_seals(&owner->executor, apps[0].module, apps[0].seals, apps[0].seal_count, seal_key ? own_public : NULL)))
        return 2;
    for (unsigned a = 0; !named_apps && a < app_count; ++a) {
        App *app = &apps[a];
        if (!app->running) continue;
        if (app->owned) { /* it takes requests resolved with its module, and turns */
            app->owner.module = app->module;
            app->owner.live = true;
            after_save(&app->owner.executor); /* the snapshot, and the sends stored before */
        }
        atomic_store(&app->live, app->module);
    }
    for (unsigned a = 0; !named_apps && a < app_count; ++a) {
        Owner *o = &apps[a].owner;
        if (!apps[a].running || !apps[a].owned) continue;
        for (unsigned i = 0; i < o->executor.wire_count; ++i)
            if (!pure_wire_start(o->executor.wires[i], wire_delivered, o)) { fputs("Cannot start a wire\n", stderr); return 1; }
        for (unsigned i = 0; i < o->executor.link_count; ++i) {
            char error[256];
            if (!pure_link_start(o->executor.links[i], wire_delivered, o, error, sizeof(error))) {
                fprintf(stderr, "--send %s\n", error);
                return 2;
            }
        }
    }
    for (unsigned i = 0; i < threads; ++i) {
        hosts[i].capacity = capacity;
        hosts[i].timeout_ms = timeout_ms;
        hosts[i].listener = hosts[i].epoll = -1;
        if (!start_shard(&hosts[i], budget, &port)) return 1;
    }
    workers_asked = workers;
    if (!more_workers()) { fputs("Cannot start a worker\n", stderr); return 1; }
    if (!named_apps && apps[0].owned) schedule(owner); /* a first turn: its timers begin (an app of --apps had its) */
    if (link_port) { /* records from the senders named, over UDP */
        char error[256];
        if ((!named_apps && !apps[0].owned) || !seal_key) {
            fputs("--link-port needs an app with states and the node's --key\n", stderr);
            return 2;
        }
        PureLinkReceiver *receiver =
            pure_link_receiver(seal_key, own_public, &(PureLinkHost){link_input, link_deliver, NULL}, error, sizeof(error));
        bool allowed = receiver != NULL && allow_senders(receiver, senders_path, error, sizeof(error));
        for (unsigned i = 0; allowed && i < sender_count; ++i)
            allowed = pure_link_allow(receiver, senders[i].name, senders[i].key, error, sizeof(error));
        if (allowed && link_idle_ms) pure_link_idle(receiver, link_idle_ms);
        char memo[4096];
        snprintf(memo, sizeof(memo), "%s.links", key_path); /* beside the key the secrets come from */
        allowed = allowed && pure_link_remember(receiver, memo, error, sizeof(error)) &&
                  (link_listener = pure_link_listen(receiver, link_port, error, sizeof(error)));
        if (!allowed || !pure_link_serve(link_listener, error, sizeof(error))) {
            fprintf(stderr, "--link-port: %s\n", error);
            return 2;
        }
    }
    if (named_apps) { /* each app, and why one stopped */
        printf("{\"port\":%u,\"threads\":%u,\"workers\":%u,\"apps\":[", port, threads, worker_count);
        for (unsigned a = 0; a < app_count; ++a) {
            printf("%s{\"name\":", a ? "," : "");
            print_json(stdout, apps[a].name);
            printf(",\"running\":%s", apps[a].running ? "true" : "false");
            if (!apps[a].running) {
                printf(",\"why\":");
                print_json(stdout, apps[a].why);
            }
            printf("}");
        }
        printf("]%s%s%s}\n", *public_key ? ",\"public_key\":\"" : "", public_key, *public_key ? "\"" : "");
    } else { /* its name too: its sends' sources are NAME.OUTPUT, which a receiver's --sender must name */
        printf("{\"port\":%u,\"threads\":%u,\"name\":\"", port, threads);
        for (const char *c = name; *c; ++c) printf(*c == '"' || *c == '\\' ? "\\%c" : "%c", *c);
        if (*public_key) printf("\",\"public_key\":\"%s\"}\n", public_key);
        else printf("\"}\n");
    }
    fflush(stdout);
    /* A host of apps reads its file again on SIGHUP, on a thread of its own. */
    pthread_t reading;
    if (named_apps && pthread_create(&reading, NULL, run_apps, (void *)apps_path)) { fputs("Cannot start a thread\n", stderr); return 1; }
    if (named_apps) pthread_setname_np(reading, "pure-apps");
    /* A surface reads the owner's snapshots and gives it events. */
    if (surface_path && (named_apps || !apps[0].owned)) { fputs("--surface needs an app with states\n", stderr); return 2; }
    if (surface_path && !start_surface(&surface, owner, surface_argc, surface_argv)) return 1;
    if (apps[0].owned && !start_view_workers()) { fputs("Cannot start a thread\n", stderr); return 1; }
    for (unsigned i = 1; i < threads; ++i)
        if (pthread_create(&thread[i], NULL, run_shard, &hosts[i])) { fputs("Cannot start a thread\n", stderr); return 1; }
    run_shard(&hosts[0]);
    for (unsigned i = 1; i < threads; ++i) pthread_join(thread[i], NULL);
    stop_view_workers(); /* the connection threads had back all they handed */
    if (surface_path) { /* stopped before the owner, which may still apply its last event */
        surface_changed(&surface);
        pthread_join(surface.thread, NULL);
    }
    if (named_apps) { /* a reading under way ends first */
        pthread_kill(reading, SIGHUP);
        pthread_join(reading, NULL);
    }
    for (unsigned a = 0, count = places(); a < count; ++a) { /* no gathering from now on */
        Owner *o = &apps[a].owner;
        if (!apps[a].running || !apps[a].owned) continue;
        pthread_mutex_lock(&o->lock);
        o->stopping = true;
        pthread_cond_signal(&o->waiting);
        pthread_mutex_unlock(&o->lock);
    }
    pthread_mutex_lock(&turns.lock);
    turns.stopping = true;
    pthread_cond_broadcast(&turns.wake);
    pthread_mutex_unlock(&turns.lock);
    for (unsigned i = 0; i < worker_count; ++i) pthread_join(workers_made[i], NULL);
    PureLinkCounts sent = {0}, heard = {0}; /* what links sent, and what the listener took */
    pure_link_listener_close(link_listener, &heard); /* after the owner, which tells it what it stored */
    size_t active = 0, retained = 0, state_bytes = 0;
    uint64_t unauthorized = 0, forbidden = 0, subscribed = 0, unsubscribed = 0, pushes = 0, pushed = 0, replaced = 0;
    size_t subscribers = 0;
    uint64_t moved = 0, handed = 0, accepted = 0, requests = 0, bursts = 0, events = 0, expired = 0, ticks = 0, shown = 0, pooled = 0, checkpoints = 0,
             memory_refused = 0, windows_full = 0, publications = 0, answers = 0, delivered = 0, attempts = 0;
    for (unsigned i = 0; i < threads; ++i) {
        shown += hosts[i].shown, pooled += hosts[i].pooled;
        moved += hosts[i].moved_away;
        active += hosts[i].active;
        retained += hosts[i].runtime.retained;
        accepted += hosts[i].accepted;
        requests += hosts[i].requests;
        bursts += hosts[i].local.bursts;
        events += hosts[i].local.burst_events;
        ticks += hosts[i].local.ticks;
        memory_refused += hosts[i].local.memory_refused;
        expired += hosts[i].expired;
        unauthorized += hosts[i].unauthorized, forbidden += hosts[i].forbidden;
        subscribers += hosts[i].subscribers_at_stop, subscribed += hosts[i].subscribed, unsubscribed += hosts[i].unsubscribed;
        pushes += hosts[i].pushes, pushed += hosts[i].pushed_events, replaced += hosts[i].replaced;
        stop_shard(&hosts[i]);
    }
    for (unsigned a = 0, count = places(); a < count; ++a) { /* each app's runs, the present one stopped now */
        App *app = &apps[a];
        Counts *c = &app->past;
        if (app->running && app->owned) {
            Executor *ex = &app->owner.executor;
            state_bytes += pure_app_state_bytes(&ex->app);
            close_outlets(ex, c); /* what was not delivered stays in the outbox */
            stop_executor(ex);
            fold_run(ex, c);
        }
        free(app->owner.jobs);
        free(app->owner.spare);
        bursts += c->bursts, events += c->events, ticks += c->ticks, memory_refused += c->memory_refused;
        windows_full += c->windows_full, publications += c->publications, answers += c->answers, handed += c->handed;
        checkpoints += c->checkpoints, delivered += c->delivered, attempts += c->attempts;
        sent.packets += c->sent.packets, sent.records += c->sent.records, sent.resent += c->sent.resent;
        sent.lost += c->sent.lost, sent.acknowledged += c->sent.acknowledged, sent.received += c->sent.received;
        sent.stored += c->sent.stored, sent.refused += c->sent.refused;
    }
    unsigned long groups, grouped;
    pure_store_group_counts(&groups, &grouped);
    fprintf(stderr, "{\"active\":%zu,\"retained_payload_bytes\":%zu,\"accepted\":%" PRIu64 ",\"requests\":%" PRIu64
            ",\"bursts\":%" PRIu64 ",\"burst_events\":%" PRIu64 ",\"ticks\":%" PRIu64 ",\"expired\":%" PRIu64
            ",\"threads\":%u,\"owner\":%s,\"snapshots\":%" PRIu64 ",\"views_from_snapshots\":%" PRIu64
            ",\"view_workers\":%u,\"views_by_workers\":%" PRIu64
            ",\"delivered\":%" PRIu64 ",\"delivery_attempts\":%" PRIu64 ",\"answers\":%" PRIu64
            ",\"commit_groups\":%lu,\"grouped_commits\":%lu,\"checkpoints\":%" PRIu64
            ",\"link_packets\":%" PRIu64 ",\"link_records\":%" PRIu64 ",\"link_resent\":%" PRIu64
            ",\"link_lost\":%" PRIu64 ",\"link_acknowledged\":%" PRIu64 ",\"link_stored\":%" PRIu64
            ",\"link_refused\":%" PRIu64 ",\"listener_packets\":%" PRIu64 ",\"listener_received\":%" PRIu64
            ",\"listener_deliveries\":%" PRIu64 ",\"listener_woken\":%" PRIu64 ",\"listener_remembered\":%" PRIu64
            ",\"listener_swept\":%" PRIu64 ",\"listener_active\":%" PRIu64 ",\"listener_keyed\":%" PRIu64
            ",\"listener_secrets\":%" PRIu64 ",\"listener_epochs\":%" PRIu64 ",\"listener_refused\":%" PRIu64
            ",\"state_bytes\":%zu,\"memory_refused\":%" PRIu64 ",\"windows_full\":%" PRIu64
            ",\"unauthorized\":%" PRIu64 ",\"forbidden\":%" PRIu64 ",\"subscribers\":%zu,\"subscribed\":%" PRIu64
            ",\"pushes\":%" PRIu64 ",\"pushed\":%" PRIu64 ",\"push_replaced\":%" PRIu64,
            active, retained, accepted, requests, bursts, events, ticks, expired, threads, apps[0].owned ? "true" : "false",
            publications, shown, view_thread_count, pooled, delivered, attempts, answers, groups, grouped,
            checkpoints, sent.packets, sent.records, sent.resent, sent.lost, sent.acknowledged, sent.stored,
            sent.refused, heard.packets, heard.received, heard.deliveries, heard.woken, heard.remembered, heard.swept,
            heard.active, heard.keyed, heard.secrets, heard.epochs, heard.refused, state_bytes, memory_refused,
            windows_full, unauthorized, forbidden, subscribers, subscribed, pushes, pushed, replaced);
    if (named_apps) { /* what each app did */
        fprintf(stderr, ",\"workers\":%u,\"moved_home\":%" PRIu64 ",\"handed\":%" PRIu64 ",\"apps\":[",
                worker_count, moved, handed);
        for (unsigned a = 0, count = places(); a < count; ++a) {
            const App *app = &apps[a];
            fprintf(stderr, "%s{\"name\":", a ? "," : "");
            print_json(stderr, app->name);
            fprintf(stderr, ",\"running\":%s,\"turns\":%" PRIu64 ",\"bursts\":%" PRIu64 ",\"events\":%" PRIu64
                    ",\"busy\":%" PRIu64 ",\"memory_refused\":%" PRIu64 "}", app->running ? "true" : "false",
                    app->owner.turns, app->past.bursts, app->past.events, app->owner.busy, app->past.memory_refused);
        }
        fputc(']', stderr);
    }
    fputs("}\n", stderr);
    free(hosts);
    free(thread);
    free(workers_made);
    return 0;
usage:
    fputs("pure-serve --module FILE.pmod --store DIRECTORY [--port 8767] [--threads 1|N|auto] [--budget BYTES] "
          "[--connections 1024] [--timeout-ms 10000] [--subscribers 65536] [--push-every-ms 10] [--view-workers N] [--send OUTPUT=URL ...] [--name NAME] "
          "[--send-ca FILE] [--reply OUTPUT=INPUT ...] [--seal OUTPUT=KEY ...] [--key FILE] [--sealed-only] "
          "[--sender NAME=KEY ...] [--link-port PORT] [--senders FILE] [--link-idle-ms 30000] [--memory BYTES] [--work N] "
          "[--window 4194304] [--tokens FILE] [--surface LIBRARY [-- ARGUMENTS]]\n"
          "pure-serve --apps FILE [--port 8767] [--threads 1|N|auto] [--workers N] [--app-slots 256] [--budget BYTES] [--connections 1024] "
          "[--timeout-ms 10000] [--subscribers 65536] [--push-every-ms 10] [--view-workers N] [--key FILE] [--sealed-only] [--sender NAME=KEY ...] [--link-port PORT] [--senders FILE] "
          "[--send-ca FILE] [--window BYTES]\n"
          "  --apps FILE: a host of apps (docs/design/017-apps-on-one-host.md), a line each:\n"
          "      NAME MODULE.pmod STORE [memory=BYTES] [work=N] [queue=N] [send=OUTPUT=URL ...] [seal=OUTPUT=KEY ...]\n"
          "      [reply=OUTPUT=INPUT ...] [tokens=FILE]; its routes start /NAME/, its sends' sources are NAME.OUTPUT, and a\n"
          "      --link-port takes its inputs as NAME/INPUT. On SIGHUP the host reads FILE again: the apps of lines\n"
          "      gone stop, those of new lines start, those of changed lines change in place (their states kept in\n"
          "      memory) unless their store moved; a line out says what it did\n"
          "  --app-slots N: the apps a host of apps holds at most, those started later included\n"
          "  --tokens FILE (tokens=FILE for an app of --apps): its routes take only requests bearing a token\n"
          "      (Authorization: Bearer TOKEN) the file grants, a line each: read|write|input=NAME[,NAME] SHA256-HEX;\n"
          "      a delivery sealed by a key --sender names needs none (docs/design/017-apps-on-one-host.md, 3b)\n"
          "pure-serve --make-token read|write|input=NAME[,NAME]: a new token and its line for a tokens file\n"
          "  --workers N: the threads the apps' owners take turns on (1 for one app; for apps, one a CPU at most)\n"
          "  --view-workers N: the threads that show views with a body, asked and pushed, off the connection threads\n"
          "      (a CPU each; 0: on the connection threads, docs/design/020); past an app's queue=N, 503 busy\n"
          "  --send OUTPUT=pure://host:port/INPUT: to a Pure node's --link-port, the way between nodes (UDP, no\n"
          "      connection, docs/design/012-link-protocol.md; its key by --seal OUTPUT=KEY)\n"
          "  --send OUTPUT=http[s]://host:port/path: to anything else, over HTTP\n"
          "  --senders FILE: more senders the --link-port takes, a line each: NAME KEY\n"
          "  --name NAME: the node's name, its sends' sources being NAME.OUTPUT (the module file's name without\n"
          "      .pmod unless given; the first line out says it)\n"
          "  --memory BYTES: what the app's states may hold (lists' items, texts, sends not yet stored): an event\n"
          "      past it is refused, 503 memory (docs/design/013-memory-budget.md)\n"
          "  --work N: what an event, or one evaluation of a view, may run, in instructions counted as written: past\n"
          "      it the event is refused and the view has no value, 422 work (docs/design/017-apps-on-one-host.md)\n"
          "  --window BYTES: what each --send holds of its sends beyond what its receiver has (and 65,536 sends);\n"
          "      the outbox keeps the rest\n", stderr);
    return 2;
}
