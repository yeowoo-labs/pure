#define _POSIX_C_SOURCE 200809L
#include "store.h"
#include "program.h"
#include "store-port.h"
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

enum { SLOTS = 8, HEADER_LIMIT = 4096, CHUNK = 65536 };
typedef enum { READING_HEADER, READING_BODY, WRITING } Phase;
typedef struct Server Server;
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
    PureProgram program;
    PureRuntime runtime;
    PureWork work[SLOTS];
    size_t budget, active, peak_active;
    uint64_t accepted, expired, prefix_copy_bytes;
    uint64_t timeout_ms;
    int send_buffer;
    Connection connections[SLOTS];
};
static volatile sig_atomic_t stopping;
static void stop_server(int sig) { (void)sig; stopping = 1; }

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

static void run_request(Connection *c) {
    if (c->put && !pure_runtime_ready(c->ticket)) abort();
    PureOutcome result;
    while (!pure_runtime_result(c->ticket, &result))
        if (!pure_runtime_step(&c->server->runtime)) abort();
    c->body = NULL; /* Core now owns input until all consumers release it. */
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

int main(int argc, char **argv) {
    Server server = {.budget = 4u * 1024u * 1024u, .timeout_ms = 10000};
    unsigned port = 8767;
    const char *program_path = NULL;
    bool explain = argc == 3 && !strcmp(argv[1], "--check-program");
    char program_error[160];
    if (explain) program_path = argv[2];
    for (int i = 1; !explain && i < argc; i += 2) {
        if (i + 1 == argc) goto usage;
        uint64_t value;
        if (!strcmp(argv[i], "--store")) server.path = argv[i + 1];
        else if (!strcmp(argv[i], "--program")) program_path = argv[i + 1];
        else if (!strcmp(argv[i], "--port") && number(argv[i + 1], 65535, &value)) port = (unsigned)value;
        else if (!strcmp(argv[i], "--budget") && number(argv[i + 1], SIZE_MAX, &value) && value) server.budget = (size_t)value;
        else if (!strcmp(argv[i], "--timeout-ms") && number(argv[i + 1], 3600000, &value) && value) server.timeout_ms = value;
        else if (!strcmp(argv[i], "--send-buffer") && number(argv[i + 1], INT32_MAX, &value)) server.send_buffer = (int)value;
        else goto usage;
    }
    if (!program_path || (!explain && !server.path)) goto usage;
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
        puts("]}");
        return 0;
    }
    if (pure_store_init(server.path) != PURE_OK) { fputs("Cannot initialize byte store directory\n", stderr); return 1; }
    if (!pure_runtime_init(&server.runtime, server.work, SLOTS, &server.program,
                           pure_store_host(server.path), server.budget)) return 1;
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, stop_server);
    signal(SIGTERM, stop_server);
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
    for (size_t i = 0; i < SLOTS; ++i) server.connections[i].fd = -1;
    printf("{\"port\":%u,\"storage\":\"immutable-byte-extents-v1\"}\n", ntohs(address.sin_port));
    fflush(stdout);
    while (!stopping) {
        struct pollfd polls[SLOTS + 1] = {{.fd = server.active < SLOTS ? listener : -1, .events = POLLIN}};
        for (size_t i = 0; i < SLOTS; ++i) {
            Connection *c = &server.connections[i];
            polls[i + 1] = (struct pollfd){.fd = c->fd, .events = c->phase == WRITING ? POLLOUT : POLLIN};
        }
        int ready = poll(polls, SLOTS + 1, 50);
        if (ready < 0 && errno != EINTR) { perror("poll"); stopping = 1; }
        uint64_t now = now_ms();
        for (size_t i = 0; i < SLOTS; ++i) {
            Connection *c = &server.connections[i];
            if (c->fd < 0) continue;
            if (now >= c->deadline) { ++server.expired; close_connection(c); continue; }
            short events = polls[i + 1].revents;
            if (events & (POLLERR | POLLNVAL)) { close_connection(c); continue; }
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
    pure_runtime_stop(&server.runtime);
    for (size_t i = 0; i < SLOTS; ++i) close_connection(&server.connections[i]);
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
    fputs("pure-connections --program FILE --store DIRECTORY [--port 8767] [--budget BYTES] [--timeout-ms 10000] [--send-buffer BYTES]\n"
          "pure-connections --check-program FILE\n", stderr);
    return 2;
}
