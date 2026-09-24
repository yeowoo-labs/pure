#define _POSIX_C_SOURCE 200809L
/* Optional authenticated Pure-peer adapter. No TLS, socket, or protocol type
 * enters runtime.c, the scalar module, or the standalone native CLI. */
#include "peer-wire.h"
#include "../../common/tls-link.h"
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

extern const PureScalarPlan pure_app_plan;
static const unsigned char alpn[] = {6, 'p','u','r','e','/','1'};
static volatile sig_atomic_t stopping;
static void signal_stop(int signum) { (void)signum; stopping = 1; }
static SSL_CTX *context(bool server, const char *ca, const char *cert, const char *key) {
    return pure_link_context(server, ca, cert, key, alpn);
}
static bool handshake(PureLink *channel, SSL_CTX *ctx, bool server, const char *peer_name) {
    return pure_link_handshake(channel, ctx, server, peer_name, alpn);
}

/* A connection has one physical input source. Disconnect ends all held input
 * phases before another peer can use the same source slot. */
static void release_inputs(PureNativeApp *app) {
    for (uint32_t i = 0; i < app->plan->input_count && i < 8; ++i) {
        PureScalarInputKind kind = app->plan->inputs[i].kind;
        if (kind == PURE_KEY && (app->key_sources[i] & (1u << 1)))
            (void)pure_native_app_action(app, (PureAction){.input = i, .phase = PURE_UP, .source = 1});
        if (kind == PURE_POINT && (app->held & (1u << i)) && app->pointer_owners[i] == 1)
            (void)pure_native_app_action(app, (PureAction){.input = i, .phase = PURE_CANCEL,
                                                           .target = UINT32_MAX, .source = 1});
    }
    for (uint32_t i = 0; i < app->plan->target_count && i < 24; ++i)
        if ((app->held & (1u << (8 + i))) && app->pointer_owners[8 + i] == 1)
            (void)pure_native_app_action(app, (PureAction){.input = app->plan->targets[i].input,
                                                           .phase = PURE_CANCEL, .target = i, .source = 1});
}
static PureResult execute(PureNativeApp *app, const PurePeerRequest *request) {
    if (request->operation == PURE_PEER_STATE) return PURE_OK;
    if (request->expected != app->revision) return PURE_CONFLICT;
    PureOutcome result;
    if (request->operation == PURE_PEER_SAVE) result = pure_native_app_save(app);
    else if (request->operation == PURE_PEER_LOAD) result = pure_native_app_restore(app);
    else if (request->operation == PURE_PEER_ACTION) {
        PureAction action = request->action;
        action.source = 1;
        result = pure_native_app_action(app, action);
    } else return PURE_INVALID;
    return result.status;
}
static bool serve_connection(PureLink *channel, PureNativeApp *app) {
    unsigned char hello[PURE_PEER_HELLO_BYTES];
    pure_peer_hello(app, hello);
    if (!pure_link_write_frame(channel, hello, sizeof(hello), pure_link_now_ms() + PURE_LINK_DEADLINE_MS)) return false;
    for (;;) {
        unsigned char buffer[PURE_PEER_REQUEST_MAX];
        size_t length = 0;
        uint64_t deadline = pure_link_now_ms() + PURE_LINK_DEADLINE_MS;
        int got = pure_link_read_frame(channel, buffer, sizeof(buffer), &length, deadline);
        if (!got) return true;
        if (got < 0) return false;
        PurePeerRequest request;
        if (!pure_peer_decode_request(buffer, length, app->plan, &request)) return false;
        PureResult status = execute(app, &request);
        unsigned char reply[PURE_PEER_REPLY_MAX];
        length = pure_peer_encode_reply(app, status, reply);
        if (!length || !pure_link_write_frame(channel, reply, length, deadline)) return false;
    }
}
static int serve(const char *bind_address, const char *port, const char *directory,
                 const char *document, const char *ca, const char *cert, const char *key,
                 const char *client_name, unsigned max_connections) {
    if (!pure_link_port_number(port, true) || !client_name || !*client_name) return 2;
    SSL_CTX *ctx = context(true, ca, cert, key);
    if (!ctx) { ERR_print_errors_fp(stderr); return 2; }
    size_t size = pure_scalar_bytes();
    void *memory = malloc(size);
    PureNativeApp app;
    if (!memory || pure_native_app_init(&app, &pure_app_plan, memory, size, directory, document, 4096) != PURE_OK) {
        fprintf(stderr, "app initialization failed\n"); free(memory); SSL_CTX_free(ctx); return 2;
    }
    PureOutcome restore = pure_native_app_restore(&app);
    if (restore.status != PURE_OK && restore.status != PURE_NOT_FOUND) {
        fprintf(stderr, "existing state is invalid or unavailable: %u\n", restore.status);
        free(memory); SSL_CTX_free(ctx); return 2;
    }
    int listener = pure_link_listen_at(bind_address, port);
    if (listener < 0) { perror("listen"); free(memory); SSL_CTX_free(ctx); return 2; }
    struct sockaddr_storage address;
    socklen_t length = sizeof(address);
    if (getsockname(listener, (struct sockaddr *)&address, &length)) abort();
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
        PureLink channel = {.fd = fd};
        bool healthy = pure_link_nonblocking(fd) && handshake(&channel, ctx, true, client_name);
        if (healthy) healthy = serve_connection(&channel, &app);
        pure_link_close(&channel, healthy);
        release_inputs(&app);
    }
    close(listener);
    pure_runtime_stop(&app.execution);
    int status = app.execution.active || app.execution.retained ? 1 : 0;
    free(memory); SSL_CTX_free(ctx);
    return status;
}

static void json_string(const char *value) {
    putchar('"');
    for (const unsigned char *p = (const unsigned char *)value; *p; ++p) {
        if (*p == '"' || *p == '\\') { putchar('\\'); putchar(*p); }
        else if (*p < 32) printf("\\u%04x", *p);
        else putchar(*p);
    }
    putchar('"');
}
static void print_reply(const PurePeerReply *reply) {
    printf("{\"status\":%u,\"revision\":\"%" PRId64 "\",\"stored_revision\":\"%" PRId64
           "\",\"scalar_error\":%u,\"held\":%u,\"states\":{",
           reply->status, reply->revision, reply->stored_revision, reply->scalar_error, reply->held);
    for (uint32_t i = 0; i < reply->state_count; ++i) {
        if (i) putchar(',');
        json_string(pure_app_plan.states[i].name);
        printf(":%.17g", reply->states[i]);
    }
    puts("}}"); fflush(stdout);
}
static bool number(const char *text, double *value) {
    if (!text || !*text) return false;
    char *end;
    errno = 0;
    *value = strtod(text, &end);
    return !errno && !*end && isfinite(*value);
}
static bool parse_command(char *line, PurePeerRequest *request, bool *quit) {
    char *words[8];
    size_t count = 0;
    for (char *part = strtok(line, " \t\r\n"); part; part = strtok(NULL, " \t\r\n")) {
        if (count == 8) return false;
        words[count++] = part;
    }
    if (!count) return false;
    *request = (PurePeerRequest){0};
    if (count == 1) {
        if (!strcmp(words[0], "quit")) { *quit = true; return true; }
        if (!strcmp(words[0], "state")) request->operation = PURE_PEER_STATE;
        else if (!strcmp(words[0], "save")) request->operation = PURE_PEER_SAVE;
        else if (!strcmp(words[0], "load")) request->operation = PURE_PEER_LOAD;
        else if (!strcmp(words[0], "frame") || !strcmp(words[0], "reset")) {
            request->operation = PURE_PEER_ACTION;
            request->action = (PureAction){.input = UINT32_MAX, .target = UINT32_MAX,
                .phase = !strcmp(words[0], "frame") ? PURE_FRAME : PURE_RESET};
        } else return false;
        return true;
    }
    if (count < 3) return false;
    uint32_t input = pure_app_plan.input_count;
    for (uint32_t i = 0; i < pure_app_plan.input_count; ++i)
        if (!strcmp(words[1], pure_app_plan.inputs[i].name)) { input = i; break; }
    if (input >= pure_app_plan.input_count) return false;
    PureScalarInputKind kind = pure_app_plan.inputs[input].kind;
    request->operation = PURE_PEER_ACTION;
    request->action = (PureAction){.input = input, .target = UINT32_MAX};
    if (count == 3 && !strcmp(words[0], "key") && kind == PURE_KEY) {
        if (!strcmp(words[2], "down")) request->action.phase = PURE_DOWN;
        else if (!strcmp(words[2], "up")) request->action.phase = PURE_UP;
        else return false;
    } else if (count == 3 && !strcmp(words[0], "value") && kind == PURE_VALUE) {
        request->action.phase = PURE_CHANGE;
        if (!number(words[2], &request->action.x)) return false;
    } else if (count == 6 && !strcmp(words[0], "point") && (kind == PURE_POINT || kind == PURE_ROUTED)) {
        const char *phases[] = {"down", "up", "move", "cancel"};
        bool phase = false;
        for (unsigned i = 0; i < 4; ++i) if (!strcmp(words[2], phases[i])) {
            request->action.phase = i; phase = true; break;
        }
        if (!phase || !number(words[4], &request->action.x) || !number(words[5], &request->action.y)) return false;
        if (kind == PURE_POINT) {
            if (strcmp(words[3], pure_app_plan.inputs[input].target)) return false;
        } else {
            bool found = false;
            for (uint32_t i = 0; i < pure_app_plan.target_count; ++i)
                if (pure_app_plan.targets[i].input == input && !strcmp(words[3], pure_app_plan.targets[i].view)) {
                    request->action.target = i; found = true; break;
                }
            if (!found) return false;
        }
    } else return false;
    return true;
}
static bool request_reply(PureLink *channel, const PurePeerRequest *request, PurePeerReply *reply) {
    unsigned char wire[PURE_PEER_REPLY_MAX];
    size_t length = pure_peer_encode_request(request, &pure_app_plan, wire);
    if (!length || !pure_link_write_frame(channel, wire, length, pure_link_now_ms() + PURE_LINK_DEADLINE_MS)) return false;
    int got = pure_link_read_frame(channel, wire, sizeof(wire), &length, pure_link_now_ms() + PURE_LINK_DEADLINE_MS);
    return got == 1 && pure_peer_decode_reply(wire, length, &pure_app_plan, reply);
}
static int call(const char *host, const char *port, const char *server_name,
                const char *ca, const char *cert, const char *key) {
    if (!pure_link_port_number(port, false) || !server_name || !*server_name) return 2;
    SSL_CTX *ctx = context(false, ca, cert, key);
    if (!ctx) { ERR_print_errors_fp(stderr); return 2; }
    PureLink channel = {.fd = pure_link_connect_to(host, port)};
    if (channel.fd < 0 || !handshake(&channel, ctx, false, server_name)) {
        fprintf(stderr, "peer authentication or connection failed\n");
        pure_link_close(&channel, false); SSL_CTX_free(ctx); return 2;
    }
    unsigned char hello[PURE_PEER_HELLO_BYTES];
    size_t length = 0;
    int64_t revision, stored;
    if (pure_link_read_frame(&channel, hello, sizeof(hello), &length, pure_link_now_ms() + PURE_LINK_DEADLINE_MS) != 1 ||
        !pure_peer_check_hello(hello, length, &pure_app_plan, &revision, &stored)) {
        fprintf(stderr, "peer application definition mismatch\n");
        pure_link_close(&channel, false); SSL_CTX_free(ctx); return 2;
    }
    (void)stored;
    PurePeerReply reply;
    PurePeerRequest state = {.operation = PURE_PEER_STATE};
    if (!request_reply(&channel, &state, &reply)) {
        fprintf(stderr, "initial state failed\n");
        pure_link_close(&channel, false); SSL_CTX_free(ctx); return 2;
    }
    print_reply(&reply);
    revision = reply.revision;
    int failed = 0;
    char line[512];
    while (fgets(line, sizeof(line), stdin)) {
        if (!strchr(line, '\n') && !feof(stdin)) {
            int c;
            while ((c = getchar()) != '\n' && c != EOF) {}
            fprintf(stderr, "command exceeds 510 bytes\n"); failed = 1; continue;
        }
        PurePeerRequest request;
        bool quit = false;
        if (!parse_command(line, &request, &quit)) {
            fprintf(stderr, "invalid command\n"); failed = 1; continue;
        }
        if (quit) break;
        request.expected = revision;
        if (!request_reply(&channel, &request, &reply)) {
            fprintf(stderr, "peer request failed or timed out; effect may have committed; query before retry\n");
            failed = 1; break;
        }
        print_reply(&reply);
        revision = reply.revision;
        if (reply.status) failed = 1;
    }
    if (ferror(stdin) || ferror(stdout)) failed = 1;
    pure_link_close(&channel, true); SSL_CTX_free(ctx);
    return failed;
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);
    if (argc == 8 && !strcmp(argv[1], "call"))
        return call(argv[2], argv[3], argv[4], argv[5], argv[6], argv[7]);
    if ((argc == 10 || argc == 11) && !strcmp(argv[1], "serve")) {
        unsigned limit = 0;
        if (argc == 11) {
            char *end;
            errno = 0;
            if (!argv[10][0] || argv[10][0] < '0' || argv[10][0] > '9') goto usage;
            unsigned long parsed = strtoul(argv[10], &end, 10);
            if (errno || *end || !parsed || parsed > UINT_MAX) goto usage;
            limit = (unsigned)parsed;
        }
        return serve(argv[2], argv[3], argv[4], argv[5], argv[6], argv[7], argv[8], argv[9], limit);
    }
usage:
    fprintf(stderr, "pure-peer serve BIND_IP PORT STORE_DIR DOCUMENT_KEY CA.pem SERVER_CERT.pem SERVER_KEY.pem CLIENT_DNS [MAX_CONNECTIONS]\n"
                    "pure-peer call HOST PORT SERVER_DNS CA.pem CLIENT_CERT.pem CLIENT_KEY.pem\n"
                    "Commands: state, save, load, frame, reset, key NAME down|up, value NAME NUMBER,\n"
                    "point NAME down|move|up|cancel TARGET X Y, quit.\n");
    return 2;
}
