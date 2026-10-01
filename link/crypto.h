#ifndef PURE_CRYPTO_H
#define PURE_CRYPTO_H
#include "seal.h"
#include "tls.h"

/* What pure-serve asks of OpenSSL, as the table pure-crypto.so exports
 * (link/crypto-openssl.c). pure-serve maps it, and OpenSSL with it, the
 * first time a server seals, opens a seal or connects to an https receiver
 * (link/crypto.c): one that does none of these maps no OpenSSL, 2.3 MiB
 * less resident and 0.6 MiB less of its own. */
enum { PURE_CRYPTO_VERSION = 6 };

typedef struct {
    unsigned version;
    /* TLS client: the context (verifying against ca_file, or the system's
     * CAs when NULL), sessions over connected non-blocking sockets, their
     * handshakes' steps, reads and writes (as link/tls.h says), and their
     * end. */
    bool (*tls_start)(const char *ca_file);
    PureTls *(*tls_open)(const char *host, int fd);
    int (*tls_step)(PureTls *);
    int (*tls_read)(PureTls *, void *bytes, int size);
    int (*tls_write)(PureTls *, const void *bytes, int size);
    void (*tls_close)(PureTls *);
    /* Sealing, as link/seal.h says; a sender's sessions in the suite named. */
    PureSealKey *(*seal_key)(const char *path, char public_hex[2 * PURE_SEAL_KEY + 1], char *error, size_t size);
    void (*seal_key_free)(PureSealKey *);
    PureSealer *(*sealer)(const char *suite, const unsigned char peer[PURE_SEAL_KEY], const PureSealKey *own,
                          unsigned char enc[PURE_SEAL_KEY]);
    bool (*seal)(PureSealer *, const char *aad, const unsigned char *plain, size_t size, unsigned char *out,
                 uint64_t *n);
    void (*sealer_free)(PureSealer *);
    PureOpener *(*opener)(const PureSealKey *, const char *suite, const unsigned char enc[PURE_SEAL_KEY],
                          const unsigned char *from);
    bool (*unseal)(PureOpener *, uint64_t n, const char *aad, const unsigned char *sealed, size_t size,
                   unsigned char *plain);
    void (*opener_free)(PureOpener *);
    /* Links, as link/seal.h says. */
    bool (*link_secret)(const PureSealKey *, const unsigned char peer[PURE_SEAL_KEY], unsigned char out[32]);
    bool (*link_ephemeral)(unsigned char private_key[32], unsigned char public_key[PURE_SEAL_KEY]);
    bool (*link_agree)(const unsigned char private_key[32], const unsigned char peer[PURE_SEAL_KEY], unsigned char out[32]);
    bool (*link_derive)(const unsigned char secret[32], const unsigned char *info, size_t info_size, unsigned char *out,
                        size_t out_size);
    PureAead *(*aead)(const unsigned char key[PURE_AEAD_KEY]);
    bool (*aead_seal)(PureAead *, const unsigned char *key, const unsigned char nonce[PURE_AEAD_NONCE],
                      const unsigned char *aad, size_t aad_size, const unsigned char *plain, size_t size,
                      unsigned char *out);
    bool (*aead_open)(PureAead *, const unsigned char *key, const unsigned char nonce[PURE_AEAD_NONCE],
                      const unsigned char *aad, size_t aad_size, const unsigned char *sealed, size_t size,
                      unsigned char *plain);
    void (*aead_free)(PureAead *);
} PureCrypto;
#endif
