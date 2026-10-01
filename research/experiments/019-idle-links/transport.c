#define _GNU_SOURCE
/* A link and a listener in one process, with no app and no store: what the
 * transport alone costs a record (M5 of docs/design/011-pure-transport.md),
 * and how it fares on a path of loss and queues (M6).
 * The listener says each delivery stored as it hands it over.
 *
 *   transport SENDER.pmod RECEIVER.pmod COUNT SIZE [WINDOW [RATE [SECONDS]]]
 *       COUNT records of SIZE item bytes (an order of the sender's output
 *       "ordered", to the receiver's input "order"), at most WINDOW waiting
 *       at once (100,000); a line of JSON: how long, records a second, and
 *       CPU a record by thread (the link's, the listener's, the queuing
 *       one's), the records handed over in each 100 ms. WINDOW 0: all are
 *       queued before the link starts (a backlog drained). RATE: records
 *       queued a second (0: as fast as the window allows), and the time
 *       from each queued to handed over. SECONDS: stop after that long
 *       (0: once all are handed over). TRANSPORT_GSO=0: no UDP GSO bursts;
 *       TRANSPORT_PORT: the listener's port.
 *       Built beside pure-crypto.so (build/). */
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
static uint64_t began, *queued_at, *waited; /* by number: when queued, how long until handed over (ns) */
enum { BUCKETS = 36000 };                   /* an hour of 100 ms */
static uint32_t timeline[BUCKETS];

static uint64_t now_ns(void);

/* Records applied as a host does: from the one after the last taken on,
 * each as sent (else wrong); ones taken before are let be (the listener
 * does not know yet how far this side heard: its first delivery after the
 * link starts may begin anywhere); a gap stops it, and says so. */
static bool deliver(void *context, const PureLinkDelivery *d) {
    (void)context;
    const unsigned char *at = d->records;
    uint64_t now = now_ns(), took = 0;
    bool gap = false;
    for (unsigned k = 0; k < d->count; ++k) {
        uint32_t size = (uint32_t)at[0] | (uint32_t)at[1] << 8 | (uint32_t)at[2] << 16 | (uint32_t)at[3] << 24;
        uint64_t seq = d->first + k;
        if (seq > heard + 1) {
            gap = true;
            break;
        }
        if (seq == heard + 1) {
            if (size != payload_size || memcmp(at + 4, payload, size)) ++wrong;
            if (queued_at) waited[seq] = now - queued_at[seq];
            heard = seq;
            ++took;
        }
        at += 4 + size;
    }
    if ((now - began) / 100000000u < BUCKETS) timeline[(now - began) / 100000000u] += (uint32_t)took;
    atomic_fetch_add(&delivered, took);
    pure_link_done(listener, d->token, heard, true, gap);
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

static int by_value(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
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
    double rate = argc > 6 ? strtod(argv[6], NULL) : 0, limit = argc > 7 ? strtod(argv[7], NULL) : 0;
    const char *gso = getenv("TRANSPORT_GSO");
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
    /* The port TRANSPORT_PORT names, or one nobody uses. */
    int probe = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in any = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    socklen_t length = sizeof(any);
    if (probe < 0 || bind(probe, (struct sockaddr *)&any, sizeof(any)) || getsockname(probe, (struct sockaddr *)&any, &length))
        return 1;
    unsigned port = getenv("TRANSPORT_PORT") ? (unsigned)atoi(getenv("TRANSPORT_PORT")) : ntohs(any.sin_port);
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
    if (gso && !strcmp(gso, "0")) pure_link_gso(link, false);
    if (rate > 0 && (!(queued_at = calloc(count + 1, sizeof(*queued_at))) || !(waited = calloc(count + 1, sizeof(*waited)))))
        return fputs("transport: out of memory\n", stderr), 1;
    payload = malloc(size + 256);
    payload_size = order(&sender, (uint32_t)output, size, payload);
    for (size_t i = 0; i < payload_size; ++i) payload[i] ^= (unsigned char)(i * 7); /* bytes that differ by place */
    uint64_t first = 1;
    if (!window) /* a backlog: all queued before the link starts */
        for (; first <= count; ++first)
            if (!pure_link_queue(link, first, payload, payload_size)) return fputs("transport: cannot queue\n", stderr), 1;
    if (!window) pure_link_push(link);
    began = now_ns();
    uint64_t cpu_link = 0, cpu_listen = thread_cpu("pure-listen"), cpu_all = thread_cpu(NULL);
    uint64_t end = limit > 0 ? began + (uint64_t)(limit * 1e9) : UINT64_MAX;
    if (!pure_link_start(link, notify, NULL, error, sizeof(error))) return fprintf(stderr, "transport: %s\n", error), 1;
    for (uint64_t seq = first; seq <= count && now_ns() < end; ++seq) {
        uint64_t due = rate > 0 ? began + (uint64_t)((double)(seq - first) * 1e9 / rate) : 0;
        while ((seq - pure_link_delivered(link) > window || now_ns() < due) && now_ns() < end) {
            struct timespec pause = {0, 50000};
            nanosleep(&pause, NULL);
        }
        if (now_ns() >= end) break;
        if (queued_at) queued_at[seq] = now_ns();
        if (!pure_link_queue(link, seq, payload, payload_size)) return fputs("transport: cannot queue\n", stderr), 1;
        pure_link_push(link);
    }
    while (pure_link_delivered(link) < count && now_ns() < end) {
        struct timespec pause = {0, 50000};
        nanosleep(&pause, NULL);
    }
    double seconds = (double)((end < now_ns() ? end : now_ns()) - began) / 1e9;
    uint64_t link_ns = thread_cpu("pure-send") - cpu_link, listen_ns = thread_cpu("pure-listen") - cpu_listen,
             all_ns = thread_cpu(NULL) - cpu_all;
    PureLinkCounts sent, got;
    pure_link_counts(link, &sent);
    pure_link_close(link);
    pure_link_listener_close(listener, &got);
    uint64_t done = heard; /* in order, as the listener handed them over */
    if (!done) done = 1;
    printf("{\"records\":%llu,\"delivered\":%llu,\"record_bytes\":%zu,\"seconds\":%.3f,\"per_second\":%.0f,\"mbit\":%.2f,"
           "\"packets\":%llu,\"wrong\":%llu,\"resent\":%llu,\"lost\":%llu,\"late\":%llu,\"undone\":%llu,"
           "\"link_us_per_record\":%.3f,\"listener_us_per_record\":%.3f,\"other_us_per_record\":%.3f",
           (unsigned long long)count, (unsigned long long)heard, payload_size, seconds, (double)heard / seconds,
           (double)heard * (double)payload_size * 8 / seconds / 1e6, (unsigned long long)sent.packets,
           (unsigned long long)wrong, (unsigned long long)sent.resent, (unsigned long long)sent.lost,
           (unsigned long long)sent.late, (unsigned long long)sent.undone, (double)link_ns / 1e3 / (double)done,
           (double)listen_ns / 1e3 / (double)done, (double)(all_ns - link_ns - listen_ns) / 1e3 / (double)done);
    if (queued_at && heard) { /* the waits of those handed over */
        qsort(waited + 1, heard, sizeof(*waited), by_value);
        printf(",\"waited_ms_p50\":%.3f,\"waited_ms_p99\":%.3f,\"waited_ms_max\":%.3f", (double)waited[1 + heard / 2] / 1e6,
               (double)waited[1 + heard * 99 / 100] / 1e6, (double)waited[heard] / 1e6);
    }
    size_t last = 0;
    for (size_t b = 0; b < BUCKETS; ++b)
        if (timeline[b]) last = b;
    printf(",\"timeline_100ms\":[");
    for (size_t b = 0; b <= last; ++b) printf("%s%u", b ? "," : "", timeline[b]);
    printf("]}\n");
    snprintf(path, sizeof(path), "rm -rf %s", dir);
    return system(path) || wrong || (!limit && heard < count) ? 1 : 0;
}
