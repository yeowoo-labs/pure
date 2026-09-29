#ifndef PURE_TLS_H
#define PURE_TLS_H
#include <stdbool.h>
#include <stddef.h>

/* TLS to https receivers (link/crypto.c): one client context, verifying a
 * receiver's certificate against the system's CAs or ca_file, and its name. */
typedef struct PureTls PureTls;
/* The context, made once; false with why in error when it cannot be. */
bool pure_tls_start(const char *ca_file, char *error, size_t size);
/* A session over a connected socket to host (a DNS name, or an IP address
 * written as one), its certificate verified; NULL when it fails. */
PureTls *pure_tls_open(const char *host, int fd);
/* As SSL_read and SSL_write: the bytes moved, or <= 0. */
int pure_tls_read(PureTls *, void *bytes, int size);
int pure_tls_write(PureTls *, const void *bytes, int size);
void pure_tls_close(PureTls *);
#endif
