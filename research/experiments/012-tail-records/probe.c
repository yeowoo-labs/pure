#define _GNU_SOURCE
/* What a small commit's sync costs by where its head goes, on one device:
 * a commit of SIZE bytes written over zeros written earlier (no new size or
 * blocks, so no journal commit), 40 times per way. The ways run round-robin
 * so device drift reaches all of them alike.
 * probe DIRECTORY [SIZE] -> one JSON object per way (milliseconds). */
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
enum { WAYS = 3, ROUNDS = 40, HEAD = 128, DATA = 8192 };
static const char *names[WAYS] = {
    "bytes, head in a slot at the start, fdatasync",
    "head just before the bytes, fdatasync",
    "bytes only, fdatasync",
};
int main(int argc, char **argv) {
    const char *directory = argc > 1 ? argv[1] : "/var/tmp";
    size_t size = argc > 2 ? (size_t)atoi(argv[2]) : 8192;
    char path[WAYS][512];
    int fd[WAYS];
    off_t end[WAYS];
    char *bytes = malloc(size), head[HEAD];
    if (!bytes) return 1;
    memset(bytes, 'r', size);
    for (int w = 0; w < WAYS; ++w) {
        snprintf(path[w], sizeof(path[w]), "%s/pure-record-probe-%d", directory, w);
        fd[w] = open(path[w], O_RDWR | O_CREAT | O_TRUNC, 0600);
        if (fd[w] < 0) return 1;
        size_t zeros_size = DATA + (size_t)(ROUNDS + 2) * (size + HEAD);
        char *zeros = calloc(1, zeros_size);
        if (!zeros || pwrite(fd[w], zeros, zeros_size, 0) != (ssize_t)zeros_size) return 1;
        free(zeros);
        fsync(fd[w]);
        end[w] = DATA;
    }
    double t[WAYS][ROUNDS];
    for (int r = 0; r < ROUNDS; ++r)
        for (int w = 0; w < WAYS; ++w) {
            memset(head, 'h' + r % 2, sizeof(head));
            double start = now_ms();
            if (w == 1 && pwrite(fd[w], head, HEAD, end[w]) != HEAD) return 1;
            off_t at = end[w] + (w == 1 ? HEAD : 0);
            if (pwrite(fd[w], bytes, size, at) != (ssize_t)size) return 1;
            end[w] = at + (off_t)size;
            if (w == 0 && pwrite(fd[w], head, HEAD, (r % 2) * 4096) != HEAD) return 1;
            fdatasync(fd[w]);
            t[w][r] = now_ms() - start;
        }
    for (int w = 0; w < WAYS; ++w) {
        qsort(t[w], ROUNDS, sizeof(double), order);
        printf("{\"way\":\"%s\",\"bytes\":%zu,\"p50_ms\":%.2f,\"p90_ms\":%.2f,\"max_ms\":%.2f}\n", names[w], size,
               t[w][ROUNDS / 2], t[w][ROUNDS * 9 / 10], t[w][ROUNDS - 1]);
        close(fd[w]);
        unlink(path[w]);
    }
    free(bytes);
    return 0;
}
