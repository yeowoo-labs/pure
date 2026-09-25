/* Durable edit cost of one selected byte store, through its public API only.
 * Built once per store source; the store directory must be on the device
 * being measured (not tmpfs, where fsync is free). */
#define _POSIX_C_SOURCE 200809L
#include "store.h"
#include <dirent.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e3 + (double)t.tv_nsec / 1e6;
}

/* Linux accounting: wchar is bytes passed to write calls, write_bytes is
 * bytes this process caused to be sent to the block layer. */
static uint64_t io_field(const char *name) {
    FILE *file = fopen("/proc/self/io", "r");
    char line[128];
    uint64_t value = 0;
    size_t length = strlen(name);
    while (file && fgets(line, sizeof(line), file))
        if (!strncmp(line, name, length) && line[length] == ':') value = strtoull(line + length + 1, NULL, 10);
    if (file) fclose(file);
    return value;
}

static uint64_t store_bytes(const char *path) {
    DIR *directory = opendir(path);
    struct dirent *entry;
    uint64_t total = 0;
    char name[4096];
    while (directory && (entry = readdir(directory))) {
        struct stat info;
        snprintf(name, sizeof(name), "%s/%s", path, entry->d_name);
        if (!stat(name, &info) && S_ISREG(info.st_mode)) total += (uint64_t)info.st_size;
    }
    if (directory) closedir(directory);
    return total;
}

static int compare(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

int main(int argc, char **argv) {
    if (argc != 6) {
        fputs("bench STORE_NAME DIRECTORY SIZE insert-middle|delete-middle|replace-end|append-end COUNT\n", stderr);
        return 2;
    }
    const char *name = argv[1], *path = argv[2], *op = argv[4];
    size_t size = strtoull(argv[3], NULL, 10), count = strtoull(argv[5], NULL, 10);
    unsigned char *body = malloc(size + 1);
    double *samples = malloc(count * sizeof(*samples)), reads[16];
    if (!body || !samples || pure_store_init(path) != PURE_OK) return 1;
    for (size_t i = 0; i < size; ++i) body[i] = (unsigned char)('a' + i % 26);
    int64_t revision;
    if (pure_store_put(path, "doc", 0, (PureSpan){body, size}, &revision) != PURE_OK) return 1;
    size_t length = size;
    uint64_t wchar = io_field("wchar"), device = io_field("write_bytes");
    for (size_t i = 0; i < count; ++i) {
        size_t offset, remove, insert;
        if (!strcmp(op, "insert-middle")) { offset = length / 2; remove = 0; insert = 1; }
        else if (!strcmp(op, "delete-middle")) { offset = length / 2; remove = 1; insert = 0; }
        else if (!strcmp(op, "replace-end")) { offset = length - 1; remove = 1; insert = 1; }
        else if (!strcmp(op, "append-end")) { offset = length; remove = 0; insert = 1; }
        else return 2;
        unsigned char byte = (unsigned char)('A' + i % 26);
        double start = now_ms();
        PureResult result = pure_store_splice(path, "doc", revision, offset, remove,
                                              (PureSpan){&byte, insert}, PURE_OBJECT_LIMIT, &revision);
        samples[i] = now_ms() - start;
        if (result != PURE_OK) { fprintf(stderr, "splice failed: %d\n", (int)result); return 1; }
        length = length - remove + insert;
    }
    wchar = io_field("wchar") - wchar;
    device = io_field("write_bytes") - device;
    uint64_t checksum = 0;
    for (size_t i = 0; i < 16; ++i) {
        PureStoredView view;
        double start = now_ms();
        if (pure_store_get(path, "doc", PURE_OBJECT_LIMIT, &view) != PURE_OK || view.body.size != length) return 1;
        for (size_t k = 0; k < view.body.size; k += 64) checksum += view.body.data[k];
        pure_store_release(&view);
        reads[i] = now_ms() - start;
    }
    qsort(samples, count, sizeof(*samples), compare);
    qsort(reads, 16, sizeof(*reads), compare);
    printf("{\"store\":\"%s\",\"op\":\"%s\",\"document_bytes\":%zu,\"edits\":%zu,"
           "\"edit_median_ms\":%.3f,\"edit_p90_ms\":%.3f,\"edit_max_ms\":%.3f,"
           "\"write_call_bytes_per_edit\":%" PRIu64 ",\"device_write_bytes_per_edit\":%" PRIu64 ","
           "\"store_file_bytes_after\":%" PRIu64 ",\"whole_read_median_ms\":%.3f,\"read_checksum\":%" PRIu64 "}\n",
           name, op, size, count, samples[count / 2], samples[count * 9 / 10], samples[count - 1],
           wchar / count, device / count, store_bytes(path), reads[8], checksum);
    free(samples);
    free(body);
    return 0;
}
