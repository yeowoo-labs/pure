/* Research 140, design 027 stage 3: the cost of appending into a stored list's item's list (aup's check.py BOARD_SOURCE
 * compiled, add_card into one column, in memory): ns an add, on average so far, as the list grows.
 *   cc -O2 -std=c17 -I../../../core append_cost.c ../../../build/libpure.a -lm -pthread -o append_cost
 *   ./append_cost board.pmod */
#define _POSIX_C_SOURCE 200809L
#include "module.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
static double now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e9 + t.tv_nsec; }
int main(int argc, char **argv) {
    PureModule m = {0}; char error[160];
    if (pure_module_read(argv[1], &m, error, sizeof(error)) != PURE_OK) { fprintf(stderr, "%s\n", error); return 1; }
    PureApp app;
    if (pure_app_start(&app, &m, (PureData){0}) != PURE_OK) return 1;
    int column = pure_module_input(&m, "add_column"), card = pure_module_input(&m, "add_card");
    pure_app_message(&app, (uint32_t)column, NULL, -1, (PureSpan){(const unsigned char *)"todo", 4});
    unsigned char payload[16];
    double zero = 0;
    memcpy(payload, &zero, 8);
    memcpy(payload + 8, "card0001", 8);
    double start = now_ns();
    for (int n = 1; n <= 8000; ++n) {
        if (pure_app_message(&app, (uint32_t)card, NULL, -1, (PureSpan){payload, 16}) != PURE_OK) { fprintf(stderr, "fail at %d\n", n); return 1; }
        if (n == 500 || n == 1000 || n == 2000 || n == 4000 || n == 8000) {
            double t = now_ns();
            printf("{\"cards\":%d,\"ns_per_add_so_far\":%.0f}\n", n, (t - start) / n);
        }
    }
    return 0;
}
