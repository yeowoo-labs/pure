/* icount CMD...: user-mode instructions and cycles of CMD and its children (perf_event_open, inherited). */
#define _GNU_SOURCE
#include <linux/perf_event.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
static int counter(uint64_t config) {
    struct perf_event_attr a;
    memset(&a, 0, sizeof(a));
    a.size = sizeof(a), a.type = PERF_TYPE_HARDWARE, a.config = config, a.disabled = 1, a.exclude_kernel = 1, a.exclude_hv = 1, a.inherit = 1;
    return (int)syscall(SYS_perf_event_open, &a, 0, -1, -1, 0);
}
int main(int argc, char **argv) {
    (void)argc;
    int ins = counter(PERF_COUNT_HW_INSTRUCTIONS), cyc = counter(PERF_COUNT_HW_CPU_CYCLES);
    ioctl(ins, PERF_EVENT_IOC_ENABLE, 0), ioctl(cyc, PERF_EVENT_IOC_ENABLE, 0);
    pid_t p = fork();
    if (!p) { execvp(argv[1], argv + 1); _exit(127); }
    int st; waitpid(p, &st, 0);
    uint64_t i = 0, c = 0;
    read(ins, &i, 8), read(cyc, &c, 8);
    fprintf(stderr, "{\"instructions\":%llu,\"cycles\":%llu}\n", (unsigned long long)i, (unsigned long long)c);
}
