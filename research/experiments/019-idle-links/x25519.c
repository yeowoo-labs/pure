/* What an epoch's X25519 costs here (docs/research/099-link-epochs.md): a
 * pair made, a raw private half taken in (OpenSSL works out its public half
 * again), and the secret with a peer's public key.
 *
 *     cc -O2 x25519.c -lcrypto -o x25519 && ./x25519 */
#include <openssl/evp.h>
#include <stdio.h>
#include <time.h>

static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

int main(void) {
    enum { N = 2000 };
    unsigned char private_key[32], peer[32], out[32];
    size_t length = 32;
    EVP_PKEY *other = EVP_PKEY_Q_keygen(NULL, NULL, "X25519"), *mine = NULL;
    EVP_PKEY_get_raw_public_key(other, peer, &length);
    double t = now();
    for (int i = 0; i < N; ++i) {
        EVP_PKEY *k = EVP_PKEY_Q_keygen(NULL, NULL, "X25519");
        length = 32;
        EVP_PKEY_get_raw_private_key(k, private_key, &length);
        EVP_PKEY_free(k);
    }
    printf("{\"pair_us\":%.1f,", (now() - t) / N * 1e6);
    t = now();
    for (int i = 0; i < N; ++i) EVP_PKEY_free(EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, NULL, private_key, 32));
    printf("\"private_in_us\":%.1f,", (now() - t) / N * 1e6);
    mine = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, NULL, private_key, 32);
    EVP_PKEY *theirs = EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, NULL, peer, 32);
    EVP_PKEY_CTX *context = EVP_PKEY_CTX_new(mine, NULL);
    EVP_PKEY_derive_init(context);
    EVP_PKEY_derive_set_peer(context, theirs);
    t = now();
    for (int i = 0; i < N; ++i) {
        length = 32;
        EVP_PKEY_derive(context, out, &length);
    }
    printf("\"secret_us\":%.1f,\"openssl\":\"%s\"}\n", (now() - t) / N * 1e6, OpenSSL_version(OPENSSL_VERSION));
    EVP_PKEY_CTX_free(context);
    EVP_PKEY_free(theirs);
    EVP_PKEY_free(mine);
    EVP_PKEY_free(other);
    return 0;
}
