/* osample TID PERIOD: sample one thread's user-mode instruction pointer once
 * per PERIOD instructions until stdin closes; prints one hex address per
 * sample, then "instructions N" (the thread's user instructions meanwhile)
 * and "lost N". perf_event_open alone (paranoid 2: own processes, user mode). */
#define _GNU_SOURCE
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

enum { PAGES = 256 };
static size_t page;
static uint64_t lost;
static struct perf_event_mmap_page *meta;
static unsigned char *data;

static void drain(FILE *out) {
    uint64_t head = __atomic_load_n(&meta->data_head, __ATOMIC_ACQUIRE), tail = meta->data_tail;
    size_t size = PAGES * page;
    while (tail < head) {
        struct perf_event_header h;
        unsigned char record[256];
        for (size_t i = 0; i < sizeof(h); ++i) ((unsigned char *)&h)[i] = data[(tail + i) % size];
        if (h.size > sizeof(record) || h.size < sizeof(h)) break;
        for (size_t i = 0; i < h.size; ++i) record[i] = data[(tail + i) % size];
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
    __atomic_store_n(&meta->data_tail, tail, __ATOMIC_RELEASE);
}

int main(int argc, char **argv) {
    if (argc != 3) return 2;
    page = (size_t)sysconf(_SC_PAGESIZE);
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
    attr.wakeup_events = 64;
    int fd = (int)syscall(SYS_perf_event_open, &attr, atoi(argv[1]), -1, -1, 0);
    if (fd < 0) { perror("perf_event_open"); return 1; }
    void *map = mmap(NULL, (PAGES + 1) * page, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) { perror("mmap"); return 1; }
    meta = map;
    data = (unsigned char *)map + page;
    FILE *out = stdout;
    ioctl(fd, PERF_EVENT_IOC_RESET, 0);
    ioctl(fd, PERF_EVENT_IOC_ENABLE, 0);
    fprintf(stderr, "ready\n");
    for (;;) {
        struct pollfd fds[2] = {{.fd = 0, .events = POLLIN}, {.fd = fd, .events = POLLIN}};
        poll(fds, 2, 100);
        drain(out);
        if (fds[0].revents) {
            char buffer[64];
            if (read(0, buffer, sizeof(buffer)) <= 0) break;
        }
    }
    ioctl(fd, PERF_EVENT_IOC_DISABLE, 0);
    drain(out);
    uint64_t count = 0;
    if (read(fd, &count, sizeof(count)) != sizeof(count)) count = 0;
    fprintf(out, "instructions %llu\nlost %llu\n", (unsigned long long)count, (unsigned long long)lost);
    return 0;
}
