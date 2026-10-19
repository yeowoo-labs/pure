/* Research 045: an operation's cost in machine instructions and cycles, from small apps (the apps in units): each takes
 * message go { n: u32 } and does its operation n times; MESSAGES messages, counted (perf_event_open, user mode).
 *
 *   cc -O2 -std=c17 -I../../../core unit.c ../../../build/libpure.a -lm -pthread -o unit
 *   ./unit MODULE N MESSAGES     one JSON line: instructions and cycles an operation */
#define _GNU_SOURCE
#include "module.h"
#include <linux/perf_event.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>

static int counter(uint64_t config) {
    struct perf_event_attr a;
    memset(&a, 0, sizeof(a));
    a.size = sizeof(a), a.type = PERF_TYPE_HARDWARE, a.config = config, a.disabled = 1, a.exclude_kernel = 1, a.exclude_hv = 1;
    return (int)syscall(SYS_perf_event_open, &a, 0, -1, -1, 0);
}

int main(int argc, char **argv) {
    if (argc != 4) { fprintf(stderr, "usage: %s MODULE N MESSAGES\n", argv[0]); return 2; }
    uint32_t n = (uint32_t)atoi(argv[2]);
    int messages = atoi(argv[3]);
    PureModule module = {0};
    char error[160];
    if (pure_module_read(argv[1], &module, error, sizeof(error)) != PURE_OK) { fprintf(stderr, "%s\n", error); return 1; }
    PureApp app;
    if (pure_app_start(&app, &module, (PureData){0}) != PURE_OK) return 1;
    int go = pure_module_input(&module, "go");
    unsigned char payload[4];
    memcpy(payload, &n, 4);
    if (pure_app_message(&app, (uint32_t)go, (PureSpan){payload, 4}) != PURE_OK) { fprintf(stderr, "failed\n"); return 1; }
    int ins = counter(PERF_COUNT_HW_INSTRUCTIONS), cyc = counter(PERF_COUNT_HW_CPU_CYCLES);
    ioctl(ins, PERF_EVENT_IOC_ENABLE, 0), ioctl(cyc, PERF_EVENT_IOC_ENABLE, 0);
    for (int k = 0; k < messages; ++k)
        if (pure_app_message(&app, (uint32_t)go, (PureSpan){payload, 4}) != PURE_OK) return 1;
    ioctl(ins, PERF_EVENT_IOC_DISABLE, 0), ioctl(cyc, PERF_EVENT_IOC_DISABLE, 0);
    uint64_t i = 0, c = 0;
    if (read(ins, &i, 8) != 8 || read(cyc, &c, 8) != 8) return 1;
    double ops = (double)n * messages;
    printf("{\"n\":%u,\"messages\":%d,\"instructions_per_op\":%.1f,\"cycles_per_op\":%.1f}\n", n, messages, i / ops, c / ops);
    pure_app_stop(&app);
    return 0;
}
