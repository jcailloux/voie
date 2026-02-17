#pragma once

#include "arena_pool.h"
#include "connection.h"
#include "router.h"

#include <voie/handler.h>

#include <atomic>
#include <cstdint>
#include <ctime>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace voie::detail {

struct parsed_request;

enum class sqpoll_mode : std::uint8_t {
    off,  // default — SQPOLL disabled
    on,   // explicit opt-in — requires spare cores and CAP_SYS_NICE or elevated RLIMIT_MEMLOCK
};

struct app_config {
    unsigned thread_count = 0;
    std::size_t max_body_size = 1 << 20;
    int listen_backlog = 512;
    sqpoll_mode sqpoll = sqpoll_mode::off;
    unsigned idle_timeout_secs = 60;
};

class event_loop {
public:
    event_loop(const router& router, const app_config& config,
               const handler* not_found,
               const std::function<void(ctx&, std::exception_ptr)>* error_handler);
    virtual ~event_loop();

    event_loop(const event_loop&) = delete;
    event_loop& operator=(const event_loop&) = delete;

    virtual void run(std::string_view address, std::uint16_t port,
                     std::atomic<unsigned>* ready_signal = nullptr) = 0;
    void stop() noexcept { running_ = false; }

protected:
    // I/O primitives — each backend implements these
    virtual void start_recv(std::uint32_t conn_id) = 0;
    virtual void start_send(std::uint32_t conn_id) = 0;
    virtual void start_close(std::uint32_t conn_id) = 0;
    virtual void flush_io() = 0;

    // Shared event handlers (called by each backend after I/O completion)
    void on_recv(std::uint32_t conn_id, int res);
    void on_send(std::uint32_t conn_id, int res);
    void on_timeout();

    // Shared protocol logic
    void process_request(std::uint32_t conn_id);
    void build_response(connection& conn, ctx& c);
    void send_error_response(std::uint32_t conn_id, int status, std::string_view body);
    static bool should_close_connection(const parsed_request& req);

    // Shared infrastructure
    int create_listen_socket(std::string_view address, std::uint16_t port);
    std::uint32_t alloc_conn_id();
    void free_conn_id(std::uint32_t id);

    // Date header cache (per-second)
    void update_date_cache();
    [[nodiscard]] std::string_view cached_date() noexcept;

    const router& router_;
    const app_config& config_;
    const handler* not_found_;
    const std::function<void(ctx&, std::exception_ptr)>* error_handler_;

    arena_pool pool_;
    int listen_fd_ = -1;
    bool running_ = false;

    static constexpr std::uint32_t max_connections = 10240;
    std::unique_ptr<std::optional<connection>[]> connections_;
    std::vector<std::uint32_t> free_list_;

    char date_buf_[32]{};
    std::size_t date_len_ = 0;
    std::time_t last_date_update_ = 0;
};

} // namespace voie::detail
