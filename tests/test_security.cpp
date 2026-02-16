#include <catch2/catch_test_macros.hpp>
#include <voie/ctx.h>
#include <voie/types.h>
#include "connection.h"
#include "http_parser.h"
#include "router.h"

#include <picohttpparser.h>
#include <cstring>
#include <string>

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

// ============================================================================
// CRLF injection in response headers
// ============================================================================

TEST_CASE("security: CRLF in header name is rejected", "[security]") {
    auto conn = make_conn();
    const char raw[] = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
    auto tr = make_request(raw, sizeof(raw) - 1);
    route_match match{};
    ctx c(conn, tr.req, match);

    c.set("X\r\nEvil", "value");
    REQUIRE(c.resp_header_count() == 0);
}

TEST_CASE("security: CRLF in header value is rejected", "[security]") {
    auto conn = make_conn();
    const char raw[] = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
    auto tr = make_request(raw, sizeof(raw) - 1);
    route_match match{};
    ctx c(conn, tr.req, match);

    c.set("X-Header", "value\r\nEvil: yes");
    REQUIRE(c.resp_header_count() == 0);
}

TEST_CASE("security: null byte in header value is rejected", "[security]") {
    auto conn = make_conn();
    const char raw[] = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
    auto tr = make_request(raw, sizeof(raw) - 1);
    route_match match{};
    ctx c(conn, tr.req, match);

    c.set("X-Header", std::string_view("val\0ue", 6));
    REQUIRE(c.resp_header_count() == 0);
}

TEST_CASE("security: invalid header names are rejected", "[security]") {
    auto conn = make_conn();
    const char raw[] = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
    auto tr = make_request(raw, sizeof(raw) - 1);
    route_match match{};
    ctx c(conn, tr.req, match);

    // Empty name
    c.set("", "value");
    REQUIRE(c.resp_header_count() == 0);

    // Space in name
    c.set("X Header", "value");
    REQUIRE(c.resp_header_count() == 0);

    // Colon in name
    c.set("X:Header", "value");
    REQUIRE(c.resp_header_count() == 0);
}

TEST_CASE("security: valid header names are accepted", "[security]") {
    auto conn = make_conn();
    const char raw[] = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
    auto tr = make_request(raw, sizeof(raw) - 1);
    route_match match{};
    ctx c(conn, tr.req, match);

    c.set("X-Custom", "val1");
    c.set("X_Under", "val2");
    REQUIRE(c.resp_header_count() == 2);
}

// ============================================================================
// Unknown HTTP method
// ============================================================================

TEST_CASE("security: unknown HTTP method returns nullopt", "[security]") {
    REQUIRE(!string_to_method("PURGE").has_value());
    REQUIRE(!string_to_method("CONNECT").has_value());
    REQUIRE(!string_to_method("").has_value());
    REQUIRE(!string_to_method("get").has_value());  // case-sensitive
}

TEST_CASE("security: known HTTP methods return correct value", "[security]") {
    REQUIRE(string_to_method("GET") == http_method::GET);
    REQUIRE(string_to_method("POST") == http_method::POST);
    REQUIRE(string_to_method("PUT") == http_method::PUT);
    REQUIRE(string_to_method("DELETE") == http_method::DELETE);
    REQUIRE(string_to_method("PATCH") == http_method::PATCH);
    REQUIRE(string_to_method("HEAD") == http_method::HEAD);
    REQUIRE(string_to_method("OPTIONS") == http_method::OPTIONS);
}

// ============================================================================
// Content-Length validation
// ============================================================================

TEST_CASE("security: Content-Length incomplete body", "[security]") {
    const char* req =
        "POST /data HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Content-Length: 100\r\n"
        "\r\n"
        "short body";
    phr_header headers[16];
    auto result = parse_request(req, std::strlen(req), 0, headers, 16);
    REQUIRE(!result.has_value());
    REQUIRE(result.error() == parse_error::incomplete);
}

TEST_CASE("security: Content-Length too large", "[security]") {
    const char* req =
        "POST /data HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Content-Length: 2000\r\n"
        "\r\n";
    phr_header headers[16];
    auto result = parse_request(req, std::strlen(req), 0, headers, 16, 1024);
    REQUIRE(!result.has_value());
    REQUIRE(result.error() == parse_error::too_large);
}

TEST_CASE("security: Content-Length complete body", "[security]") {
    const char* req =
        "POST /data HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Content-Length: 5\r\n"
        "\r\n"
        "hello";
    phr_header headers[16];
    auto result = parse_request(req, std::strlen(req), 0, headers, 16);
    REQUIRE(result.has_value());
    REQUIRE(result.value().body == "hello");
    REQUIRE(result.value().body.size() == 5);
}

TEST_CASE("security: Content-Length integer overflow", "[security]") {
    const char* req =
        "POST /data HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Content-Length: 99999999999999999999\r\n"
        "\r\n";
    phr_header headers[16];
    auto result = parse_request(req, std::strlen(req), 0, headers, 16);
    REQUIRE(!result.has_value());
    REQUIRE(result.error() == parse_error::bad_request);
}

TEST_CASE("security: double Content-Length (CL-CL smuggling)", "[security]") {
    const char* req =
        "POST /data HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Content-Length: 5\r\n"
        "Content-Length: 10\r\n"
        "\r\n"
        "hello";
    phr_header headers[16];
    auto result = parse_request(req, std::strlen(req), 0, headers, 16);
    REQUIRE(!result.has_value());
    REQUIRE(result.error() == parse_error::bad_request);
}

TEST_CASE("security: negative Content-Length", "[security]") {
    const char* req =
        "POST /data HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Content-Length: -1\r\n"
        "\r\n";
    phr_header headers[16];
    auto result = parse_request(req, std::strlen(req), 0, headers, 16);
    REQUIRE(!result.has_value());
    REQUIRE(result.error() == parse_error::bad_request);
}

TEST_CASE("security: Transfer-Encoding chunked is rejected", "[security]") {
    const char* req =
        "POST /data HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "5\r\nhello\r\n0\r\n\r\n";
    phr_header headers[16];
    auto result = parse_request(req, std::strlen(req), 0, headers, 16);
    REQUIRE(!result.has_value());
    REQUIRE(result.error() == parse_error::bad_request);
}

// ============================================================================
// Path normalization
// ============================================================================

TEST_CASE("security: normalize collapses //", "[security]") {
    char path[] = "//foo//bar";
    auto result = normalize_path_inplace(path, std::strlen(path));
    REQUIRE(result == "/foo/bar");
}

TEST_CASE("security: normalize resolves . and ..", "[security]") {
    {
        char path[] = "/foo/./bar";
        auto result = normalize_path_inplace(path, std::strlen(path));
        REQUIRE(result == "/foo/bar");
    }
    {
        char path[] = "/foo/../bar";
        auto result = normalize_path_inplace(path, std::strlen(path));
        REQUIRE(result == "/bar");
    }
}

TEST_CASE("security: path traversal above root is rejected", "[security]") {
    char path[] = "/foo/../../bar";
    auto result = normalize_path_inplace(path, std::strlen(path));
    REQUIRE(result.empty());
}

TEST_CASE("security: root stays root", "[security]") {
    char path[] = "/";
    auto result = normalize_path_inplace(path, 1);
    REQUIRE(result == "/");
}

// ============================================================================
// URL decoding
// ============================================================================

TEST_CASE("security: URL decode basic", "[security]") {
    {
        char buf[] = "hello%20world";
        auto len = url_decode_inplace(buf, std::strlen(buf));
        REQUIRE(std::string_view(buf, len) == "hello world");
    }
    {
        char buf[] = "%2F";
        auto len = url_decode_inplace(buf, std::strlen(buf));
        REQUIRE(std::string_view(buf, len) == "/");
    }
    {
        char buf[] = "hello+world";
        auto len = url_decode_inplace(buf, std::strlen(buf));
        REQUIRE(std::string_view(buf, len) == "hello world");
    }
}

TEST_CASE("security: URL decode invalid sequences pass through", "[security]") {
    char buf[] = "%ZZ";
    auto len = url_decode_inplace(buf, std::strlen(buf));
    REQUIRE(std::string_view(buf, len) == "%ZZ");
}

TEST_CASE("security: null byte in URL path is rejected", "[security]") {
    char buf[] = "foo%00bar";
    bool null_found = false;
    auto len = url_decode_inplace(buf, std::strlen(buf), &null_found);
    REQUIRE(null_found);
    REQUIRE(len == 0);
}

// ============================================================================
// Header overwrite
// ============================================================================

TEST_CASE("security: set() overwrites existing header", "[security]") {
    auto conn = make_conn();
    const char raw[] = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
    auto tr = make_request(raw, sizeof(raw) - 1);
    route_match match{};
    ctx c(conn, tr.req, match);

    c.set("X-Key", "a");
    c.set("X-Key", "b");
    REQUIRE(c.resp_header_count() == 1);
    REQUIRE(c.resp_headers()[0].value == "b");
}

// ============================================================================
// Long URL
// ============================================================================

TEST_CASE("security: very long URL overflows 4096 buffer", "[security]") {
    // Build a request whose request line alone exceeds 4096 bytes
    std::string long_path(4100, 'a');
    std::string req = "GET /" + long_path + " HTTP/1.1\r\nHost: localhost\r\n\r\n";
    // Only provide first 4096 bytes (truncated — request line incomplete)
    phr_header headers[16];
    auto result = parse_request(req.data(), 4096, 0, headers, 16);
    REQUIRE(!result.has_value());
    REQUIRE(result.error() == parse_error::incomplete);
}
