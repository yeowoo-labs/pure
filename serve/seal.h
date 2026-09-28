#ifndef PURE_SEAL_H
#define PURE_SEAL_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Sends sealed end to end (docs/research/086-sealed-sends.md, 088): a
 * receiving node's key opens what a sender sealed to its public key, and
 * nothing between them (a relay, a proxy, a mailbox) can read or change it.
 * HPKE (RFC 9180): X25519, HKDF-SHA256, and AES-128-GCM where the CPU has AES
 * instructions, else ChaCha20-Poly1305; auth mode when the sender has a key
 * of its own, so the receiver knows who sealed. A sender keeps one session
 * (one encapsulation) and numbers its seals in it; a receiver opens a
 * session once and then each seal by its number. A private key lives in
 * OpenSSL's secure heap: locked in memory, out of core dumps, cleared when
 * freed. OpenSSL comes with pure-crypto.so (serve/crypto.c), mapped the
 * first time a key is read or a session made. */
enum { PURE_SEAL_KEY = 32, PURE_SEAL_OVERHEAD = 16 }; /* a public key; a seal's tag */

typedef struct PureSealKey PureSealKey;
typedef struct PureSealer PureSealer; /* a sender's session */
typedef struct PureOpener PureOpener; /* a receiver's session */

/* Whether this machine can seal (pure-crypto.so maps), with why not in error. */
bool pure_seal_ready(char *error, size_t size);
/* A node's key: read from path, or made and written there (0600) when there
 * is none; its public half in hex. NULL with why in error. */
PureSealKey *pure_seal_key(const char *path, char public_hex[2 * PURE_SEAL_KEY + 1], char *error, size_t size);
void pure_seal_key_free(PureSealKey *);
/* A public key from its hex. */
bool pure_seal_peer(const char *hex, unsigned char out[PURE_SEAL_KEY]);
/* The suite this machine seals with, as the X-Pure-Seal header names it. */
const char *pure_seal_suite(void);

/* A session sealing to peer, authenticated by own when given: its
 * encapsulated key into enc (what a receiver opens the session with). */
PureSealer *pure_sealer(const unsigned char peer[PURE_SEAL_KEY], const PureSealKey *own,
                        unsigned char enc[PURE_SEAL_KEY]);
/* plain sealed as the session's next seal, bound to aad: size +
 * PURE_SEAL_OVERHEAD bytes into out, and its number into n. */
bool pure_seal(PureSealer *, const char *aad, const unsigned char *plain, size_t size, unsigned char *out,
               uint64_t *n);
void pure_sealer_free(PureSealer *);

/* A session a sender made to key's public half, in suite (this machine's or
 * the other known one), from its enc; from is the sender's public key when
 * it sealed with its own (NULL otherwise). NULL when it does not open. */
PureOpener *pure_opener(const PureSealKey *key, const char *suite, const unsigned char enc[PURE_SEAL_KEY],
                        const unsigned char *from);
/* The session's seal number n: size - PURE_SEAL_OVERHEAD bytes into plain;
 * false when it does not open (another session, another aad, or changed on
 * the way). */
bool pure_unseal(PureOpener *, uint64_t n, const char *aad, const unsigned char *sealed, size_t size,
                 unsigned char *plain);
void pure_opener_free(PureOpener *);

/* Links (serve/link.h): the secret two nodes share (X25519 of this node's
 * private key and the other's public key: the same on both sides, and
 * never all zeros), keys from it (HKDF-SHA256 with the info given), and
 * AES-128-GCM under such a key with the nonce the caller gives (sealing may
 * be in place: out may be plain). A context is used by one thread at a time. */
typedef struct PureAead PureAead;
enum { PURE_AEAD_KEY = 16, PURE_AEAD_NONCE = 12, PURE_AEAD_TAG = 16 };
bool pure_link_secret(const PureSealKey *own, const unsigned char peer[PURE_SEAL_KEY], unsigned char out[32]);
bool pure_link_derive(const unsigned char secret[32], const unsigned char *info, size_t info_size, unsigned char *out,
                      size_t out_size);
PureAead *pure_aead(const unsigned char key[PURE_AEAD_KEY]);
/* size + PURE_AEAD_TAG bytes into out. */
bool pure_aead_seal(PureAead *, const unsigned char nonce[PURE_AEAD_NONCE], const unsigned char *aad, size_t aad_size,
                    const unsigned char *plain, size_t size, unsigned char *out);
/* size - PURE_AEAD_TAG bytes into plain; false when it does not open. */
bool pure_aead_open(PureAead *, const unsigned char nonce[PURE_AEAD_NONCE], const unsigned char *aad,
                    size_t aad_size, const unsigned char *sealed, size_t size, unsigned char *plain);
void pure_aead_free(PureAead *);
#endif
