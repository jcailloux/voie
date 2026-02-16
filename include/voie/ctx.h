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

class ctx {
public:
    // -- Request access --
    [[nodiscard]] std::string_view method() const noexcept;
    [[nodiscard]] std::string_view path() const noexcept;
    [[nodiscard]] std::string_view param(std::string_view name) const noexcept;
    [[nodiscard]] std::string_view query(std::string_view name) noexcept;
    [[nodiscard]] std::string_view header(std::string_view name) const noexcept;
    [[nodiscard]] bool has_header(std::string_view name) const noexcept;
    [[nodiscard]] std::string_view body() const noexcept;

    // -- Request header iteration --
    [[nodiscard]] std::size_t request_header_count() const noexcept;
    [[nodiscard]] std::pair<std::string_view, std::string_view>
        request_header_at(std::size_t index) const noexcept;

    // -- Response building (chainable) --
    ctx& status(int code) noexcept;
    ctx& set(std::string_view name, std::string_view value);

    // -- Terminal response methods --
    void text(std::string_view body);
    void json(std::string_view body);
    void html(std::string_view body);
    void send(std::string_view body, std::string_view content_type);
    void no_content();
    void redirect(std::string_view location, int code = 302);

    // -- Pre-built response (zero-copy, skips build_response) --
    void send_prebuilt(const char* data, std::size_t len);

    // -- Middleware flow --
    void next();

    // -- Context storage --
    void  store(std::string_view key, void* value) noexcept;
    void* load(std::string_view key) const noexcept;

    template <typename T>
    void store(std::string_view key, T* value) noexcept {
        store(key, static_cast<void*>(value));
    }

    template <typename T>
    [[nodiscard]] T* load(std::string_view key) const noexcept {
        return static_cast<T*>(load(key));
    }

    // -- Internal state access --
    [[nodiscard]] int status_code() const noexcept { return status_code_; }
    [[nodiscard]] bool response_sent() const noexcept { return response_sent_; }
    [[nodiscard]] bool is_prebuilt() const noexcept { return prebuilt_; }

    struct resp_header {
        std::string_view name;
        std::string_view value;
    };
    [[nodiscard]] const resp_header* resp_headers() const noexcept { return resp_headers_; }
    [[nodiscard]] std::uint8_t resp_header_count() const noexcept { return resp_header_count_; }
    [[nodiscard]] std::string_view resp_body() const noexcept { return resp_body_; }
    [[nodiscard]] std::string_view resp_content_type() const noexcept { return resp_content_type_; }

    // HEAD request support (set by io_loop when HEAD falls back to GET handler)
    void mark_head_request() noexcept { head_request_ = true; }
    [[nodiscard]] bool is_head_request() const noexcept { return head_request_; }

    // Internal: construct from connection, parsed request, and route match
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
