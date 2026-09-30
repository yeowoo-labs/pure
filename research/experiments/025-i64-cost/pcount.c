/* pcount PID: count user-mode instructions and cycles of every thread of
 * PID from "ready" (on stderr) until stdin closes; prints them as JSON.
 * perf_event_open alone: a perf_event_paranoid of 2 allows it for one's own
 * processes, and no perf tool is needed. */
#define _GNU_SOURCE
#include <dirent.h>
#include <linux/perf_event.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>

static int open_counter(pid_t tid, uint64_t config) {
    struct perf_event_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.size = sizeof(attr);
    attr.type = PERF_TYPE_HARDWARE;
    attr.config = config;
    attr.disabled = 1;
    attr.exclude_kernel = 1;
    attr.exclude_hv = 1;
    return (int)syscall(SYS_perf_event_open, &attr, tid, -1, -1, 0);
}

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    char path[64];
    snprintf(path, sizeof(path), "/proc/%s/task", argv[1]);
    DIR *dir = opendir(path);
    if (!dir) { perror("opendir"); return 1; }
    int fds[512][2], n = 0;
    struct dirent *e;
    while ((e = readdir(dir)) && n < 512) {
        if (e->d_name[0] == '.') continue;
        pid_t tid = atoi(e->d_name);
        fds[n][0] = open_counter(tid, PERF_COUNT_HW_INSTRUCTIONS);
        fds[n][1] = open_counter(tid, PERF_COUNT_HW_CPU_CYCLES);
        if (fds[n][0] < 0 || fds[n][1] < 0) { perror("perf_event_open"); return 1; }
        ++n;
    }
    closedir(dir);
    for (int i = 0; i < n; ++i) for (int k = 0; k < 2; ++k) ioctl(fds[i][k], PERF_EVENT_IOC_RESET, 0), ioctl(fds[i][k], PERF_EVENT_IOC_ENABLE, 0);
    fprintf(stderr, "ready\n");
    char buffer[64];
    while (fread(buffer, 1, sizeof(buffer), stdin) > 0) {}
    uint64_t total[2] = {0, 0};
    for (int i = 0; i < n; ++i)
        for (int k = 0; k < 2; ++k) {
            uint64_t v = 0;
            ioctl(fds[i][k], PERF_EVENT_IOC_DISABLE, 0);
            if (read(fds[i][k], &v, sizeof(v)) == sizeof(v)) total[k] += v;
        }
    printf("{\"threads\": %d, \"instructions\": %llu, \"cycles\": %llu}\n", n, (unsigned long long)total[0], (unsigned long long)total[1]);
    return 0;
}
