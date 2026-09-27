#define _GNU_SOURCE
/* Whether one device syncs several files at once: THREADS threads, each with
 * a file of its own, each commit SIZE bytes written over zeros written
 * earlier (no new size or blocks, so no journal commit) and fdatasync, for
 * SECONDS. Sharding apps by key gives each owner a store of its own, so this
 * is what separate owners would get from the device.
 * probe DIRECTORY THREADS SECONDS [SIZE] -> one JSON object. */
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum { MOST = 20000 };
typedef struct {
    int fd;
    size_t size;
    double until, times[MOST];
    int count;
} Syncer;

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

static int order(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

static void *commit_all(void *argument) {
    Syncer *s = argument;
    char *bytes = malloc(s->size);
    memset(bytes, 'c', s->size);
    off_t at = 0;
    while (s->count < MOST && now_ms() < s->until) {
        double start = now_ms();
        if (pwrite(s->fd, bytes, s->size, at) != (ssize_t)s->size || fdatasync(s->fd)) break;
        at += (off_t)s->size;
        s->times[s->count++] = now_ms() - start;
    }
    free(bytes);
    return NULL;
}

int main(int argc, char **argv) {
    if (argc < 4) return 2;
    const char *directory = argv[1];
    int threads = atoi(argv[2]);
    double seconds = atof(argv[3]);
    size_t size = argc > 4 ? (size_t)atoi(argv[4]) : 4096;
    if (threads < 1 || threads > 16) return 2;
    Syncer *syncers = calloc((size_t)threads, sizeof(*syncers));
    char paths[16][512];
    size_t zeros_size = (size_t)MOST * size;
    char *zeros = calloc(1, zeros_size);
    for (int i = 0; i < threads; ++i) {
        snprintf(paths[i], sizeof(paths[i]), "%s/pure-sync-probe-%d", directory, i);
        syncers[i].fd = open(paths[i], O_RDWR | O_CREAT | O_TRUNC, 0600);
        if (syncers[i].fd < 0 || pwrite(syncers[i].fd, zeros, zeros_size, 0) != (ssize_t)zeros_size) return 1;
        fsync(syncers[i].fd);
        syncers[i].size = size;
    }
    free(zeros);
    pthread_t running[16];
    double start = now_ms();
    for (int i = 0; i < threads; ++i) {
        syncers[i].until = start + seconds * 1e3;
        pthread_create(&running[i], NULL, commit_all, &syncers[i]);
    }
    int total = 0;
    for (int i = 0; i < threads; ++i) {
        pthread_join(running[i], NULL);
        total += syncers[i].count;
    }
    double elapsed = now_ms() - start;
    double *all = malloc((size_t)total * sizeof(*all));
    int n = 0;
    for (int i = 0; i < threads; ++i) {
        memcpy(all + n, syncers[i].times, (size_t)syncers[i].count * sizeof(double));
        n += syncers[i].count;
        close(syncers[i].fd);
        unlink(paths[i]);
    }
    qsort(all, (size_t)n, sizeof(double), order);
    printf("{\"threads\":%d,\"bytes\":%zu,\"commits\":%d,\"commits_per_second\":%.1f,\"p50_ms\":%.2f,\"p90_ms\":%.2f,"
           "\"p99_ms\":%.2f}\n", threads, size, n, n / (elapsed / 1e3), n ? all[n / 2] : 0, n ? all[n * 9 / 10] : 0,
           n ? all[n * 99 / 100] : 0);
    free(all);
    free(syncers);
    return 0;
}
