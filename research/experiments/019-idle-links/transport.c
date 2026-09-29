#define _GNU_SOURCE
/* A link and a listener in one process, with no app and no store: what the
 * transport alone costs a record (M5 of docs/design/011-pure-transport.md).
 * The listener says each delivery stored as it hands it over.
 *
 *   transport SENDER.pmod RECEIVER.pmod COUNT SIZE [WINDOW]
 *       COUNT records of SIZE item bytes (an order of the sender's output
 *       "ordered", to the receiver's input "order"), at most WINDOW waiting
 *       at once (100,000); a line of JSON: how long, records a second, and
 *       CPU a record by thread (the link's, the listener's, the queuing
 *       one's). WINDOW 0: all are queued before the link starts (a backlog
 *       drained). Built beside pure-crypto.so (build/). */
#include "link.h"
#include "module.h"
#include "seal.h"
#include <arpa/inet.h>
#include <dirent.h>
#include <netinet/in.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static PureModule sender, receiver;
static PureLinkListener *listener;
static atomic_uint_fast64_t delivered;
static unsigned char *payload;
static size_t payload_size;
static uint64_t heard, wrong; /* the last record taken; deliveries not as sent */

/* Records as sent, each once, in order: the one after the last taken on
 * (one taken before would be a delivery again), of the payload's bytes. */
static bool deliver(void *context, const PureLinkDelivery *d) {
    (void)context;
    const unsigned char *at = d->records;
    for (unsigned k = 0; k < d->count; ++k) {
        uint32_t size = (uint32_t)at[0] | (uint32_t)at[1] << 8 | (uint32_t)at[2] << 16 | (uint32_t)at[3] << 24;
        if (d->first + k != heard + 1 + k || size != payload_size || memcmp(at + 4, payload, size)) ++wrong;
        at += 4 + size;
    }
    if ((size_t)(at - d->records) != d->size || d->first > heard + 1) ++wrong;
    if (d->first + d->count - 1 > heard) heard = d->first + d->count - 1;
    atomic_fetch_add(&delivered, d->count);
    pure_link_done(listener, d->token, heard, true, false);
    return true;
}

static void notify(void *context) { (void)context; }

/* The receiver's input a stream goes to: "order", of the sender's shape. */
static int input_of(void *context, const char *name, uint32_t shape) {
    (void)context;
    int index = pure_module_input(&receiver, name);
    return index >= 0 && pure_module_shape(&receiver, (uint32_t)index) == shape ? index : -1;
}

static uint64_t now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

/* CPU ns of this process's threads named name (NULL: all). */
static uint64_t thread_cpu(const char *name) {
    uint64_t total = 0;
    DIR *tasks = opendir("/proc/self/task");
    for (struct dirent *e; tasks && (e = readdir(tasks));) {
        if (e->d_name[0] == '.') continue;
        char path[320], comm[64] = "";
        snprintf(path, sizeof(path), "/proc/self/task/%s/comm", e->d_name);
        FILE *f = fopen(path, "r");
        if (f) {
            if (!fgets(comm, sizeof(comm), f)) comm[0] = 0;
            fclose(f);
        }
        comm[strcspn(comm, "\n")] = 0;
        if (name && strcmp(comm, name)) continue;
        snprintf(path, sizeof(path), "/proc/self/task/%s/schedstat", e->d_name);
        unsigned long long ns = 0;
        if ((f = fopen(path, "r"))) {
            if (fscanf(f, "%llu", &ns) != 1) ns = 0;
            fclose(f);
        }
        total += ns;
    }
    if (tasks) closedir(tasks);
    return total;
}

static void put32(unsigned char *p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = (unsigned char)(v >> 8 * i); }

/* The stored form of an order of input: u32 fields 1, texts of size bytes. */
static size_t order(const PureModule *m, uint32_t input, size_t size, unsigned char *out) {
    const PureInputShape *s = &m->input[input];
    memset(out, 0, s->prefix);
    size_t at = s->prefix;
    for (uint32_t f = 0; f < s->field_count; ++f) {
        const PureField *field = &m->field[s->field_start + f];
        if (field->type == PURE_FIELD_U32) put32(out + field->offset, 1);
        else if (field->type == PURE_FIELD_TEXT || field->type == PURE_FIELD_BYTES) {
            size_t length = f ? 5 : size; /* the first text is the item; the rest ("Seoul") short */
            if (field->offset != PURE_FIELD_REST) put32(out + field->offset, (uint32_t)length);
            memset(out + at, 'x', length);
            at += length;
        }
    }
    return at;
}

int main(int argc, char **argv) {
    if (argc < 5) {
        fputs("transport SENDER.pmod RECEIVER.pmod COUNT SIZE [WINDOW]\n", stderr);
        return 2;
    }
    char error[256], dir[] = "/tmp/pure-transport-XXXXXX", path[512], hex[2 * PURE_SEAL_KEY + 1];
    uint64_t count = strtoull(argv[3], NULL, 10), window = argc > 5 ? strtoull(argv[5], NULL, 10) : 100000;
    size_t size = strtoull(argv[4], NULL, 10);
    if (pure_module_read(argv[1], &sender, error, sizeof(error)) != PURE_OK ||
        pure_module_read(argv[2], &receiver, error, sizeof(error)) != PURE_OK || !mkdtemp(dir)) {
        fprintf(stderr, "transport: %s\n", error);
        return 1;
    }
    unsigned char public_a[PURE_SEAL_KEY], public_b[PURE_SEAL_KEY];
    snprintf(path, sizeof(path), "%s/a.key", dir);
    PureSealKey *a = pure_seal_key(path, hex, error, sizeof(error));
    if (!a || !pure_seal_peer(hex, public_a)) return fprintf(stderr, "transport: %s\n", error), 1;
    snprintf(path, sizeof(path), "%s/b.key", dir);
    PureSealKey *b = pure_seal_key(path, hex, error, sizeof(error));
    if (!b || !pure_seal_peer(hex, public_b)) return fprintf(stderr, "transport: %s\n", error), 1;
    /* A port nobody uses. */
    int probe = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in any = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    socklen_t length = sizeof(any);
    if (probe < 0 || bind(probe, (struct sockaddr *)&any, sizeof(any)) || getsockname(probe, (struct sockaddr *)&any, &length))
        return 1;
    unsigned port = ntohs(any.sin_port);
    close(probe);
    PureLinkReceiver *r = pure_link_receiver(b, public_b, &(PureLinkHost){input_of, deliver, NULL}, error, sizeof(error));
    if (!r || !pure_link_allow(r, "bench.ordered", public_a, error, sizeof(error)) ||
        !(listener = pure_link_listen(r, port, error, sizeof(error))) || !pure_link_serve(listener, error, sizeof(error)))
        return fprintf(stderr, "transport: %s\n", error), 1;
    char url[128];
    snprintf(url, sizeof(url), "pure://127.0.0.1:%u/order", port);
    int output = pure_module_input(&sender, "ordered");
    PureLink *link = output < 0 ? NULL
                                : pure_link_open(url, "bench.ordered", pure_module_shape(&sender, (uint32_t)output), a,
                                                 public_a, error, sizeof(error));
    if (!link) return fprintf(stderr, "transport: %s\n", output < 0 ? "no output 'ordered'" : error), 1;
    pure_link_peer(link, public_b);
    payload = malloc(size + 256);
    payload_size = order(&sender, (uint32_t)output, size, payload);
    for (size_t i = 0; i < payload_size; ++i) payload[i] ^= (unsigned char)(i * 7); /* bytes that differ by place */
    uint64_t first = 1;
    if (!window) /* a backlog: all queued before the link starts */
        for (; first <= count; ++first)
            if (!pure_link_queue(link, first, payload, payload_size)) return fputs("transport: cannot queue\n", stderr), 1;
    uint64_t began = now_ns(), cpu_link = 0, cpu_listen = thread_cpu("pure-listen"), cpu_all = thread_cpu(NULL);
    if (!pure_link_start(link, notify, NULL, error, sizeof(error))) return fprintf(stderr, "transport: %s\n", error), 1;
    for (uint64_t seq = first; seq <= count; ++seq) {
        while (seq - pure_link_delivered(link) > window) {
            struct timespec pause = {0, 50000};
            nanosleep(&pause, NULL);
        }
        if (!pure_link_queue(link, seq, payload, payload_size)) return fputs("transport: cannot queue\n", stderr), 1;
    }
    while (pure_link_delivered(link) < count) {
        struct timespec pause = {0, 50000};
        nanosleep(&pause, NULL);
    }
    double seconds = (double)(now_ns() - began) / 1e9;
    uint64_t link_ns = thread_cpu("pure-link") - cpu_link, listen_ns = thread_cpu("pure-listen") - cpu_listen,
             all_ns = thread_cpu(NULL) - cpu_all;
    PureLinkCounts sent;
    pure_link_counts(link, &sent);
    printf("{\"records\":%llu,\"record_bytes\":%zu,\"seconds\":%.3f,\"per_second\":%.0f,\"packets\":%llu,"
           "\"wrong\":%llu,\"resent\":%llu,\"link_us_per_record\":%.3f,\"listener_us_per_record\":%.3f,\"other_us_per_record\":%.3f}\n",
           (unsigned long long)count, payload_size, seconds, (double)count / seconds, (unsigned long long)sent.packets,
           (unsigned long long)wrong, (unsigned long long)sent.resent, (double)link_ns / 1e3 / (double)count, (double)listen_ns / 1e3 / (double)count,
           (double)(all_ns - link_ns - listen_ns) / 1e3 / (double)count);
    pure_link_close(link);
    PureLinkCounts heard;
    pure_link_listener_close(listener, &heard);
    snprintf(path, sizeof(path), "rm -rf %s", dir);
    return system(path) || wrong ? 1 : 0;
}
