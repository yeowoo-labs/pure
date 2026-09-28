#ifndef PURE_STORE_DIGEST_H
#define PURE_STORE_DIGEST_H
/* The store's digest, shared by its files (the tree, the log, list images). */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* The xxHash64 construction without a seed: four multiply-rotate lanes over
 * 32-byte stripes, several GB/s. An all-zero Digest is an empty one. */
typedef struct { uint64_t lane[4], length; unsigned char held[32]; size_t count; } Digest;

static inline uint64_t load64(const unsigned char *bytes) {
    uint64_t value;
    memcpy(&value, bytes, 8);
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    value = __builtin_bswap64(value);
#endif
    return value;
}

/* Digests find torn and missing writes; they are not a defence against a
 * hostile file. */
static const uint64_t P1 = UINT64_C(0x9E3779B185EBCA87), P2 = UINT64_C(0xC2B2AE3D27D4EB4F),
                      P3 = UINT64_C(0x165667B19E3779F9), P4 = UINT64_C(0x85EBCA77C2B2AE63),
                      P5 = UINT64_C(0x27D4EB2F165667C5);

static inline uint64_t rotate(uint64_t value, unsigned bits) { return value << bits | value >> (64 - bits); }
static inline uint64_t mix(uint64_t lane, uint64_t word) { return rotate(lane + word * P2, 31) * P1; }

static inline void stripes(Digest *digest, const unsigned char *bytes, size_t count) {
    uint64_t a = digest->lane[0], b = digest->lane[1], c = digest->lane[2], d = digest->lane[3];
    for (; count; --count, bytes += 32) {
        a = mix(a, load64(bytes));
        b = mix(b, load64(bytes + 8));
        c = mix(c, load64(bytes + 16));
        d = mix(d, load64(bytes + 24));
    }
    digest->lane[0] = a, digest->lane[1] = b, digest->lane[2] = c, digest->lane[3] = d;
}

static inline void digest_add(Digest *digest, const unsigned char *bytes, size_t length) {
    digest->length += length;
    if (digest->count) {
        size_t take = 32 - digest->count < length ? 32 - digest->count : length;
        memcpy(digest->held + digest->count, bytes, take);
        digest->count += take;
        bytes += take;
        length -= take;
        if (digest->count < 32) return;
        stripes(digest, digest->held, 1);
        digest->count = 0;
    }
    stripes(digest, bytes, length / 32);
    digest->count = length % 32;
    if (digest->count) memcpy(digest->held, bytes + length - digest->count, digest->count);
}

static inline uint64_t digest_end(const Digest *digest) {
    const uint64_t *lane = digest->lane;
    uint64_t hash = rotate(lane[0], 1) + rotate(lane[1], 7) + rotate(lane[2], 12) + rotate(lane[3], 18);
    for (unsigned i = 0; i < 4; ++i) hash = (hash ^ mix(0, lane[i])) * P1 + P4;
    hash += digest->length;
    size_t i = 0;
    for (; i + 8 <= digest->count; i += 8) hash = rotate(hash ^ mix(0, load64(digest->held + i)), 27) * P1 + P4;
    for (; i < digest->count; ++i) hash = rotate(hash ^ digest->held[i] * P5, 11) * P1;
    hash ^= hash >> 33;
    hash *= P2;
    hash ^= hash >> 29;
    hash *= P3;
    return hash ^ hash >> 32;
}

static inline uint64_t digest_of(const unsigned char *bytes, size_t length) {
    Digest digest = {0};
    digest_add(&digest, bytes, length);
    return digest_end(&digest);
}

#endif
