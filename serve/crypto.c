#define _GNU_SOURCE
/* Sealing and TLS for pure-serve (serve/seal.h, serve/tls.h), by way of
 * pure-crypto.so (serve/crypto.h): mapped, and OpenSSL with it, the first
 * time a server needs it. What needs no OpenSSL (a key's hex, the suite
 * this machine seals with) is here. */
#include "crypto.h"
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#if defined(__aarch64__) && defined(__linux__)
#include <asm/hwcap.h>
#include <sys/auxv.h>
#endif

static pthread_once_t once = PTHREAD_ONCE_INIT;
static const PureCrypto *crypto;
static char why[512];

/* pure-crypto.so beside the executable, or where PURE_CRYPTO names it. */
static void load(void) {
    char path[4096];
    const char *named = getenv("PURE_CRYPTO");
    if (named) snprintf(path, sizeof(path), "%s", named);
    else {
        ssize_t length = readlink("/proc/self/exe", path, sizeof(path) - 32);
        char *slash = length > 0 ? memrchr(path, '/', (size_t)length) : NULL;
        if (!slash) { snprintf(why, sizeof(why), "pure-crypto.so: cannot find pure-serve's directory"); return; }
        strcpy(slash + 1, "pure-crypto.so");
    }
    void *library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    const PureCrypto *table = library ? dlsym(library, "pure_crypto") : NULL;
    if (!table || table->version != PURE_CRYPTO_VERSION) {
        snprintf(why, sizeof(why), "%s", !library ? dlerror() : "pure-crypto.so is not this pure-serve's");
        return;
    }
    crypto = table;
}

static const PureCrypto *loaded(char *error, size_t size) {
    pthread_once(&once, load);
    if (!crypto && error) snprintf(error, size, "%s", why);
    return crypto;
}

/* ---- TLS */

bool pure_tls_start(const char *ca_file, char *error, size_t size) {
    static int started; /* 1 made, -1 failed; by the thread that opens wires */
    const PureCrypto *c = loaded(error, size);
    if (!c) return false;
    if (!started) started = c->tls_start(ca_file) ? 1 : -1;
    if (started < 0) snprintf(error, size, "no TLS context: %s cannot be read", ca_file ? ca_file : "the system's CAs");
    return started > 0;
}

/* A session exists only once the table is loaded. */
PureTls *pure_tls_open(const char *host, int fd) { return crypto ? crypto->tls_open(host, fd) : NULL; }
int pure_tls_read(PureTls *tls, void *bytes, int size) { return crypto->tls_read(tls, bytes, size); }
int pure_tls_write(PureTls *tls, const void *bytes, int size) { return crypto->tls_write(tls, bytes, size); }
void pure_tls_close(PureTls *tls) { if (tls) crypto->tls_close(tls); }

/* ---- Sealing */

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

const char *pure_seal_suite(void) { return has_aes() ? aes_name : chacha_name; }

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

bool pure_seal_ready(char *error, size_t size) { return loaded(error, size) != NULL; }

PureSealKey *pure_seal_key(const char *path, char public_hex[2 * PURE_SEAL_KEY + 1], char *error, size_t size) {
    const PureCrypto *c = loaded(error, size);
    return c ? c->seal_key(path, public_hex, error, size) : NULL;
}

/* A key or a session exists only once the table is loaded. */
void pure_seal_key_free(PureSealKey *key) { if (key) crypto->seal_key_free(key); }

PureSealer *pure_sealer(const unsigned char peer[PURE_SEAL_KEY], const PureSealKey *own,
                        unsigned char enc[PURE_SEAL_KEY]) {
    const PureCrypto *c = loaded(NULL, 0);
    return c ? c->sealer(pure_seal_suite(), peer, own, enc) : NULL;
}

bool pure_seal(PureSealer *s, const char *aad, const unsigned char *plain, size_t size, unsigned char *out,
               uint64_t *n) {
    return crypto->seal(s, aad, plain, size, out, n);
}

void pure_sealer_free(PureSealer *s) { if (s) crypto->sealer_free(s); }

PureOpener *pure_opener(const PureSealKey *key, const char *suite, const unsigned char enc[PURE_SEAL_KEY],
                        const unsigned char *from) {
    return key ? crypto->opener(key, suite, enc, from) : NULL;
}

bool pure_unseal(PureOpener *o, uint64_t n, const char *aad, const unsigned char *sealed, size_t size,
                 unsigned char *plain) {
    return crypto->unseal(o, n, aad, sealed, size, plain);
}

void pure_opener_free(PureOpener *o) { if (o) crypto->opener_free(o); }
