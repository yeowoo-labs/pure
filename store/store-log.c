/* The app log and list images (core/log.h).
 *
 * The log's entries go into segments, files log-0, log-1, ... in the store's
 * directory, each allocated whole when made, so that an append neither grows
 * a file nor takes new blocks and its sync writes the entry alone, and used
 * again once every entry in it is released. A segment begins with a header
 * naming its generation (higher than any before it) and the generation of the
 * segment it continues; an entry is its save's number, the number and digest
 * of the entry before it (0 for the first), its payload's length and flags,
 * a digest of those and the payload, then the payload, padded to 8 bytes.
 * Opening reads each segment's entries while each names the one before it,
 * and chains the segments from the newest back by the generations and entries
 * they name: a torn entry, or bytes a segment kept from an earlier use, end
 * what is read. Save numbers may skip (a save that changed no list writes no
 * entry) and are used again (for an entry taken back), so an entry is known
 * by the digest of the one it names, not by numbers alone.
 *
 * An image is written to a temporary file in 256 KiB steps, each started to
 * the device as the next is written, then synced and renamed over the image
 * it replaces: a writer never has more than two steps waiting, so the log's
 * syncs between them wait for little. */
#if defined(__linux__)
#define _GNU_SOURCE
#else
#define _POSIX_C_SOURCE 200809L
#endif
#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif
#include "log.h"
#include "store.h"
#include "digest.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

enum { HEADER = 64, ENTRY = 40, SEGMENTS = 256, STEP = 256 << 10, TREE = 1 };
#ifdef PURE_STORE_TEST
enum { SEGMENT = 16384 };
#else
enum { SEGMENT = 8 << 20 };
#endif
static const unsigned char log_magic[8] = {'P', 'U', 'R', 'E', 'L', 'O', 'G', '1'};
static const unsigned char image_magic[8] = {'P', 'U', 'R', 'E', 'I', 'M', 'G', '1'};

static bool fault(PureStoreStage stage) {
#ifdef PURE_STORE_TEST
    if (pure_store_test_fault(stage)) { errno = EIO; return true; }
#else
    (void)stage;
#endif
    return false;
}

static uint64_t get64(const unsigned char *bytes) {
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) value |= (uint64_t)bytes[i] << (8 * i);
    return value;
}

static void put64(unsigned char *bytes, uint64_t value) {
    for (unsigned i = 0; i < 8; ++i) bytes[i] = (unsigned char)(value >> (8 * i));
}

static bool write_all(int fd, const unsigned char *bytes, size_t length, uint64_t offset) {
    while (length) {
        ssize_t n = pwrite(fd, bytes, length, (off_t)offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        bytes += (size_t)n;
        length -= (size_t)n;
        offset += (uint64_t)n;
    }
    return true;
}

static bool sync_whole(int fd) {
    int rc;
    do { rc = fsync(fd); } while (rc && errno == EINTR);
    return rc == 0;
}

static bool sync_data(int fd) {
#ifdef __APPLE__
    return sync_whole(fd);
#else
    int rc;
    do { rc = fdatasync(fd); } while (rc && errno == EINTR);
    return rc == 0;
#endif
}

/* Room for size bytes from the start of a file, allocated. */
static bool allocate(int fd, uint64_t size) {
#if defined(__APPLE__)
    return !ftruncate(fd, (off_t)size);
#else
    int rc = posix_fallocate(fd, 0, (off_t)size);
    return rc == 0 || ((rc == EOPNOTSUPP || rc == EINVAL) && !ftruncate(fd, (off_t)size));
#endif
}

/* ---- The log */

typedef struct {
    int fd;
    uint64_t generation;  /* 0: no header */
    uint64_t predecessor; /* the generation of the segment it continues (0: none) */
    uint64_t size;        /* bytes allocated */
    /* Its entries (first 0: none), the entry named before the first (and its
     * digest), the last that counts (and its digest) and where that one ends. */
    uint64_t first, previous, named, last, digest, end;
} Segment;

struct PureLog {
    int directory;
    Segment segment[SEGMENTS];
    unsigned count;
    int current;         /* the segment appended to, or -1 */
    uint64_t last, digest; /* the last entry's number and digest */
    uint64_t generation; /* the highest any header has had */
    uint64_t released;
    /* The segments as opened, mapped until the first append (a start reads
     * them; what is appended after is never read in the same run). */
    unsigned char *maps[SEGMENTS];
    size_t mapped[SEGMENTS];
    /* The last append, to take back: its segment as it was and the last entry before it. */
    int appended;
    Segment before;
    uint64_t last_before, digest_before;
    unsigned char *buffer;
    size_t capacity;
};

static uint64_t entry_digest(const unsigned char *head, const unsigned char *payload, size_t size) {
    Digest digest = {0};
    digest_add(&digest, head, ENTRY - 8);
    digest_add(&digest, payload, size);
    return digest_end(&digest);
}

static size_t padded(size_t size) { return (size + 7) & ~(size_t)7; }
static uint32_t entry_length(const unsigned char *head) { return (uint32_t)get64(head + 16); }

/* The entries of a mapped segment from its header on, each naming the one
 * before: the first (and the entry it names), the last, and their end. */
static void scan(Segment *s, const unsigned char *map, size_t size) {
    s->first = s->previous = s->named = s->last = s->digest = 0;
    s->end = HEADER;
    uint64_t at = HEADER;
    while (at + ENTRY <= size) {
        const unsigned char *head = map + at;
        uint64_t seq = get64(head), previous = get64(head + 8), named = get64(head + 24);
        uint32_t length = entry_length(head);
        if (!seq || (s->first && (previous != s->last || named != s->digest || seq <= s->last)) ||
            (!s->first && previous >= seq) || length > size - at - ENTRY ||
            get64(head + 32) != entry_digest(head, head + ENTRY, length))
            break;
        if (!s->first) s->first = seq, s->previous = previous, s->named = named;
        s->last = seq;
        s->digest = get64(head + 32);
        at += ENTRY + padded(length);
        s->end = at;
    }
}

/* Where entry seq of a segment as opened ends (0: not among its entries),
 * and its digest. */
static uint64_t end_of(const PureLog *log, unsigned i, uint64_t seq, uint64_t *digest) {
    const Segment *s = &log->segment[i];
    const unsigned char *map = log->maps[i];
    if (!map || !s->first || seq < s->first || seq > s->last) return 0;
    for (uint64_t at = HEADER; at < s->end;) {
        const unsigned char *head = map + at;
        at += ENTRY + padded(entry_length(head));
        if (get64(head) == seq) {
            *digest = get64(head + 32);
            return at;
        }
    }
    return 0;
}

static int generation_at(const PureLog *log, uint64_t generation) {
    for (unsigned i = 0; generation && i < log->count; ++i)
        if (log->segment[i].generation == generation) return (int)i;
    return -1;
}

/* Zeros over an entry's header: what follows it is no longer read. */
static bool erase(const Segment *s, uint64_t at) {
    static const unsigned char zeros[ENTRY];
    return at + ENTRY > s->size || (write_all(s->fd, zeros, ENTRY, at) && sync_data(s->fd));
}

static void unmap(PureLog *log) {
    for (unsigned i = 0; i < log->count; ++i)
        if (log->maps[i]) {
            munmap(log->maps[i], log->mapped[i]);
            log->maps[i] = NULL;
        }
}

PureResult pure_log_open(const char *directory, PureLog **out) {
    *out = NULL;
    PureLog *log = calloc(1, sizeof(*log));
    if (!log) return PURE_ALLOCATION;
    log->current = log->appended = -1;
    log->directory = open(directory, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (log->directory < 0) { free(log); return PURE_STORE_ERROR; }
    PureResult result = PURE_OK;
    for (unsigned n = 0; n < SEGMENTS; ++n) {
        char name[24];
        snprintf(name, sizeof(name), "log-%u", n);
        int fd = openat(log->directory, name, O_RDWR | O_NOFOLLOW | O_CLOEXEC);
        if (fd < 0) {
            if (errno != ENOENT) result = PURE_STORE_ERROR;
            break;
        }
        struct stat file;
        Segment *s = &log->segment[log->count++];
        *s = (Segment){.fd = fd, .end = HEADER};
        if (fstat(fd, &file)) { result = PURE_STORE_ERROR; break; }
        s->size = (uint64_t)file.st_size;
        if (s->size < HEADER) continue;
        void *map = mmap(NULL, (size_t)s->size, PROT_READ, MAP_SHARED, fd, 0);
        if (map == MAP_FAILED) { result = PURE_STORE_ERROR; break; }
        log->maps[log->count - 1] = map;
        log->mapped[log->count - 1] = (size_t)s->size;
        const unsigned char *header = map;
        if (memcmp(header, log_magic, 8) || get64(header + HEADER - 8) != digest_of(header, HEADER - 8)) continue;
        s->generation = get64(header + 8);
        s->predecessor = get64(header + 16);
        if (s->generation > log->generation) log->generation = s->generation;
        scan(s, map, (size_t)s->size);
    }
    if (result != PURE_OK) { pure_log_close(log); return result; }
    /* The newest segment with entries holds the last; from it back, each
     * segment names the one it continues, which counts up to the entry the
     * later one names first. The rest hold nothing the log still has. */
    int top = -1;
    for (unsigned i = 0; i < log->count; ++i)
        if (log->segment[i].first && (top < 0 || log->segment[i].generation > log->segment[top].generation)) top = (int)i;
    bool chained[SEGMENTS] = {false};
    for (int at = top, later = -1; at >= 0; later = at, at = generation_at(log, log->segment[at].predecessor)) {
        Segment *s = &log->segment[at];
        if (chained[at] || s->generation >= (later >= 0 ? log->segment[later].generation : UINT64_MAX)) break;
        if (later >= 0) { /* it counts up to the entry the later segment names */
            uint64_t digest, ends = end_of(log, (unsigned)at, log->segment[later].previous, &digest);
            if (!ends || digest != log->segment[later].named) break; /* released: known from the later segment on */
            s->last = log->segment[later].previous;
            s->digest = digest;
            s->end = ends;
        }
        chained[at] = true;
        if (!s->previous) break;
    }
    for (unsigned i = 0; i < log->count; ++i)
        if (!chained[i]) {
            Segment *s = &log->segment[i];
            s->first = s->previous = s->last = 0;
            s->end = HEADER;
        }
    if (top >= 0) {
        log->current = top;
        log->last = log->segment[top].last;
        log->digest = log->segment[top].digest;
    }
    *out = log;
    return PURE_OK;
}

void pure_log_close(PureLog *log) {
    if (!log) return;
    unmap(log);
    for (unsigned i = 0; i < log->count; ++i) close(log->segment[i].fd);
    close(log->directory);
    free(log->buffer);
    free(log);
}

uint64_t pure_log_last(const PureLog *log) { return log->last; }

uint64_t pure_log_bytes(const PureLog *log) {
    uint64_t bytes = 0;
    for (unsigned i = 0; i < log->count; ++i) {
        const Segment *s = &log->segment[i];
        if (s->first && ((int)i == log->current || s->last > log->released)) bytes += s->end;
    }
    return bytes;
}

PureResult pure_log_read(PureLog *log, uint64_t from,
                         bool (*visit)(void *, uint64_t seq, bool tree, PureSpan payload), void *context) {
    if (log->current < 0 || log->last < from) return PURE_OK;
    int chain[SEGMENTS];
    unsigned links = 0;
    for (int at = log->current; at >= 0 && links < SEGMENTS;) {
        const Segment *s = &log->segment[at];
        if (!s->first) break;
        chain[links++] = at;
        if (s->first <= from || !s->previous) break;
        at = generation_at(log, s->predecessor);
    }
    if (!links) return PURE_OK;
    const Segment *oldest = &log->segment[chain[links - 1]];
    if (from && oldest->first > from && oldest->previous >= from) return PURE_STORE_ERROR; /* entries from `from` on are gone */
    while (links--) {
        const unsigned char *map = log->maps[chain[links]];
        const Segment *s = &log->segment[chain[links]];
        for (uint64_t at = HEADER; map && at < s->end;) {
            const unsigned char *head = map + at;
            uint64_t seq = get64(head);
            if (seq >= from && !visit(context, seq, (get64(head + 16) >> 32) & TREE, (PureSpan){head + ENTRY, entry_length(head)}))
                return PURE_OK;
            at += ENTRY + padded(entry_length(head));
        }
    }
    return PURE_OK;
}

void pure_log_end(PureLog *log, uint64_t seq) {
    if (seq >= log->last) return;
    /* The chained segment holding seq keeps its entries up to it; after it,
     * there and in every newer segment, the device is erased too, so that
     * opening again finds none of them. */
    int holder = -1;
    for (int at = log->current; at >= 0; at = generation_at(log, log->segment[at].predecessor)) {
        const Segment *s = &log->segment[at];
        if (!s->first) break;
        if (s->first <= seq && seq <= s->last) { holder = at; break; }
        if (!s->previous) break;
    }
    uint64_t kept = holder >= 0 ? log->segment[holder].generation : 0, digest = 0;
    for (unsigned i = 0; i < log->count; ++i) {
        Segment *s = &log->segment[i];
        if ((int)i == holder) {
            s->end = end_of(log, i, seq, &digest);
            s->last = seq;
            s->digest = digest;
            erase(s, s->end);
        } else if (s->generation > kept) {
            erase(s, HEADER);
            s->first = s->previous = s->last = 0;
            s->end = HEADER;
        }
    }
    log->current = holder;
    log->last = seq;
    log->digest = digest;
}

/* A segment to begin: one holding no entry still counted, else a new file. */
static int fresh_segment(PureLog *log, uint64_t needed) {
    int pick = -1;
    for (unsigned i = 0; i < log->count; ++i) {
        const Segment *s = &log->segment[i];
        if ((int)i == log->current || (s->first && s->last > log->released)) continue;
        if (pick < 0 || s->size > log->segment[pick].size) pick = (int)i;
    }
    if (pick >= 0) {
        Segment *s = &log->segment[pick];
        if (s->size < needed && !allocate(s->fd, needed)) return -1;
        if (s->size < needed) s->size = needed;
        return pick;
    }
    if (log->count == SEGMENTS) return -1;
    char name[24];
    snprintf(name, sizeof(name), "log-%u", log->count);
    int fd = openat(log->directory, name, O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    uint64_t size = needed > SEGMENT ? needed : SEGMENT;
    /* The new file and its name are durable before an entry in it counts. */
    if (fd < 0 || !allocate(fd, size) || !sync_whole(fd) || !sync_whole(log->directory)) {
        if (fd >= 0) {
            close(fd);
            unlinkat(log->directory, name, 0);
        }
        return -1;
    }
    log->segment[log->count] = (Segment){.fd = fd, .size = size, .end = HEADER};
    return (int)log->count++;
}

PureResult pure_log_append(PureLog *log, uint64_t seq, bool tree, PureSpan payload) {
    if (seq <= log->last || payload.size > UINT32_MAX) return PURE_INVALID;
    unmap(log);
    size_t entry = ENTRY + padded(payload.size);
    int at = log->current;
    Segment *s = at >= 0 ? &log->segment[at] : NULL;
    /* A segment with no entry yet (a first one taken back) takes the next
     * entry after its header, grown to hold it. */
    if (s && !s->first && s->end + entry > s->size) {
        if (!allocate(s->fd, s->end + entry)) return PURE_STORE_ERROR;
        s->size = s->end + entry;
    }
    bool begin = !s || s->end + entry > s->size;
    if (begin && (at = fresh_segment(log, HEADER + entry)) < 0) return PURE_STORE_ERROR;
    s = &log->segment[at];
    size_t size = (begin ? HEADER : 0) + entry;
    if (log->capacity < size) {
        size_t capacity = log->capacity ? log->capacity : 65536;
        while (capacity < size) capacity *= 2;
        unsigned char *grown = realloc(log->buffer, capacity);
        if (!grown) return PURE_ALLOCATION;
        log->buffer = grown;
        log->capacity = capacity;
    }
    uint64_t generation = begin ? log->generation + 1 : s->generation, offset = begin ? 0 : s->end;
    uint64_t predecessor = begin ? (log->current >= 0 ? log->segment[log->current].generation : 0) : s->predecessor;
    unsigned char *out = log->buffer;
    if (begin) { /* a segment's header goes with its first entry, in one write */
        memset(out, 0, HEADER);
        memcpy(out, log_magic, 8);
        put64(out + 8, generation);
        put64(out + 16, predecessor);
        put64(out + HEADER - 8, digest_of(out, HEADER - 8));
        out += HEADER;
    }
    put64(out, seq);
    put64(out + 8, log->last);
    put64(out + 16, (uint64_t)payload.size | (uint64_t)(tree ? TREE : 0) << 32);
    put64(out + 24, log->digest);
    if (payload.size) memcpy(out + ENTRY, payload.data, payload.size);
    memset(out + ENTRY + payload.size, 0, padded(payload.size) - payload.size);
    uint64_t digest = entry_digest(out, out + ENTRY, payload.size);
    put64(out + 32, digest);
    if (fault(PURE_LOG_BEFORE_WRITE)) return PURE_STORE_ERROR; /* nothing written: no entry */
    if (!write_all(s->fd, log->buffer, size, offset) || fault(PURE_LOG_BEFORE_SYNC) || !sync_data(s->fd))
        return PURE_UNCERTAIN;
    if (begin) {
        log->generation = generation;
        *s = (Segment){.fd = s->fd, .size = s->size, .generation = generation, .predecessor = predecessor, .end = HEADER};
    }
    log->before = *s;
    log->last_before = log->last;
    log->digest_before = log->digest;
    log->appended = at;
    if (!s->first) s->first = seq, s->previous = log->last, s->named = log->digest;
    s->last = seq;
    s->digest = digest;
    s->end = offset + (begin ? HEADER : 0) + entry;
    log->last = seq;
    log->digest = digest;
    log->current = at;
    return PURE_OK;
}

void pure_log_retract(PureLog *log) {
    if (log->appended < 0) return;
    /* The segment keeps its header even when the entry was its first: it
     * stays the one appended to, and the next entry takes this one's place. */
    log->segment[log->appended] = log->before;
    log->current = log->appended;
    log->last = log->last_before;
    log->digest = log->digest_before;
    log->appended = -1;
}

void pure_log_release(PureLog *log, uint64_t seq) {
    if (seq > log->released) log->released = seq;
}

/* ---- Images */

static bool image_name(char out[64], const char *name, const char *prefix) {
    size_t length = strlen(name);
    if (!length || length > 40) return false;
    for (size_t i = 0; i < length; ++i) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) return false;
    }
    snprintf(out, 64, "%simage-%s", prefix, name);
    return true;
}

PureResult pure_image_write(const char *directory, const char *name, bool (*next)(void *, PureSpan *chunk), void *context) {
    char final[64], temporary[96];
    if (!image_name(final, name, "")) return PURE_INVALID;
    static uint64_t sequence;
    snprintf(temporary, sizeof(temporary), ".%s-%ld-%llu", final, (long)getpid(),
             (unsigned long long)__atomic_add_fetch(&sequence, 1, __ATOMIC_RELAXED));
    int dir = open(directory, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dir < 0) return PURE_STORE_ERROR;
    int fd = openat(dir, temporary, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) { close(dir); return PURE_STORE_ERROR; }
    unsigned char head[16];
    memcpy(head, image_magic, 8);
    put64(head + 8, 0); /* its length, once known */
    Digest digest = {0};
    bool ok = write_all(fd, head, sizeof(head), 0), produced = true;
    uint64_t at = sizeof(head), started = 0;
    for (PureSpan chunk; ok && (produced = next(context, &chunk)) && chunk.size;)
        for (size_t done = 0; ok && done < chunk.size;) {
            size_t take = chunk.size - done < STEP ? chunk.size - done : STEP;
            digest_add(&digest, chunk.data + done, take);
            ok = write_all(fd, chunk.data + done, take, at);
            done += take;
            at += take;
#if defined(__linux__)
            /* Written steps go to the device as the next are written; a
             * writer waits for the step before the last. */
            if (ok && at - started >= STEP) {
                sync_file_range(fd, (off_t)started, (off_t)(at - started), SYNC_FILE_RANGE_WRITE);
                if (started >= STEP)
                    sync_file_range(fd, (off_t)(started - STEP), STEP,
                                    SYNC_FILE_RANGE_WAIT_BEFORE | SYNC_FILE_RANGE_WRITE | SYNC_FILE_RANGE_WAIT_AFTER);
                started = at;
            }
#endif
        }
    unsigned char tail[8];
    put64(tail, digest_end(&digest));
    put64(head + 8, at - sizeof(head));
    ok = ok && produced && write_all(fd, tail, sizeof(tail), at) && write_all(fd, head, sizeof(head), 0) &&
         !fault(PURE_IMAGE_BEFORE_RENAME) && sync_data(fd);
    if (close(fd)) ok = false;
    ok = ok && !renameat(dir, temporary, dir, final);
    if (!ok) unlinkat(dir, temporary, 0);
    ok = ok && sync_whole(dir);
    close(dir);
    return ok ? PURE_OK : produced ? PURE_STORE_ERROR : PURE_ALLOCATION;
}

PureResult pure_image_read(const char *directory, const char *name, unsigned char **bytes, size_t *size) {
    char final[64];
    *bytes = NULL;
    *size = 0;
    if (!image_name(final, name, "")) return PURE_INVALID;
    int dir = open(directory, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dir < 0) return PURE_STORE_ERROR;
    int fd = openat(dir, final, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    close(dir);
    if (fd < 0) return errno == ENOENT ? PURE_NOT_FOUND : PURE_STORE_ERROR;
    struct stat file;
    unsigned char *all = NULL;
    PureResult result = PURE_STORE_ERROR;
    if (!fstat(fd, &file) && file.st_size >= 24 && (all = malloc((size_t)file.st_size))) {
        size_t have = 0, want = (size_t)file.st_size;
        while (have < want) {
            ssize_t n = pread(fd, all + have, want - have, (off_t)have);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            have += (size_t)n;
        }
        uint64_t length = have == want ? get64(all + 8) : UINT64_MAX;
        if (have == want && !memcmp(all, image_magic, 8) && length == want - 24 &&
            get64(all + 16 + length) == digest_of(all + 16, (size_t)length)) {
            memmove(all, all + 16, (size_t)length);
            *bytes = all;
            *size = (size_t)length;
            result = PURE_OK;
        }
    }
    if (result != PURE_OK) free(all);
    close(fd);
    return result;
}

PureResult pure_image_remove(const char *directory, const char *name) {
    char final[64];
    if (!image_name(final, name, "")) return PURE_INVALID;
    int dir = open(directory, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dir < 0) return PURE_STORE_ERROR;
    bool ok = (!unlinkat(dir, final, 0) || errno == ENOENT) && sync_whole(dir);
    close(dir);
    return ok ? PURE_OK : PURE_STORE_ERROR;
}
