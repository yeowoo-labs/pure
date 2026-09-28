/* The app log (store/store-log.c) against a model of its entries: appends
 * of every size (segments of 16 KiB in this build, so entries cross them and
 * segments are used again), entries taken back, released, ended, torn on the
 * device, and the log opened again after each: what it reads is what the
 * model kept, and its files stay as many as the unreleased entries need.
 * Then list images: written whole, read back, refused when damaged. */
#define _POSIX_C_SOURCE 200809L
#include "log.h"
#include "store.h"
#include <dirent.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); abort(); } } while (0)

static int fail_stage = -1;
int pure_store_test_fault(PureStoreStage stage) { return (int)stage == fail_stage; }

static uint64_t next_random(uint64_t *state) {
    uint64_t x = *state;
    x ^= x << 13, x ^= x >> 7, x ^= x << 17;
    return *state = x;
}

typedef struct { uint64_t seq; bool tree; unsigned char *bytes; size_t size; } Kept;
typedef struct { Kept *entries; size_t count, capacity; } Model;

static void keep(Model *m, uint64_t seq, bool tree, const unsigned char *bytes, size_t size) {
    if (m->count == m->capacity) {
        m->capacity = m->capacity ? 2 * m->capacity : 64;
        CHECK((m->entries = realloc(m->entries, m->capacity * sizeof(*m->entries))));
    }
    Kept *k = &m->entries[m->count++];
    *k = (Kept){seq, tree, malloc(size ? size : 1), size};
    CHECK(k->bytes);
    memcpy(k->bytes, bytes, size);
}

static void drop_after(Model *m, uint64_t seq) {
    while (m->count && m->entries[m->count - 1].seq > seq) free(m->entries[--m->count].bytes);
}

typedef struct { const Model *model; size_t at; bool stop_after; uint64_t stop; } Reading;

static bool visit(void *context, uint64_t seq, bool tree, PureSpan payload) {
    Reading *r = context;
    while (r->at < r->model->count && r->model->entries[r->at].seq < seq) ++r->at;
    CHECK(r->at < r->model->count);
    const Kept *k = &r->model->entries[r->at++];
    CHECK(k->seq == seq && k->tree == tree && k->size == payload.size && !memcmp(k->bytes, payload.data, payload.size));
    return !(r->stop_after && seq >= r->stop);
}

static unsigned segment_files(const char *path) {
    DIR *dir = opendir(path);
    unsigned count = 0;
    for (struct dirent *e; dir && (e = readdir(dir));) count += !strncmp(e->d_name, "log-", 4);
    if (dir) closedir(dir);
    return count;
}

static char *new_directory(void) {
    char *path = strdup("/tmp/pure-log-XXXXXX");
    CHECK(path && mkdtemp(path));
    return path;
}

static void remove_directory(char *path) {
    DIR *dir = opendir(path);
    for (struct dirent *e; dir && (e = readdir(dir));) {
        if (e->d_name[0] == '.' && (!e->d_name[1] || (e->d_name[1] == '.' && !e->d_name[2]))) continue;
        char file[512];
        snprintf(file, sizeof(file), "%s/%s", path, e->d_name);
        unlink(file);
    }
    if (dir) closedir(dir);
    rmdir(path);
    free(path);
}

static void logs(void) {
    char *path = new_directory();
    uint64_t seed = 0x6c6f6773u, seq = 0, released = 0, retracted_seq = 0;
    Model model = {0};
    PureLog *log;
    CHECK(pure_log_open(path, &log) == PURE_OK && pure_log_last(log) == 0);
    unsigned char *payload = malloc(40000);
    CHECK(payload);
    unsigned reopened = 0, retracted = 0, ended = 0, most_files = 0;
    for (unsigned step = 0; step < 6000; ++step) {
        unsigned roll = (unsigned)(next_random(&seed) % 100);
        if (roll < 70) { /* an entry, the number sometimes skipping (a save that wrote none) */
            seq += 1 + (next_random(&seed) % 4 == 0 ? next_random(&seed) % 3 : 0);
            size_t size = next_random(&seed) % 10 == 0 ? next_random(&seed) % 40000 : next_random(&seed) % 900;
            for (size_t i = 0; i < size; ++i) payload[i] = (unsigned char)next_random(&seed);
            bool tree = next_random(&seed) % 2;
            CHECK(pure_log_append(log, seq, tree, (PureSpan){payload, size}) == PURE_OK);
            keep(&model, seq, tree, payload, size);
            retracted_seq = 0;
            /* Its save failed after it (the tree's commit): taken back, its
             * number used again. Until an entry takes its place it is still
             * on the device, and a start drops it (a tree entry past the
             * tree's last save). */
            if (tree && next_random(&seed) % 12 == 0) {
                pure_log_retract(log);
                drop_after(&model, seq - 1);
                retracted_seq = seq--;
                ++retracted;
            }
        } else if (roll < 80 && model.count) { /* older entries released */
            if (model.entries[model.count / 2].seq > released) released = model.entries[model.count / 2].seq;
            pure_log_release(log, released);
        } else if (roll < 97) { /* opened again: the kept entries, read from the oldest one not released */
            pure_log_close(log);
            CHECK(pure_log_open(path, &log) == PURE_OK);
            uint64_t kept_last = model.count ? model.entries[model.count - 1].seq : 0;
            if (retracted_seq) { /* found again, and dropped as a start would */
                CHECK(pure_log_last(log) == retracted_seq);
                pure_log_end(log, kept_last);
                retracted_seq = 0;
            }
            CHECK(pure_log_last(log) == kept_last);
            size_t first = 0;
            while (first < model.count && model.entries[first].seq <= released) ++first;
            if (first < model.count) {
                Reading reading = {&model, first, false, 0};
                CHECK(pure_log_read(log, model.entries[first].seq, visit, &reading) == PURE_OK);
                CHECK(reading.at == model.count);
            }
            if (next_random(&seed) % 4 == 0 && first < model.count) { /* entries after one are dropped */
                uint64_t end = model.entries[first + next_random(&seed) % (model.count - first)].seq;
                pure_log_end(log, end);
                drop_after(&model, end);
                seq = end;
                ++ended;
            }
            pure_log_release(log, released);
            ++reopened;
        }
        unsigned files = segment_files(path);
        most_files = files > most_files ? files : most_files;
    }
    CHECK(reopened > 500 && retracted > 100 && ended > 100 && most_files < 64);
    /* A write that fails before its sync is uncertain; an append numbered
     * no higher than the last is refused. */
    fail_stage = PURE_LOG_BEFORE_SYNC;
    CHECK(pure_log_append(log, seq + 1, false, (PureSpan){payload, 10}) == PURE_UNCERTAIN);
    fail_stage = -1;
    CHECK(pure_log_append(log, seq, false, (PureSpan){payload, 10}) == PURE_INVALID || !seq);
    pure_log_close(log);
    /* Bytes past the last entry, as an interrupted write leaves them, end the log there. */
    CHECK(pure_log_open(path, &log) == PURE_OK);
    uint64_t last = pure_log_last(log);
    CHECK(pure_log_append(log, last + 1, true, (PureSpan){payload, 3000}) == PURE_OK);
    pure_log_close(log);
    char file[512];
    unsigned damaged = 0;
    for (unsigned n = 0; n < 256; ++n) {
        snprintf(file, sizeof(file), "%s/log-%u", path, n);
        int fd = open(file, O_RDWR);
        if (fd < 0) break;
        /* The entry just written is the one of the newest segment ending in its 3000 bytes: damage its payload. */
        unsigned char probe[8];
        struct stat st;
        CHECK(!fstat(fd, &st));
        for (off_t at = 64; at + 32 + 3000 <= st.st_size; at += 8) {
            CHECK(pread(fd, probe, 8, at) == 8);
            uint64_t found = 0;
            for (unsigned i = 0; i < 8; ++i) found |= (uint64_t)probe[i] << (8 * i);
            if (found != last + 1) continue;
            unsigned char byte = 0x5a;
            CHECK(pwrite(fd, &byte, 1, at + 32 + 1500) == 1);
            ++damaged;
            break;
        }
        close(fd);
    }
    CHECK(damaged >= 1);
    CHECK(pure_log_open(path, &log) == PURE_OK && pure_log_last(log) == last);
    pure_log_close(log);
    for (size_t i = 0; i < model.count; ++i) free(model.entries[i].bytes);
    free(model.entries);
    free(payload);
    remove_directory(path);
    printf("{\"log_steps\":6000,\"reopened\":%u,\"retracted\":%u,\"ended\":%u,\"most_segment_files\":%u}\n", reopened,
           retracted, ended, most_files);
}

typedef struct { const PureSpan *parts; size_t count, at; } Parts;

static bool next_part(void *context, PureSpan *chunk) {
    Parts *p = context;
    *chunk = p->at < p->count ? p->parts[p->at++] : (PureSpan){0};
    if (chunk->data && !chunk->size) return next_part(context, chunk); /* an empty part is not the end */
    return true;
}

static PureResult write_parts(const char *path, const char *name, const PureSpan *parts, size_t count) {
    Parts p = {parts, count, 0};
    return pure_image_write(path, name, next_part, &p);
}

static bool refuse(void *context, PureSpan *chunk) {
    (void)context, (void)chunk;
    return false;
}

static void images(void) {
    char *path = new_directory();
    unsigned char *bytes;
    size_t size;
    CHECK(pure_image_read(path, "tasks", &bytes, &size) == PURE_NOT_FOUND);
    unsigned char *big = malloc(3 << 20);
    CHECK(big);
    for (size_t i = 0; i < (3u << 20); ++i) big[i] = (unsigned char)(i * 131 + (i >> 11));
    PureSpan parts[3] = {{(const unsigned char *)"head", 4}, {big, 3u << 20}, {(const unsigned char *)"", 0}};
    CHECK(write_parts(path, "tasks", parts, 3) == PURE_OK);
    CHECK(pure_image_read(path, "tasks", &bytes, &size) == PURE_OK && size == 4 + (3u << 20));
    CHECK(!memcmp(bytes, "head", 4) && !memcmp(bytes + 4, big, 3u << 20));
    free(bytes);
    /* A failure before the rename leaves the image as it was, and no temporary file. */
    fail_stage = PURE_IMAGE_BEFORE_RENAME;
    PureSpan other = {(const unsigned char *)"other", 5};
    CHECK(write_parts(path, "tasks", &other, 1) == PURE_STORE_ERROR);
    fail_stage = -1;
    CHECK(pure_image_read(path, "tasks", &bytes, &size) == PURE_OK && size == 4 + (3u << 20));
    free(bytes);
    CHECK(write_parts(path, "tasks", &other, 1) == PURE_OK);
    CHECK(pure_image_read(path, "tasks", &bytes, &size) == PURE_OK && size == 5 && !memcmp(bytes, "other", 5));
    free(bytes);
    /* Damaged: refused. */
    char file[512];
    snprintf(file, sizeof(file), "%s/image-tasks", path);
    int fd = open(file, O_RDWR);
    CHECK(fd >= 0 && pwrite(fd, "X", 1, 17) == 1);
    close(fd);
    CHECK(pure_image_read(path, "tasks", &bytes, &size) == PURE_STORE_ERROR);
    CHECK(write_parts(path, "bad/name", &other, 1) == PURE_INVALID);
    /* A producer that fails replaces nothing. */
    CHECK(pure_image_write(path, "tasks", refuse, NULL) == PURE_ALLOCATION);
    CHECK(pure_image_read(path, "tasks", &bytes, &size) == PURE_STORE_ERROR); /* still the damaged one */
    CHECK(pure_image_remove(path, "tasks") == PURE_OK && pure_image_read(path, "tasks", &bytes, &size) == PURE_NOT_FOUND);
    free(big);
    remove_directory(path);
    puts("{\"images\":\"written-read-kept-on-failure-refused-when-damaged\"}");
}

int main(void) {
    logs();
    images();
    return 0;
}
