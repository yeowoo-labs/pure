#ifndef PURE_SENDING_H
#define PURE_SENDING_H
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The process's sending loop (docs/design/017-apps-on-one-host.md, 2b): one
 * thread, pure-send, sends for every link and wire, an epoll over their
 * sockets. A sender joins it, and has rounds on that thread: when its socket
 * is ready, when it was pushed (from any thread: there is more to send), and
 * when the time it last wanted comes. The thread is made with the first
 * sender joined and stopped with the last that leaves. A sender is a
 * PureSending in its struct, with its round. */
typedef struct PureSending PureSending;
struct PureSending {
    /* Its round, with what its socket was ready for (EPOLLIN, EPOLLOUT,
     * EPOLLERR, EPOLLHUP; 0: pushed, or its time came): when it wants the
     * next (CLOCK_MONOTONIC ns; 0: when pushed or its socket is ready). */
    uint64_t (*round)(PureSending *, uint32_t ready);
    /* The loop's own. */
    PureSending *next;
    int fd;          /* the socket it waits on, or -1 */
    uint32_t events; /* for what */
    bool joined, watched;
    uint32_t ready;
    uint64_t due;
    _Atomic bool pushed;
};

/* Before it joins: no socket. */
void pure_sending_init(PureSending *, uint64_t (*round)(PureSending *, uint32_t ready));
/* Its first round comes at once. False with why in error when the loop
 * cannot be made. */
bool pure_sending_join(PureSending *, char *error, size_t size);
/* Out of the loop: no round of it runs once this returns (one under way is
 * waited for). Not from a round. */
void pure_sending_leave(PureSending *);
/* A round for it soon (any thread); the loop woken when it sleeps. */
void pure_sending_push(PureSending *);
/* The socket it waits on and for what (EPOLLIN, EPOLLOUT; fd -1: none), in
 * its round or before it joins; before the socket it waited on is closed. */
void pure_sending_watch(PureSending *, int fd, uint32_t events);
/* Now on CLOCK_MONOTONIC, ns. */
uint64_t pure_sending_now(void);
#endif
