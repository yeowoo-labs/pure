/* Research 046: what a store did to its files, as the shim (shim.c) saw it and the follower (follower.c) does it again.
 * An op is a u8 kind, its path (u16le length, bytes; relative to the store's directory, "" the directory itself), then:
 *   OP_CREATE   u32le the open's flags among O_CREAT, O_EXCL, O_TRUNC; u32le mode
 *   OP_WRITE    u64le offset, u32le length, the bytes
 *   OP_TRUNCATE u64le length
 *   OP_ALLOCATE u64le offset, u64le length (posix_fallocate)
 *   OP_RENAME   the second path (u16le length, bytes)
 *   OP_UNLINK   u32le flags (AT_REMOVEDIR)
 *   OP_MKDIR    u32le mode
 *   OP_SYNC     (an fsync or fdatasync of that path: where a cut ends)
 * A cut is the ops since the last, ending in one OP_SYNC. On the link a record is u8 1 when it ends its cut (else 0),
 * u64le the cut's number, then whole ops. */
#ifndef OPS_H
#define OPS_H
#include <stdint.h>
#include <string.h>

enum { OP_CREATE = 1, OP_WRITE, OP_TRUNCATE, OP_ALLOCATE, OP_RENAME, OP_UNLINK, OP_MKDIR, OP_SYNC, OP_KINDS };
static const char *const op_names[OP_KINDS] = {"", "create", "write", "truncate", "allocate", "rename", "unlink", "mkdir", "sync"};

static inline void put16(unsigned char *p, uint16_t v) { p[0] = (unsigned char)v, p[1] = (unsigned char)(v >> 8); }
static inline void put32(unsigned char *p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = (unsigned char)(v >> (8 * i)); }
static inline void put64(unsigned char *p, uint64_t v) { for (int i = 0; i < 8; ++i) p[i] = (unsigned char)(v >> (8 * i)); }
static inline uint16_t get16(const unsigned char *p) { return (uint16_t)(p[0] | p[1] << 8); }
static inline uint32_t get32(const unsigned char *p) { uint32_t v = 0; for (int i = 3; i >= 0; --i) v = v << 8 | p[i]; return v; }
static inline uint64_t get64(const unsigned char *p) { uint64_t v = 0; for (int i = 7; i >= 0; --i) v = v << 8 | p[i]; return v; }

/* An op's size from its start (at most size bytes there), or 0 when it is not whole. */
static inline size_t op_size(const unsigned char *p, size_t size) {
    if (size < 3) return 0;
    size_t at = 3 + get16(p + 1);
    if (at > size) return 0;
    switch (p[0]) {
    case OP_CREATE: at += 8; break;
    case OP_WRITE: if (at + 12 > size) return 0; at += 12 + get32(p + at + 8); break;
    case OP_TRUNCATE: at += 8; break;
    case OP_ALLOCATE: at += 16; break;
    case OP_RENAME: if (at + 2 > size) return 0; at += 2 + get16(p + at); break;
    case OP_UNLINK: case OP_MKDIR: at += 4; break;
    case OP_SYNC: break;
    default: return 0;
    }
    return at <= size ? at : 0;
}
#endif
