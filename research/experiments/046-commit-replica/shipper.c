/* Research 046: the owner's side of the link. Takes the shim's cuts on a unix socket (u64le the cut's number, u64le
 * its size, its ops), sends each over pure link as records of whole ops (ops.h: u8 1 on its last, the cut's number,
 * the ops; at most RECORD bytes but for an op larger alone; SHIPPER_RECORD sets it), and answers the shim with the cut's number once the
 * link says the follower saved its last record (pure_link_delivered): the owner's sync waits for that.
 *
 *   shipper SOCKET pure://127.0.0.1:PORT/store KEY FOLLOWER_PUBLIC_HEX     at its end, one JSON line of counts */
#define _GNU_SOURCE
#include "link.h"
#include "seal.h"
#include "ops.h"
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

static size_t RECORD = 1 << 20; /* SHIPPER_RECORD: smaller, so a cut is several records (one torn at a kill) */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t saved = PTHREAD_COND_INITIALIZER;

static void notify(void *context) {
    (void)context;
    pthread_mutex_lock(&lock);
    pthread_cond_broadcast(&saved);
    pthread_mutex_unlock(&lock);
}

static bool whole_read(int fd, void *bytes, size_t size) {
    for (unsigned char *p = bytes; size;) {
        ssize_t n = read(fd, p, size);
        if (n <= 0) { if (n < 0 && errno == EINTR) continue; return false; }
        p += n, size -= (size_t)n;
    }
    return true;
}

int main(int argc, char **argv) {
    if (argc != 5) { fprintf(stderr, "usage: %s SOCKET URL KEY FOLLOWER_PUBLIC_HEX\n", argv[0]); return 2; }
    if (getenv("SHIPPER_RECORD")) RECORD = (size_t)atol(getenv("SHIPPER_RECORD"));
    char error[256], public_hex[2 * PURE_SEAL_KEY + 1];
    unsigned char own_public[PURE_SEAL_KEY], peer[PURE_SEAL_KEY];
    PureSealKey *key = pure_seal_key(argv[3], public_hex, error, sizeof(error));
    if (!key || !pure_seal_peer(public_hex, own_public) || !pure_seal_peer(argv[4], peer)) { fprintf(stderr, "shipper: %s\n", error); return 1; }
    PureLink *link = pure_link_open(argv[2], "owner", 1, key, own_public, error, sizeof(error));
    if (!link) { fprintf(stderr, "shipper: %s\n", error); return 1; }
    pure_link_peer(link, peer);
    if (!pure_link_start(link, notify, NULL, error, sizeof(error))) { fprintf(stderr, "shipper: %s\n", error); return 1; }
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    if (strlen(argv[1]) >= sizeof(address.sun_path)) return 1;
    strcpy(address.sun_path, argv[1]);
    unlink(argv[1]);
    int listening = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listening < 0 || bind(listening, (struct sockaddr *)&address, sizeof(address)) || listen(listening, 1)) {
        perror("shipper: socket");
        return 1;
    }
    printf("{\"ready\":true}\n");
    fflush(stdout);
    int shim = accept(listening, NULL, NULL);
    if (shim < 0) return 1;
    uint64_t seq = 0, cuts = 0, bytes = 0, records = 0, waited_ns = 0;
    unsigned char *cut = NULL, *record = malloc(RECORD + 64);
    size_t room = 0;
    for (;;) {
        unsigned char head[16];
        if (!whole_read(shim, head, sizeof(head))) break; /* the owner ended (or was killed) */
        uint64_t number = get64(head), size = get64(head + 8);
        if (size > room && !(cut = realloc(cut, room = size))) return 1;
        if (!whole_read(shim, cut, size)) break;
        /* whole ops into records */
        for (size_t at = 0; at < size;) {
            size_t length = 9;
            size_t end = at;
            while (end < size) {
                size_t n = op_size(cut + end, size - end);
                if (!n) { fprintf(stderr, "shipper: a torn op in cut %llu\n", (unsigned long long)number); return 1; }
                if (end > at && length + n > RECORD) break;
                length += n, end += n;
            }
            unsigned char *r = length <= RECORD + 64 ? record : malloc(length);
            r[0] = end == size;
            put64(r + 1, number);
            memcpy(r + 9, cut + at, end - at);
            if (!pure_link_queue(link, ++seq, r, length)) { fprintf(stderr, "shipper: not queued\n"); return 1; }
            if (r != record) free(r);
            ++records, bytes += length;
            at = end;
        }
        struct timespec a, b;
        clock_gettime(CLOCK_MONOTONIC, &a);
        pure_link_push(link);
        pthread_mutex_lock(&lock);
        while (pure_link_delivered(link) < seq) {
            struct timespec until;
            clock_gettime(CLOCK_REALTIME, &until);
            until.tv_nsec += 10 * 1000000;
            if (until.tv_nsec >= 1000000000) until.tv_sec += 1, until.tv_nsec -= 1000000000;
            pthread_cond_timedwait(&saved, &lock, &until);
        }
        pthread_mutex_unlock(&lock);
        clock_gettime(CLOCK_MONOTONIC, &b);
        waited_ns += (uint64_t)((b.tv_sec - a.tv_sec) * 1000000000LL + (b.tv_nsec - a.tv_nsec));
        ++cuts;
        unsigned char ack[8];
        put64(ack, number);
        if (write(shim, ack, sizeof(ack)) != sizeof(ack)) break;
    }
    PureLinkCounts counts;
    pure_link_counts(link, &counts);
    printf("{\"cuts\":%llu,\"records\":%llu,\"record_bytes\":%llu,\"waited_ms\":%.1f,\"packets\":%llu,\"resent\":%llu}\n",
           (unsigned long long)cuts, (unsigned long long)records, (unsigned long long)bytes, waited_ns / 1e6,
           (unsigned long long)counts.packets, (unsigned long long)counts.resent);
    pure_link_close(link);
    pure_seal_key_free(key);
    return 0;
}
