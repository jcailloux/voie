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
    std::size_t total_consumed = 0;
};

enum class parse_error {
    incomplete,
    bad_request,
    too_large,
};

[[nodiscard]]
std::expected<parsed_request, parse_error>
parse_request(const char* buf, std::size_t len, std::size_t prev_len,
              phr_header* header_buf, std::size_t max_headers,
              std::size_t max_body_size = 0);

// Normalize a URL path in-place. Returns the normalized view (same or shorter).
// Collapses //, resolves /. and /.., rejects paths that escape root or contain
// null bytes. Returns empty string_view if the path is invalid.
[[nodiscard]]
std::string_view normalize_path_inplace(char* path, std::size_t len);

// Decode %XX and + sequences in-place. Returns decoded length.
// The output is always <= input length. Returns 0 with null_found=true
// if a %00 null byte is encountered.
[[nodiscard]]
std::size_t url_decode_inplace(char* buf, std::size_t len, bool* null_found = nullptr);

} // namespace voie::detail
