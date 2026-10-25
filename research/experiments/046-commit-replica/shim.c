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
 * exit (ops and bytes by kind, cuts, the time waited for acks, the names renamed to, and the owner's own syncs by the
 * file they sync: count, time, percentiles and those over 10 ms; and the time from one sync of pure.tree to the next,
 * a save's whole time where each save ends in one). SHIM_JOURNAL: a file that gets every cut's ops as shipped, in order
 * (prefixes.py makes stores of its prefixes). SHIM_ALONE=1: no cut is sent (no shipper): the owner alone, its syncs
 * timed as they are.
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
/* The owner's own syncs (after the ack), by what they sync: their durations in ns. */
enum { SYNC_LOG, SYNC_TREE, SYNC_COMPACTION, SYNC_IMAGE, SYNC_DIRECTORY, SYNC_OTHER, SYNC_KINDS };
static const char *const sync_names[SYNC_KINDS] = {"log", "tree", "compaction", "image", "directory", "other"};
static uint64_t *sync_ns[SYNC_KINDS];
static size_t sync_count[SYNC_KINDS], sync_room[SYNC_KINDS];
static uint64_t *saves_ns, last_tree_ns; /* from one tree sync's end to the next's */
static size_t saves, saves_room;

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

static int by_value(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

__attribute__((destructor)) static void finish(void) {
    const char *at = getenv("SHIM_STATS");
    FILE *f = at && active ? fopen(at, "w") : NULL;
    if (!f) return;
    fprintf(f, "{\"cuts\":%llu,\"shipped_bytes\":%llu,\"waited_ms\":%.1f", (unsigned long long)cuts,
            (unsigned long long)shipped, waited_ns / 1e6);
    fprintf(f, ",\"syncs\":{");
    for (int k = 0, first = 1; k < SYNC_KINDS; ++k) {
        size_t n = sync_count[k];
        if (!n) continue;
        uint64_t total = 0;
        for (size_t i = 0; i < n; ++i) total += sync_ns[k][i];
        qsort(sync_ns[k], n, sizeof(*sync_ns[k]), by_value);
        size_t over = 0;
        uint64_t over_ns = 0;
        for (size_t i = 0; i < n; ++i)
            if (sync_ns[k][i] > 10000000) ++over, over_ns += sync_ns[k][i];
        fprintf(f, "%s\"%s\":{\"n\":%zu,\"ms\":%.1f,\"p50_us\":%.0f,\"p90_us\":%.0f,\"p99_us\":%.0f,\"over_10ms_n\":%zu,"
                   "\"over_10ms_ms\":%.1f}", first ? "" : ",", sync_names[k], n, total / 1e6, sync_ns[k][n / 2] / 1e3,
                sync_ns[k][n * 9 / 10] / 1e3, sync_ns[k][n * 99 / 100] / 1e3, over, over_ns / 1e6);
        first = 0;
    }
    fprintf(f, "}");
    if (saves) {
        qsort(saves_ns, saves, sizeof(*saves_ns), by_value);
        fprintf(f, ",\"saves\":{\"n\":%zu,\"p50_us\":%.0f,\"p90_us\":%.0f,\"p99_us\":%.0f}", saves, saves_ns[saves / 2] / 1e3,
                saves_ns[saves * 9 / 10] / 1e3, saves_ns[saves * 99 / 100] / 1e3);
    }
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
    if (getenv("SHIM_ALONE")) { /* the owner alone: its ops counted, none sent */
        shipped += used;
        used = 0;
        return;
    }
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

/* What a sync of fd (its path relative to the store) syncs. */
static int sync_kind(int fd, const char *path) {
    struct stat st;
    if (!fstat(fd, &st) && S_ISDIR(st.st_mode)) return SYNC_DIRECTORY;
    if (!strncmp(path, "log-", 4)) return SYNC_LOG;
    if (!strcmp(path, "pure.tree")) return SYNC_TREE;
    if (!strncmp(path, ".pending", 8)) return SYNC_COMPACTION;
    if (strstr(path, "image")) return SYNC_IMAGE;
    return SYNC_OTHER;
}

static uint64_t now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

static int synced(int fd, int (*real)(int)) {
    pthread_mutex_lock(&lock);
    bool due = false;
    if (fd >= 0 && fd < FDS && paths[fd]) cut(paths[fd]), due = true;
    int kind = due ? sync_kind(fd, paths[fd]) : -1;
    uint64_t began = now_ns();
    int rc = real(fd);
    uint64_t took = now_ns() - began;
    if (kind >= 0) {
        if (sync_count[kind] == sync_room[kind]) {
            size_t room_ = sync_room[kind] ? 2 * sync_room[kind] : 1024;
            uint64_t *grown = realloc(sync_ns[kind], room_ * sizeof(*grown));
            if (grown) sync_ns[kind] = grown, sync_room[kind] = room_;
        }
        if (sync_count[kind] < sync_room[kind]) sync_ns[kind][sync_count[kind]++] = took;
    }
    if (kind == SYNC_TREE) { /* a save's end: the time since the last */
        uint64_t end = began + took;
        if (last_tree_ns) {
            if (saves == saves_room) {
                size_t room_ = saves_room ? 2 * saves_room : 1024;
                uint64_t *grown = realloc(saves_ns, room_ * sizeof(*grown));
                if (grown) saves_ns = grown, saves_room = room_;
            }
            if (saves < saves_room) saves_ns[saves++] = end - last_tree_ns;
        }
        last_tree_ns = end;
    }
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
