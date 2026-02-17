#pragma once

#include "event_loop.h"

#include <cstdint>
#include <memory>

struct io_uring;
struct io_uring_sqe;

namespace voie::detail {

class uring_loop : public event_loop {
public:
    using event_loop::event_loop;
    ~uring_loop() override;

    void run(std::string_view address, std::uint16_t port,
             std::atomic<unsigned>* ready_signal = nullptr) override;

    /// Quick runtime check: can we create an io_uring instance?
    [[nodiscard]] static bool probe() noexcept;

protected:
    void start_recv(std::uint32_t conn_id) override;
    void start_send(std::uint32_t conn_id) override;
    void start_close(std::uint32_t conn_id) override;
    void flush_io() override;

private:
    enum class event_type : std::uint8_t {
        accept,
        recv,
        send,
        close,
        timeout,
    };

    struct event_data {
        event_type type;
        std::uint32_t conn_id;
    };

    ::io_uring_sqe* acquire_sqe();
    void close_sync(std::uint32_t conn_id);
    void submit_accept();
    void submit_timeout();
    void handle_accept(int res, unsigned cqe_flags);

    io_uring* ring_ = nullptr;

    static constexpr std::uint32_t ACCEPT_EVENT_ID = max_connections;
    static constexpr std::uint32_t TIMEOUT_EVENT_ID = max_connections + 1;
    std::unique_ptr<event_data[]> event_store_;
};

} // namespace voie::detail
