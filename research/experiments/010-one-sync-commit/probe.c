#define _GNU_SOURCE
/* What each way of making a small commit durable costs on one device: an
 * 8 KiB commit plus a 64-byte head, 40 times per way. The ways run
 * round-robin so device drift reaches all of them alike.
 * probe DIRECTORY -> one JSON object per way (milliseconds). */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}
static int order(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}
enum { WAYS = 5, ROUNDS = 40, REGION = 8192 };
static const char *names[WAYS] = {
    "append, fsync, head, fsync (two syncs)",
    "append, head, fdatasync",
    "append, head, fsync",
    "overwrite written zeros, head, fdatasync",
    "overwrite written zeros, head, fsync",
};
int main(int argc, char **argv) {
    const char *directory = argc > 1 ? argv[1] : "/var/tmp";
    char path[WAYS][512];
    int fd[WAYS];
    off_t end[WAYS];
    static char region[REGION], head[64];
    memset(region, 'r', sizeof(region));
    for (int w = 0; w < WAYS; ++w) {
        snprintf(path[w], sizeof(path[w]), "%s/pure-sync-probe-%d", directory, w);
        fd[w] = open(path[w], O_RDWR | O_CREAT | O_TRUNC, 0600);
        if (fd[w] < 0) return 1;
        end[w] = 4096;
        if (w >= 3) {
            size_t bytes = (size_t)(ROUNDS + 2) * REGION + 4096;
            char *zeros = calloc(1, bytes);
            if (!zeros || pwrite(fd[w], zeros, bytes, 0) != (ssize_t)bytes) return 1;
            free(zeros);
        } else if (pwrite(fd[w], region, 4096, 0) != 4096) return 1;
        fsync(fd[w]);
    }
    double t[WAYS][ROUNDS];
    for (int r = 0; r < ROUNDS; ++r)
        for (int w = 0; w < WAYS; ++w) {
            head[0] = (char)r;
            double start = now_ms();
            if (pwrite(fd[w], region, REGION, end[w]) != REGION) return 1;
            end[w] += REGION;
            if (w == 0) fsync(fd[w]);
            if (pwrite(fd[w], head, 64, (r % 2) * 64) != 64) return 1;
            if (w == 1 || w == 3) fdatasync(fd[w]); else fsync(fd[w]);
            t[w][r] = now_ms() - start;
        }
    for (int w = 0; w < WAYS; ++w) {
        qsort(t[w], ROUNDS, sizeof(double), order);
        printf("{\"way\":\"%s\",\"p50_ms\":%.2f,\"p90_ms\":%.2f,\"max_ms\":%.2f}\n", names[w], t[w][ROUNDS / 2],
               t[w][ROUNDS * 9 / 10], t[w][ROUNDS - 1]);
        close(fd[w]);
        unlink(path[w]);
    }
    return 0;
}
