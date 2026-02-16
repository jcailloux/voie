#include "http_parser.h"
#include <picohttpparser.h>

namespace voie::detail {

std::expected<parsed_request, parse_error>
parse_request(const char* buf, std::size_t len, std::size_t prev_len,
              phr_header* header_buf, std::size_t max_headers) {
    const char* method_ptr = nullptr;
    std::size_t method_len = 0;
    const char* path_ptr = nullptr;
    std::size_t path_len = 0;
    int minor_version = 0;
    std::size_t num_headers = max_headers;

    int ret = phr_parse_request(buf, len,
                                &method_ptr, &method_len,
                                &path_ptr, &path_len,
                                &minor_version,
                                header_buf, &num_headers,
                                prev_len);

    if (ret == -1) return std::unexpected(parse_error::bad_request);
    if (ret == -2) return std::unexpected(parse_error::incomplete);

    auto header_len = static_cast<std::size_t>(ret);

    // Split path and query string
    std::string_view full_path{path_ptr, path_len};
    std::string_view path = full_path;
    std::string_view query;

    if (auto pos = full_path.find('?'); pos != std::string_view::npos) {
        path = full_path.substr(0, pos);
        query = full_path.substr(pos + 1);
    }

    // Determine body from Content-Length
    std::string_view body_view;
    if (len > header_len) {
        body_view = std::string_view{buf + header_len, len - header_len};
    }

    return parsed_request{
        .method = {method_ptr, method_len},
        .path = path,
        .query = query,
        .minor_version = minor_version,
        .headers = header_buf,
        .num_headers = num_headers,
        .body = body_view,
        .total_header_len = header_len,
    };
}

} // namespace voie::detail
