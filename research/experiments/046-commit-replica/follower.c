/* Research 046: the follower. Hears the owner's records over pure link (a receiver on a UDP port) and does each op
 * again under its own directory, in order (ops.h): the same bytes at the same places, the same names created, renamed
 * and removed. A record that ends its cut is followed by an fsync of every file and directory the cut touched; then the
 * records are said saved (pure_link_done), which is what the owner's sync waits for. Its heard (the last record done)
 * is kept in memory: a follower started again is not this experiment's. FOLLOWER_SYNC=data: it syncs as the owner's
 * store does instead (research 050): fdatasync a file, and fsync a directory only when the cut made, renamed or removed
 * a name.
 *
 *   follower keys DIR                                   makes DIR/owner.key and DIR/follower.key, prints both public halves
 *   follower DIR PORT KEY OWNER_PUBLIC_HEX              runs until SIGTERM, then one JSON line of counts */
#define _GNU_SOURCE
#include "link.h"
#include "seal.h"
#include "ops.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int directory = -1;
static PureLinkListener *listener;
static uint64_t heard, cuts, applied_ops, applied_bytes;
static bool within_cut; /* the last record done did not end its cut: the store holds part of one */
static volatile sig_atomic_t stopping;

typedef struct Job { struct Job *next; void *token; uint64_t first; unsigned count; unsigned char *records; size_t size; } Job;
static Job *head, *tail;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t waiting = PTHREAD_COND_INITIALIZER;

static void fail(const char *what, const char *path) {
    fprintf(stderr, "follower: %s %s: %s\n", what, path, strerror(errno));
    exit(1);
}

/* The paths a cut touched, to sync at its end. */
static char *touched[4096];
static unsigned touched_count;
static void touch(const char *path) {
    for (unsigned i = 0; i < touched_count; ++i) if (!strcmp(touched[i], path)) return;
    if (touched_count < 4096) touched[touched_count++] = strdup(path);
}
/* Its syncs' time: of files, of directories (the store's own at each cut's end too), and each cut's whole chain. */
static uint64_t file_syncs, file_sync_ns, directory_syncs, directory_sync_ns, *chain_ns;
static size_t chains, chain_room;
static bool as_data, named; /* FOLLOWER_SYNC=data; the cut so far made, renamed or removed a name */
static uint64_t now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}
static void timed_sync(int fd) {
    struct stat st;
    bool is_directory = !fstat(fd, &st) && S_ISDIR(st.st_mode);
    if (as_data && is_directory && !named) return;
    uint64_t began = now_ns();
    if (as_data && !is_directory) fdatasync(fd);
    else fsync(fd);
    uint64_t took = now_ns() - began;
    if (is_directory) ++directory_syncs, directory_sync_ns += took;
    else ++file_syncs, file_sync_ns += took;
}
static void sync_touched(void) {
    uint64_t began = now_ns();
    for (unsigned i = 0; i < touched_count; ++i) {
        int fd = openat(directory, touched[i][0] ? touched[i] : ".", O_RDONLY | O_CLOEXEC);
        if (fd >= 0) timed_sync(fd), close(fd); /* one renamed or removed since is synced as its directory is */
        free(touched[i]);
    }
    touched_count = 0;
    timed_sync(directory);
    named = false;
    if (chains == chain_room) {
        size_t room = chain_room ? 2 * chain_room : 1024;
        uint64_t *grown = realloc(chain_ns, room * sizeof(*grown));
        if (grown) chain_ns = grown, chain_room = room;
    }
    if (chains < chain_room) chain_ns[chains++] = now_ns() - began;
}
static int by_value(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

static const char *name_of(const unsigned char *p, char *out) {
    uint16_t n = get16(p + 1);
    memcpy(out, p + 3, n);
    out[n] = 0;
    return out[0] ? out : ".";
}

static void apply(const unsigned char *p, size_t size) {
    char path[PATH_MAX], other[PATH_MAX];
    const char *at = name_of(p, path);
    const unsigned char *rest = p + 3 + get16(p + 1);
    int fd;
    switch (p[0]) {
    case OP_CREATE: {
        uint32_t flags = get32(rest), mode = get32(rest + 4);
        if ((fd = openat(directory, at, O_WRONLY | O_CLOEXEC | (int)flags, (mode_t)mode)) < 0) fail("create", at);
        close(fd);
        break;
    }
    case OP_WRITE: {
        uint64_t offset = get64(rest);
        uint32_t length = get32(rest + 8);
        if ((fd = openat(directory, at, O_WRONLY | O_CLOEXEC)) < 0) fail("open", at);
        for (const unsigned char *b = rest + 12; length;) {
            ssize_t n = pwrite(fd, b, length, (off_t)offset);
            if (n <= 0) fail("write", at);
            b += n, offset += (uint64_t)n, length -= (uint32_t)n;
        }
        close(fd);
        break;
    }
    case OP_TRUNCATE:
        if ((fd = openat(directory, at, O_WRONLY | O_CLOEXEC)) < 0 || ftruncate(fd, (off_t)get64(rest))) fail("truncate", at);
        close(fd);
        break;
    case OP_ALLOCATE:
        if ((fd = openat(directory, at, O_WRONLY | O_CLOEXEC)) < 0 || posix_fallocate(fd, (off_t)get64(rest), (off_t)get64(rest + 8)))
            fail("allocate", at);
        close(fd);
        break;
    case OP_RENAME: {
        uint16_t n = get16(rest);
        memcpy(other, rest + 2, n);
        other[n] = 0;
        if (renameat(directory, at, directory, other)) fail("rename", at);
        touch(other);
        break;
    }
    case OP_UNLINK:
        if (unlinkat(directory, at, (int)get32(rest))) fail("unlink", at);
        break;
    case OP_MKDIR:
        if (path[0] && mkdirat(directory, at, (mode_t)get32(rest)) && errno != EEXIST) fail("mkdir", at);
        break;
    case OP_SYNC: break;
    }
    if (p[0] == OP_CREATE || p[0] == OP_RENAME || p[0] == OP_UNLINK || p[0] == OP_MKDIR) named = true;
    if (p[0] != OP_UNLINK && p[0] != OP_RENAME) touch(path);
    ++applied_ops, applied_bytes += size;
}

static void *work(void *unused) {
    (void)unused;
    for (;;) {
        pthread_mutex_lock(&lock);
        while (!head) pthread_cond_wait(&waiting, &lock);
        Job *job = head;
        if (!(head = job->next)) tail = NULL;
        pthread_mutex_unlock(&lock);
        const unsigned char *p = job->records;
        for (unsigned i = 0; i < job->count; ++i) {
            uint32_t size = get32(p);
            const unsigned char *r = p + 4;
            uint64_t seq = job->first + i;
            p += 4 + size;
            if (seq <= heard) continue;   /* heard already */
            if (seq != heard + 1) break;  /* a gap: the link gives it again */
            for (size_t at = 9; at < size;) {
                size_t n = op_size(r + at, size - at);
                if (!n) { fprintf(stderr, "follower: a torn op in record %llu\n", (unsigned long long)seq); exit(1); }
                apply(r + at, n);
                at += n;
            }
            if (r[0]) sync_touched(), ++cuts; /* the cut's end: what it touched is durable before it is said saved */
            within_cut = !r[0];
            heard = seq;
        }
        pure_link_done(listener, (PureLinkDone){job->token, heard, true, false, false});
        free(job->records);
        free(job);
    }
    return NULL;
}

static int input(void *context, const char *name, uint32_t shape, const unsigned char *fields, size_t size) {
    (void)context, (void)shape, (void)fields, (void)size;
    return strcmp(name, "store") ? -1 : 0;
}

static bool deliver(void *context, const PureLinkDelivery *d) {
    (void)context;
    Job *job = malloc(sizeof(*job));
    unsigned char *copy = malloc(d->size ? d->size : 1);
    if (!job || !copy) return false;
    memcpy(copy, d->records, d->size);
    *job = (Job){NULL, d->token, d->first, d->count, copy, d->size};
    pthread_mutex_lock(&lock);
    if (tail) tail->next = job;
    else head = job;
    tail = job;
    pthread_cond_signal(&waiting);
    pthread_mutex_unlock(&lock);
    return true;
}

static void on_term(int signal) { (void)signal; stopping = 1; }

int main(int argc, char **argv) {
    char error[256], hex[2 * PURE_SEAL_KEY + 1], other_hex[2 * PURE_SEAL_KEY + 1];
    if (argc == 3 && !strcmp(argv[1], "keys")) {
        char a[PATH_MAX], b[PATH_MAX];
        snprintf(a, sizeof(a), "%s/owner.key", argv[2]), snprintf(b, sizeof(b), "%s/follower.key", argv[2]);
        PureSealKey *x = pure_seal_key(a, hex, error, sizeof(error)), *y = pure_seal_key(b, other_hex, error, sizeof(error));
        if (!x || !y) { fprintf(stderr, "follower: %s\n", error); return 1; }
        printf("{\"owner\":\"%s\",\"follower\":\"%s\"}\n", hex, other_hex);
        return 0;
    }
    if (argc != 5) { fprintf(stderr, "usage: %s DIR PORT KEY OWNER_PUBLIC_HEX | keys DIR\n", argv[0]); return 2; }
    as_data = getenv("FOLLOWER_SYNC") && !strcmp(getenv("FOLLOWER_SYNC"), "data");
    mkdir(argv[1], 0700);
    if ((directory = open(argv[1], O_RDONLY | O_DIRECTORY | O_CLOEXEC)) < 0) fail("open", argv[1]);
    unsigned char own_public[PURE_SEAL_KEY], owner[PURE_SEAL_KEY];
    PureSealKey *key = pure_seal_key(argv[3], hex, error, sizeof(error));
    if (!key || !pure_seal_peer(hex, own_public) || !pure_seal_peer(argv[4], owner)) { fprintf(stderr, "follower: %s\n", error); return 1; }
    PureLinkReceiver *receiver = pure_link_receiver(key, own_public, &(PureLinkHost){input, deliver, NULL}, error, sizeof(error));
    if (!receiver || !pure_link_allow(receiver, "owner", owner, error, sizeof(error)) ||
        !(listener = pure_link_listen(receiver, (unsigned)atoi(argv[2]), error, sizeof(error)))) {
        fprintf(stderr, "follower: %s\n", error);
        return 1;
    }
    pthread_t worker;
    pthread_create(&worker, NULL, work, NULL);
    if (!pure_link_serve(listener, error, sizeof(error))) { fprintf(stderr, "follower: %s\n", error); return 1; }
    signal(SIGTERM, on_term), signal(SIGINT, on_term);
    printf("{\"ready\":true}\n");
    fflush(stdout);
    while (!stopping) pause();
    pthread_mutex_lock(&lock); /* the worker between records: what it did is whole */
    PureLinkCounts counts;
    pure_link_listener_close(listener, &counts);
    if (chains) qsort(chain_ns, chains, sizeof(*chain_ns), by_value);
    printf("{\"within_cut\":%s,\"heard\":%llu,\"cuts\":%llu,\"ops\":%llu,\"op_bytes\":%llu,\"received\":%llu,\"deliveries\":%llu,\"stored\":%llu,"
           "\"file_syncs\":%llu,\"file_sync_ms\":%.1f,\"directory_syncs\":%llu,\"directory_sync_ms\":%.1f,\"chain_p50_us\":%.0f,"
           "\"chain_p90_us\":%.0f,\"chain_p99_us\":%.0f}\n",
           within_cut ? "true" : "false", (unsigned long long)heard, (unsigned long long)cuts, (unsigned long long)applied_ops,
           (unsigned long long)applied_bytes, (unsigned long long)counts.received, (unsigned long long)counts.deliveries,
           (unsigned long long)counts.stored, (unsigned long long)file_syncs, file_sync_ns / 1e6,
           (unsigned long long)directory_syncs, directory_sync_ns / 1e6, chains ? chain_ns[chains / 2] / 1e3 : 0.0,
           chains ? chain_ns[chains * 9 / 10] / 1e3 : 0.0, chains ? chain_ns[chains * 99 / 100] / 1e3 : 0.0);
    fflush(stdout);
    _exit(0);
}
