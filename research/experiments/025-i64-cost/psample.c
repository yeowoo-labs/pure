/* psample PID PERIOD: sample the user-mode instruction pointer of every
 * thread of PID once per PERIOD instructions, from "ready" (on stderr) until
 * stdin closes; prints one hex address per sample, then "lost N". Like
 * pcount, perf_event_open alone (the threads PID has when it starts). */
#define _GNU_SOURCE
#include <dirent.h>
#include <linux/perf_event.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

enum { PAGES = 64, MAX_THREADS = 256 };
static size_t page;
static uint64_t lost;

typedef struct { int fd; struct perf_event_mmap_page *meta; unsigned char *data; } Ring;

static void drain(Ring *r, FILE *out) {
    uint64_t head = __atomic_load_n(&r->meta->data_head, __ATOMIC_ACQUIRE), tail = r->meta->data_tail;
    size_t size = PAGES * page;
    while (tail < head) {
        struct perf_event_header h;
        unsigned char record[256];
        for (size_t i = 0; i < sizeof(h); ++i) ((unsigned char *)&h)[i] = r->data[(tail + i) % size];
        if (h.size > sizeof(record) || h.size < sizeof(h)) break;
        for (size_t i = 0; i < h.size; ++i) record[i] = r->data[(tail + i) % size];
        if (h.type == PERF_RECORD_SAMPLE) {
            uint64_t ip;
            memcpy(&ip, record + sizeof(h), sizeof(ip));
            fprintf(out, "%llx\n", (unsigned long long)ip);
        } else if (h.type == PERF_RECORD_LOST) {
            uint64_t n;
            memcpy(&n, record + sizeof(h) + 8, sizeof(n));
            lost += n;
        }
        tail += h.size;
    }
    __atomic_store_n(&r->meta->data_tail, tail, __ATOMIC_RELEASE);
}

int main(int argc, char **argv) {
    if (argc != 3) return 2;
    page = (size_t)sysconf(_SC_PAGESIZE);
    char path[64];
    snprintf(path, sizeof(path), "/proc/%s/task", argv[1]);
    DIR *dir = opendir(path);
    if (!dir) { perror("opendir"); return 1; }
    Ring rings[MAX_THREADS];
    int n = 0;
    struct dirent *e;
    while ((e = readdir(dir)) && n < MAX_THREADS) {
        if (e->d_name[0] == '.') continue;
        struct perf_event_attr attr;
        memset(&attr, 0, sizeof(attr));
        attr.size = sizeof(attr);
        attr.type = PERF_TYPE_HARDWARE;
        attr.config = PERF_COUNT_HW_INSTRUCTIONS;
        attr.sample_period = strtoull(argv[2], NULL, 10);
        attr.sample_type = PERF_SAMPLE_IP;
        attr.disabled = 1;
        attr.exclude_kernel = 1;
        attr.exclude_hv = 1;
        attr.wakeup_events = 1;
        int fd = (int)syscall(SYS_perf_event_open, &attr, atoi(e->d_name), -1, -1, 0);
        if (fd < 0) { perror("perf_event_open"); return 1; }
        void *map = mmap(NULL, (PAGES + 1) * page, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (map == MAP_FAILED) { perror("mmap"); return 1; }
        rings[n++] = (Ring){fd, map, (unsigned char *)map + page};
    }
    closedir(dir);
    FILE *out = stdout;
    for (int i = 0; i < n; ++i) ioctl(rings[i].fd, PERF_EVENT_IOC_ENABLE, 0);
    fprintf(stderr, "ready\n");
    struct pollfd fds[MAX_THREADS + 1];
    for (;;) {
        fds[0] = (struct pollfd){.fd = 0, .events = POLLIN};
        for (int i = 0; i < n; ++i) fds[i + 1] = (struct pollfd){.fd = rings[i].fd, .events = POLLIN};
        poll(fds, (nfds_t)n + 1, 100);
        for (int i = 0; i < n; ++i) drain(&rings[i], out);
        if (fds[0].revents) {
            char buffer[64];
            if (read(0, buffer, sizeof(buffer)) <= 0) break;
        }
    }
    for (int i = 0; i < n; ++i) ioctl(rings[i].fd, PERF_EVENT_IOC_DISABLE, 0), drain(&rings[i], out);
    fprintf(out, "lost %llu\n", (unsigned long long)lost);
    return 0;
}
