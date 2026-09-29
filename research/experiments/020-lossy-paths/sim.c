/* M6 of docs/design/011-pure-transport.md in virtual time: one link
 * through a path of the sim's own. Each direction: random loss, a
 * bottleneck (rate, queue limit, drop-tail), a one-way delay and random
 * extra delay (so packets reorder). Records of a size, queued all at once
 * (a backlog), stored by the host at once. It takes in link-protocol.c
 * whole, to read the sender's window and RTT.
 *
 *   sim SIZE COUNT DELAY_MS LOSS_PERMIL JITTER_MS RATE_MBIT QUEUE_PACKETS SEED [PACE]
 * prints a line of JSON: virtual seconds, records a second, packets sent,
 * dropped (by loss, by the queue), called lost, of them late, congestion
 * episodes undone, sent again, and the window and RTT seen each 100 ms.
 * PACE: records queued a second (else all at once), and the time each took
 * from queued to handed over (p50, p99, max), and probes sent (PTO).
 *
 *   cc -O2 -std=c17 -I link research/experiments/020-lossy-paths/sim.c link/crypto.c -lm -ldl -o build/sim
 *   PURE_CRYPTO=build/pure-crypto.so build/sim 1000 30000 10 10 5 50 100 1 */
#include "link-protocol.c"
#include <math.h>

typedef struct {
    uint64_t at;
    bool to_receiver;
    size_t size;
    unsigned char bytes[PURE_LINK_DATAGRAM_MOST];
} Packet;

static uint64_t sim_seed;
static uint64_t sim_random(void) {
    sim_seed ^= sim_seed << 13, sim_seed ^= sim_seed >> 7, sim_seed ^= sim_seed << 17;
    return sim_seed;
}

static Packet *packets;
static size_t packet_count, packet_capacity;
static uint64_t now, delay_ns, jitter, loss_permil, rate_bps, queue_limit;
static uint64_t link_free[2]; /* when each direction's bottleneck is free */
static uint64_t dropped_loss, dropped_queue, sent_count;
static uint64_t heard, records, size;
static PureLinkDone dones[64];
static size_t done_count;
static uint64_t *queued_at, *waited;

static void put(bool to_receiver, const unsigned char *bytes, size_t n) {
    ++sent_count;
    if (sim_random() % 1000 < loss_permil) {
        ++dropped_loss;
        return;
    }
    int d = to_receiver ? 0 : 1;
    uint64_t start = link_free[d] > now ? link_free[d] : now;
    uint64_t serial = rate_bps ? (uint64_t)((n + 28) * 8) * 1000000000u / rate_bps : 0;
    /* queue: packets waiting before this one, by time */
    if (rate_bps && serial && (start - now) / serial >= queue_limit) {
        ++dropped_queue;
        return;
    }
    link_free[d] = start + serial;
    if (packet_count == packet_capacity) {
        packet_capacity = packet_capacity ? 2 * packet_capacity : 1024;
        packets = realloc(packets, packet_capacity * sizeof(*packets));
    }
    Packet *p = &packets[packet_count++];
    p->at = start + serial + delay_ns + (jitter ? sim_random() % jitter : 0);
    p->to_receiver = to_receiver;
    p->size = n;
    memcpy(p->bytes, bytes, n);
}

static int by_value(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

static bool sim_answer(void *context, const unsigned char *packet, size_t n, const unsigned char *to, size_t to_size) {
    (void)context, (void)to, (void)to_size;
    put(false, packet, n);
    return true;
}

static int input_of(void *context, const char *name, uint32_t shape) {
    (void)context, (void)name, (void)shape;
    return 0;
}

static bool sim_deliver(void *context, const PureLinkDelivery *d) {
    (void)context;
    if (done_count == 64) return false;
    uint64_t h = heard;
    for (unsigned k = 0; k < d->count; ++k)
        if (d->first + k == h + 1) {
            h = d->first + k;
            if (queued_at) waited[h] = now - queued_at[h];
        }
    heard = h;
    dones[done_count++] = (PureLinkDone){d->token, heard, true, d->first > heard + 1};
    return true;
}

int main(int argc, char **argv) {
    if (argc < 9) return 2;
    size = strtoull(argv[1], NULL, 10);
    records = strtoull(argv[2], NULL, 10);
    delay_ns = strtoull(argv[3], NULL, 10) * MS;
    loss_permil = strtoull(argv[4], NULL, 10);
    jitter = strtoull(argv[5], NULL, 10) * MS;
    rate_bps = strtoull(argv[6], NULL, 10) * 1000000u;
    queue_limit = strtoull(argv[7], NULL, 10);
    sim_seed = strtoull(argv[8], NULL, 10) * 0x9e3779b97f4a7c15u | 1;
    double pace = argc > 9 ? strtod(argv[9], NULL) : 0;
    if (pace > 0) queued_at = calloc(records + 1, sizeof(*queued_at)), waited = calloc(records + 1, sizeof(*waited));
    char dir[] = "/tmp/pure-sim-XXXXXX", path[512], hex[65], error[256];
    if (!mkdtemp(dir)) return 1;
    unsigned char pa[32], pb[32];
    snprintf(path, sizeof(path), "%s/a", dir);
    PureSealKey *a = pure_seal_key(path, hex, error, sizeof(error));
    pure_seal_peer(hex, pa);
    snprintf(path, sizeof(path), "%s/b", dir);
    PureSealKey *b = pure_seal_key(path, hex, error, sizeof(error));
    pure_seal_peer(hex, pb);
    PureLinkSender *s = pure_link_sender("sim.out", "order", 1, a, pa);
    pure_link_sender_peer(s, pb);
    PureLinkReceiver *r = pure_link_receiver(b, pb, &(PureLinkHost){input_of, sim_deliver, NULL}, error, sizeof(error));
    pure_link_allow(r, "sim.out", pa, error, sizeof(error));
    unsigned char *bytes = calloc(1, size + 1);
    uint64_t queued = 0, ptos = 0;
    for (uint64_t seq = 1; !queued_at && seq <= records; ++seq)
        if (!pure_link_sender_queue(s, seq, bytes, size)) return 1;
    if (!queued_at) queued = records;
    unsigned char out[PURE_LINK_DATAGRAM_MOST];
    uint64_t sample_at = 0, window_sum = 0, rtt_sum = 0, samples = 0, window_least = UINT64_MAX;
    PureLinkDone told[64];
    while (heard < records || s->stored < records) {
        for (; queued < records && (uint64_t)((double)queued * 1e9 / pace) <= now; ++queued) {
            queued_at[queued + 1] = now;
            if (!pure_link_sender_queue(s, queued + 1, bytes, size)) return 1;
        }
        uint64_t number;
        for (size_t n; (n = pure_link_sender_packet(s, out, false, now, &number));) put(true, out, n);
        uint64_t next = UINT64_MAX, due = pure_link_sender_due(s), turn = pure_link_receiver_due(r);
        for (size_t k = 0; k < packet_count; ++k)
            if (packets[k].at < next) next = packets[k].at;
        if (due && due < next) next = due;
        if (turn && turn < next) next = turn;
        if (done_count && now + 1000 < next) next = now + 1000;
        if (queued < records && (uint64_t)((double)queued * 1e9 / pace) < next) next = (uint64_t)((double)queued * 1e9 / pace);
        if (next == UINT64_MAX || next > 3600000 * MS) {
            fprintf(stderr, "sim: stuck at %llu of %llu\n", (unsigned long long)heard, (unsigned long long)records);
            return 1;
        }
        now = next > now ? next : now;
        while (now >= sample_at) {
            window_sum += s->cc.window, rtt_sum += s->smoothed_rtt, ++samples;
            if (s->cc.window < window_least) window_least = s->cc.window;
            sample_at += 100 * MS;
        }
        for (size_t k = 0; k < packet_count;) {
            Packet *p = &packets[k];
            if (p->at > now) {
                ++k;
                continue;
            }
            if (p->to_receiver) pure_link_receiver_take(r, p->bytes, p->size, "s", 1, now);
            else pure_link_sender_take(s, p->bytes, p->size, now);
            *p = packets[--packet_count];
        }
        unsigned before = s->pto_count;
        pure_link_sender_tick(s, now);
        ptos += s->pto_count > before;
        size_t count = done_count;
        memcpy(told, dones, count * sizeof(*dones));
        done_count = 0;
        pure_link_receiver_turn(r, told, count, now, sim_answer, NULL);
    }
    double seconds = (double)now / 1e9;
    printf("{\"size\":%llu,\"records\":%llu,\"delay_ms\":%llu,\"loss_permil\":%llu,\"jitter_ms\":%llu,\"rate_mbit\":%llu,"
           "\"queue\":%llu,\"seconds\":%.3f,\"per_second\":%.0f,\"mbit\":%.2f,\"sent\":%llu,\"dropped_loss\":%llu,"
           "\"dropped_queue\":%llu,\"declared_lost\":%llu,\"late\":%llu,\"undone\":%llu,\"resent\":%llu,\"window_mean\":%.0f,\"window_least\":%llu,"
           "\"srtt_mean_ms\":%.2f,\"probes\":%llu",
           (unsigned long long)size, (unsigned long long)records, (unsigned long long)(delay_ns / MS),
           (unsigned long long)loss_permil, (unsigned long long)(jitter / MS), (unsigned long long)(rate_bps / 1000000),
           (unsigned long long)queue_limit, seconds, records / seconds, records * (double)size * 8 / seconds / 1e6,
           (unsigned long long)s->counts.packets, (unsigned long long)dropped_loss, (unsigned long long)dropped_queue,
           (unsigned long long)s->counts.lost, (unsigned long long)s->counts.late, (unsigned long long)s->counts.undone, (unsigned long long)s->counts.resent, (double)window_sum / samples,
           (unsigned long long)window_least, (double)rtt_sum / samples / 1e6, (unsigned long long)ptos);
    if (queued_at) {
        qsort(waited + 1, records, sizeof(*waited), by_value);
        printf(",\"waited_ms_p50\":%.3f,\"waited_ms_p99\":%.3f,\"waited_ms_max\":%.3f", waited[1 + records / 2] / 1e6,
               waited[1 + records * 99 / 100] / 1e6, waited[records] / 1e6);
    }
    printf("}\n");
    snprintf(path, sizeof(path), "rm -rf %s", dir);
    return system(path) ? 1 : 0;
}
