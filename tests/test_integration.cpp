#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
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
#include <vector>

// ============================================================================
// Helpers
// ============================================================================

static std::atomic<uint16_t> next_port{19800};

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

// Parsed HTTP response
struct http_response {
    int status = 0;
    std::string raw;      // full raw response
    std::string body;
};

// Read exactly one HTTP response from a connected fd.
// Parses Content-Length to know where the body ends (works with keep-alive).
static http_response read_response(int fd) {
    http_response resp;
    char buf[4096];

    // Read until we have the complete headers (\r\n\r\n)
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

    // Calculate how much body we already have
    auto hdr_end = resp.raw.find("\r\n\r\n") + 4;
    std::size_t body_received = resp.raw.size() - hdr_end;

    // Read remaining body bytes
    while (body_received < content_length) {
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        resp.raw.append(buf, static_cast<std::size_t>(n));
        body_received += static_cast<std::size_t>(n);
    }

    resp.body = resp.raw.substr(hdr_end, content_length);
    return resp;
}

// Open a TCP connection to localhost:port with a read timeout
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

// Send a request and read one response on an existing connection
static http_response send_request(int fd, const std::string& request) {
    ::send(fd, request.data(), request.size(), MSG_NOSIGNAL);
    return read_response(fd);
}

// One-shot: connect, send GET, read response, close.
static http_response http_get(uint16_t port, const std::string& path) {
    int fd = connect_to(port);
    if (fd < 0) return {};
    std::string req = "GET " + path + " HTTP/1.1\r\nHost: localhost\r\n\r\n";
    auto resp = send_request(fd, req);
    ::close(fd);
    return resp;
}

// One-shot: connect, send raw request, read response, close.
static http_response send_raw(uint16_t port, const std::string& request) {
    int fd = connect_to(port);
    if (fd < 0) return {};
    auto resp = send_request(fd, request);
    ::close(fd);
    return resp;
}

// RAII server: starts in constructor, stops in destructor.
struct test_server {
    voie::app app;
    std::thread thread;
    uint16_t port;

    explicit test_server(uint16_t p, voie::backend be = voie::backend::auto_detect) : port(p) {
        app.threads(2);
        app.set_backend(be);
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
// Backend parametrization
// ============================================================================

#define BACKEND_TEST_PREAMBLE \
    auto be = GENERATE(voie::backend::io_uring, voie::backend::epoll); \
    if (!voie::app::backend_available(be)) SKIP("Backend not available"); \
    CAPTURE(be)

// ============================================================================
// Basic request-response
// ============================================================================

TEST_CASE("integration: GET text response", "[integration]") {
    BACKEND_TEST_PREAMBLE;
    auto port = alloc_port();
    test_server srv(port, be);
    srv.app.get("/hello", [](voie::ctx& c) { c.text("hello, world"); });
    srv.start();

    auto resp = http_get(port, "/hello");
    REQUIRE(resp.status == 200);
    REQUIRE(resp.body == "hello, world");
    REQUIRE(resp.raw.find("text/plain") != std::string::npos);
}

TEST_CASE("integration: GET json response", "[integration]") {
    BACKEND_TEST_PREAMBLE;
    auto port = alloc_port();
    test_server srv(port, be);
    srv.app.get("/data", [](voie::ctx& c) { c.json(R"({"ok":true})"); });
    srv.start();

    auto resp = http_get(port, "/data");
    REQUIRE(resp.status == 200);
    REQUIRE(resp.body == R"({"ok":true})");
    REQUIRE(resp.raw.find("application/json") != std::string::npos);
}

TEST_CASE("integration: prebuilt response", "[integration]") {
    BACKEND_TEST_PREAMBLE;
    auto port = alloc_port();
    test_server srv(port, be);
    srv.app.get("/fast", voie::prebuilt("prebuilt!", "text/plain"));
    srv.start();

    auto resp = http_get(port, "/fast");
    REQUIRE(resp.status == 200);
    REQUIRE(resp.body == "prebuilt!");
}

TEST_CASE("integration: custom status code", "[integration]") {
    BACKEND_TEST_PREAMBLE;
    auto port = alloc_port();
    test_server srv(port, be);
    srv.app.get("/created", [](voie::ctx& c) { c.status(201).json(R"({"id":1})"); });
    srv.start();

    auto resp = http_get(port, "/created");
    REQUIRE(resp.status == 201);
}

// ============================================================================
// Routing
// ============================================================================

TEST_CASE("integration: route parameters", "[integration]") {
    BACKEND_TEST_PREAMBLE;
    auto port = alloc_port();
    test_server srv(port, be);
    srv.app.get("/users/:id", [](voie::ctx& c) {
        std::string body = "user:";
        body += c.param("id");
        c.text(body);
    });
    srv.start();

    auto resp = http_get(port, "/users/42");
    REQUIRE(resp.status == 200);
    REQUIRE(resp.body == "user:42");
}

TEST_CASE("integration: multiple routes", "[integration]") {
    BACKEND_TEST_PREAMBLE;
    auto port = alloc_port();
    test_server srv(port, be);
    srv.app.get("/a", [](voie::ctx& c) { c.text("A"); });
    srv.app.get("/b", [](voie::ctx& c) { c.text("B"); });
    srv.app.get("/c", [](voie::ctx& c) { c.text("C"); });
    srv.start();

    int fd = connect_to(port);
    REQUIRE(fd >= 0);

    REQUIRE(send_request(fd, "GET /a HTTP/1.1\r\nHost: localhost\r\n\r\n").body == "A");
    REQUIRE(send_request(fd, "GET /b HTTP/1.1\r\nHost: localhost\r\n\r\n").body == "B");
    REQUIRE(send_request(fd, "GET /c HTTP/1.1\r\nHost: localhost\r\n\r\n").body == "C");

    ::close(fd);
}

TEST_CASE("integration: different HTTP methods", "[integration]") {
    BACKEND_TEST_PREAMBLE;
    auto port = alloc_port();
    test_server srv(port, be);
    srv.app.get("/res", [](voie::ctx& c) { c.text("got"); });
    srv.app.post("/res", [](voie::ctx& c) { c.text("posted"); });
    srv.start();

    int fd = connect_to(port);
    REQUIRE(fd >= 0);

    auto r1 = send_request(fd,
        "GET /res HTTP/1.1\r\nHost: localhost\r\n\r\n");
    REQUIRE(r1.body == "got");

    auto r2 = send_request(fd,
        "POST /res HTTP/1.1\r\nHost: localhost\r\nContent-Length: 0\r\n\r\n");
    REQUIRE(r2.body == "posted");

    ::close(fd);
}

// ============================================================================
// Groups
// ============================================================================

TEST_CASE("integration: group prefix routing", "[integration]") {
    BACKEND_TEST_PREAMBLE;
    auto port = alloc_port();
    test_server srv(port, be);
    auto api = srv.app.group("/api/v1");
    api.get("/items", [](voie::ctx& c) { c.json(R"(["a","b"])"); });
    api.get("/items/:id", [](voie::ctx& c) {
        std::string body = "item:";
        body += c.param("id");
        c.text(body);
    });
    srv.start();

    int fd = connect_to(port);
    REQUIRE(fd >= 0);

    REQUIRE(send_request(fd, "GET /api/v1/items HTTP/1.1\r\nHost: localhost\r\n\r\n").body == R"(["a","b"])");
    REQUIRE(send_request(fd, "GET /api/v1/items/7 HTTP/1.1\r\nHost: localhost\r\n\r\n").body == "item:7");
    REQUIRE(send_request(fd, "GET /api/v1/missing HTTP/1.1\r\nHost: localhost\r\n\r\n").status == 404);

    ::close(fd);
}

// ============================================================================
// Middleware
// ============================================================================

TEST_CASE("integration: middleware chain", "[integration]") {
    BACKEND_TEST_PREAMBLE;
    auto port = alloc_port();
    test_server srv(port, be);
    srv.app.get("/chain",
        [](voie::ctx& c) {
            c.set("X-Step", "1");
            c.next();
        },
        [](voie::ctx& c) {
            c.text("chained");
        }
    );
    srv.start();

    auto resp = http_get(port, "/chain");
    REQUIRE(resp.status == 200);
    REQUIRE(resp.body == "chained");
    REQUIRE(resp.raw.find("X-Step: 1") != std::string::npos);
}

TEST_CASE("integration: middleware short-circuit", "[integration]") {
    BACKEND_TEST_PREAMBLE;
    auto port = alloc_port();
    test_server srv(port, be);
    srv.app.get("/guarded",
        [](voie::ctx& c) {
            if (c.header("Authorization").empty()) {
                c.status(401).json(R"({"error":"unauthorized"})");
                return;
            }
            c.next();
        },
        [](voie::ctx& c) {
            c.text("secret");
        }
    );
    srv.start();

    int fd = connect_to(port);
    REQUIRE(fd >= 0);

    // Without auth → 401
    auto resp1 = send_request(fd,
        "GET /guarded HTTP/1.1\r\nHost: localhost\r\n\r\n");
    REQUIRE(resp1.status == 401);

    // With auth → 200
    auto resp2 = send_request(fd,
        "GET /guarded HTTP/1.1\r\nHost: localhost\r\nAuthorization: Bearer tok\r\n\r\n");
    REQUIRE(resp2.status == 200);
    REQUIRE(resp2.body == "secret");

    ::close(fd);
}

TEST_CASE("integration: global middleware", "[integration]") {
    BACKEND_TEST_PREAMBLE;
    auto port = alloc_port();
    test_server srv(port, be);
    srv.app.use([](voie::ctx& c) {
        c.set("X-Global", "yes");
        c.next();
    });
    srv.app.get("/a", [](voie::ctx& c) { c.text("A"); });
    srv.app.get("/b", [](voie::ctx& c) { c.text("B"); });
    srv.start();

    int fd = connect_to(port);
    REQUIRE(fd >= 0);

    auto resp1 = send_request(fd,
        "GET /a HTTP/1.1\r\nHost: localhost\r\n\r\n");
    REQUIRE(resp1.raw.find("X-Global: yes") != std::string::npos);

    auto resp2 = send_request(fd,
        "GET /b HTTP/1.1\r\nHost: localhost\r\n\r\n");
    REQUIRE(resp2.raw.find("X-Global: yes") != std::string::npos);

    ::close(fd);
}

// ============================================================================
// Error handling
// ============================================================================

TEST_CASE("integration: 404 default", "[integration]") {
    BACKEND_TEST_PREAMBLE;
    auto port = alloc_port();
    test_server srv(port, be);
    srv.app.get("/exists", [](voie::ctx& c) { c.text("ok"); });
    srv.start();

    auto resp = http_get(port, "/nope");
    REQUIRE(resp.status == 404);
}

TEST_CASE("integration: custom 404 handler", "[integration]") {
    BACKEND_TEST_PREAMBLE;
    auto port = alloc_port();
    test_server srv(port, be);
    srv.app.get("/exists", [](voie::ctx& c) { c.text("ok"); });
    srv.app.not_found([](voie::ctx& c) {
        c.status(404).json(R"({"error":"not found"})");
    });
    srv.start();

    auto resp = http_get(port, "/nope");
    REQUIRE(resp.status == 404);
    REQUIRE(resp.body.find("not found") != std::string::npos);
}

TEST_CASE("integration: error handler catches exceptions", "[integration]") {
    BACKEND_TEST_PREAMBLE;
    auto port = alloc_port();
    test_server srv(port, be);
    srv.app.get("/boom", [](voie::ctx& c) {
        throw std::runtime_error("test error");
    });
    srv.app.on_error([](voie::ctx& c, std::exception_ptr) {
        c.status(500).json(R"({"error":"caught"})");
    });
    srv.start();

    auto resp = http_get(port, "/boom");
    REQUIRE(resp.status == 500);
    REQUIRE(resp.body.find("caught") != std::string::npos);
}

TEST_CASE("integration: malformed request gets 400", "[integration]") {
    BACKEND_TEST_PREAMBLE;
    auto port = alloc_port();
    test_server srv(port, be);
    srv.app.get("/", [](voie::ctx& c) { c.text("ok"); });
    srv.start();

    auto resp = send_raw(port, "INVALID\r\n\r\n");
    REQUIRE(resp.status == 400);
}

// ============================================================================
// Keep-alive
// ============================================================================

TEST_CASE("integration: keep-alive multiple requests on one connection", "[integration]") {
    BACKEND_TEST_PREAMBLE;
    auto port = alloc_port();
    test_server srv(port, be);
    srv.app.get("/ping", [](voie::ctx& c) { c.text("pong"); });
    srv.start();

    int fd = connect_to(port);
    REQUIRE(fd >= 0);

    for (int i = 0; i < 5; ++i) {
        auto resp = send_request(fd,
            "GET /ping HTTP/1.1\r\nHost: localhost\r\n\r\n");
        REQUIRE(resp.status == 200);
        REQUIRE(resp.body == "pong");
    }
    ::close(fd);
}

// ============================================================================
// Concurrency
// ============================================================================

TEST_CASE("integration: concurrent connections", "[integration]") {
    BACKEND_TEST_PREAMBLE;
    auto port = alloc_port();
    test_server srv(port, be);
    srv.app.get("/concurrent", [](voie::ctx& c) { c.text("ok"); });
    srv.start();

    constexpr int N = 20;
    std::vector<std::thread> threads;
    std::atomic<int> success_count{0};

    for (int i = 0; i < N; ++i) {
        threads.emplace_back([port, &success_count]() {
            // Retry up to 3 times — under high concurrency a connect or
            // recv can transiently fail (kernel backlog, busy io_uring).
            for (int attempt = 0; attempt < 3; ++attempt) {
                auto resp = http_get(port, "/concurrent");
                if (resp.status == 200 && resp.body == "ok") {
                    success_count++;
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        });
    }

    for (auto& t : threads) t.join();
    REQUIRE(success_count == N);
}

TEST_CASE("integration: concurrent keep-alive connections", "[integration]") {
    BACKEND_TEST_PREAMBLE;
    auto port = alloc_port();
    test_server srv(port, be);
    srv.app.get("/ka", [](voie::ctx& c) { c.text("ok"); });
    srv.start();

    constexpr int CLIENTS = 5;
    constexpr int REQUESTS_PER_CLIENT = 10;
    std::vector<std::thread> threads;
    std::atomic<int> success_count{0};

    for (int i = 0; i < CLIENTS; ++i) {
        threads.emplace_back([port, &success_count]() {
            // Retry connect if it transiently fails under concurrency
            int fd = -1;
            for (int attempt = 0; attempt < 3 && fd < 0; ++attempt) {
                fd = connect_to(port);
                if (fd < 0) std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            if (fd < 0) return;
            for (int j = 0; j < REQUESTS_PER_CLIENT; ++j) {
                auto resp = send_request(fd,
                    "GET /ka HTTP/1.1\r\nHost: localhost\r\n\r\n");
                if (resp.status == 200 && resp.body == "ok") {
                    success_count++;
                }
            }
            ::close(fd);
        });
    }

    for (auto& t : threads) t.join();
    REQUIRE(success_count == CLIENTS * REQUESTS_PER_CLIENT);
}

// ============================================================================
// POST with body
// ============================================================================

TEST_CASE("integration: POST with body echo", "[integration]") {
    BACKEND_TEST_PREAMBLE;
    auto port = alloc_port();
    test_server srv(port, be);
    srv.app.post("/echo", [](voie::ctx& c) {
        c.text(c.body());
    });
    srv.start();

    std::string body = "hello from client";
    std::string req = "POST /echo HTTP/1.1\r\nHost: localhost\r\n"
                      "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
    auto resp = send_raw(port, req);
    REQUIRE(resp.status == 200);
    REQUIRE(resp.body == body);
}

// ============================================================================
// Redirect
// ============================================================================

TEST_CASE("integration: redirect response", "[integration]") {
    BACKEND_TEST_PREAMBLE;
    auto port = alloc_port();
    test_server srv(port, be);
    srv.app.get("/old", [](voie::ctx& c) { c.redirect("/new", 301); });
    srv.start();

    auto resp = http_get(port, "/old");
    REQUIRE(resp.status == 301);
    REQUIRE(resp.raw.find("Location: /new") != std::string::npos);
}

// ============================================================================
// Query string through full stack
// ============================================================================

TEST_CASE("integration: query string parameters", "[integration]") {
    BACKEND_TEST_PREAMBLE;
    auto port = alloc_port();
    test_server srv(port, be);
    srv.app.get("/search", [](voie::ctx& c) {
        std::string result = "q=";
        result += c.query("q");
        c.text(result);
    });
    srv.start();

    auto resp = http_get(port, "/search?q=hello");
    REQUIRE(resp.status == 200);
    REQUIRE(resp.body == "q=hello");
}

// ============================================================================
// Custom response headers
// ============================================================================

TEST_CASE("integration: custom response headers", "[integration]") {
    BACKEND_TEST_PREAMBLE;
    auto port = alloc_port();
    test_server srv(port, be);
    srv.app.get("/headers", [](voie::ctx& c) {
        c.set("X-Custom", "myvalue");
        c.set("X-Request-Id", "abc-123");
        c.text("ok");
    });
    srv.start();

    auto resp = http_get(port, "/headers");
    REQUIRE(resp.status == 200);
    REQUIRE(resp.raw.find("X-Custom: myvalue") != std::string::npos);
    REQUIRE(resp.raw.find("X-Request-Id: abc-123") != std::string::npos);
}

// ============================================================================
// app.all() — registers for all HTTP methods
// ============================================================================

TEST_CASE("integration: all() responds to multiple methods", "[integration]") {
    BACKEND_TEST_PREAMBLE;
    auto port = alloc_port();
    test_server srv(port, be);
    srv.app.all("/any", [](voie::ctx& c) {
        std::string body = "method:";
        body += c.method();
        c.text(body);
    });
    srv.start();

    int fd = connect_to(port);
    REQUIRE(fd >= 0);

    // GET
    auto r1 = send_request(fd,
        "GET /any HTTP/1.1\r\nHost: localhost\r\n\r\n");
    REQUIRE(r1.status == 200);
    REQUIRE(r1.body == "method:GET");

    // POST
    auto r2 = send_request(fd,
        "POST /any HTTP/1.1\r\nHost: localhost\r\nContent-Length: 0\r\n\r\n");
    REQUIRE(r2.status == 200);
    REQUIRE(r2.body == "method:POST");

    // PUT
    auto r3 = send_request(fd,
        "PUT /any HTTP/1.1\r\nHost: localhost\r\nContent-Length: 0\r\n\r\n");
    REQUIRE(r3.status == 200);
    REQUIRE(r3.body == "method:PUT");

    // DELETE
    auto r4 = send_request(fd,
        "DELETE /any HTTP/1.1\r\nHost: localhost\r\n\r\n");
    REQUIRE(r4.status == 200);
    REQUIRE(r4.body == "method:DELETE");

    // PATCH
    auto r5 = send_request(fd,
        "PATCH /any HTTP/1.1\r\nHost: localhost\r\nContent-Length: 0\r\n\r\n");
    REQUIRE(r5.status == 200);
    REQUIRE(r5.body == "method:PATCH");

    ::close(fd);
}

// ============================================================================
// listen(address, port)
// ============================================================================

TEST_CASE("integration: listen on specific address", "[integration]") {
    BACKEND_TEST_PREAMBLE;
    auto port = alloc_port();
    voie::app app;
    app.threads(1);
    app.set_backend(be);
    app.get("/addr", [](voie::ctx& c) { c.text("bound"); });

    std::thread t([&]() { app.listen("127.0.0.1", port); });
    REQUIRE(app.wait_ready());

    auto resp = http_get(port, "/addr");
    REQUIRE(resp.status == 200);
    REQUIRE(resp.body == "bound");

    app.shutdown();
    wake_loops(port, 3);
    t.join();
}

// ============================================================================
// max_body() enforcement
// ============================================================================

// ============================================================================
// Rapid connection churn — stress test for SQE/fd cleanup
// ============================================================================

TEST_CASE("integration: rapid connection churn", "[integration]") {
    BACKEND_TEST_PREAMBLE;
    auto port = alloc_port();
    test_server srv(port, be);
    srv.app.get("/churn", [](voie::ctx& c) { c.text("ok"); });
    srv.start();

    constexpr int ROUNDS = 50;
    int success_count = 0;

    for (int i = 0; i < ROUNDS; ++i) {
        for (int attempt = 0; attempt < 3; ++attempt) {
            int fd = connect_to(port);
            if (fd < 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            auto resp = send_request(fd,
                "GET /churn HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
            ::close(fd);
            if (resp.status == 200 && resp.body == "ok") {
                ++success_count;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    REQUIRE(success_count == ROUNDS);
}

// ============================================================================
// max_body() enforcement
// ============================================================================

TEST_CASE("integration: max_body rejects oversized body", "[integration]") {
    BACKEND_TEST_PREAMBLE;
    auto port = alloc_port();
    test_server srv(port, be);
    srv.app.max_body(64);
    srv.app.post("/upload", [](voie::ctx& c) { c.text("ok"); });
    srv.start();

    int fd = connect_to(port);
    REQUIRE(fd >= 0);

    // Body within limit
    std::string small_body(32, 'x');
    std::string small_req = "POST /upload HTTP/1.1\r\nHost: localhost\r\n"
        "Content-Length: " + std::to_string(small_body.size()) + "\r\n\r\n" + small_body;
    auto r1 = send_request(fd, small_req);
    REQUIRE(r1.status == 200);

    // Body exceeding limit (server sends 413 + Connection: close)
    std::string big_body(128, 'x');
    std::string big_req = "POST /upload HTTP/1.1\r\nHost: localhost\r\n"
        "Content-Length: " + std::to_string(big_body.size()) + "\r\n\r\n" + big_body;
    auto r2 = send_request(fd, big_req);
    REQUIRE(r2.status == 413);

    ::close(fd);
}
