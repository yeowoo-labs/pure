#define _GNU_SOURCE
/* Links over UDP (link/link.h): link/link-protocol.h's two sides with
 * sockets and threads. */
#include "link.h"
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/udp.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

enum { DATAGRAM = PURE_LINK_DATAGRAM, DATAGRAM_MOST = PURE_LINK_DATAGRAM_MOST };
/* Packets sent at once with UDP GSO: each but the last is a whole datagram
 * (a full one padded), as many as fit a UDP datagram's 65,507 bytes (54 of
 * 1,200 bytes). */
enum { BURST = 54, BURST_BYTES = 65507 };
#ifndef UDP_SEGMENT
#define UDP_SEGMENT 103
#endif
#ifndef UDP_GRO
#define UDP_GRO 104
#endif
#define MS UINT64_C(1000000)

static uint64_t now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

/* Datagrams taken from a socket at once (recvmmsg). */
enum { TAKE = 64, SLOT = DATAGRAM_MOST + 64 };
typedef struct {
    unsigned char bytes[TAKE][SLOT];
    struct sockaddr_storage from[TAKE];
    struct iovec parts[TAKE];
    struct mmsghdr messages[TAKE];
} Taking;

/* With UDP GRO the kernel hands a listener the datagrams of one sender that
 * came one after another as one buffer, each the segment's size but the
 * last (a burst sent with GSO comes whole, not cut up): up to GATHER such
 * buffers at once. */
enum { GATHER = 8, GATHERED = 65536 };
typedef struct {
    unsigned char bytes[GATHER][GATHERED];
    struct sockaddr_storage from[GATHER];
    struct iovec parts[GATHER];
    struct mmsghdr messages[GATHER];
    _Alignas(8) unsigned char control[GATHER][128]; /* a cmsghdr's alignment */
} Gathering;

/* A packet's path as a receiver keeps it (PURE_LINK_ADDRESS bytes): where
 * it came from, and the address of the node's it came to. Answers go back
 * from that address: a node may have many, and a sender's socket takes
 * answers only from the one it sent to. */
typedef struct {
    union { struct sockaddr_in6 six; struct sockaddr_in four; } from;
    union { struct in6_pktinfo six; struct in_pktinfo four; } to;
} Path;
_Static_assert(sizeof(Path) <= PURE_LINK_ADDRESS, "a path fits what a receiver keeps");

static int gather(int fd, Gathering *g) {
    for (unsigned k = 0; k < GATHER; ++k) {
        g->parts[k] = (struct iovec){g->bytes[k], GATHERED};
        g->messages[k] = (struct mmsghdr){.msg_hdr = {.msg_name = &g->from[k], .msg_namelen = sizeof(g->from[k]),
                                                      .msg_iov = &g->parts[k], .msg_iovlen = 1,
                                                      .msg_control = g->control[k],
                                                      .msg_controllen = sizeof(g->control[k])}};
    }
    int n = recvmmsg(fd, g->messages, GATHER, MSG_DONTWAIT, NULL);
    return n > 0 ? n : 0;
}

/* The size of each datagram in a buffer gathered (all of it when one), and
 * the path they came by. */
static size_t segment_of(struct msghdr *m, size_t length, Path *path) {
    size_t step = length ? length : 1;
    memset(path, 0, sizeof(*path));
    memcpy(&path->from, m->msg_name, m->msg_namelen < sizeof(path->from) ? m->msg_namelen : sizeof(path->from));
    for (struct cmsghdr *c = CMSG_FIRSTHDR(m); c; c = CMSG_NXTHDR(m, c)) {
        int size;
        if (c->cmsg_level == SOL_UDP && c->cmsg_type == UDP_GRO && (memcpy(&size, CMSG_DATA(c), sizeof(size)), size > 0))
            step = (size_t)size;
        else if (c->cmsg_level == IPPROTO_IPV6 && c->cmsg_type == IPV6_PKTINFO)
            memcpy(&path->to.six, CMSG_DATA(c), sizeof(path->to.six));
        else if (c->cmsg_level == IPPROTO_IP && c->cmsg_type == IP_PKTINFO)
            memcpy(&path->to.four, CMSG_DATA(c), sizeof(path->to.four));
    }
    return step;
}

/* Up to TAKE datagrams from fd: how many (0 when none waits). */
static int take_datagrams(int fd, Taking *t) {
    for (unsigned k = 0; k < TAKE; ++k) {
        t->parts[k] = (struct iovec){t->bytes[k], SLOT};
        t->messages[k] = (struct mmsghdr){.msg_hdr = {.msg_name = &t->from[k], .msg_namelen = sizeof(t->from[k]),
                                                      .msg_iov = &t->parts[k], .msg_iovlen = 1}};
    }
    int n = recvmmsg(fd, t->messages, TAKE, MSG_DONTWAIT, NULL);
    return n > 0 ? n : 0;
}

/* ---- The sending side */

struct PureLink {
    PureLinkSender *sender; /* under the lock */
    char host[256], port[8], input[PURE_LINK_NAME + 1];
    bool peered, keyed; /* given the receiver's key; the keys made from it */
    pthread_mutex_t lock;
    bool stopping, started;
    /* The thread sleeps (in poll), and has been woken since; records were
     * queued since it last sent. A queue wakes it only when it sleeps, once. */
    bool sleeping, signaled, queued_new;
    uint64_t told; /* the last stored the owner was told of */
    int fd, wake;
    pthread_t thread;
    void (*notify)(void *);
    void *context;
    /* The thread's own. */
    Taking *taking;       /* answers, received at once */
    unsigned char *burst; /* packets sealed and not yet sent (BURST datagrams) */
    size_t burst_size;
    unsigned burst_count;
    uint64_t burst_pn[BURST];
    size_t segment; /* the datagram's size the bursts are cut at */
    bool gso, no_gso;
};

PureLink *pure_link_open(const char *url, const char *source, uint32_t shape, const PureSealKey *own,
                         const unsigned char own_public[PURE_SEAL_KEY], char *error, size_t size) {
    if (!own) {
        snprintf(error, size, "a pure:// link needs the node's --key");
        return NULL;
    }
    size_t length = strlen(source);
    const char *host = url + 7, *slash = strchr(host, '/'), *colon = slash ? memchr(host, ':', (size_t)(slash - host)) : NULL;
    if (!length || length > PURE_LINK_NAME || strncmp(url, "pure://", 7) || !slash || !colon || colon == host ||
        (size_t)(colon - host) >= 256 || slash - colon - 1 < 1 || slash - colon - 1 > 5 || !slash[1] ||
        strlen(slash + 1) > PURE_LINK_NAME) {
        snprintf(error, size, "a link is pure://host:port/INPUT");
        return NULL;
    }
    PureLink *l = calloc(1, sizeof(*l));
    if (!l || pthread_mutex_init(&l->lock, NULL)) {
        free(l);
        snprintf(error, size, "out of memory");
        return NULL;
    }
    memcpy(l->host, host, (size_t)(colon - host));
    memcpy(l->port, colon + 1, (size_t)(slash - colon - 1));
    strcpy(l->input, slash + 1);
    l->fd = l->wake = -1;
    if (!(l->sender = pure_link_sender(source, l->input, shape, own, own_public))) {
        pthread_mutex_destroy(&l->lock);
        free(l);
        snprintf(error, size, "out of memory");
        return NULL;
    }
    return l;
}

void pure_link_gso(PureLink *l, bool on) { l->no_gso = !on; }

void pure_link_peer(PureLink *l, const unsigned char peer[PURE_SEAL_KEY]) {
    l->peered = true;
    l->keyed = pure_link_sender_peer(l->sender, peer);
}

static void wake(int fd) {
    uint64_t one = 1;
    if (fd >= 0 && write(fd, &one, sizeof(one)) != sizeof(one)) { /* full: it is readable already */ }
}

bool pure_link_queue(PureLink *l, uint64_t seq, const unsigned char *bytes, size_t size) {
    pthread_mutex_lock(&l->lock);
    bool ok = pure_link_sender_queue(l->sender, seq, bytes, size), signal = false;
    if (ok) {
        l->queued_new = true;
        signal = l->sleeping && !l->signaled;
        l->signaled = l->signaled || signal;
    }
    pthread_mutex_unlock(&l->lock);
    if (signal) wake(l->wake);
    return ok;
}

uint64_t pure_link_queued(PureLink *l) {
    pthread_mutex_lock(&l->lock);
    uint64_t queued = pure_link_sender_queued(l->sender);
    pthread_mutex_unlock(&l->lock);
    return queued;
}

uint64_t pure_link_delivered(PureLink *l) {
    pthread_mutex_lock(&l->lock);
    uint64_t stored = pure_link_sender_stored(l->sender);
    pthread_mutex_unlock(&l->lock);
    return stored;
}

static void take_answers(PureLink *l, uint64_t now) {
    for (unsigned rounds = 0, n = TAKE; rounds < 4 && n == TAKE; ++rounds) {
        n = (unsigned)take_datagrams(l->fd, l->taking);
        for (unsigned k = 0; k < n; ++k)
            pure_link_sender_take(l->sender, l->taking->bytes[k], l->taking->messages[k].msg_len, now);
    }
}

static bool transient(void) { return errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOBUFS || errno == EINTR; }

/* The burst in one call, cut into datagrams of the segment's size by the
 * system (UDP GSO, asked for this call alone: a packet sent by itself is
 * never cut, however large). */
static ssize_t send_burst(PureLink *l) {
    _Alignas(8) unsigned char control[CMSG_SPACE(sizeof(uint16_t))] = {0};
    struct iovec part = {l->burst, l->burst_size};
    struct msghdr m = {.msg_iov = &part, .msg_iovlen = 1, .msg_control = control, .msg_controllen = sizeof(control)};
    struct cmsghdr *c = CMSG_FIRSTHDR(&m);
    c->cmsg_level = SOL_UDP;
    c->cmsg_type = UDP_SEGMENT;
    c->cmsg_len = CMSG_LEN(sizeof(uint16_t));
    uint16_t segment = (uint16_t)l->segment;
    memcpy(CMSG_DATA(c), &segment, sizeof(segment));
    return sendmsg(l->fd, &m, 0);
}

/* The packets of the burst to the socket (each but the last a whole
 * datagram): with GSO in one call, else one by one. */
static void flush(PureLink *l) {
    unsigned n = l->burst_count, sent = 0;
    bool whole = n > 1 && l->gso;
    if (whole && send_burst(l) >= 0) sent = n;
    else if (whole && !transient()) l->gso = false, whole = false; /* no GSO here: one by one from now on */
    for (size_t at = 0; !whole && sent < n; ++sent) {
        size_t size = sent + 1 < n ? l->segment : l->burst_size - at;
        if (send(l->fd, l->burst + at, size, 0) < 0 && transient()) break;
        at += size;
    }
    for (unsigned k = sent; k < n; ++k) pure_link_sender_refused(l->sender, l->burst_pn[k]);
    l->burst_count = 0;
    l->burst_size = 0;
}

/* Packets while the sender has them (up to 128 a turn), into bursts: a
 * short packet ends one, and a probe of a larger datagram goes alone. */
static void send_packets(PureLink *l, uint64_t now) {
    l->queued_new = false; /* all queued is looked at now */
    l->segment = pure_link_sender_datagram(l->sender); /* larger once the path is found to carry it */
    unsigned most = BURST_BYTES / l->segment < BURST ? (unsigned)(BURST_BYTES / l->segment) : BURST;
    for (unsigned n = 0; n < 128; ++n) {
        if (l->burst_count && l->burst_size != (size_t)l->burst_count * l->segment) flush(l); /* the last was short */
        uint64_t pn;
        size_t size = pure_link_sender_packet(l->sender, l->burst + l->burst_size, l->gso, now, &pn);
        if (!size) break;
        if (size > l->segment) { /* a probe: the burst before it, then it alone */
            unsigned char probe[DATAGRAM_MOST];
            memcpy(probe, l->burst + l->burst_size, size);
            flush(l);
            memcpy(l->burst, probe, size);
            l->burst_pn[0] = pn, l->burst_count = 1, l->burst_size = size;
            flush(l);
            continue;
        }
        l->burst_pn[l->burst_count++] = pn;
        l->burst_size += size;
        if (l->burst_count == most) flush(l);
    }
    flush(l);
}

static void *run_link(void *context) {
    PureLink *l = context;
    for (;;) {
        pthread_mutex_lock(&l->lock);
        uint64_t now = now_ns(), due = pure_link_sender_due(l->sender);
        bool stopping = l->stopping, more = l->queued_new; /* queued while it sent: no sleep */
        l->sleeping = !more;
        pthread_mutex_unlock(&l->lock);
        if (stopping) break;
        int timeout = more ? 0 : due ? (int)((due > now ? due - now : 0) / MS) + 1 : -1;
        struct pollfd waits[2] = {{l->fd, POLLIN, 0}, {l->wake, POLLIN, 0}};
        if (poll(waits, 2, timeout) < 0 && errno != EINTR) break;
        if (waits[1].revents & POLLIN) {
            uint64_t signals;
            if (read(l->wake, &signals, sizeof(signals)) < 0) { /* cleared already */ }
        }
        pthread_mutex_lock(&l->lock);
        l->sleeping = l->signaled = false;
        now = now_ns();
        if (waits[0].revents) take_answers(l, now);
        pure_link_sender_tick(l->sender, now);
        send_packets(l, now);
        uint64_t stored = pure_link_sender_stored(l->sender);
        bool told = stored != l->told;
        l->told = stored;
        pthread_mutex_unlock(&l->lock);
        if (told && l->notify) l->notify(l->context);
    }
    return NULL;
}

bool pure_link_start(PureLink *l, void (*notify)(void *), void *context, char *error, size_t size) {
    if (!l->peered) {
        snprintf(error, size, "pure://%s:%s/%s: no receiver's key (--seal)", l->host, l->port, l->input);
        return false;
    }
    struct addrinfo hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_DGRAM}, *found = NULL;
    int resolved = getaddrinfo(l->host, l->port, &hints, &found);
    if (resolved) {
        snprintf(error, size, "%s: %s", l->host, gai_strerror(resolved));
        return false;
    }
    l->fd = socket(found->ai_family, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    bool ok = l->fd >= 0 && !connect(l->fd, found->ai_addr, found->ai_addrlen);
    int buffer = 4 << 20, family = found->ai_family;
    freeaddrinfo(found);
    if (ok) {
        setsockopt(l->fd, SOL_SOCKET, SO_SNDBUF, &buffer, sizeof(buffer));
        setsockopt(l->fd, SOL_SOCKET, SO_RCVBUF, &buffer, sizeof(buffer));
        /* Never cut up on the way (the don't-fragment bit): a packet the
         * path does not carry is lost, as a probe of a larger size must be. */
        int probe = IP_PMTUDISC_PROBE, probe6 = IPV6_PMTUDISC_PROBE;
        if (family == AF_INET6) setsockopt(l->fd, IPPROTO_IPV6, IPV6_MTU_DISCOVER, &probe6, sizeof(probe6));
        else setsockopt(l->fd, IPPROTO_IP, IP_MTU_DISCOVER, &probe, sizeof(probe));
    }
    /* GSO where the system has it: asked for each burst, never for the socket. */
    int segment = DATAGRAM, none = 0;
    l->segment = DATAGRAM;
    l->gso = ok && !l->no_gso && !setsockopt(l->fd, SOL_UDP, UDP_SEGMENT, &segment, sizeof(segment)) &&
             !setsockopt(l->fd, SOL_UDP, UDP_SEGMENT, &none, sizeof(none));
    ok = ok && l->keyed && (l->burst = malloc(BURST_BYTES + DATAGRAM_MOST)) && (l->taking = malloc(sizeof(*l->taking))) &&
         (l->wake = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)) >= 0;
    l->notify = notify;
    l->context = context;
    if (!ok || pthread_create(&l->thread, NULL, run_link, l)) {
        snprintf(error, size, "pure://%s:%s/%s: cannot start (a socket, the keys)", l->host, l->port, l->input);
        return false;
    }
    pthread_setname_np(l->thread, "pure-link"); /* a link's CPU, seen apart */
    l->started = true;
    return true;
}

void pure_link_counts(PureLink *l, PureLinkCounts *out) {
    pthread_mutex_lock(&l->lock);
    pure_link_sender_counts(l->sender, out);
    pthread_mutex_unlock(&l->lock);
}

void pure_link_close(PureLink *l) {
    if (!l) return;
    if (l->started) {
        pthread_mutex_lock(&l->lock);
        l->stopping = true;
        pthread_mutex_unlock(&l->lock);
        wake(l->wake);
        pthread_join(l->thread, NULL);
    }
    pure_link_sender_free(l->sender);
    free(l->burst);
    free(l->taking);
    if (l->fd >= 0) close(l->fd);
    if (l->wake >= 0) close(l->wake);
    pthread_mutex_destroy(&l->lock);
    free(l);
}

/* ---- The receiving side */

struct PureLinkListener {
    PureLinkReceiver *receiver; /* the thread's */
    unsigned port;
    int fd, wake, family;
    pthread_t thread;
    bool started;
    pthread_mutex_t lock; /* the done list and stopping */
    PureLinkDone *done;
    size_t done_count, done_capacity;
    bool stopping;
};

PureLinkListener *pure_link_listen(PureLinkReceiver *r, unsigned port, char *error, size_t size) {
    PureLinkListener *s = calloc(1, sizeof(*s));
    if (!s || pthread_mutex_init(&s->lock, NULL)) {
        free(s);
        snprintf(error, size, "out of memory");
        return NULL;
    }
    s->receiver = r;
    s->port = port;
    s->fd = s->wake = -1;
    return s;
}

/* An answer by the path its sender's packet came: to where it came from,
 * from the address it came to. */
static bool send_answer(void *context, const unsigned char *packet, size_t size, const unsigned char *to,
                        size_t to_size) {
    const PureLinkListener *s = context;
    Path path;
    if (to_size != sizeof(path)) return false;
    memcpy(&path, to, sizeof(path));
    _Alignas(8) unsigned char control[CMSG_SPACE(sizeof(struct in6_pktinfo))] = {0};
    struct iovec part = {(void *)packet, size};
    struct msghdr m = {.msg_name = &path.from, .msg_iov = &part, .msg_iovlen = 1, .msg_control = control};
    m.msg_namelen = path.from.six.sin6_family == AF_INET6 ? sizeof(path.from.six) : sizeof(path.from.four);
    struct cmsghdr *c = (struct cmsghdr *)(void *)control; /* the first */
    if (s->family == AF_INET6 && !IN6_IS_ADDR_UNSPECIFIED(&path.to.six.ipi6_addr)) {
        *c = (struct cmsghdr){.cmsg_level = IPPROTO_IPV6, .cmsg_type = IPV6_PKTINFO, .cmsg_len = CMSG_LEN(sizeof(path.to.six))};
        memcpy(CMSG_DATA(c), &path.to.six, sizeof(path.to.six));
        m.msg_controllen = CMSG_SPACE(sizeof(path.to.six));
    } else if (s->family == AF_INET && path.to.four.ipi_addr.s_addr) {
        struct in_pktinfo from = {.ipi_ifindex = path.to.four.ipi_ifindex, .ipi_spec_dst = path.to.four.ipi_addr};
        *c = (struct cmsghdr){.cmsg_level = IPPROTO_IP, .cmsg_type = IP_PKTINFO, .cmsg_len = CMSG_LEN(sizeof(from))};
        memcpy(CMSG_DATA(c), &from, sizeof(from));
        m.msg_controllen = CMSG_SPACE(sizeof(from));
    }
    if (!m.msg_controllen) m.msg_control = NULL;
    return sendmsg(s->fd, &m, 0) >= 0;
}

static void *run_listener(void *context) {
    PureLinkListener *s = context;
    Gathering *gathering = malloc(sizeof(*gathering));
    if (!gathering) return NULL;
    for (;;) {
        uint64_t now = now_ns(), due = pure_link_receiver_due(s->receiver);
        int timeout = due ? (int)((due > now ? due - now : 0) / MS) + 1 : -1;
        struct pollfd waits[2] = {{s->fd, POLLIN, 0}, {s->wake, POLLIN, 0}};
        if (poll(waits, 2, timeout) < 0 && errno != EINTR) break;
        pthread_mutex_lock(&s->lock);
        bool stopping = s->stopping;
        pthread_mutex_unlock(&s->lock);
        if (stopping) break;
        if (waits[1].revents & POLLIN) {
            uint64_t signals;
            if (read(s->wake, &signals, sizeof(signals)) < 0) { /* cleared already */ }
        }
        now = now_ns();
        for (unsigned rounds = 0, n = GATHER; rounds < 8 && n == GATHER; ++rounds) {
            n = (unsigned)gather(s->fd, gathering);
            for (unsigned k = 0; k < n; ++k) {
                struct mmsghdr *m = &gathering->messages[k];
                Path path;
                size_t length = m->msg_len, step = segment_of(&m->msg_hdr, length, &path);
                for (size_t at = 0; at < length; at += step)
                    pure_link_receiver_take(s->receiver, gathering->bytes[k] + at, step < length - at ? step : length - at,
                                            &path, sizeof(path), now);
            }
        }
        pthread_mutex_lock(&s->lock);
        PureLinkDone *done = s->done;
        size_t count = s->done_count;
        s->done = NULL;
        s->done_count = s->done_capacity = 0;
        pthread_mutex_unlock(&s->lock);
        pure_link_receiver_turn(s->receiver, done, count, now, send_answer, s);
        free(done);
    }
    free(gathering);
    return NULL;
}

bool pure_link_serve(PureLinkListener *s, char *error, size_t size) {
    struct sockaddr_in6 six = {.sin6_family = AF_INET6, .sin6_port = htons((uint16_t)s->port), .sin6_addr = in6addr_any};
    int off = 0, buffer = 4 << 20;
    s->fd = socket(AF_INET6, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    bool bound = s->fd >= 0 && !setsockopt(s->fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off)) &&
                 !bind(s->fd, (struct sockaddr *)&six, sizeof(six));
    if (!bound) { /* no IPv6 here */
        if (s->fd >= 0) close(s->fd);
        struct sockaddr_in four = {.sin_family = AF_INET, .sin_port = htons((uint16_t)s->port), .sin_addr = {INADDR_ANY}};
        s->fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        bound = s->fd >= 0 && !bind(s->fd, (struct sockaddr *)&four, sizeof(four));
    }
    if (!bound) {
        snprintf(error, size, "--link-port %u: %s", s->port, strerror(errno));
        return false;
    }
    setsockopt(s->fd, SOL_SOCKET, SO_RCVBUF, &buffer, sizeof(buffer));
    setsockopt(s->fd, SOL_SOCKET, SO_SNDBUF, &buffer, sizeof(buffer));
    int one = 1; /* without GRO, a buffer is a datagram */
    setsockopt(s->fd, SOL_UDP, UDP_GRO, &one, sizeof(one));
    /* The address each packet came to, answered from (IPv4 too, as ::ffff:a.b.c.d, on a socket of both). */
    struct sockaddr_storage own;
    socklen_t own_size = sizeof(own);
    s->family = getsockname(s->fd, (struct sockaddr *)&own, &own_size) ? AF_INET : own.ss_family;
    if (s->family == AF_INET6) setsockopt(s->fd, IPPROTO_IPV6, IPV6_RECVPKTINFO, &one, sizeof(one));
    else setsockopt(s->fd, IPPROTO_IP, IP_PKTINFO, &one, sizeof(one));
    s->wake = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (s->wake < 0 || pthread_create(&s->thread, NULL, run_listener, s)) {
        snprintf(error, size, "--link-port %u: cannot start", s->port);
        return false;
    }
    pthread_setname_np(s->thread, "pure-listen");
    s->started = true;
    return true;
}

void pure_link_done(PureLinkListener *s, void *token, uint64_t heard, bool stored, bool gap) {
    pthread_mutex_lock(&s->lock);
    if (s->done_count == s->done_capacity) {
        size_t capacity = s->done_capacity ? 2 * s->done_capacity : 16;
        PureLinkDone *grown = realloc(s->done, capacity * sizeof(*grown));
        if (!grown) abort(); /* a delivery's word cannot be lost */
        s->done = grown;
        s->done_capacity = capacity;
    }
    s->done[s->done_count++] = (PureLinkDone){token, heard, stored, gap};
    pthread_mutex_unlock(&s->lock);
    wake(s->wake);
}

void pure_link_listener_close(PureLinkListener *s, PureLinkCounts *counts) {
    if (!s) return;
    if (s->started) {
        pthread_mutex_lock(&s->lock);
        s->stopping = true;
        pthread_mutex_unlock(&s->lock);
        wake(s->wake);
        pthread_join(s->thread, NULL);
    }
    if (counts) pure_link_receiver_counts(s->receiver, counts);
    pure_link_receiver_free(s->receiver);
    free(s->done);
    if (s->fd >= 0) close(s->fd);
    if (s->wake >= 0) close(s->wake);
    pthread_mutex_destroy(&s->lock);
    free(s);
}
