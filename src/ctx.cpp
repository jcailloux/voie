#include <voie/ctx.h>
#include "http_parser.h"
#include "router.h"
#include "connection.h"

#include <picohttpparser.h>
#include <algorithm>
#include <cstring>

namespace voie {

// --- Header validation helpers ---

static bool is_valid_header_name(std::string_view name) noexcept {
    if (name.empty()) return false;
    for (char c : name) {
        // RFC 7230 token characters
        if (c <= 0x20 || c >= 0x7f) return false;
        switch (c) {
            case '(': case ')': case '<': case '>': case '@':
            case ',': case ';': case ':': case '\\': case '"':
            case '/': case '[': case ']': case '?': case '=':
            case '{': case '}':
                return false;
            default: break;
        }
    }
    return true;
}

static bool is_valid_header_value(std::string_view value) noexcept {
    for (char c : value) {
        if (c == '\r' || c == '\n' || c == '\0') return false;
    }
    return true;
}

static bool header_name_eq(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return false;
    }
    return true;
}

// --- ctx implementation ---

ctx::ctx(detail::connection& conn,
         const detail::parsed_request& req,
         const detail::route_match& match)
    : conn_{conn}, req_{req}, match_{match} {
    handler_chain_ = match.handlers;
    handler_count_ = match.handler_count;
}

std::string_view ctx::method() const noexcept {
    return req_.method;
}

std::string_view ctx::path() const noexcept {
    return req_.path;
}

std::string_view ctx::param(std::string_view name) const noexcept {
    for (std::uint8_t i = 0; i < match_.param_count; ++i) {
        if (match_.params[i].key == name) {
            return match_.params[i].value;
        }
    }
    return {};
}

std::string_view ctx::query(std::string_view name) noexcept {
    std::string_view qs = req_.query;
    while (!qs.empty()) {
        auto amp = qs.find('&');
        auto pair = qs.substr(0, amp);
        auto eq = pair.find('=');
        std::string_view key = pair.substr(0, eq);
        if (key == name) {
            if (eq == std::string_view::npos) return {};
            auto raw_value = pair.substr(eq + 1);
            if (raw_value.empty()) return raw_value;
            // URL-decode the value into arena
            auto& a = conn_.alloc();
            char* buf = static_cast<char*>(a.alloc(raw_value.size(), 1));
            std::memcpy(buf, raw_value.data(), raw_value.size());
            std::size_t decoded_len = detail::url_decode_inplace(buf, raw_value.size());
            return {buf, decoded_len};
        }
        qs = amp != std::string_view::npos ? qs.substr(amp + 1) : std::string_view{};
    }
    return {};
}

std::string_view ctx::header(std::string_view name) const noexcept {
    for (std::size_t i = 0; i < req_.num_headers; ++i) {
        std::string_view hdr_name{req_.headers[i].name, req_.headers[i].name_len};
        if (header_name_eq(hdr_name, name)) {
            return {req_.headers[i].value, req_.headers[i].value_len};
        }
    }
    return {};
}

bool ctx::has_header(std::string_view name) const noexcept {
    for (std::size_t i = 0; i < req_.num_headers; ++i) {
        std::string_view hdr_name{req_.headers[i].name, req_.headers[i].name_len};
        if (header_name_eq(hdr_name, name)) {
            return true;
        }
    }
    return false;
}

std::string_view ctx::body() const noexcept {
    return req_.body;
}

std::size_t ctx::request_header_count() const noexcept {
    return req_.num_headers;
}

std::pair<std::string_view, std::string_view>
ctx::request_header_at(std::size_t index) const noexcept {
    if (index >= req_.num_headers) return {{}, {}};
    return {
        {req_.headers[index].name, req_.headers[index].name_len},
        {req_.headers[index].value, req_.headers[index].value_len}
    };
}

ctx& ctx::status(int code) noexcept {
    status_code_ = code;
    return *this;
}

ctx& ctx::set(std::string_view name, std::string_view value) {
    if (!is_valid_header_name(name) || !is_valid_header_value(value)) {
        return *this;
    }

    // Overwrite if header name already exists (case-insensitive)
    for (std::uint8_t i = 0; i < resp_header_count_; ++i) {
        if (header_name_eq(resp_headers_[i].name, name)) {
            auto& a = conn_.alloc();
            resp_headers_[i].value = a.dup(value);
            return *this;
        }
    }

    if (resp_header_count_ < max_resp_headers_) {
        auto& a = conn_.alloc();
        resp_headers_[resp_header_count_++] = {a.dup(name), a.dup(value)};
    }
    return *this;
}

void ctx::text(std::string_view body) {
    send(body, "text/plain");
}

void ctx::json(std::string_view body) {
    send(body, "application/json");
}

void ctx::html(std::string_view body) {
    send(body, "text/html");
}

void ctx::send(std::string_view body, std::string_view content_type) {
    if (response_sent_) return;
    auto& a = conn_.alloc();
    resp_body_ = a.dup(body);

    // Append charset for text/* types if not already present
    if (content_type.starts_with("text/") &&
        content_type.find("charset") == std::string_view::npos) {
        std::size_t new_len = content_type.size() + 15; // "; charset=utf-8"
        char* buf = static_cast<char*>(a.alloc(new_len, 1));
        std::memcpy(buf, content_type.data(), content_type.size());
        std::memcpy(buf + content_type.size(), "; charset=utf-8", 15);
        resp_content_type_ = std::string_view{buf, new_len};
    } else {
        resp_content_type_ = a.dup(content_type);
    }

    response_sent_ = true;
}

void ctx::no_content() {
    if (response_sent_) return;
    status_code_ = 204;
    resp_body_ = {};
    resp_content_type_ = {};
    response_sent_ = true;
}

void ctx::send_prebuilt(const char* data, std::size_t len) {
    if (response_sent_) return;
    conn_.set_send(data, len);
    response_sent_ = true;
    prebuilt_ = true;
}

void ctx::redirect(std::string_view location, int code) {
    status_code_ = code;
    set("Location", location);
    send("", "text/plain");
}

void ctx::next() {
    if (handler_index_ < handler_count_) {
        auto idx = handler_index_++;
        handler_chain_[idx](*this);
    }
}

void ctx::store(std::string_view key, void* value) noexcept {
    for (std::uint8_t i = 0; i < store_count_; ++i) {
        if (store_[i].key == key) {
            store_[i].value = value;
            return;
        }
    }
    if (store_count_ < max_store_) {
        store_[store_count_++] = {key, value};
    }
}

void* ctx::load(std::string_view key) const noexcept {
    for (std::uint8_t i = 0; i < store_count_; ++i) {
        if (store_[i].key == key) {
            return store_[i].value;
        }
    }
    return nullptr;
}

} // namespace voie
