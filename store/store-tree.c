/* Selected storage experiment: every document lives in one append-only file as
 * a persistent B-tree of byte extents. An edit writes only the leaves and the
 * path it changes, so inserting or deleting in a large document costs O(log n)
 * nodes instead of a whole-document copy. Unchanged extents are shared by old
 * and new versions. The document catalog is the same structure holding fixed
 * records sorted by key. A commit appends a record (its head: where its nodes
 * are and their digest) and then its nodes, and syncs once: one contiguous
 * range, which a flash device writes about twice as fast as two. Every few
 * commits the head also goes into the older of two slots at the start, where
 * a reader begins before following the records after it; readers take no
 * lock and map one immutable prefix. */
#define _POSIX_C_SOURCE 200809L
#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif
#include "store.h"
#include "wait.h"
#include "state.h"
#include "digest.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

enum { FREE = 0xffff };
/* Each head slot has a block of its own: a torn write of one never touches
 * the other. */
enum { SLOT = 128, BLOCK = 4096, DATA = 2 * BLOCK, LEAF_MAX = 4096, LEAF_MIN = 1024,
       FAN_MAX = 32, FAN_MIN = 8, RECORD = 96, KEY = 64, HEIGHT_MAX = 12,
       BUFFER = 65536 };
/* Written zeros kept after the published end (see prepare_tail). */
enum { TAIL_MIN = 65536, TAIL_STEP = 262144 };
#ifdef PURE_STORE_TEST
enum { COMPACT_SLACK = 65536 };
#else
enum { COMPACT_SLACK = 1048576 };
#endif
#define LEAF_BIT (UINT64_C(1) << 63)
#define MEMORY_BIT (UINT64_C(1) << 62)
#define LOCATION (MEMORY_BIT - 1)
/* Slots of this format, and of the one before tail records (still read). */
static const unsigned char head_magic[8] = {'P','U','R','E','T','R','E','3'};
static const unsigned char earlier_magic[8] = {'P','U','R','E','T','R','E','2'};
/* A commit's record of the format before links (a whole head; still read). */
static const unsigned char record_magic[8] = {'P','U','R','E','R','E','C','1'};
/* A commit's record, the chain's link: only what the head before it does not
 * say. Its seq is that head's + 1, its bytes start right after it, its live
 * is that head's; its digest binds the seq it takes. */
enum { LINK = 40 };
static const unsigned char link_magic[4] = {'P','R','L','1'};
/* An app store's file is a ring (030 S2): one file for the store's life, its
 * bytes reused in place, read only by its owner. Its slots say so. Its live
 * nodes may lie anywhere in it, so its readers map it whole; a commit's link
 * is where its bytes start, at the newest commit's end or, when that commit
 * went elsewhere, where a wrap marker there names it (with its link's own
 * digest: a link left there before by a commit a start passed over does not
 * count). */
static const unsigned char ring_magic[8] = {'P','U','R','E','R','N','G','1'};
static const unsigned char marker_magic[4] = {'P','R','W','1'};
/* Commits between two heads written to a slot. */
enum { CHECKPOINT = 16 };
static const unsigned char branch_magic[4] = {'P','T','B','1'};
static const char tree_name[] = "pure.tree";
/* A store's turns in this process, one for each store directory (known by
 * its identity: two paths to one directory share them). Threads take turns
 * as its writer: the file lock arbitrates only between processes, and the
 * process's one writer on the store holds it (Holding, below). And their
 * commits to it group (Group commits, below). Stores apart wait for nothing
 * of each other's, a sync least of all. */
typedef struct Turns {
    dev_t device;
    ino_t inode;
    pthread_mutex_t writer;
    pthread_mutex_t lock; /* the group's */
    pthread_cond_t finished, joined;
    struct Waiter *head, *tail;
    size_t queued, expected; /* expected: how many requests groups have held lately */
    bool leading;
    unsigned long groups, requests;
    struct Holding *holding; /* the process's writer on the store (below) */
    bool swept; /* this process cleared what dead writers left (sweep, below) */
    struct Turns *next;
} Turns;
static struct { pthread_mutex_t lock; Turns *first; } stores = {PTHREAD_MUTEX_INITIALIZER, NULL};


/* The turns of the store whose directory is info, made the first time;
 * NULL without memory. */
static Turns *turns_for(const struct stat *info) {
    pthread_mutex_lock(&stores.lock);
    Turns *t = stores.first;
    while (t && (t->device != info->st_dev || t->inode != info->st_ino)) t = t->next;
    if (!t && (t = calloc(1, sizeof(*t)))) {
        *t = (Turns){.device = info->st_dev, .inode = info->st_ino, .expected = 1, .next = stores.first};
        bool made = !pthread_mutex_init(&t->writer, NULL) && !pthread_mutex_init(&t->lock, NULL) &&
                    !pthread_cond_init(&t->finished, NULL);
        /* joined waits on the monotonic clock (core/wait.h); else it does not gather */
        if (made && pure_cond_monotonic(&t->joined)) made = !pthread_cond_init(&t->joined, NULL);
        if (made) stores.first = t;
        else {
            free(t);
            t = NULL;
        }
    }
    pthread_mutex_unlock(&stores.lock);
    return t;
}

static Turns *turns_of(int directory) {
    struct stat info;
    return fstat(directory, &info) ? NULL : turns_for(&info);
}
static uint64_t temporary_sequence;

/* A reference is a file offset, or an index into this edit's node arena.
 * bytes is the size of the whole subtree. Leaves hold raw document bytes. */
typedef struct { uint64_t ref, bytes; } Entry;
typedef struct { Entry *items; size_t count, capacity; } List;
typedef struct {
    unsigned char *bytes; /* leaf only */
    Entry *children;      /* branch only */
    size_t size;          /* leaf bytes, child count, or next free slot + 1 */
    unsigned height;      /* FREE for a reclaimed slot */
} Node;
/* One mapping of a file from its start, length bytes: shared by the versions
 * read from it (each a head whose bytes end at or before length) and by
 * whoever still sends extents from it. */
/* A ring's reader holds a pin (030 S2): a mapping of its own over the
 * file's (region), with the version it reads and the key (empty: every key
 * and the catalog). While it lives the writer reuses no byte that version
 * holds of that key; each draft, frozen copy and lent extent counts it. */
typedef struct PureMapping {
    const unsigned char *map;
    uint64_t length;
    unsigned refs;
    struct PureMapping *region; /* a pin's: the file's mapping it reads through (counted) */
    struct Turns *turns;        /* a pin's: whose */
    uint64_t version;
    struct PureMapping *next;   /* a pin's: the holding's pins */
    char key[KEY + 1];
} Mapping;
typedef struct {
    const unsigned char *map;
    uint64_t end;
    Mapping *shared; /* the mapping map is of (counted; NULL with no map) */
    Node *nodes;
    size_t count, capacity, free_head, free_count; /* free_head is slot + 1 */
    size_t bytes; /* payload held by arena nodes, including unreachable ones */
} Tree;
/* A commit's bytes are start..end (none for a file synced whole before it was
 * published); check is their digest. */
typedef struct { uint64_t seq, end, catalog, catalog_bytes, live, start, check; bool ring; } Head; /* ring: below */
struct Ring;
typedef struct {
    int fd;
    uint64_t at; /* file offset of the next byte */
    size_t used;
    bool failed;
    uint64_t limit;    /* a ring's commit: where the free bytes it goes to end (0: no end) */
    bool over;         /* it reached limit: it is written again where it fits */
    struct Ring *ring; /* a ring's commit: notes the nodes it writes and the stored ones it keeps */
    Digest digest; /* of every byte written */
    unsigned char buffer[BUFFER];
} Writer;
typedef struct { PureSpan part[3]; size_t index, offset; } Source;

/* A locked writer over the current published file. Its tree maps the file
 * from its start, at least to the head's end, and holds a commit's catalog
 * edits until the commit ends. file.st_size: what the file has at least. */
typedef struct {
    int directory, lock, fd;
    Head head;
    uint64_t slots[2]; /* the sequences of the slots' heads */
    Tree tree;
    struct stat file;
    struct Ring *ring; /* an app store's (030 S2), or NULL */
} Session;

/* The process's one writer on a store, opened by its first holder and closed
 * by its last: a store handle holds it while open, a commit while it
 * publishes. So while a handle is open the writer lock is this process's,
 * and a commit neither locks nor reads its head: the file changes only
 * through this session. The session is guarded by the turns' writer; the
 * holders, and the newest version it published or read (what handles see),
 * by the turns' lock. */
typedef struct Holding {
    Session session;
    unsigned holders;
    unsigned forks; /* the process's forks when it opened: a forked child's copy holds no lock */
    bool stale;     /* a commit left it unsure of its head: read again before the next */
    Writer *buffer; /* the commits' writer */
    Head newest;
    Mapping *mapping; /* newest's (a reference) */
    dev_t device;     /* newest's file */
    ino_t inode;
    struct Turns *turns;
    Mapping *pins;    /* a ring's readers' (turns->lock), swept by the writer once let go */
    unsigned held_pins; /* those not let go (turns->lock) */
    bool closing;     /* its holders are gone, a ring's pins not yet: the session (and its lock) stays */
} Holding;

/* Forks in this process's line. Around a fork the stores' list and every
 * store's lock are taken, so the child's copies are whole; the child closes
 * its copies of every writer's descriptors at once (on Linux the lock is the
 * open file description's, which the child's copies would keep after the
 * parent let it go) and counts the fork: a writer opened before is the
 * parent's. */
static unsigned forks;
static void before_fork(void) {
    pthread_mutex_lock(&stores.lock);
    for (Turns *t = stores.first; t; t = t->next) pthread_mutex_lock(&t->lock);
}
static void after_fork(void) {
    for (Turns *t = stores.first; t; t = t->next) pthread_mutex_unlock(&t->lock);
    pthread_mutex_unlock(&stores.lock);
}
static void in_child(void) {
    for (Turns *t = stores.first; t; t = t->next) {
        Session *s = t->holding ? &t->holding->session : NULL;
        if (s && s->lock >= 0) close(s->lock);
        if (s && s->fd >= 0) close(s->fd);
        if (s && s->directory >= 0) close(s->directory);
        if (s) s->lock = s->fd = s->directory = -1;
        pthread_mutex_unlock(&t->lock);
    }
    ++forks;
    pthread_mutex_unlock(&stores.lock);
}
static pthread_once_t fork_watch = PTHREAD_ONCE_INIT;
static void watch_forks(void) { pthread_atfork(before_fork, after_fork, in_child); }

static bool fault(PureStoreStage stage) {
#ifdef PURE_STORE_TEST
    if (pure_store_test_fault(stage)) { errno = EIO; return true; }
#else
    (void)stage;
#endif
    return false;
}

static bool valid_key(const char *key) {
    if (!key) return false;
    size_t length = strlen(key);
    if (!length || length > KEY) return false;
    for (size_t i = 0; i < length; ++i) {
        char b = key[i];
        if (!((b >= 'a' && b <= 'z') || (b >= 'A' && b <= 'Z') ||
              (b >= '0' && b <= '9') || b == '-' || b == '_')) return false;
    }
    return true;
}

/* Runtime-owned values (app states) live under '.' followed by a valid key.
 * Document keys never start with '.', so the two never meet. */
static bool runtime_key(const char *key) {
    return key && key[0] == '.' && strlen(key) <= KEY && valid_key(key + 1);
}

static uint64_t decode64(const unsigned char *bytes) {
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) value |= (uint64_t)bytes[i] << (8 * i);
    return value;
}

static void encode64(unsigned char *bytes, uint64_t value) {
    for (unsigned i = 0; i < 8; ++i) bytes[i] = (unsigned char)(value >> (8 * i));
}

#ifdef PURE_STORE_TEST
static unsigned long syncs;
unsigned long pure_tree_test_syncs(void) { return __atomic_load_n(&syncs, __ATOMIC_RELAXED); }
#define COUNT_SYNC() __atomic_add_fetch(&syncs, 1, __ATOMIC_RELAXED)
#else
#define COUNT_SYNC() ((void)0)
#endif

static bool sync_fd(int fd) {
    int rc;
    COUNT_SYNC();
    do { rc = fsync(fd); } while (rc && errno == EINTR);
    return rc == 0;
}

/* Data and the size needed to read it back, without timestamps. */
static bool sync_data(int fd) {
#ifdef __APPLE__
    return sync_fd(fd);
#else
    int rc;
    COUNT_SYNC();
    do { rc = fdatasync(fd); } while (rc && errno == EINTR);
    return rc == 0;
#endif
}

static bool write_at(int fd, const unsigned char *bytes, size_t length, uint64_t offset) {
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

static bool read_at(int fd, unsigned char *bytes, size_t length, uint64_t offset) {
    while (length) {
        ssize_t n = pread(fd, bytes, length, (off_t)offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        bytes += (size_t)n;
        length -= (size_t)n;
        offset += (uint64_t)n;
    }
    return true;
}

static void encode_head(unsigned char out[SLOT], const unsigned char magic[8], const Head *head) {
    memset(out, 0, SLOT);
    memcpy(out, magic, 8);
    encode64(out + 8, head->seq);
    encode64(out + 16, head->end);
    encode64(out + 24, head->catalog);
    encode64(out + 32, head->catalog_bytes);
    encode64(out + 40, head->live);
    encode64(out + 48, head->start);
    encode64(out + 56, head->check);
    encode64(out + SLOT - 8, digest_of(out, SLOT - 8));
}

/* A commit's link after previous: its record count, end, catalog and check,
 * and a digest of them with the seq it takes. */
static uint64_t link_digest(const unsigned char in[LINK], uint64_t seq) {
    unsigned char bound[8 + LINK - 8];
    encode64(bound, seq);
    memcpy(bound + 8, in, LINK - 8);
    return digest_of(bound, sizeof(bound));
}

static void encode_link(unsigned char out[LINK], const Head *head) {
    memcpy(out, link_magic, 4);
    uint64_t records = head->catalog_bytes / RECORD;
    for (unsigned i = 0; i < 4; ++i) out[4 + i] = (unsigned char)(records >> (8 * i));
    encode64(out + 8, head->end);
    encode64(out + 16, head->catalog);
    encode64(out + 24, head->check);
    encode64(out + 32, link_digest(out, head->seq));
}

/* The commit a link at at, after previous, names, if it is one (not a torn
 * one): its bytes start right after it. */
static bool decode_link(const unsigned char in[LINK], const Head *previous, uint64_t at, Head *next) {
    if (memcmp(in, link_magic, 4) || decode64(in + 32) != link_digest(in, previous->seq + 1)) return false;
    uint64_t records = (uint64_t)in[4] | (uint64_t)in[5] << 8 | (uint64_t)in[6] << 16 | (uint64_t)in[7] << 24;
    *next = (Head){previous->seq + 1, decode64(in + 8), decode64(in + 16), records * RECORD, previous->live,
                   at + LINK, decode64(in + 24), previous->ring};
    return next->end <= (uint64_t)INT64_MAX && next->end <= SIZE_MAX && next->start <= next->end &&
           !next->catalog == !next->catalog_bytes;
}

/* A ring's wrap marker at its front: the seq of the commit it names, where
 * that commit's link is, and that link's digest (bound). */
static void encode_marker(unsigned char out[LINK], uint64_t seq, uint64_t at, const unsigned char bound[8]) {
    memset(out, 0, LINK);
    memcpy(out, marker_magic, 4);
    encode64(out + 8, at);
    memcpy(out + 16, bound, 8);
    encode64(out + 32, link_digest(out, seq));
}

static bool decode_marker(const unsigned char in[LINK], uint64_t seq, uint64_t *at, const unsigned char **bound) {
    if (memcmp(in, marker_magic, 4) || decode64(in + 32) != link_digest(in, seq)) return false;
    *at = decode64(in + 8);
    *bound = in + 16;
    return true;
}

/* A slot's head (record false) or a commit record's. */
static bool decode_head(const unsigned char in[SLOT], bool record, Head *head) {
    bool ring = !record && !memcmp(in, ring_magic, 8);
    bool magic = record ? !memcmp(in, record_magic, 8) : ring || !memcmp(in, head_magic, 8) || !memcmp(in, earlier_magic, 8);
    if (!magic || decode64(in + SLOT - 8) != digest_of(in, SLOT - 8)) return false;
    *head = (Head){decode64(in + 8), decode64(in + 16), decode64(in + 24), decode64(in + 32),
                   decode64(in + 40), decode64(in + 48), decode64(in + 56), ring};
    return head->seq && head->end <= (uint64_t)INT64_MAX && head->end <= SIZE_MAX &&
           head->start >= DATA && head->start <= head->end && head->catalog_bytes % RECORD == 0 &&
           !head->catalog == !head->catalog_bytes;
}

/* ---- Trusted heads --------------------------------------------------------
 * One sync makes a commit's bytes and its head durable together, so a crash
 * during it can leave the head on the device without all of the bytes: a
 * head counts only once its digest matches them. The older slot needs no
 * such luck. A writer builds only on a durable head, so the newer head was
 * written after the older one's commit was complete on the device.
 * The process remembers the heads it published or checked, so it reads a
 * commit's bytes at most once, and which of them it made durable. */
enum { UNKNOWN, BROKEN, CHECKED, DURABLE, PUBLISHING };
enum { KNOWN = 32 };
typedef struct { dev_t device; ino_t inode; uint64_t seq, check; int state; } Known;
/* The newest head that counts per file, to follow records from. */
typedef struct { dev_t device; ino_t inode; Head head; } Newest;
enum { NEWEST = 4 };
static struct {
    pthread_mutex_t lock;
    Known heads[KNOWN];
    unsigned next;
    Known *publishing; /* written here, not durable yet: a head a file at most */
    size_t publishing_count, publishing_room;
    Newest newest[NEWEST];
    unsigned next_newest;
} known = {.lock = PTHREAD_MUTEX_INITIALIZER};

static bool names(const Known *known_head, const struct stat *file, const Head *head) {
    return known_head->state && known_head->device == file->st_dev && known_head->inode == file->st_ino &&
           known_head->seq == head->seq && known_head->check == head->check;
}

static int recall(const struct stat *file, const Head *head) {
    int state = UNKNOWN;
    pthread_mutex_lock(&known.lock);
    for (size_t i = 0; i < known.publishing_count && state == UNKNOWN; ++i)
        if (names(&known.publishing[i], file, head)) state = PUBLISHING;
    for (unsigned i = 0; i < KNOWN && state == UNKNOWN; ++i)
        if (names(&known.heads[i], file, head)) state = known.heads[i].state;
    pthread_mutex_unlock(&known.lock);
    return state;
}

/* PUBLISHING marks a file's head being made durable (one a file: its
 * writer's commit), whatever other files publish meanwhile; any other state
 * ends that mark (UNKNOWN only ends it). False when there is no room left to
 * mark it. */
static bool remember(const struct stat *file, const Head *head, int state) {
    Known entry = {file->st_dev, file->st_ino, head->seq, head->check, state};
    bool kept = true;
    pthread_mutex_lock(&known.lock);
    size_t mark = 0;
    while (mark < known.publishing_count &&
           (known.publishing[mark].device != file->st_dev || known.publishing[mark].inode != file->st_ino)) ++mark;
    if (state == PUBLISHING && mark == known.publishing_count && mark == known.publishing_room) {
        size_t room = known.publishing_room ? 2 * known.publishing_room : 4;
        Known *grown = realloc(known.publishing, room * sizeof(*grown));
        if (grown) known.publishing = grown, known.publishing_room = room;
        else kept = false;
    }
    if (state == PUBLISHING && kept) {
        known.publishing[mark] = entry;
        if (mark == known.publishing_count) ++known.publishing_count;
    } else if (state != PUBLISHING && mark < known.publishing_count && names(&known.publishing[mark], file, head))
        known.publishing[mark] = known.publishing[--known.publishing_count];
    if (state != PUBLISHING && state != UNKNOWN) {
        Known *at = NULL;
        for (unsigned i = 0; i < KNOWN && !at; ++i)
            if (names(&known.heads[i], file, head)) at = &known.heads[i];
        if (!at) known.heads[known.next++ % KNOWN] = entry;
        else if (at->state != DURABLE) *at = entry; /* a durable head stays durable */
    }
    if (state == CHECKED || state == DURABLE) {
        Newest *at = NULL;
        for (unsigned i = 0; i < NEWEST && !at; ++i)
            if (known.newest[i].head.seq && known.newest[i].device == file->st_dev &&
                known.newest[i].inode == file->st_ino) at = &known.newest[i];
        if (!at) at = &known.newest[known.next_newest++ % NEWEST];
        else if (at->head.seq >= head->seq) at = NULL;
        if (at) *at = (Newest){file->st_dev, file->st_ino, *head};
    }
    pthread_mutex_unlock(&known.lock);
    return kept;
}

static bool newest_known(const struct stat *file, Head *out) {
    bool found = false;
    pthread_mutex_lock(&known.lock);
    for (unsigned i = 0; i < NEWEST && !found; ++i)
        if (known.newest[i].head.seq && known.newest[i].device == file->st_dev && known.newest[i].inode == file->st_ino) {
            *out = known.newest[i].head;
            found = true;
        }
    pthread_mutex_unlock(&known.lock);
    return found;
}

#ifdef PURE_STORE_TEST
/* Test-only: forget every head, as a new process after a crash would. */
void pure_tree_test_forget(void) {
    pthread_mutex_lock(&known.lock);
    memset(known.heads, 0, sizeof(known.heads));
    memset(known.newest, 0, sizeof(known.newest));
    known.publishing_count = 0;
    pthread_mutex_unlock(&known.lock);
    /* and the writers this process holds read their heads again */
    pthread_mutex_lock(&stores.lock);
    for (Turns *t = stores.first; t; t = t->next) {
        pthread_mutex_lock(&t->writer);
        if (t->holding) t->holding->stale = true;
        pthread_mutex_unlock(&t->writer);
    }
    pthread_mutex_unlock(&stores.lock);
}
#endif

/* Whether a head's commit has all of its bytes. */
static bool whole(int fd, const Head *head) {
    unsigned char buffer[16384];
    Digest digest = {0};
    for (uint64_t at = head->start; at < head->end;) {
        size_t n = head->end - at < sizeof(buffer) ? (size_t)(head->end - at) : sizeof(buffer);
        if (!read_at(fd, buffer, n, at)) return false;
        digest_add(&digest, buffer, n);
        at += n;
    }
    return digest_end(&digest) == head->check;
}

/* The newest head that counts. A reader passes over the head this process is
 * still making durable, so the process never answers with a commit that a
 * crash could take back; a writer never meets one (it is the only writer).
 * A slot whose head does not count (a crash in its checkpoint's sync kept
 * the head, not the commit's bytes) is the one the next checkpoint writes:
 * its sequence is 0, as an empty slot's. */
static bool pick(int fd, const struct stat *file, const unsigned char *slots[2], bool writer, Head *out,
                 uint64_t sequences[2]) {
    Head heads[2];
    bool valid[2];
    for (unsigned i = 0; i < 2; ++i) {
        valid[i] = decode_head(slots[i], false, &heads[i]) && heads[i].end <= (uint64_t)file->st_size;
        if (sequences) sequences[i] = valid[i] ? heads[i].seq : 0;
    }
    unsigned newest = valid[1] && (!valid[0] || heads[1].seq > heads[0].seq);
    for (unsigned turn = 0; turn < 2; ++turn) {
        unsigned slot = turn ? !newest : newest;
        const Head *head = &heads[slot];
        if (!valid[slot]) continue;
        int state = recall(file, head);
        if (state == PUBLISHING && !writer) continue;
        if (state == UNKNOWN) {
            state = whole(fd, head) ? CHECKED : BROKEN;
            remember(file, head, state);
        }
        if (state == BROKEN) {
            if (sequences) sequences[slot] = 0;
            continue;
        }
        *out = *head;
        return true;
    }
    return false;
}

/* The commit a ring's wrap marker at head's end names: its link where the
 * marker says, with the digest the marker binds. */
static bool marked(int fd, uint64_t size, const Head *head, const unsigned char record[LINK], Head *next) {
    uint64_t at;
    const unsigned char *bound;
    unsigned char link[LINK];
    return decode_marker(record, head->seq + 1, &at, &bound) && at >= DATA && at <= size && size - at >= LINK &&
           read_at(fd, link, LINK, at) && !memcmp(link + 32, bound, 8) && decode_link(link, head, at, next);
}

/* The commit whose record follows head's bytes, if its own bytes are whole:
 * a link, a ring's marker naming one, or a record of the format before. */
static bool follow(int fd, const struct stat *file, bool writer, const Head *head, Head *out) {
    unsigned char record[SLOT];
    uint64_t size = (uint64_t)file->st_size;
    Head next;
    if (head->end > size || size - head->end < LINK || !read_at(fd, record, LINK, head->end)) return false;
    if (!decode_link(record, head, head->end, &next) && !(head->ring && marked(fd, size, head, record, &next)) &&
        (head->ring || size - head->end < SLOT || !read_at(fd, record, SLOT, head->end) || !decode_head(record, true, &next) ||
         next.seq != head->seq + 1 || next.start != head->end + SLOT)) return false;
    if (next.end > size) return false;
    int state = recall(file, &next);
    if (state == BROKEN || (state == PUBLISHING && !writer)) return false;
    if (state == UNKNOWN) {
        bool counts = whole(fd, &next);
        remember(file, &next, counts ? CHECKED : BROKEN);
        if (!counts) return false;
    }
    *out = next;
    return true;
}

/* Whether head's record (its link, or a record of the format before) is in
 * the file as it was made. */
static bool recorded(int fd, const struct stat *file, const Head *head) {
    unsigned char stored[SLOT], made[SLOT];
    if (head->start < DATA + LINK || head->end > (uint64_t)file->st_size) return false;
    encode_link(made, head);
    if (read_at(fd, stored, LINK, head->start - LINK) && !memcmp(stored, made, LINK)) return true;
    if (head->start < DATA + SLOT) return false;
    encode_head(made, record_magic, head);
    return read_at(fd, stored, SLOT, head->start - SLOT) && !memcmp(stored, made, SLOT);
}

/* The newest commit: from the newer slot's head that counts, or from the
 * newest this process has seen in the file when its record is still there,
 * along the records that follow. sequences gets the slots' (0: none). */
static bool read_head(int fd, struct stat *file, bool writer, Head *out, uint64_t sequences[2]) {
    unsigned char slots[BLOCK + SLOT];
    Head head, seen;
    if (fstat(fd, file) || !S_ISREG(file->st_mode) || file->st_size < 0 || !read_at(fd, slots, sizeof(slots), 0) ||
        !pick(fd, file, (const unsigned char *[2]){slots, slots + BLOCK}, writer, &head, sequences)) return false;
    if (newest_known(file, &seen) && seen.seq > head.seq && recorded(fd, file, &seen)) head = seen;
    while (follow(fd, file, writer, &head, &head)) {}
    *out = head;
    return true;
}

/* The file mapped to the head's end, a mapping counted as every one is: a
 * draft opened by path can be frozen and lend it as a handle's draft can. */
static bool map_tree(int fd, const struct stat *file, const Head *head, Tree *tree) {
    *tree = (Tree){0};
    if ((uint64_t)file->st_size < head->end) return false;
    uint64_t length = head->ring ? (uint64_t)file->st_size : head->end; /* a ring's live nodes lie anywhere in it */
    Mapping *mapping = malloc(sizeof(*mapping));
    void *map = mapping ? mmap(NULL, (size_t)length, PROT_READ, MAP_SHARED, fd, 0) : MAP_FAILED;
    if (map == MAP_FAILED) { free(mapping); return false; }
    *mapping = (Mapping){.map = map, .length = length, .refs = 1};
    *tree = (Tree){.map = map, .end = length, .shared = mapping};
    return true;
}

/* A lent mapping may be released on another thread (a host that answers on
 * its connection threads): its count is atomic. */
static void let_pin_go(Mapping *pin);
static void release_mapping(Mapping *mapping) {
    if (!mapping || __atomic_sub_fetch(&mapping->refs, 1, __ATOMIC_ACQ_REL)) return;
    if (mapping->region) { /* a pin: swept by the writer */
        let_pin_go(mapping);
        return;
    }
    munmap((void *)mapping->map, (size_t)mapping->length);
    free(mapping);
}

static void release_tree(Tree *tree) {
    release_mapping(tree->shared);
    for (size_t i = 0; i < tree->count; ++i) {
        free(tree->nodes[i].bytes);
        free(tree->nodes[i].children);
    }
    free(tree->nodes);
    *tree = (Tree){0};
}

static const unsigned char *leaf_bytes(const Tree *tree, Entry entry) {
    if (!(entry.ref & LEAF_BIT) || !entry.bytes || entry.bytes > LEAF_MAX) return NULL;
    uint64_t at = entry.ref & LOCATION;
    if (entry.ref & MEMORY_BIT) {
        if (at >= tree->count) return NULL;
        const Node *node = &tree->nodes[at];
        return node->bytes && node->size == entry.bytes ? node->bytes : NULL;
    }
    if (at < DATA || at > tree->end || entry.bytes > tree->end - at) return NULL;
    return tree->map + at;
}

/* Returns the child count of a checked branch at height, or 0. */
static size_t children(const Tree *tree, Entry entry, unsigned height, Entry out[FAN_MAX]) {
    if ((entry.ref & LEAF_BIT) || !height) return 0;
    uint64_t at = entry.ref & LOCATION;
    size_t count;
    if (entry.ref & MEMORY_BIT) {
        if (at >= tree->count) return 0;
        const Node *node = &tree->nodes[at];
        if (node->bytes || node->height != height || !node->size || node->size > FAN_MAX) return 0;
        count = node->size;
        memcpy(out, node->children, count * sizeof(*out));
    } else {
        if (at < DATA || at % 8 || at > tree->end || tree->end - at < 8) return 0;
        const unsigned char *p = tree->map + at;
        count = (size_t)p[4] | (size_t)p[5] << 8;
        unsigned stored = (unsigned)p[6] | (unsigned)p[7] << 8;
        if (memcmp(p, branch_magic, sizeof(branch_magic)) || stored != height || !count ||
            count > FAN_MAX || (tree->end - at - 8) / 16 < count) return 0;
        for (size_t i = 0; i < count; ++i)
            out[i] = (Entry){decode64(p + 8 + 16 * i), decode64(p + 16 + 16 * i)};
    }
    uint64_t sum = 0;
    for (size_t i = 0; i < count; ++i) {
        bool leaf = (out[i].ref & LEAF_BIT) != 0;
        if (!out[i].bytes || out[i].bytes > UINT64_MAX - sum || leaf != (height == 1)) return 0;
        sum += out[i].bytes;
    }
    return sum == entry.bytes ? count : 0;
}

static bool root_height(const Tree *tree, Entry root, unsigned *height) {
    *height = 0;
    if (!root.bytes) return !root.ref;
    if (root.ref & LEAF_BIT) return true;
    uint64_t at = root.ref & LOCATION;
    if (root.ref & MEMORY_BIT) {
        if (at >= tree->count || tree->nodes[at].bytes) return false;
        *height = tree->nodes[at].height;
    } else {
        if (at < DATA || at > tree->end || tree->end - at < 8) return false;
        *height = (unsigned)tree->map[at + 6] | (unsigned)tree->map[at + 7] << 8;
    }
    return *height >= 1 && *height <= HEIGHT_MAX;
}

/* Visit a range as borrowed extents: 1 done, 0 damaged, -1 stopped by visit. */
static int visit_range(const Tree *tree, Entry entry, unsigned height, uint64_t offset,
                       uint64_t length, bool (*visit)(void *, PureSpan), void *context) {
    if (!height) {
        const unsigned char *bytes = leaf_bytes(tree, entry);
        if (!bytes || offset > entry.bytes || length > entry.bytes - offset) return 0;
        return visit(context, (PureSpan){bytes + offset, (size_t)length}) ? 1 : -1;
    }
    Entry kids[FAN_MAX];
    size_t count = children(tree, entry, height, kids);
    if (!count) return 0;
    uint64_t start = 0;
    for (size_t i = 0; i < count && length; ++i) {
        uint64_t end = start + kids[i].bytes;
        if (offset < end) {
            uint64_t inside = offset - start, take = kids[i].bytes - inside;
            if (take > length) take = length;
            int result = visit_range(tree, kids[i], height - 1, inside, take, visit, context);
            if (result != 1) return result;
            offset += take;
            length -= take;
        }
        start = end;
    }
    return length ? 0 : 1;
}

static bool copy_span(void *context, PureSpan span) {
    unsigned char **out = context;
    memcpy(*out, span.data, span.size);
    *out += span.size;
    return true;
}

static bool copy_range(const Tree *tree, Entry entry, unsigned height, uint64_t offset,
                       uint64_t length, unsigned char *out) {
    return !length || visit_range(tree, entry, height, offset, length, copy_span, &out) == 1;
}

/* The bytes of a range held by one leaf, or NULL when it spans leaves. */
static const unsigned char *single_leaf(const Tree *tree, Entry entry, unsigned height,
                                        uint64_t offset, uint64_t length) {
    while (height) {
        Entry kids[FAN_MAX];
        size_t count = children(tree, entry, height, kids), i = 0;
        uint64_t start = 0;
        if (!count) return NULL;
        while (i + 1 < count && offset >= start + kids[i].bytes) start += kids[i++].bytes;
        if (offset - start > kids[i].bytes || length > kids[i].bytes - (offset - start)) return NULL;
        entry = kids[i];
        offset -= start;
        --height;
    }
    const unsigned char *bytes = leaf_bytes(tree, entry);
    return bytes && offset <= entry.bytes && length <= entry.bytes - offset ? bytes + offset : NULL;
}

static bool push(List *list, Entry entry) {
    if (list->count == list->capacity) {
        size_t capacity = list->capacity ? 2 * list->capacity : 8;
        Entry *items = realloc(list->items, capacity * sizeof(*items));
        if (!items) return false;
        list->items = items;
        list->capacity = capacity;
    }
    list->items[list->count++] = entry;
    return true;
}

static size_t node_bytes(const Node *node) {
    return node->bytes ? node->size : node->size * sizeof(Entry);
}

static bool add_node(Tree *tree, Node node, uint64_t *index) {
    tree->bytes += node_bytes(&node);
    if (tree->free_head) {
        *index = tree->free_head - 1;
        tree->free_head = tree->nodes[*index].size;
        --tree->free_count;
        tree->nodes[*index] = node;
        return true;
    }
    if (tree->count == tree->capacity) {
        size_t capacity = tree->capacity ? 2 * tree->capacity : 64;
        Node *nodes = realloc(tree->nodes, capacity * sizeof(*nodes));
        if (!nodes) { tree->bytes -= node_bytes(&node); return false; }
        tree->nodes = nodes;
        tree->capacity = capacity;
    }
    tree->nodes[tree->count] = node;
    *index = tree->count++;
    return true;
}

static void take(Source *source, unsigned char *out, size_t length) {
    while (length) {
        PureSpan part = source->part[source->index];
        size_t left = part.size - source->offset;
        if (!left) { ++source->index; source->offset = 0; continue; }
        size_t n = left < length ? left : length;
        memcpy(out, part.data + source->offset, n);
        out += n;
        length -= n;
        source->offset += n;
    }
}

/* Balanced leaves: every leaf of a multi-leaf result holds >= LEAF_MAX / 2. */
static bool chunk_bytes(Tree *tree, Source *source, uint64_t total, List *out) {
    uint64_t pieces = (total + LEAF_MAX - 1) / LEAF_MAX;
    for (uint64_t i = 0; i < pieces; ++i) {
        size_t size = (size_t)(total / pieces + (i < total % pieces));
        unsigned char *bytes = malloc(size);
        uint64_t index;
        if (!bytes) return false;
        take(source, bytes, size);
        if (!add_node(tree, (Node){.bytes = bytes, .size = size}, &index)) { free(bytes); return false; }
        if (!push(out, (Entry){LEAF_BIT | MEMORY_BIT | index, size})) return false;
    }
    return true;
}

static bool chunk_entries(Tree *tree, const Entry *items, size_t count, unsigned height, List *out) {
    size_t pieces = (count + FAN_MAX - 1) / FAN_MAX;
    for (size_t i = 0, at = 0; i < pieces; ++i) {
        size_t size = count / pieces + (i < count % pieces);
        Entry *kids = malloc(size * sizeof(*kids));
        uint64_t sum = 0, index;
        if (!kids) return false;
        memcpy(kids, items + at, size * sizeof(*kids));
        for (size_t k = 0; k < size; ++k) sum += kids[k].bytes;
        if (!add_node(tree, (Node){.children = kids, .size = size, .height = height}, &index)) {
            free(kids);
            return false;
        }
        if (!push(out, (Entry){MEMORY_BIT | index, sum})) return false;
        at += size;
    }
    return true;
}

/* Only nodes created by this edit can be underfull; stored siblings are left as they are. */
static bool underfull(const Tree *tree, Entry entry, unsigned height) {
    if (!(entry.ref & MEMORY_BIT)) return false;
    return height ? tree->nodes[entry.ref & LOCATION].size < FAN_MIN : entry.bytes < LEAF_MIN;
}

static bool replace(List *list, size_t at, size_t removed, const List *with) {
    size_t count = list->count - removed + with->count;
    if (count > list->capacity) {
        Entry *items = realloc(list->items, count * sizeof(*items));
        if (!items) return false;
        list->items = items;
        list->capacity = count;
    }
    memmove(list->items + at + with->count, list->items + at + removed,
            (list->count - at - removed) * sizeof(*list->items));
    memcpy(list->items + at, with->items, with->count * sizeof(*list->items));
    list->count = count;
    return true;
}

static bool merge(Tree *tree, Entry a, Entry b, unsigned height, List *out);

/* Merge each new underfull node with a neighbour. A merge removes a node or
 * yields two nodes of at least half capacity, so the loop terminates. */
static bool repair(Tree *tree, List *all, unsigned height) {
    bool ok = true;
    for (size_t i = 0; ok && all->count > 1 && i < all->count;) {
        if (!underfull(tree, all->items[i], height)) { ++i; continue; }
        size_t at = i + 1 < all->count ? i : i - 1;
        List merged = {0};
        ok = merge(tree, all->items[at], all->items[at + 1], height, &merged) &&
             replace(all, at, 2, &merged);
        free(merged.items);
        i = at;
    }
    return ok;
}

static bool merge(Tree *tree, Entry a, Entry b, unsigned height, List *out) {
    if (!height) {
        const unsigned char *x = leaf_bytes(tree, a), *y = leaf_bytes(tree, b);
        if (!x || !y) return false;
        Source source = {.part = {{x, (size_t)a.bytes}, {y, (size_t)b.bytes}}};
        return chunk_bytes(tree, &source, a.bytes + b.bytes, out);
    }
    Entry kids[FAN_MAX];
    List all = {0};
    size_t count = children(tree, a, height, kids);
    bool ok = count != 0;
    for (size_t i = 0; ok && i < count; ++i) ok = push(&all, kids[i]);
    count = ok ? children(tree, b, height, kids) : 0;
    ok = count != 0;
    for (size_t i = 0; ok && i < count; ++i) ok = push(&all, kids[i]);
    /* A small child carried in from a one-child parent is repaired here too. */
    ok = ok && repair(tree, &all, height - 1) && chunk_entries(tree, all.items, all.count, height, out);
    free(all.items);
    return ok;
}

/* Replace [offset, offset + remove) of entry with insert. Appends the
 * replacement nodes, all at entry's height, to out (possibly none). */
static bool edit(Tree *tree, Entry entry, unsigned height, uint64_t offset, uint64_t remove,
                 PureSpan insert, List *out) {
    if (offset > entry.bytes || remove > entry.bytes - offset) return false;
    if (!height) {
        const unsigned char *bytes = leaf_bytes(tree, entry);
        if (!bytes) return false;
        Source source = {.part = {{bytes, (size_t)offset}, insert,
            {bytes + offset + remove, (size_t)(entry.bytes - offset - remove)}}};
        return chunk_bytes(tree, &source, entry.bytes - remove + insert.size, out);
    }
    Entry kids[FAN_MAX];
    size_t count = children(tree, entry, height, kids), first = count, final = count;
    if (!count) return false;
    uint64_t last = remove ? offset + remove - 1 : offset, start = 0, first_start = 0, final_start = 0;
    for (size_t i = 0; i < count; ++i) {
        uint64_t end = start + kids[i].bytes;
        bool tail = i + 1 == count;
        if (first == count && (offset < end || tail)) { first = i; first_start = start; }
        if (first != count && (last < end || tail)) { final = i; final_start = start; break; }
        start = end;
    }
    List middle = {0}, all = {0};
    bool ok = first == final ?
        edit(tree, kids[first], height - 1, offset - first_start, remove, insert, &middle) :
        edit(tree, kids[first], height - 1, offset - first_start,
             first_start + kids[first].bytes - offset, insert, &middle) &&
        edit(tree, kids[final], height - 1, 0, offset + remove - final_start, (PureSpan){0}, &middle);
    for (size_t i = 0; ok && i < first; ++i) ok = push(&all, kids[i]);
    for (size_t i = 0; ok && i < middle.count; ++i) ok = push(&all, middle.items[i]);
    for (size_t i = final + 1; ok && i < count; ++i) ok = push(&all, kids[i]);
    free(middle.items);
    ok = ok && repair(tree, &all, height - 1) && chunk_entries(tree, all.items, all.count, height, out);
    free(all.items);
    return ok;
}

/* The new root of root with [offset, offset + remove) replaced by insert. */
static bool rebuild(Tree *tree, Entry root, uint64_t offset, uint64_t remove,
                    PureSpan insert, Entry *out) {
    unsigned height;
    List list = {0};
    bool ok = root_height(tree, root, &height);
    if (ok && !root.bytes) {
        Source source = {.part = {insert}};
        ok = !offset && !remove && chunk_bytes(tree, &source, insert.size, &list);
    } else if (ok) ok = edit(tree, root, height, offset, remove, insert, &list);
    while (ok && list.count > 1) {
        List up = {0};
        ok = height < HEIGHT_MAX && chunk_entries(tree, list.items, list.count, ++height, &up);
        free(list.items);
        list = up;
    }
    Entry result = ok && list.count ? list.items[0] : (Entry){0};
    free(list.items);
    while (ok && height && result.bytes) {
        Entry kids[FAN_MAX];
        size_t count = children(tree, result, height, kids);
        if (count != 1) { ok = count > 1; break; }
        result = kids[0];
        --height;
    }
    *out = result;
    return ok;
}

static bool flush(Writer *writer) {
    if (writer->used && !writer->failed) {
        digest_add(&writer->digest, writer->buffer, writer->used);
        if (!write_at(writer->fd, writer->buffer, writer->used, writer->at - writer->used)) writer->failed = true;
    }
    writer->used = 0;
    return !writer->failed;
}

/* Zeros from..to; a failure only costs speed. */
static void zero_fill(int fd, uint64_t from, uint64_t to) {
    static const unsigned char zeros[BUFFER];
    while (from < to) {
        size_t n = to - from < BUFFER ? (size_t)(to - from) : BUFFER;
        if (!write_at(fd, zeros, n, from)) return;
        from += n;
    }
}

/* Where the written zeros after a published end should reach. */
static uint64_t tail_end(uint64_t end) {
    return end + ((end - DATA) / 8 > TAIL_STEP ? (end - DATA) / 8 : TAIL_STEP);
}

static void put_bytes(Writer *writer, const unsigned char *bytes, size_t length) {
    if (writer->limit && !writer->failed && (writer->at > writer->limit || length > writer->limit - writer->at)) {
        writer->over = writer->failed = true;
        return;
    }
    while (length && !writer->failed) {
        if (writer->used == BUFFER && !flush(writer)) return;
        size_t n = BUFFER - writer->used < length ? BUFFER - writer->used : length;
        memcpy(writer->buffer + writer->used, bytes, n);
        writer->used += n;
        writer->at += n;
        bytes += n;
        length -= n;
    }
}

/* ---- The ring (030 S2) -----------------------------------------------------
 * An app store's one file, its bytes reused in place. What is free is worked
 * out, never kept on disk: at an open, every byte that neither a node of the
 * newest version nor a commit on the chain from the older slot's head (what a
 * start may follow, with a slot damaged) is in; after, 64 KiB segments come
 * back whole once no live node is in them and every commit in them is older
 * than the older slot's head (H_rec). A commit goes at the front (the newest
 * commit's end) while the bytes there are free; past them it is written
 * again where the lowest free run holds twice what it wrote, a marker at the
 * front naming it, or past the end after the file grows by zeros. The 40
 * bytes after the newest commit are always free: its successor's link or
 * marker goes there. Its owner alone reads the file (pins: below). */
enum { SEGMENT = 65536, GROW = 262144, CHECKPOINT_BYTES = 262144 };
typedef struct { uint64_t start, end; } Extent;
typedef struct {
    uint64_t live;   /* bytes of the newest version's nodes in it, and of dead ones a pin still reads */
    uint64_t newest; /* the newest seq written into it (a commit's link and bytes, a marker) */
    uint64_t born;   /* no seq before it wrote what is in it (0: unknown, as at an open) */
    bool back;       /* given back whole, nothing written into it since */
} Segment;
/* Nodes that died at died while a pin may still read them: of key (empty:
 * the catalog), written no earlier than born. */
typedef struct { uint64_t start, end, born, died; char key[KEY + 1]; } Deferred;
typedef struct Ring {
    Segment *segments;
    size_t segment_count;
    Extent *free; /* sorted, apart */
    size_t free_count, free_room;
    uint64_t size;  /* the file's */
    uint64_t since; /* commit bytes since the newer slot's commit */
    Extent *wrote;  /* a commit's nodes */
    size_t wrote_count, wrote_room;
    uint64_t *kept; /* the stored nodes its new trees keep */
    size_t kept_count, kept_room;
    Deferred *deferred;
    size_t deferred_count, deferred_room;
    bool failed; /* a note it had no memory for: the commit does not publish */
} Ring;

static bool ring_room(void **array, size_t *room, size_t need, size_t size) {
    if (need <= *room) return true;
    size_t next = *room ? 2 * *room : 64;
    while (next < need) next *= 2;
    void *grown = realloc(*array, next * size);
    if (!grown) return false;
    *array = grown;
    *room = next;
    return true;
}

static void note_wrote(Ring *ring, uint64_t at, uint64_t bytes) {
    if (!ring_room((void **)&ring->wrote, &ring->wrote_room, ring->wrote_count + 1, sizeof(*ring->wrote))) ring->failed = true;
    else ring->wrote[ring->wrote_count++] = (Extent){at, at + bytes};
}

static void note_kept(Ring *ring, uint64_t at) {
    if (!ring_room((void **)&ring->kept, &ring->kept_room, ring->kept_count + 1, sizeof(*ring->kept))) ring->failed = true;
    else ring->kept[ring->kept_count++] = at;
}

static uint64_t put_branch(Writer *writer, const Entry *kids, size_t count, unsigned height) {
    static const unsigned char zero[8];
    put_bytes(writer, zero, (size_t)((8 - writer->at % 8) % 8));
    uint64_t at = writer->at;
    if (writer->ring) note_wrote(writer->ring, at, 8 + 16 * (uint64_t)count);
    unsigned char header[8];
    memcpy(header, branch_magic, sizeof(branch_magic));
    header[4] = (unsigned char)count;
    header[5] = (unsigned char)(count >> 8);
    header[6] = (unsigned char)height;
    header[7] = (unsigned char)(height >> 8);
    put_bytes(writer, header, sizeof(header));
    for (size_t i = 0; i < count; ++i) {
        unsigned char pair[16];
        encode64(pair, kids[i].ref);
        encode64(pair + 8, kids[i].bytes);
        put_bytes(writer, pair, sizeof(pair));
    }
    return at;
}

/* Append a subtree's arena nodes, children first, and set entry to the
 * stored reference. The arena is not changed, so a failed commit leaves a
 * draft intact. With relocate, stored nodes are copied too (compaction, or a
 * draft whose base file was replaced by compaction). */
static bool write_subtree(const Tree *tree, Writer *writer, Entry *entry, unsigned height,
                          bool relocate) {
    if (!entry->bytes || (!(entry->ref & MEMORY_BIT) && !relocate)) {
        if (writer->ring && entry->bytes) note_kept(writer->ring, entry->ref & LOCATION);
        return true;
    }
    if (!height) {
        const unsigned char *bytes = leaf_bytes(tree, *entry);
        if (!bytes) return false;
        entry->ref = LEAF_BIT | writer->at;
        if (writer->ring) note_wrote(writer->ring, writer->at, entry->bytes);
        put_bytes(writer, bytes, (size_t)entry->bytes);
        return !writer->failed;
    }
    Entry kids[FAN_MAX];
    size_t count = children(tree, *entry, height, kids);
    if (!count) return false;
    for (size_t i = 0; i < count; ++i)
        if (!write_subtree(tree, writer, &kids[i], height - 1, relocate)) return false;
    entry->ref = put_branch(writer, kids, count, height);
    return !writer->failed;
}

static bool write_root(const Tree *tree, Writer *writer, Entry *root, bool relocate) {
    unsigned height;
    return root_height(tree, *root, &height) && write_subtree(tree, writer, root, height, relocate);
}

/* How large a value under key may grow: a runtime-owned state's ('.', or no
 * key at all) or a document's (store.h). */
static uint64_t limit_of(const char *key) {
    return !key[0] || key[0] == '.' ? PURE_STATE_LIMIT : PURE_OBJECT_LIMIT;
}

static bool read_record(const Tree *tree, Entry catalog, uint64_t index,
                        unsigned char record[RECORD], Entry *document, int64_t *revision) {
    unsigned height;
    if (!root_height(tree, catalog, &height) ||
        !copy_range(tree, catalog, height, index * RECORD, RECORD, record)) return false;
    *document = (Entry){decode64(record + KEY), decode64(record + KEY + 8)};
    uint64_t stored = decode64(record + KEY + 16);
    *revision = (int64_t)stored;
    return stored && stored <= (uint64_t)INT64_MAX && !document->ref == !document->bytes &&
           document->bytes <= limit_of((const char *)record);
}

/* Binary search over records ordered by their zero-padded key bytes. */
static bool find(const Tree *tree, Entry catalog, const char *key, uint64_t *index,
                 bool *found, Entry *document, int64_t *revision) {
    unsigned char wanted[KEY] = {0}, record[RECORD];
    memcpy(wanted, key, strlen(key));
    uint64_t low = 0, high = catalog.bytes / RECORD;
    *found = false;
    *document = (Entry){0};
    *revision = 0;
    while (low < high) {
        uint64_t middle = low + (high - low) / 2;
        if (!read_record(tree, catalog, middle, record, document, revision)) return false;
        int order = memcmp(wanted, record, KEY);
        if (!order) { *index = middle; *found = true; return true; }
        if (order < 0) high = middle; else low = middle + 1;
    }
    *document = (Entry){0};
    *revision = 0;
    *index = low;
    return true;
}

static int create_temporary(int directory, char name[96]) {
    for (unsigned attempt = 0; attempt < 64; ++attempt) {
        uint64_t sequence = __atomic_add_fetch(&temporary_sequence, 1, __ATOMIC_RELAXED); /* writers of other stores too */
        if (sequence == UINT64_MAX) break;
        snprintf(name, 96, ".pending-%ld-%" PRIu64, (long)getpid(), sequence);
        int fd = openat(directory, name, O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd >= 0) return fd;
        if (errno != EEXIST) break;
    }
    name[0] = 0; /* Never remove an existing file on a name collision. */
    return -1;
}

/* Where a fresh file's written zeros should reach: past everything the cycle
 * to the next compaction will write (4 x live + slack more), so the commits
 * of that cycle neither grow the file nor take new blocks, whose sync costs
 * a journal commit (research 084: +22% commits, half the p99). No further
 * than the file it replaces reached, so a store never takes more space than
 * it did; a store whose live data shrank shrinks with it. */
static uint64_t cycle_end(const Head *head, uint64_t replaced) {
    uint64_t cycle = head->end + 4 * head->live + COMPACT_SLACK + TAIL_MIN;
    uint64_t end = cycle < replaced ? cycle : replaced;
    return end > tail_end(head->end) ? end : tail_end(head->end);
}

/* Publish a fresh file with both slots pointing at head. It is synced whole
 * before it is renamed into place, so its head names no commit bytes.
 * replaced: the size of the file it replaces (0 for none). */
static bool install(int directory, char temporary[96], int fd, Head *head, uint64_t replaced) {
    unsigned char slot[SLOT];
    struct stat file;
    head->start = head->end;
    head->check = digest_end(&(Digest){0});
    encode_head(slot, head->ring ? ring_magic : head_magic, head);
    zero_fill(fd, head->end, cycle_end(head, replaced));
    bool ok = write_at(fd, slot, SLOT, 0) && write_at(fd, slot, SLOT, BLOCK) && sync_fd(fd) && !fstat(fd, &file);
    if (close(fd)) ok = false;
    ok = ok && !renameat(directory, temporary, directory, tree_name);
    if (ok) temporary[0] = 0;
    ok = ok && !fault(PURE_INSTALL_BEFORE_DIR_SYNC) && sync_fd(directory);
    if (ok) remember(&file, head, DURABLE);
    return ok;
}

/* Rewrite only reachable nodes, read through the writer's mapping of the
 * file (replaced: its size). Open readers keep their mapping of the previous
 * file. A failure leaves the published file untouched; true when the new
 * file is in place. */
static bool compact(int directory, Mapping *mapping, uint64_t replaced, const Head *head, bool ring) {
    __atomic_add_fetch(&mapping->refs, 1, __ATOMIC_RELAXED);
    Tree tree = {.map = mapping->map, .end = head->end, .shared = mapping};
    char temporary[96];
    int fd = create_temporary(directory, temporary);
    Writer *writer = fd >= 0 ? malloc(sizeof(*writer)) : NULL;
    uint64_t count = head->catalog_bytes / RECORD;
    unsigned char *records = count ? malloc((size_t)(count * RECORD)) : NULL;
    Entry catalog = {head->catalog, head->catalog_bytes};
    unsigned height;
    bool ok = writer && (!count || records) && root_height(&tree, catalog, &height) &&
              (!count || copy_range(&tree, catalog, height, 0, count * RECORD, records));
    if (writer) *writer = (Writer){.fd = fd, .at = DATA};
    for (uint64_t i = 0; ok && i < count; ++i) {
        unsigned char *record = records + i * RECORD;
        Entry document = {decode64(record + KEY), decode64(record + KEY + 8)};
        unsigned document_height;
        ok = root_height(&tree, document, &document_height) &&
             write_subtree(&tree, writer, &document, document_height, true);
        encode64(record + KEY, document.ref);
    }
    Entry fresh = {0};
    ok = ok && rebuild(&tree, (Entry){0}, 0, 0, (PureSpan){records, (size_t)(count * RECORD)}, &fresh) &&
         write_root(&tree, writer, &fresh, false) && flush(writer);
    if (ok) {
        Head compacted = {head->seq + 1, writer->at, fresh.ref, fresh.bytes, writer->at - DATA, 0, 0, ring};
        ok = install(directory, temporary, fd, &compacted, replaced);
        fd = -1;
    }
    if (fd >= 0) close(fd);
    if (*temporary) unlinkat(directory, temporary, 0);
    free(records);
    free(writer);
    release_tree(&tree);
    return ok;
}

/* The writer's lock, without waiting. On Linux it is the open file
 * description's: no other descriptor of the lock file, opened and closed in
 * this process, lets it go (a process's POSIX lock goes with any). */
#if defined(__linux__) && !defined(F_OFD_SETLK)
#define F_OFD_SETLK 37
#endif
static bool lock_writer(int lock) {
    struct flock hold = {.l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 0, .l_len = 0};
#ifdef F_OFD_SETLK
    return fcntl(lock, F_OFD_SETLK, &hold) == 0;
#else
    return fcntl(lock, F_SETLK, &hold) == 0;
#endif
}

PureResult pure_store_init(const char *path) {
    if (!path || !*path) return PURE_STORE_ERROR;
    bool created = mkdir(path, 0700) == 0;
    if (!created && errno != EEXIST) return PURE_STORE_ERROR;
    int directory = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (directory < 0) return PURE_STORE_ERROR; /* Never reinterpret an old DB file. */
    Turns *turns = turns_of(directory);
    if (!turns) {
        close(directory);
        return PURE_STORE_ERROR;
    }
    pthread_mutex_lock(&turns->writer);
    /* A store this process writes is there, and its lock file is the
     * writer's: closing any descriptor of it would drop the lock. */
    pthread_mutex_lock(&turns->lock);
    bool writing = turns->holding && turns->holding->holders && turns->holding->forks == forks;
    pthread_mutex_unlock(&turns->lock);
    if (writing) {
        pthread_mutex_unlock(&turns->writer);
        close(directory);
        return PURE_OK;
    }
    int lock = openat(directory, ".writer.lock", O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    struct stat info;
    bool ok = lock >= 0 && fstat(lock, &info) == 0 && S_ISREG(info.st_mode);
    /* A file there is read as readers read it, without the lock (a store's
     * writer holds it for its life). A missing one is made under the lock,
     * looking again once it is held: another may have made it. While the file
     * is missing only another process making it can hold the lock (a writer
     * needs the file): it is waited for a little, looking again. */
    PureResult made = PURE_STORE_ERROR;
    if (ok) {
        int fd = openat(directory, tree_name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        for (int tries = 0; fd < 0 && errno == ENOENT; ++tries) {
            if (lock_writer(lock)) {
                if ((fd = openat(directory, tree_name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC)) < 0 && errno == ENOENT) {
                    char temporary[96];
                    int created_fd = create_temporary(directory, temporary);
                    Head head = {.seq = 1, .end = DATA};
                    if (created_fd >= 0 && install(directory, temporary, created_fd, &head, 0)) made = PURE_OK;
                    if (*temporary) unlinkat(directory, temporary, 0);
                }
                break;
            }
            if ((errno != EACCES && errno != EAGAIN) || tries == 200) {
                made = errno == EACCES || errno == EAGAIN ? PURE_BUSY : PURE_STORE_ERROR;
                break;
            }
            nanosleep(&(struct timespec){0, 10000000}, NULL);
            fd = openat(directory, tree_name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        }
        if (fd >= 0) {
            Head head;
            struct stat file;
            if (read_head(fd, &file, false, &head, NULL)) made = PURE_OK; /* An unreadable file is never replaced. */
            close(fd);
        }
    }
    ok = made == PURE_OK && sync_fd(lock) && sync_fd(directory);
    if (lock >= 0) close(lock);
    pthread_mutex_unlock(&turns->writer);
    close(directory);
    if (made == PURE_BUSY) return PURE_BUSY;
    if (ok && created) {
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

/* A ring is read only through its owner (030 S2): a path read of one holds
 * its writer for the read (from another process, its lock: busy while its
 * owner runs), so that no commit reuses what it reads. *owned stays NULL
 * for a file of the format before, read as it is. */
static PureResult hold(Turns *turns, const char *path);
static void let_go(Turns *turns);
static PureResult own_ring(const char *path, int fd, Turns **owned) {
    unsigned char magic[2][8];
    *owned = NULL;
    if (!read_at(fd, magic[0], 8, 0) || !read_at(fd, magic[1], 8, BLOCK) ||
        (memcmp(magic[0], ring_magic, 8) && memcmp(magic[1], ring_magic, 8))) return PURE_OK;
    int directory = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    Turns *turns = directory >= 0 ? turns_of(directory) : NULL;
    if (directory >= 0) close(directory);
    if (!turns) return PURE_STORE_ERROR;
    pthread_mutex_lock(&turns->writer);
    PureResult result = hold(turns, path);
    if (result != PURE_OK) {
        pthread_mutex_unlock(&turns->writer);
        return result;
    }
    *owned = turns;
    return PURE_OK;
}

static void disown(Turns *turns) {
    if (!turns) return;
    let_go(turns);
    pthread_mutex_unlock(&turns->writer);
}

PureResult pure_store_select(const char *path, const char *key, size_t offset,
                             size_t length, bool whole, size_t minimum,
                             size_t available, PureStoredView *out) {
    *out = (PureStoredView){0};
    if (!valid_key(key)) return PURE_STORE_ERROR;
    int directory = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (directory < 0) return PURE_STORE_ERROR;
    int fd = openat(directory, tree_name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    close(directory);
    if (fd < 0) return PURE_STORE_ERROR;
    Tree tree = {0};
    Head head;
    struct stat file;
    Entry document;
    uint64_t index;
    int64_t revision;
    bool found;
    unsigned height;
    Turns *owned;
    PureResult result = own_ring(path, fd, &owned);
    if (result != PURE_OK) {
        close(fd);
        return result;
    }
    result = PURE_STORE_ERROR;
    if (!read_head(fd, &file, false, &head, NULL) || !map_tree(fd, &file, &head, &tree) ||
        !find(&tree, (Entry){head.catalog, head.catalog_bytes}, key, &index, &found,
              &document, &revision)) goto finish;
    if (!found) { result = PURE_NOT_FOUND; goto finish; }
    size_t size = (size_t)document.bytes;
    if (size < minimum || offset > size) { result = PURE_LIMIT; goto finish; }
    if (whole) length = size - offset;
    if (length > size - offset) { result = PURE_LIMIT; goto finish; }
    if (length > available) { result = PURE_BUDGET; goto finish; }
    if (length) {
        if (!root_height(&tree, document, &height)) goto finish;
        /* (a ring's bytes are copied out: they may be reused once the read ends) */
        const unsigned char *direct = head.ring ? NULL : single_leaf(&tree, document, height, offset, length);
        void *mapping;
        size_t mapped_size, prefix = 0;
        if (direct) {
            /* One extent: map it in place, as the flat store does. */
            long page = sysconf(_SC_PAGESIZE);
            if (page <= 0) goto finish;
            size_t start = (size_t)(direct - tree.map), aligned = start - start % (size_t)page;
            prefix = start - aligned;
            mapped_size = prefix + length;
            mapping = mmap(NULL, mapped_size, PROT_READ, MAP_PRIVATE, fd, (off_t)aligned);
            if (mapping == MAP_FAILED) goto finish;
        } else {
            /* A contiguous view of several extents is assembled for this API. */
            mapped_size = length;
            mapping = mmap(NULL, mapped_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (mapping == MAP_FAILED) goto finish;
            if (!copy_range(&tree, document, height, offset, length, mapping) ||
                mprotect(mapping, mapped_size, PROT_READ)) {
                munmap(mapping, mapped_size);
                goto finish;
            }
        }
        out->mapping = mapping;
        out->mapped_size = mapped_size;
        out->body = (PureSpan){(const unsigned char *)mapping + prefix, length};
    }
    out->revision = revision;
    out->object_size = size;
    result = PURE_OK;
finish:
    release_tree(&tree);
    close(fd);
    disown(owned);
    return result;
}

PureResult pure_store_get(const char *path, const char *key, size_t available, PureStoredView *out) {
    return pure_store_select(path, key, 0, 0, true, 0, available, out);
}

PureResult pure_store_get_range(const char *path, const char *key, size_t offset,
                                size_t length, size_t available, PureStoredView *out) {
    return pure_store_select(path, key, offset, length, false, 0, available, out);
}

/* Keep written zeros after the published end, where commits and their
 * unpublished leftovers go. A sync of blocks the file already has writes
 * only them; a new size or new blocks add a journal commit (on an SD card
 * 2.4 ms against 6 ms per commit). The zeros become durable with the next
 * commit's sync. */
/* The ring's free extents: one added (merged with those it meets), one
 * taken away, the end of the one an offset is in, the lowest part of one
 * outside avoid that holds need. False without memory. */
static bool free_add(Ring *ring, uint64_t start, uint64_t end) {
    if (start >= end) return true;
    size_t i = 0;
    while (i < ring->free_count && ring->free[i].end < start) ++i;
    size_t j = i;
    while (j < ring->free_count && ring->free[j].start <= end) {
        if (ring->free[j].start < start) start = ring->free[j].start;
        if (ring->free[j].end > end) end = ring->free[j].end;
        ++j;
    }
    if (j == i) {
        if (!ring_room((void **)&ring->free, &ring->free_room, ring->free_count + 1, sizeof(*ring->free))) return false;
        memmove(&ring->free[i + 1], &ring->free[i], (ring->free_count - i) * sizeof(*ring->free));
        ++ring->free_count;
    } else if (j > i + 1) {
        memmove(&ring->free[i + 1], &ring->free[j], (ring->free_count - j) * sizeof(*ring->free));
        ring->free_count -= j - i - 1;
    }
    ring->free[i] = (Extent){start, end};
    return true;
}

static bool free_take(Ring *ring, uint64_t start, uint64_t end) {
    for (size_t i = 0; i < ring->free_count; ++i) {
        Extent e = ring->free[i];
        if (e.end <= start || e.start >= end) continue;
        if (e.start < start && e.end > end) {
            if (!ring_room((void **)&ring->free, &ring->free_room, ring->free_count + 1, sizeof(*ring->free))) return false;
            memmove(&ring->free[i + 2], &ring->free[i + 1], (ring->free_count - i - 1) * sizeof(*ring->free));
            ++ring->free_count;
            ring->free[i] = (Extent){e.start, start};
            ring->free[i + 1] = (Extent){end, e.end};
            return true;
        }
        if (e.start < start) ring->free[i].end = start;
        else if (e.end > end) ring->free[i].start = end;
        else {
            memmove(&ring->free[i], &ring->free[i + 1], (ring->free_count - i - 1) * sizeof(*ring->free));
            --ring->free_count;
            --i;
        }
    }
    return true;
}

static uint64_t free_end(const Ring *ring, uint64_t at) {
    for (size_t i = 0; i < ring->free_count; ++i)
        if (ring->free[i].start <= at && at < ring->free[i].end) return ring->free[i].end;
    return at;
}

static bool free_lowest(const Ring *ring, uint64_t need, uint64_t avoid, uint64_t avoid_end, Extent *part) {
    for (size_t i = 0; i < ring->free_count; ++i) {
        Extent e = ring->free[i];
        Extent parts[2] = {{e.start, e.end < avoid ? e.end : avoid}, {e.start > avoid_end ? e.start : avoid_end, e.end}};
        for (unsigned k = 0; k < 2; ++k)
            if (parts[k].start < parts[k].end && parts[k].end - parts[k].start >= need) {
                *part = parts[k];
                return true;
            }
    }
    return false;
}

/* Segments to the file's size; a range's bytes into or out of their live
 * counts; a range written by seq. */
static bool reach(Ring *ring, uint64_t size) {
    size_t count = (size_t)((size + SEGMENT - 1) / SEGMENT);
    if (count <= ring->segment_count) return true;
    Segment *grown = realloc(ring->segments, count * sizeof(*grown));
    if (!grown) return false;
    memset(grown + ring->segment_count, 0, (count - ring->segment_count) * sizeof(*grown));
    ring->segments = grown;
    ring->segment_count = count;
    return true;
}

static void count_live(Ring *ring, uint64_t start, uint64_t end, bool add) {
    for (uint64_t s = start / SEGMENT; s * SEGMENT < end && s < ring->segment_count; ++s) {
        uint64_t lo = start > s * SEGMENT ? start : s * SEGMENT, hi = end < (s + 1) * SEGMENT ? end : (s + 1) * SEGMENT;
        Segment *g = &ring->segments[s];
        if (add) g->live += hi - lo, g->back = false;
        else g->live = g->live >= hi - lo ? g->live - (hi - lo) : 0;
    }
}

static void count_written(Ring *ring, uint64_t start, uint64_t end, uint64_t seq) {
    for (uint64_t s = start / SEGMENT; s * SEGMENT < end && s < ring->segment_count; ++s) {
        Segment *g = &ring->segments[s];
        if (g->back) g->born = seq;
        if (g->newest < seq) g->newest = seq;
        g->back = false;
    }
}

static uint64_t born_in(const Ring *ring, uint64_t start, uint64_t end) {
    uint64_t born = UINT64_MAX;
    for (uint64_t s = start / SEGMENT; s * SEGMENT < end && s < ring->segment_count; ++s)
        if (ring->segments[s].born < born) born = ring->segments[s].born;
    return born == UINT64_MAX ? 0 : born;
}

/* Whether a live pin may read what key (empty: the catalog) held from born
 * until died: one of that key or of every key, of a version between. */
static bool pinned_in(const Mapping *pins, const char *key, uint64_t born, uint64_t died) {
    for (const Mapping *p = pins; p; p = p->next)
        if (__atomic_load_n(&p->refs, __ATOMIC_ACQUIRE) && (!p->key[0] || (key[0] && !strcmp(p->key, key))) &&
            born <= p->version && p->version < died) return true;
    return false;
}

/* Nodes that died: no longer live, or deferred while a pin may read them. */
static void count_death(Ring *ring, const Mapping *pins, uint64_t start, uint64_t end, const char *key, uint64_t died) {
    uint64_t born = born_in(ring, start, end);
    if (!pinned_in(pins, key, born, died)) {
        count_live(ring, start, end, false);
        return;
    }
    if (!ring_room((void **)&ring->deferred, &ring->deferred_room, ring->deferred_count + 1, sizeof(*ring->deferred))) {
        ring->failed = true; /* kept live: its segment does not come back until the ring is read again */
        return;
    }
    Deferred *d = &ring->deferred[ring->deferred_count++];
    *d = (Deferred){start, end, born, died, {0}};
    memcpy(d->key, key, strlen(key) + 1);
}

/* Segments that nothing lives in and that no commit from the older slot's
 * head on (H_rec) is in come back whole. */
static uint64_t h_rec(const Session *session) {
    uint64_t a = session->slots[0], b = session->slots[1];
    return !a ? b : !b ? a : a < b ? a : b;
}

static bool give_back(Ring *ring, uint64_t rec) {
    for (size_t s = 0; s < ring->segment_count; ++s) {
        Segment *g = &ring->segments[s];
        if (g->back || g->live || g->newest >= rec) continue;
        uint64_t lo = s * SEGMENT < DATA ? DATA : s * SEGMENT, hi = (s + 1) * SEGMENT < ring->size ? (s + 1) * SEGMENT : ring->size;
        if (lo < hi && !free_add(ring, lo, hi)) return false;
        g->back = true;
    }
    return true;
}

static int compare_offsets(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

static bool kept_at(const Ring *ring, uint64_t at) {
    size_t lo = 0, hi = ring->kept_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (ring->kept[mid] < at) lo = mid + 1;
        else hi = mid;
    }
    return lo < ring->kept_count && ring->kept[lo] == at;
}

/* The nodes of a tree a commit replaced that its new trees do not keep: they
 * no longer live (a damaged one stays counted: its segment never comes back). */
static void count_dead(Ring *ring, const Mapping *pins, const Tree *tree, Entry entry, unsigned height, const char *key,
                       uint64_t died) {
    if (!entry.bytes || (entry.ref & MEMORY_BIT)) return;
    uint64_t at = entry.ref & LOCATION;
    if (kept_at(ring, at)) return;
    if (!height) {
        count_death(ring, pins, at, at + entry.bytes, key, died);
        return;
    }
    Entry kids[FAN_MAX];
    size_t count = children(tree, entry, height, kids);
    if (!count) return;
    count_death(ring, pins, at, at + 8 + 16 * (uint64_t)count, key, died);
    for (size_t i = 0; i < count; ++i) count_dead(ring, pins, tree, kids[i], height - 1, key, died);
}

static void free_ring(Ring *ring) {
    if (!ring) return;
    free(ring->segments);
    free(ring->free);
    free(ring->wrote);
    free(ring->kept);
    free(ring->deferred);
    free(ring);
}

/* As a ring opens: what lives (the newest version's nodes) and what a start
 * may follow (the commits from the older slot's head on) are in use, every
 * other byte of [DATA, size) is free; segments wholly free are back. */
typedef struct { Ring *ring; Extent *used; size_t count, room; bool failed; } Opening;

static void opening_use(Opening *o, uint64_t start, uint64_t end) {
    if (!ring_room((void **)&o->used, &o->room, o->count + 1, sizeof(*o->used))) o->failed = true;
    else o->used[o->count++] = (Extent){start, end};
}

static bool opening_tree(Opening *o, const Tree *tree, Entry entry, unsigned height) {
    if (!entry.bytes) return true;
    if (entry.ref & MEMORY_BIT) return false;
    uint64_t at = entry.ref & LOCATION;
    if (!height) {
        count_live(o->ring, at, at + entry.bytes, true);
        opening_use(o, at, at + entry.bytes);
        return true;
    }
    Entry kids[FAN_MAX];
    size_t count = children(tree, entry, height, kids);
    if (!count) return false;
    count_live(o->ring, at, at + 8 + 16 * (uint64_t)count, true);
    opening_use(o, at, at + 8 + 16 * (uint64_t)count);
    for (size_t i = 0; i < count; ++i)
        if (!opening_tree(o, tree, kids[i], height - 1)) return false;
    return true;
}

static int compare_starts(const void *a, const void *b) {
    uint64_t x = ((const Extent *)a)->start, y = ((const Extent *)b)->start;
    return (x > y) - (x < y);
}

static bool ring_open(Session *session) {
    Ring *ring = calloc(1, sizeof(*ring));
    Opening o = {ring, NULL, 0, 0, false};
    const Tree *tree = &session->tree;
    Entry catalog = {session->head.catalog, session->head.catalog_bytes};
    uint64_t count = catalog.bytes / RECORD;
    unsigned char *records = count ? malloc((size_t)(count * RECORD)) : NULL;
    unsigned height;
    bool ok = ring && (!count || records) && (ring->size = (uint64_t)session->file.st_size, reach(ring, ring->size)) &&
              root_height(tree, catalog, &height) && opening_tree(&o, tree, catalog, height) &&
              (!count || copy_range(tree, catalog, height, 0, count * RECORD, records));
    for (uint64_t i = 0; ok && i < count; ++i) {
        Entry document = {decode64(records + i * RECORD + KEY), decode64(records + i * RECORD + KEY + 8)};
        ok = root_height(tree, document, &height) && opening_tree(&o, tree, document, height);
    }
    free(records);
    /* the chain from the older slot's head that counts */
    unsigned char slots[BLOCK + SLOT];
    uint64_t rec = h_rec(session);
    Head head = {0};
    ok = ok && read_at(session->fd, slots, sizeof(slots), 0);
    for (unsigned i = 0; ok && i < 2 && !head.seq; ++i)
        if (decode_head(slots + i * BLOCK, false, &head) && head.seq != rec) head = (Head){0};
    ok = ok && head.seq && head.end <= ring->size;
    if (ok) {
        opening_use(&o, head.start, head.end);
        count_written(ring, head.start, head.end, head.seq);
    }
    while (ok && head.seq < session->head.seq) {
        Head next;
        ok = follow(session->fd, &session->file, true, &head, &next);
        if (!ok) break;
        if (next.start - LINK != head.end) { /* a marker at head's end named it */
            opening_use(&o, head.end, head.end + LINK);
            count_written(ring, head.end, head.end + LINK, next.seq);
        }
        opening_use(&o, next.start - LINK, next.end);
        count_written(ring, next.start - LINK, next.end, next.seq);
        head = next;
    }
    ok = ok && !o.failed && head.seq == session->head.seq;
    if (ok) {
        if (o.count) qsort(o.used, o.count, sizeof(*o.used), compare_starts);
        uint64_t at = DATA;
        for (size_t i = 0; ok && i <= o.count; ++i) {
            uint64_t until = i < o.count ? o.used[i].start : ring->size;
            if (until > at) ok = free_add(ring, at, until);
            if (i < o.count && o.used[i].end > at) at = o.used[i].end;
        }
        for (size_t s = 0; s < ring->segment_count; ++s)
            ring->segments[s].back = !ring->segments[s].live && !ring->segments[s].newest;
    }
    free(o.used);
    if (!ok) {
        free_ring(ring);
        return false;
    }
    free_ring(session->ring);
    session->ring = ring;
    return true;
}

static void prepare_tail(Session *session) {
    uint64_t size = (uint64_t)session->file.st_size, end = session->head.end;
    if (size >= end + TAIL_MIN) return;
    zero_fill(session->fd, size > end ? size : end, tail_end(end));
    struct stat file;
    if (!fstat(session->fd, &file)) session->file.st_size = file.st_size;
}

/* A commit's edits, written or given up: the arena's nodes go, the mapping stays. */
static void clear_arena(Tree *tree) {
    for (size_t i = 0; i < tree->count; ++i) {
        free(tree->nodes[i].bytes);
        free(tree->nodes[i].children);
    }
    tree->count = tree->free_head = tree->free_count = tree->bytes = 0;
}

/* Map the file to its size when the mapping ends before the head's bytes do
 * (a new file, a grown tail): one mapping serves every version until then. */
static bool cover(Session *session) {
    Mapping *mapping = session->tree.shared;
    if (!mapping || mapping->length < session->head.end) {
        struct stat file;
        if (fstat(session->fd, &file) || (uint64_t)file.st_size < session->head.end) return false;
        session->file.st_size = file.st_size;
        Mapping *fresh = malloc(sizeof(*fresh));
        void *map = fresh ? mmap(NULL, (size_t)file.st_size, PROT_READ, MAP_SHARED, session->fd, 0) : MAP_FAILED;
        if (map == MAP_FAILED) { free(fresh); return false; }
        *fresh = (Mapping){.map = map, .length = (uint64_t)file.st_size, .refs = 1};
        release_mapping(mapping);
        session->tree.map = map;
        session->tree.shared = fresh;
    }
    session->tree.end = session->head.ring ? session->tree.shared->length : session->head.end;
    return true;
}

static void close_session(Session *session) {
    free_ring(session->ring);
    release_tree(&session->tree);
    if (session->fd >= 0) close(session->fd);
    if (session->lock >= 0) close(session->lock); /* the process's writer lock goes with it */
    if (session->directory >= 0) close(session->directory);
    *session = (Session){.directory = -1, .lock = -1, .fd = -1};
}

/* What a crash may have left where the next commit's record goes (a commit
 * passed over, one part short of counting), and a slot that leads to no
 * commit, cleared before anything is built on head (and made durable with
 * it). Else a second crash could bring such a commit back: the next commit
 * at its place, landing only bytes equal to its own (its record's first
 * bytes, or the part its bytes lacked), would make it count again, though the
 * store had passed over it and gone on. */
static uint64_t record_room(void);
static bool clear_debris(int fd, const struct stat *file, const Head *head, const uint64_t slots[2], bool *cleared) {
    static const unsigned char zero[SLOT];
    unsigned char bytes[SLOT];
    uint64_t room = record_room();
    if (head->end + room <= (uint64_t)file->st_size) {
        if (!read_at(fd, bytes, (size_t)room, head->end)) return false;
        if (memcmp(bytes, zero, (size_t)room)) {
            if (!write_at(fd, zero, (size_t)room, head->end)) return false;
            *cleared = true;
        }
    }
    for (unsigned i = 0; i < 2; ++i) {
        if (slots[i]) continue;
        if (!read_at(fd, bytes, SLOT, i * BLOCK)) return false;
        if (memcmp(bytes, zero, SLOT)) {
            if (!write_at(fd, zero, SLOT, i * BLOCK)) return false;
            *cleared = true;
        }
    }
    return true;
}

/* Read the head of the file at the store's name, and map it. A head this
 * process did not make durable may be the last write of a writer that died
 * before its sync: it is made durable before anything builds on it. So is
 * the name of a new file (compaction's) unless it is known to be (synced):
 * a crash would bring back the file it replaced, and drop what was built on
 * the new one. */
static PureResult reopen(Session *session, bool synced) {
    int fd = openat(session->directory, tree_name, O_RDWR | O_NOFOLLOW | O_CLOEXEC);
    struct stat file;
    if (fd < 0 || fstat(fd, &file) ||
        (!synced && session->fd >= 0 && (file.st_dev != session->file.st_dev || file.st_ino != session->file.st_ino) &&
         !sync_fd(session->directory))) {
        if (fd >= 0) close(fd);
        return PURE_STORE_ERROR;
    }
    if (session->fd >= 0) close(session->fd);
    session->fd = fd;
    release_tree(&session->tree); /* its mapping may be of a file compaction replaced */
    bool cleared = false;
    if (!read_head(fd, &session->file, true, &session->head, session->slots) ||
        !clear_debris(fd, &session->file, &session->head, session->slots, &cleared)) return PURE_STORE_ERROR;
    if (cleared || recall(&session->file, &session->head) != DURABLE) {
        if (!sync_data(fd)) return PURE_STORE_ERROR;
        remember(&session->file, &session->head, DURABLE);
    }
    if (session->head.ring) return cover(session) && ring_open(session) ? PURE_OK : PURE_STORE_ERROR;
    prepare_tail(session);
    return cover(session) ? PURE_OK : PURE_STORE_ERROR;
}

/* Handles see the session's head from now on. */
static void show_newest(Turns *turns, Holding *holding) {
    Mapping *mapping = holding->session.tree.shared;
    __atomic_add_fetch(&mapping->refs, 1, __ATOMIC_RELAXED);
    pthread_mutex_lock(&turns->lock);
    Mapping *old = holding->mapping;
    holding->newest = holding->session.head;
    holding->mapping = mapping;
    holding->device = holding->session.file.st_dev;
    holding->inode = holding->session.file.st_ino;
    pthread_mutex_unlock(&turns->lock);
    release_mapping(old);
}

/* One more holder of an open writer, without waiting for a commit in it: a
 * handle opened meanwhile can join that commit's group. */
static bool join(Turns *turns) {
    pthread_mutex_lock(&turns->lock);
    Holding *holding = turns->holding;
    bool joined = holding && holding->holders && holding->forks == forks;
    if (joined) ++holding->holders;
    pthread_mutex_unlock(&turns->lock);
    return joined;
}

/* After a commit that may or may not be stored: the head the file has now,
 * made durable (the commit's, when its bytes are whole), read at once, so
 * that what handles see, and what an app starting again on one finds, is
 * what the store has. Left unsure when it cannot. */
static void settle_uncertain(Turns *turns, Holding *holding) {
    if (reopen(&holding->session, false) != PURE_OK) return;
    holding->stale = false;
    show_newest(turns, holding);
}

/* What dead writers' compactions left (their temporary files): under the
 * lock no other writer can be making one. */
static void sweep(int directory) {
    int listing = openat(directory, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    DIR *entries = listing >= 0 ? fdopendir(listing) : NULL;
    if (!entries) {
        if (listing >= 0) close(listing);
        return;
    }
    for (struct dirent *entry; (entry = readdir(entries));)
        if (!strncmp(entry->d_name, ".pending-", 9)) unlinkat(directory, entry->d_name, 0);
    closedir(entries);
}

/* With turns->writer held: one more holder of the process's writer on the
 * store at path, which opens it, or reads its head again when a commit left
 * it unsure. A forked child lets its copy of the parent's go first (the lock
 * is the parent's) and opens its own. */
static PureResult hold(Turns *turns, const char *path) {
    if (!turns->holding) {
        Holding *made = calloc(1, sizeof(*made));
        if (!made) return PURE_ALLOCATION;
        made->session = (Session){.directory = -1, .lock = -1, .fd = -1};
        made->turns = turns;
        pthread_mutex_lock(&turns->lock);
        turns->holding = made;
        pthread_mutex_unlock(&turns->lock);
    }
    Holding *holding = turns->holding;
    pthread_mutex_lock(&turns->lock);
    bool revived = holding->closing && holding->forks == forks; /* its pins kept it open */
    if (revived) holding->closing = false, holding->holders = 1;
    pthread_mutex_unlock(&turns->lock);
    if (revived) return PURE_OK;
    if (join(turns)) {
        if (!holding->stale) return PURE_OK;
        PureResult result = reopen(&holding->session, false);
        if (result == PURE_OK) {
            holding->stale = false;
            show_newest(turns, holding);
            return PURE_OK;
        }
        pthread_mutex_lock(&turns->lock);
        --holding->holders; /* others hold it still: it stays open, unsure */
        pthread_mutex_unlock(&turns->lock);
        return result;
    }
    Session *session = &holding->session;
    pthread_once(&fork_watch, watch_forks);
    close_session(session); /* none, or a forked child's copy of its parent's */
    struct stat info;
    session->directory = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (session->directory < 0 || fstat(session->directory, &info) || info.st_dev != turns->device ||
        info.st_ino != turns->inode) { close_session(session); return PURE_STORE_ERROR; }
    session->lock = openat(session->directory, ".writer.lock", O_RDWR | O_NOFOLLOW | O_CLOEXEC);
    PureResult result = PURE_STORE_ERROR;
    if (session->lock >= 0 && !fstat(session->lock, &info) && S_ISREG(info.st_mode)) {
        if (lock_writer(session->lock)) {
            if (!turns->swept) sweep(session->directory); /* once a process (another's crash meanwhile: the next start's) */
            turns->swept = true;
            result = reopen(session, false);
        } else result = errno == EACCES || errno == EAGAIN ? PURE_BUSY : PURE_STORE_ERROR;
    }
    if (result != PURE_OK) { close_session(session); return result; }
    holding->stale = false;
    show_newest(turns, holding);
    pthread_mutex_lock(&turns->lock);
    holding->forks = forks;
    holding->holders = 1;
    pthread_mutex_unlock(&turns->lock);
    return PURE_OK;
}

/* The writer closed (and its lock let go), with turns->writer held and no
 * holder or live pin left: pins let go meanwhile are freed. */
static void close_holding(Turns *turns, Holding *holding) {
    pthread_mutex_lock(&turns->lock);
    Mapping *old = holding->mapping, *pins = holding->pins;
    holding->mapping = NULL;
    holding->newest = (Head){0};
    holding->pins = NULL;
    holding->closing = false;
    pthread_mutex_unlock(&turns->lock);
    release_mapping(old);
    while (pins) {
        Mapping *next = pins->next;
        release_mapping(pins->region);
        free(pins);
        pins = next;
    }
    close_session(&holding->session);
    free(holding->buffer);
    holding->buffer = NULL;
}

/* With turns->writer held: the last holder closes the writer (and its lock),
 * unless a ring's readers still hold pins: then the last of those does. */
static void let_go(Turns *turns) {
    Holding *holding = turns->holding;
    clear_arena(&holding->session.tree);
    pthread_mutex_lock(&turns->lock);
    bool last = !--holding->holders, pinned = last && holding->held_pins;
    if (pinned) holding->closing = true;
    pthread_mutex_unlock(&turns->lock);
    if (last && !pinned) close_holding(turns, holding);
}

/* A pin on version of a ring (key, or every key when empty), through region,
 * made with turns->lock held: the version is the newest, or one a pin the
 * caller holds already keeps. NULL without memory. */
static Mapping *pin_at(Holding *holding, Mapping *region, uint64_t version, const char *key) {
    Mapping *pin = calloc(1, sizeof(*pin));
    if (!pin || !region) {
        free(pin);
        return NULL;
    }
    __atomic_add_fetch(&region->refs, 1, __ATOMIC_RELAXED);
    *pin = (Mapping){region->map, region->length, 1, region, holding->turns, version, holding->pins, {0}};
    memcpy(pin->key, key, strlen(key) + 1);
    holding->pins = pin;
    ++holding->held_pins;
    return pin;
}

/* A pin no one holds any longer (its deaths go at the writer's next commit);
 * the last one of a holding whose holders are gone closes it. */
static void let_pin_go(Mapping *pin) {
    Turns *turns = pin->turns;
    pthread_mutex_lock(&turns->lock);
    Holding *holding = turns->holding;
    bool close = holding && !--holding->held_pins && !holding->holders && holding->closing;
    if (close) holding->closing = false;
    pthread_mutex_unlock(&turns->lock);
    if (!close) return;
    pthread_mutex_lock(&turns->writer);
    pthread_mutex_lock(&turns->lock);
    close = !holding->holders && !holding->held_pins;
    pthread_mutex_unlock(&turns->lock);
    if (close) close_holding(turns, holding);
    pthread_mutex_unlock(&turns->writer);
}

#ifdef PURE_STORE_TEST
/* Test-only until apps' stores are rings (030 S2): the store at path becomes
 * one, by a compaction into the ring's format. */
PureResult pure_tree_test_ring(const char *path) {
    int directory = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    Turns *turns = directory >= 0 ? turns_of(directory) : NULL;
    if (directory >= 0) close(directory);
    if (!turns) return PURE_STORE_ERROR;
    pthread_mutex_lock(&turns->writer);
    PureResult result = hold(turns, path);
    if (result == PURE_OK) {
        Holding *holding = turns->holding;
        Session *session = &holding->session;
        if (!session->head.ring) {
            result = PURE_STORE_ERROR;
            if (!fstat(session->fd, &session->file)) {
                bool synced =
                    compact(session->directory, session->tree.shared, (uint64_t)session->file.st_size, &session->head, true);
                result = reopen(session, synced);
            }
            if (result == PURE_OK) show_newest(turns, holding);
            else holding->stale = true;
        }
        let_go(turns);
    }
    pthread_mutex_unlock(&turns->writer);
    return result;
}

/* A ring's free bytes, in extents, and its file's size. */
void pure_tree_test_ring_space(const char *path, uint64_t *free_bytes, uint64_t *extents, uint64_t *size) {
    int directory = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    Turns *turns = directory >= 0 ? turns_of(directory) : NULL;
    if (directory >= 0) close(directory);
    *free_bytes = *extents = *size = 0;
    if (!turns) return;
    pthread_mutex_lock(&turns->writer);
    if (hold(turns, path) == PURE_OK) {
        const Ring *ring = turns->holding->session.ring;
        for (size_t i = 0; ring && i < ring->free_count; ++i) *free_bytes += ring->free[i].end - ring->free[i].start;
        if (ring) *extents = ring->free_count, *size = ring->size;
        let_go(turns);
    }
    pthread_mutex_unlock(&turns->writer);
}
#endif

/* Room before a commit's bytes for its record: a link (a test can still
 * write the format before, to read it). */
#ifdef PURE_STORE_TEST
static bool earlier_records;
void pure_tree_test_earlier_records(bool earlier) { earlier_records = earlier; }
static uint64_t record_room(void) { return earlier_records ? SLOT : LINK; }
/* and checkpoints of the format before: a slot named its own commit */
static bool earlier_slots;
void pure_tree_test_earlier_slots(bool earlier) { earlier_slots = earlier; }
#else
static uint64_t record_room(void) { return LINK; }
#endif

/* The writer's buffer, at the start of a commit's bytes. */
static Writer *writer_of(Holding *holding) {
    Writer *writer = holding->buffer ? holding->buffer : (holding->buffer = malloc(sizeof(*writer)));
    if (!writer) return NULL;
    writer->fd = holding->session.fd;
    writer->at = holding->session.head.end + record_room(); /* a commit's bytes start after room for its record */
    writer->used = 0;
    writer->failed = writer->over = false;
    writer->limit = 0;
    writer->ring = NULL;
    writer->digest = (Digest){0};
    return writer;
}

/* The catalog with each document's record (revisions come in as the base
 * and leave as the new one), written at writer; olds gets each document's
 * root before (0: none). */
static bool write_catalog(Session *session, Writer *writer, size_t count, const char *const *keys,
                          const Entry *documents, const int64_t *revisions, Entry *catalog, Entry *olds) {
    *catalog = (Entry){session->head.catalog, session->head.catalog_bytes};
    for (size_t i = 0; i < count; ++i) {
        unsigned char record[RECORD] = {0};
        uint64_t index;
        bool found;
        Entry current, updated;
        int64_t stored;
        memcpy(record, keys[i], strlen(keys[i]));
        encode64(record + KEY, documents[i].ref);
        encode64(record + KEY + 8, documents[i].bytes);
        encode64(record + KEY + 16, (uint64_t)revisions[i] + 1);
        if (!find(&session->tree, *catalog, keys[i], &index, &found, &current, &stored) ||
            !rebuild(&session->tree, *catalog, index * RECORD, found ? RECORD : 0, (PureSpan){record, RECORD}, &updated))
            return false;
        olds[i] = found ? current : (Entry){0};
        *catalog = updated;
    }
    return write_root(&session->tree, writer, catalog, false);
}

/* Where a ring's commit goes, its link at *link_at and its bytes after: at
 * the front while the free bytes there hold it with the next front's 40
 * left free; written again (need: twice what it wrote there) at the lowest
 * free run outside the front's 40 that holds need, else past the end, the
 * file grown by zeros in this commit's sync. */
static bool ring_place(Session *session, Writer *writer, uint64_t need, uint64_t *link_at) {
    Ring *ring = session->ring;
    uint64_t front = session->head.end;
    Extent part = {front, free_end(ring, front)}; /* (the front's 40 bytes are free) */
    if (need && !free_lowest(ring, need, front, front + LINK, &part)) {
        uint64_t size = ring->size, grown = (size + need) / GROW * GROW + GROW;
        zero_fill(session->fd, size, grown);
        if (!reach(ring, grown) || !free_add(ring, size, grown)) return false;
        ring->size = grown;
        if (!free_lowest(ring, need, front, front + LINK, &part)) return false;
    }
    /* elsewhere, within its part: never over the front's 40 bytes, where its marker goes */
    *link_at = part.start;
    writer->at = part.start + LINK;
    writer->limit = part.end > part.start + 2 * LINK ? part.end - LINK : part.start + LINK;
    return true;
}

/* After a ring's commit is durable: its link, bytes and marker are in use,
 * its nodes live, the nodes it replaced dead (each written key's root before
 * and the catalog's, down to what the new trees keep), deferred while a pin
 * may read them; pins let go are swept, deaths they deferred go; segments
 * that came free are back. False without memory (read the ring again). */
static bool settle_ring(Holding *holding, const Head *before, uint64_t link_at, bool marked, size_t count,
                        const char *const *keys, const Entry *olds, bool checkpoint) {
    Session *session = &holding->session;
    Ring *ring = session->ring;
    const Head *published = &session->head;
    bool ok = free_take(ring, link_at, published->end) && (!marked || free_take(ring, before->end, before->end + LINK));
    count_written(ring, link_at, published->end, published->seq);
    if (marked) count_written(ring, before->end, before->end + LINK, published->seq);
    for (size_t i = 0; i < ring->wrote_count; ++i) count_live(ring, ring->wrote[i].start, ring->wrote[i].end, true);
    if (ring->kept_count) qsort(ring->kept, ring->kept_count, sizeof(*ring->kept), compare_offsets);
    pthread_mutex_lock(&holding->turns->lock);
    Mapping **link = &holding->pins, *swept = NULL;
    while (*link) /* pins let go */
        if (__atomic_load_n(&(*link)->refs, __ATOMIC_ACQUIRE)) link = &(*link)->next;
        else {
            Mapping *pin = *link;
            *link = pin->next;
            pin->next = swept;
            swept = pin;
        }
    for (size_t i = 0; i < ring->deferred_count;) {
        Deferred *d = &ring->deferred[i];
        if (pinned_in(holding->pins, d->key, d->born, d->died)) ++i;
        else {
            count_live(ring, d->start, d->end, false);
            ring->deferred[i] = ring->deferred[--ring->deferred_count];
        }
    }
    unsigned height;
    Entry catalog = {before->catalog, before->catalog_bytes};
    if (root_height(&session->tree, catalog, &height))
        count_dead(ring, holding->pins, &session->tree, catalog, height, "", published->seq);
    for (size_t i = 0; i < count; ++i)
        if (root_height(&session->tree, olds[i], &height))
            count_dead(ring, holding->pins, &session->tree, olds[i], height, keys[i], published->seq);
    pthread_mutex_unlock(&holding->turns->lock);
    while (swept) {
        Mapping *next = swept->next;
        release_mapping(swept->region);
        free(swept);
        swept = next;
    }
    ring->since = (checkpoint ? 0 : ring->since) + published->end - link_at + (marked ? LINK : 0);
    ring->wrote_count = ring->kept_count = 0;
    return ok && !ring->failed && give_back(ring, h_rec(session));
}

/* The documents' new nodes are already written at writer, and the catalog
 * after them: write the head as the record before them and sync once: every
 * document becomes visible at once, or none does. Readers of this process
 * see the head only once it is durable. The session then stands at the new
 * head; a compaction leaves it on the new file (compacted), and when it
 * cannot follow it is stale. A ring's record is at link_at, and a marker at
 * the front names it when that is elsewhere. */
#ifdef PURE_STORE_TEST
static unsigned long publications;
unsigned long pure_tree_test_publications(void) { return publications; }
#endif
static PureResult publish(Holding *holding, Writer *writer, size_t count, const char *const *keys, int64_t *revisions,
                          Entry catalog, const Entry *olds, uint64_t link_at, bool *compacted) {
#ifdef PURE_STORE_TEST
    ++publications;
#endif
    Session *session = &holding->session;
    Ring *ring = session->ring;
    *compacted = false;
    /* Unpublished bytes stay in the tail (a ring's in its free bytes); the next commit overwrites them. */
    if (fault(PURE_AFTER_HEADER) || fault(PURE_BEFORE_FILE_SYNC) || fault(PURE_BEFORE_RENAME)) return PURE_STORE_ERROR;
    uint64_t room = ring ? LINK : record_room();
    Head published = {session->head.seq + 1, writer->at, catalog.ref, catalog.bytes, session->head.live,
                      ring ? link_at + LINK : session->head.end + room, digest_end(&writer->digest), session->head.ring};
    unsigned char record[SLOT], slot[SLOT], marker[LINK];
    if (room == LINK) encode_link(record, &published);
    else encode_head(record, record_magic, &published);
    bool marked = ring && link_at != session->head.end;
    if (marked) encode_marker(marker, published.seq, link_at, record + 32);
    /* Every few commits the head before this commit (durable since its own
     * sync) goes into the older slot: one more range for this sync, and fewer
     * records for a reader to follow. Never this commit's own head: a crash in
     * this sync could keep the slot and lose the commit's bytes (a slot that
     * leads nowhere), or its record (the store would open there by the slot,
     * while the walk from the other slot stopped short of it). A ring's comes
     * after 256 KiB of commits too: what a start may follow stays bounded. */
    Head named = session->head;
#ifdef PURE_STORE_TEST
    if (earlier_slots) named = published;
#endif
    encode_head(slot, named.ring ? ring_magic : head_magic, &named);
    uint64_t newer = session->slots[0] > session->slots[1] ? session->slots[0] : session->slots[1];
    unsigned older = session->slots[0] > session->slots[1];
    bool checkpoint = named.seq - newer >= CHECKPOINT || (ring && ring->since >= CHECKPOINT_BYTES);
    if (!remember(&session->file, &published, PUBLISHING)) return PURE_ALLOCATION; /* nothing written */
    for (size_t i = 0; i < count; ++i) ++revisions[i]; /* Past this point the head may already be visible. */
    bool durable = write_at(session->fd, record, (size_t)room, published.start - room) &&
                   (!marked || write_at(session->fd, marker, LINK, session->head.end)) &&
                   (!checkpoint || write_at(session->fd, slot, SLOT, older * BLOCK)) &&
                   !fault(PURE_BEFORE_DIR_SYNC) && sync_data(session->fd);
    remember(&session->file, &published, durable ? DURABLE : CHECKED);
    if (!durable) {
        holding->stale = true;
        return PURE_UNCERTAIN;
    }
    Head before = session->head;
    session->head = published;
    if (checkpoint) session->slots[older] = named.seq;
    if (ring && !settle_ring(holding, &before, link_at, marked, count, keys, olds, checkpoint)) holding->stale = true;
    clear_arena(&session->tree);
    if (!cover(session)) {
        holding->stale = true;
        return PURE_OK;
    }
    if (ring) return PURE_OK; /* in place: no compaction, no tail of zeros */
    if (published.end - DATA > 4 * published.live + COMPACT_SLACK && !fstat(session->fd, &session->file)) {
        /* The name decides, not compact's result: its rename may be in place
         * though the directory's sync after it failed. */
        bool synced = compact(session->directory, session->tree.shared, (uint64_t)session->file.st_size, &published, false);
        struct stat named;
        if (!fstatat(session->directory, tree_name, &named, AT_SYMLINK_NOFOLLOW) &&
            (named.st_dev != session->file.st_dev || named.st_ino != session->file.st_ino)) {
            *compacted = true;
            if (reopen(session, synced) != PURE_OK) holding->stale = true;
            return PURE_OK;
        }
    }
    prepare_tail(session);
    return PURE_OK;
}

/* A commit of documents: each root's new nodes (in its tree; relocate: its
 * stored ones too), the catalog's after them, then published. A ring's
 * commit that the free bytes at its place do not hold is written again
 * where twice what it wrote fits (a write leaves the trees as they were). */
static PureResult commit_documents(Holding *holding, size_t count, const char *const *keys, Tree *const *trees,
                                   const Entry *roots, const bool *relocate, int64_t *revisions, Entry *written,
                                   bool *compacted) {
    Session *session = &holding->session;
    Ring *ring = session->ring;
    Writer *writer = writer_of(holding);
    Entry *olds = malloc((count ? count : 1) * sizeof(*olds));
    if (!writer || !olds) {
        free(olds);
        return PURE_ALLOCATION;
    }
    uint64_t link_at = session->head.end, need = 0;
    Entry catalog;
    PureResult result = PURE_OK;
    for (;;) {
        if (ring) {
            ring->wrote_count = ring->kept_count = 0;
            ring->failed = false;
            writer->ring = ring;
            if (!ring_place(session, writer, need, &link_at)) {
                result = PURE_STORE_ERROR;
                break;
            }
        }
        bool ok = true;
        for (size_t i = 0; ok && i < count; ++i) {
            written[i] = roots[i];
            ok = write_root(trees[i], writer, &written[i], relocate[i]);
        }
        ok = ok && write_catalog(session, writer, count, keys, written, revisions, &catalog, olds) && flush(writer);
        if (ok && !(ring && ring->failed)) break;
        if (!ring || !writer->over || ring->failed) {
            result = ring && ring->failed ? PURE_ALLOCATION : PURE_STORE_ERROR;
            break;
        }
        uint64_t wrote = writer->at - (link_at + LINK);
        need = 2 * (wrote > BLOCK ? wrote : BLOCK) + 2 * LINK;
        writer->used = 0;
        writer->failed = writer->over = false;
        writer->digest = (Digest){0};
    }
    if (result == PURE_OK) result = publish(holding, writer, count, keys, revisions, catalog, olds, link_at, compacted);
    free(olds);
    return result;
}
static PureResult mutate(const char *path, const char *key, int64_t expected,
                         PureSpan body, bool splice, size_t offset, size_t remove,
                         size_t max_size, int64_t *revision) {
    *revision = 0;
    if (!valid_key(key) || (body.size && !body.data)) return PURE_STORE_ERROR;
    if (body.size > PURE_OBJECT_LIMIT || max_size > PURE_OBJECT_LIMIT || expected < 0) return PURE_LIMIT;
    int directory = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    Turns *turns = directory >= 0 ? turns_of(directory) : NULL;
    if (directory >= 0) close(directory);
    if (!turns) return PURE_STORE_ERROR;
    pthread_mutex_lock(&turns->writer);
    PureResult result = hold(turns, path);
    if (result != PURE_OK) {
        pthread_mutex_unlock(&turns->writer);
        return result;
    }
    Holding *holding = turns->holding;
    Session *session = &holding->session;
    Entry document, next;
    uint64_t index;
    bool found, compacted;
    result = PURE_STORE_ERROR;
    if (!find(&session->tree, (Entry){session->head.catalog, session->head.catalog_bytes}, key,
              &index, &found, &document, revision)) goto finish;
    if (*revision != expected) { result = PURE_CONFLICT; goto finish; }
    if (*revision == INT64_MAX) { result = PURE_LIMIT; goto finish; }
    uint64_t old_size = document.bytes;
    if (splice && (offset > old_size || remove > old_size - offset ||
                   old_size - remove > max_size ||
                   body.size > max_size - (old_size - remove))) { result = PURE_LIMIT; goto finish; }
    if (!rebuild(&session->tree, splice ? document : (Entry){0}, splice ? offset : 0, splice ? remove : 0, body, &next))
        goto finish;
    Tree *trees[1] = {&session->tree};
    bool relocate[1] = {false};
    Entry written;
    result = commit_documents(holding, 1, &key, trees, &next, relocate, revision, &written, &compacted);
    if (result == PURE_OK && !holding->stale) show_newest(turns, holding);
    else if (result == PURE_UNCERTAIN) settle_uncertain(turns, holding);
finish:
    let_go(turns);
    pthread_mutex_unlock(&turns->writer);
    return result;
}

/* Keys come from the catalog in ascending byte order. Runtime values are not
 * documents and are left out. */
PureResult pure_store_keys(const char *path, bool (*visit)(void *, const char *key),
                           void *context) {
    int directory = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (directory < 0) return PURE_STORE_ERROR;
    int fd = openat(directory, tree_name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    close(directory);
    if (fd < 0) return PURE_STORE_ERROR;
    Tree tree = {0};
    Head head;
    struct stat file;
    Turns *owned;
    PureResult result = own_ring(path, fd, &owned);
    if (result != PURE_OK) {
        close(fd);
        return result;
    }
    result = PURE_STORE_ERROR;
    if (read_head(fd, &file, false, &head, NULL) && map_tree(fd, &file, &head, &tree)) {
        Entry catalog = {head.catalog, head.catalog_bytes}, document;
        unsigned char record[RECORD];
        char key[KEY + 1] = {0};
        int64_t revision;
        result = PURE_OK;
        for (uint64_t i = 0; i < catalog.bytes / RECORD; ++i) {
            if (!read_record(&tree, catalog, i, record, &document, &revision)) { result = PURE_STORE_ERROR; break; }
            memcpy(key, record, KEY);
            if (runtime_key(key)) continue;
            if (!valid_key(key)) { result = PURE_STORE_ERROR; break; }
            if (!visit(context, key)) break;
        }
    }
    release_tree(&tree);
    close(fd);
    disown(owned);
    return result;
}

/* A store kept open by one owner thread: a holder of the process's writer on
 * it, and the version its drafts open at (the newest when it last looked),
 * whose mapping they share. */
struct PureStore {
    char *path;
    Turns *turns;
    Head head;
    Mapping *mapping;
    dev_t device; /* the version's file */
    ino_t inode;
    bool pinned; /* drafts open at the version of the last check */
};

struct PureDraft {
    PureStore *store; /* the handle it was opened through, or NULL */
    char *path;       /* owned unless opened through a store handle */
    char key[KEY + 1];
    Tree tree;        /* the base file's mapping plus this draft's node arena */
    dev_t device;     /* identity of the base file; compaction replaces it */
    ino_t inode;
    Entry root;
    int64_t revision;
    size_t live;      /* arena bytes reachable at the last collection */
    bool held;        /* a transaction still reads earlier versions */
    Mapping **earlier; /* a ring's: the pins of versions it read while held (those versions may still be read) */
    size_t earlier_count;
};

/* Rebase the draft on the committed version of its key. */
static PureResult load_draft(PureDraft *draft) {
    int directory = open(draft->path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (directory < 0) return PURE_STORE_ERROR;
    int fd = openat(directory, tree_name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    close(directory);
    if (fd < 0) return PURE_STORE_ERROR;
    Tree tree = {0};
    Head head;
    struct stat info;
    Entry document;
    uint64_t index;
    int64_t revision;
    bool found;
    bool ok = read_head(fd, &info, false, &head, NULL) && !head.ring /* (no draft on a ring yet: pins) */ &&
              map_tree(fd, &info, &head, &tree) &&
              find(&tree, (Entry){head.catalog, head.catalog_bytes}, draft->key, &index, &found,
                   &document, &revision);
    close(fd);
    if (!ok) { release_tree(&tree); return PURE_STORE_ERROR; }
    release_tree(&draft->tree);
    draft->tree = tree;
    draft->device = info.st_dev;
    draft->inode = info.st_ino;
    draft->root = document;
    draft->revision = revision;
    draft->live = 0;
    return PURE_OK;
}

PureResult pure_draft_open(const char *path, const char *key, PureDraft **out) {
    *out = NULL;
    if (!path || !(valid_key(key) || runtime_key(key))) return PURE_INVALID;
    PureDraft *draft = calloc(1, sizeof(*draft));
    if (!draft || !(draft->path = strdup(path))) { free(draft); return PURE_ALLOCATION; }
    memcpy(draft->key, key, strlen(key) + 1);
    PureResult result = load_draft(draft);
    if (result != PURE_OK) { pure_draft_close(draft); return result; }
    *out = draft;
    return PURE_OK;
}

PureResult pure_state_open(const char *path, const char *name, PureDraft **out) {
    char key[KEY + 2];
    *out = NULL;
    if (!valid_key(name) || strlen(name) >= KEY) return PURE_INVALID;
    snprintf(key, sizeof(key), ".%s", name);
    return pure_draft_open(path, key, out);
}

PureDraft *pure_draft_new(void) {
    return calloc(1, sizeof(PureDraft)); /* no path: nothing mapped, nothing to commit */
}

PureDraft *pure_draft_freeze(const PureDraft *draft) {
    if (!draft->tree.shared || (draft->root.ref & MEMORY_BIT)) return NULL;
    PureDraft *frozen = calloc(1, sizeof(*frozen));
    if (!frozen) return NULL;
    Mapping *mapping = draft->tree.shared;
    __atomic_add_fetch(&mapping->refs, 1, __ATOMIC_RELAXED);
    *frozen = (PureDraft){.tree = {.map = mapping->map, .end = draft->tree.end, .shared = mapping},
                          .device = draft->device, .inode = draft->inode, .root = draft->root,
                          .revision = draft->revision};
    memcpy(frozen->key, draft->key, sizeof(frozen->key));
    return frozen;
}

static void release_earlier(PureDraft *draft);
void pure_draft_close(PureDraft *draft) {
    if (!draft) return;
    release_earlier(draft);
    release_tree(&draft->tree);
    if (!draft->store) free(draft->path);
    free(draft);
}

/* Bring the handle to the newest version: the writer it holds made or read
 * every one, so this looks at no file. */
static PureResult refresh(PureStore *store) {
    Turns *turns = store->turns;
    Holding *holding = turns->holding;
    Mapping *old = NULL;
    pthread_mutex_lock(&turns->lock);
    if (holding->newest.ring) { /* an unpinned handle holds no version of a ring (it pins one as it opens a draft) */
        Mapping *pin = store->pinned ? pin_at(holding, holding->mapping, holding->newest.seq, "") : NULL;
        old = store->mapping;
        store->head = holding->newest;
        store->mapping = pin;
        pthread_mutex_unlock(&turns->lock);
        release_mapping(old);
        return !store->pinned || pin ? PURE_OK : PURE_ALLOCATION;
    }
    if (holding->mapping && (holding->mapping != store->mapping || holding->newest.seq != store->head.seq ||
                             holding->newest.check != store->head.check)) {
        __atomic_add_fetch(&holding->mapping->refs, 1, __ATOMIC_RELAXED);
        old = store->mapping;
        store->mapping = holding->mapping;
        store->head = holding->newest;
        store->device = holding->device;
        store->inode = holding->inode;
    }
    pthread_mutex_unlock(&turns->lock);
    release_mapping(old);
    return store->mapping ? PURE_OK : PURE_STORE_ERROR;
}

/* Base a draft on the handle's newest version of its key, sharing the mapping.
 * On failure the draft is unchanged. */
static PureResult rebase(PureDraft *draft) {
    PureStore *store = draft->store;
    Mapping *mapping = store->mapping;
    Tree tree = {.map = mapping->map, .end = store->head.end, .shared = mapping};
    Entry document;
    uint64_t index;
    int64_t revision;
    bool found;
    if (!find(&tree, (Entry){store->head.catalog, store->head.catalog_bytes}, draft->key, &index, &found, &document,
              &revision)) return PURE_STORE_ERROR;
    __atomic_add_fetch(&mapping->refs, 1, __ATOMIC_RELAXED);
    release_tree(&draft->tree);
    draft->tree = tree;
    draft->device = store->device;
    draft->inode = store->inode;
    draft->root = document;
    draft->revision = revision;
    draft->live = 0;
    return PURE_OK;
}

/* A handle holds the process's writer on the store while it is open: busy
 * when another process writes it. */
PureResult pure_store_open(const char *path, PureStore **out) {
    *out = NULL;
    PureStore *store = calloc(1, sizeof(*store));
    if (!store || !path || !(store->path = strdup(path))) { free(store); return PURE_ALLOCATION; }
    int directory = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    Turns *turns = directory >= 0 ? turns_of(directory) : NULL;
    if (directory >= 0) close(directory);
    PureResult result = turns ? PURE_OK : PURE_STORE_ERROR;
    if (turns && !join(turns)) {
        pthread_mutex_lock(&turns->writer);
        result = hold(turns, path);
        pthread_mutex_unlock(&turns->writer);
    }
    if (result != PURE_OK) {
        free(store->path);
        free(store);
        return result;
    }
    store->turns = turns;
    result = refresh(store);
    if (result != PURE_OK) { pure_store_close(store); return result; }
    *out = store;
    return PURE_OK;
}

const char *pure_store_path(const PureStore *store) { return store->path; }

void pure_store_close(PureStore *store) {
    if (!store) return;
    release_mapping(store->mapping);
    pthread_mutex_lock(&store->turns->writer);
    let_go(store->turns);
    pthread_mutex_unlock(&store->turns->writer);
    free(store->path);
    free(store);
}

PureMapping *pure_draft_mapping(const PureDraft *draft) {
    Mapping *mapping = draft->tree.shared;
    if (mapping) __atomic_add_fetch(&mapping->refs, 1, __ATOMIC_RELAXED);
    return mapping;
}

bool pure_mapping_holds(const PureMapping *mapping, PureSpan span) {
    uintptr_t base = (uintptr_t)mapping->map, at = (uintptr_t)span.data;
    return !span.size || (span.data && at >= base && at - base <= mapping->length && span.size <= mapping->length - (at - base));
}

void pure_mapping_release(PureMapping *mapping) { release_mapping(mapping); }

PureResult pure_store_pin(PureStore *store, bool pinned) {
    store->pinned = pinned; /* (a ring's handle holds a pin only while pinned) */
    PureResult result = pinned || store->head.ring ? refresh(store) : PURE_OK;
    if (result != PURE_OK) store->pinned = false;
    return result;
}

/* A ring's draft opens on the pinned handle's version, or the newest, its
 * key pinned there; the catalog is read under a pin of every key. */
static PureResult ring_draft(PureStore *store, PureDraft *draft) {
    Turns *turns = store->turns;
    Holding *holding = turns->holding;
    pthread_mutex_lock(&turns->lock);
    if (!store->pinned) store->head = holding->newest;
    Mapping *all = store->pinned ? store->mapping : pin_at(holding, holding->mapping, holding->newest.seq, "");
    Mapping *mine = all ? pin_at(holding, all->region, store->head.seq, draft->key) : NULL;
    pthread_mutex_unlock(&turns->lock);
    Tree tree = {.map = all ? all->map : NULL, .end = all ? all->length : 0};
    Entry document;
    uint64_t index;
    int64_t revision;
    bool found;
    bool ok = mine && find(&tree, (Entry){store->head.catalog, store->head.catalog_bytes}, draft->key, &index, &found,
                           &document, &revision);
    if (!store->pinned) release_mapping(all);
    if (!ok) {
        release_mapping(mine);
        return mine ? PURE_STORE_ERROR : PURE_ALLOCATION;
    }
    draft->tree = (Tree){.map = mine->map, .end = mine->length, .shared = mine};
    draft->device = holding->device;
    draft->inode = holding->inode;
    draft->root = document;
    draft->revision = revision;
    draft->live = 0;
    return PURE_OK;
}

static PureResult open_through(PureStore *store, const char *key, PureDraft **out) {
    *out = NULL;
    if (store->head.ring) {
        PureDraft *draft = calloc(1, sizeof(*draft));
        if (!draft) return PURE_ALLOCATION;
        *draft = (PureDraft){.store = store, .path = store->path};
        memcpy(draft->key, key, strlen(key) + 1);
        PureResult result = ring_draft(store, draft);
        if (result != PURE_OK) free(draft);
        else *out = draft;
        return result;
    }
    PureResult result = store->pinned ? PURE_OK : refresh(store);
    if (result != PURE_OK) return result;
    PureDraft *draft = calloc(1, sizeof(*draft));
    if (!draft) return PURE_ALLOCATION;
    *draft = (PureDraft){.store = store, .path = store->path};
    memcpy(draft->key, key, strlen(key) + 1);
    result = rebase(draft);
    if (result != PURE_OK) { free(draft); return result; }
    *out = draft;
    return PURE_OK;
}

PureResult pure_store_draft(PureStore *store, const char *key, PureDraft **out) {
    *out = NULL;
    return store && valid_key(key) ? open_through(store, key, out) : PURE_INVALID;
}

PureResult pure_store_state(PureStore *store, const char *name, PureDraft **out) {
    char key[KEY + 2];
    *out = NULL;
    if (!store || !valid_key(name) || strlen(name) >= KEY) return PURE_INVALID;
    snprintf(key, sizeof(key), ".%s", name);
    return open_through(store, key, out);
}

size_t pure_draft_size(const PureDraft *draft) { return (size_t)draft->root.bytes; }

int64_t pure_draft_revision(const PureDraft *draft) { return draft->revision; }

static size_t mark(const Tree *tree, Entry entry, unsigned char *marks) {
    if (!(entry.ref & MEMORY_BIT)) return 0;
    size_t at = (size_t)(entry.ref & LOCATION);
    if (marks[at]) return 0;
    marks[at] = 1;
    const Node *node = &tree->nodes[at];
    size_t bytes = node_bytes(node);
    if (!node->bytes)
        for (size_t i = 0; i < node->size; ++i) bytes += mark(tree, node->children[i], marks);
    return bytes;
}

/* Every edit copies a leaf and path, so reclaim arena nodes the root no
 * longer reaches once they outweigh the reachable ones (and 32 KiB). */
static void collect(PureDraft *draft) {
    Tree *tree = &draft->tree;
    if (draft->held) return;
    size_t allowance = draft->live > 32768 ? draft->live : 32768;
    if (tree->bytes <= draft->live || tree->bytes - draft->live <= allowance) return;
    unsigned char *marks = calloc(tree->count, 1);
    if (!marks) return;
    draft->live = mark(tree, draft->root, marks);
    for (size_t i = 0; i < tree->count; ++i) {
        Node *node = &tree->nodes[i];
        if (marks[i] || node->height == FREE) continue;
        tree->bytes -= node_bytes(node);
        free(node->bytes);
        free(node->children);
        *node = (Node){.size = tree->free_head, .height = FREE};
        tree->free_head = i + 1;
        ++tree->free_count;
    }
    free(marks);
}

PureResult pure_draft_splice(PureDraft *draft, size_t offset, size_t remove, PureSpan insert) {
    uint64_t size = draft->root.bytes;
    if (insert.size && !insert.data) return PURE_INVALID;
    if (offset > size || remove > size - offset || insert.size > limit_of(draft->key) - (size - remove))
        return PURE_LIMIT;
    if (!remove && !insert.size) return PURE_OK;
    Entry next;
    bool ok = rebuild(&draft->tree, draft->root, offset, remove, insert, &next);
    if (ok) draft->root = next;
    collect(draft);
    return ok ? PURE_OK : PURE_STORE_ERROR;
}

PureVersion pure_draft_version(const PureDraft *draft) {
    return (PureVersion){draft->root.ref, draft->root.bytes};
}

PureResult pure_draft_read_version(const PureDraft *draft, PureVersion version, size_t offset,
                                   size_t length, bool (*visit)(void *, PureSpan), void *context) {
    Entry root = {version.ref, version.bytes};
    unsigned height;
    if (offset > root.bytes || length > root.bytes - offset) return PURE_LIMIT;
    if (!length) return PURE_OK;
    if (!root_height(&draft->tree, root, &height)) return PURE_STORE_ERROR;
    return visit_range(&draft->tree, root, height, offset, length, visit, context) ?
           PURE_OK : PURE_STORE_ERROR;
}

PureResult pure_draft_read(const PureDraft *draft, size_t offset, size_t length,
                           bool (*visit)(void *, PureSpan), void *context) {
    return pure_draft_read_version(draft, pure_draft_version(draft), offset, length, visit, context);
}

void pure_draft_restore(PureDraft *draft, PureVersion version) {
    draft->root = (Entry){version.ref, version.bytes};
}

static void release_earlier(PureDraft *draft) {
    for (size_t i = 0; i < draft->earlier_count; ++i) release_mapping(draft->earlier[i]);
    free(draft->earlier);
    draft->earlier = NULL;
    draft->earlier_count = 0;
}

void pure_draft_hold(PureDraft *draft, bool held) {
    draft->held = held;
    if (!held) {
        release_earlier(draft);
        collect(draft);
    }
}

/* ---- Group commits ------------------------------------------------------
 * Threads that commit at the same time share one commit: they queue, the
 * first becomes the leader and publishes every queued request of the same
 * store with one head (one pair of syncs); the others wait for its result.
 * Each request stays all or nothing on its own: a request with a stale draft
 * is left out, and so is one whose key an earlier request of the same group
 * already changes. The leader touches the waiters' drafts and handles only
 * while they wait. */
typedef struct Waiter {
    PureDraft *const *drafts;
    size_t count;
    int64_t *revisions;
    PureResult result;
    bool accepted, done;
    struct Waiter *next;
} Waiter;

/* Every store's groups so far, and the requests they held. */
void pure_store_group_counts(unsigned long *groups, unsigned long *requests) {
    *groups = *requests = 0;
    pthread_mutex_lock(&stores.lock);
    for (Turns *t = stores.first; t; t = t->next) {
        pthread_mutex_lock(&t->lock);
        *groups += t->groups;
        *requests += t->requests;
        pthread_mutex_unlock(&t->lock);
    }
    pthread_mutex_unlock(&stores.lock);
}

enum { GATHER_NS = 2000000 };

/* A committed draft continues from what its commit published: the root it
 * wrote and the revision it took, on the writer's mapping. */
static void settle_draft(PureDraft *draft, const Session *session, Entry root, int64_t revision) {
    Mapping *mapping = session->tree.shared;
    if (session->head.ring) { /* its key pinned at the version its commit made (the newest) */
        Holding *holding = draft->store->turns->holding;
        pthread_mutex_lock(&holding->turns->lock);
        Mapping *pin = pin_at(holding, mapping, session->head.seq, draft->key);
        pthread_mutex_unlock(&holding->turns->lock);
        Mapping **earlier = draft->held && draft->tree.shared ?
                            realloc(draft->earlier, (draft->earlier_count + 1) * sizeof(*earlier)) : NULL;
        if (earlier) { /* held: what it read may still be read */
            draft->earlier = earlier;
            earlier[draft->earlier_count++] = draft->tree.shared;
            draft->tree.shared = NULL;
        }
        if (!pin) { /* no memory: it keeps the version it had (stale: its next commit conflicts) */
            draft->revision = revision - 1;
            return;
        }
        release_tree(&draft->tree);
        draft->tree = (Tree){.map = pin->map, .end = pin->length, .shared = pin};
        draft->device = session->file.st_dev;
        draft->inode = session->file.st_ino;
        draft->root = root;
        draft->revision = revision;
        draft->live = 0;
        return;
    }
    __atomic_add_fetch(&mapping->refs, 1, __ATOMIC_RELAXED);
    release_tree(&draft->tree);
    draft->tree = (Tree){.map = mapping->map, .end = session->head.end, .shared = mapping};
    draft->device = session->file.st_dev;
    draft->inode = session->file.st_ino;
    draft->root = root;
    draft->revision = revision;
    draft->live = 0;
}

/* Publish the queued requests of a store in one commit of the process's
 * writer on it (whatever the spelling of each one's path: they are all this
 * store's), opened by the first one's path when nothing holds it. */
static void commit_group(Turns *turns, Waiter *first) {
    const char *path = first->drafts[0]->path;
    size_t total = 0;
    for (Waiter *w = first; w; w = w->next) { total += w->count; w->accepted = false; }
    Entry *roots = malloc(total * sizeof(*roots));
    const char **keys = malloc(total * sizeof(*keys));
    int64_t *revisions = malloc(total * sizeof(*revisions));
    PureDraft **drafts = malloc(total * sizeof(*drafts));
    bool allocated = roots && keys && revisions && drafts, checked = false;
    pthread_mutex_lock(&turns->writer);
    PureResult result = allocated ? hold(turns, path) : PURE_ALLOCATION;
    bool held = result == PURE_OK;
    Holding *holding = turns->holding;
    size_t count = 0;
    if (held) {
        Session *session = &holding->session;
        /* A request holds when each of its keys is at its draft's base
         * revision and no earlier request of this group changes it. */
        for (Waiter *w = first; w; w = w->next) {
            w->result = PURE_OK;
            for (size_t i = 0; i < w->count && w->result == PURE_OK; ++i) {
                PureDraft *d = w->drafts[i];
                Entry current;
                uint64_t index;
                int64_t stored;
                bool found;
                if (!find(&session->tree, (Entry){session->head.catalog, session->head.catalog_bytes}, d->key, &index,
                          &found, &current, &stored)) { w->result = PURE_STORE_ERROR; break; }
                for (size_t k = 0; k < count; ++k)
                    if (!strcmp(keys[k], d->key)) { ++stored; break; } /* it moves with the earlier request */
                if (stored != d->revision) { w->revisions[i] = stored; w->result = PURE_CONFLICT; }
                else if (stored == INT64_MAX) w->result = PURE_LIMIT;
            }
            if (w->result != PURE_OK) continue;
            w->accepted = true;
            for (size_t i = 0; i < w->count; ++i) {
                drafts[count] = w->drafts[i];
                roots[count] = w->drafts[i]->root;
                keys[count] = w->drafts[i]->key;
                revisions[count++] = w->drafts[i]->revision;
            }
        }
        checked = true;
        if (count) {
            bool compacted = false;
            Tree **trees = malloc(count * sizeof(*trees));
            bool *relocate = malloc(count * sizeof(*relocate));
            Entry *written = malloc(count * sizeof(*written));
            if (!trees || !relocate || !written) result = PURE_ALLOCATION;
            else {
                for (size_t i = 0; i < count; ++i) {
                    trees[i] = &drafts[i]->tree;
                    /* After compaction a draft's stored references name the old file. */
                    relocate[i] = session->file.st_dev != drafts[i]->device || session->file.st_ino != drafts[i]->inode;
                }
                result = commit_documents(holding, count, keys, trees, roots, relocate, revisions, written, &compacted);
                memcpy(roots, written, count * sizeof(*roots)); /* each as stored */
            }
            free(trees);
            free(relocate);
            free(written);
            /* The drafts go on from the published version while the writer is
             * still held; their arenas are no longer needed. A compaction's
             * new file has each root elsewhere: found there. A writer that
             * could not follow its commit leaves each draft with equal content
             * at its new revision. */
            if (result == PURE_OK && !holding->stale) show_newest(turns, holding);
            else if (result == PURE_UNCERTAIN) settle_uncertain(turns, holding);
            PureStore *refreshed[8] = {NULL};
            size_t fresh = 0;
            for (size_t i = 0; result == PURE_OK && i < count; ++i) {
                PureDraft *d = drafts[i];
                Entry document = roots[i];
                uint64_t index;
                int64_t revision = revisions[i];
                bool found;
                if (holding->stale || (compacted && !find(&session->tree, (Entry){session->head.catalog, session->head.catalog_bytes},
                                                         d->key, &index, &found, &document, &revision))) {
                    d->revision = revisions[i];
                    continue;
                }
                settle_draft(d, session, document, revision);
                bool seen = !d->store;
                for (size_t k = 0; k < fresh && !seen; ++k) seen = refreshed[k] == d->store;
                if (!seen && refresh(d->store) == PURE_OK && fresh < 8) refreshed[fresh++] = d->store;
            }
        }
        let_go(turns);
    }
    pthread_mutex_unlock(&turns->writer);
    /* Before the checks every request shares the failure; after them an
     * accepted request takes the publication's result and new revisions, and
     * a refused one keeps its own result. */
    size_t at = 0;
    for (Waiter *w = first; w; w = w->next) {
        if (!checked) { w->result = result; continue; }
        if (!w->accepted) continue;
        for (size_t i = 0; i < w->count; ++i) w->revisions[i] = revisions[at++];
        w->result = result;
    }
    free(roots);
    free((void *)keys);
    free(revisions);
    free(drafts);
}

PureResult pure_draft_commit_all(PureDraft *const *drafts, size_t count, int64_t *revisions) {
    for (size_t i = 0; i < count; ++i) {
        revisions[i] = drafts[i]->revision;
        if (!drafts[i]->path || strcmp(drafts[i]->path, drafts[0]->path)) return PURE_INVALID;
        for (size_t k = 0; k < i; ++k) if (!strcmp(drafts[k]->key, drafts[i]->key)) return PURE_INVALID;
    }
    if (!count) return PURE_OK;
    struct stat info;
    Turns *group = drafts[0]->store ? drafts[0]->store->turns : stat(drafts[0]->path, &info) ? NULL : turns_for(&info);
    if (!group) return PURE_STORE_ERROR;
    Waiter me = {.drafts = drafts, .count = count, .revisions = revisions};
    pthread_mutex_lock(&group->lock);
    if (group->tail) group->tail->next = &me; else group->head = &me;
    group->tail = &me;
    ++group->queued;
    pthread_cond_signal(&group->joined);
    while (!me.done) {
        if (group->leading) { pthread_cond_wait(&group->finished, &group->lock); continue; }
        group->leading = true;
        /* When several threads have been committing together, give the others
         * a moment (at most 2 ms) to join, or they would miss this group and
         * wait for the next one: a sync costs far more. One thread never waits. */
        if (group->expected > 1 && group->queued < group->expected) {
            struct timespec deadline = pure_wait_until(GATHER_NS);
            while (group->queued < group->expected &&
                   pthread_cond_timedwait(&group->joined, &group->lock, &deadline) == 0) {}
        }
        Waiter *batch = group->head;
        size_t size = group->queued;
        group->head = group->tail = NULL;
        group->queued = 0;
        ++group->groups;
        group->requests += size;
        /* Remember the largest recent group, forgetting one member per group. */
        group->expected = size > group->expected - 1 ? size : group->expected - 1;
        if (!group->expected) group->expected = 1;
        pthread_mutex_unlock(&group->lock);
        commit_group(group, batch); /* one for every queued request, whatever path names the store */
        pthread_mutex_lock(&group->lock);
        for (Waiter *w = batch, *next; w; w = next) { next = w->next; w->done = true; }
        group->leading = false;
        pthread_cond_broadcast(&group->finished);
    }
    pthread_mutex_unlock(&group->lock);
    return me.result;
}

PureResult pure_draft_commit(PureDraft *draft, int64_t *revision) {
    return pure_draft_commit_all(&draft, 1, revision);
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

#ifdef PURE_STORE_TEST
static bool shape(const Tree *tree, Entry entry, unsigned height, bool root,
                  uint64_t *leaves, uint64_t *small) {
    if (!height) {
        ++*leaves;
        if (!root && entry.bytes < LEAF_MIN) ++*small;
        return leaf_bytes(tree, entry) != NULL;
    }
    Entry kids[FAN_MAX];
    size_t count = children(tree, entry, height, kids);
    if (!count || (root && count < 2)) return false;
    if (!root && count < FAN_MIN) ++*small;
    for (size_t i = 0; i < count; ++i)
        if (!shape(tree, kids[i], height - 1, false, leaves, small)) return false;
    return true;
}

/* Test-only: live arena nodes and their payload bytes. */
void pure_draft_test_arena(const PureDraft *draft, size_t *nodes, size_t *bytes) {
    *nodes = *bytes = 0;
    for (size_t i = 0; i < draft->tree.count; ++i) {
        const Node *node = &draft->tree.nodes[i];
        if (node->height == FREE) continue;
        ++*nodes;
        *bytes += node->bytes ? node->size : node->size * sizeof(Entry);
    }
}

/* Test-only: the stored height, leaf count and number of underfull nodes. */
bool pure_tree_test_shape(const char *path, const char *key, unsigned *height,
                          uint64_t *leaves, uint64_t *small, uint64_t *file_end) {
    int directory = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    int fd = directory >= 0 ? openat(directory, tree_name, O_RDONLY | O_CLOEXEC) : -1;
    if (directory >= 0) close(directory);
    Tree tree = {0};
    Head head;
    struct stat file;
    Entry document;
    uint64_t index;
    int64_t revision;
    bool found = false;
    *leaves = *small = 0;
    Turns *owned = NULL;
    bool ok = fd >= 0 && own_ring(path, fd, &owned) == PURE_OK && read_head(fd, &file, false, &head, NULL) &&
              map_tree(fd, &file, &head, &tree) &&
              find(&tree, (Entry){head.catalog, head.catalog_bytes}, key, &index, &found,
                   &document, &revision) && found && root_height(&tree, document, height) &&
              (!document.bytes || shape(&tree, document, *height, true, leaves, small));
    if (ok) *file_end = head.end;
    release_tree(&tree);
    if (fd >= 0) close(fd);
    disown(owned);
    return ok;
}
#endif
