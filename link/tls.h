#ifndef PURE_TLS_H
#define PURE_TLS_H
#include <stdbool.h>
#include <stddef.h>

/* TLS to https receivers (link/crypto.c): one client context, verifying a
 * receiver's certificate against the system's CAs or ca_file, and its name. */
typedef struct PureTls PureTls;
/* The context, made once; false with why in error when it cannot be. */
bool pure_tls_start(const char *ca_file, char *error, size_t size);
/* A session over a connected, non-blocking socket to host (a DNS name, or
 * an IP address written as one); NULL when it cannot be. Its handshake is
 * made a step at a time, as the socket is ready, by pure_tls_step. */
PureTls *pure_tls_open(const char *host, int fd);
/* What a step, a read or a write says when it moved no bytes: the session
 * ended (the peer closed it), failed, or waits for the socket to be
 * readable or writable (then the same again). */
enum { PURE_TLS_CLOSED = 0, PURE_TLS_FAILED = -1, PURE_TLS_WANTS_READ = -2, PURE_TLS_WANTS_WRITE = -3 };
/* The handshake: 1 once it is made and the receiver's certificate
 * verified, else what it waits for, or PURE_TLS_FAILED. */
int pure_tls_step(PureTls *);
/* As SSL_read and SSL_write: the bytes moved (a write may move part), or
 * one of the above. */
int pure_tls_read(PureTls *, void *bytes, int size);
int pure_tls_write(PureTls *, const void *bytes, int size);
void pure_tls_close(PureTls *);
#endif
