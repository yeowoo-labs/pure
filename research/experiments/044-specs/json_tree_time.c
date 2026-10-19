/* Research 140, design 027 stage 2b: aup's examples/json_tree.aup (run by
 * Pure: a JSON text read into a tree, then a digest of the tree) against C:
 * cJSON (the system's libcjson.so.1, its few declarations written here)
 * reading the same document into its tree, and the same digest. Both digests
 * must be equal; then each is timed (the median of seven runs of rounds).
 *
 *   cc -O2 -std=c17 -I../../../core json_tree_time.c ../../../build/libpure.a -l:libcjson.so.1 -lm -pthread -o json_tree_time
 *   ./json_tree_time json_tree.pmod ROUNDS
 *
 * The document is json_time.c's: 2,000 objects of a key and an array of a
 * number, true, null and a string with an escape. One JSON line. */
#define _POSIX_C_SOURCE 200809L
#include "module.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct cJSON {
    struct cJSON *next, *prev, *child;
    int type;
    char *valuestring;
    int valueint;
    double valuedouble;
    char *string;
} cJSON;
cJSON *cJSON_Parse(const char *value);
void cJSON_Delete(cJSON *item);
enum { FALSE_ = 1, TRUE_ = 2, NULL_ = 4, NUMBER = 8, STRING = 16, ARRAY = 32, OBJECT = 64 };

/* nulls, trues, falses, numbers, strings, arrays, objects, sum, text bytes, key bytes: breadth first, as json_tree.aup. */
typedef struct { double count[7], sum, text_bytes, key_bytes; } Digest;
static Digest digest(const char *text, cJSON **queue) {
    Digest d = {{0}, 0, 0, 0};
    cJSON *root = cJSON_Parse(text);
    size_t at = 0, end = 0;
    queue[end++] = root;
    while (at < end) {
        cJSON *x = queue[at++];
        switch (x->type & 0xFF) {
        case NULL_: d.count[0]++; break;
        case TRUE_: d.count[1]++; break;
        case FALSE_: d.count[2]++; break;
        case NUMBER: d.count[3]++, d.sum += x->valuedouble; break;
        case STRING: d.count[4]++, d.text_bytes += (double)strlen(x->valuestring); break;
        case ARRAY: d.count[5]++; for (cJSON *y = x->child; y; y = y->next) queue[end++] = y; break;
        default: d.count[6]++; for (cJSON *y = x->child; y; y = y->next) d.key_bytes += (double)strlen(y->string), queue[end++] = y;
        }
    }
    cJSON_Delete(root);
    return d;
}

static double now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e9 + (double)t.tv_nsec;
}

static int by_value(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

int main(int argc, char **argv) {
    if (argc != 3) { fprintf(stderr, "usage: %s MODULE ROUNDS\n", argv[0]); return 2; }
    int rounds = atoi(argv[2]);
    static char doc[1 << 18];
    static cJSON *queue[1 << 16];
    size_t n = 0;
    doc[n++] = '[';
    for (int k = 0; k < 2000; ++k)
        n += (size_t)sprintf(doc + n, "%s{\"key%d\": [1.5e3, true, null, \"text \\u00e9 %d\"]}", k ? "," : "", k, k);
    doc[n++] = ']';
    doc[n] = 0;
    PureModule module = {0};
    char error[160];
    if (pure_module_read(argv[1], &module, error, sizeof(error)) != PURE_OK) { fprintf(stderr, "%s\n", error); return 1; }
    PureApp app;
    if (pure_app_start(&app, &module, (PureData){0}) != PURE_OK) return 1;
    int check = pure_module_input(&module, "check");
    if (pure_app_message(&app, (uint32_t)check, (PureSpan){(const unsigned char *)doc, n}) != PURE_OK) {
        fprintf(stderr, "event failed\n");
        return 1;
    }
    uint64_t work = app.work;
    Digest c = digest(doc, queue);
    /* The states after valid: nulls, trues, falses, numbers, strings, arrays, objects, sum, text_bytes, key_bytes. */
    const char *names[] = {"nulls", "trues", "falses", "numbers", "strings", "arrays", "objects", "sum", "text_bytes", "key_bytes"};
    double want[] = {c.count[0], c.count[1], c.count[2], c.count[3], c.count[4], c.count[5], c.count[6], c.sum, c.text_bytes, c.key_bytes};
    for (unsigned k = 0; k < 10; ++k) {
        int state = -1;
        for (uint32_t s = 0; s < module.states; ++s) if (!strcmp(module.state_names[s], names[k])) state = (int)s;
        if (state < 0 || app.values[state].num != want[k]) {
            fprintf(stderr, "%s: %g against %g\n", names[k], state < 0 ? -1 : app.values[state].num, want[k]);
            return 1;
        }
    }
    double c_runs[7], pure_runs[7];
    volatile double sink = 0;
    for (int r = 0; r < 7; ++r) {
        double start = now_ns();
        for (int i = 0; i < rounds; ++i) sink += digest(doc, queue).sum;
        c_runs[r] = (now_ns() - start) / rounds;
        start = now_ns();
        for (int i = 0; i < rounds; ++i)
            if (pure_app_message(&app, (uint32_t)check, (PureSpan){(const unsigned char *)doc, n}) != PURE_OK) return 1;
        pure_runs[r] = (now_ns() - start) / rounds;
    }
    qsort(c_runs, 7, sizeof(double), by_value);
    qsort(pure_runs, 7, sizeof(double), by_value);
    const char *jit = getenv("PURE_JIT"), *batch = getenv("PURE_BATCH");
    printf("{\"size\":%zu,\"jit\":\"%s\",\"batch\":\"%s\",\"c_ns_per_byte\":%.3f,\"pure_ns_per_byte\":%.3f,\"ratio\":%.1f,"
           "\"instructions\":%llu,\"instructions_per_byte\":%.1f}\n",
           n, jit ? jit : "", batch ? batch : "", c_runs[3] / (double)n, pure_runs[3] / (double)n, pure_runs[3] / c_runs[3],
           (unsigned long long)work, (double)work / (double)n);
    pure_app_stop(&app);
    return 0;
}
