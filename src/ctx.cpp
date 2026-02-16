#include <voie/ctx.h>
#include "http_parser.h"
#include "router.h"
#include "connection.h"

#include <picohttpparser.h>
#include <algorithm>
#include <cstring>

namespace voie {

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

std::string_view ctx::query(std::string_view name) const noexcept {
    // Parse query string lazily from req_.query
    std::string_view qs = req_.query;
    while (!qs.empty()) {
        auto amp = qs.find('&');
        auto pair = qs.substr(0, amp);
        auto eq = pair.find('=');
        std::string_view key = pair.substr(0, eq);
        if (key == name) {
            return eq != std::string_view::npos ? pair.substr(eq + 1) : std::string_view{};
        }
        qs = amp != std::string_view::npos ? qs.substr(amp + 1) : std::string_view{};
    }
    return {};
}

std::string_view ctx::header(std::string_view name) const noexcept {
    for (std::size_t i = 0; i < req_.num_headers; ++i) {
        std::string_view hdr_name{req_.headers[i].name, req_.headers[i].name_len};
        // Case-insensitive comparison
        if (hdr_name.size() == name.size()) {
            bool match = true;
            for (std::size_t j = 0; j < name.size(); ++j) {
                char a = hdr_name[j];
                char b = name[j];
                if (a >= 'A' && a <= 'Z') a += 32;
                if (b >= 'A' && b <= 'Z') b += 32;
                if (a != b) { match = false; break; }
            }
            if (match) {
                return {req_.headers[i].value, req_.headers[i].value_len};
            }
        }
    }
    return {};
}

std::string_view ctx::body() const noexcept {
    return req_.body;
}

ctx& ctx::status(int code) noexcept {
    status_code_ = code;
    return *this;
}

ctx& ctx::set(std::string_view name, std::string_view value) {
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
    resp_content_type_ = a.dup(content_type);
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
