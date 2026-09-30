#ifndef PURE_WAIT_H
#define PURE_WAIT_H
/* Timed waits that count on the monotonic clock. A deadline read from the
 * wall clock moves when the clock is set (by a person, or NTP): set back an
 * hour, a wait begun before lasts an hour more; set forward, it ends at once.
 * A condition made by pure_cond_monotonic waits until a time pure_wait_until
 * gives, and neither happens. (macOS has no clock for conditions: there both
 * keep to the wall clock.) */
#include <pthread.h>
#include <time.h>

#ifdef __APPLE__
#define PURE_WAIT_CLOCK CLOCK_REALTIME
#else
#define PURE_WAIT_CLOCK CLOCK_MONOTONIC
#endif

static inline int pure_cond_monotonic(pthread_cond_t *cond) {
#ifdef __APPLE__
    return pthread_cond_init(cond, NULL);
#else
    pthread_condattr_t attr;
    int result = pthread_condattr_init(&attr);
    if (result) return result;
    result = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    if (!result) result = pthread_cond_init(cond, &attr);
    pthread_condattr_destroy(&attr);
    return result;
#endif
}

/* The time ns from now, on the clock pure_cond_monotonic's conditions wait on. */
static inline struct timespec pure_wait_until(long long ns) {
    struct timespec t;
    clock_gettime(PURE_WAIT_CLOCK, &t);
    t.tv_sec += (time_t)(ns / 1000000000);
    t.tv_nsec += (long)(ns % 1000000000);
    if (t.tv_nsec >= 1000000000) {
        ++t.tv_sec;
        t.tv_nsec -= 1000000000;
    }
    return t;
}
#endif
