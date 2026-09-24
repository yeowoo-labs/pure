#define _POSIX_C_SOURCE 200809L
#include "tls-link.h"
#include <openssl/err.h>
#include <openssl/x509v3.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

uint64_t pure_link_now_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) abort();
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}
static bool ready(int fd, short events, uint64_t deadline) {
    for (;;) {
        uint64_t now = pure_link_now_ms();
        if (now >= deadline) return false;
        int remaining = (int)(deadline - now);
        struct pollfd p = {.fd = fd, .events = events};
        int result = poll(&p, 1, remaining);
        if (result > 0) return !(p.revents & (POLLERR | POLLNVAL)) && (p.revents & (events | POLLHUP));
        if (!result) return false;
        if (errno != EINTR) return false;
    }
}
bool pure_link_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}
static bool tls_retry(PureLink *channel, int code, uint64_t deadline) {
    if (code == SSL_ERROR_WANT_READ) return ready(channel->fd, POLLIN, deadline);
    if (code == SSL_ERROR_WANT_WRITE) return ready(channel->fd, POLLOUT, deadline);
    return false;
}
int pure_link_read(PureLink *channel, void *output, size_t size, uint64_t deadline) {
    size_t at = 0;
    while (at < size) {
        if (pure_link_now_ms() >= deadline) return -1;
        size_t received = 0;
        ERR_clear_error();
        int result = SSL_read_ex(channel->ssl, (unsigned char *)output + at, size - at, &received);
        if (result == 1) { if (!received) return -1; at += received; continue; }
        int code = SSL_get_error(channel->ssl, result);
        if (code == SSL_ERROR_ZERO_RETURN && !at) return 0;
        if (!tls_retry(channel, code, deadline)) return -1;
    }
    return 1;
}
bool pure_link_write(PureLink *channel, const void *input, size_t size, uint64_t deadline) {
    size_t at = 0;
    while (at < size) {
        if (pure_link_now_ms() >= deadline) return false;
        size_t sent = 0;
        size_t chunk = size - at;
        if (chunk > 16384) chunk = 16384;
        ERR_clear_error();
        int result = SSL_write_ex(channel->ssl, (const unsigned char *)input + at, chunk, &sent);
        if (result == 1) { if (!sent) return false; at += sent; continue; }
        int code = SSL_get_error(channel->ssl, result);
        if (!tls_retry(channel, code, deadline)) return false;
    }
    return true;
}
int pure_link_read_frame(PureLink *channel, unsigned char *output, size_t capacity,
                         size_t *size, uint64_t deadline) {
    unsigned char prefix[2];
    int first = pure_link_read(channel, prefix, 2, deadline);
    if (first != 1) return first;
    size_t length = ((size_t)prefix[0] << 8) | prefix[1];
    if (!length || length > capacity || pure_link_read(channel, output, length, deadline) != 1) return -1;
    *size = length;
    return 1;
}
bool pure_link_write_frame(PureLink *channel, const unsigned char *data, size_t size,
                           uint64_t deadline) {
    if (!size || size > UINT16_MAX) return false;
    unsigned char prefix[2] = {(unsigned char)(size >> 8), (unsigned char)size};
    return pure_link_write(channel, prefix, 2, deadline) && pure_link_write(channel, data, size, deadline);
}
void pure_link_close(PureLink *channel, bool healthy) {
    if (channel->ssl) {
        if (healthy) (void)SSL_shutdown(channel->ssl);
        SSL_free(channel->ssl);
    }
    if (channel->fd >= 0) close(channel->fd);
    *channel = (PureLink){.fd = -1};
}
static int select_alpn(SSL *ssl, const unsigned char **out, unsigned char *outlen,
                       const unsigned char *in, unsigned int length, void *arg) {
    (void)ssl;
    const unsigned char *alpn = arg;
    for (unsigned int at = 0; at < length;) {
        unsigned int n = in[at++];
        if (!n || n > length - at) return SSL_TLSEXT_ERR_ALERT_FATAL;
        if (n == alpn[0] && !memcmp(in + at, alpn + 1, n)) {
            *out = alpn + 1; *outlen = alpn[0]; return SSL_TLSEXT_ERR_OK;
        }
        at += n;
    }
    return SSL_TLSEXT_ERR_ALERT_FATAL;
}
SSL_CTX *pure_link_context(bool server, const char *ca, const char *cert,
                           const char *key, const unsigned char *alpn) {
    SSL_CTX *ctx = SSL_CTX_new(server ? TLS_server_method() : TLS_client_method());
    if (!ctx) return NULL;
    if (!alpn || !alpn[0] || !SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION) ||
        !SSL_CTX_set_max_proto_version(ctx, TLS1_3_VERSION) ||
        !SSL_CTX_set_max_early_data(ctx, 0) ||
        !SSL_CTX_load_verify_locations(ctx, ca, NULL) ||
        SSL_CTX_use_certificate_chain_file(ctx, cert) != 1 ||
        SSL_CTX_use_PrivateKey_file(ctx, key, SSL_FILETYPE_PEM) != 1 ||
        SSL_CTX_check_private_key(ctx) != 1) {
        SSL_CTX_free(ctx); return NULL;
    }
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER | (server ? SSL_VERIFY_FAIL_IF_NO_PEER_CERT : 0), NULL);
    if (server) SSL_CTX_set_alpn_select_cb(ctx, select_alpn, (void *)alpn);
    return ctx;
}
bool pure_link_handshake(PureLink *channel, SSL_CTX *ctx, bool server,
                         const char *peer_name, const unsigned char *alpn) {
    /* This protocol writes a small reply header followed by a separate TLS
     * body record. Disable Nagle on the connected TCP socket so that the
     * header cannot hold the body behind a delayed acknowledgement. */
    int enabled = 1;
    if (setsockopt(channel->fd, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled))) return false;
    channel->ssl = SSL_new(ctx);
    if (!channel->ssl || SSL_set_fd(channel->ssl, channel->fd) != 1) return false;
    if (server) SSL_set_accept_state(channel->ssl);
    else {
        SSL_set_connect_state(channel->ssl);
        SSL_set_hostflags(channel->ssl, X509_CHECK_FLAG_NO_WILDCARDS | X509_CHECK_FLAG_NEVER_CHECK_SUBJECT);
        if (SSL_set1_host(channel->ssl, peer_name) != 1 ||
            SSL_set_tlsext_host_name(channel->ssl, peer_name) != 1 ||
            SSL_set_alpn_protos(channel->ssl, alpn, alpn[0] + 1u) != 0) return false;
    }
    uint64_t deadline = pure_link_now_ms() + PURE_LINK_DEADLINE_MS;
    for (;;) {
        ERR_clear_error();
        int result = SSL_do_handshake(channel->ssl);
        if (result == 1) break;
        int code = SSL_get_error(channel->ssl, result);
        if (!tls_retry(channel, code, deadline)) return false;
    }
    if (SSL_version(channel->ssl) != TLS1_3_VERSION || SSL_get_verify_result(channel->ssl) != X509_V_OK)
        return false;
    X509 *peer = SSL_get1_peer_certificate(channel->ssl);
    if (!peer) return false;
    bool verified = !server || X509_check_host(peer, peer_name, 0,
                            X509_CHECK_FLAG_NO_WILDCARDS | X509_CHECK_FLAG_NEVER_CHECK_SUBJECT, NULL) == 1;
    X509_free(peer);
    const unsigned char *chosen = NULL;
    unsigned int length = 0;
    SSL_get0_alpn_selected(channel->ssl, &chosen, &length);
    return verified && length == alpn[0] && !memcmp(chosen, alpn + 1, length);
}
bool pure_link_port_number(const char *text, bool allow_zero) {
    if (!text || !*text) return false;
    unsigned long value = 0;
    for (const char *p = text; *p; ++p) {
        if (*p < '0' || *p > '9') return false;
        unsigned digit = (unsigned)(*p - '0');
        if (value > (65535ul - digit) / 10) return false;
        value = value * 10 + digit;
    }
    return allow_zero || value != 0;
}
int pure_link_listen_at(const char *host, const char *port) {
    struct addrinfo hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM,
                             .ai_flags = AI_PASSIVE | AI_NUMERICHOST};
    struct addrinfo *addresses = NULL;
    if (getaddrinfo(host, port, &hints, &addresses)) return -1;
    int listener = -1;
    for (struct addrinfo *it = addresses; it; it = it->ai_next) {
        int fd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (fd < 0) continue;
        int enabled = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
        if (bind(fd, it->ai_addr, it->ai_addrlen) == 0 && listen(fd, 32) == 0 && pure_link_nonblocking(fd)) {
            listener = fd; break;
        }
        close(fd);
    }
    freeaddrinfo(addresses);
    return listener;
}
int pure_link_connect_to(const char *host, const char *port) {
    struct addrinfo hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM};
    struct addrinfo *addresses = NULL;
    if (getaddrinfo(host, port, &hints, &addresses)) return -1;
    int connected = -1;
    uint64_t deadline = pure_link_now_ms() + PURE_LINK_DEADLINE_MS;
    for (struct addrinfo *it = addresses; it; it = it->ai_next) {
        int fd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (fd < 0) continue;
        if (pure_link_nonblocking(fd)) {
            int result = connect(fd, it->ai_addr, it->ai_addrlen);
            if (result == 0 || (result < 0 && errno == EINPROGRESS && ready(fd, POLLOUT, deadline))) {
                int error = 0; socklen_t size = sizeof(error);
                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) == 0 && !error) {
                    connected = fd; break;
                }
            }
        }
        close(fd);
    }
    freeaddrinfo(addresses);
    return connected;
}
