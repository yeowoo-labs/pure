#define _POSIX_C_SOURCE 200809L
/* Selected document peer: same prepared program, store and execution core as
 * pure-edit. TLS and the wire protocol remain outside libpure-execution.a. */
#include "runtime.h"
#include "store-port.h"
#include "store.h"
#include "../common/tls-link.h"
#include <openssl/err.h>
#include <openssl/sha.h>
#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

enum { REQUEST_BYTES = 14, REPLY_BYTES = 13, HELLO_BYTES = 40,
       OP_READ = 1, OP_WRITE = 2, WANT_BODY = 0x80 };
static const unsigned char alpn[] = {10, 'p','u','r','e','-','d','o','c','/','1'};
static volatile sig_atomic_t stopping;
static void signal_stop(int signum) { (void)signum; stopping = 1; }

static uint32_t get32(const unsigned char *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static uint64_t get64(const unsigned char *p) {
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) value = (value << 8) | p[i];
    return value;
}
static void put32(unsigned char *p, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) p[i] = (unsigned char)(value >> (24 - 8 * i));
}
static void put64(unsigned char *p, uint64_t value) {
    for (unsigned i = 0; i < 8; ++i) p[i] = (unsigned char)(value >> (56 - 8 * i));
}
#ifndef PURE_PEER_SERVER_ONLY
static void put32le(unsigned char *p, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) p[i] = (unsigned char)(value >> (8 * i));
}
#endif
static bool number(const char *text, uint64_t limit, uint64_t *out) {
    if (!text || !*text) return false;
    uint64_t value = 0;
    for (; *text; ++text) {
        if (*text < '0' || *text > '9') return false;
        unsigned digit = (unsigned)(*text - '0');
        if (digit > limit || value > (limit - digit) / 10) return false;
        value = value * 10 + digit;
    }
    *out = value;
    return true;
}
static bool load_program(const char *path, PureProgram *program, unsigned char digest[32]) {
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    char source[PURE_PROGRAM_LIMIT + 1];
    size_t length = fread(source, 1, sizeof(source), file);
    bool bad = ferror(file) != 0;
    if (fclose(file)) bad = true;
    char error[160];
    if (bad || !pure_program_parse(source, length, program, error, sizeof(error))) {
        if (!bad) fprintf(stderr, "Program: %s\n", error);
        return false;
    }
    return SHA256((const unsigned char *)source, length, digest) != NULL;
}
#ifndef PURE_PEER_SERVER_ONLY
static bool edit_layout(const PureProgram *p, size_t *prefix) {
    if (!p->splice || !p->write.to_end || p->write_step || p->input_steps ||
        !p->splice_offset.from_u32le || !p->splice_remove.from_u32le) return false;
    size_t start = p->splice_offset.value, remove = p->splice_remove.value;
    *prefix = p->write.offset;
    /* The convenience command is deliberately the eight-byte editor profile.
     * Other authored layouts use apply, with no layout-specific wire fields. */
    return *prefix == 8 && p->input_required <= 8 &&
           ((start == 0 && remove == 4) || (start == 4 && remove == 0));
}
#endif
static bool write_hello(PureLink *link, const unsigned char digest[32], uint64_t deadline) {
    unsigned char hello[HELLO_BYTES] = {'P','D','O','C',1,0,0,0};
    memcpy(hello + 8, digest, 32);
    return pure_link_write(link, hello, sizeof(hello), deadline);
}
#ifndef PURE_PEER_SERVER_ONLY
static bool read_hello(PureLink *link, const unsigned char digest[32], uint64_t deadline) {
    unsigned char hello[HELLO_BYTES];
    const unsigned char header[] = {'P','D','O','C',1,0,0,0};
    return pure_link_read(link, hello, sizeof(hello), deadline) == 1 &&
           !memcmp(hello, header, sizeof(header)) && !memcmp(hello + 8, digest, 32);
}
#endif
/* The header always reports the logical output length. The requester chooses
 * whether that borrowed output must cross this boundary at all. */
static bool send_result(PureLink *link, PureOutcome outcome, bool want_body, uint64_t deadline) {
    if (outcome.status > PURE_ALLOCATION || outcome.revision < 0 || outcome.body.size > PURE_OBJECT_LIMIT ||
        (outcome.body.size && !outcome.body.data)) return false;
    unsigned char header[REPLY_BYTES];
    header[0] = (unsigned char)outcome.status;
    put64(header + 1, (uint64_t)outcome.revision);
    put32(header + 9, (uint32_t)outcome.body.size);
    return pure_link_write(link, header, sizeof(header), deadline) &&
           (!want_body || pure_link_write(link, outcome.body.data, outcome.body.size, deadline));
}
static bool serve_request(PureLink *link, PureRuntime *runtime, int *end) {
    uint64_t deadline = pure_link_now_ms() + PURE_LINK_DEADLINE_MS;
    unsigned char header[REQUEST_BYTES];
    int got = pure_link_read(link, header, sizeof(header), deadline);
    if (!got) { *end = 1; return true; }
    if (got != 1) return false;
    unsigned op = header[0] & ~WANT_BODY, key_size = header[1];
    bool want_body = (header[0] & WANT_BODY) != 0;
    uint64_t expected = get64(header + 2);
    uint32_t body_size = get32(header + 10);
    if ((op != OP_READ && op != OP_WRITE) || !key_size || key_size > 64 ||
        expected > INT64_MAX || body_size > PURE_OBJECT_LIMIT ||
        (op == OP_READ && (expected || body_size))) return false;
    char key[65];
    if (pure_link_read(link, key, key_size, deadline) != 1) return false;
    if (memchr(key, 0, key_size)) return false;
    key[key_size] = 0;
    PureTicket ticket;
    PureResult accepted = pure_runtime_accept(runtime, op == OP_READ ? PURE_INPUT_READ : PURE_INPUT_WRITE,
                                              key, (int64_t)expected, body_size, &ticket);
    if (accepted != PURE_OK) {
        /* The body is intentionally not consumed after failed admission. */
        PureOutcome error = {.status = accepted};
        (void)send_result(link, error, want_body, deadline);
        return false;
    }
    if (op == OP_WRITE) {
        size_t size = 0;
        unsigned char *ingress = pure_runtime_ingress(ticket, &size);
        if (size != body_size || (size && (!ingress || pure_link_read(link, ingress, size, deadline) != 1)) ||
            !pure_runtime_ready(ticket)) {
            (void)pure_runtime_release(ticket);
            return false;
        }
    }
    PureOutcome outcome;
    while (!pure_runtime_result(ticket, &outcome)) {
        if (!pure_runtime_step(runtime)) { (void)pure_runtime_release(ticket); return false; }
    }
    bool sent = send_result(link, outcome, want_body, deadline);
    (void)pure_runtime_release(ticket);
    return sent;
}
static bool serve_connection(PureLink *link, PureRuntime *runtime, const unsigned char digest[32]) {
    if (!write_hello(link, digest, pure_link_now_ms() + PURE_LINK_DEADLINE_MS)) return false;
    int end = 0;
    while (!end) if (!serve_request(link, runtime, &end)) return false;
    return true;
}
static int serve(const char *bind_address, const char *port, const char *program_path,
                 const char *store_path, const char *ca, const char *cert,
                 const char *key, const char *client_name, unsigned max_connections) {
    if (!pure_link_port_number(port, true) || !client_name || !*client_name) return 2;
    PureProgram program;
    unsigned char digest[32];
    if (!load_program(program_path, &program, digest) || pure_store_init(store_path) != PURE_OK) return 2;
    SSL_CTX *ctx = pure_link_context(true, ca, cert, key, alpn);
    if (!ctx) { ERR_print_errors_fp(stderr); return 2; }
    PureRuntime runtime;
    PureWork work[1];
    if (!pure_runtime_init(&runtime, work, 1, &program, pure_store_host(store_path),
                           4u * 1024u * 1024u)) { SSL_CTX_free(ctx); return 2; }
    int listener = pure_link_listen_at(bind_address, port);
    if (listener < 0) { perror("listen"); SSL_CTX_free(ctx); return 2; }
    struct sockaddr_storage address;
    socklen_t address_size = sizeof(address);
    if (getsockname(listener, (struct sockaddr *)&address, &address_size)) abort();
    unsigned actual_port = address.ss_family == AF_INET
        ? ntohs(((struct sockaddr_in *)&address)->sin_port)
        : ntohs(((struct sockaddr_in6 *)&address)->sin6_port);
    printf("READY %u\n", actual_port); fflush(stdout);
    signal(SIGTERM, signal_stop); signal(SIGINT, signal_stop);
    unsigned accepted = 0;
    while (!stopping && (!max_connections || accepted < max_connections)) {
        struct pollfd p = {.fd = listener, .events = POLLIN};
        int result = poll(&p, 1, 1000);
        if (result < 0) { if (errno == EINTR) continue; break; }
        if (!result) continue;
        int fd = accept(listener, NULL, NULL);
        if (fd < 0) { if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue; break; }
        ++accepted;
        PureLink link = {.fd = fd};
        bool healthy = pure_link_nonblocking(fd) &&
            pure_link_handshake(&link, ctx, true, client_name, alpn);
        if (healthy) healthy = serve_connection(&link, &runtime, digest);
        pure_link_close(&link, healthy);
    }
    close(listener);
    pure_runtime_stop(&runtime);
    int status = runtime.active || runtime.retained ? 1 : 0;
    SSL_CTX_free(ctx);
    return status;
}
#ifndef PURE_PEER_SERVER_ONLY
static int receive_result(PureLink *link, bool show, uint64_t deadline) {
    unsigned char header[REPLY_BYTES];
    if (pure_link_read(link, header, sizeof(header), deadline) != 1 ||
        header[0] > PURE_ALLOCATION || get64(header + 1) > INT64_MAX ||
        get32(header + 9) > PURE_OBJECT_LIMIT) return -1;
    PureResult result = (PureResult)header[0];
    int64_t revision = (int64_t)get64(header + 1);
    uint32_t length = get32(header + 9);
    if (result != PURE_OK && length) return -1;
    unsigned char buffer[4096];
    size_t left = show ? length : 0;
    int failed = 0;
    while (left) {
        size_t count = left < sizeof(buffer) ? left : sizeof(buffer);
        if (pure_link_read(link, buffer, count, deadline) != 1) return -1;
        if (show && result == PURE_OK && fwrite(buffer, 1, count, stdout) != count) failed = 1;
        left -= count;
    }
    if (result != PURE_OK) {
        fprintf(stderr, "{\"status\":%u,\"revision\":\"%" PRId64 "\"}\n", result, revision);
        return result == PURE_CONFLICT ? 3 : 2;
    }
    if (show) fprintf(stderr, "{\"revision\":\"%" PRId64 "\",\"bytes\":%u}\n", revision, length);
    else if (printf("{\"revision\":\"%" PRId64 "\",\"reply_bytes\":%u}\n", revision, length) < 0) failed = 1;
    if (fflush(stdout)) failed = 1;
    return failed ? 2 : 0;
}
static int call(const char *host, const char *port, const char *server_name,
                const char *program_path, const char *ca, const char *cert,
                const char *key, int argc, char **argv) {
    if (!pure_link_port_number(port, false) || !server_name || !*server_name || argc < 2) return 2;
    bool show = !strcmp(argv[0], "show"), edit = !strcmp(argv[0], "edit");
    if ((!show && !edit && strcmp(argv[0], "apply")) || argc != (show ? 2 : edit ? 6 : 4)) return 2;
    PureProgram program;
    unsigned char digest[32];
    if (!load_program(program_path, &program, digest)) return 2;
    size_t prefix = 0;
    if (edit && !edit_layout(&program, &prefix)) return 2;
    uint64_t expected = 0, offset = 0, remove = 0;
    if (!show && !number(argv[2], INT64_MAX, &expected)) return 2;
    if (edit && (!number(argv[3], UINT32_MAX, &offset) ||
                 !number(argv[4], UINT32_MAX, &remove))) return 2;
    const char *document = argv[1];
    size_t key_size = strlen(document);
    if (!key_size || key_size > 64) return 2;
    FILE *file = NULL;
    size_t body_size = 0;
    if (!show) {
        file = fopen(argv[edit ? 5 : 3], "rb");
        if (!file) { perror("input"); return 2; }
        long bytes;
        if (fseek(file, 0, SEEK_END) || (bytes = ftell(file)) < 0 ||
            (uintmax_t)bytes > PURE_OBJECT_LIMIT - prefix || fseek(file, 0, SEEK_SET)) {
            fclose(file); return 2;
        }
        body_size = prefix + (size_t)bytes;
    }
    SSL_CTX *ctx = pure_link_context(false, ca, cert, key, alpn);
    if (!ctx) { if (file) fclose(file); ERR_print_errors_fp(stderr); return 2; }
    PureLink link = {.fd = pure_link_connect_to(host, port)};
    bool ready = link.fd >= 0 && pure_link_handshake(&link, ctx, false, server_name, alpn);
    bool matched = ready && read_hello(&link, digest, pure_link_now_ms() + PURE_LINK_DEADLINE_MS);
    if (!matched) {
        fputs(ready ? "peer application definition mismatch\n" :
              "peer authentication or connection failed\n", stderr);
        pure_link_close(&link, false);
        SSL_CTX_free(ctx);
        if (file) fclose(file);
        return 2;
    }
    int status = 2;
    {
        uint64_t deadline = pure_link_now_ms() + PURE_LINK_DEADLINE_MS;
        unsigned char header[REQUEST_BYTES] = {0};
        header[0] = (show ? OP_READ : OP_WRITE) | (show ? WANT_BODY : 0);
        header[1] = (unsigned char)key_size;
        put64(header + 2, expected);
        put32(header + 10, (uint32_t)body_size);
        ready = pure_link_write(&link, header, sizeof(header), deadline) &&
                pure_link_write(&link, document, key_size, deadline);
        if (ready && prefix) {
            unsigned char fields[8] = {0};
            put32le(fields + program.splice_offset.value, (uint32_t)offset);
            put32le(fields + program.splice_remove.value, (uint32_t)remove);
            ready = pure_link_write(&link, fields, prefix, deadline);
        }
        unsigned char buffer[4096];
        size_t left = body_size - prefix;
        while (ready && left) {
            size_t count = left < sizeof(buffer) ? left : sizeof(buffer);
            if (fread(buffer, 1, count, file) != count) { ready = false; break; }
            ready = pure_link_write(&link, buffer, count, deadline);
            left -= count;
        }
        if (file && (fgetc(file) != EOF || ferror(file))) ready = false;
        if (ready) {
            status = receive_result(&link, show, deadline);
            if (status < 0) { status = 2; ready = false; }
        }
    }
    if (!ready) fputs("peer failed or timed out; an interrupted write may have committed; query before retry\n", stderr);
    pure_link_close(&link, ready);
    SSL_CTX_free(ctx);
    if (file && fclose(file)) status = 2;
    return status;
}
#endif
int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);
#ifndef PURE_PEER_SERVER_ONLY
    if (argc >= 11 && !strcmp(argv[1], "call"))
        return call(argv[2], argv[3], argv[4], argv[5], argv[6], argv[7], argv[8], argc - 9, argv + 9);
#endif
    if ((argc == 10 || argc == 11) && !strcmp(argv[1], "serve")) {
        uint64_t limit = 0;
        if (argc == 11 && (!number(argv[10], UINT32_MAX, &limit) || !limit)) goto usage;
        return serve(argv[2], argv[3], argv[4], argv[5], argv[6], argv[7], argv[8], argv[9],
                     (unsigned)limit);
    }
usage:
    fputs("pure-document-peer serve BIND_IP PORT PROGRAM STORE_DIR CA.pem SERVER_CERT.pem SERVER_KEY.pem CLIENT_DNS [MAX_CONNECTIONS]\n", stderr);
#ifndef PURE_PEER_SERVER_ONLY
    fputs("pure-document-peer call HOST PORT SERVER_DNS PROGRAM CA.pem CLIENT_CERT.pem CLIENT_KEY.pem show KEY\n"
          "pure-document-peer call HOST PORT SERVER_DNS PROGRAM CA.pem CLIENT_CERT.pem CLIENT_KEY.pem edit KEY REV OFFSET REMOVE INSERT_FILE\n"
          "pure-document-peer call HOST PORT SERVER_DNS PROGRAM CA.pem CLIENT_CERT.pem CLIENT_KEY.pem apply KEY REV INPUT_FILE\n", stderr);
#endif
    return 2;
}
