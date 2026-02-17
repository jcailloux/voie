#pragma once

#include "event_loop.h"

#include <cstdint>
#include <sys/epoll.h>

namespace voie::detail {

class epoll_loop : public event_loop {
public:
    using event_loop::event_loop;
    ~epoll_loop() override;

    void run(std::string_view address, std::uint16_t port,
             std::atomic<unsigned>* ready_signal = nullptr) override;

protected:
    void start_recv(std::uint32_t conn_id) override;
    void start_send(std::uint32_t conn_id) override;
    void start_close(std::uint32_t conn_id) override;
    void flush_io() override;

private:
    static constexpr std::uint32_t ACCEPT_EVENT_ID = max_connections;
    static constexpr std::uint32_t TIMEOUT_EVENT_ID = max_connections + 1;

    int epfd_ = -1;
    int timer_fd_ = -1;

    // Edge-triggered state: flags to break recv/send drain loops
    // when on_recv/on_send triggers a mode switch (recv↔send/close).
    bool recv_break_ = false;
    bool send_break_ = false;

    static constexpr int MAX_EVENTS = 256;
    struct epoll_event events_[MAX_EVENTS];
};

} // namespace voie::detail
