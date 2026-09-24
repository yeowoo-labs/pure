#ifndef PURE_TLS_LINK_H
#define PURE_TLS_LINK_H
#include <openssl/ssl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { PURE_LINK_DEADLINE_MS = 5000 };
typedef struct { int fd; SSL *ssl; } PureLink;

uint64_t pure_link_now_ms(void);
bool pure_link_nonblocking(int fd);
/* read returns 1 for all bytes, 0 for clean EOF before any byte, -1 for an
 * incomplete frame, TLS failure or deadline. Never treat -1 as an EOF. */
int pure_link_read(PureLink *, void *output, size_t size, uint64_t deadline);
bool pure_link_write(PureLink *, const void *input, size_t size, uint64_t deadline);
int pure_link_read_frame(PureLink *, unsigned char *output, size_t capacity,
                         size_t *size, uint64_t deadline);
bool pure_link_write_frame(PureLink *, const unsigned char *data, size_t size,
                           uint64_t deadline);
void pure_link_close(PureLink *, bool healthy);

/* alpn is a static length-prefixed protocol name, retained by SSL_CTX's
 * callback until the context is freed. Its content has exactly alpn[0]+1 bytes. */
SSL_CTX *pure_link_context(bool server, const char *ca, const char *cert,
                           const char *key, const unsigned char *alpn);
bool pure_link_handshake(PureLink *, SSL_CTX *, bool server, const char *peer_name,
                         const unsigned char *alpn);
bool pure_link_port_number(const char *text, bool allow_zero);
int pure_link_listen_at(const char *host, const char *port);
int pure_link_connect_to(const char *host, const char *port);
#endif
