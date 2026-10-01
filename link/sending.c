#define _GNU_SOURCE
#include "sending.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <time.h>
#include <unistd.h>

#define MS UINT64_C(1000000)

uint64_t pure_sending_now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

/* The loop: its senders under the lock, which it holds over its rounds. A
 * push wakes it only when it sleeps, once: pushed tells it a sender was
 * (it then does not sleep), sleeping that it is about to, signaled that it
 * was woken. A sender that left while it waited may be in the events it
 * got: those are let go (left counts the leaves), and come again. */
static struct {
    pthread_mutex_t lock;
    PureSending *first;
    int epoll, wake;
    pthread_t thread;
    bool running, stopping;
    uint64_t left;
    _Atomic bool pushed, sleeping, signaled;
} loop = {.lock = PTHREAD_MUTEX_INITIALIZER, .epoll = -1, .wake = -1};

static void wake(void) {
    uint64_t one = 1;
    if (loop.wake >= 0 && write(loop.wake, &one, sizeof(one)) != sizeof(one)) { /* full: it is readable already */ }
}

void pure_sending_init(PureSending *s, uint64_t (*round)(PureSending *, uint32_t ready)) {
    memset(s, 0, sizeof(*s));
    s->round = round;
    s->fd = -1;
}

void pure_sending_push(PureSending *s) {
    atomic_store(&s->pushed, true);
    atomic_store(&loop.pushed, true);
    if (atomic_load(&loop.sleeping) && !atomic_exchange(&loop.signaled, true)) wake();
}

static void watch_now(PureSending *s, int fd, uint32_t events) {
    if (s->watched && (fd != s->fd || !events)) {
        epoll_ctl(loop.epoll, EPOLL_CTL_DEL, s->fd, NULL);
        s->watched = false;
    }
    if (fd >= 0 && events) {
        struct epoll_event event = {.events = events, .data.ptr = s};
        s->watched = !epoll_ctl(loop.epoll, s->watched ? EPOLL_CTL_MOD : EPOLL_CTL_ADD, fd, &event);
    }
}

void pure_sending_watch(PureSending *s, int fd, uint32_t events) {
    if (s->joined && (fd != s->fd || events != s->events || (fd >= 0 && events && !s->watched))) watch_now(s, fd, events);
    s->fd = fd;
    s->events = events;
}

static void *run(void *context) {
    (void)context;
    struct epoll_event events[64];
    int timeout = 0;
    pthread_mutex_lock(&loop.lock);
    uint64_t left = loop.left;
    pthread_mutex_unlock(&loop.lock);
    for (;;) {
        atomic_store(&loop.sleeping, true);
        if (atomic_load(&loop.pushed)) timeout = 0; /* pushed since: no sleep */
        int ready = epoll_wait(loop.epoll, events, 64, timeout);
        atomic_store(&loop.sleeping, false);
        atomic_store(&loop.signaled, false);
        atomic_store(&loop.pushed, false);
        pthread_mutex_lock(&loop.lock);
        if (loop.stopping) {
            pthread_mutex_unlock(&loop.lock);
            break;
        }
        bool again = ready > 0 && left != loop.left; /* one may be gone: its events let go, the others' come again */
        for (int e = 0; e < ready; ++e) {
            PureSending *s = events[e].data.ptr;
            if (!s) {
                uint64_t signals;
                if (read(loop.wake, &signals, sizeof(signals)) < 0) { /* cleared already */ }
            } else if (!again) s->ready |= events[e].events;
        }
        uint64_t soonest = 0, now = pure_sending_now();
        for (PureSending *s = loop.first; s; s = s->next) {
            bool pushed = atomic_exchange(&s->pushed, false); /* pushed after: the next round's */
            if (s->ready || pushed || (s->due && s->due <= now)) {
                uint32_t was = s->ready;
                s->ready = 0;
                s->due = s->round(s, was);
            }
            if (s->due && (!soonest || s->due < soonest)) soonest = s->due;
        }
        left = loop.left;
        pthread_mutex_unlock(&loop.lock);
        timeout = again ? 0 : !soonest ? -1 : soonest <= now ? 0 : (int)((soonest - now) / MS) + 1;
    }
    return NULL;
}

bool pure_sending_join(PureSending *s, char *error, size_t size) {
    pthread_mutex_lock(&loop.lock);
    bool ok = loop.running;
    if (!ok) { /* the first: the loop */
        loop.stopping = false;
        struct epoll_event event = {.events = EPOLLIN, .data.ptr = NULL};
        ok = (loop.epoll >= 0 || (loop.epoll = epoll_create1(EPOLL_CLOEXEC)) >= 0) &&
             (loop.wake >= 0 || ((loop.wake = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)) >= 0 &&
                                 !epoll_ctl(loop.epoll, EPOLL_CTL_ADD, loop.wake, &event))) &&
             !pthread_create(&loop.thread, NULL, run, NULL);
        if (ok) pthread_setname_np(loop.thread, "pure-send"); /* the sends' CPU, seen apart */
        loop.running = ok;
    }
    if (ok) {
        s->joined = true;
        if (s->fd >= 0 && s->events) watch_now(s, s->fd, s->events);
        s->next = loop.first;
        loop.first = s;
        atomic_store(&s->pushed, true); /* a first round */
        atomic_store(&loop.pushed, true);
    }
    pthread_mutex_unlock(&loop.lock);
    if (!ok) {
        snprintf(error, size, "the sending loop cannot be made");
        return false;
    }
    wake();
    return true;
}

void pure_sending_leave(PureSending *s) {
    if (!s->joined) return;
    pthread_mutex_lock(&loop.lock);
    for (PureSending **at = &loop.first; *at; at = &(*at)->next)
        if (*at == s) {
            *at = s->next;
            break;
        }
    if (s->watched) epoll_ctl(loop.epoll, EPOLL_CTL_DEL, s->fd, NULL);
    s->watched = s->joined = false;
    ++loop.left;
    bool last = !loop.first && loop.running;
    if (last) loop.stopping = true, loop.running = false;
    pthread_mutex_unlock(&loop.lock);
    if (last) { /* the last: the loop stops */
        wake();
        pthread_join(loop.thread, NULL);
    }
}
