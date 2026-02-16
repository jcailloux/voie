#pragma once

#include "arena.h"

#include <cstddef>
#include <cstdint>

namespace voie::detail {

enum class conn_state : std::uint8_t {
    reading,
    processing,
    writing,
    closing,
};

class connection {
public:
    explicit connection(int fd, arena&& arena);
    ~connection();

    connection(const connection&) = delete;
    connection& operator=(const connection&) = delete;
    connection(connection&& other) noexcept;
    connection& operator=(connection&& other) noexcept;

    [[nodiscard]] int fd() const noexcept { return fd_; }
    [[nodiscard]] conn_state state() const noexcept { return state_; }
    void set_state(conn_state s) noexcept { state_ = s; }

    // Receive buffer
    [[nodiscard]] char* recv_buf() noexcept { return recv_buf_; }
    [[nodiscard]] std::size_t recv_capacity() const noexcept { return sizeof(recv_buf_); }
    [[nodiscard]] std::size_t recv_len() const noexcept { return recv_len_; }
    void advance_recv(std::size_t n) noexcept { recv_len_ += n; }

    // Send buffer
    [[nodiscard]] const char* send_buf() const noexcept { return send_buf_; }
    [[nodiscard]] std::size_t send_len() const noexcept { return send_len_; }
    [[nodiscard]] std::size_t send_offset() const noexcept { return send_offset_; }
    void set_send(const char* data, std::size_t len) noexcept;
    void advance_send(std::size_t n) noexcept { send_offset_ += n; }
    [[nodiscard]] bool send_complete() const noexcept { return send_offset_ >= send_len_; }

    // Arena
    [[nodiscard]] arena& alloc() noexcept { return arena_; }

    // Reset for keep-alive
    void reset_for_next_request() noexcept;

private:
    int fd_ = -1;
    conn_state state_ = conn_state::reading;
    arena arena_;

    char recv_buf_[4096];
    std::size_t recv_len_ = 0;

    const char* send_buf_ = nullptr;
    std::size_t send_len_ = 0;
    std::size_t send_offset_ = 0;
};

} // namespace voie::detail
