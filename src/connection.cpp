#include "connection.h"
#include <unistd.h>
#include <cstring>

namespace voie::detail {

connection::connection(int fd, arena&& arena)
    : fd_{fd}, arena_{std::move(arena)} {
    std::memset(recv_buf_, 0, sizeof(recv_buf_));
    touch();
}

connection::~connection() {
    if (fd_ >= 0) ::close(fd_);
}

connection::connection(connection&& other) noexcept
    : fd_{other.fd_}, state_{other.state_}, arena_{std::move(other.arena_)},
      recv_len_{other.recv_len_}, send_buf_{other.send_buf_},
      send_len_{other.send_len_}, send_offset_{other.send_offset_},
      last_activity_{other.last_activity_},
      last_request_consumed_{other.last_request_consumed_},
      close_after_send_{other.close_after_send_} {
    std::memcpy(recv_buf_, other.recv_buf_, recv_len_);
    other.fd_ = -1;
}

connection& connection::operator=(connection&& other) noexcept {
    if (this != &other) {
        if (fd_ >= 0) ::close(fd_);
        fd_ = other.fd_;
        state_ = other.state_;
        arena_ = std::move(other.arena_);
        recv_len_ = other.recv_len_;
        std::memcpy(recv_buf_, other.recv_buf_, recv_len_);
        send_buf_ = other.send_buf_;
        send_len_ = other.send_len_;
        send_offset_ = other.send_offset_;
        last_activity_ = other.last_activity_;
        last_request_consumed_ = other.last_request_consumed_;
        close_after_send_ = other.close_after_send_;
        other.fd_ = -1;
    }
    return *this;
}

void connection::set_send(const char* data, std::size_t len) noexcept {
    send_buf_ = data;
    send_len_ = len;
    send_offset_ = 0;
}

void connection::reset_for_next_request(std::size_t consumed_bytes) noexcept {
    state_ = conn_state::reading;

    // Preserve any pipelined data that belongs to the next request
    if (consumed_bytes < recv_len_) {
        std::size_t remaining = recv_len_ - consumed_bytes;
        std::memmove(recv_buf_, recv_buf_ + consumed_bytes, remaining);
        recv_len_ = remaining;
    } else {
        recv_len_ = 0;
    }

    send_buf_ = nullptr;
    send_len_ = 0;
    send_offset_ = 0;
    last_request_consumed_ = 0;
    close_after_send_ = false;
    arena_.reset();
    touch();
}

} // namespace voie::detail
