#define _POSIX_C_SOURCE 200809L
/* Research-only server. It shares the product peer's TLS transport, wire
 * format, program parser/evaluator, and store, but calls the store directly. */
#include "program.h"
#include "store.h"
#include "../../../prototypes/common/tls-link.h"
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

typedef struct { PureResult status; int64_t revision; PureSpan body; } DirectOutcome;

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
static bool valid_key(const char *key) {
    if (!key || !*key) return false;
    size_t size = 0;
    for (; size <= 64 && key[size]; ++size) {
        unsigned char c = (unsigned char)key[size];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_')) return false;
    }
    return size <= 64;
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
static bool write_hello(PureLink *link, const unsigned char digest[32], uint64_t deadline) {
    unsigned char hello[HELLO_BYTES] = {'P','D','O','C',1,0,0,0};
    memcpy(hello + 8, digest, 32);
    return pure_link_write(link, hello, sizeof(hello), deadline);
}
static bool send_result(PureLink *link, DirectOutcome outcome, bool want_body, uint64_t deadline) {
    if (outcome.status > PURE_ALLOCATION || outcome.revision < 0 ||
        outcome.body.size > PURE_OBJECT_LIMIT || (outcome.body.size && !outcome.body.data)) return false;
    unsigned char header[REPLY_BYTES];
    header[0] = (unsigned char)outcome.status;
    put64(header + 1, (uint64_t)outcome.revision);
    put32(header + 9, (uint32_t)outcome.body.size);
    return pure_link_write(link, header, sizeof(header), deadline) &&
           (!want_body || pure_link_write(link, outcome.body.data, outcome.body.size, deadline));
}
static bool serve_request(PureLink *link, const PureProgram *program, const char *store, int *end) {
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
    if (!valid_key(key)) {
        (void)send_result(link, (DirectOutcome){.status = PURE_INVALID}, want_body, deadline);
        return false;
    }
    DirectOutcome outcome = {0};
    if (op == OP_READ) {
        PureStoredView view = {0};
        outcome.status = pure_store_select(store, key, program->read.offset,
            program->read.length, program->read.to_end, program->read_required,
            4u * 1024u * 1024u, &view);
        if (outcome.status == PURE_OK) {
            outcome.body = view.body;
            outcome.revision = view.revision;
        }
        bool sent = send_result(link, outcome, want_body, deadline);
        pure_store_release(&view);
        return sent;
    }
    unsigned char *input = body_size ? malloc(body_size) : NULL;
    if (body_size && !input) {
        (void)send_result(link, (DirectOutcome){.status = PURE_ALLOCATION}, want_body, deadline);
        return false; /* Body deliberately not consumed after rejected admission. */
    }
    if (body_size && pure_link_read(link, input, body_size, deadline) != 1) {
        free(input);
        return false;
    }
    PureSpan value = {0}, reply = {0}, source = {input, body_size};
    size_t offset = 0, remove = 0;
    if (!pure_program_input(program, source, &value, &reply) ||
        (program->splice && !pure_program_splice(program, source, &offset, &remove)))
        outcome.status = PURE_LIMIT;
    else if (program->splice)
        outcome.status = pure_store_splice(store, key, (int64_t)expected,
                                            offset, remove, value, &outcome.revision);
    else outcome.status = pure_store_put(store, key, (int64_t)expected, value,
                                          &outcome.revision);
    if (outcome.status == PURE_OK) outcome.body = reply;
    bool sent = send_result(link, outcome, want_body, deadline);
    free(input);
    return sent;
}
static bool serve_connection(PureLink *link, const PureProgram *program, const char *store,
                             const unsigned char digest[32]) {
    if (!write_hello(link, digest, pure_link_now_ms() + PURE_LINK_DEADLINE_MS)) return false;
    int end = 0;
    while (!end) if (!serve_request(link, program, store, &end)) return false;
    return true;
}
static int serve(const char *bind_address, const char *port, const char *program_path,
                 const char *store, const char *ca, const char *cert,
                 const char *key, const char *client_name, unsigned max_connections) {
    if (!pure_link_port_number(port, true) || !client_name || !*client_name) return 2;
    PureProgram program;
    unsigned char digest[32];
    if (!load_program(program_path, &program, digest) || pure_store_init(store) != PURE_OK) return 2;
    SSL_CTX *ctx = pure_link_context(true, ca, cert, key, alpn);
    if (!ctx) { ERR_print_errors_fp(stderr); return 2; }
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
        if (healthy) healthy = serve_connection(&link, &program, store, digest);
        pure_link_close(&link, healthy);
    }
    close(listener);
    SSL_CTX_free(ctx);
    return 0;
}
int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);
    if ((argc == 10 || argc == 11) && !strcmp(argv[1], "serve")) {
        uint64_t limit = 0;
        if (argc == 11 && (!number(argv[10], UINT32_MAX, &limit) || !limit)) return 2;
        return serve(argv[2], argv[3], argv[4], argv[5], argv[6], argv[7], argv[8], argv[9],
                     (unsigned)limit);
    }
    fputs("direct-peer serve BIND_IP PORT PROGRAM STORE_DIR CA CERT KEY CLIENT_DNS [MAX_CONNECTIONS]\n", stderr);
    return 2;
}
