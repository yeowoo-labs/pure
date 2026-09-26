// Reference service for Pure's HTTP benchmarks in drogon (C++), with the
// contract of axum-baseline (see its src/main.rs): GET/PUT /objects/{key},
// If-Match / If-None-Match: *, a PUT body of u32le start, u32le remove, then
// the bytes to insert; the reply is the inserted bytes; ETag is the revision.
//
// drogon-baseline --port P [--durable DIR | --atomic DIR]
// --durable writes the document and fdatasyncs before answering (not crash
// safe); --atomic writes key.tmp, fsyncs it, renames it over the key and
// fsyncs the directory. Handlers block their event loop while they sync, as
// a straightforward drogon handler would.
#include <arpa/inet.h>
#include <drogon/drogon.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

using namespace drogon;

struct Document {
    std::mutex lock;
    std::string bytes;
    uint64_t revision = 0;
};

static std::mutex documents_lock;
static std::unordered_map<std::string, std::shared_ptr<Document>> documents;
static std::string durable;
static bool atomic_writes = false;

static bool valid_key(const std::string &key) {
    if (key.empty() || key.size() > 64) return false;
    for (char c : key)
        if (!isalnum((unsigned char)c) && c != '-' && c != '_') return false;
    return true;
}

static std::shared_ptr<Document> document(const std::string &key) {
    std::lock_guard<std::mutex> guard(documents_lock);
    auto &slot = documents[key];
    if (!slot) slot = std::make_shared<Document>();
    return slot;
}

static HttpResponsePtr tagged(HttpStatusCode status, uint64_t revision, const char *error, std::string body) {
    auto response = HttpResponse::newHttpResponse();
    response->setStatusCode(status);
    response->setContentTypeCode(CT_APPLICATION_OCTET_STREAM);
    response->addHeader("X-Pure-Error", error);
    if (revision) response->addHeader("ETag", "\"" + std::to_string(revision) + "\"");
    response->setBody(std::move(body));
    return response;
}

static bool write_all(int fd, const std::string &bytes) {
    for (size_t at = 0; at < bytes.size();) {
        ssize_t n = write(fd, bytes.data() + at, bytes.size() - at);
        if (n <= 0) return false;
        at += (size_t)n;
    }
    return true;
}

static bool persist(const std::string &key, const std::string &bytes) {
    std::string path = durable + "/" + key;
    if (!atomic_writes) {
        int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        bool ok = fd >= 0 && write_all(fd, bytes) && fdatasync(fd) == 0;
        if (fd >= 0) close(fd);
        return ok;
    }
    std::string temporary = path + ".tmp";
    int fd = open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    bool ok = fd >= 0 && write_all(fd, bytes) && fsync(fd) == 0;
    if (fd >= 0) close(fd);
    ok = ok && rename(temporary.c_str(), path.c_str()) == 0;
    int directory = ok ? open(durable.c_str(), O_RDONLY | O_DIRECTORY) : -1;
    ok = ok && directory >= 0 && fsync(directory) == 0;
    if (directory >= 0) close(directory);
    return ok;
}

static HttpResponsePtr show(const std::string &key) {
    if (!valid_key(key)) return tagged(k400BadRequest, 0, "request", "");
    auto doc = document(key);
    std::lock_guard<std::mutex> guard(doc->lock);
    if (!doc->revision) return tagged(k404NotFound, 0, "not_found", "");
    return tagged(k200OK, doc->revision, "none", doc->bytes);
}

static HttpResponsePtr edit(const HttpRequestPtr &request, const std::string &key) {
    const std::string &match = request->getHeader("if-match"), &none = request->getHeader("if-none-match");
    std::string_view body = request->body();
    uint64_t expected = 0;
    bool known = false;
    if (!match.empty() && none.empty() && match.size() > 2 && match.front() == '"' && match.back() == '"') {
        char *end = nullptr;
        std::string digits = match.substr(1, match.size() - 2);
        expected = strtoull(digits.c_str(), &end, 10);
        known = !digits.empty() && end && !*end;
    } else if (match.empty() && none == "*") known = true;
    if (!known || !valid_key(key) || body.size() < 8) return tagged(k400BadRequest, 0, "request", "");
    uint32_t start, remove;
    memcpy(&start, body.data(), 4);
    memcpy(&remove, body.data() + 4, 4);
    auto doc = document(key);
    std::lock_guard<std::mutex> guard(doc->lock);
    if (doc->revision != expected) return tagged(k412PreconditionFailed, doc->revision, "conflict", "");
    if (start > doc->bytes.size() || remove > doc->bytes.size() - start)
        return tagged(k413RequestEntityTooLarge, 0, "limit", "");
    std::string_view insert = body.substr(8);
    doc->bytes.replace(start, remove, insert.data(), insert.size());
    if (!durable.empty() && !persist(key, doc->bytes)) return tagged(k500InternalServerError, 0, "storage", "");
    ++doc->revision;
    return tagged(k200OK, doc->revision, "none", std::string(insert));
}

/* drogon listens once per event loop (SO_REUSEPORT): port 0 would give each
 * loop a port of its own, so a free port is chosen first. */
static uint16_t free_port() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t length = sizeof(address);
    uint16_t port = 0;
    if (fd >= 0 && !bind(fd, (sockaddr *)&address, sizeof(address)) && !getsockname(fd, (sockaddr *)&address, &length))
        port = ntohs(address.sin_port);
    if (fd >= 0) close(fd);
    return port;
}

int main(int argc, char **argv) {
    uint16_t port = 8769;
    for (int i = 1; i + 1 < argc; i += 2) {
        std::string flag = argv[i];
        if (flag == "--port") port = (uint16_t)std::stoi(argv[i + 1]);
        else if (flag == "--durable") durable = argv[i + 1];
        else if (flag == "--atomic") durable = argv[i + 1], atomic_writes = true;
        else {
            fprintf(stderr, "unknown argument %s\n", argv[i]);
            return 2;
        }
    }
    if (!durable.empty()) mkdir(durable.c_str(), 0755);
    app().registerHandler(
        "/objects/{key}",
        [](const HttpRequestPtr &request, std::function<void(const HttpResponsePtr &)> &&callback,
           const std::string &key) {
            callback(request->method() == Get ? show(key) : edit(request, key));
        },
        {Get, Put});
    if (!port) port = free_port();
    app().addListener("127.0.0.1", port);
    app().setThreadNum(3); // the benchmark gives servers three cores
    app().setClientMaxBodySize(64 << 20);
    app().setClientMaxMemoryBodySize(64 << 20);
    app().setLogLevel(trantor::Logger::kWarn);
    app().registerBeginningAdvice([] {
        auto listeners = app().getListeners();
        printf("{\"port\":%u}\n", listeners.empty() ? 0u : (unsigned)listeners[0].toPort());
        fflush(stdout);
    });
    app().run();
}
