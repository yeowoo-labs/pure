/* M5 over ngtcp2 (docs/design/011-pure-transport.md): what the quinn
 * harness (../quic) does, in C, with OpenSSL 3.5's QUIC TLS (ngtcp2's ossl
 * backend), so a second QUIC stands beside Pure's links.
 *
 *   drain serve CERT.der KEY.der [PORT]   prints {"port":N}, then serves
 *   drain drain PORT COUNT SIZE           one connection, one stream
 *
 * The client writes "pure", then COUNT messages each after its u32 length
 * (SIZE bytes), then a length of 0, and ends the stream; the service counts
 * the messages (no store) and answers the count (u64). Packets go in bursts
 * with UDP GSO (DRAIN_GSO=0: one by one), as quinn's and Pure's do. Built by
 * build.sh against ngtcp2 1.25.0 fetched into build/third. */
#define _GNU_SOURCE
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/udp.h>
#include <poll.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <ngtcp2/ngtcp2.h>
#include <ngtcp2/ngtcp2_crypto.h>
#include <ngtcp2/ngtcp2_crypto_ossl.h>
#include <openssl/err.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>

#ifndef UDP_SEGMENT
#define UDP_SEGMENT 103
#endif

static const unsigned char ALPN[] = "\x04pure";
enum { BURST = 64 * 1024, CHUNK = 64 * 1024 };

static uint64_t now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * NGTCP2_SECONDS + (uint64_t)t.tv_nsec;
}

static void fail(const char *what) {
    fprintf(stderr, "drain: %s\n", what);
    exit(1);
}

typedef struct {
    ngtcp2_crypto_conn_ref ref;
    ngtcp2_crypto_ossl_ctx *tls;
    SSL *ssl;
    ngtcp2_conn *conn;
    int fd;
    bool gso, server, done;
    struct sockaddr_storage local, remote;
    socklen_t local_size, remote_size;
    ngtcp2_ccerr error;
    /* The client's stream: its bytes are a function of their offset, so
     * what ngtcp2 keeps to send again points into buffers that never change:
     * "pure", then the messages (a pattern of one period), then a length of
     * 0. */
    int64_t stream;
    uint64_t count, size, offset, total;
    unsigned char *pattern;
    size_t period;
    /* The service's count, and where it is in a message. */
    uint64_t messages, need, skip;
    unsigned char length[4];
    unsigned have;
    bool header, answered;
    unsigned char answer[8];
    uint64_t read;
} Side;

static void log_line(void *user, const char *format, ...) {
    (void)user;
    va_list list;
    va_start(list, format);
    vfprintf(stderr, format, list);
    va_end(list);
    fputc('\n', stderr);
}

static ngtcp2_conn *conn_of(ngtcp2_crypto_conn_ref *ref) { return ((Side *)ref->user_data)->conn; }

static void random_bytes(uint8_t *out, size_t n, const ngtcp2_rand_ctx *ctx) {
    (void)ctx;
    if (RAND_bytes(out, (int)n) != 1) abort();
}

static int new_cid(ngtcp2_conn *conn, ngtcp2_cid *cid, ngtcp2_stateless_reset_token *token, size_t n, void *user) {
    (void)conn, (void)user;
    if (RAND_bytes(cid->data, (int)n) != 1 || RAND_bytes(token->data, sizeof(token->data)) != 1)
        return NGTCP2_ERR_CALLBACK_FAILURE;
    cid->datalen = n;
    return 0;
}

/* The client: its stream opened once the service allows one. */
static int streams_allowed(ngtcp2_conn *conn, uint64_t most, void *user) {
    (void)most;
    Side *c = user;
    if (c->stream < 0 && ngtcp2_conn_open_bidi_stream(conn, &c->stream, NULL)) c->stream = -1;
    return 0;
}

/* The service: messages counted as they come, over any cut; at the length of
 * 0 the count goes back and the stream ends. */
static int stream_data(ngtcp2_conn *conn, uint32_t flags, int64_t stream, uint64_t offset, const uint8_t *data, size_t n,
                       void *user, void *stream_user) {
    (void)offset, (void)stream_user;
    Side *s = user;
    if (!s->server) { /* the client: the count said back */
        for (size_t i = 0; i < n && s->read < 8; ++i) s->answer[s->read++] = data[i];
        if ((flags & NGTCP2_STREAM_DATA_FLAG_FIN) || s->read >= 8) s->done = true;
    } else
        for (size_t i = 0; i < n;) {
            if (!s->header) { /* "pure" */
                if (++s->have == 4) s->header = true, s->have = 0;
                ++i;
            } else if (s->skip) {
                size_t take = n - i < s->skip ? n - i : (size_t)s->skip;
                s->skip -= take;
                i += take;
            } else {
                s->length[s->have++] = data[i++];
                if (s->have < 4) continue;
                s->have = 0;
                uint32_t length = (uint32_t)s->length[0] | (uint32_t)s->length[1] << 8 | (uint32_t)s->length[2] << 16 |
                                  (uint32_t)s->length[3] << 24;
                if (!length) {
                    s->answered = true;
                    s->stream = stream;
                    for (int b = 0; b < 8; ++b) s->answer[b] = (unsigned char)(s->messages >> 8 * b);
                    break;
                }
                ++s->messages;
                s->skip = length;
            }
        }
    ngtcp2_conn_extend_max_stream_offset(conn, stream, n);
    ngtcp2_conn_extend_max_offset(conn, n);
    return 0;
}

static ngtcp2_callbacks callbacks(bool server) {
    ngtcp2_callbacks c = {
        .recv_crypto_data = ngtcp2_crypto_recv_crypto_data_cb,
        .encrypt = ngtcp2_crypto_encrypt_cb,
        .decrypt = ngtcp2_crypto_decrypt_cb,
        .hp_mask = ngtcp2_crypto_hp_mask_cb,
        .recv_stream_data = stream_data,
        .rand = random_bytes,
        .update_key = ngtcp2_crypto_update_key_cb,
        .delete_crypto_aead_ctx = ngtcp2_crypto_delete_crypto_aead_ctx_cb,
        .delete_crypto_cipher_ctx = ngtcp2_crypto_delete_crypto_cipher_ctx_cb,
        .version_negotiation = ngtcp2_crypto_version_negotiation_cb,
        .get_new_connection_id2 = new_cid,
        .get_path_challenge_data2 = ngtcp2_crypto_get_path_challenge_data2_cb,
    };
    if (server) c.recv_client_initial = ngtcp2_crypto_recv_client_initial_cb;
    else {
        c.client_initial = ngtcp2_crypto_client_initial_cb;
        c.recv_retry = ngtcp2_crypto_recv_retry_cb;
        c.extend_max_local_streams_bidi = streams_allowed;
    }
    return c;
}

/* Windows wide enough that flow control never holds the drain back. */
static void windows(ngtcp2_transport_params *p) {
    ngtcp2_transport_params_default(p);
    p->initial_max_streams_bidi = 4;
    p->initial_max_stream_data_bidi_local = 16 << 20;
    p->initial_max_stream_data_bidi_remote = 16 << 20;
    p->initial_max_data = 64 << 20;
    p->max_idle_timeout = 60 * NGTCP2_SECONDS;
}

/* The client's next bytes at its offset: a pointer into what never changes. */
static size_t next_bytes(Side *c, ngtcp2_vec *v) {
    static const unsigned char head[4] = {'p', 'u', 'r', 'e'}, tail[4] = {0};
    uint64_t messages_end = 4 + c->count * c->period;
    if (c->offset >= c->total) return 0;
    if (c->offset < 4) v->base = (uint8_t *)head + c->offset, v->len = 4 - c->offset;
    else if (c->offset < messages_end) {
        uint64_t phase = (c->offset - 4) % c->period, left = messages_end - c->offset;
        v->base = c->pattern + phase;
        v->len = left < CHUNK ? left : CHUNK;
    } else
        v->base = (uint8_t *)tail + (c->offset - messages_end), v->len = c->total - c->offset;
    return 1;
}

static ngtcp2_ssize write_one(ngtcp2_conn *conn, ngtcp2_path *path, ngtcp2_pkt_info *pi, uint8_t *dest, size_t size,
                              ngtcp2_tstamp ts, void *user) {
    Side *s = user;
    for (;;) {
        ngtcp2_vec v;
        size_t count = 0;
        int64_t stream = -1;
        uint32_t flags = NGTCP2_WRITE_STREAM_FLAG_MORE | NGTCP2_WRITE_STREAM_FLAG_PADDING;
        if (!s->server && s->stream >= 0 && (count = next_bytes(s, &v))) {
            stream = s->stream;
            if (s->offset + v.len >= s->total) flags |= NGTCP2_WRITE_STREAM_FLAG_FIN;
        } else if (s->server && s->answered && s->read < 8) { /* the count, then the end */
            stream = s->stream, v.base = s->answer + s->read, v.len = 8 - s->read, count = 1;
            flags |= NGTCP2_WRITE_STREAM_FLAG_FIN;
        }
        ngtcp2_ssize sent = 0;
        ngtcp2_ssize n = ngtcp2_conn_writev_stream(conn, path, pi, dest, size, &sent, flags, stream, &v, count, ts);
        if (sent > 0) {
            if (s->server) s->read += (uint64_t)sent;
            else s->offset += (uint64_t)sent;
        }
        if (n == NGTCP2_ERR_WRITE_MORE) continue;
        if (n == NGTCP2_ERR_STREAM_DATA_BLOCKED || n == NGTCP2_ERR_STREAM_SHUT_WR) {
            n = ngtcp2_conn_writev_stream(conn, path, pi, dest, size, NULL, NGTCP2_WRITE_STREAM_FLAG_PADDING, -1, NULL,
                                          0, ts);
        }
        return n < 0 ? NGTCP2_ERR_CALLBACK_FAILURE : n;
    }
}

static void send_burst(Side *s, const uint8_t *bytes, size_t n, size_t segment) {
    _Alignas(8) unsigned char control[CMSG_SPACE(sizeof(uint16_t))] = {0};
    struct iovec part = {(void *)bytes, n};
    struct msghdr m = {.msg_name = &s->remote, .msg_namelen = s->remote_size, .msg_iov = &part, .msg_iovlen = 1};
    if (s->gso && n > segment) {
        m.msg_control = control, m.msg_controllen = sizeof(control);
        struct cmsghdr *c = CMSG_FIRSTHDR(&m);
        c->cmsg_level = SOL_UDP, c->cmsg_type = UDP_SEGMENT, c->cmsg_len = CMSG_LEN(sizeof(uint16_t));
        uint16_t size = (uint16_t)segment;
        memcpy(CMSG_DATA(c), &size, sizeof(size));
        if (sendmsg(s->fd, &m, 0) >= 0) return;
        s->gso = false; /* no GSO here: one by one */
        m.msg_control = NULL, m.msg_controllen = 0;
    }
    for (size_t at = 0; at < n; at += segment) {
        part.iov_base = (void *)(bytes + at);
        part.iov_len = n - at < segment ? n - at : segment;
        while (sendmsg(s->fd, &m, 0) < 0 && errno == EINTR) {}
    }
}

/* What may go now, in bursts. */
static void write_out(Side *s) {
    static uint8_t burst[BURST];
    for (int round = 0; round < 64; ++round) {
        ngtcp2_path_storage ps;
        ngtcp2_path_storage_zero(&ps);
        ngtcp2_pkt_info pi;
        size_t segment = 0;
        ngtcp2_tstamp ts = now_ns();
        ngtcp2_ssize n = ngtcp2_conn_write_aggregate_pkt(s->conn, &ps.path, &pi, burst, sizeof(burst), &segment,
                                                         write_one, ts);
        if (n < 0) fail(ngtcp2_strerror((int)n));
        if (!n) return;
        send_burst(s, burst, (size_t)n, segment);
    }
}

static void read_in(Side *s) {
    uint8_t bytes[65536];
    for (;;) {
        struct sockaddr_storage from;
        struct iovec part = {bytes, sizeof(bytes)};
        struct msghdr m = {.msg_name = &from, .msg_namelen = sizeof(from), .msg_iov = &part, .msg_iovlen = 1};
        ssize_t n = recvmsg(s->fd, &m, MSG_DONTWAIT);
        if (n < 0) return;
        if (!s->conn) { /* the service, a client's first packet */
            ngtcp2_pkt_hd hd;
            int accepted = ngtcp2_accept(&hd, bytes, (size_t)n);
            if (getenv("DRAIN_LOG")) fprintf(stderr, "first packet %zd bytes: %s\n", n, accepted ? ngtcp2_strerror(accepted) : "accepted");
            if (accepted) continue;
            memcpy(&s->remote, &from, m.msg_namelen), s->remote_size = m.msg_namelen;
            ngtcp2_path path = {.local = {(struct sockaddr *)&s->local, s->local_size},
                                .remote = {(struct sockaddr *)&s->remote, s->remote_size}};
            ngtcp2_cid scid = {.datalen = 8};
            RAND_bytes(scid.data, 8);
            ngtcp2_settings settings;
            ngtcp2_settings_default(&settings);
            settings.initial_ts = now_ns();
            if (getenv("DRAIN_LOG")) settings.log_printf = log_line;
            ngtcp2_transport_params params;
            windows(&params);
            params.original_dcid = hd.dcid, params.original_dcid_present = 1;
            ngtcp2_callbacks cb = callbacks(true);
            if (ngtcp2_conn_server_new(&s->conn, &hd.scid, &scid, &path, hd.version, &cb, &settings, &params, NULL, s))
                fail("a connection");
            ngtcp2_conn_set_tls_native_handle(s->conn, s->tls);
        }
        ngtcp2_path path = {.local = {(struct sockaddr *)&s->local, s->local_size},
                            .remote = {(struct sockaddr *)&from, m.msg_namelen}};
        ngtcp2_pkt_info pi = {0};
        int rv = ngtcp2_conn_read_pkt(s->conn, &path, &pi, bytes, (size_t)n, now_ns());
        if (rv == NGTCP2_ERR_DRAINING || rv == NGTCP2_ERR_CLOSING) s->done = true;
        else if (rv) fail(ngtcp2_strerror(rv));
    }
}

static void run(Side *s) {
    while (!s->done) {
        if (s->conn) write_out(s);
        ngtcp2_tstamp expiry = s->conn ? ngtcp2_conn_get_expiry2(s->conn) : UINT64_MAX, now = now_ns();
        int timeout = expiry == UINT64_MAX ? 1000 : expiry <= now ? 0 : (int)((expiry - now) / NGTCP2_MILLISECONDS) + 1;
        struct pollfd wait = {s->fd, POLLIN, 0};
        if (poll(&wait, 1, timeout) < 0 && errno != EINTR) fail("poll");
        if (wait.revents & POLLIN) read_in(s);
        if (s->conn && ngtcp2_conn_get_expiry2(s->conn) <= now_ns()) {
            int rv = ngtcp2_conn_handle_expiry(s->conn, now_ns());
            if (rv == NGTCP2_ERR_IDLE_CLOSE) s->done = true;
            else if (rv) fail(ngtcp2_strerror(rv));
        }
    }
}

static int alpn(SSL *ssl, const unsigned char **out, unsigned char *out_size, const unsigned char *in, unsigned in_size,
                void *arg) {
    (void)ssl, (void)arg;
    for (unsigned at = 0; at + 1 + ALPN[0] <= in_size; at += 1u + in[at])
        if (!memcmp(in + at, ALPN, 1u + ALPN[0])) {
            *out = in + at + 1, *out_size = in[at];
            return SSL_TLSEXT_ERR_OK;
        }
    return SSL_TLSEXT_ERR_ALERT_FATAL;
}

static unsigned char *read_file(const char *path, long *size) {
    FILE *f = fopen(path, "rb");
    if (!f || fseek(f, 0, SEEK_END) || (*size = ftell(f)) <= 0 || fseek(f, 0, SEEK_SET)) fail(path);
    unsigned char *bytes = malloc((size_t)*size);
    if (!bytes || fread(bytes, 1, (size_t)*size, f) != (size_t)*size) fail(path);
    fclose(f);
    return bytes;
}

static void tls(Side *s, SSL_CTX *ctx) {
    if (!(s->ssl = SSL_new(ctx)) || ngtcp2_crypto_ossl_ctx_new(&s->tls, s->ssl)) fail("TLS");
    if (s->server ? ngtcp2_crypto_ossl_configure_server_session(s->ssl)
                  : ngtcp2_crypto_ossl_configure_client_session(s->ssl))
        fail("TLS session");
    s->ref = (ngtcp2_crypto_conn_ref){conn_of, s};
    SSL_set_app_data(s->ssl, &s->ref);
    if (s->server) SSL_set_accept_state(s->ssl);
    else {
        SSL_set_connect_state(s->ssl);
        SSL_set_alpn_protos(s->ssl, ALPN, sizeof(ALPN) - 1);
        SSL_set_tlsext_host_name(s->ssl, "localhost");
    }
}

static _Noreturn void serve(const char *cert, const char *key, unsigned port) {
    Side s = {.server = true, .stream = -1, .gso = !getenv("DRAIN_GSO") || strcmp(getenv("DRAIN_GSO"), "0")};
    long cert_size, key_size;
    unsigned char *cert_der = read_file(cert, &cert_size), *key_der = read_file(key, &key_size);
    const unsigned char *k = key_der;
    EVP_PKEY *pkey = d2i_AutoPrivateKey(NULL, &k, key_size);
    SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
    if (!ctx || !pkey || SSL_CTX_use_certificate_ASN1(ctx, (int)cert_size, cert_der) != 1 ||
        SSL_CTX_use_PrivateKey(ctx, pkey) != 1)
        fail("the certificate");
    SSL_CTX_set_alpn_select_cb(ctx, alpn, NULL);
    struct sockaddr_in at = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port), .sin_addr = {htonl(INADDR_LOOPBACK)}};
    s.fd = socket(AF_INET, SOCK_DGRAM, 0);
    int buffer = 4 << 20;
    setsockopt(s.fd, SOL_SOCKET, SO_RCVBUF, &buffer, sizeof(buffer));
    setsockopt(s.fd, SOL_SOCKET, SO_SNDBUF, &buffer, sizeof(buffer));
    s.local_size = sizeof(struct sockaddr_in);
    if (bind(s.fd, (struct sockaddr *)&at, sizeof(at)) || getsockname(s.fd, (struct sockaddr *)&s.local, &s.local_size))
        fail("a port");
    printf("{\"port\":%u}\n", ntohs(((struct sockaddr_in *)&s.local)->sin_port));
    fflush(stdout);
    for (;;) { /* a connection at a time, as long as it is let run */
        tls(&s, ctx);
        run(&s);
        ngtcp2_conn_del(s.conn);
        ngtcp2_crypto_ossl_ctx_del(s.tls);
        SSL_free(s.ssl);
        Side next = {.server = true, .stream = -1, .gso = s.gso, .fd = s.fd, .local = s.local, .local_size = s.local_size};
        s = next;
    }
}

static int drain(unsigned port, uint64_t count, uint64_t size) {
    Side c = {.stream = -1, .count = count, .size = size, .gso = !getenv("DRAIN_GSO") || strcmp(getenv("DRAIN_GSO"), "0")};
    c.period = 4 + size;
    c.total = 4 + count * c.period + 4;
    size_t periods = CHUNK / c.period + 2;
    if (!(c.pattern = malloc(periods * c.period))) fail("memory");
    for (size_t p = 0; p < periods; ++p) {
        unsigned char *m = c.pattern + p * c.period;
        for (int b = 0; b < 4; ++b) m[b] = (unsigned char)(size >> 8 * b);
        memset(m + 4, 'x', size);
    }
    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) fail("TLS");
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL); /* as quinn's client: no certificate checked */
    tls(&c, ctx);
    struct sockaddr_in to = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port), .sin_addr = {htonl(INADDR_LOOPBACK)}};
    c.fd = socket(AF_INET, SOCK_DGRAM, 0);
    int buffer = 4 << 20;
    setsockopt(c.fd, SOL_SOCKET, SO_RCVBUF, &buffer, sizeof(buffer));
    setsockopt(c.fd, SOL_SOCKET, SO_SNDBUF, &buffer, sizeof(buffer));
    memcpy(&c.remote, &to, sizeof(to)), c.remote_size = sizeof(to);
    c.local_size = sizeof(c.local);
    if (connect(c.fd, (struct sockaddr *)&to, sizeof(to)) || getsockname(c.fd, (struct sockaddr *)&c.local, &c.local_size))
        fail("a socket");
    ngtcp2_path path = {.local = {(struct sockaddr *)&c.local, c.local_size},
                        .remote = {(struct sockaddr *)&c.remote, c.remote_size}};
    ngtcp2_cid dcid = {.datalen = NGTCP2_MIN_INITIAL_DCIDLEN}, scid = {.datalen = 8};
    RAND_bytes(dcid.data, (int)dcid.datalen), RAND_bytes(scid.data, (int)scid.datalen);
    ngtcp2_settings settings;
    ngtcp2_settings_default(&settings);
    settings.initial_ts = now_ns();
    if (getenv("DRAIN_LOG")) settings.log_printf = log_line;
    ngtcp2_transport_params params;
    windows(&params);
    ngtcp2_callbacks cb = callbacks(false);
    if (ngtcp2_conn_client_new(&c.conn, &dcid, &scid, &path, NGTCP2_PROTO_VER_V1, &cb, &settings, &params, NULL, &c))
        fail("a connection");
    ngtcp2_conn_set_tls_native_handle(c.conn, c.tls);
    uint64_t began = now_ns();
    run(&c);
    double seconds = (double)(now_ns() - began) / 1e9;
    uint64_t read = 0;
    for (int b = 7; b >= 0; --b) read = read << 8 | c.answer[b];
    printf("{\"messages\":%llu,\"size\":%llu,\"read\":%llu,\"seconds\":%.3f,\"per_second\":%.0f}\n",
           (unsigned long long)count, (unsigned long long)size, (unsigned long long)read, seconds, (double)count / seconds);
    ngtcp2_path_storage ps;
    ngtcp2_path_storage_zero(&ps);
    ngtcp2_pkt_info pi;
    uint8_t close[1280];
    ngtcp2_ccerr_default(&c.error);
    ngtcp2_ssize n = ngtcp2_conn_write_connection_close(c.conn, &ps.path, &pi, close, sizeof(close), &c.error, now_ns());
    if (n > 0) send(c.fd, close, (size_t)n, 0);
    return read == count ? 0 : 1;
}

int main(int argc, char **argv) {
    if (ngtcp2_crypto_ossl_init()) fail("ngtcp2's OpenSSL");
    if (argc >= 4 && argc <= 5 && !strcmp(argv[1], "serve"))
        serve(argv[2], argv[3], argc == 5 ? (unsigned)strtoul(argv[4], NULL, 10) : 0);
    if (argc == 5 && !strcmp(argv[1], "drain"))
        return drain((unsigned)strtoul(argv[2], NULL, 10), strtoull(argv[3], NULL, 10), strtoull(argv[4], NULL, 10));
    fputs("drain serve CERT.der KEY.der [PORT] | drain drain PORT COUNT SIZE\n", stderr);
    return 2;
}
