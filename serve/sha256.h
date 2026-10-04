#ifndef PURE_SHA256_H
#define PURE_SHA256_H
#include <stddef.h>

/* SHA-256 (FIPS 180-4) of size bytes: an app's tokens are kept as these
 * (docs/design/017's 3b), with no OpenSSL loaded to check them. */
void pure_sha256(const void *data, size_t size, unsigned char out[32]);
#endif
