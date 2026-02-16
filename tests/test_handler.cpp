#include <catch2/catch_test_macros.hpp>
#include <voie/handler.h>
#include <voie/ctx.h>
#include "connection.h"
#include "http_parser.h"
#include "router.h"

#include <picohttpparser.h>
#include <memory>
#include <string>

using namespace voie;
using namespace voie::detail;

// Helper: create a minimal ctx for invoking handlers
static connection make_conn() {
    return connection(-1, arena(4096));
}

struct test_request {
    parsed_request req;
    phr_header headers[16]{};
};

static test_request make_request() {
    static const char raw[] = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
    test_request tr;
    auto result = parse_request(raw, sizeof(raw) - 1, 0, tr.headers, 16);
    REQUIRE(result.has_value());
    tr.req = result.value();
    return tr;
}

// ============================================================================
// Basic functionality
// ============================================================================

TEST_CASE("handler: default constructed is empty", "[handler]") {
    handler h;
    REQUIRE(!static_cast<bool>(h));
}

TEST_CASE("handler: lambda is callable", "[handler]") {
    bool called = false;
    handler h([&called](ctx& c) { called = true; });
    REQUIRE(static_cast<bool>(h));

    auto conn = make_conn();
    auto tr = make_request();
    route_match match{};
    ctx c(conn, tr.req, match);
    h(c);
    REQUIRE(called);
}

// ============================================================================
// SBO (Small Buffer Optimization) vs Heap
// ============================================================================

TEST_CASE("handler: small lambda uses SBO", "[handler]") {
    // A small capture (pointer-sized) should fit in the 24-byte buffer
    int x = 42;
    handler h([&x](ctx& c) { x = 99; });
    REQUIRE(static_cast<bool>(h));

    auto conn = make_conn();
    auto tr = make_request();
    route_match match{};
    ctx c(conn, tr.req, match);
    h(c);
    REQUIRE(x == 99);
}

TEST_CASE("handler: large capture goes to heap", "[handler]") {
    // Capture more than 24 bytes to force heap allocation
    std::string large(100, 'A');
    bool called = false;
    handler h([large, &called](ctx& c) { called = (large.size() == 100); });
    REQUIRE(static_cast<bool>(h));

    auto conn = make_conn();
    auto tr = make_request();
    route_match match{};
    ctx c(conn, tr.req, match);
    h(c);
    REQUIRE(called);
}

TEST_CASE("handler: shared_ptr capture (typical middleware)", "[handler]") {
    auto data = std::make_shared<std::string>("prebuilt data");
    handler h([data](ctx& c) {
        // shared_ptr is 16 bytes — fits in SBO
        (void)data->size();
    });
    REQUIRE(static_cast<bool>(h));
    REQUIRE(data.use_count() == 2); // one in test, one in handler
}

// ============================================================================
// Move semantics
// ============================================================================

TEST_CASE("handler: move constructor transfers ownership", "[handler]") {
    bool called = false;
    handler h1([&called](ctx& c) { called = true; });
    handler h2(std::move(h1));

    REQUIRE(!static_cast<bool>(h1));
    REQUIRE(static_cast<bool>(h2));

    auto conn = make_conn();
    auto tr = make_request();
    route_match match{};
    ctx c(conn, tr.req, match);
    h2(c);
    REQUIRE(called);
}

TEST_CASE("handler: move assignment transfers ownership", "[handler]") {
    bool called1 = false, called2 = false;
    handler h1([&called1](ctx& c) { called1 = true; });
    handler h2([&called2](ctx& c) { called2 = true; });

    h2 = std::move(h1);

    REQUIRE(!static_cast<bool>(h1));
    REQUIRE(static_cast<bool>(h2));

    auto conn = make_conn();
    auto tr = make_request();
    route_match match{};
    ctx c(conn, tr.req, match);
    h2(c);
    REQUIRE(called1);
    REQUIRE(!called2); // old h2 was destroyed
}

TEST_CASE("handler: move from heap-allocated lambda", "[handler]") {
    std::string large(100, 'B');
    bool called = false;
    handler h1([large, &called](ctx& c) { called = true; });
    handler h2(std::move(h1));

    REQUIRE(!static_cast<bool>(h1));
    REQUIRE(static_cast<bool>(h2));

    auto conn = make_conn();
    auto tr = make_request();
    route_match match{};
    ctx c(conn, tr.req, match);
    h2(c);
    REQUIRE(called);
}

TEST_CASE("handler: self-move-assignment is safe", "[handler]") {
    bool called = false;
    handler h([&called](ctx& c) { called = true; });
    auto* ptr = &h;
    *ptr = std::move(h); // self-move

    // After self-move, behavior is implementation-defined but should not crash
    // Our implementation handles this case (this != &other check)
    auto conn = make_conn();
    auto tr = make_request();
    route_match match{};
    ctx c(conn, tr.req, match);
    h(c);
    REQUIRE(called);
}

// ============================================================================
// Vector of handlers (middleware chain pattern)
// ============================================================================

TEST_CASE("handler: vector of handlers executes in order", "[handler]") {
    std::vector<int> order;
    std::vector<handler> chain;

    chain.emplace_back([&order](ctx& c) { order.push_back(1); c.next(); });
    chain.emplace_back([&order](ctx& c) { order.push_back(2); c.next(); });
    chain.emplace_back([&order](ctx& c) { order.push_back(3); c.text("done"); });

    auto conn = make_conn();
    auto tr = make_request();
    route_match match{};
    match.handlers = chain.data();
    match.handler_count = static_cast<std::uint8_t>(chain.size());
    ctx c(conn, tr.req, match);

    c.next();
    REQUIRE(order == std::vector<int>{1, 2, 3});
    REQUIRE(c.resp_body() == "done");
}
