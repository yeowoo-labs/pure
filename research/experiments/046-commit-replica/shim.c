/* Research 046 (029's principle 2, its breaking experiment): what an owner's store does to its files, shipped as it is.
 * Preloaded into the owner (pure-run --store DIR), it sees every call that changes a file under DIR (SHIM_STORE, an
 * absolute path) and keeps it as an op (ops.h). Each fsync or fdatasync of such a file or directory ends a cut: the
 * cut goes to the shipper (SHIM_SOCKET, a unix socket), and the sync returns only once the shipper says the follower
 * has it (synchronous: the owner holds one cut in flight, no log). A call that would change a file under DIR some way
 * not shipped (a shared writable mapping, a duplicated descriptor) stops the process: a copy that differs in silence
 * would be no finding at all.
 *
 * SHIM_PAUSE_EVERY=N with SHIM_PAUSE_DIR: after every Nth cut's sync, it writes the cut's number to DIR/paused and
 * waits for a byte from DIR/resume (the harness compares the two stores meanwhile). SHIM_STATS: a file for counts at
 * exit (ops and bytes by kind, cuts, the time waited for acks, the names renamed to). SHIM_JOURNAL: a file that gets
 * every cut's ops as shipped, in order (prefixes.py makes stores of its prefixes).
 *
 *   cc -O2 -std=gnu17 -fPIC -shared shim.c -ldl -pthread -o shim.so */
#define _GNU_SOURCE
#include "ops.h"
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

enum { FDS = 65536 };
static char *paths[FDS]; /* an open descriptor's path relative to the store, when it is under it */
static char root[PATH_MAX];
static size_t root_length;
static bool active;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned char *ops;
static size_t used, room;
static uint64_t cuts, op_count[OP_KINDS], op_bytes[OP_KINDS], shipped, waited_ns;
static int link_fd = -1, journal_fd = -1; /* SHIM_JOURNAL: every cut's ops, in order, for prefixes.py */
static char renamed[32][64]; /* the names renamed to, and how often (in the counts at exit) */
static uint64_t renamed_count[32];
static unsigned renamed_names;
static unsigned pause_every;
static char pause_dir[PATH_MAX];

static int (*real_open)(const char *, int, ...);
static int (*real_openat)(int, const char *, int, ...);
static int (*real_close)(int);
static ssize_t (*real_pwrite)(int, const void *, size_t, off_t);
static int (*real_ftruncate)(int, off_t);
static int (*real_posix_fallocate)(int, off_t, off_t);
static int (*real_renameat)(int, const char *, int, const char *);
static int (*real_unlinkat)(int, const char *, int);
static int (*real_mkdir)(const char *, mode_t);
static int (*real_fsync)(int);
static int (*real_fdatasync)(int);
static void *(*real_mmap)(void *, size_t, int, int, int, off_t);
static int (*real_fcntl)(int, int, ...);
static ssize_t (*real_write)(int, const void *, size_t);

static void stop(const char *why, const char *path) {
    fprintf(stderr, "shim: %s (%s): stopping, the follower would differ\n", why, path ? path : "");
    abort();
}

__attribute__((constructor)) static void start(void) {
    real_open = dlsym(RTLD_NEXT, "open"), real_openat = dlsym(RTLD_NEXT, "openat"), real_close = dlsym(RTLD_NEXT, "close");
    real_pwrite = dlsym(RTLD_NEXT, "pwrite"), real_ftruncate = dlsym(RTLD_NEXT, "ftruncate");
    real_posix_fallocate = dlsym(RTLD_NEXT, "posix_fallocate"), real_renameat = dlsym(RTLD_NEXT, "renameat");
    real_unlinkat = dlsym(RTLD_NEXT, "unlinkat"), real_mkdir = dlsym(RTLD_NEXT, "mkdir");
    real_fsync = dlsym(RTLD_NEXT, "fsync"), real_fdatasync = dlsym(RTLD_NEXT, "fdatasync");
    real_mmap = dlsym(RTLD_NEXT, "mmap"), real_fcntl = dlsym(RTLD_NEXT, "fcntl"), real_write = dlsym(RTLD_NEXT, "write");
    const char *store = getenv("SHIM_STORE");
    if (!store || store[0] != '/' || strlen(store) >= sizeof(root)) return;
    strcpy(root, store);
    root_length = strlen(root);
    while (root_length > 1 && root[root_length - 1] == '/') root[--root_length] = 0;
    const char *every = getenv("SHIM_PAUSE_EVERY"), *dir = getenv("SHIM_PAUSE_DIR");
    if (every && dir && strlen(dir) < sizeof(pause_dir) - 16) pause_every = (unsigned)atoi(every), strcpy(pause_dir, dir);
    active = true;
}

__attribute__((destructor)) static void finish(void) {
    const char *at = getenv("SHIM_STATS");
    FILE *f = at && active ? fopen(at, "w") : NULL;
    if (!f) return;
    fprintf(f, "{\"cuts\":%llu,\"shipped_bytes\":%llu,\"waited_ms\":%.1f", (unsigned long long)cuts,
            (unsigned long long)shipped, waited_ns / 1e6);
    for (int k = 1; k < OP_KINDS; ++k)
        fprintf(f, ",\"%s\":[%llu,%llu]", op_names[k], (unsigned long long)op_count[k], (unsigned long long)op_bytes[k]);
    fprintf(f, ",\"renamed_to\":{");
    for (unsigned r = 0; r < renamed_names; ++r) fprintf(f, "%s\"%s\":%llu", r ? "," : "", renamed[r], (unsigned long long)renamed_count[r]);
    fprintf(f, "}}\n");
    fclose(f);
}

/* path made absolute (against dirfd's or the working directory) and without "." parts: relative to the store, or NULL. */
static char *under(int dirfd, const char *path) {
    if (!active || !path) return NULL;
    char whole[2 * PATH_MAX];
    if (path[0] == '/') snprintf(whole, sizeof(whole), "%s", path);
    else if (dirfd == AT_FDCWD) {
        char cwd[PATH_MAX];
        if (!getcwd(cwd, sizeof(cwd))) return NULL;
        snprintf(whole, sizeof(whole), "%s/%s", cwd, path);
    } else if (dirfd >= 0 && dirfd < FDS && paths[dirfd])
        snprintf(whole, sizeof(whole), "%s%s%s/%s", root, paths[dirfd][0] ? "/" : "", paths[dirfd], path);
    else return NULL;
    /* drop "/." parts and a trailing "/" */
    char clean[2 * PATH_MAX];
    size_t n = 0;
    for (const char *p = whole; *p;) {
        if (p[0] == '/' && p[1] == '.' && (p[2] == '/' || p[2] == 0)) { p += 2; continue; }
        if (p[0] == '/' && p[1] == '/') { ++p; continue; }
        clean[n++] = *p++;
    }
    while (n > 1 && clean[n - 1] == '/') --n;
    clean[n] = 0;
    if (!strncmp(clean, root, root_length) && (clean[root_length] == 0 || clean[root_length] == '/'))
        return strdup(clean[root_length] ? clean + root_length + 1 : "");
    return NULL;
}

static void keep(int fd, char *relative) {
    if (fd < 0 || fd >= FDS) { free(relative); return; }
    free(paths[fd]);
    paths[fd] = relative;
}

static unsigned char *op(uint8_t kind, const char *path, size_t extra) {
    size_t length = strlen(path), need = 3 + length + extra;
    if (used + need > room) {
        size_t more = room ? room : 1 << 20;
        while (used + need > more) more *= 2;
        if (!(ops = realloc(ops, more))) stop("out of memory", path);
        room = more;
    }
    unsigned char *p = ops + used;
    p[0] = kind;
    put16(p + 1, (uint16_t)length);
    memcpy(p + 3, path, length);
    used += need;
    ++op_count[kind], op_bytes[kind] += need;
    return p + 3 + length;
}

static bool full(int fd, const void *bytes, size_t size) {
    for (const unsigned char *p = bytes; size;) {
        ssize_t n = real_write(fd, p, size);
        if (n <= 0) { if (n < 0 && errno == EINTR) continue; return false; }
        p += n, size -= (size_t)n;
    }
    return true;
}
static bool whole_read(int fd, void *bytes, size_t size) {
    for (unsigned char *p = bytes; size;) {
        ssize_t n = read(fd, p, size);
        if (n <= 0) { if (n < 0 && errno == EINTR) continue; return false; }
        p += n, size -= (size_t)n;
    }
    return true;
}

/* A cut ends with path's sync: shipped, and waited for until the follower has it. */
static void cut(const char *path) {
    (void)op(OP_SYNC, path, 0);
    ++cuts;
    if (link_fd < 0) {
        const char *at = getenv("SHIM_SOCKET");
        struct sockaddr_un address = {.sun_family = AF_UNIX};
        if (!at || strlen(at) >= sizeof(address.sun_path)) stop("no SHIM_SOCKET", path);
        strcpy(address.sun_path, at);
        if ((link_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0)) < 0 ||
            connect(link_fd, (struct sockaddr *)&address, sizeof(address)))
            stop("cannot reach the shipper", at);
    }
    unsigned char head[16];
    put64(head, cuts), put64(head + 8, used);
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    if (!full(link_fd, head, sizeof(head)) || !full(link_fd, ops, used)) stop("the shipper went", path);
    unsigned char ack[8];
    if (!whole_read(link_fd, ack, sizeof(ack)) || get64(ack) != cuts) stop("no ack for the cut", path);
    clock_gettime(CLOCK_MONOTONIC, &b);
    waited_ns += (uint64_t)((b.tv_sec - a.tv_sec) * 1000000000LL + (b.tv_nsec - a.tv_nsec));
    if (journal_fd < 0 && getenv("SHIM_JOURNAL")) journal_fd = real_open(getenv("SHIM_JOURNAL"), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (journal_fd >= 0 && !full(journal_fd, ops, used)) stop("the journal", getenv("SHIM_JOURNAL"));
    shipped += used;
    used = 0;
}

static void pause_if_due(void) {
    if (!pause_every || cuts % pause_every) return;
    char at[PATH_MAX + 16], line[32];
    snprintf(at, sizeof(at), "%s/paused", pause_dir);
    int fd = real_open(at, O_WRONLY | O_CLOEXEC);
    if (fd < 0) stop("no paused fifo", at);
    int n = snprintf(line, sizeof(line), "%llu\n", (unsigned long long)cuts);
    full(fd, line, (size_t)n);
    real_close(fd);
    snprintf(at, sizeof(at), "%s/resume", pause_dir);
    if ((fd = real_open(at, O_RDONLY | O_CLOEXEC)) < 0) stop("no resume fifo", at);
    char go;
    whole_read(fd, &go, 1);
    real_close(fd);
}

static int opened(int fd, char *relative, int flags, mode_t mode) {
    if (fd < 0 || !relative) { free(relative); return fd; }
    if (flags & (O_CREAT | O_TRUNC)) {
        unsigned char *p = op(OP_CREATE, relative, 8);
        put32(p, (uint32_t)(flags & (O_CREAT | O_EXCL | O_TRUNC))), put32(p + 4, (uint32_t)mode);
    }
    keep(fd, relative);
    return fd;
}

int open(const char *path, int flags, ...) {
    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE)) { va_list v; va_start(v, flags); mode = va_arg(v, mode_t); va_end(v); }
    pthread_mutex_lock(&lock);
    char *relative = under(AT_FDCWD, path);
    if (relative && (flags & O_TMPFILE) == O_TMPFILE) stop("O_TMPFILE", path);
    int fd = real_open(path, flags, mode);
    fd = opened(fd, relative, flags, mode);
    pthread_mutex_unlock(&lock);
    return fd;
}

int openat(int dirfd, const char *path, int flags, ...) {
    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE)) { va_list v; va_start(v, flags); mode = va_arg(v, mode_t); va_end(v); }
    pthread_mutex_lock(&lock);
    char *relative = under(dirfd, path);
    if (relative && (flags & O_TMPFILE) == O_TMPFILE) stop("O_TMPFILE", path);
    int fd = real_openat(dirfd, path, flags, mode);
    fd = opened(fd, relative, flags, mode);
    pthread_mutex_unlock(&lock);
    return fd;
}

int close(int fd) {
    pthread_mutex_lock(&lock);
    if (fd >= 0 && fd < FDS && paths[fd]) free(paths[fd]), paths[fd] = NULL;
    pthread_mutex_unlock(&lock);
    return real_close(fd);
}

/* A call that changes a file runs under the lock: two threads' ops are kept in the order they happened. */
ssize_t pwrite(int fd, const void *bytes, size_t size, off_t offset) {
    pthread_mutex_lock(&lock);
    ssize_t n = real_pwrite(fd, bytes, size, offset);
    if (n > 0 && fd >= 0 && fd < FDS && paths[fd]) {
        unsigned char *p = op(OP_WRITE, paths[fd], 12 + (size_t)n);
        put64(p, (uint64_t)offset), put32(p + 8, (uint32_t)n);
        memcpy(p + 12, bytes, (size_t)n);
    }
    pthread_mutex_unlock(&lock);
    return n;
}
ssize_t pwrite64(int fd, const void *bytes, size_t size, off_t offset) { return pwrite(fd, bytes, size, offset); }

ssize_t write(int fd, const void *bytes, size_t size) {
    if (fd >= 0 && fd < FDS && paths[fd] && size) stop("write without an offset", paths[fd]);
    return real_write(fd, bytes, size);
}

int ftruncate(int fd, off_t length) {
    pthread_mutex_lock(&lock);
    int rc = real_ftruncate(fd, length);
    if (!rc && fd >= 0 && fd < FDS && paths[fd]) put64(op(OP_TRUNCATE, paths[fd], 8), (uint64_t)length);
    pthread_mutex_unlock(&lock);
    return rc;
}

int posix_fallocate(int fd, off_t offset, off_t length) {
    pthread_mutex_lock(&lock);
    int rc = real_posix_fallocate(fd, offset, length);
    if (!rc && fd >= 0 && fd < FDS && paths[fd]) {
        unsigned char *p = op(OP_ALLOCATE, paths[fd], 16);
        put64(p, (uint64_t)offset), put64(p + 8, (uint64_t)length);
    }
    pthread_mutex_unlock(&lock);
    return rc;
}

int renameat(int from_dir, const char *from, int to_dir, const char *to) {
    pthread_mutex_lock(&lock);
    char *a = under(from_dir, from), *b = under(to_dir, to);
    int rc = real_renameat(from_dir, from, to_dir, to);
    if (!rc && (a || b)) {
        if (!a || !b) stop("a rename into or out of the store", a ? a : b);
        unsigned r = 0;
        while (r < renamed_names && strcmp(renamed[r], b)) ++r;
        if (r == renamed_names && r < 32) snprintf(renamed[renamed_names++], sizeof(renamed[0]), "%s", b);
        if (r < 32) ++renamed_count[r];
        unsigned char *p = op(OP_RENAME, a, 2 + strlen(b));
        put16(p, (uint16_t)strlen(b));
        memcpy(p + 2, b, strlen(b));
    }
    free(a), free(b);
    pthread_mutex_unlock(&lock);
    return rc;
}

int unlinkat(int dirfd, const char *path, int flags) {
    pthread_mutex_lock(&lock);
    char *relative = under(dirfd, path);
    int rc = real_unlinkat(dirfd, path, flags);
    if (!rc && relative) put32(op(OP_UNLINK, relative, 4), (uint32_t)flags);
    free(relative);
    pthread_mutex_unlock(&lock);
    return rc;
}

int mkdir(const char *path, mode_t mode) {
    pthread_mutex_lock(&lock);
    char *relative = under(AT_FDCWD, path);
    int rc = real_mkdir(path, mode);
    if (!rc && relative) put32(op(OP_MKDIR, relative, 4), (uint32_t)mode);
    free(relative);
    pthread_mutex_unlock(&lock);
    return rc;
}

static int synced(int fd, int (*real)(int)) {
    pthread_mutex_lock(&lock);
    bool due = false;
    if (fd >= 0 && fd < FDS && paths[fd]) cut(paths[fd]), due = true;
    int rc = real(fd);
    if (due) pause_if_due();
    pthread_mutex_unlock(&lock);
    return rc;
}
int fsync(int fd) { return synced(fd, real_fsync); }
int fdatasync(int fd) { return synced(fd, real_fdatasync); }

void *mmap(void *at, size_t size, int protection, int flags, int fd, off_t offset) {
    if (fd >= 0 && fd < FDS && paths[fd] && protection & PROT_WRITE && flags & MAP_SHARED) stop("a shared writable mapping", paths[fd]);
    return real_mmap(at, size, protection, flags, fd, offset);
}

int fcntl(int fd, int command, ...) {
    va_list v;
    va_start(v, command);
    void *argument = va_arg(v, void *);
    va_end(v);
    if (fd >= 0 && fd < FDS && paths[fd] && (command == F_DUPFD || command == F_DUPFD_CLOEXEC)) stop("a duplicated descriptor", paths[fd]);
    return real_fcntl(fd, command, argument);
}
