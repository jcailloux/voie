#include <catch2/catch_test_macros.hpp>
#include <voie/voie.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>

// ============================================================================
// Helpers (duplicated from test_integration.cpp — separate binary)
// ============================================================================

static std::atomic<uint16_t> next_port{20800};

static uint16_t alloc_port() {
    return next_port.fetch_add(1);
}


static void wake_loops(uint16_t port, int count = 10) {
    for (int i = 0; i < count; ++i) {
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) continue;
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        ::close(fd);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

struct http_response {
    int status = 0;
    std::string raw;
    std::string body;
};

static http_response read_response(int fd) {
    http_response resp;
    char buf[4096];

    while (resp.raw.find("\r\n\r\n") == std::string::npos) {
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) return resp;
        resp.raw.append(buf, static_cast<std::size_t>(n));
    }

    // Parse status (manually — avoid stoi exceptions on corrupted data)
    if (resp.raw.size() >= 12) {
        int s = 0;
        bool valid = true;
        for (int i = 9; i < 12; ++i) {
            if (resp.raw[i] < '0' || resp.raw[i] > '9') { valid = false; break; }
            s = s * 10 + (resp.raw[i] - '0');
        }
        if (valid) resp.status = s;
    }

    // Find Content-Length (manually — avoid stoull exceptions)
    std::size_t content_length = 0;
    auto cl_pos = resp.raw.find("Content-Length: ");
    if (cl_pos != std::string::npos) {
        auto val_start = cl_pos + 16;
        auto val_end = resp.raw.find("\r\n", val_start);
        if (val_end != std::string::npos) {
            for (auto i = val_start; i < val_end; ++i) {
                if (resp.raw[i] < '0' || resp.raw[i] > '9') break;
                content_length = content_length * 10 + static_cast<std::size_t>(resp.raw[i] - '0');
            }
        }
    }

    auto hdr_end = resp.raw.find("\r\n\r\n") + 4;
    std::size_t body_received = resp.raw.size() - hdr_end;

    while (body_received < content_length) {
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        resp.raw.append(buf, static_cast<std::size_t>(n));
        body_received += static_cast<std::size_t>(n);
    }

    resp.body = resp.raw.substr(hdr_end, content_length);
    return resp;
}

static int connect_to(uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        return -1;
    }
    struct timeval tv{};
    tv.tv_sec = 5;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    return fd;
}

static http_response send_request(int fd, const std::string& request) {
    ::send(fd, request.data(), request.size(), MSG_NOSIGNAL);
    return read_response(fd);
}

static http_response http_get(uint16_t port, const std::string& path) {
    int fd = connect_to(port);
    if (fd < 0) return {};
    std::string req = "GET " + path + " HTTP/1.1\r\nHost: localhost\r\n\r\n";
    auto resp = send_request(fd, req);
    ::close(fd);
    return resp;
}

static http_response send_raw(uint16_t port, const std::string& request) {
    int fd = connect_to(port);
    if (fd < 0) return {};
    auto resp = send_request(fd, request);
    ::close(fd);
    return resp;
}

struct test_server {
    voie::app app;
    std::thread thread;
    uint16_t port;

    explicit test_server(uint16_t p) : port(p) {
        app.threads(2);
    }

    void start() {
        thread = std::thread([this]() { app.listen(port); });
        REQUIRE(app.wait_ready());
    }

    ~test_server() {
        app.shutdown();
        wake_loops(port);
        if (thread.joinable()) thread.join();
    }
};

// ============================================================================
// RFC 7231 section 7.1.1.2 — Date header
// ============================================================================

TEST_CASE("rfc: Date header present in response", "[rfc]") {
    auto port = alloc_port();
    test_server srv(port);
    srv.app.get("/", [](voie::ctx& c) { c.text("ok"); });
    srv.start();

    auto resp = http_get(port, "/");
    REQUIRE(resp.status == 200);
    REQUIRE(resp.raw.find("Date:") != std::string::npos);
}

// ============================================================================
// RFC 7230 section 3.3.2 — Content-Length
// ============================================================================

TEST_CASE("rfc: Content-Length present in response with body", "[rfc]") {
    auto port = alloc_port();
    test_server srv(port);
    srv.app.get("/", [](voie::ctx& c) { c.text("ok"); });
    srv.start();

    auto resp = http_get(port, "/");
    REQUIRE(resp.raw.find("Content-Length: 2") != std::string::npos);
}

TEST_CASE("rfc: 204 response has no Content-Length", "[rfc]") {
    auto port = alloc_port();
    test_server srv(port);
    srv.app.get("/empty", [](voie::ctx& c) { c.no_content(); });
    srv.start();

    auto resp = http_get(port, "/empty");
    REQUIRE(resp.status == 204);
    REQUIRE(resp.body.empty());
    REQUIRE(resp.raw.find("Content-Length") == std::string::npos);
}

// ============================================================================
// RFC 7231 section 4.3.2 — HEAD
// ============================================================================

TEST_CASE("rfc: HEAD returns GET headers without body", "[rfc]") {
    auto port = alloc_port();
    test_server srv(port);
    srv.app.get("/data", [](voie::ctx& c) { c.text("hello world"); });
    srv.start();

    int fd = connect_to(port);
    REQUIRE(fd >= 0);
    const char req[] = "HEAD /data HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    ::send(fd, req, sizeof(req) - 1, MSG_NOSIGNAL);

    http_response resp;
    char buf[4096];
    while (true) {
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        resp.raw.append(buf, static_cast<std::size_t>(n));
    }
    ::close(fd);

    if (resp.raw.size() >= 12) {
        int s = 0;
        bool valid = true;
        for (int i = 9; i < 12; ++i) {
            if (resp.raw[i] < '0' || resp.raw[i] > '9') { valid = false; break; }
            s = s * 10 + (resp.raw[i] - '0');
        }
        if (valid) resp.status = s;
    }

    REQUIRE(resp.status == 200);
    REQUIRE(resp.raw.find("Content-Length: 11") != std::string::npos);
    auto hdr_end = resp.raw.find("\r\n\r\n");
    REQUIRE(hdr_end != std::string::npos);
    REQUIRE(resp.raw.size() == hdr_end + 4);
}

// ============================================================================
// RFC 7231 section 4.3.7 — OPTIONS
// ============================================================================

TEST_CASE("rfc: OPTIONS auto-response with Allow header", "[rfc]") {
    auto port = alloc_port();
    test_server srv(port);
    srv.app.get("/resource", [](voie::ctx& c) { c.text("get"); });
    srv.app.post("/resource", [](voie::ctx& c) { c.text("post"); });
    srv.start();

    auto resp = send_raw(port,
        "OPTIONS /resource HTTP/1.1\r\nHost: localhost\r\n\r\n");
    REQUIRE(resp.status == 204);
    REQUIRE(resp.raw.find("Allow:") != std::string::npos);
    REQUIRE(resp.raw.find("GET") != std::string::npos);
    REQUIRE(resp.raw.find("POST") != std::string::npos);
}

// ============================================================================
// RFC 7231 section 6.6.2 — 501 Not Implemented
// ============================================================================

TEST_CASE("rfc: 501 for unknown HTTP method", "[rfc]") {
    auto port = alloc_port();
    test_server srv(port);
    srv.app.get("/", [](voie::ctx& c) { c.text("ok"); });
    srv.start();

    auto resp = send_raw(port,
        "PURGE / HTTP/1.1\r\nHost: localhost\r\n\r\n");
    REQUIRE(resp.status == 501);
}

// ============================================================================
// RFC 7230 section 3.1.2 — Status line format
// ============================================================================

TEST_CASE("rfc: status line format", "[rfc]") {
    auto port = alloc_port();
    test_server srv(port);
    srv.app.get("/", [](voie::ctx& c) { c.text("ok"); });
    srv.start();

    auto resp = http_get(port, "/");
    REQUIRE(resp.raw.starts_with("HTTP/1.1 200 OK\r\n"));
}

// ============================================================================
// RFC 7231 section 3.1.1.2 — charset for text/*
// ============================================================================

TEST_CASE("rfc: charset appended for text/* content types", "[rfc]") {
    auto port = alloc_port();
    test_server srv(port);
    srv.app.get("/text", [](voie::ctx& c) { c.text("hello"); });
    srv.app.get("/json", [](voie::ctx& c) { c.json("{}"); });
    srv.start();

    auto text_resp = http_get(port, "/text");
    REQUIRE(text_resp.raw.find("text/plain; charset=utf-8") != std::string::npos);

    auto json_resp = http_get(port, "/json");
    REQUIRE(json_resp.raw.find("application/json") != std::string::npos);
    // application/json should NOT have charset appended
    REQUIRE(json_resp.raw.find("application/json; charset") == std::string::npos);
}

// ============================================================================
// RFC 3986 section 2.1 — URL decoding
// ============================================================================

TEST_CASE("rfc: URL-decoded route parameters", "[rfc]") {
    auto port = alloc_port();
    test_server srv(port);
    srv.app.get("/users/:name", [](voie::ctx& c) {
        c.text(c.param("name"));
    });
    srv.start();

    auto resp = http_get(port, "/users/John%20Doe");
    REQUIRE(resp.status == 200);
    REQUIRE(resp.body == "John Doe");
}

TEST_CASE("rfc: URL-decoded query parameters", "[rfc]") {
    auto port = alloc_port();
    test_server srv(port);
    srv.app.get("/search", [](voie::ctx& c) {
        c.text(c.query("q"));
    });
    srv.start();

    auto resp = http_get(port, "/search?q=hello%20world");
    REQUIRE(resp.status == 200);
    REQUIRE(resp.body == "hello world");
}

// ============================================================================
// RFC 7230 section 3.2 — has_header vs empty header
// ============================================================================

TEST_CASE("rfc: has_header distinguishes empty from absent", "[rfc]") {
    auto port = alloc_port();
    test_server srv(port);
    srv.app.get("/check", [](voie::ctx& c) {
        bool has_x = c.has_header("X-Empty");
        bool has_missing = c.has_header("X-Missing");
        std::string result;
        result += has_x ? "has_x=true" : "has_x=false";
        result += ",";
        result += has_missing ? "has_missing=true" : "has_missing=false";
        c.text(result);
    });
    srv.start();

    auto resp = send_raw(port,
        "GET /check HTTP/1.1\r\nHost: localhost\r\nX-Empty:\r\n\r\n");
    REQUIRE(resp.status == 200);
    REQUIRE(resp.body == "has_x=true,has_missing=false");
}

// ============================================================================
// RFC 7230 section 6.3.2 — Pipelining
// ============================================================================

TEST_CASE("rfc: pipelining two requests", "[rfc]") {
    auto port = alloc_port();
    test_server srv(port);
    srv.app.get("/a", [](voie::ctx& c) { c.text("A"); });
    srv.app.get("/b", [](voie::ctx& c) { c.text("B"); });
    srv.start();

    int fd = connect_to(port);
    REQUIRE(fd >= 0);

    std::string pipeline =
        "GET /a HTTP/1.1\r\nHost: localhost\r\n\r\n"
        "GET /b HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    ::send(fd, pipeline.data(), pipeline.size(), MSG_NOSIGNAL);

    std::string all;
    char buf[8192];
    while (true) {
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        all.append(buf, static_cast<std::size_t>(n));
    }
    ::close(fd);

    auto first = all.find("HTTP/1.1 200 OK");
    REQUIRE(first != std::string::npos);
    auto second = all.find("HTTP/1.1 200 OK", first + 15);
    REQUIRE(second != std::string::npos);
}

// ============================================================================
// RFC 7230 section 6.1 — Connection: close
// ============================================================================

TEST_CASE("rfc: Connection close is respected", "[rfc]") {
    auto port = alloc_port();
    test_server srv(port);
    srv.app.get("/hello", [](voie::ctx& c) { c.text("hi"); });
    srv.start();

    int fd = connect_to(port);
    REQUIRE(fd >= 0);
    auto resp = send_request(fd,
        "GET /hello HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    REQUIRE(resp.status == 200);
    REQUIRE(resp.raw.find("Connection: close") != std::string::npos);

    char tmp[1];
    ssize_t n = ::recv(fd, tmp, 1, 0);
    REQUIRE(n <= 0);
    ::close(fd);
}

// ============================================================================
// RFC 7230 section 6.3 — HTTP/1.0 default close
// ============================================================================

TEST_CASE("rfc: HTTP/1.0 closes connection by default", "[rfc]") {
    auto port = alloc_port();
    test_server srv(port);
    srv.app.get("/hello", [](voie::ctx& c) { c.text("hi"); });
    srv.start();

    int fd = connect_to(port);
    REQUIRE(fd >= 0);
    auto resp = send_request(fd,
        "GET /hello HTTP/1.0\r\nHost: localhost\r\n\r\n");
    REQUIRE(resp.status == 200);

    char tmp[1];
    ssize_t n = ::recv(fd, tmp, 1, 0);
    REQUIRE(n <= 0);
    ::close(fd);
}

// ============================================================================
// RFC 7230 section 5.4 — Host header required in HTTP/1.1
// ============================================================================

TEST_CASE("rfc: Host header required in HTTP/1.1", "[rfc]") {
    auto port = alloc_port();
    test_server srv(port);
    srv.app.get("/hello", [](voie::ctx& c) { c.text("hi"); });
    srv.start();

    auto resp = send_raw(port,
        "GET /hello HTTP/1.1\r\n\r\n");
    REQUIRE(resp.status == 400);
}

// ============================================================================
// RFC 7230 section 3.1.1 — Method is case-sensitive
// ============================================================================

TEST_CASE("rfc: method is case-sensitive", "[rfc]") {
    auto port = alloc_port();
    test_server srv(port);
    srv.app.get("/hello", [](voie::ctx& c) { c.text("hi"); });
    srv.start();

    auto resp = send_raw(port,
        "get /hello HTTP/1.1\r\nHost: localhost\r\n\r\n");
    REQUIRE(resp.status == 501);
}
