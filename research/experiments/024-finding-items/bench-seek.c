/* Finding items in a keyed list the way TPC-C's stock is found (docs/research/109):
 *     bench-seek ITEMS OPERATIONS
 * A list of ITEMS items, five number fields, keyed by the first (keys 1..ITEMS);
 * then OPERATIONS random keys, four ways: a seek alone; a seek and three
 * fields read; and three fields read and each changed (journaled); the same
 * with a share every 30 changes, as a snapshot a save takes. Prints ns each,
 * and the machine's latency to memory (a random pointer chase over 64 MiB). */
#include "module-list.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static uint64_t now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

static uint64_t state = 0x9e3779b97f4a7c15u;
static uint64_t next(void) {
    state ^= state << 13, state ^= state >> 7, state ^= state << 17;
    return state;
}

int main(int argc, char **argv) {
    size_t items = argc > 1 ? strtoull(argv[1], NULL, 10) : 100000, operations = argc > 2 ? strtoull(argv[2], NULL, 10) : 2000000;
    static const uint8_t numbers[5] = {0};
    PureList *list = pure_list_new(5, numbers);
    if (!list || pure_list_keyed(list, 0) != PURE_OK) return 1;
    for (size_t i = 0; i < items; ++i) {
        PureCell cells[5] = {{.num = (double)(i + 1)}, {.num = 50}, {.num = 0}, {.num = 0}, {.num = 0}};
        if (pure_list_insert(list, i, cells, NULL) != PURE_OK) return 1;
    }
    pure_list_journal(list, true);
    double sink = 0;
    const char *names[4] = {"seek", "seek+3 reads", "seek+3 reads+3 changes", "same, a share each 30"};
    for (int way = 0; way < 4; ++way) {
        PureRetired retired = {0};
        PureList *copy = NULL;
        uint64_t epoch = 0;
        uint64_t began = now_ns();
        for (size_t op = 0; op < operations; ++op) {
            bool found;
            size_t at = pure_list_seek(list, 0, (PureCell){.num = (double)(next() % items + 1)}, &found);
            if (way == 0) { sink += (double)at; continue; }
            for (unsigned f = 1; f <= 3; ++f) {
                double value = pure_list_get(list, at, f).num;
                sink += value;
                if (way >= 2) pure_list_set(list, at, f, (PureCell){.num = value + 1}, NULL);
            }
            if (way >= 2 && op % 64 == 63) pure_list_forget(list, 0);
            if (way == 3 && op % 30 == 29) {
                if (copy) {
                    pure_list_free(copy);
                    pure_retired_reuse(&retired, &list, 1);
                }
                copy = pure_list_share(list, &retired, ++epoch);
            }
        }
        uint64_t took = now_ns() - began;
        if (copy) pure_list_free(copy);
        pure_retired_free(&retired);
        printf("%-26s %7.1f ns\n", names[way], (double)took / (double)operations);
    }
    /* The latency to memory: a random cycle through 64 MiB of pointers. */
    size_t slots = (64u << 20) / sizeof(size_t);
    size_t *chain = malloc(slots * sizeof(*chain)), *order = malloc(slots * sizeof(*order));
    for (size_t i = 0; i < slots; ++i) order[i] = i;
    for (size_t i = slots - 1; i > 0; --i) { size_t j = next() % (i + 1), t = order[i]; order[i] = order[j]; order[j] = t; }
    for (size_t i = 0; i < slots; ++i) chain[order[i]] = order[(i + 1) % slots];
    size_t at = order[0];
    uint64_t began = now_ns();
    for (size_t i = 0; i < 4000000; ++i) at = chain[at];
    printf("%-26s %7.1f ns\n", "memory latency", (double)(now_ns() - began) / 4000000.0);
    sink += (double)at;
    fprintf(stderr, "%g\n", sink);
    pure_list_free(list);
    return 0;
}
