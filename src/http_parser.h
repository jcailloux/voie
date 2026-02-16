#pragma once

#include <cstddef>
#include <expected>
#include <span>
#include <string_view>

struct phr_header;

namespace voie::detail {

struct parsed_request {
    std::string_view method;
    std::string_view path;
    std::string_view query;
    int minor_version = 1;
    phr_header* headers = nullptr;
    std::size_t num_headers = 0;
    std::string_view body;
    std::size_t total_header_len = 0;
};

enum class parse_error {
    incomplete,
    bad_request,
    too_large,
};

[[nodiscard]]
std::expected<parsed_request, parse_error>
parse_request(const char* buf, std::size_t len, std::size_t prev_len,
              phr_header* header_buf, std::size_t max_headers);

} // namespace voie::detail
