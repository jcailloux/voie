#pragma once

#include <voie/types.h>
#include <voie/handler.h>

#include <cstdint>
#include <string_view>
#include <utility>

namespace voie {

namespace detail {
class io_loop;
struct parsed_request;
struct route_match;
class connection;
} // namespace detail

/// Per-request context providing access to the HTTP request and response.
///
/// A `ctx` is created by the framework for each incoming request and passed
/// to the handler chain.  It is only valid for the duration of that chain;
/// do not store references to it.
///
/// Response-building methods (`status`, `set`) are chainable.  Terminal
/// response methods (`text`, `json`, `html`, `send`, `no_content`,
/// `redirect`) finalize the response — only the first call takes effect.
class ctx {
public:
    // -- Request access ------------------------------------------------------

    /// @return The HTTP method string (e.g. `"GET"`, `"POST"`).
    [[nodiscard]] std::string_view method() const noexcept;

    /// @return The request path after normalization and percent-decoding.
    [[nodiscard]] std::string_view path() const noexcept;

    /// Retrieve a named route parameter.
    /// For a route pattern `"/users/:id"` and path `"/users/42"`,
    /// `param("id")` returns `"42"`.
    /// @param name  Parameter name without the leading `:` or `*`.
    /// @return The captured value, or an empty view if not found.
    [[nodiscard]] std::string_view param(std::string_view name) const noexcept;

    /// Retrieve a query-string parameter (URL-decoded).
    /// For `"/search?q=hello%20world"`, `query("q")` returns `"hello world"`.
    /// @param name  Parameter name.
    /// @return The decoded value, or an empty view if not found.
    [[nodiscard]] std::string_view query(std::string_view name) noexcept;

    /// Retrieve a request header value (case-insensitive lookup).
    /// @param name  Header name (e.g. `"Content-Type"`).
    /// @return The header value, or an empty view if not present.
    [[nodiscard]] std::string_view header(std::string_view name) const noexcept;

    /// Test whether a request header is present (case-insensitive).
    /// @param name  Header name.
    [[nodiscard]] bool has_header(std::string_view name) const noexcept;

    /// @return The raw request body, or an empty view if none.
    [[nodiscard]] std::string_view body() const noexcept;

    // -- Request header iteration --------------------------------------------

    /// @return Number of headers in the request.
    [[nodiscard]] std::size_t request_header_count() const noexcept;

    /// Access a request header by index.
    /// @param index  Zero-based header index.
    /// @return `{name, value}` pair, or `{{}, {}}` if @p index is out of range.
    [[nodiscard]] std::pair<std::string_view, std::string_view>
        request_header_at(std::size_t index) const noexcept;

    // -- Response building (chainable) ---------------------------------------

    /// Set the HTTP response status code.
    /// @param code  HTTP status code (e.g. 200, 404).  Default is 200.
    /// @return `*this` for chaining.
    ctx& status(int code) noexcept;

    /// Add or overwrite a response header.
    /// Names and values are validated per RFC 7230; invalid entries are
    /// silently ignored.  Lookup is case-insensitive; at most 16 custom
    /// response headers are supported.
    /// @param name   Header name.
    /// @param value  Header value.
    /// @return `*this` for chaining.
    ctx& set(std::string_view name, std::string_view value);

    // -- Terminal response methods -------------------------------------------

    /// Send a `text/plain; charset=utf-8` response.
    /// @param body  Response body.
    void text(std::string_view body);

    /// Send an `application/json` response.
    /// @param body  Response body (caller must provide valid JSON).
    void json(std::string_view body);

    /// Send a `text/html; charset=utf-8` response.
    /// @param body  Response body.
    void html(std::string_view body);

    /// Send a response with an explicit Content-Type.
    /// For `text/*` types, `; charset=utf-8` is appended automatically
    /// unless the type already contains `charset`.
    /// @param body          Response body.
    /// @param content_type  MIME type (e.g. `"image/png"`).
    void send(std::string_view body, std::string_view content_type);

    /// Send a `204 No Content` response with no body.
    void no_content();

    /// Send a redirect response.
    /// @param location  Target URL.
    /// @param code      HTTP redirect status code (default 302).
    void redirect(std::string_view location, int code = 302);

    // -- Pre-built response --------------------------------------------------

    /// Send a pre-built raw HTTP response (zero-copy).
    /// The data must be a complete HTTP response including the status line and
    /// headers.  See voie::prebuilt() for a convenient helper.
    /// @param data  Pointer to the response bytes.
    /// @param len   Length in bytes.
    void send_prebuilt(const char* data, std::size_t len);

    // -- Middleware flow ------------------------------------------------------

    /// Invoke the next handler in the middleware chain.
    /// Must be called by each middleware; otherwise subsequent handlers
    /// (and the final route handler) are skipped.
    void next();

    // -- Context storage -----------------------------------------------------

    /// Store an arbitrary pointer under a key for later retrieval.
    /// The caller is responsible for the lifetime of the pointed-to object.
    /// At most 8 entries are supported; excess stores are silently dropped.
    /// @param key    Lookup key (must remain valid for the request duration).
    /// @param value  Pointer to store.
    void  store(std::string_view key, void* value) noexcept;

    /// Retrieve a pointer previously saved with store().
    /// @param key  Lookup key.
    /// @return The stored pointer, or `nullptr` if not found.
    void* load(std::string_view key) const noexcept;

    /// Type-safe convenience wrapper around store(key, void*).
    template <typename T>
    void store(std::string_view key, T* value) noexcept {
        store(key, static_cast<void*>(value));
    }

    /// Type-safe convenience wrapper around load(key).
    /// @tparam T  Expected pointee type.
    /// @return Typed pointer, or `nullptr` if not found.
    template <typename T>
    [[nodiscard]] T* load(std::string_view key) const noexcept {
        return static_cast<T*>(load(key));
    }

    // -- Internal state access (used by io_loop) -----------------------------

    /// @return The current response status code.
    [[nodiscard]] int status_code() const noexcept { return status_code_; }
    /// @return `true` if a terminal response method has been called.
    [[nodiscard]] bool response_sent() const noexcept { return response_sent_; }
    /// @return `true` if the response was sent via send_prebuilt().
    [[nodiscard]] bool is_prebuilt() const noexcept { return prebuilt_; }

    /// Response header entry.
    struct resp_header {
        std::string_view name;   ///< Header name.
        std::string_view value;  ///< Header value.
    };
    /// @return Pointer to the response header array.
    [[nodiscard]] const resp_header* resp_headers() const noexcept { return resp_headers_; }
    /// @return Number of response headers set so far.
    [[nodiscard]] std::uint8_t resp_header_count() const noexcept { return resp_header_count_; }
    /// @return The response body view.
    [[nodiscard]] std::string_view resp_body() const noexcept { return resp_body_; }
    /// @return The response Content-Type.
    [[nodiscard]] std::string_view resp_content_type() const noexcept { return resp_content_type_; }

    /// Mark this request as a HEAD request (set by io_loop when HEAD falls
    /// back to the GET handler).
    void mark_head_request() noexcept { head_request_ = true; }
    /// @return `true` if this is a HEAD request served by the GET handler.
    [[nodiscard]] bool is_head_request() const noexcept { return head_request_; }

    // Internal: construct from connection, parsed request, and route match.
    ctx(detail::connection& conn,
        const detail::parsed_request& req,
        const detail::route_match& match);

private:

    detail::connection& conn_;
    const detail::parsed_request& req_;
    const detail::route_match& match_;

    int status_code_ = 200;
    bool response_sent_ = false;
    bool prebuilt_ = false;
    bool head_request_ = false;

    const handler* handler_chain_ = nullptr;
    std::uint8_t handler_count_ = 0;
    std::uint8_t handler_index_ = 0;

    static constexpr std::uint8_t max_resp_headers_ = 16;
    resp_header resp_headers_[max_resp_headers_]{};
    std::uint8_t resp_header_count_ = 0;

    std::string_view resp_body_;
    std::string_view resp_content_type_;

    static constexpr std::uint8_t max_store_ = 8;
    struct kv_pair { std::string_view key; void* value; };
    kv_pair store_[max_store_]{};
    std::uint8_t store_count_ = 0;
};

} // namespace voie
