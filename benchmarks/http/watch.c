#define _GNU_SOURCE
/* How fresh a view stays for many clients (docs/design/019): N clients
 * follow a counter view while one writer moves it RATE times a second.
 *
 * watch PORT CLIENTS SECONDS RATE push|poll[:MS] VIEW WRITE
 *   push    each client subscribes to GET VIEW (Accept: text/event-stream)
 *   poll:MS each client GETs VIEW every MS milliseconds (kept alive, its
 *           first at a random point of the first MS)
 * The writer POSTs WRITE (a text input that adds one to the counter) on a
 * connection of its own every 1/RATE seconds: its k-th answer means the
 * counter is k past where it began. A client that sees the value v has
 * every write up to v: each such write's freshness is when the client saw
 * it less when the writer had its answer (0 when the client saw it
 * first). Prints one JSON line: writes, clients that followed to the end,
 * events or answers taken, the freshness percentiles (µs) and how many
 * writes it was measured for. */
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

enum { BUCKETS = 64 * 16, IN = 4096 };

static uint64_t now_us(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u;
}

/* Log-linear histogram: 16 buckets per power of two. */
static uint64_t histogram[BUCKETS], samples;
static unsigned bucket(uint64_t us) {
    if (us < 16) return (unsigned)us;
    unsigned power = 63u - (unsigned)__builtin_clzll(us);
    unsigned b = (power - 3) * 16 + (unsigned)((us >> (power - 4)) & 15);
    return b < BUCKETS ? b : BUCKETS - 1;
}
static uint64_t bucket_value(unsigned b) {
    if (b < 16) return b;
    unsigned power = b / 16 + 3;
    return ((uint64_t)16 + b % 16) << (power - 4);
}
static uint64_t percentile(double p) {
    uint64_t want = (uint64_t)(p * (double)samples), seen = 0;
    for (unsigned b = 0; b < BUCKETS; ++b)
        if ((seen += histogram[b]) > want) return bucket_value(b);
    return 0;
}

typedef struct {
    int fd;
    bool writer, streaming;
    char in[IN];
    size_t used;
    int64_t seen;     /* the newest value it has seen (-1: none) */
    uint64_t next_at; /* a poller's next request, or the writer's */
    bool asking;      /* a request is out */
} Client;

static int64_t base = -1;       /* the counter when the writer began */
static uint64_t *acked;         /* by write: when its answer came (0: not yet) */
static size_t writes, acked_count;
static uint64_t taken;

/* Connected, then non-blocking: a request goes at once. */
static int dial(int port) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port),
                                  .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    if (connect(fd, (struct sockaddr *)&address, sizeof(address))) { perror("connect"); exit(1); }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    return fd;
}

static void send_all(int fd, const char *text) {
    size_t size = strlen(text), at = 0;
    while (at < size) {
        ssize_t n = send(fd, text + at, size - at, MSG_NOSIGNAL);
        if (n > 0) at += (size_t)n;
        else if (n < 0 && errno != EAGAIN && errno != EINTR) { perror("send"); exit(1); }
    }
}

/* A client saw the counter at v: the writes it had not seen up to v are fresh as of now. */
static void saw(Client *c, int64_t v, uint64_t now) {
    if (base < 0 || v <= c->seen) { if (v > c->seen) c->seen = v; return; }
    for (int64_t k = (c->seen < base ? base : c->seen) + 1; k <= v; ++k) {
        size_t write = (size_t)(k - base - 1);
        if (write >= writes) break;
        uint64_t at = acked[write];
        ++histogram[bucket(at && now > at ? now - at : 0)];
        ++samples;
    }
    c->seen = v;
}

int main(int argc, char **argv) {
    if (argc != 8) {
        fputs("watch PORT CLIENTS SECONDS RATE push|poll[:MS] VIEW WRITE\n", stderr);
        return 2;
    }
    int port = atoi(argv[1]);
    size_t clients = (size_t)atol(argv[2]);
    double seconds = atof(argv[3]), rate = atof(argv[4]);
    bool push = !strcmp(argv[5], "push");
    uint64_t every = push ? 0 : (uint64_t)atol(argv[5] + 5) * 1000;
    const char *view = argv[6], *write_path = argv[7];
    writes = (size_t)(seconds * rate) + 1;
    acked = calloc(writes, sizeof(*acked));
    Client *all = calloc(clients + 1, sizeof(*all));
    int epoll = epoll_create1(EPOLL_CLOEXEC);
    char request[512];
    srand(1);
    uint64_t start = now_us();
    /* The followers first, opened a few hundred at a time (a subscriber asks at once); then the writer. */
    for (size_t i = 0; i < clients; ++i) {
        Client *c = &all[i];
        c->fd = dial(port);
        if (push) {
            snprintf(request, sizeof(request), "GET %s HTTP/1.1\r\nHost: x\r\nAccept: text/event-stream\r\n\r\n", view);
            send_all(c->fd, request);
        }
        c->seen = -1;
        c->next_at = push ? 0 : start + (uint64_t)rand() % (every ? every : 1);
        struct epoll_event event = {.events = EPOLLIN | EPOLLOUT | EPOLLET, .data.ptr = c};
        epoll_ctl(epoll, EPOLL_CTL_ADD, c->fd, &event);
        if (i % 200 == 199) usleep(20000);
    }
    Client *writer = &all[clients];
    writer->writer = true;
    writer->fd = dial(port);
    struct epoll_event writer_event = {.events = EPOLLIN | EPOLLOUT | EPOLLET, .data.ptr = writer};
    epoll_ctl(epoll, EPOLL_CTL_ADD, writer->fd, &writer_event);
    usleep(200000); /* every follower connected */
    /* The counter where it begins: one GET on the writer's connection. */
    snprintf(request, sizeof(request), "GET %s HTTP/1.1\r\nHost: x\r\n\r\n", view);
    send_all(writer->fd, request);
    writer->asking = true;
    struct epoll_event events[256];
    uint64_t began = 0, end = UINT64_MAX;
    size_t sent = 0;
    while (now_us() < end) {
        uint64_t now = now_us();
        /* The writer's next write, and the pollers' next requests. */
        if (base >= 0 && !writer->asking && sent < writes && now >= began + (uint64_t)((double)sent * 1e6 / rate)) {
            snprintf(request, sizeof(request), "POST %s HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\n\r\nx", write_path);
            send_all(writer->fd, request);
            writer->asking = true;
            ++sent;
        }
        for (size_t i = 0; !push && i < clients; ++i) {
            Client *c = &all[i];
            if (c->asking || now < c->next_at) continue;
            snprintf(request, sizeof(request), "GET %s HTTP/1.1\r\nHost: x\r\n\r\n", view);
            send_all(c->fd, request);
            c->asking = true;
            c->next_at += every;
        }
        int ready = epoll_wait(epoll, events, 256, 1);
        now = now_us();
        for (int e = 0; e < ready; ++e) {
            Client *c = events[e].data.ptr;
            for (;;) {
                ssize_t n = recv(c->fd, c->in + c->used, IN - 1 - c->used, 0);
                if (n == 0) { fprintf(stderr, "a connection closed\n"); exit(1); }
                if (n < 0) break;
                c->used += (size_t)n;
                c->in[c->used] = 0;
                for (;;) { /* whole answers (Content-Length) or whole events */
                    char *at = c->in;
                    if (c->writer || !push) {
                        char *head_end = strstr(at, "\r\n\r\n"), *length = strcasestr(at, "Content-Length: ");
                        if (!head_end || !length || length > head_end) break;
                        size_t body = (size_t)atol(length + 16), whole = (size_t)(head_end + 4 - at) + body;
                        if (c->used < whole) break;
                        int64_t value = body ? atoll(head_end + 4) : -1;
                        if (c->writer) {
                            if (base < 0) { base = value; began = now; end = now + (uint64_t)(seconds * 1e6) + 500000; }
                            else { acked[acked_count++] = now; }
                        } else {
                            ++taken;
                            saw(c, value, now);
                        }
                        c->asking = false;
                        memmove(c->in, c->in + whole, c->used - whole);
                        c->used -= whole;
                        c->in[c->used] = 0;
                        continue;
                    }
                    if (!c->streaming) { /* the stream's head */
                        char *head_end = strstr(at, "\r\n\r\n");
                        if (!head_end) break;
                        if (strncmp(at, "HTTP/1.1 200", 12)) { fprintf(stderr, "refused: %.40s\n", at); exit(1); }
                        c->streaming = true;
                        size_t whole = (size_t)(head_end + 4 - at);
                        memmove(c->in, c->in + whole, c->used - whole);
                        c->used -= whole;
                        c->in[c->used] = 0;
                        continue;
                    }
                    char *event_end = strstr(at, "\n\n");
                    if (!event_end) break;
                    char *data = strstr(at, "data: ");
                    if (data && data < event_end) { ++taken; saw(c, atoll(data + 6), now); }
                    size_t whole = (size_t)(event_end + 2 - at);
                    memmove(c->in, c->in + whole, c->used - whole);
                    c->used -= whole;
                    c->in[c->used] = 0;
                }
            }
        }
    }
    size_t followed = 0;
    for (size_t i = 0; i < clients; ++i) followed += all[i].seen >= base + (int64_t)acked_count;
    printf("{\"clients\":%zu,\"way\":\"%s\",\"rate\":%g,\"writes\":%zu,\"followed_to_end\":%zu,\"taken\":%llu,"
           "\"measured\":%llu,\"p50_us\":%llu,\"p90_us\":%llu,\"p99_us\":%llu,\"max_us\":%llu}\n",
           clients, argv[5], rate, acked_count, followed, (unsigned long long)taken, (unsigned long long)samples,
           (unsigned long long)percentile(0.5), (unsigned long long)percentile(0.9), (unsigned long long)percentile(0.99),
           (unsigned long long)percentile(0.999999));
    return 0;
}
