#define _GNU_SOURCE
/* pure-crypto.so: what pure-serve asks of OpenSSL (serve/crypto.h), mapped
 * only when a server seals, opens a seal or reaches an https receiver. It
 * exports one table, pure_crypto. */
#include "crypto.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/core_names.h>
#include <openssl/hpke.h>
#include <openssl/kdf.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---- TLS: one client context, verifying the receiver's certificate against
 * the system's CAs or the file given, and its name. */
static SSL_CTX *tls_context;

static bool tls_start(const char *ca_file) {
    SSL_CTX *context = SSL_CTX_new(TLS_client_method());
    if (!context) return false;
    if (!SSL_CTX_set_min_proto_version(context, TLS1_2_VERSION) ||
        !(ca_file ? SSL_CTX_load_verify_locations(context, ca_file, NULL) : SSL_CTX_set_default_verify_paths(context))) {
        SSL_CTX_free(context);
        return false;
    }
    SSL_CTX_set_verify(context, SSL_VERIFY_PEER, NULL);
    SSL_CTX_set_mode(context, SSL_MODE_AUTO_RETRY);
    tls_context = context;
    return true;
}

/* TLS over a connected socket: the receiver's certificate must name the host
 * (a DNS name, or an IP address written as one). */
static PureTls *tls_open(const char *host, int fd) {
    SSL *ssl = SSL_new(tls_context);
    if (!ssl) return NULL;
    unsigned char address[16];
    bool literal = inet_pton(AF_INET, host, address) == 1 || inet_pton(AF_INET6, host, address) == 1;
    bool named = literal ? X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(ssl), host) == 1 :
                           SSL_set_tlsext_host_name(ssl, host) == 1 && SSL_set1_host(ssl, host) == 1;
    if (!named || SSL_set_fd(ssl, fd) != 1 || SSL_connect(ssl) != 1 || SSL_get_verify_result(ssl) != X509_V_OK) {
        SSL_free(ssl);
        return NULL;
    }
    return (PureTls *)ssl;
}

static int tls_read(PureTls *tls, void *bytes, int size) { return SSL_read((SSL *)tls, bytes, size); }
static int tls_write(PureTls *tls, const void *bytes, int size) { return SSL_write((SSL *)tls, bytes, size); }
static void tls_close(PureTls *tls) { SSL_free((SSL *)tls); }

/* ---- Sealing: HPKE (RFC 9180), X25519 and HKDF-SHA256, with the AEAD the
 * suite names; auth mode when the sender seals with a key of its own. A
 * session is one encapsulation, its seals numbered by the context. */
struct PureSealKey { EVP_PKEY *key; };
struct PureSealer { OSSL_HPKE_CTX *context; };
struct PureOpener { OSSL_HPKE_CTX *context; };

static const unsigned char info[] = "pure sealed send 2";

static bool suite_named(const char *name, OSSL_HPKE_SUITE *out) {
    OSSL_HPKE_SUITE suite = {OSSL_HPKE_KEM_ID_X25519, OSSL_HPKE_KDF_ID_HKDF_SHA256, 0};
    if (!strcmp(name, "x25519-sha256-aes128gcm")) suite.aead_id = OSSL_HPKE_AEAD_ID_AES_GCM_128;
    else if (!strcmp(name, "x25519-sha256-chacha20poly1305")) suite.aead_id = OSSL_HPKE_AEAD_ID_CHACHA_POLY1305;
    else return false;
    *out = suite;
    return true;
}

static pthread_once_t secure_once = PTHREAD_ONCE_INIT;
static void secure_heap(void) { CRYPTO_secure_malloc_init(16384, 32); }

static PureSealKey *seal_key(const char *path, char public_hex[2 * PURE_SEAL_KEY + 1], char *error, size_t size) {
    pthread_once(&secure_once, secure_heap);
    unsigned char *raw = OPENSSL_secure_malloc(PURE_SEAL_KEY), public_key[PURE_SEAL_KEY];
    PureSealKey *made = calloc(1, sizeof(*made));
    EVP_PKEY *key = NULL;
    size_t length = PURE_SEAL_KEY;
    if (!raw || !made) { snprintf(error, size, "out of memory"); goto fail; }
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    struct stat info_;
    if (fd >= 0) {
        bool ok = !fstat(fd, &info_) && S_ISREG(info_.st_mode) && !(info_.st_mode & 077) &&
                  read(fd, raw, PURE_SEAL_KEY) == PURE_SEAL_KEY && info_.st_size == PURE_SEAL_KEY;
        close(fd);
        if (!ok) { snprintf(error, size, "%s: a key is 32 bytes that only its owner may read (0600)", path); goto fail; }
        key = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, NULL, raw, PURE_SEAL_KEY);
    } else if (errno == ENOENT) {
        /* A new key: made, written whole and synced before it is used. */
        key = EVP_PKEY_Q_keygen(NULL, NULL, "X25519");
        fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        bool ok = key && fd >= 0 && EVP_PKEY_get_raw_private_key(key, raw, &length) == 1 && length == PURE_SEAL_KEY &&
                  write(fd, raw, PURE_SEAL_KEY) == PURE_SEAL_KEY && !fsync(fd);
        if (fd >= 0 && close(fd)) ok = false;
        if (!ok) { snprintf(error, size, "%s: cannot make a key there", path); unlink(path); goto fail; }
    } else { snprintf(error, size, "%s: %s", path, strerror(errno)); goto fail; }
    length = PURE_SEAL_KEY;
    if (!key || EVP_PKEY_get_raw_public_key(key, public_key, &length) != 1 || length != PURE_SEAL_KEY) {
        snprintf(error, size, "%s: not an X25519 key", path);
        goto fail;
    }
    for (size_t i = 0; i < PURE_SEAL_KEY; ++i) snprintf(public_hex + 2 * i, 3, "%02x", public_key[i]);
    OPENSSL_secure_clear_free(raw, PURE_SEAL_KEY);
    made->key = key;
    return made;
fail:
    EVP_PKEY_free(key);
    if (raw) OPENSSL_secure_clear_free(raw, PURE_SEAL_KEY);
    free(made);
    return NULL;
}

static void seal_key_free(PureSealKey *key) {
    if (!key) return;
    EVP_PKEY_free(key->key);
    free(key);
}

static PureSealer *sealer(const char *suite_name, const unsigned char peer[PURE_SEAL_KEY], const PureSealKey *own,
                          unsigned char enc[PURE_SEAL_KEY]) {
    OSSL_HPKE_SUITE suite;
    PureSealer *made = suite_named(suite_name, &suite) ? calloc(1, sizeof(*made)) : NULL;
    if (!made) return NULL;
    made->context = OSSL_HPKE_CTX_new(own ? OSSL_HPKE_MODE_AUTH : OSSL_HPKE_MODE_BASE, suite, OSSL_HPKE_ROLE_SENDER,
                                      NULL, NULL);
    size_t length = PURE_SEAL_KEY;
    if (!made->context || (own && OSSL_HPKE_CTX_set1_authpriv(made->context, own->key) != 1) ||
        OSSL_HPKE_encap(made->context, enc, &length, peer, PURE_SEAL_KEY, info, sizeof(info) - 1) != 1 ||
        length != PURE_SEAL_KEY) {
        OSSL_HPKE_CTX_free(made->context);
        free(made);
        return NULL;
    }
    return made;
}

static bool seal(PureSealer *s, const char *aad, const unsigned char *plain, size_t size, unsigned char *out,
                 uint64_t *n) {
    size_t sealed = size + PURE_SEAL_OVERHEAD;
    return OSSL_HPKE_CTX_get_seq(s->context, n) == 1 &&
           OSSL_HPKE_seal(s->context, out, &sealed, (const unsigned char *)aad, strlen(aad), plain, size) == 1 &&
           sealed == size + PURE_SEAL_OVERHEAD;
}

static void sealer_free(PureSealer *s) {
    if (!s) return;
    OSSL_HPKE_CTX_free(s->context);
    free(s);
}

static PureOpener *opener(const PureSealKey *key, const char *suite_name, const unsigned char enc[PURE_SEAL_KEY],
                          const unsigned char *from) {
    OSSL_HPKE_SUITE suite;
    PureOpener *made = key && suite_named(suite_name, &suite) ? calloc(1, sizeof(*made)) : NULL;
    if (!made) return NULL;
    made->context = OSSL_HPKE_CTX_new(from ? OSSL_HPKE_MODE_AUTH : OSSL_HPKE_MODE_BASE, suite, OSSL_HPKE_ROLE_RECEIVER,
                                      NULL, NULL);
    if (!made->context || (from && OSSL_HPKE_CTX_set1_authpub(made->context, from, PURE_SEAL_KEY) != 1) ||
        OSSL_HPKE_decap(made->context, enc, PURE_SEAL_KEY, key->key, info, sizeof(info) - 1) != 1) {
        OSSL_HPKE_CTX_free(made->context);
        free(made);
        return NULL;
    }
    return made;
}

/* A receiver may set the number (a sender may not: it would reuse a nonce). */
static bool unseal(PureOpener *o, uint64_t n, const char *aad, const unsigned char *sealed, size_t size,
                   unsigned char *plain) {
    size_t opened = size - PURE_SEAL_OVERHEAD;
    return size >= PURE_SEAL_OVERHEAD && OSSL_HPKE_CTX_set_seq(o->context, n) == 1 &&
           OSSL_HPKE_open(o->context, plain, &opened, (const unsigned char *)aad, strlen(aad), sealed, size) == 1 &&
           opened == size - PURE_SEAL_OVERHEAD;
}

static void opener_free(PureOpener *o) {
    if (!o) return;
    OSSL_HPKE_CTX_free(o->context);
    free(o);
}

/* ---- Links: X25519 between two nodes' keys, HKDF-SHA256, AES-128-GCM with
 * the caller's nonce (one context sealing and one opening, each keeping its
 * key's schedule). */
struct PureAead { EVP_CIPHER_CTX *seal, *open; };

static bool link_secret(const PureSealKey *own, const unsigned char peer[PURE_SEAL_KEY], unsigned char out[32]) {
    EVP_PKEY *theirs = EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, NULL, peer, PURE_SEAL_KEY);
    EVP_PKEY_CTX *context = theirs && own ? EVP_PKEY_CTX_new(own->key, NULL) : NULL;
    size_t length = 32;
    bool ok = context && EVP_PKEY_derive_init(context) == 1 && EVP_PKEY_derive_set_peer(context, theirs) == 1 &&
              EVP_PKEY_derive(context, out, &length) == 1 && length == 32;
    EVP_PKEY_CTX_free(context);
    EVP_PKEY_free(theirs);
    unsigned char any = 0; /* a small-order key makes zeros: no secret */
    for (size_t i = 0; ok && i < 32; ++i) any |= out[i];
    return ok && any;
}

static bool link_derive(const unsigned char secret[32], const unsigned char *info, size_t info_size, unsigned char *out,
                        size_t out_size) {
    static const char salt[] = "pure link 0";
    EVP_KDF *kdf = EVP_KDF_fetch(NULL, "HKDF", NULL);
    EVP_KDF_CTX *context = kdf ? EVP_KDF_CTX_new(kdf) : NULL;
    OSSL_PARAM params[] = {OSSL_PARAM_construct_utf8_string(OSSL_KDF_PARAM_DIGEST, (char *)"SHA256", 0),
                           OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_KEY, (void *)secret, 32),
                           OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_SALT, (void *)salt, sizeof(salt) - 1),
                           OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_INFO, (void *)info, info_size),
                           OSSL_PARAM_construct_end()};
    bool ok = context && EVP_KDF_derive(context, out, out_size, params) == 1;
    EVP_KDF_CTX_free(context);
    EVP_KDF_free(kdf);
    return ok;
}

static void aead_free(PureAead *a) {
    if (!a) return;
    EVP_CIPHER_CTX_free(a->seal);
    EVP_CIPHER_CTX_free(a->open);
    free(a);
}

static PureAead *aead(const unsigned char key[PURE_AEAD_KEY]) {
    PureAead *a = calloc(1, sizeof(*a));
    if (!a) return NULL;
    a->seal = EVP_CIPHER_CTX_new();
    a->open = EVP_CIPHER_CTX_new();
    if (!a->seal || !a->open || EVP_EncryptInit_ex(a->seal, EVP_aes_128_gcm(), NULL, key, NULL) != 1 ||
        EVP_DecryptInit_ex(a->open, EVP_aes_128_gcm(), NULL, key, NULL) != 1) {
        aead_free(a);
        return NULL;
    }
    return a;
}

static bool aead_seal(PureAead *a, const unsigned char nonce[PURE_AEAD_NONCE], const unsigned char *aad,
                      size_t aad_size, const unsigned char *plain, size_t size, unsigned char *out) {
    int n = 0, last = 0;
    return size <= INT_MAX && EVP_EncryptInit_ex(a->seal, NULL, NULL, NULL, nonce) == 1 &&
           (!aad_size || EVP_EncryptUpdate(a->seal, NULL, &n, aad, (int)aad_size) == 1) &&
           (!size || EVP_EncryptUpdate(a->seal, out, &n, plain, (int)size) == 1) &&
           EVP_EncryptFinal_ex(a->seal, out + size, &last) == 1 &&
           EVP_CIPHER_CTX_ctrl(a->seal, EVP_CTRL_GCM_GET_TAG, PURE_AEAD_TAG, out + size) == 1;
}

static bool aead_open(PureAead *a, const unsigned char nonce[PURE_AEAD_NONCE], const unsigned char *aad,
                      size_t aad_size, const unsigned char *sealed, size_t size, unsigned char *plain) {
    int n = 0, last = 0;
    if (size < PURE_AEAD_TAG || size - PURE_AEAD_TAG > INT_MAX) return false;
    size_t body = size - PURE_AEAD_TAG;
    return EVP_DecryptInit_ex(a->open, NULL, NULL, NULL, nonce) == 1 &&
           (!aad_size || EVP_DecryptUpdate(a->open, NULL, &n, aad, (int)aad_size) == 1) &&
           (!body || EVP_DecryptUpdate(a->open, plain, &n, sealed, (int)body) == 1) &&
           EVP_CIPHER_CTX_ctrl(a->open, EVP_CTRL_GCM_SET_TAG, PURE_AEAD_TAG, (void *)(sealed + body)) == 1 &&
           EVP_DecryptFinal_ex(a->open, plain + body, &last) == 1;
}

const PureCrypto pure_crypto = {PURE_CRYPTO_VERSION, tls_start, tls_open, tls_read, tls_write, tls_close,
                                seal_key, seal_key_free, sealer, seal, sealer_free, opener, unseal, opener_free,
                                link_secret, link_derive, aead, aead_seal, aead_open, aead_free};
