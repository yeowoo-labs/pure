#define _GNU_SOURCE
#include "seal.h"
#include <errno.h>
#include <fcntl.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hpke.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__aarch64__) && defined(__linux__)
#include <asm/hwcap.h>
#include <sys/auxv.h>
#endif

struct PureSealKey { EVP_PKEY *key; };

static const unsigned char info[] = "pure sealed send 1";
static const char aes_name[] = "x25519-sha256-aes128gcm", chacha_name[] = "x25519-sha256-chacha20poly1305";

/* AES-GCM where AES is in the CPU (two to three times ChaCha20-Poly1305 from
 * a kilobyte up on a Cortex-A76, research 086); ChaCha20-Poly1305, fast in
 * plain code, elsewhere. */
static bool has_aes(void) {
#if defined(__aarch64__) && defined(__linux__)
    return (getauxval(AT_HWCAP) & HWCAP_AES) && (getauxval(AT_HWCAP) & HWCAP_PMULL);
#elif defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
    return __builtin_cpu_supports("aes") && __builtin_cpu_supports("pclmul");
#else
    return false;
#endif
}

static bool suite_named(const char *name, OSSL_HPKE_SUITE *out) {
    OSSL_HPKE_SUITE suite = {OSSL_HPKE_KEM_ID_X25519, OSSL_HPKE_KDF_ID_HKDF_SHA256, 0};
    if (!strcmp(name, aes_name)) suite.aead_id = OSSL_HPKE_AEAD_ID_AES_GCM_128;
    else if (!strcmp(name, chacha_name)) suite.aead_id = OSSL_HPKE_AEAD_ID_CHACHA_POLY1305;
    else return false;
    *out = suite;
    return true;
}

const char *pure_seal_suite(void) { return has_aes() ? aes_name : chacha_name; }

static pthread_once_t secure_once = PTHREAD_ONCE_INIT;
static void secure_heap(void) { CRYPTO_secure_malloc_init(16384, 32); }

static bool hex_value(char c, unsigned *v) {
    if (c >= '0' && c <= '9') *v = (unsigned)(c - '0');
    else if (c >= 'a' && c <= 'f') *v = (unsigned)(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F') *v = (unsigned)(c - 'A' + 10);
    else return false;
    return true;
}

bool pure_seal_peer(const char *hex, unsigned char out[PURE_SEAL_KEY]) {
    if (strlen(hex) != 2 * PURE_SEAL_KEY) return false;
    for (size_t i = 0; i < PURE_SEAL_KEY; ++i) {
        unsigned high, low;
        if (!hex_value(hex[2 * i], &high) || !hex_value(hex[2 * i + 1], &low)) return false;
        out[i] = (unsigned char)(high << 4 | low);
    }
    return true;
}

PureSealKey *pure_seal_key(const char *path, char public_hex[2 * PURE_SEAL_KEY + 1], char *error, size_t size) {
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

void pure_seal_key_free(PureSealKey *key) {
    if (!key) return;
    EVP_PKEY_free(key->key);
    free(key);
}

bool pure_seal(const unsigned char peer[PURE_SEAL_KEY], const char *aad, const unsigned char *plain, size_t size,
               unsigned char *out) {
    OSSL_HPKE_SUITE suite;
    if (!suite_named(pure_seal_suite(), &suite)) return false;
    OSSL_HPKE_CTX *context = OSSL_HPKE_CTX_new(OSSL_HPKE_MODE_BASE, suite, OSSL_HPKE_ROLE_SENDER, NULL, NULL);
    size_t encapsulated = PURE_SEAL_KEY, sealed = size + 16;
    bool ok = context && OSSL_HPKE_encap(context, out, &encapsulated, peer, PURE_SEAL_KEY, info, sizeof(info) - 1) == 1 &&
              encapsulated == PURE_SEAL_KEY &&
              OSSL_HPKE_seal(context, out + PURE_SEAL_KEY, &sealed, (const unsigned char *)aad, strlen(aad), plain, size) == 1 &&
              sealed == size + 16;
    OSSL_HPKE_CTX_free(context);
    return ok;
}

bool pure_unseal(const PureSealKey *key, const char *suite_name, const char *aad, const unsigned char *sealed, size_t size,
                 unsigned char *plain) {
    OSSL_HPKE_SUITE suite;
    if (!key || size < PURE_SEAL_OVERHEAD || !suite_named(suite_name, &suite)) return false;
    OSSL_HPKE_CTX *context = OSSL_HPKE_CTX_new(OSSL_HPKE_MODE_BASE, suite, OSSL_HPKE_ROLE_RECEIVER, NULL, NULL);
    size_t opened = size - PURE_SEAL_OVERHEAD;
    bool ok = context &&
              OSSL_HPKE_decap(context, sealed, PURE_SEAL_KEY, key->key, info, sizeof(info) - 1) == 1 &&
              OSSL_HPKE_open(context, plain, &opened, (const unsigned char *)aad, strlen(aad), sealed + PURE_SEAL_KEY,
                             size - PURE_SEAL_KEY) == 1 &&
              opened == size - PURE_SEAL_OVERHEAD;
    OSSL_HPKE_CTX_free(context);
    return ok;
}
