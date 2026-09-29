#define _GNU_SOURCE
/* LD_PRELOAD: the wall clock (CLOCK_REALTIME) set back an hour from
 * CLOCK_BACK_MS milliseconds (200 unless given) after the process first
 * asks for it, as a person or NTP may set it. A program's timers must go on
 * all the same: they wait on the monotonic clock (tests/check.py). */
#include <dlfcn.h>
#include <stdlib.h>
#include <time.h>

int clock_gettime(clockid_t id, struct timespec *t) {
    static int (*real)(clockid_t, struct timespec *);
    static long long began = -1;
    if (!real) *(void **)&real = dlsym(RTLD_NEXT, "clock_gettime"); /* as POSIX's dlsym example does */
    int result = real(id, t);
    if (result || id != CLOCK_REALTIME) return result;
    struct timespec now;
    real(CLOCK_MONOTONIC, &now);
    long long ms = (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000;
    if (began < 0) began = ms;
    const char *after = getenv("CLOCK_BACK_MS");
    if (ms - began >= (after ? atoll(after) : 200)) t->tv_sec -= 3600;
    return result;
}
