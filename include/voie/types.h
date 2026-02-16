#pragma once

#include <cstdint>
#include <string_view>

namespace voie {

enum class http_method : std::uint8_t {
    GET,
    POST,
    PUT,
    DELETE,
    PATCH,
    HEAD,
    OPTIONS,
};

constexpr std::uint8_t http_method_count = 7;

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

constexpr http_method string_to_method(std::string_view s) noexcept {
    if (s == "GET")     return http_method::GET;
    if (s == "POST")    return http_method::POST;
    if (s == "PUT")     return http_method::PUT;
    if (s == "DELETE")  return http_method::DELETE;
    if (s == "PATCH")   return http_method::PATCH;
    if (s == "HEAD")    return http_method::HEAD;
    if (s == "OPTIONS") return http_method::OPTIONS;
    return http_method::GET;
}

} // namespace voie
