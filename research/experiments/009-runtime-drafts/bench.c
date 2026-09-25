/* Where an app's working value lives, measured on one store (store-tree.c).
 *
 * copy:  the app loads its own copy into a gap buffer, types into it, and on
 *        save reads the stored value back, finds the changed range by
 *        comparing prefix and suffix, then submits that splice (the pattern
 *        the current terminal editor uses).
 * draft: the runtime owns the working version; typing is a draft splice and
 *        save is a commit of the draft's new extents.
 *
 * Each save follows one typed character in the middle of the document.
 * "create" makes the stored document in a separate process, so the peak RSS
 * of copy and draft covers only the editing session. */
#define _POSIX_C_SOURCE 200809L
#include "state.h"
#include "store.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_us(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e6 + (double)t.tv_nsec / 1e3;
}

static int compare(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

/* Linux VmHWM belongs to this exec's address space; ru_maxrss would carry
 * the launching interpreter's peak across exec. */
static long peak_kib(void) {
    FILE *file = fopen("/proc/self/status", "r");
    char line[256];
    long value = -1;
    while (file && fgets(line, sizeof(line), file))
        if (!strncmp(line, "VmHWM:", 6)) value = strtol(line + 6, NULL, 10);
    if (file) fclose(file);
    return value;
}

typedef struct { unsigned char *text; size_t capacity, gap_start, gap_end; } Gap;

static unsigned char at(const Gap *g, size_t i) {
    return i < g->gap_start ? g->text[i] : g->text[i + (g->gap_end - g->gap_start)];
}

int main(int argc, char **argv) {
    if (argc != 5 || (strcmp(argv[1], "copy") && strcmp(argv[1], "draft") && strcmp(argv[1], "create"))) {
        fputs("bench create|copy|draft DIRECTORY SIZE SAVES\n", stderr);
        return 2;
    }
    const char *mode = argv[1], *path = argv[2];
    size_t size = strtoull(argv[3], NULL, 10), saves = strtoull(argv[4], NULL, 10);
    int64_t revision = 1;
    if (!strcmp(mode, "create")) {
        unsigned char *body = malloc(size);
        if (!body || pure_store_init(path) != PURE_OK) return 1;
        for (size_t i = 0; i < size; ++i) body[i] = (unsigned char)('a' + i % 26);
        return pure_store_put(path, "doc", 0, (PureSpan){body, size}, &revision) != PURE_OK;
    }
    double *save = malloc(saves * sizeof(*save)), *key = malloc(saves * sizeof(*key));
    if (!save || !key) return 1;
    size_t cursor = size / 2, length = size;
    if (!strcmp(mode, "copy")) {
        PureStoredView view;
        if (pure_store_get(path, "doc", PURE_OBJECT_LIMIT, &view) != PURE_OK) return 1;
        Gap g = {malloc(size + 4096), size + 4096, cursor, cursor + 4096};
        if (!g.text) return 1;
        memcpy(g.text, view.body.data, cursor);
        memcpy(g.text + g.gap_end, view.body.data + cursor, size - cursor);
        pure_store_release(&view);
        for (size_t i = 0; i < saves; ++i) {
            double start = now_us();
            g.text[g.gap_start++] = (unsigned char)('A' + i % 26);
            ++length;
            key[i] = now_us() - start;
            start = now_us();
            if (pure_store_get(path, "doc", PURE_OBJECT_LIMIT, &view) != PURE_OK) return 1;
            size_t old = view.body.size, prefix = 0, suffix = 0;
            while (prefix < old && prefix < length && view.body.data[prefix] == at(&g, prefix)) ++prefix;
            while (suffix < old - prefix && suffix < length - prefix &&
                   view.body.data[old - suffix - 1] == at(&g, length - suffix - 1)) ++suffix;
            size_t remove = old - prefix - suffix, insert = length - prefix - suffix;
            unsigned char *bytes = malloc(insert ? insert : 1);
            for (size_t k = 0; k < insert; ++k) bytes[k] = at(&g, prefix + k);
            pure_store_release(&view);
            PureResult result = pure_store_splice(path, "doc", revision, prefix, remove,
                                                  (PureSpan){bytes, insert}, PURE_OBJECT_LIMIT, &revision);
            free(bytes);
            save[i] = now_us() - start;
            if (result != PURE_OK) return 1;
        }
        free(g.text);
    } else {
        PureDraft *draft;
        if (pure_draft_open(path, "doc", &draft) != PURE_OK) return 1;
        for (size_t i = 0; i < saves; ++i) {
            unsigned char byte = (unsigned char)('A' + i % 26);
            double start = now_us();
            if (pure_draft_splice(draft, cursor++, 0, (PureSpan){&byte, 1}) != PURE_OK) return 1;
            key[i] = now_us() - start;
            start = now_us();
            if (pure_draft_commit(draft, &revision) != PURE_OK) return 1;
            save[i] = now_us() - start;
        }
        pure_draft_close(draft);
    }
    qsort(save, saves, sizeof(*save), compare);
    qsort(key, saves, sizeof(*key), compare);
    printf("{\"mode\":\"%s\",\"document_bytes\":%zu,\"saves\":%zu,\"keystroke_median_us\":%.2f,"
           "\"save_median_ms\":%.3f,\"save_p90_ms\":%.3f,\"peak_rss_kib\":%ld,\"final_revision\":%lld}\n",
           mode, size, saves, key[saves / 2], save[saves / 2] / 1e3, save[saves * 9 / 10] / 1e3,
           peak_kib(), (long long)revision);
    free(save);
    free(key);
    return 0;
}
