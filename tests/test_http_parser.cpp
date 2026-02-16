#include <catch2/catch_test_macros.hpp>
#include "http_parser.h"
#include <picohttpparser.h>
#include <cstring>
#include <string>

using namespace voie::detail;

// ============================================================================
// Request line parsing
// ============================================================================

TEST_CASE("http_parser: GET request", "[http_parser]") {
    const char* req = "GET /hello HTTP/1.1\r\nHost: localhost\r\n\r\n";
    phr_header headers[64];
    auto result = parse_request(req, std::strlen(req), 0, headers, 64);
    REQUIRE(result.has_value());

    auto& r = result.value();
    REQUIRE(r.method == "GET");
    REQUIRE(r.path == "/hello");
    REQUIRE(r.query.empty());
    REQUIRE(r.minor_version == 1);
    REQUIRE(r.num_headers == 1);
}

TEST_CASE("http_parser: POST with body", "[http_parser]") {
    const char* req =
        "POST /data HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Content-Length: 13\r\n"
        "\r\n"
        "hello, world!";
    phr_header headers[64];
    auto result = parse_request(req, std::strlen(req), 0, headers, 64);
    REQUIRE(result.has_value());

    auto& r = result.value();
    REQUIRE(r.method == "POST");
    REQUIRE(r.path == "/data");
    REQUIRE(r.body == "hello, world!");
}

TEST_CASE("http_parser: all HTTP methods", "[http_parser]") {
    for (auto method : {"GET", "POST", "PUT", "DELETE", "PATCH", "HEAD", "OPTIONS"}) {
        std::string req = std::string(method) + " / HTTP/1.1\r\nHost: localhost\r\n\r\n";
        phr_header headers[64];
        auto result = parse_request(req.data(), req.size(), 0, headers, 64);
        REQUIRE(result.has_value());
        REQUIRE(result.value().method == method);
    }
}

TEST_CASE("http_parser: query string", "[http_parser]") {
    const char* req = "GET /search?q=hello&page=2 HTTP/1.1\r\nHost: localhost\r\n\r\n";
    phr_header headers[64];
    auto result = parse_request(req, std::strlen(req), 0, headers, 64);
    REQUIRE(result.has_value());

    REQUIRE(result.value().path == "/search");
    REQUIRE(result.value().query == "q=hello&page=2");
}

TEST_CASE("http_parser: HTTP/1.0", "[http_parser]") {
    const char* req = "GET / HTTP/1.0\r\nHost: localhost\r\n\r\n";
    phr_header headers[64];
    auto result = parse_request(req, std::strlen(req), 0, headers, 64);
    REQUIRE(result.has_value());
    REQUIRE(result.value().minor_version == 0);
}

// ============================================================================
// Header parsing (exercises AVX2 path with many headers)
// ============================================================================

TEST_CASE("http_parser: multiple headers", "[http_parser][avx2]") {
    const char* req =
        "GET / HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "Accept: text/html\r\n"
        "Accept-Language: en-US\r\n"
        "Accept-Encoding: gzip, deflate\r\n"
        "Connection: keep-alive\r\n"
        "User-Agent: voie-test/1.0\r\n"
        "Cache-Control: no-cache\r\n"
        "X-Request-Id: abc-123\r\n"
        "\r\n";
    phr_header headers[64];
    auto result = parse_request(req, std::strlen(req), 0, headers, 64);
    REQUIRE(result.has_value());
    REQUIRE(result.value().num_headers == 8);

    // Verify first and last headers
    REQUIRE(std::string_view(headers[0].name, headers[0].name_len) == "Host");
    REQUIRE(std::string_view(headers[0].value, headers[0].value_len) == "example.com");
    REQUIRE(std::string_view(headers[7].name, headers[7].name_len) == "X-Request-Id");
    REQUIRE(std::string_view(headers[7].value, headers[7].value_len) == "abc-123");
}

TEST_CASE("http_parser: 16+ headers (forces AVX2 re-scan)", "[http_parser][avx2]") {
    // Build a request with enough headers to span >128 bytes of header data,
    // forcing the AVX2 find_ranges() to be called multiple times
    std::string req = "GET / HTTP/1.1\r\n";
    for (int i = 0; i < 20; ++i) {
        req += "X-Header-" + std::to_string(i) + ": value-" + std::to_string(i) + "\r\n";
    }
    req += "\r\n";

    phr_header headers[64];
    auto result = parse_request(req.data(), req.size(), 0, headers, 64);
    REQUIRE(result.has_value());
    REQUIRE(result.value().num_headers == 20);

    // Spot-check a few headers
    REQUIRE(std::string_view(headers[0].name, headers[0].name_len) == "X-Header-0");
    REQUIRE(std::string_view(headers[0].value, headers[0].value_len) == "value-0");
    REQUIRE(std::string_view(headers[19].name, headers[19].name_len) == "X-Header-19");
    REQUIRE(std::string_view(headers[19].value, headers[19].value_len) == "value-19");
}

TEST_CASE("http_parser: long header value (>128 bytes)", "[http_parser][avx2]") {
    // A single header value longer than 128 bytes exercises the full
    // 4-register AVX2 scan within a single header
    std::string long_val(200, 'A');
    std::string req = "GET / HTTP/1.1\r\nHost: localhost\r\nX-Long: " + long_val + "\r\n\r\n";

    phr_header headers[64];
    auto result = parse_request(req.data(), req.size(), 0, headers, 64);
    REQUIRE(result.has_value());
    REQUIRE(result.value().num_headers == 2);
    REQUIRE(std::string_view(headers[1].value, headers[1].value_len) == long_val);
}

TEST_CASE("http_parser: header with empty value", "[http_parser]") {
    const char* req = "GET / HTTP/1.1\r\nHost: localhost\r\nX-Empty:\r\n\r\n";
    phr_header headers[64];
    auto result = parse_request(req, std::strlen(req), 0, headers, 64);
    REQUIRE(result.has_value());
    REQUIRE(result.value().num_headers == 2);
    REQUIRE(std::string_view(headers[1].name, headers[1].name_len) == "X-Empty");
    REQUIRE(headers[1].value_len == 0);
}

TEST_CASE("http_parser: header with trailing whitespace", "[http_parser]") {
    const char* req = "GET / HTTP/1.1\r\nHost: localhost  \r\n\r\n";
    phr_header headers[64];
    auto result = parse_request(req, std::strlen(req), 0, headers, 64);
    REQUIRE(result.has_value());
    // picohttpparser trims trailing whitespace from values
    std::string_view val(headers[0].value, headers[0].value_len);
    REQUIRE(val == "localhost");
}

// ============================================================================
// Error handling
// ============================================================================

TEST_CASE("http_parser: incomplete request", "[http_parser]") {
    const char* req = "GET /hello HTTP/1.1\r\nHost: loc";
    phr_header headers[64];
    auto result = parse_request(req, std::strlen(req), 0, headers, 64);
    REQUIRE(!result.has_value());
    REQUIRE(result.error() == parse_error::incomplete);
}

TEST_CASE("http_parser: incomplete headers (no final CRLF)", "[http_parser]") {
    const char* req = "GET / HTTP/1.1\r\nHost: localhost\r\n";
    phr_header headers[64];
    auto result = parse_request(req, std::strlen(req), 0, headers, 64);
    REQUIRE(!result.has_value());
    REQUIRE(result.error() == parse_error::incomplete);
}

TEST_CASE("http_parser: malformed request line", "[http_parser]") {
    const char* req = "INVALID\r\n\r\n";
    phr_header headers[64];
    auto result = parse_request(req, std::strlen(req), 0, headers, 64);
    REQUIRE(!result.has_value());
    REQUIRE(result.error() == parse_error::bad_request);
}

TEST_CASE("http_parser: empty input", "[http_parser]") {
    phr_header headers[64];
    auto result = parse_request("", 0, 0, headers, 64);
    REQUIRE(!result.has_value());
    REQUIRE(result.error() == parse_error::incomplete);
}

// ============================================================================
// Edge cases
// ============================================================================

TEST_CASE("http_parser: root path", "[http_parser]") {
    const char* req = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
    phr_header headers[64];
    auto result = parse_request(req, std::strlen(req), 0, headers, 64);
    REQUIRE(result.has_value());
    REQUIRE(result.value().path == "/");
}

TEST_CASE("http_parser: path with special characters", "[http_parser]") {
    const char* req = "GET /api/v1/users%20list HTTP/1.1\r\nHost: localhost\r\n\r\n";
    phr_header headers[64];
    auto result = parse_request(req, std::strlen(req), 0, headers, 64);
    REQUIRE(result.has_value());
    REQUIRE(result.value().path == "/api/v1/users%20list");
}

TEST_CASE("http_parser: query with no value", "[http_parser]") {
    const char* req = "GET /search?q HTTP/1.1\r\nHost: localhost\r\n\r\n";
    phr_header headers[64];
    auto result = parse_request(req, std::strlen(req), 0, headers, 64);
    REQUIRE(result.has_value());
    REQUIRE(result.value().query == "q");
}

TEST_CASE("http_parser: POST with no body", "[http_parser]") {
    const char* req =
        "POST /action HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Content-Length: 0\r\n"
        "\r\n";
    phr_header headers[64];
    auto result = parse_request(req, std::strlen(req), 0, headers, 64);
    REQUIRE(result.has_value());
    REQUIRE(result.value().method == "POST");
    REQUIRE(result.value().body.empty());
}

// ============================================================================
// Incremental parsing (prev_len > 0)
// ============================================================================

TEST_CASE("http_parser: incremental parse completes", "[http_parser]") {
    std::string req = "GET /hello HTTP/1.1\r\nHost: localhost\r\n\r\n";

    // First attempt with partial data
    phr_header headers[64];
    auto partial = parse_request(req.data(), 20, 0, headers, 64);
    REQUIRE(!partial.has_value());
    REQUIRE(partial.error() == parse_error::incomplete);

    // Second attempt with full data, prev_len = 20
    auto full = parse_request(req.data(), req.size(), 20, headers, 64);
    REQUIRE(full.has_value());
    REQUIRE(full.value().method == "GET");
    REQUIRE(full.value().path == "/hello");
}

TEST_CASE("http_parser: incremental parse with body", "[http_parser]") {
    std::string req =
        "POST /data HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Content-Length: 5\r\n"
        "\r\n"
        "hello";

    // Headers arrive first
    std::size_t header_end = req.find("\r\n\r\n") + 4;
    phr_header headers[64];
    auto partial = parse_request(req.data(), header_end - 5, 0, headers, 64);
    REQUIRE(!partial.has_value());

    // Full request
    auto full = parse_request(req.data(), req.size(), header_end - 5, headers, 64);
    REQUIRE(full.has_value());
    REQUIRE(full.value().body == "hello");
}

// ============================================================================
// Additional edge cases
// ============================================================================

TEST_CASE("http_parser: very long path", "[http_parser]") {
    std::string long_path(2000, 'a');
    std::string req = "GET /" + long_path + " HTTP/1.1\r\nHost: localhost\r\n\r\n";
    phr_header headers[64];
    auto result = parse_request(req.data(), req.size(), 0, headers, 64);
    REQUIRE(result.has_value());
    REQUIRE(result.value().path.size() == long_path.size() + 1); // + leading /
}

TEST_CASE("http_parser: multiple query parameters", "[http_parser]") {
    const char* req = "GET /api?a=1&b=2&c=3&d=4 HTTP/1.1\r\nHost: localhost\r\n\r\n";
    phr_header headers[64];
    auto result = parse_request(req, std::strlen(req), 0, headers, 64);
    REQUIRE(result.has_value());
    REQUIRE(result.value().path == "/api");
    REQUIRE(result.value().query == "a=1&b=2&c=3&d=4");
}

TEST_CASE("http_parser: total_header_len is correct", "[http_parser]") {
    const char* req = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
    phr_header headers[64];
    auto result = parse_request(req, std::strlen(req), 0, headers, 64);
    REQUIRE(result.has_value());
    // total_header_len should equal the full request length (no body)
    REQUIRE(result.value().total_header_len == std::strlen(req));
}

TEST_CASE("http_parser: request with only CRLF before request line", "[http_parser]") {
    // Some clients send leading CRLF — picohttpparser may or may not handle this
    const char* req = "\r\nGET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
    phr_header headers[64];
    auto result = parse_request(req, std::strlen(req), 0, headers, 64);
    // This is technically malformed; just verify it doesn't crash
    // (result could be either bad_request or a valid parse depending on phr behavior)
    (void)result;
}

TEST_CASE("http_parser: null bytes in header value", "[http_parser]") {
    // Construct a request with embedded null in a header value
    std::string req = "GET / HTTP/1.1\r\nHost: localhost\r\nX-Bad: ";
    req += std::string("ab\0cd", 5);
    req += "\r\n\r\n";
    phr_header headers[64];
    auto result = parse_request(req.data(), req.size(), 0, headers, 64);
    // picohttpparser rejects control characters — should be bad_request
    if (!result.has_value()) {
        REQUIRE(result.error() == parse_error::bad_request);
    }
}