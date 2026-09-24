#define _DEFAULT_SOURCE
#include "store.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

/* Selected Linux storage experiment with 16 KiB logical pages. Small values retain the
 * existing PUREB001 inline file. Large roots name an append-only page
 * generation. Both forms expose one immutable, contiguous mapped read. */
enum { PAGE = 16384, INLINE_BYTES = 24, ROOT_BYTES = 48,
       MAX_PAGES = (PURE_OBJECT_LIMIT + PAGE - 1) / PAGE };
static const unsigned char magic[8] = {'P','U','R','E','P','0','0','1'};
static const unsigned char inline_magic[8] = {'P','U','R','E','B','0','0','1'};
static uint64_t temporary_sequence;

typedef struct {
    int64_t revision;
    size_t size;
    uint64_t generation;
    uint64_t refs[MAX_PAGES];
    bool inline_data;
} Root;

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

static void segment_name(const char *key, uint64_t generation, char name[96]) {
    snprintf(name, 96, "dat-%s-%016" PRIx64, key, generation);
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
    int result;
    do { result = fsync(fd); } while (result && errno == EINTR);
    return result == 0;
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

static bool read_bytes(int fd, unsigned char *bytes, size_t length, off_t offset) {
    while (length) {
        ssize_t n = pread(fd, bytes, length, offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        bytes += (size_t)n;
        length -= (size_t)n;
        offset += n;
    }
    return true;
}

static size_t pages(size_t size) { return (size + PAGE - 1) / PAGE; }

static size_t segment_limit(size_t size) {
    size_t limit = 2 * pages(size) * PAGE;
#ifdef PURE_STORE_TEST
    if (limit > 6 * PAGE) limit = 6 * PAGE;
#else
    if (limit < 8 * PAGE) limit = 8 * PAGE;
#endif
    return limit;
}

static bool read_root(int fd, Root *root) {
    struct stat info;
    unsigned char header[ROOT_BYTES];
    if (fstat(fd, &info) || !S_ISREG(info.st_mode) ||
        !read_bytes(fd, header, INLINE_BYTES, 0)) return false;
    uint64_t revision = decode64(header + 8), size = decode64(header + 16);
    if (!revision || revision > INT64_MAX || size > PURE_OBJECT_LIMIT) return false;
    root->revision = (int64_t)revision;
    root->size = (size_t)size;
    if (!memcmp(header, inline_magic, sizeof(inline_magic))) {
        if (info.st_size != (off_t)(INLINE_BYTES + size)) return false;
        root->inline_data = true;
        root->generation = 0;
        return true;
    }
    if (memcmp(header, magic, sizeof(magic)) ||
        !read_bytes(fd, header + INLINE_BYTES, ROOT_BYTES - INLINE_BYTES, INLINE_BYTES)) return false;
    uint64_t generation = decode64(header + 24), page = decode64(header + 32);
    uint64_t count = decode64(header + 40);
    if (!generation || page != PAGE || count != pages((size_t)size) ||
        info.st_size != (off_t)(ROOT_BYTES + count * 8)) return false;
    root->inline_data = false;
    root->generation = generation;
    unsigned char bytes[MAX_PAGES * 8];
    if (count && !read_bytes(fd, bytes, (size_t)count * 8, ROOT_BYTES)) return false;
    for (size_t i = 0; i < (size_t)count; ++i) root->refs[i] = decode64(bytes + i * 8);
    return true;
}

static int open_segment(int directory, const char *key, const Root *root, int flags) {
    char name[96];
    segment_name(key, root->generation, name);
    int fd = openat(directory, name, flags | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) return -1;
    struct stat info;
    if (fstat(fd, &info) || !S_ISREG(info.st_mode) || info.st_size < 0) goto bad;
    for (size_t i = 0; i < pages(root->size); ++i)
        if (root->refs[i] > (uint64_t)INT64_MAX / PAGE ||
            (uint64_t)info.st_size / PAGE <= root->refs[i]) goto bad;
    return fd;
bad:
    close(fd);
    return -1;
}

static PureResult open_locked(const char *path, bool writer, int *directory, int *lock) {
    *directory = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (*directory < 0) return PURE_STORE_ERROR;
    *lock = openat(*directory, ".writer.lock",
                   (writer ? O_RDWR : O_RDONLY) | O_NOFOLLOW | O_CLOEXEC);
    struct stat info;
    if (*lock < 0 || fstat(*lock, &info) || !S_ISREG(info.st_mode)) return PURE_STORE_ERROR;
    struct flock hold = {.l_type = writer ? F_WRLCK : F_RDLCK, .l_whence = SEEK_SET,
                         .l_start = 0, .l_len = 0};
    int rc;
    do { rc = fcntl(*lock, writer ? F_SETLK : F_SETLKW, &hold); }
    while (rc && errno == EINTR && !writer);
    if (rc) return writer && (errno == EACCES || errno == EAGAIN) ? PURE_BUSY : PURE_STORE_ERROR;
    return PURE_OK;
}

PureResult pure_store_init(const char *path) {
    long host_page = sysconf(_SC_PAGESIZE);
    /* Every file offset and MAP_FIXED address uses a PAGE-sized step, so a
     * smaller OS page works when its size divides the logical page. */
    if (!path || !*path || host_page <= 0 || PAGE % host_page)
        return PURE_STORE_ERROR;
    bool created = mkdir(path, 0700) == 0;
    if (!created && errno != EEXIST) return PURE_STORE_ERROR;
    int directory = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (directory < 0) return PURE_STORE_ERROR;
    int lock = openat(directory, ".writer.lock", O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    struct stat info;
    bool ok = lock >= 0 && !fstat(lock, &info) && S_ISREG(info.st_mode) && sync_fd(lock);
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
    int directory = -1, lock = -1, head = -1, data = -1;
    PureResult result = open_locked(path, false, &directory, &lock);
    if (result != PURE_OK) goto finish;
    head = openat(directory, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (head < 0) { result = errno == ENOENT ? PURE_NOT_FOUND : PURE_STORE_ERROR; goto finish; }
    Root root;
    if (!read_root(head, &root)) { result = PURE_STORE_ERROR; goto finish; }
    if (root.size < minimum || offset > root.size) { result = PURE_LIMIT; goto finish; }
    if (whole) length = root.size - offset;
    if (length > root.size - offset) { result = PURE_LIMIT; goto finish; }
    if (length > available) { result = PURE_BUDGET; goto finish; }
    if (root.inline_data) {
        if (length) {
            size_t start = INLINE_BYTES + offset;
            size_t aligned = start - start % PAGE;
            size_t prefix = start - aligned;
            size_t mapped_size = prefix + length;
            void *mapped = mmap(NULL, mapped_size, PROT_READ, MAP_PRIVATE, head, (off_t)aligned);
            if (mapped == MAP_FAILED) { result = PURE_STORE_ERROR; goto finish; }
            out->mapping = mapped;
            out->mapped_size = mapped_size;
            out->body = (PureSpan){(const unsigned char *)mapped + prefix, length};
        }
    } else {
        data = open_segment(directory, key, &root, O_RDONLY);
        if (data < 0) { result = PURE_STORE_ERROR; goto finish; }
        if (length) {
            size_t first = offset / PAGE, count = pages(offset % PAGE + length);
            size_t mapped_size = count * PAGE;
            void *base = mmap(NULL, mapped_size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (base == MAP_FAILED) { result = PURE_STORE_ERROR; goto finish; }
            bool valid = true;
            for (size_t at = 0; at < count;) {
                size_t run = 1;
                while (at + run < count && root.refs[first + at + run] ==
                       root.refs[first + at] + run) ++run;
                void *address = (unsigned char *)base + at * PAGE;
                void *mapped = mmap(address, run * PAGE, PROT_READ, MAP_PRIVATE | MAP_FIXED,
                                    data, (off_t)(root.refs[first + at] * PAGE));
                if (mapped != address) { valid = false; break; }
                at += run;
            }
            if (!valid) { munmap(base, mapped_size); result = PURE_STORE_ERROR; goto finish; }
            out->mapping = base;
            out->mapped_size = mapped_size;
            out->body = (PureSpan){(const unsigned char *)base + offset % PAGE, length};
        }
    }
    out->revision = root.revision;
    out->object_size = root.size;
    result = PURE_OK;
finish:
    if (data >= 0) close(data);
    if (head >= 0) close(head);
    if (lock >= 0) close(lock);
    if (directory >= 0) close(directory);
    return result;
}

PureResult pure_store_get(const char *path, const char *key, size_t available, PureStoredView *out) {
    return pure_store_select(path, key, 0, 0, true, 0, available, out);
}

PureResult pure_store_get_range(const char *path, const char *key, size_t offset,
                                size_t length, size_t available, PureStoredView *out) {
    return pure_store_select(path, key, offset, length, false, 0, available, out);
}

static bool read_logical(int data, const Root *root, size_t offset,
                         unsigned char *destination, size_t length) {
    if (root->inline_data)
        return read_bytes(data, destination, length, (off_t)(INLINE_BYTES + offset));
    while (length) {
        size_t page = offset / PAGE, inside = offset % PAGE;
        size_t n = PAGE - inside < length ? PAGE - inside : length;
        if (!read_bytes(data, destination, n, (off_t)(root->refs[page] * PAGE + inside))) return false;
        destination += n;
        offset += n;
        length -= n;
    }
    return true;
}

static bool copy_logical(int source, const Root *root, int destination,
                         size_t offset, size_t length) {
    unsigned char scratch[4096];
    while (length) {
        size_t n = length < sizeof(scratch) ? length : sizeof(scratch);
        if (!read_logical(source, root, offset, scratch, n) ||
            !write_bytes(destination, scratch, n)) return false;
        offset += n;
        length -= n;
    }
    return true;
}

static bool write_full_pages(int old_data, const Root *old, int destination,
                             bool splice, size_t offset, size_t remove, PureSpan body,
                             Root *next) {
    size_t count = pages(next->size);
    for (size_t page = 0; page < count; ++page) {
        unsigned char bytes[PAGE] = {0};
        size_t start = page * PAGE, end = start + PAGE;
        if (end > next->size) end = next->size;
        size_t at = start;
        while (at < end) {
            size_t n;
            if (splice && at < offset) {
                n = offset - at < end - at ? offset - at : end - at;
                if (!read_logical(old_data, old, at, bytes + at - start, n)) return false;
            } else if (at < offset + body.size || !splice) {
                size_t source = splice ? at - offset : at;
                n = body.size - source < end - at ? body.size - source : end - at;
                memcpy(bytes + at - start, body.data + source, n);
            } else {
                n = end - at;
                if (!read_logical(old_data, old, at - body.size + remove,
                                  bytes + at - start, n)) return false;
            }
            at += n;
        }
        if (!write_bytes(destination, bytes, PAGE)) return false;
        next->refs[page] = page;
    }
    return true;
}

static bool append_patch(int data, const Root *old, Root *next,
                         size_t offset, PureSpan insert) {
    if (!insert.size) return true;
    struct stat info;
    if (fstat(data, &info) || info.st_size < 0 ||
        (uint64_t)info.st_size > INT64_MAX - PAGE) return false;
    off_t start = (info.st_size + PAGE - 1) / PAGE * PAGE;
    if (ftruncate(data, start) || lseek(data, start, SEEK_SET) != start) return false;
    size_t first = offset / PAGE, end = pages(offset + insert.size);
    for (size_t page = first; page < end; ++page) {
        unsigned char bytes[PAGE];
        if (!read_bytes(data, bytes, PAGE, (off_t)(old->refs[page] * PAGE))) return false;
        size_t begin = page * PAGE < offset ? offset : page * PAGE;
        size_t limit = (page + 1) * PAGE > offset + insert.size ?
                       offset + insert.size : (page + 1) * PAGE;
        memcpy(bytes + begin - page * PAGE, insert.data + begin - offset, limit - begin);
        if (!write_bytes(data, bytes, PAGE)) return false;
        next->refs[page] = (uint64_t)start / PAGE + page - first;
    }
    return sync_fd(data);
}

static bool publish_root(int directory, const char *name, Root *next, int64_t *revision,
                         bool *published, int old_data, const Root *old,
                         bool splice, size_t offset, size_t remove, PureSpan body) {
    char temporary[96] = "";
    int fd = -1;
    for (unsigned attempt = 0; attempt < 64; ++attempt) {
        if (temporary_sequence == UINT64_MAX) break;
        snprintf(temporary, sizeof(temporary), ".pending-%ld-%" PRIu64,
                 (long)getpid(), ++temporary_sequence);
        fd = openat(directory, temporary, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd >= 0) break;
        temporary[0] = 0;
        if (errno != EEXIST) break;
    }
    if (fd < 0) return false;
    unsigned char bytes[ROOT_BYTES + MAX_PAGES * 8];
    memcpy(bytes, next->inline_data ? inline_magic : magic, 8);
    encode64(bytes + 8, (uint64_t)next->revision);
    encode64(bytes + 16, next->size);
    bool ok;
    if (next->inline_data) {
        ok = write_bytes(fd, bytes, INLINE_BYTES) && !fault(PURE_AFTER_HEADER);
        if (ok && splice)
            ok = copy_logical(old_data, old, fd, 0, offset) &&
                 write_bytes(fd, body.data, body.size) &&
                 copy_logical(old_data, old, fd, offset + remove,
                              old->size - offset - remove);
        else if (ok) ok = write_bytes(fd, body.data, body.size);
    } else {
        encode64(bytes + 24, next->generation);
        encode64(bytes + 32, PAGE);
        encode64(bytes + 40, pages(next->size));
        for (size_t i = 0; i < pages(next->size); ++i)
            encode64(bytes + ROOT_BYTES + i * 8, next->refs[i]);
        ok = write_bytes(fd, bytes, ROOT_BYTES) && !fault(PURE_AFTER_HEADER) &&
             write_bytes(fd, bytes + ROOT_BYTES, pages(next->size) * 8);
    }
    if (ok) ok = !fault(PURE_BEFORE_FILE_SYNC) && sync_fd(fd);
    if (close(fd)) ok = false;
    if (ok) ok = !fault(PURE_BEFORE_RENAME) && !renameat(directory, temporary, directory, name);
    if (ok) {
        *published = true;
        *revision = next->revision;
        temporary[0] = 0;
        ok = !fault(PURE_BEFORE_DIR_SYNC) && sync_fd(directory);
    }
    if (*temporary) unlinkat(directory, temporary, 0);
    return ok;
}

static void collect_old_segments(int directory, const char *key, uint64_t current) {
    char prefix[72], retained[96];
    snprintf(prefix, sizeof(prefix), "dat-%s-", key);
    segment_name(key, current, retained);
    int copy = dup(directory);
    if (copy < 0) return;
    DIR *entries = fdopendir(copy);
    if (!entries) { close(copy); return; }
    bool removed = false;
    struct dirent *entry;
    size_t prefix_size = strlen(prefix);
    while ((entry = readdir(entries))) {
        if (!strncmp(entry->d_name, ".pending-", 9)) {
            if (!unlinkat(directory, entry->d_name, 0)) removed = true;
            continue;
        }
        if (strncmp(entry->d_name, prefix, prefix_size) ||
            strlen(entry->d_name + prefix_size) != 16 ||
            !strcmp(entry->d_name, retained)) continue;
        bool hexadecimal = true;
        for (const char *p = entry->d_name + prefix_size; *p; ++p)
            if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f'))) hexadecimal = false;
        if (hexadecimal && !unlinkat(directory, entry->d_name, 0)) removed = true;
    }
    closedir(entries);
    if (removed) sync_fd(directory);
}

static PureResult mutate(const char *path, const char *key, int64_t expected,
                          PureSpan body, bool splice, size_t offset, size_t remove,
                          size_t max_size, int64_t *revision) {
    *revision = 0;
    char name[72], new_segment[96] = "";
    if (!file_name(key, name) || (body.size && !body.data)) return PURE_STORE_ERROR;
    if (body.size > PURE_OBJECT_LIMIT || max_size > PURE_OBJECT_LIMIT || expected < 0) return PURE_LIMIT;
    int directory = -1, lock = -1, head = -1, old_data = -1, new_data = -1;
    Root old = {0}, next = {0};
    bool published = false, reused_segment = false, owned_segment = false;
    struct stat info;
    PureResult result = open_locked(path, true, &directory, &lock);
    if (result != PURE_OK) goto finish;
    result = PURE_STORE_ERROR;
    head = openat(directory, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (head >= 0) {
        if (!read_root(head, &old)) goto finish;
        old_data = old.inline_data ? dup(head) : open_segment(directory, key, &old, O_RDWR);
        if (old_data < 0) goto finish;
    } else if (errno != ENOENT) goto finish;
    *revision = old.revision;
    if (expected != old.revision) { result = PURE_CONFLICT; goto finish; }
    if (old.revision == INT64_MAX) { result = PURE_LIMIT; goto finish; }
    size_t result_size = body.size;
    if (splice) {
        if (offset > old.size || remove > old.size - offset ||
            old.size - remove > max_size ||
            body.size > max_size - (old.size - remove)) { result = PURE_LIMIT; goto finish; }
        result_size = old.size - remove + body.size;
    }
    next = old;
    next.revision = old.revision + 1;
    next.size = result_size;
    next.inline_data = result_size <= PAGE;
    if (next.inline_data) next.generation = 0;
    bool patch = old.revision && !old.inline_data && !next.inline_data &&
                 splice && remove == body.size && result_size == old.size;
    /* A small value retains the existing PUREB001 24-byte header. */
    if (!next.inline_data && patch && fstat(old_data, &info) == 0 &&
               info.st_size < (off_t)segment_limit(old.size)) {
        reused_segment = true;
        if (!append_patch(old_data, &old, &next, offset, body)) goto finish;
    } else if (!next.inline_data) {
        uint64_t candidate = old.generation;
        for (unsigned attempt = 0; attempt < 64; ++attempt) {
            if (candidate == UINT64_MAX) goto finish;
            segment_name(key, ++candidate, new_segment);
            new_data = openat(directory, new_segment,
                              O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
            if (new_data >= 0) { owned_segment = true; break; }
            if (errno != EEXIST) goto finish;
        }
        if (new_data < 0) goto finish;
        next.generation = candidate;
        if (!write_full_pages(old_data, &old, new_data, splice, offset, remove, body, &next) ||
            !sync_fd(new_data)) goto finish;
        /* A new data file's directory entry must survive before the root
         * can durably name it. */
        if (!sync_fd(directory)) goto finish;
    }
    if (!publish_root(directory, name, &next, revision, &published,
                      old_data, &old, splice, offset, remove, body)) {
        result = published ? PURE_UNCERTAIN : PURE_STORE_ERROR;
        goto finish;
    }
    result = PURE_OK;
    /* Readers have mapped each old inode before releasing their shared lock.
     * A failed cleanup leaks space but cannot invalidate an existing view. */
    /* An inline-only edit creates no segments. Avoid a directory-wide scan
     * on the small-object hot path. */
    if (!reused_segment && (old.generation || next.generation))
        collect_old_segments(directory, key, next.generation);
finish:
    if (new_data >= 0) close(new_data);
    if (old_data >= 0) close(old_data);
    if (head >= 0) close(head);
    if (result != PURE_OK && result != PURE_UNCERTAIN && owned_segment)
        unlinkat(directory, new_segment, 0);
    if (lock >= 0) close(lock);
    if (directory >= 0) close(directory);
    return result;
}

PureResult pure_store_put(const char *path, const char *key, int64_t expected,
                          PureSpan body, int64_t *revision) {
    return mutate(path, key, expected, body, false, 0, 0, PURE_OBJECT_LIMIT, revision);
}

PureResult pure_store_splice(const char *path, const char *key, int64_t expected,
                             size_t offset, size_t remove, PureSpan insert,
                             size_t max_size, int64_t *revision) {
    return mutate(path, key, expected, insert, true, offset, remove, max_size, revision);
}
