/* A prototype of 022's index, before building it in the list: leaves as a
 * B+tree keeps them (32 items, three number columns, allocated apart and
 * not in order), a bucket table (a power of two of keys each, about eight
 * items a bucket: the leaf and place of its first item) and the leaves'
 * table (pointer, first position). Per node, in a random order: the slice's
 * two bounds and its first item's second field, as seekfloor; against C's
 * offsets. */
#define _POSIX_C_SOURCE 200809L
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static uint64_t state = 121;
static uint64_t next(void) { state ^= state << 13; state ^= state >> 7; state ^= state << 17; return state; }
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e9 + t.tv_nsec; }
static int cmp64(const void *a, const void *b) { uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b; return (x > y) - (x < y); }
typedef struct { uint64_t made; uint32_t count; uint8_t leaf; size_t items; void *first; double lead; double key[32], to[32], w[32]; } Leaf;
typedef struct { const Leaf *leaf; uint32_t start; } LeafAt;
int main(int argc, char **argv) {
    unsigned per_bucket = argc > 1 ? (unsigned)atoi(argv[1]) : 8;
    enum { N = 100000, M = 1000000 };
    uint64_t *e = malloc(M * sizeof(*e));
    for (size_t k = 0; k < M; ++k) { uint64_t a = next() % N, b = next() % N; e[k] = a << 20 | b; }
    qsort(e, M, sizeof(*e), cmp64);
    size_t m = 0;
    for (size_t k = 0; k < M; ++k) if (!m || e[k] != e[m - 1]) e[m++] = e[k];
    size_t leaves = (m + 31) / 32;
    Leaf **pool = malloc(leaves * sizeof(*pool));
    for (size_t i = 0; i < leaves; ++i) pool[i] = malloc(sizeof(Leaf));
    for (size_t i = leaves - 1; i > 0; --i) { size_t j = next() % (i + 1); Leaf *t = pool[i]; pool[i] = pool[j]; pool[j] = t; } /* apart, not in order */
    LeafAt *table = malloc(leaves * sizeof(*table));
    for (size_t i = 0; i < leaves; ++i) {
        Leaf *l = pool[i];
        l->count = 0;
        for (size_t k = i * 32; k < m && k < i * 32 + 32; ++k) l->key[l->count] = (double)e[k], l->to[l->count] = (double)(e[k] & 0xFFFFF), l->w[l->count++] = 1;
        table[i] = (LeafAt){l, (uint32_t)(i * 32)};
    }
    double low = (double)e[0], span = (double)e[m - 1] - low;
    int exponent;
    frexp(span / (double)(m / per_bucket + 1), &exponent);
    double scale = ldexp(1.0, -exponent);
    low = floor(low * scale) / scale; /* buckets start at multiples of their width */
    span = (double)e[m - 1] - low;
    size_t buckets = (size_t)floor(span * scale) + 2;
    uint32_t *first = malloc(buckets * 4), *bucket_pos = malloc(buckets * 4);
    size_t filled = 0;
    for (size_t i = 0; i < leaves; ++i)
        for (unsigned j = 0; j < table[i].leaf->count; ++j) {
            size_t b = (size_t)floor((table[i].leaf->key[j] - low) * scale);
            while (filled <= b) bucket_pos[filled] = (uint32_t)(i * 32 + j), first[filled++] = (uint32_t)(i << 5 | j);
        }
    while (filled < buckets) bucket_pos[filled] = (uint32_t)m, first[filled++] = (uint32_t)(leaves << 5);
    uint32_t *start = calloc(N + 1, 4), *to = malloc(m * 4);
    for (size_t k = 0; k < m; ++k) { ++start[(e[k] >> 20) + 1]; to[k] = (uint32_t)(e[k] & 0xFFFFF); }
    for (size_t v = 0; v < N; ++v) start[v + 1] += start[v];
    uint32_t *order = malloc(N * 4);
    for (uint32_t v = 0; v < N; ++v) order[v] = v;
    for (uint32_t v = N - 1; v > 0; --v) { uint32_t w = (uint32_t)(next() % (v + 1)), t = order[v]; order[v] = order[w]; order[w] = t; }
    fprintf(stderr, "buckets %zu (%zu KB) leaves table %zu KB\n", buckets, buckets * 4 / 1024, leaves * sizeof(*table) / 1024);
    for (int round = 0; round < 5; ++round) {
        double t0 = now();
        size_t sum = 0;
        for (size_t i = 0; i < N; ++i) {
            size_t pos[2];
            const Leaf *leaf0 = NULL;
            unsigned place0 = 0;
            for (int side = 0; side < 2; ++side) {
                double key = (double)(((uint64_t)order[i] + (uint64_t)side) << 20);
                double at = floor((key - low) * scale);
                size_t b = !(at > 0) ? 0 : at >= (double)buckets ? buckets - 1 : (size_t)at;
                if (side && b < buckets && low + (double)b / scale == key) { pos[1] = bucket_pos[b]; break; } /* a bucket's start: its position, no leaf read */
                size_t li = first[b] >> 5;
                unsigned place = first[b] & 31;
                for (;;) {
                    if (li >= leaves) { pos[side] = m; break; }
                    const Leaf *l = table[li].leaf;
                    while (place < l->count && l->key[place] < key) ++place;
                    if (place < l->count) {
                        pos[side] = table[li].start + place;
                        if (!side) leaf0 = l, place0 = place;
                        break;
                    }
                    ++li, place = 0;
                }
            }
            sum += pos[1] - pos[0];
            if (pos[0] < pos[1]) sum += (size_t)leaf0->to[place0];
        }
        double t1 = now();
        size_t csum = 0;
        for (size_t i = 0; i < N; ++i) {
            uint32_t v = order[i], s = start[v], f = start[v + 1];
            csum += f - s;
            if (s < f) csum += to[s];
        }
        double t2 = now();
        printf("{\"per_bucket\":%u,\"round\":%d,\"index_ns_per_node\":%.1f,\"c_ns_per_node\":%.1f,\"same\":%d}\n", per_bucket, round,
               (t1 - t0) / N, (t2 - t1) / N, sum == csum);
    }
    return 0;
}
