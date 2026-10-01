#define _POSIX_C_SOURCE 200809L
#include "store.h"
#include "program.h"
#include "store-port.h"
#ifdef PURE_EMBEDDED_PROGRAM
#include "embedded-plan.h"
#endif
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <poll.h>
#ifdef PURE_HTTP_ASYNC
#include <pthread.h>
#endif
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#if defined(PURE_HTTP_ASYNC) && !defined(PURE_ASYNC_EFFECTS)
#error PURE_HTTP_ASYNC requires PURE_ASYNC_EFFECTS
#endif

enum { SLOTS = 8, HEADER_LIMIT = 4096, CHUNK = 65536 };
typedef enum { READING_HEADER, READING_BODY, WRITING, WAITING_EFFECT } Phase;
typedef struct Server Server;
#ifdef PURE_HTTP_ASYNC
typedef enum { JOB_FREE, JOB_QUEUED, JOB_RUNNING, JOB_DONE } JobState;
typedef struct {
    PureEffect effect;
    PureResource resource;
    PureSpan reply;
    PureResult status;
    int64_t revision;
    JobState state;
} EffectJob;
typedef struct { Server *server; PureInput input; } EffectWorker;
#endif
typedef struct {
    int fd;
    Phase phase;
    uint64_t deadline;
    char request[HEADER_LIMIT + 1], response[512], key[65];
    size_t header_size, body_size, received;
    unsigned char *body;
    bool put;
    int64_t expected;
    PureTicket ticket;
    PureTransfer transfer;
    Server *server;
} Connection;
struct Server {
    const char *path;
#ifndef PURE_EMBEDDED_PROGRAM
    PureProgram program;
#endif
    PureRuntime runtime;
    PureWork work[SLOTS];
    size_t budget, active, peak_active;
    uint64_t accepted, expired, prefix_copy_bytes;
    uint64_t timeout_ms;
    int send_buffer;
    Connection connections[SLOTS];
#ifdef PURE_HTTP_ASYNC
    PureHost effect_host;
    pthread_t worker[2];
    EffectWorker worker_context[2];
    pthread_mutex_t job_mutex;
    pthread_cond_t job_ready[2];
    EffectJob jobs[SLOTS];
    size_t next_job[2];
    int effect_wake;
    bool worker_stop;
#endif
};
static volatile sig_atomic_t stopping;
static volatile sig_atomic_t wake_write = -1;
static void stop_server(int sig) {
    (void)sig;
    int saved_errno = errno;
    stopping = 1;
    if (wake_write >= 0) {
        const char byte = 1;
        if (write((int)wake_write, &byte, 1) < 0) { /* full: its reader is woken already */ }
    }
    errno = saved_errno;
}

static uint64_t now_ms(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) abort();
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}

static bool number(const char *text, uint64_t maximum, uint64_t *value) {
    if (!*text) return false;
    uint64_t n = 0;
    for (; *text; ++text) {
        if (*text < '0' || *text > '9') return false;
        unsigned digit = (unsigned)(*text - '0');
        if (digit > maximum || n > (maximum - digit) / 10) return false;
        n = n * 10 + digit;
    }
    *value = n;
    return true;
}

static void release_payload(void *owner) {
    Connection *c = owner;
    if (c->ticket.runtime) {
        bool released = pure_runtime_release(c->ticket);
        assert(released);
        (void)released;
    }
    c->ticket = (PureTicket){0};
    c->body = NULL;
}

static void close_connection(Connection *c) {
    if (c->fd < 0) return;
    if (c->transfer.active) pure_transfer_abort(&c->transfer);
    else release_payload(c);
    close(c->fd);
    --c->server->active;
    *c = (Connection){.fd = -1};
}

static void respond(Connection *c, int status, const char *reason, const char *error,
                    int64_t revision, PureSpan body) {
    char tag[80] = "";
    if (revision > 0) snprintf(tag, sizeof(tag), "ETag: \"%" PRId64 "\"\r\n", revision);
    int length = snprintf(c->response, sizeof(c->response),
        "HTTP/1.1 %d %s\r\nContent-Length: %zu\r\nConnection: close\r\n"
        "Content-Type: application/octet-stream\r\nCache-Control: no-store\r\n"
        "X-Pure-Error: %s\r\n%s\r\n", status, reason, body.size, error, tag);
    assert(length > 0 && (size_t)length < sizeof(c->response));
    c->phase = WRITING;
    pure_transfer_init(&c->transfer,
        (PureSpan){(const unsigned char *)c->response, (size_t)length}, body, c, release_payload);
}

static void fail(Connection *c, int status, const char *reason, const char *error) {
    /* No outstanding backend access on the owner thread at this point. */
    release_payload(c);
    respond(c, status, reason, error, 0, (PureSpan){0});
}

static void answer(Connection *c, PureOutcome result) {
    switch (result.status) {
    case PURE_OK: respond(c, 200, "OK", "none", result.revision, result.body); break;
    case PURE_NOT_FOUND: fail(c, 404, "Not Found", "not_found"); break;
    case PURE_CONFLICT:
        respond(c, 412, "Precondition Failed", "conflict", result.revision, (PureSpan){0}); break;
    case PURE_BUSY: fail(c, 503, "Service Unavailable", "store_busy"); break;
    case PURE_BUDGET: fail(c, 503, "Service Unavailable", "payload_budget"); break;
    case PURE_ALLOCATION: fail(c, 503, "Service Unavailable", "allocation"); break;
    case PURE_CLOSED: fail(c, 503, "Service Unavailable", "stopped"); break;
    case PURE_DENIED: fail(c, 403, "Forbidden", "denied"); break;
    case PURE_LIMIT: fail(c, 413, "Content Too Large", "limit"); break;
    case PURE_UNCERTAIN: fail(c, 500, "Internal Server Error", "durability_unknown"); break;
    default: fail(c, 500, "Internal Server Error", "storage"); break;
    }
}

#ifdef PURE_HTTP_ASYNC
/* Immutable reads may run beside one writer; only the write worker touches
 * the store's temporary-name sequence and publication lock. Runtime and
 * connections remain on poll's owner thread. Borrowed spans outlive work. */
static void *effect_worker(void *context) {
    EffectWorker *worker = context;
    Server *server = worker->server;
    PureInput input = worker->input;
    for (;;) {
        pthread_mutex_lock(&server->job_mutex);
        size_t slot = SLOTS;
        while (slot == SLOTS) {
            for (size_t offset = 0; offset < SLOTS; ++offset) {
                size_t candidate = (server->next_job[input] + offset) % SLOTS;
                if (server->jobs[candidate].state == JOB_QUEUED &&
                    server->jobs[candidate].effect.input == input) {
                    slot = candidate;
                    break;
                }
            }
            if (slot < SLOTS || server->worker_stop) break;
            pthread_cond_wait(&server->job_ready[input], &server->job_mutex);
        }
        if (slot == SLOTS) { pthread_mutex_unlock(&server->job_mutex); return NULL; }
        EffectJob *job = &server->jobs[slot];
        server->next_job[input] = (slot + 1) % SLOTS;
        job->state = JOB_RUNNING;
        PureEffect effect = job->effect;
        pthread_mutex_unlock(&server->job_mutex);

        PureResource resource = {0};
        PureSpan reply = {0};
        int64_t revision = 0;
        PureHost host = server->effect_host;
        PureResult status = effect.input == PURE_INPUT_READ ?
            host.read(host.context, server->runtime.definition, effect.key,
                      effect.available, &resource, &revision) :
            host.write(host.context, server->runtime.definition, effect.key,
                       effect.expected, effect.value, &reply, &revision);

        pthread_mutex_lock(&server->job_mutex);
        job->resource = resource;
        job->reply = reply;
        job->revision = revision;
        job->status = status;
        job->state = JOB_DONE;
        pthread_mutex_unlock(&server->job_mutex);
        const char byte = 1;
        if (write(server->effect_wake, &byte, 1) < 0) { /* full: its reader is woken already */ }
    }
}

static void finish_effects(Server *server) {
    for (size_t i = 0; i < SLOTS; ++i) {
        pthread_mutex_lock(&server->job_mutex);
        EffectJob *job = &server->jobs[i];
        if (job->state != JOB_DONE) { pthread_mutex_unlock(&server->job_mutex); continue; }
        PureTicket ticket = job->effect.ticket;
        PureInput input = job->effect.input;
        PureResource resource = job->resource;
        PureSpan reply = job->reply;
        PureResult status = job->status;
        int64_t revision = job->revision;
        *job = (EffectJob){0};
        pthread_mutex_unlock(&server->job_mutex);
        if (!pure_runtime_complete(ticket, status,
                                   input == PURE_INPUT_READ ? &resource : NULL,
                                   reply, revision)) abort();
        for (size_t j = 0; j < SLOTS; ++j) {
            Connection *c = &server->connections[j];
            if (c->fd < 0 || c->phase != WAITING_EFFECT ||
                c->ticket.slot != ticket.slot || c->ticket.generation != ticket.generation)
                continue;
            PureOutcome result;
            if (!pure_runtime_result(ticket, &result)) abort();
            c->body = NULL;
            answer(c, result);
            break;
        }
    }
}
#endif

static void run_request(Connection *c) {
    if (c->put && !pure_runtime_ready(c->ticket)) abort();
#ifdef PURE_HTTP_ASYNC
    Server *server = c->server;
    PureEffect effect;
    PureTake taken = pure_runtime_take(&server->runtime, &effect);
    if (taken == PURE_TAKE_EFFECT) {
        pthread_mutex_lock(&server->job_mutex);
        EffectJob *job = &server->jobs[effect.ticket.slot];
        if (job->state != JOB_FREE) abort();
        *job = (EffectJob){.effect = effect, .state = JOB_QUEUED};
        pthread_cond_signal(&server->job_ready[effect.input]);
        pthread_mutex_unlock(&server->job_mutex);
        c->phase = WAITING_EFFECT;
        return;
    }
    abort();
#else
    PureOutcome result;
    while (!pure_runtime_result(c->ticket, &result))
        if (!pure_runtime_step(&c->server->runtime)) abort();
#endif
    c->body = NULL; /* Core now owns input until all consumers release it. */
#ifdef PURE_HTTP_ASYNC
    PureOutcome result;
    if (!pure_runtime_result(c->ticket, &result)) abort();
#endif
    answer(c, result);
}

/* Deliberately bounded HTTP/1.1 subset: one GET/PUT, Content-Length, close.
 * No transfer coding, pipelining, trailers, Expect, URL decoding, or proxying. */
static bool parse_header(Connection *c, size_t end) {
    for (size_t i = 0; i < end; ++i) {
        unsigned char b = (unsigned char)c->request[i];
        if ((b < 32 && b != '\r' && b != '\n' && b != '\t') || b == 127) return false;
    }
    char *line = c->request, *next = strstr(line, "\r\n");
    if (!next) return false;
    *next = 0;
    char *path = strchr(line, ' ');
    if (!path) return false;
    *path++ = 0;
    if (strcmp(line, "PUT") == 0) c->put = true;
    else if (strcmp(line, "GET") != 0) return false;
    char *version = strchr(path, ' ');
    if (!version) return false;
    *version++ = 0;
    if (strcmp(version, "HTTP/1.1") || strncmp(path, "/objects/", 9)) return false;
    const char *key = path + 9;
    size_t key_size = strlen(key);
    if (!key_size || key_size > 64) return false;
    for (size_t i = 0; i < key_size; ++i) {
        char b = key[i];
        if (!((b >= 'a' && b <= 'z') || (b >= 'A' && b <= 'Z') ||
              (b >= '0' && b <= '9') || b == '-' || b == '_')) return false;
    }
    memcpy(c->key, key, key_size + 1);
    bool length_seen = false, condition_seen = false, host_seen = false;
    line = next + 2;
    while (*line != '\r') {
        next = strstr(line, "\r\n");
        if (!next) return false;
        *next = 0;
        char *value = strchr(line, ':');
        if (!value || value == line) return false;
        *value++ = 0;
        for (char *p = line; *p; ++p)
            if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                  (*p >= '0' && *p <= '9') || *p == '-')) return false;
        while (*value == ' ' || *value == '\t') ++value;
        char *tail = value + strlen(value);
        while (tail > value && (tail[-1] == ' ' || tail[-1] == '\t')) *--tail = 0;
        if (strchr(value, '\r') || strchr(value, '\n')) return false;
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
        } else if (!strcasecmp(line, "Transfer-Encoding") || !strcasecmp(line, "Expect")) return false;
        line = next + 2;
    }
    return host_seen && (c->put ? length_seen && condition_seen : !condition_seen && c->body_size == 0);
}

static void read_connection(Connection *c) {
    if (c->phase == READING_HEADER) {
        ssize_t n = recv(c->fd, c->request + c->header_size, HEADER_LIMIT - c->header_size, 0);
        if (n < 0) { if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) close_connection(c); return; }
        if (!n) { close_connection(c); return; }
        c->header_size += (size_t)n;
        c->request[c->header_size] = 0;
        /* Scan by byte count, so embedded NUL cannot hide a terminator. */
        size_t end = 0;
        for (size_t i = 3; i < c->header_size; ++i)
            if (!memcmp(c->request + i - 3, "\r\n\r\n", 4)) { end = i + 1; break; }
        if (!end) {
            if (c->header_size == HEADER_LIMIT) fail(c, 431, "Request Header Fields Too Large", "headers");
            return;
        }
        size_t prefix = c->header_size - end;
        if (!parse_header(c, end)) { fail(c, 400, "Bad Request", "request"); return; }
        if (c->body_size > PURE_OBJECT_LIMIT) { fail(c, 413, "Content Too Large", "limit"); return; }
        if (prefix > c->body_size) { fail(c, 400, "Bad Request", "extra_body"); return; }
        PureResult admitted = pure_runtime_accept(&c->server->runtime,
            c->put ? PURE_INPUT_WRITE : PURE_INPUT_READ, c->key, c->expected, c->body_size, &c->ticket);
        if (admitted != PURE_OK) { answer(c, (PureOutcome){.status = admitted}); return; }
        if (!c->put) { run_request(c); return; }
        c->body = pure_runtime_ingress(c->ticket, NULL);
        if (prefix) memcpy(c->body, c->request + end, prefix);
        c->server->prefix_copy_bytes += prefix;
        c->received = prefix;
        c->phase = READING_BODY;
        if (c->received == c->body_size) run_request(c);
    } else {
        size_t remaining = c->body_size - c->received;
        ssize_t n = recv(c->fd, c->body + c->received, remaining < CHUNK ? remaining : CHUNK, 0);
        if (n < 0) { if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) close_connection(c); return; }
        if (!n) { close_connection(c); return; }
        c->received += (size_t)n;
        if (c->received == c->body_size) run_request(c);
    }
}

static void write_connection(Connection *c) {
    PureSpan span = pure_transfer_next(&c->transfer, CHUNK);
    ssize_t n = send(c->fd, span.data, span.size, 0);
    if (n < 0) { if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) close_connection(c); return; }
    if (!n) { close_connection(c); return; }
    if (!pure_transfer_consume(&c->transfer, (size_t)n)) abort();
    if (!c->transfer.active) close_connection(c);
}

static bool nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

static bool close_on_exec(int fd) {
    int flags = fcntl(fd, F_GETFD, 0);
    return flags >= 0 && fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == 0;
}

int main(int argc, char **argv) {
    Server server = {.budget = 4u * 1024u * 1024u, .timeout_ms = 10000};
    unsigned port = 8767;
#ifndef PURE_EMBEDDED_PROGRAM
    const char *program_path = NULL;
    bool explain = argc == 3 && !strcmp(argv[1], "--check-program");
    char program_error[160];
    if (explain) program_path = argv[2];
#else
    bool explain = false;
#endif
    for (int i = 1; !explain && i < argc; i += 2) {
        if (i + 1 == argc) goto usage;
        uint64_t value;
        if (!strcmp(argv[i], "--store")) server.path = argv[i + 1];
#ifndef PURE_EMBEDDED_PROGRAM
        else if (!strcmp(argv[i], "--program")) program_path = argv[i + 1];
#endif
        else if (!strcmp(argv[i], "--port") && number(argv[i + 1], 65535, &value)) port = (unsigned)value;
        else if (!strcmp(argv[i], "--budget") && number(argv[i + 1], SIZE_MAX, &value) && value) server.budget = (size_t)value;
        else if (!strcmp(argv[i], "--timeout-ms") && number(argv[i + 1], 3600000, &value) && value) server.timeout_ms = value;
        else if (!strcmp(argv[i], "--send-buffer") && number(argv[i + 1], INT32_MAX, &value)) server.send_buffer = (int)value;
        else goto usage;
    }
    if (!server.path && !explain) goto usage;
#ifndef PURE_EMBEDDED_PROGRAM
    if (!program_path) goto usage;
    if (!pure_program_load(program_path, &server.program, program_error, sizeof(program_error))) {
        fprintf(stderr, "Program: %s\n", program_error); return 2;
    }
    if (explain) {
        PureProgram *p = &server.program;
        printf("{\"read\":{\"to_end\":%s,\"offset\":%zu,\"length\":%zu,\"minimum_source_bytes\":%zu},"
            "\"write\":{\"to_end\":%s,\"offset\":%zu,\"length\":%zu,\"minimum_source_bytes\":%zu},"
            "\"reply_input\":%s,\"view_definitions\":%zu,\"range_definitions\":%zu,"
            "\"runtime_view_steps\":%zu,\"write_step\":%zu,\"reply_step\":%zu,\"prepared_program_bytes\":%zu,\"steps\":[",
            p->read.to_end ? "true" : "false", p->read.offset, p->read.length, p->read_required,
            p->write.to_end ? "true" : "false", p->write.offset, p->write.length, p->input_required,
            p->reply_input ? "true" : "false", p->view_definitions, p->range_definitions,
            p->input_steps, p->write_step, p->reply_step, sizeof(*p));
        for (size_t i = 0; i < p->input_steps; ++i) {
            const PureRangeStep *step = &p->steps[i];
            printf("%s{\"source_step\":%zu,\"source\":{\"offset\":%zu,\"length\":%zu,\"to_end\":%s},"
                "\"offset\":{\"value\":%zu,\"from_u32le\":%s},\"length\":{\"value\":%zu,\"from_u32le\":%s}}",
                i ? "," : "", step->source_step, step->source.offset, step->source.length,
                step->source.to_end ? "true" : "false", step->offset.value, step->offset.from_u32le ? "true" : "false",
                step->length.value, step->length.from_u32le ? "true" : "false");
        }
        printf("],\"write_effect\":\"%s\",\"splice_offset\":{\"value\":%zu,\"from_u32le\":%s},"
               "\"splice_remove\":{\"value\":%zu,\"from_u32le\":%s}}\n",
               p->splice ? "splice" : "write", p->splice_offset.value,
               p->splice_offset.from_u32le ? "true" : "false", p->splice_remove.value,
               p->splice_remove.from_u32le ? "true" : "false");
        return 0;
    }
#endif
    const PureProgram *program =
#ifdef PURE_EMBEDDED_PROGRAM
        &pure_embedded_program;
#else
        &server.program;
#endif
    if (pure_store_init(server.path) != PURE_OK) { fputs("Cannot initialize byte store directory\n", stderr); return 1; }
    PureHost host = pure_store_host(server.path);
#ifdef PURE_HTTP_ASYNC
    server.effect_host = host;
    host.read = NULL;
    host.write = NULL;
#endif
    if (!pure_runtime_init(&server.runtime, server.work, SLOTS, program,
                           host, server.budget)) return 1;
#ifdef PURE_HTTP_ASYNC
    if (!pure_runtime_enable_async(&server.runtime)) abort();
#endif
    signal(SIGPIPE, SIG_IGN);
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) { perror("socket"); return 1; }
    int one = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port),
                                 .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    if (bind(listener, (struct sockaddr *)&address, sizeof(address)) || listen(listener, SLOTS) || !nonblocking(listener)) {
        perror("listen"); close(listener); return 1;
    }
    socklen_t address_size = sizeof(address);
    if (getsockname(listener, (struct sockaddr *)&address, &address_size)) { close(listener); return 1; }
    int wake[2];
    if (pipe(wake)) { perror("pipe"); close(listener); return 1; }
    if (!nonblocking(wake[1]) || !close_on_exec(wake[0]) || !close_on_exec(wake[1])
#ifdef PURE_HTTP_ASYNC
        || !nonblocking(wake[0])
#endif
       ) {
        perror("wake pipe"); close(wake[0]); close(wake[1]); close(listener); return 1;
    }
    wake_write = wake[1];
    struct sigaction stop_action = {.sa_handler = stop_server};
    sigemptyset(&stop_action.sa_mask);
    if (sigaction(SIGINT, &stop_action, NULL) || sigaction(SIGTERM, &stop_action, NULL)) {
        perror("sigaction"); wake_write = -1;
        close(wake[0]); close(wake[1]); close(listener); return 1;
    }
#ifdef PURE_HTTP_ASYNC
    server.effect_wake = wake[1];
    pthread_attr_t worker_attr;
    if (pthread_mutex_init(&server.job_mutex, NULL) ||
        pthread_cond_init(&server.job_ready[PURE_INPUT_READ], NULL) ||
        pthread_cond_init(&server.job_ready[PURE_INPUT_WRITE], NULL) ||
        pthread_attr_init(&worker_attr) ||
        pthread_attr_setstacksize(&worker_attr, 128u * 1024u)) {
        fputs("Cannot initialize bounded storage workers\n", stderr);
        wake_write = -1;
        close(wake[0]); close(wake[1]); close(listener); return 1;
    }
    size_t workers_started = 0;
    for (size_t i = 0; i < 2; ++i) {
        server.worker_context[i] = (EffectWorker){.server = &server, .input = (PureInput)i};
        if (pthread_create(&server.worker[i], &worker_attr,
                           effect_worker, &server.worker_context[i])) break;
        ++workers_started;
    }
    pthread_attr_destroy(&worker_attr);
    if (workers_started != 2) {
        pthread_mutex_lock(&server.job_mutex);
        server.worker_stop = true;
        pthread_cond_signal(&server.job_ready[PURE_INPUT_READ]);
        pthread_cond_signal(&server.job_ready[PURE_INPUT_WRITE]);
        pthread_mutex_unlock(&server.job_mutex);
        for (size_t i = 0; i < workers_started; ++i) pthread_join(server.worker[i], NULL);
        pthread_cond_destroy(&server.job_ready[PURE_INPUT_READ]);
        pthread_cond_destroy(&server.job_ready[PURE_INPUT_WRITE]);
        pthread_mutex_destroy(&server.job_mutex);
        fputs("Cannot start bounded storage workers\n", stderr);
        wake_write = -1;
        close(wake[0]); close(wake[1]); close(listener); return 1;
    }
#endif
    for (size_t i = 0; i < SLOTS; ++i) server.connections[i].fd = -1;
    printf("{\"port\":%u,\"storage\":\"immutable-byte-extents-v1\"}\n", ntohs(address.sin_port));
    fflush(stdout);
    while (!stopping) {
        struct pollfd polls[SLOTS + 2] = {
            {.fd = server.active < SLOTS ? listener : -1, .events = POLLIN},
            {.fd = wake[0], .events = POLLIN}
        };
        uint64_t nearest_deadline = UINT64_MAX;
        for (size_t i = 0; i < SLOTS; ++i) {
            Connection *c = &server.connections[i];
            polls[i + 2] = (struct pollfd){.fd = c->phase == WAITING_EFFECT ? -1 : c->fd,
                .events = c->phase == WRITING ? POLLOUT : POLLIN};
            if (c->fd >= 0 && c->deadline < nearest_deadline) nearest_deadline = c->deadline;
        }
        int wait_ms = -1;
        if (nearest_deadline != UINT64_MAX) {
            uint64_t now = now_ms();
            uint64_t remaining = nearest_deadline > now ? nearest_deadline - now : 0;
            wait_ms = remaining > INT_MAX ? INT_MAX : (int)remaining;
        }
        int ready = poll(polls, SLOTS + 2, wait_ms);
        if (ready < 0 && errno != EINTR) { perror("poll"); stopping = 1; }
#ifdef PURE_HTTP_ASYNC
        if (polls[1].revents & POLLIN) {
            char bytes[64];
            while (read(wake[0], bytes, sizeof(bytes)) > 0) {}
        }
        finish_effects(&server);
#endif
        uint64_t now = now_ms();
        for (size_t i = 0; i < SLOTS; ++i) {
            Connection *c = &server.connections[i];
            if (c->fd < 0) continue;
            if (now >= c->deadline) { ++server.expired; close_connection(c); continue; }
            short events = polls[i + 2].revents;
            if (events & (POLLERR | POLLNVAL)) { close_connection(c); continue; }
#ifdef PURE_HTTP_ASYNC
            if (c->phase == WAITING_EFFECT) continue;
#endif
            if (events & (POLLIN | POLLOUT | POLLHUP)) {
                if (c->phase == WRITING) write_connection(c); else read_connection(c);
            }
        }
        if (!stopping && (polls[0].revents & POLLIN)) {
            int fd = accept(listener, NULL, NULL);
            if (fd < 0) continue;
            if (!nonblocking(fd)) { close(fd); continue; }
            if (server.send_buffer) setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &server.send_buffer, sizeof(server.send_buffer));
            for (size_t i = 0; i < SLOTS; ++i) if (server.connections[i].fd < 0) {
                server.connections[i] = (Connection){.fd = fd, .server = &server, .deadline = now_ms() + server.timeout_ms};
                ++server.active;
                ++server.accepted;
                if (server.active > server.peak_active) server.peak_active = server.active;
                break;
            }
        }
    }
    close(listener);
    wake_write = -1;
#ifndef PURE_HTTP_ASYNC
    close(wake[0]);
    close(wake[1]);
#endif
    pure_runtime_stop(&server.runtime);
    for (size_t i = 0; i < SLOTS; ++i) close_connection(&server.connections[i]);
#ifdef PURE_HTTP_ASYNC
    pthread_mutex_lock(&server.job_mutex);
    server.worker_stop = true;
    pthread_cond_signal(&server.job_ready[PURE_INPUT_READ]);
    pthread_cond_signal(&server.job_ready[PURE_INPUT_WRITE]);
    pthread_mutex_unlock(&server.job_mutex);
    for (size_t i = 0; i < 2; ++i) if (pthread_join(server.worker[i], NULL)) abort();
    finish_effects(&server);
    close(wake[0]);
    close(wake[1]);
    pthread_cond_destroy(&server.job_ready[PURE_INPUT_READ]);
    pthread_cond_destroy(&server.job_ready[PURE_INPUT_WRITE]);
    pthread_mutex_destroy(&server.job_mutex);
#endif
    fprintf(stderr, "{\"active\":%zu,\"retained_payload_bytes\":%zu,\"peak_retained_payload_bytes\":%zu,"
        "\"payload_budget\":%zu,\"peak_connections\":%zu,\"accepted\":%" PRIu64 ",\"commits\":%" PRIu64 ","
        "\"gets\":%" PRIu64 ",\"conflicts\":%" PRIu64 ",\"budget_rejections\":%" PRIu64 ","
        "\"expired\":%" PRIu64 ",\"releases\":%" PRIu64 ",\"ingress_prefix_copy_bytes\":%" PRIu64 ","
        "\"borrowed_output_bytes\":%" PRIu64 ",\"server_structure_bytes\":%zu}\n",
        server.active, server.runtime.retained, server.runtime.peak_retained, server.budget, server.peak_active,
        server.accepted, server.runtime.commits, server.runtime.gets, server.runtime.conflicts, server.runtime.budget_rejections,
        server.expired, server.runtime.releases, server.prefix_copy_bytes, server.runtime.borrowed_output_bytes, sizeof(server));
    return 0;
usage:
#ifdef PURE_EMBEDDED_PROGRAM
    fputs("pure-connections-fixed --store DIRECTORY [--port 8767] [--budget BYTES] [--timeout-ms 10000] [--send-buffer BYTES]\n", stderr);
#else
    fputs("pure-connections --program FILE --store DIRECTORY [--port 8767] [--budget BYTES] [--timeout-ms 10000] [--send-buffer BYTES]\n"
          "pure-connections --check-program FILE\n", stderr);
#endif
    return 2;
}
