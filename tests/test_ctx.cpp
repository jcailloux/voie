#include <catch2/catch_test_macros.hpp>
#include <voie/ctx.h>
#include "connection.h"
#include "http_parser.h"
#include "router.h"

#include <picohttpparser.h>

using namespace voie;
using namespace voie::detail;

// Helper: create a connection with a dummy fd
static connection make_conn() {
    return connection(-1, arena(4096));
}

// Helper: create a parsed_request from a raw HTTP request string
struct test_request {
    parsed_request req;
    phr_header headers[16]{};
};

static test_request make_request(const char* raw, std::size_t len) {
    test_request tr;
    auto result = parse_request(raw, len, 0, tr.headers, 16);
    REQUIRE(result.has_value());
    tr.req = result.value();
    return tr;
}

TEST_CASE("ctx: method and path", "[ctx]") {
    auto conn = make_conn();
    const char raw[] = "GET /hello HTTP/1.1\r\nHost: localhost\r\n\r\n";
    auto tr = make_request(raw, sizeof(raw) - 1);
    route_match match{};
    ctx c(conn, tr.req, match);

    REQUIRE(c.method() == "GET");
    REQUIRE(c.path() == "/hello");
}

TEST_CASE("ctx: route params", "[ctx]") {
    auto conn = make_conn();
    const char raw[] = "GET /users/42 HTTP/1.1\r\nHost: localhost\r\n\r\n";
    auto tr = make_request(raw, sizeof(raw) - 1);
    route_match match{};
    match.params[0] = {"id", "42"};
    match.param_count = 1;
    ctx c(conn, tr.req, match);

    REQUIRE(c.param("id") == "42");
    REQUIRE(c.param("missing").empty());
}

TEST_CASE("ctx: query string", "[ctx]") {
    auto conn = make_conn();
    const char raw[] = "GET /search?q=hello&page=2 HTTP/1.1\r\nHost: localhost\r\n\r\n";
    auto tr = make_request(raw, sizeof(raw) - 1);
    route_match match{};
    ctx c(conn, tr.req, match);

    REQUIRE(c.query("q") == "hello");
    REQUIRE(c.query("page") == "2");
    REQUIRE(c.query("missing").empty());
}

TEST_CASE("ctx: request headers", "[ctx]") {
    auto conn = make_conn();
    const char raw[] = "GET / HTTP/1.1\r\nHost: localhost\r\nAccept: application/json\r\n\r\n";
    auto tr = make_request(raw, sizeof(raw) - 1);
    route_match match{};
    ctx c(conn, tr.req, match);

    REQUIRE(c.header("Host") == "localhost");
    REQUIRE(c.header("Accept") == "application/json");
    // Case insensitive
    REQUIRE(c.header("host") == "localhost");
    REQUIRE(c.header("accept") == "application/json");
    REQUIRE(c.header("Missing").empty());
}

TEST_CASE("ctx: request body", "[ctx]") {
    auto conn = make_conn();
    const char raw[] = "POST /data HTTP/1.1\r\nHost: localhost\r\nContent-Length: 13\r\n\r\n{\"key\":\"val\"}";
    auto tr = make_request(raw, sizeof(raw) - 1);
    route_match match{};
    ctx c(conn, tr.req, match);

    REQUIRE(c.body() == "{\"key\":\"val\"}");
}

TEST_CASE("ctx: text response", "[ctx]") {
    auto conn = make_conn();
    const char raw[] = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
    auto tr = make_request(raw, sizeof(raw) - 1);
    route_match match{};
    ctx c(conn, tr.req, match);

    c.text("hello");
    REQUIRE(c.response_sent());
    REQUIRE(c.status_code() == 200);
    REQUIRE(c.resp_body() == "hello");
    REQUIRE(c.resp_content_type() == "text/plain; charset=utf-8");
}

TEST_CASE("ctx: json response", "[ctx]") {
    auto conn = make_conn();
    const char raw[] = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
    auto tr = make_request(raw, sizeof(raw) - 1);
    route_match match{};
    ctx c(conn, tr.req, match);

    c.json(R"({"ok": true})");
    REQUIRE(c.resp_body() == R"({"ok": true})");
    REQUIRE(c.resp_content_type() == "application/json");
}

TEST_CASE("ctx: html response", "[ctx]") {
    auto conn = make_conn();
    const char raw[] = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
    auto tr = make_request(raw, sizeof(raw) - 1);
    route_match match{};
    ctx c(conn, tr.req, match);

    c.html("<h1>Hi</h1>");
    REQUIRE(c.resp_body() == "<h1>Hi</h1>");
    REQUIRE(c.resp_content_type() == "text/html; charset=utf-8");
}

TEST_CASE("ctx: status code", "[ctx]") {
    auto conn = make_conn();
    const char raw[] = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
    auto tr = make_request(raw, sizeof(raw) - 1);
    route_match match{};
    ctx c(conn, tr.req, match);

    c.status(201).json(R"({"id": 1})");
    REQUIRE(c.status_code() == 201);
    REQUIRE(c.resp_body() == R"({"id": 1})");
}

TEST_CASE("ctx: response headers", "[ctx]") {
    auto conn = make_conn();
    const char raw[] = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
    auto tr = make_request(raw, sizeof(raw) - 1);
    route_match match{};
    ctx c(conn, tr.req, match);

    c.set("X-Req-Id", "abc").set("X-Custom", "val").text("ok");
    REQUIRE(c.resp_header_count() == 2);
    REQUIRE(c.resp_headers()[0].name == "X-Req-Id");
    REQUIRE(c.resp_headers()[0].value == "abc");
    REQUIRE(c.resp_headers()[1].name == "X-Custom");
    REQUIRE(c.resp_headers()[1].value == "val");
}

TEST_CASE("ctx: redirect", "[ctx]") {
    auto conn = make_conn();
    const char raw[] = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
    auto tr = make_request(raw, sizeof(raw) - 1);
    route_match match{};
    ctx c(conn, tr.req, match);

    c.redirect("/new", 301);
    REQUIRE(c.status_code() == 301);
    REQUIRE(c.resp_header_count() == 1);
    REQUIRE(c.resp_headers()[0].name == "Location");
    REQUIRE(c.resp_headers()[0].value == "/new");
}

TEST_CASE("ctx: send only once", "[ctx]") {
    auto conn = make_conn();
    const char raw[] = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
    auto tr = make_request(raw, sizeof(raw) - 1);
    route_match match{};
    ctx c(conn, tr.req, match);

    c.text("first");
    c.text("second");  // should be ignored
    REQUIRE(c.resp_body() == "first");
}

TEST_CASE("ctx: next() calls handler chain", "[ctx]") {
    auto conn = make_conn();
    const char raw[] = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
    auto tr = make_request(raw, sizeof(raw) - 1);

    int call_order = 0;
    handler chain[2] = {
        handler([&call_order](ctx& c) {
            REQUIRE(call_order == 0);
            call_order = 1;
            c.next();
        }),
        handler([&call_order](ctx& c) {
            REQUIRE(call_order == 1);
            call_order = 2;
            c.text("done");
        }),
    };

    route_match match{};
    match.handlers = chain;
    match.handler_count = 2;
    ctx c(conn, tr.req, match);

    c.next();
    REQUIRE(call_order == 2);
    REQUIRE(c.resp_body() == "done");
}

TEST_CASE("ctx: store and load", "[ctx]") {
    auto conn = make_conn();
    const char raw[] = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
    auto tr = make_request(raw, sizeof(raw) - 1);
    route_match match{};
    ctx c(conn, tr.req, match);

    int data = 42;
    c.store<int>("key", &data);
    auto* result = c.load<int>("key");
    REQUIRE(result == &data);
    REQUIRE(*result == 42);
    REQUIRE(c.load<int>("missing") == nullptr);
}

TEST_CASE("ctx: body survives temporary string", "[ctx]") {
    auto conn = make_conn();
    const char raw[] = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
    auto tr = make_request(raw, sizeof(raw) - 1);
    route_match match{};
    ctx c(conn, tr.req, match);

    {
        std::string temp = "temporary body content";
        c.json(temp);
    }
    // temp is destroyed but body survives (dup'd into arena)
    REQUIRE(c.resp_body() == "temporary body content");
}

TEST_CASE("ctx: middleware short-circuits without next()", "[ctx]") {
    auto conn = make_conn();
    const char raw[] = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
    auto tr = make_request(raw, sizeof(raw) - 1);

    bool handler_called = false;
    handler chain[2] = {
        handler([](ctx& c) {
            c.status(401).json(R"({"error":"unauthorized"})");
            // deliberately not calling c.next()
        }),
        handler([&handler_called](ctx& c) {
            handler_called = true;
            c.text("should not reach");
        }),
    };

    route_match match{};
    match.handlers = chain;
    match.handler_count = 2;
    ctx c(conn, tr.req, match);

    c.next();
    REQUIRE(c.status_code() == 401);
    REQUIRE(c.resp_body() == R"({"error":"unauthorized"})");
    REQUIRE_FALSE(handler_called);
}

TEST_CASE("ctx: next() past end of chain is no-op", "[ctx]") {
    auto conn = make_conn();
    const char raw[] = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
    auto tr = make_request(raw, sizeof(raw) - 1);

    handler chain[1] = {
        handler([](ctx& c) {
            c.text("done");
            c.next(); // past end — should be harmless
            c.next(); // again — still no-op
        }),
    };

    route_match match{};
    match.handlers = chain;
    match.handler_count = 1;
    ctx c(conn, tr.req, match);

    c.next();
    REQUIRE(c.resp_body() == "done");
}

TEST_CASE("ctx: empty body response", "[ctx]") {
    auto conn = make_conn();
    const char raw[] = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
    auto tr = make_request(raw, sizeof(raw) - 1);
    route_match match{};
    ctx c(conn, tr.req, match);

    c.status(204).send("", "");
    REQUIRE(c.response_sent());
    REQUIRE(c.status_code() == 204);
    REQUIRE(c.resp_body().empty());
}

TEST_CASE("ctx: default status is 200", "[ctx]") {
    auto conn = make_conn();
    const char raw[] = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
    auto tr = make_request(raw, sizeof(raw) - 1);
    route_match match{};
    ctx c(conn, tr.req, match);

    REQUIRE(c.status_code() == 200);
    REQUIRE_FALSE(c.response_sent());
}

TEST_CASE("ctx: store overwrites existing key", "[ctx]") {
    auto conn = make_conn();
    const char raw[] = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
    auto tr = make_request(raw, sizeof(raw) - 1);
    route_match match{};
    ctx c(conn, tr.req, match);

    int a = 1, b = 2;
    c.store<int>("key", &a);
    c.store<int>("key", &b);
    REQUIRE(c.load<int>("key") == &b);
}

// ============================================================================
// Overflow / saturation
// ============================================================================

TEST_CASE("ctx: store overflow is silent", "[ctx]") {
    auto conn = make_conn();
    const char raw[] = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
    auto tr = make_request(raw, sizeof(raw) - 1);
    route_match match{};
    ctx c(conn, tr.req, match);

    int values[9] = {};
    // Use string literals so string_views don't dangle
    c.store("k0", static_cast<void*>(&values[0]));
    c.store("k1", static_cast<void*>(&values[1]));
    c.store("k2", static_cast<void*>(&values[2]));
    c.store("k3", static_cast<void*>(&values[3]));
    c.store("k4", static_cast<void*>(&values[4]));
    c.store("k5", static_cast<void*>(&values[5]));
    c.store("k6", static_cast<void*>(&values[6]));
    c.store("k7", static_cast<void*>(&values[7]));
    // 9th store should be silently dropped
    c.store("overflow", static_cast<void*>(&values[8]));
    REQUIRE(c.load<int>("overflow") == nullptr);
    // Existing keys still work
    REQUIRE(c.load<int>("k0") == &values[0]);
}

TEST_CASE("ctx: set header overflow is silent", "[ctx]") {
    auto conn = make_conn();
    const char raw[] = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
    auto tr = make_request(raw, sizeof(raw) - 1);
    route_match match{};
    ctx c(conn, tr.req, match);

    // Fill all 16 header slots
    for (int i = 0; i < 16; ++i) {
        c.set("X-H-" + std::to_string(i), "v");
    }
    REQUIRE(c.resp_header_count() == 16);

    // 17th header should be silently dropped
    c.set("X-Overflow", "v");
    REQUIRE(c.resp_header_count() == 16);
}

// ============================================================================
// send_prebuilt
// ============================================================================

TEST_CASE("ctx: send_prebuilt marks response as sent and prebuilt", "[ctx]") {
    auto conn = make_conn();
    const char raw[] = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
    auto tr = make_request(raw, sizeof(raw) - 1);
    route_match match{};
    ctx c(conn, tr.req, match);

    const char data[] = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nOK";
    c.send_prebuilt(data, sizeof(data) - 1);
    REQUIRE(c.response_sent());
    REQUIRE(c.is_prebuilt());
}

TEST_CASE("ctx: send_prebuilt ignores second call", "[ctx]") {
    auto conn = make_conn();
    const char raw[] = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
    auto tr = make_request(raw, sizeof(raw) - 1);
    route_match match{};
    ctx c(conn, tr.req, match);

    const char data1[] = "first";
    const char data2[] = "second";
    c.send_prebuilt(data1, sizeof(data1) - 1);
    c.send_prebuilt(data2, sizeof(data2) - 1);
    REQUIRE(c.response_sent());
    // send_buf should point to first data
    REQUIRE(conn.send_buf() == data1);
}

TEST_CASE("ctx: send_prebuilt then text is ignored", "[ctx]") {
    auto conn = make_conn();
    const char raw[] = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
    auto tr = make_request(raw, sizeof(raw) - 1);
    route_match match{};
    ctx c(conn, tr.req, match);

    const char data[] = "prebuilt";
    c.send_prebuilt(data, sizeof(data) - 1);
    c.text("should be ignored");
    REQUIRE(c.is_prebuilt());
    REQUIRE(c.resp_body().empty()); // text() was ignored
}

TEST_CASE("ctx: text then send_prebuilt is ignored", "[ctx]") {
    auto conn = make_conn();
    const char raw[] = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
    auto tr = make_request(raw, sizeof(raw) - 1);
    route_match match{};
    ctx c(conn, tr.req, match);

    c.text("first");
    const char data[] = "prebuilt";
    c.send_prebuilt(data, sizeof(data) - 1);
    REQUIRE(!c.is_prebuilt());
    REQUIRE(c.resp_body() == "first");
}

// ============================================================================
// Query string edge cases
// ============================================================================

TEST_CASE("ctx: query with empty value", "[ctx]") {
    auto conn = make_conn();
    const char raw[] = "GET /search?q=&page=2 HTTP/1.1\r\nHost: localhost\r\n\r\n";
    auto tr = make_request(raw, sizeof(raw) - 1);
    route_match match{};
    ctx c(conn, tr.req, match);

    REQUIRE(c.query("q") == "");
    REQUIRE(c.query("page") == "2");
}

TEST_CASE("ctx: query with no equals sign returns empty", "[ctx]") {
    auto conn = make_conn();
    const char raw[] = "GET /search?verbose HTTP/1.1\r\nHost: localhost\r\n\r\n";
    auto tr = make_request(raw, sizeof(raw) - 1);
    route_match match{};
    ctx c(conn, tr.req, match);

    // Key exists but with no '=' — returns empty string_view
    REQUIRE(c.query("verbose").empty());
    // Non-existent key also returns empty
    REQUIRE(c.query("missing").empty());
}

TEST_CASE("ctx: query with consecutive ampersands", "[ctx]") {
    auto conn = make_conn();
    const char raw[] = "GET /search?a=1&&b=2 HTTP/1.1\r\nHost: localhost\r\n\r\n";
    auto tr = make_request(raw, sizeof(raw) - 1);
    route_match match{};
    ctx c(conn, tr.req, match);

    REQUIRE(c.query("a") == "1");
    REQUIRE(c.query("b") == "2");
}

TEST_CASE("ctx: query with equals in value", "[ctx]") {
    auto conn = make_conn();
    const char raw[] = "GET /search?expr=a%3Db HTTP/1.1\r\nHost: localhost\r\n\r\n";
    auto tr = make_request(raw, sizeof(raw) - 1);
    route_match match{};
    ctx c(conn, tr.req, match);

    REQUIRE(c.query("expr") == "a=b");
}

TEST_CASE("ctx: no query string", "[ctx]") {
    auto conn = make_conn();
    const char raw[] = "GET /path HTTP/1.1\r\nHost: localhost\r\n\r\n";
    auto tr = make_request(raw, sizeof(raw) - 1);
    route_match match{};
    ctx c(conn, tr.req, match);

    REQUIRE(c.query("anything").empty());
}

// ============================================================================
// Redirect
// ============================================================================

TEST_CASE("ctx: redirect default code is 302", "[ctx]") {
    auto conn = make_conn();
    const char raw[] = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
    auto tr = make_request(raw, sizeof(raw) - 1);
    route_match match{};
    ctx c(conn, tr.req, match);

    c.redirect("/login");
    REQUIRE(c.status_code() == 302);
    REQUIRE(c.resp_headers()[0].name == "Location");
    REQUIRE(c.resp_headers()[0].value == "/login");
}

// ============================================================================
// Header lookup
// ============================================================================

TEST_CASE("ctx: header case insensitive mixed case", "[ctx]") {
    auto conn = make_conn();
    const char raw[] = "GET / HTTP/1.1\r\nContent-Type: text/html\r\n\r\n";
    auto tr = make_request(raw, sizeof(raw) - 1);
    route_match match{};
    ctx c(conn, tr.req, match);

    REQUIRE(c.header("content-type") == "text/html");
    REQUIRE(c.header("CONTENT-TYPE") == "text/html");
    REQUIRE(c.header("Content-Type") == "text/html");
    REQUIRE(c.header("Content-type") == "text/html");
}

// ============================================================================
// no_content()
// ============================================================================

TEST_CASE("ctx: no_content() basic", "[ctx]") {
    auto conn = make_conn();
    const char raw[] = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
    auto tr = make_request(raw, sizeof(raw) - 1);
    route_match match{};
    ctx c(conn, tr.req, match);

    c.no_content();
    REQUIRE(c.status_code() == 204);
    REQUIRE(c.resp_body().empty());
    REQUIRE(c.resp_content_type().empty());
    REQUIRE(c.response_sent());
}

TEST_CASE("ctx: no_content() after send is no-op", "[ctx]") {
    auto conn = make_conn();
    const char raw[] = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
    auto tr = make_request(raw, sizeof(raw) - 1);
    route_match match{};
    ctx c(conn, tr.req, match);

    c.text("ok");
    c.no_content();
    REQUIRE(c.status_code() == 200);
    REQUIRE(c.resp_body() == "ok");
}

// ============================================================================
// request_header_count() / request_header_at()
// ============================================================================

TEST_CASE("ctx: request_header_count()", "[ctx]") {
    auto conn = make_conn();
    const char raw[] = "GET / HTTP/1.1\r\nHost: localhost\r\nAccept: */*\r\nX-Foo: bar\r\n\r\n";
    auto tr = make_request(raw, sizeof(raw) - 1);
    route_match match{};
    ctx c(conn, tr.req, match);

    REQUIRE(c.request_header_count() == 3);
}

TEST_CASE("ctx: request_header_at() valid indices", "[ctx]") {
    auto conn = make_conn();
    const char raw[] = "GET / HTTP/1.1\r\nHost: localhost\r\nX-Foo: bar\r\n\r\n";
    auto tr = make_request(raw, sizeof(raw) - 1);
    route_match match{};
    ctx c(conn, tr.req, match);

    auto [name0, value0] = c.request_header_at(0);
    REQUIRE(name0 == "Host");
    REQUIRE(value0 == "localhost");

    auto [name1, value1] = c.request_header_at(1);
    REQUIRE(name1 == "X-Foo");
    REQUIRE(value1 == "bar");
}

TEST_CASE("ctx: request_header_at() out of bounds", "[ctx]") {
    auto conn = make_conn();
    const char raw[] = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
    auto tr = make_request(raw, sizeof(raw) - 1);
    route_match match{};
    ctx c(conn, tr.req, match);

    auto [name, value] = c.request_header_at(99);
    REQUIRE(name.empty());
    REQUIRE(value.empty());
}

// ============================================================================
// set() overwrite case-insensitive
// ============================================================================

TEST_CASE("ctx: set() overwrites case-insensitively", "[ctx]") {
    auto conn = make_conn();
    const char raw[] = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
    auto tr = make_request(raw, sizeof(raw) - 1);
    route_match match{};
    ctx c(conn, tr.req, match);

    c.set("X-Foo", "a");
    c.set("x-foo", "b");
    REQUIRE(c.resp_header_count() == 1);
    REQUIRE(c.resp_headers()[0].value == "b");
}

// ============================================================================
// query() edge cases with URL decoding
// ============================================================================

TEST_CASE("ctx: query() returns first occurrence for duplicate keys", "[ctx]") {
    auto conn = make_conn();
    const char raw[] = "GET /search?a=1&a=2 HTTP/1.1\r\nHost: localhost\r\n\r\n";
    auto tr = make_request(raw, sizeof(raw) - 1);
    route_match match{};
    ctx c(conn, tr.req, match);

    REQUIRE(c.query("a") == "1");
}

// ============================================================================
// has_header()
// ============================================================================

TEST_CASE("ctx: has_header() case-insensitive", "[ctx]") {
    auto conn = make_conn();
    const char raw[] = "GET / HTTP/1.1\r\nContent-Type: text/html\r\nHost: localhost\r\n\r\n";
    auto tr = make_request(raw, sizeof(raw) - 1);
    route_match match{};
    ctx c(conn, tr.req, match);

    REQUIRE(c.has_header("Content-Type"));
    REQUIRE(c.has_header("content-type"));
    REQUIRE(c.has_header("CONTENT-TYPE"));
    REQUIRE(c.has_header("Host"));
    REQUIRE_FALSE(c.has_header("X-Missing"));
}
