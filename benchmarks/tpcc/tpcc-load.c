#define _GNU_SOURCE
/* TPC-C's terminals for benchmarks/tpcc/tpcc.aup over HTTP/1.1: N
 * connections on one epoll thread, each a terminal of a home warehouse
 * (connection mod W, plus one) sending one transaction at a time, picked
 * by the mix of clause 5.2.3 (New-Order 45%, Payment 43%, Order-Status,
 * Delivery and Stock-Level 4% each) with the inputs of clause 2 (NURand
 * customers and items, 1% of New-Orders naming an unused item, 1% of lines
 * and 15% of Payments from another warehouse when there is one). Delivery
 * is one district a request, as tpcc.aup takes it.
 *
 * tpcc-load PORT CONNECTIONS SECONDS WAREHOUSES [SEED [TRANSACTIONS]]
 *
 * With TRANSACTIONS each terminal stops after that many. The inputs come in
 * the order sqlite-tpcc draws them, so one terminal and a seed make the same
 * transactions there (run.py compares the states they leave).
 *
 * Prints one JSON line: transactions a second, New-Orders a minute, and for
 * each kind its count, failures (a New-Order undone for its unused item is
 * not one) and latency percentiles (µs). */
#include <arpa/inet.h>
#include <errno.h>
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

enum { BUCKETS = 64 * 16, RESPONSE = 1 << 20, REQUEST = 4096, KINDS = 5 };
enum { NEW_ORDER, PAYMENT, ORDER_STATUS, DELIVERY, STOCK_LEVEL };
static const char *const names[KINDS] = {"new_order", "payment", "order_status", "delivery", "stock_level"};

typedef struct {
    int fd, home, kind;
    bool unused; /* a New-Order naming an unused item: undone, as it should be */
    char request[REQUEST];
    size_t request_size, sent;
    char *response;
    size_t received;
    uint64_t started, random, left;
} Connection;

static uint64_t now_us(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u;
}

/* Log-linear histograms: 16 buckets a power of two, one a kind. */
static uint64_t histogram[KINDS][BUCKETS], done[KINDS], failed[KINDS], undone;
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
static uint64_t percentile(const uint64_t *counts, uint64_t total, double p) {
    uint64_t want = (uint64_t)((double)total * p), seen = 0;
    for (unsigned i = 0; i < BUCKETS; ++i) {
        seen += counts[i];
        if (seen > want) return bucket_value(i);
    }
    return 0;
}

static int port, warehouses;
static int epoll_fd;

/* xorshift64: each terminal its own sequence. */
static uint64_t next_random(uint64_t *state) {
    uint64_t x = *state;
    x ^= x << 13, x ^= x >> 7, x ^= x << 17;
    return *state = x;
}
static int uniform(uint64_t *state, int low, int high) { return low + (int)(next_random(state) % (uint64_t)(high - low + 1)); }
/* NURand (2.1.6) with a run's constants. */
static int nurand(uint64_t *state, int a, int low, int high, int c) {
    return ((uniform(state, 0, a) | uniform(state, low, high)) + c) % (high - low + 1) + low;
}

static long district_key(int w, int d) { return w * 10L + d; }
static long customer_key(int w, int d, int c) { return district_key(w, d) * 3000 + c; }

/* The terminal's next transaction, as a request. */
static void prepare(Connection *c) {
    uint64_t *r = &c->random;
    int w = c->home, d = uniform(r, 1, 10), roll = uniform(r, 1, 100);
    char body[REQUEST - 256];
    int length = 0;
    const char *path = NULL;
    c->kind = roll <= 45 ? NEW_ORDER : roll <= 88 ? PAYMENT : roll <= 92 ? ORDER_STATUS : roll <= 96 ? DELIVERY : STOCK_LEVEL;
    c->unused = false;
    if (c->kind == NEW_ORDER) {
        int customer = nurand(r, 1023, 1, 3000, 259), count = uniform(r, 5, 15);
        c->unused = uniform(r, 1, 100) == 1;
        length = snprintf(body, sizeof(body), "{\"w\":%d,\"d\":%d,\"c\":%d,\"lines\":[", w, d, customer);
        for (int i = 0; i < count; ++i) {
            int item = c->unused && i == count - 1 ? 100001 : nurand(r, 8191, 1, 100000, 7911);
            int supply = warehouses > 1 && uniform(r, 1, 100) == 1 ? uniform(r, 1, warehouses) : w;
            length += snprintf(body + length, sizeof(body) - (size_t)length, "%s{\"item\":%d,\"supply\":%d,\"quantity\":%d}",
                               i ? "," : "", item, supply, uniform(r, 1, 10));
        }
        length += snprintf(body + length, sizeof(body) - (size_t)length, "]}");
        path = "/inputs/new_order";
    } else if (c->kind == PAYMENT) {
        int customer = nurand(r, 1023, 1, 3000, 259), cw = w, cd = d;
        if (warehouses > 1 && uniform(r, 1, 100) > 85) {
            do cw = uniform(r, 1, warehouses); while (cw == w);
            cd = uniform(r, 1, 10);
        }
        double amount = uniform(r, 100, 500000) / 100.0;
        length = snprintf(body, sizeof(body), "{\"w\":%d,\"d\":%d,\"cw\":%d,\"cd\":%d,\"c\":%d,\"amount\":%.2f}", w, d, cw,
                          cd, customer, amount);
        path = "/inputs/payment";
    } else if (c->kind == DELIVERY) {
        length = snprintf(body, sizeof(body), "{\"w\":%d,\"d\":%d,\"carrier\":%d}", w, d, uniform(r, 1, 10));
        path = "/inputs/delivery";
    }
    if (path)
        c->request_size = (size_t)snprintf(c->request, sizeof(c->request),
            "POST %s HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\nContent-Length: %d\r\n\r\n%s",
            path, length, body);
    else if (c->kind == ORDER_STATUS)
        c->request_size = (size_t)snprintf(c->request, sizeof(c->request),
            "GET /views/order_status?customer=%ld HTTP/1.1\r\nHost: localhost\r\n\r\n", customer_key(w, d, nurand(r, 1023, 1, 3000, 259)));
    else
        c->request_size = (size_t)snprintf(c->request, sizeof(c->request),
            "GET /views/stock_level?w=%d&d=%d&threshold=%d HTTP/1.1\r\nHost: localhost\r\n\r\n", w, d, uniform(r, 10, 20));
    c->sent = c->received = 0;
    c->started = now_us();
}

static void open_connection(Connection *c) {
    c->fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    int one = 1;
    setsockopt(c->fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port),
                                  .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    connect(c->fd, (struct sockaddr *)&address, sizeof(address));
    struct epoll_event event = {.events = EPOLLOUT | EPOLLIN, .data.ptr = c};
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, c->fd, &event);
}

static void reopen(Connection *c) {
    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, c->fd, NULL);
    close(c->fd);
    open_connection(c);
}

/* A complete response in c->response: its status and length, or false. */
static bool complete(Connection *c, int *status, size_t *length) {
    char *end = memmem(c->response, c->received, "\r\n\r\n", 4);
    if (!end) return false;
    size_t header = (size_t)(end - c->response) + 4, content = 0;
    char *field = memmem(c->response, header, "Content-Length:", 15);
    if (!field) field = memmem(c->response, header, "content-length:", 15);
    if (field) content = strtoul(field + 15, NULL, 10);
    if (c->received < header + content) return false;
    *status = atoi(c->response + 9);
    *length = header + content;
    return true;
}

int main(int argc, char **argv) {
    if (argc < 5) {
        fputs("tpcc-load PORT CONNECTIONS SECONDS WAREHOUSES [SEED [TRANSACTIONS]]\n", stderr);
        return 2;
    }
    port = atoi(argv[1]);
    int count = atoi(argv[2]);
    double seconds = atof(argv[3]);
    warehouses = atoi(argv[4]);
    uint64_t seed = argc > 5 ? strtoull(argv[5], NULL, 10) : 1, each = argc > 6 ? strtoull(argv[6], NULL, 10) : UINT64_MAX;
    epoll_fd = epoll_create1(0);
    Connection *all = calloc((size_t)count, sizeof(*all));
    for (int i = 0; i < count; ++i) {
        all[i].home = i % warehouses + 1;
        all[i].random = 0x9e3779b97f4a7c15u * (uint64_t)(i + 1) ^ seed * 0xbf58476d1ce4e5b9u;
        all[i].left = each;
        all[i].response = malloc(RESPONSE);
        open_connection(&all[i]);
        prepare(&all[i]);
    }
    uint64_t start = now_us(), stop = start + (uint64_t)(seconds * 1e6), errors = 0;
    int finished = 0;
    struct epoll_event events[256];
    while (now_us() < stop && finished < count) {
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
                while (c->sent < c->request_size) {
                    ssize_t n = send(c->fd, c->request + c->sent, c->request_size - c->sent, MSG_NOSIGNAL);
                    if (n <= 0) break;
                    c->sent += (size_t)n;
                }
                if (c->sent == c->request_size) {
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
                size_t length;
                if (!complete(c, &status, &length)) continue;
                bool expected = status == 200 || (c->unused && status == 404);
                ++done[c->kind];
                undone += c->unused && status == 404;
                failed[c->kind] += !expected;
                ++histogram[c->kind][bucket(now_us() - c->started)];
                if (!--c->left) { /* this terminal's transactions are done */
                    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, c->fd, NULL);
                    close(c->fd);
                    ++finished;
                    continue;
                }
                struct epoll_event event = {.events = EPOLLOUT | EPOLLIN, .data.ptr = c};
                epoll_ctl(epoll_fd, EPOLL_CTL_MOD, c->fd, &event);
                prepare(c);
            }
        }
    }
    double elapsed = (double)(now_us() - start) / 1e6;
    uint64_t total = 0;
    for (int k = 0; k < KINDS; ++k) total += done[k];
    printf("{\"system\":\"pure\",\"warehouses\":%d,\"terminals\":%d,\"transactions_per_second\":%.0f,"
           "\"new_orders_per_minute\":%.0f,\"undone_new_orders\":%llu,\"errors\":%llu",
           warehouses, count, (double)total / elapsed, (double)(done[NEW_ORDER] - undone) * 60 / elapsed,
           (unsigned long long)undone, (unsigned long long)errors);
    for (int k = 0; k < KINDS; ++k)
        printf(",\"%s\":{\"done\":%llu,\"failed\":%llu,\"p50_us\":%llu,\"p99_us\":%llu}", names[k], (unsigned long long)done[k],
               (unsigned long long)failed[k], (unsigned long long)percentile(histogram[k], done[k], 0.5),
               (unsigned long long)percentile(histogram[k], done[k], 0.99));
    puts("}");
    return 0;
}
