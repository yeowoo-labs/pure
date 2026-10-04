#define _GNU_SOURCE
/* HTTP/1.1 load for Pure's benchmarks: N connections on one epoll thread,
 * each sending one request at a time and measuring its latency.
 *
 * load PORT CONNECTIONS SECONDS get|put|score|total|page|note|json|transfer KEYS [keep-alive|close] [BODY] [PREFIX]
 * get: GET /objects/kI for I in [0, KEYS).
 * put: connection C edits its own key kC (C mod KEYS), inserting BODY bytes
 *      at 0 with If-Match on the revision it last saw (If-None-Match: * at
 *      first); a 412 takes the revision from the ETag and tries again.
 * The scoreboard app: score posts a point for kC (POST /inputs/score?points=1,
 * the name as the body), total reads GET /views/total and page GET
 * /views/leaders?from=0&count=20. note reads GET /views/page (a text state
 * shown as it is, as in a notes app). json posts {"name":"k","text":"x..."}
 * (BODY bytes of text) to POST /inputs/echo as application/json. transfer is
 * pgbench's TPC-B-like transaction for benchmarks/bank: POST /inputs/transfer
 * {"aid","tid","bid","delta"}, a random account of KEYS, teller and branch of
 * the scale (100,000 accounts, 10 tellers and a branch each) and delta in
 * [-5000, 5000], as pgbench picks them.
 * PREFIX goes before every path: /NAME for an app of a host of apps
 * (pure-serve --apps, docs/design/017-apps-on-one-host.md). With
 * PURE_LOAD_TOKEN in the environment each request bears it
 * (Authorization: Bearer, docs/design/017's 3b).
 * Prints one JSON line: requests/s, statuses, latency percentiles (µs). */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

enum { BUCKETS = 64 * 16, RESPONSE = 1 << 21 };

typedef struct {
    int fd, key;
    bool connected;
    char request[512];
    size_t request_size, sent, body_size, body_sent;
    char *response;
    size_t received;
    uint64_t started, revision, random;
} Connection;

static uint64_t now_us(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u;
}

/* Log-linear histogram: 16 buckets per power of two. */
static uint64_t histogram[BUCKETS];
static unsigned bucket(uint64_t us) {
    if (us < 16) return (unsigned)us;
    unsigned power = 63 - (unsigned)__builtin_clzll(us);
    unsigned index = (power - 3) * 16 + (unsigned)((us >> (power - 4)) & 15);
    return index < BUCKETS ? index : BUCKETS - 1;
}
static uint64_t bucket_value(unsigned index) {
    if (index < 16) return index;
    unsigned power = index / 16 + 3;
    return ((uint64_t)16 + index % 16) << (power - 4);
}
static uint64_t percentile(uint64_t total, double p) {
    uint64_t want = (uint64_t)(total * p), seen = 0;
    for (unsigned i = 0; i < BUCKETS; ++i) {
        seen += histogram[i];
        if (seen > want) return bucket_value(i);
    }
    return 0;
}

static int port, connections_count, keys;
static enum { GET, PUT, SCORE, TOTAL, PAGE, NOTE, JSON, TRANSFER } method;
static bool put, keep_alive;
static char *body;
static size_t body_bytes;
static const char *prefix = ""; /* before every path: /NAME for an app of a host of apps */
static int epoll_fd;

/* xorshift64: each connection its own sequence. */
static uint64_t next_random(uint64_t *state) {
    uint64_t x = *state;
    x ^= x << 13, x ^= x >> 7, x ^= x << 17;
    return *state = x;
}

static char closing[512]; /* the head's last lines: Connection: close, the token's */

static void prepare(Connection *c) {
    if (method == TRANSFER) {
        unsigned scale = keys / 100000 ? (unsigned)(keys / 100000) : 1;
        char json[128];
        int length = snprintf(json, sizeof(json), "{\"aid\":%u,\"tid\":%u,\"bid\":%u,\"delta\":%d}",
                              (unsigned)(next_random(&c->random) % (uint64_t)keys) + 1,
                              (unsigned)(next_random(&c->random) % (10u * scale)) + 1,
                              (unsigned)(next_random(&c->random) % scale) + 1,
                              (int)(next_random(&c->random) % 10001) - 5000);
        c->request_size = (size_t)snprintf(c->request, sizeof(c->request),
            "POST %s/inputs/transfer HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\n"
            "Content-Length: %d\r\n%s\r\n%s", prefix, length, closing, json);
        c->sent = c->received = c->body_sent = c->body_size = 0;
        c->started = now_us();
        return;
    }
    if (method == JSON) {
        c->request_size = (size_t)snprintf(c->request, sizeof(c->request),
            "POST %s/inputs/echo HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\n"
            "Content-Length: %zu\r\n%s\r\n", prefix, body_bytes, closing);
        c->sent = c->received = c->body_sent = 0;
        c->body_size = body_bytes;
        c->started = now_us();
        return;
    }
    if (method >= SCORE) {
        char name[16];
        int length = snprintf(name, sizeof(name), "k%d", c->key);
        c->request_size = (size_t)(method == SCORE ?
            snprintf(c->request, sizeof(c->request), "POST %s/inputs/score?points=1 HTTP/1.1\r\nHost: localhost\r\n"
                     "Content-Length: %d\r\n%s\r\n%s", prefix, length, closing, name) :
            snprintf(c->request, sizeof(c->request), "GET %s/views/%s HTTP/1.1\r\nHost: localhost\r\n%s\r\n", prefix,
                     method == TOTAL ? "total" : method == NOTE ? "page" : "leaders?from=0&count=20", closing));
        c->sent = c->received = c->body_sent = c->body_size = 0;
        c->started = now_us();
        return;
    }
    char condition[64] = "";
    if (put) {
        if (c->revision) snprintf(condition, sizeof(condition), "If-Match: \"%llu\"\r\n", (unsigned long long)c->revision);
        else snprintf(condition, sizeof(condition), "If-None-Match: *\r\n");
    }
    c->request_size = (size_t)snprintf(c->request, sizeof(c->request),
        "%s %s/objects/k%d HTTP/1.1\r\nHost: localhost\r\nContent-Length: %zu\r\n%s%s\r\n",
        put ? "PUT" : "GET", prefix, c->key, put ? body_bytes + 8 : 0, condition, closing);
    c->sent = c->received = c->body_sent = 0;
    c->body_size = put ? body_bytes + 8 : 0;
    c->started = now_us();
}

static void open_connection(Connection *c) {
    c->fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    int one = 1;
    setsockopt(c->fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port),
                                  .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    connect(c->fd, (struct sockaddr *)&address, sizeof(address));
    c->connected = false;
    struct epoll_event event = {.events = EPOLLOUT | EPOLLIN, .data.ptr = c};
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, c->fd, &event);
}

static void reopen(Connection *c) {
    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, c->fd, NULL);
    close(c->fd);
    open_connection(c);
}

static uint64_t completed, statuses[6], errors;

/* A complete response in c->response, or false. */
static bool complete(Connection *c, int *status, bool *closing, size_t *length) {
    char *end = memmem(c->response, c->received, "\r\n\r\n", 4);
    if (!end) return false;
    size_t header = (size_t)(end - c->response) + 4, content = 0;
    char *field = memmem(c->response, header, "Content-Length:", 15);
    if (!field) field = memmem(c->response, header, "content-length:", 15);
    if (field) content = strtoul(field + 15, NULL, 10);
    if (c->received < header + content) return false;
    *status = atoi(c->response + 9);
    *closing = memmem(c->response, header, "onnection: close", 16) != NULL;
    char *tag = memmem(c->response, header, "ETag: \"", 7);
    if (!tag) tag = memmem(c->response, header, "etag: \"", 7);
    if (tag) c->revision = strtoull(tag + 7, NULL, 10);
    *length = header + content;
    return true;
}

int main(int argc, char **argv) {
    if (argc < 6) {
        fputs("load PORT CONNECTIONS SECONDS get|put|score|total|page|note|json|transfer KEYS [keep-alive|close] [BODY] "
              "[PREFIX]\n", stderr);
        return 2;
    }
    port = atoi(argv[1]);
    connections_count = atoi(argv[2]);
    double seconds = atof(argv[3]);
    static const char *const methods[] = {"get", "put", "score", "total", "page", "note", "json", "transfer"};
    for (method = GET; method <= TRANSFER && strcmp(argv[4], methods[method]); ++method) {}
    if (method > TRANSFER) { fprintf(stderr, "load: unknown method %s\n", argv[4]); return 2; }
    put = method == PUT;
    keys = atoi(argv[5]);
    keep_alive = argc > 6 && !strcmp(argv[6], "keep-alive");
    body_bytes = argc > 7 ? strtoul(argv[7], NULL, 10) : 16;
    prefix = argc > 8 ? argv[8] : "";
    const char *token = getenv("PURE_LOAD_TOKEN");
    if (token && strlen(token) > 256) return 2;
    snprintf(closing, sizeof(closing), "%s%s%s%s", keep_alive ? "" : "Connection: close\r\n", token ? "Authorization: Bearer " : "",
             token ? token : "", token ? "\r\n" : "");
    body = calloc(body_bytes + 32, 1);
    memset(body + 8, 'x', body_bytes);
    if (method == JSON) {
        static const char open[] = "{\"name\":\"k\",\"text\":\"";
        memcpy(body, open, sizeof(open) - 1);
        memset(body + sizeof(open) - 1, 'x', body_bytes);
        memcpy(body + sizeof(open) - 1 + body_bytes, "\"}", 2);
        body_bytes += sizeof(open) - 1 + 2;
    }
    epoll_fd = epoll_create1(0);
    Connection *all = calloc((size_t)connections_count, sizeof(*all));
    for (int i = 0; i < connections_count; ++i) {
        all[i].key = i % keys;
        all[i].random = 0x9e3779b97f4a7c15u * (uint64_t)(i + 1);
        all[i].response = malloc(RESPONSE);
        open_connection(&all[i]);
        prepare(&all[i]);
    }
    uint64_t start = now_us(), stop = start + (uint64_t)(seconds * 1e6);
    struct epoll_event events[256];
    while (now_us() < stop) {
        int ready = epoll_wait(epoll_fd, events, 256, 100);
        for (int e = 0; e < ready; ++e) {
            Connection *c = events[e].data.ptr;
            if (events[e].events & (EPOLLERR | EPOLLHUP) && !(events[e].events & EPOLLIN)) {
                ++errors;
                reopen(c);
                prepare(c);
                continue;
            }
            if (events[e].events & EPOLLOUT) {
                c->connected = true;
                while (c->sent < c->request_size) {
                    ssize_t n = send(c->fd, c->request + c->sent, c->request_size - c->sent, MSG_NOSIGNAL);
                    if (n <= 0) break;
                    c->sent += (size_t)n;
                }
                while (c->sent == c->request_size && c->body_sent < c->body_size) {
                    ssize_t n = send(c->fd, body + c->body_sent, c->body_size - c->body_sent, MSG_NOSIGNAL);
                    if (n <= 0) break;
                    c->body_sent += (size_t)n;
                }
                if (c->sent == c->request_size && c->body_sent == c->body_size) {
                    struct epoll_event event = {.events = EPOLLIN, .data.ptr = c};
                    epoll_ctl(epoll_fd, EPOLL_CTL_MOD, c->fd, &event);
                }
            }
            if (events[e].events & EPOLLIN) {
                ssize_t n = recv(c->fd, c->response + c->received, RESPONSE - c->received, 0);
                if (n <= 0) {
                    if (n < 0 && errno == EAGAIN) continue;
                    ++errors;
                    reopen(c);
                    prepare(c);
                    continue;
                }
                c->received += (size_t)n;
                int status;
                bool closing;
                size_t length;
                if (!complete(c, &status, &closing, &length)) continue;
                ++completed;
                ++histogram[bucket(now_us() - c->started)];
                ++statuses[status / 100 < 6 ? status / 100 : 0];
                if (!keep_alive || closing) reopen(c);
                else {
                    struct epoll_event event = {.events = EPOLLOUT | EPOLLIN, .data.ptr = c};
                    epoll_ctl(epoll_fd, EPOLL_CTL_MOD, c->fd, &event);
                }
                prepare(c);
            }
        }
    }
    double elapsed = (double)(now_us() - start) / 1e6;
    printf("{\"requests_per_second\":%.0f,\"completed\":%llu,\"2xx\":%llu,\"4xx\":%llu,\"5xx\":%llu,\"errors\":%llu,"
           "\"p50_us\":%llu,\"p99_us\":%llu,\"max_us\":%llu}\n",
           (double)completed / elapsed, (unsigned long long)completed, (unsigned long long)statuses[2],
           (unsigned long long)statuses[4], (unsigned long long)statuses[5], (unsigned long long)errors,
           (unsigned long long)percentile(completed, 0.5), (unsigned long long)percentile(completed, 0.99),
           (unsigned long long)percentile(completed, 0.99999));
    return 0;
}
