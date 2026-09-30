#define _GNU_SOURCE
/* LD_PRELOAD: the wall clock (CLOCK_REALTIME) set back an hour (moved
 * CLOCK_SHIFT_S seconds: -3600 unless given) from CLOCK_BACK_MS milliseconds
 * (200 unless given) after the process first asks for it, as a person or NTP
 * may set it. Only what the process reads moves, so a wait until a deadline
 * read from the wall clock ends at once (set back) or lasts an hour (set
 * forward), as one would with the clock set the other way after it was read.
 * A program's timers and waits must go on all the same: they count on the
 * monotonic clock (tests/check.py). */
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
    const char *after = getenv("CLOCK_BACK_MS"), *shift = getenv("CLOCK_SHIFT_S");
    if (ms - began >= (after ? atoll(after) : 200)) t->tv_sec += shift ? atoll(shift) : -3600;
    return result;
}
