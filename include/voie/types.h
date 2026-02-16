#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace voie {

/// Standard HTTP methods supported by the router.
enum class http_method : std::uint8_t {
    GET,
    POST,
    PUT,
    DELETE,
    PATCH,
    HEAD,
    OPTIONS,
};

/// Total number of HTTP methods in http_method.
constexpr std::uint8_t http_method_count = 7;

/// Convert an http_method to its uppercase string representation.
/// @param m  The method.
/// @return String view (e.g. `"GET"`), or `""` for an out-of-range value.
constexpr std::string_view method_to_string(http_method m) noexcept {
    switch (m) {
        case http_method::GET:     return "GET";
        case http_method::POST:    return "POST";
        case http_method::PUT:     return "PUT";
        case http_method::DELETE:  return "DELETE";
        case http_method::PATCH:   return "PATCH";
        case http_method::HEAD:    return "HEAD";
        case http_method::OPTIONS: return "OPTIONS";
    }
    return "";
}

/// Parse a method string into an http_method.
/// @param s  Uppercase method name (e.g. `"POST"`).
/// @return The matching enumerator, or `std::nullopt` if unrecognized.
constexpr std::optional<http_method> string_to_method(std::string_view s) noexcept {
    if (s == "GET")     return http_method::GET;
    if (s == "POST")    return http_method::POST;
    if (s == "PUT")     return http_method::PUT;
    if (s == "DELETE")  return http_method::DELETE;
    if (s == "PATCH")   return http_method::PATCH;
    if (s == "HEAD")    return http_method::HEAD;
    if (s == "OPTIONS") return http_method::OPTIONS;
    return std::nullopt;
}

} // namespace voie
