#pragma once

#include "arena_pool.h"
#include "connection.h"
#include "router.h"

#include <voie/handler.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

struct io_uring;

namespace voie::detail {

enum class sqpoll_mode : std::uint8_t {
    off,  // default — SQPOLL disabled
    on,   // explicit opt-in — requires spare cores and CAP_SYS_NICE or elevated RLIMIT_MEMLOCK
};

struct app_config {
    unsigned thread_count = 0;
    std::size_t max_body_size = 1 << 20;
    int listen_backlog = 512;
    sqpoll_mode sqpoll = sqpoll_mode::off;
};

class io_loop {
public:
    io_loop(const router& router, const app_config& config,
            const handler* not_found,
            const std::function<void(ctx&, std::exception_ptr)>* error_handler);
    ~io_loop();

    io_loop(const io_loop&) = delete;
    io_loop& operator=(const io_loop&) = delete;

    void run(std::string_view address, std::uint16_t port);
    void stop() noexcept;

private:
    enum class event_type : std::uint8_t {
        accept,
        recv,
        send,
        close,
    };

    struct event_data {
        event_type type;
        std::uint32_t conn_id;
    };

    int create_listen_socket(std::string_view address, std::uint16_t port);
    void submit_accept();
    void submit_recv(std::uint32_t conn_id);
    void submit_send(std::uint32_t conn_id);
    void submit_close(std::uint32_t conn_id);

    void handle_accept(int res, unsigned cqe_flags);
    void handle_recv(std::uint32_t conn_id, int res);
    void handle_send(std::uint32_t conn_id, int res);

    void process_request(std::uint32_t conn_id);
    void build_response(connection& conn, ctx& c);
    void send_error_response(std::uint32_t conn_id, int status, std::string_view body);

    const router& router_;
    const app_config& config_;
    const handler* not_found_;
    const std::function<void(ctx&, std::exception_ptr)>* error_handler_;

    arena_pool pool_;
    io_uring* ring_ = nullptr;
    int listen_fd_ = -1;
    bool running_ = false;

    static constexpr std::uint32_t max_connections = 10240;
    std::unique_ptr<std::optional<connection>[]> connections_;
    std::vector<std::uint32_t> free_list_;

    // Event data storage (one per connection + one for accept)
    std::unique_ptr<event_data[]> event_store_;

    std::uint32_t alloc_conn_id();
    void free_conn_id(std::uint32_t id);
};

} // namespace voie::detail
