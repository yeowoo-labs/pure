/* chain STORE: walk a tree store's commits from its older slot along their
 * records (links, or the 128-byte records of the format before), each
 * checked by its digest as the store checks it, and print one JSON line:
 * commits, their bytes a commit (record and bytes), the records' share. */
#include "../../../../store/digest.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { SLOT = 128, BLOCK = 4096, LINK = 40 };

static uint64_t le(const unsigned char *at) {
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) value |= (uint64_t)at[i] << (8 * i);
    return value;
}

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    char name[4096];
    snprintf(name, sizeof(name), "%s/pure.tree", argv[1]);
    FILE *file = fopen(name, "rb");
    if (!file || fseek(file, 0, SEEK_END)) return 1;
    long size = ftell(file);
    unsigned char *bytes = malloc((size_t)size);
    if (!bytes || fseek(file, 0, SEEK_SET) || fread(bytes, 1, (size_t)size, file) != (size_t)size) return 1;
    uint64_t seq = 0, at = 0; /* the older slot's head: more commits after it (a slot moves on every 16) */
    for (unsigned i = 0; i < 2; ++i) {
        const unsigned char *slot = bytes + i * BLOCK;
        if ((memcmp(slot, "PURETRE3", 8) && memcmp(slot, "PURETRE2", 8)) || le(slot + SLOT - 8) != digest_of(slot, SLOT - 8)) continue;
        if (!seq || le(slot + 8) < seq) seq = le(slot + 8), at = le(slot + 16);
    }
    if (!seq) return 1;
    uint64_t commits = 0, links = 0, earlier = 0, total = 0, records = 0;
    for (;;) {
        uint64_t next;
        unsigned char bound[8 + LINK - 8];
        for (unsigned i = 0; i < 8; ++i) bound[i] = (unsigned char)((seq + 1) >> (8 * i));
        if (at + LINK <= (uint64_t)size && !memcmp(bytes + at, "PRL1", 4) &&
            (memcpy(bound + 8, bytes + at, LINK - 8), le(bytes + at + LINK - 8) == digest_of(bound, sizeof(bound)))) {
            next = le(bytes + at + 8);
            records += LINK, ++links;
        } else if (at + SLOT <= (uint64_t)size && !memcmp(bytes + at, "PUREREC1", 8) &&
                   le(bytes + at + SLOT - 8) == digest_of(bytes + at, SLOT - 8) && le(bytes + at + 8) == seq + 1) {
            next = le(bytes + at + 16);
            records += SLOT, ++earlier;
        } else break;
        if (next > (uint64_t)size || next <= at) break;
        total += next - at;
        at = next;
        ++seq, ++commits;
    }
    printf("{\"commits\": %llu, \"links\": %llu, \"earlier_records\": %llu, \"bytes_a_commit\": %.1f, \"record_a_commit\": %.1f, "
           "\"newest_seq\": %llu}\n", (unsigned long long)commits, (unsigned long long)links, (unsigned long long)earlier,
           commits ? (double)total / commits : 0.0, commits ? (double)records / commits : 0.0, (unsigned long long)seq);
    return 0;
}
