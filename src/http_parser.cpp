#include "http_parser.h"
#include <picohttpparser.h>
#include <climits>
#include <cstring>

namespace voie::detail {

// --- Helpers ---

static bool ci_eq(std::string_view a, const char* target, std::size_t target_len) noexcept {
    if (a.size() != target_len) return false;
    for (std::size_t i = 0; i < target_len; ++i) {
        char c = a[i];
        if (c >= 'A' && c <= 'Z') c += 32;
        if (c != target[i]) return false;
    }
    return true;
}

static int hex_digit(char c) noexcept {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

// --- parse_request ---

std::expected<parsed_request, parse_error>
parse_request(const char* buf, std::size_t len, std::size_t prev_len,
              phr_header* header_buf, std::size_t max_headers,
              std::size_t max_body_size) {
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

    // Scan headers for Content-Length and Transfer-Encoding
    std::size_t content_length = 0;
    bool has_content_length = false;
    bool has_transfer_encoding = false;

    for (std::size_t i = 0; i < num_headers; ++i) {
        std::string_view hname{header_buf[i].name, header_buf[i].name_len};

        // Check for Transfer-Encoding (reject — not supported)
        if (ci_eq(hname, "transfer-encoding", 17)) {
            has_transfer_encoding = true;
        }

        // Check for Content-Length
        if (ci_eq(hname, "content-length", 14)) {
            std::string_view vstr{header_buf[i].value, header_buf[i].value_len};

            // Parse digits
            std::size_t val = 0;
            for (char ch : vstr) {
                if (ch < '0' || ch > '9')
                    return std::unexpected(parse_error::bad_request);
                // Overflow check
                if (val > SIZE_MAX / 10)
                    return std::unexpected(parse_error::bad_request);
                val *= 10;
                auto digit = static_cast<std::size_t>(ch - '0');
                if (val > SIZE_MAX - digit)
                    return std::unexpected(parse_error::bad_request);
                val += digit;
            }

            if (has_content_length && content_length != val) {
                // Multiple Content-Length headers with different values (CL-CL smuggling)
                return std::unexpected(parse_error::bad_request);
            }
            has_content_length = true;
            content_length = val;
        }
    }

    // Reject Transfer-Encoding (chunked not supported)
    if (has_transfer_encoding) {
        return std::unexpected(parse_error::bad_request);
    }

    // Early rejection if Content-Length exceeds max_body_size
    if (max_body_size > 0 && has_content_length && content_length > max_body_size) {
        return std::unexpected(parse_error::too_large);
    }

    // Determine body
    std::string_view body_view;

    if (has_content_length) {
        std::size_t available_body = (len > header_len) ? (len - header_len) : 0;
        if (available_body < content_length) {
            return std::unexpected(parse_error::incomplete);
        }
        body_view = std::string_view{buf + header_len, content_length};
    }
    // Without Content-Length, body is empty (correct for pipelining support)

    std::size_t total_consumed = header_len + body_view.size();

    return parsed_request{
        .method = {method_ptr, method_len},
        .path = path,
        .query = query,
        .minor_version = minor_version,
        .headers = header_buf,
        .num_headers = num_headers,
        .body = body_view,
        .total_header_len = header_len,
        .total_consumed = total_consumed,
    };
}

// --- normalize_path_inplace ---

std::string_view normalize_path_inplace(char* path, std::size_t len) {
    if (len == 0 || path[0] != '/') return {};

    // Reject null bytes
    for (std::size_t i = 0; i < len; ++i) {
        if (path[i] == '\0') return {};
    }

    std::size_t write = 0;
    std::size_t read = 0;

    while (read < len) {
        // Write the '/'
        path[write++] = '/';
        read++; // skip the '/'

        // Skip consecutive slashes
        while (read < len && path[read] == '/') read++;

        // Find end of segment
        std::size_t seg_start = read;
        while (read < len && path[read] != '/') read++;
        std::size_t seg_len = read - seg_start;

        if (seg_len == 1 && path[seg_start] == '.') {
            // "." segment — remove (revert the written '/')
            write--;
        } else if (seg_len == 2 && path[seg_start] == '.' && path[seg_start + 1] == '.') {
            // ".." segment — go up one level
            write--; // remove the '/' we just wrote
            // Back up past the parent segment
            while (write > 0 && path[write - 1] != '/') write--;
            // Remove the parent's '/' too (the next iteration writes its own)
            if (write > 0) {
                write--;
            } else {
                return {}; // tried to go above root
            }
        } else if (seg_len > 0) {
            // Normal segment — copy it
            std::memmove(path + write, path + seg_start, seg_len);
            write += seg_len;
        }
    }

    if (write == 0) { path[0] = '/'; write = 1; }

    return std::string_view{path, write};
}

// --- url_decode_inplace ---

std::size_t url_decode_inplace(char* buf, std::size_t len, bool* null_found) {
    if (null_found) *null_found = false;

    std::size_t write = 0;
    for (std::size_t i = 0; i < len; ++i) {
        if (buf[i] == '%' && i + 2 < len) {
            int h = hex_digit(buf[i + 1]);
            int l = hex_digit(buf[i + 2]);
            if (h >= 0 && l >= 0) {
                char decoded = static_cast<char>((h << 4) | l);
                if (decoded == '\0') {
                    if (null_found) *null_found = true;
                    return 0;
                }
                buf[write++] = decoded;
                i += 2;
                continue;
            }
        }
        if (buf[i] == '+') {
            buf[write++] = ' ';
        } else {
            buf[write++] = buf[i];
        }
    }
    return write;
}

} // namespace voie::detail
