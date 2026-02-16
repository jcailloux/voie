#include <catch2/catch_test_macros.hpp>
#include <catch2/benchmark/catch_benchmark.hpp>
#include <voie/ctx.h>
#include "router.h"
#include "http_parser.h"
#include "connection.h"
#include <picohttpparser.h>
#include <string>

using namespace voie;
using namespace voie::detail;

static std::vector<handler> make_chain() {
    std::vector<handler> chain;
    chain.emplace_back([](ctx&) {});
    return chain;
}

// ============================================================================
// Router lookup benchmarks
// ============================================================================

TEST_CASE("bench: router lookup", "[!benchmark]") {
    router r;
    r.add_route(http_method::GET, "/", make_chain());
    r.add_route(http_method::GET, "/users", make_chain());
    r.add_route(http_method::GET, "/users/:id", make_chain());
    r.add_route(http_method::POST, "/users", make_chain());
    r.add_route(http_method::GET, "/users/:id/posts", make_chain());
    r.add_route(http_method::GET, "/users/:id/posts/:post_id", make_chain());
    r.add_route(http_method::GET, "/api/v1/health", make_chain());
    r.add_route(http_method::GET, "/api/v1/users", make_chain());
    r.add_route(http_method::GET, "/api/v1/users/:id", make_chain());
    r.add_route(http_method::POST, "/api/v1/users", make_chain());
    r.add_route(http_method::PUT, "/api/v1/users/:id", make_chain());
    r.add_route(http_method::DELETE, "/api/v1/users/:id", make_chain());
    r.add_route(http_method::GET, "/static/*filepath", make_chain());
    r.freeze();

    BENCHMARK("root /") {
        return r.lookup(http_method::GET, "/");
    };

    BENCHMARK("static /users") {
        return r.lookup(http_method::GET, "/users");
    };

    BENCHMARK("param /users/42") {
        return r.lookup(http_method::GET, "/users/42");
    };

    BENCHMARK("nested /users/42/posts/99") {
        return r.lookup(http_method::GET, "/users/42/posts/99");
    };

    BENCHMARK("deep /api/v1/users/42") {
        return r.lookup(http_method::GET, "/api/v1/users/42");
    };

    BENCHMARK("catch-all /static/css/main.css") {
        return r.lookup(http_method::GET, "/static/css/main.css");
    };

    BENCHMARK("miss /not-found") {
        return r.lookup(http_method::GET, "/not-found");
    };
}

// ============================================================================
// HTTP parser benchmarks
// ============================================================================

TEST_CASE("bench: http parser", "[!benchmark]") {
    // Simple request (few headers)
    const char* simple_req =
        "GET /api/v1/users HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Accept: application/json\r\n"
        "\r\n";
    std::size_t simple_len = std::strlen(simple_req);

    BENCHMARK("parse simple (2 headers)") {
        phr_header headers[64];
        return parse_request(simple_req, simple_len, 0, headers, 64);
    };

    // Realistic request (many headers, exercises AVX2)
    const char* full_req =
        "GET /api/v1/users/42 HTTP/1.1\r\n"
        "Host: api.example.com\r\n"
        "Accept: application/json\r\n"
        "Accept-Language: en-US,en;q=0.9\r\n"
        "Accept-Encoding: gzip, deflate, br\r\n"
        "Connection: keep-alive\r\n"
        "User-Agent: Mozilla/5.0 (X11; Linux x86_64) voie-bench/1.0\r\n"
        "Authorization: Bearer eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9\r\n"
        "Cache-Control: no-cache\r\n"
        "X-Request-Id: 550e8400-e29b-41d4-a716-446655440000\r\n"
        "X-Forwarded-For: 192.168.1.1\r\n"
        "X-Forwarded-Proto: https\r\n"
        "Cookie: session=abc123; theme=dark\r\n"
        "\r\n";
    std::size_t full_len = std::strlen(full_req);

    BENCHMARK("parse realistic (12 headers)") {
        phr_header headers[64];
        return parse_request(full_req, full_len, 0, headers, 64);
    };

    // Build a large request with 30 headers
    static std::string big_req = []() {
        std::string r = "GET / HTTP/1.1\r\n";
        for (int i = 0; i < 30; ++i)
            r += "X-Header-" + std::to_string(i) + ": value-padding-" + std::to_string(i) + "-abcdef\r\n";
        r += "\r\n";
        return r;
    }();
    std::size_t big_len = big_req.size();

    BENCHMARK("parse large (30 headers)") {
        phr_header headers[64];
        return parse_request(big_req.data(), big_len, 0, headers, 64);
    };
}

// ============================================================================
// Full request cycle (parse + route + ctx)
// ============================================================================

TEST_CASE("bench: full request cycle", "[!benchmark]") {
    router r;
    r.add_route(http_method::GET, "/api/v1/users/:id", [] {
        std::vector<handler> chain;
        chain.emplace_back([](ctx& c) { c.json(R"({"id":42})"); });
        return chain;
    }());
    r.freeze();

    const char* req =
        "GET /api/v1/users/42 HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Accept: application/json\r\n"
        "\r\n";
    std::size_t req_len = std::strlen(req);

    BENCHMARK("parse + route + handler") {
        phr_header headers[64];
        auto parsed = parse_request(req, req_len, 0, headers, 64);
        if (!parsed.has_value()) return;
        auto method = string_to_method(parsed->method);
        auto match = r.lookup(method, parsed->path);
        if (!match.has_value()) return;

        connection conn(-1, arena(4096));
        ctx c(conn, *parsed, *match);
        c.next();
    };
}