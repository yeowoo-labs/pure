#define _POSIX_C_SOURCE 200809L
#include "store.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

/* Immutable extent envelope: marker, little-endian u64 revision and payload
 * length, then unchanged caller bytes. Single-threaded control API; cooperating
 * processes serialize publication using .writer.lock. */
enum { HEADER_BYTES = 24 };
static const unsigned char magic[8] = {'P','U','R','E','B','0','0','1'};
static uint64_t temporary_sequence;

static bool fault(PureStoreStage stage) {
#ifdef PURE_STORE_TEST
    if (pure_store_test_fault(stage)) { errno = EIO; return true; }
#else
    (void)stage;
#endif
    return false;
}

static bool file_name(const char *key, char name[72]) {
    if (!key) return false;
    size_t length = strlen(key);
    if (!length || length > 64) return false;
    for (size_t i = 0; i < length; ++i) {
        char b = key[i];
        if (!((b >= 'a' && b <= 'z') || (b >= 'A' && b <= 'Z') ||
              (b >= '0' && b <= '9') || b == '-' || b == '_')) return false;
    }
    snprintf(name, 72, "obj-%s", key);
    return true;
}

static uint64_t decode64(const unsigned char *bytes) {
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) value |= (uint64_t)bytes[i] << (8 * i);
    return value;
}

static void encode64(unsigned char *bytes, uint64_t value) {
    for (unsigned i = 0; i < 8; ++i) bytes[i] = (unsigned char)(value >> (8 * i));
}

static bool sync_fd(int fd) {
    int rc;
    do { rc = fsync(fd); } while (rc && errno == EINTR);
    return rc == 0;
}

static bool write_bytes(int fd, const unsigned char *bytes, size_t length) {
    while (length) {
        ssize_t n = write(fd, bytes, length);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        bytes += (size_t)n;
        length -= (size_t)n;
    }
    return true;
}

static bool read_header(int fd, int64_t *revision, size_t *length) {
    struct stat info;
    if (fstat(fd, &info) || !S_ISREG(info.st_mode)) return false;
    unsigned char header[HEADER_BYTES];
    size_t used = 0;
    while (used < sizeof(header)) {
        ssize_t n = pread(fd, header + used, sizeof(header) - used, (off_t)used);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        used += (size_t)n;
    }
    uint64_t version = decode64(header + 8), size = decode64(header + 16);
    if (memcmp(header, magic, sizeof(magic)) || !version || version > INT64_MAX ||
        size > PURE_OBJECT_LIMIT || info.st_size != (off_t)(HEADER_BYTES + size)) return false;
    *revision = (int64_t)version;
    *length = (size_t)size;
    return true;
}

PureResult pure_store_init(const char *path) {
    if (!path || !*path) return PURE_STORE_ERROR;
    bool created = mkdir(path, 0700) == 0;
    if (!created && errno != EEXIST) return PURE_STORE_ERROR;
    int directory = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (directory < 0) return PURE_STORE_ERROR; /* Never reinterpret an old DB file. */
    int lock = openat(directory, ".writer.lock", O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    struct stat info;
    bool ok = lock >= 0 && fstat(lock, &info) == 0 && S_ISREG(info.st_mode) && sync_fd(lock);
    if (lock >= 0) close(lock);
    if (ok) ok = sync_fd(directory);
    close(directory);
    if (!ok) return PURE_STORE_ERROR;
    if (created) {
        char *parent = strdup(path);
        if (!parent) return PURE_STORE_ERROR;
        size_t length = strlen(parent);
        while (length > 1 && parent[length - 1] == '/') parent[--length] = 0;
        char *slash = strrchr(parent, '/');
        if (!slash) strcpy(parent, ".");
        else if (slash == parent) parent[1] = 0;
        else *slash = 0;
        int fd = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        free(parent);
        ok = fd >= 0 && sync_fd(fd);
        if (fd >= 0) close(fd);
    }
    return ok ? PURE_OK : PURE_STORE_ERROR;
}

void pure_store_release(PureStoredView *view) {
    if (view->mapped_size) munmap(view->mapping, view->mapped_size);
    *view = (PureStoredView){0};
}

PureResult pure_store_select(const char *path, const char *key, size_t offset,
                              size_t length, bool whole, size_t minimum,
                              size_t available, PureStoredView *out) {
    *out = (PureStoredView){0};
    char name[72];
    if (!file_name(key, name)) return PURE_STORE_ERROR;
    int directory = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (directory < 0) return PURE_STORE_ERROR;
    int fd = openat(directory, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    int cause = errno;
    close(directory);
    if (fd < 0) return cause == ENOENT ? PURE_NOT_FOUND : PURE_STORE_ERROR;
    int64_t revision;
    size_t size;
    PureResult result = PURE_STORE_ERROR;
    if (!read_header(fd, &revision, &size)) goto finish;
    if (size < minimum) { result = PURE_LIMIT; goto finish; }
    if (offset > size) { result = PURE_LIMIT; goto finish; }
    if (whole) length = size - offset;
    if (offset > size || length > size - offset) { result = PURE_LIMIT; goto finish; }
    if (length > available) { result = PURE_BUDGET; goto finish; }
    if (length) {
        long page = sysconf(_SC_PAGESIZE);
        if (page <= 0) goto finish;
        size_t start = HEADER_BYTES + offset;
        size_t aligned = start - start % (size_t)page;
        size_t prefix = start - aligned;
        size_t mapped_size = prefix + length;
        void *mapping = mmap(NULL, mapped_size, PROT_READ, MAP_PRIVATE, fd, (off_t)aligned);
        if (mapping == MAP_FAILED) goto finish;
        out->mapping = mapping;
        out->mapped_size = mapped_size;
        out->body = (PureSpan){(const unsigned char *)mapping + prefix, length};
    }
    out->revision = revision;
    out->object_size = size;
    result = PURE_OK;
finish:
    close(fd); /* The mapping retains this immutable file version after rename. */
    return result;
}

PureResult pure_store_get(const char *path, const char *key, size_t available, PureStoredView *out) {
    return pure_store_select(path, key, 0, 0, true, 0, available, out);
}

PureResult pure_store_get_range(const char *path, const char *key, size_t offset,
                                size_t length, size_t available, PureStoredView *out) {
    return pure_store_select(path, key, offset, length, false, 0, available, out);
}

PureResult pure_store_put(const char *path, const char *key, int64_t expected,
                          PureSpan body, int64_t *revision) {
    *revision = 0;
    char name[72], temporary[96] = "";
    if (!file_name(key, name) || (body.size && !body.data)) return PURE_STORE_ERROR;
    if (body.size > PURE_OBJECT_LIMIT || expected < 0) return PURE_LIMIT;
    int directory = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (directory < 0) return PURE_STORE_ERROR;
    int lock = openat(directory, ".writer.lock", O_RDWR | O_NOFOLLOW | O_CLOEXEC);
    int fd = -1;
    PureResult result = PURE_STORE_ERROR;
    struct stat info;
    if (lock < 0 || fstat(lock, &info) || !S_ISREG(info.st_mode)) goto finish;
    struct flock hold = {.l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 0, .l_len = 0};
    if (fcntl(lock, F_SETLK, &hold)) {
        if (errno == EACCES || errno == EAGAIN) result = PURE_BUSY;
        goto finish;
    }
    fd = openat(directory, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd >= 0) {
        size_t size;
        if (!read_header(fd, revision, &size)) goto finish;
        close(fd);
        fd = -1;
    } else if (errno != ENOENT) goto finish;
    if (*revision != expected) { result = PURE_CONFLICT; goto finish; }
    if (*revision == INT64_MAX) { result = PURE_LIMIT; goto finish; }
    for (unsigned attempt = 0; attempt < 64; ++attempt) {
        if (temporary_sequence == UINT64_MAX) goto finish;
        snprintf(temporary, sizeof(temporary), ".pending-%ld-%" PRIu64, (long)getpid(), ++temporary_sequence);
        fd = openat(directory, temporary, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd >= 0) break;
        temporary[0] = 0; /* Never remove an existing file on a name collision. */
        if (errno != EEXIST) goto finish;
    }
    if (fd < 0) goto finish;
    unsigned char header[HEADER_BYTES];
    memcpy(header, magic, sizeof(magic));
    encode64(header + 8, (uint64_t)*revision + 1);
    encode64(header + 16, body.size);
    if (!write_bytes(fd, header, sizeof(header)) || fault(PURE_AFTER_HEADER) ||
        !write_bytes(fd, body.data, body.size) || fault(PURE_BEFORE_FILE_SYNC) || !sync_fd(fd)) goto finish;
    int close_result = close(fd);
    fd = -1;
    if (close_result || fault(PURE_BEFORE_RENAME) || renameat(directory, temporary, directory, name)) goto finish;
    temporary[0] = 0; /* Published: a later sync failure does not roll it back. */
    ++*revision;
    result = fault(PURE_BEFORE_DIR_SYNC) || !sync_fd(directory) ? PURE_UNCERTAIN : PURE_OK;
finish:
    if (fd >= 0) close(fd);
    if (*temporary) unlinkat(directory, temporary, 0);
    if (lock >= 0) close(lock);
    close(directory);
    return result;
}
