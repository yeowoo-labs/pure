#ifndef PURE_SEAL_H
#define PURE_SEAL_H
#include <stdbool.h>
#include <stddef.h>

/* Sends sealed end to end (docs/research/086-sealed-sends.md): a receiving
 * node's key opens what a sender sealed to its public key, and nothing
 * between them (a relay, a proxy, a mailbox) can read or change it. HPKE
 * (RFC 9180) base mode: X25519, HKDF-SHA256, and AES-128-GCM where the CPU
 * has AES instructions, else ChaCha20-Poly1305. A private key lives in
 * OpenSSL's secure heap: locked in memory, out of core dumps, cleared when
 * freed. */
enum { PURE_SEAL_KEY = 32, PURE_SEAL_OVERHEAD = 32 + 16 }; /* the encapsulated key, then the tag */

typedef struct PureSealKey PureSealKey;
/* A node's key: read from path, or made and written there (0600) when there
 * is none; its public half in hex. NULL with why in error. */
PureSealKey *pure_seal_key(const char *path, char public_hex[2 * PURE_SEAL_KEY + 1], char *error, size_t size);
void pure_seal_key_free(PureSealKey *);
/* A public key from its hex. */
bool pure_seal_peer(const char *hex, unsigned char out[PURE_SEAL_KEY]);
/* The suite this machine seals with, as the X-Pure-Seal header names it. */
const char *pure_seal_suite(void);
/* plain sealed to peer, bound to aad: size + PURE_SEAL_OVERHEAD bytes into out. */
bool pure_seal(const unsigned char peer[PURE_SEAL_KEY], const char *aad, const unsigned char *plain, size_t size,
               unsigned char *out);
/* Opened with key under suite (either this machine's or the other known one):
 * size - PURE_SEAL_OVERHEAD bytes into plain; false when it does not open
 * (another key, another aad, or changed on the way). */
bool pure_unseal(const PureSealKey *, const char *suite, const char *aad, const unsigned char *sealed, size_t size,
                 unsigned char *plain);
#endif
