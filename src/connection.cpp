#include "connection.h"
#include <unistd.h>
#include <cstring>

namespace voie::detail {

connection::connection(int fd, arena&& arena)
    : fd_{fd}, arena_{std::move(arena)} {
    std::memset(recv_buf_, 0, sizeof(recv_buf_));
}

connection::~connection() {
    if (fd_ >= 0) ::close(fd_);
}

connection::connection(connection&& other) noexcept
    : fd_{other.fd_}, state_{other.state_}, arena_{std::move(other.arena_)},
      recv_len_{other.recv_len_}, send_buf_{other.send_buf_},
      send_len_{other.send_len_}, send_offset_{other.send_offset_} {
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
        other.fd_ = -1;
    }
    return *this;
}

void connection::set_send(const char* data, std::size_t len) noexcept {
    send_buf_ = data;
    send_len_ = len;
    send_offset_ = 0;
}

void connection::reset_for_next_request() noexcept {
    state_ = conn_state::reading;
    recv_len_ = 0;
    send_buf_ = nullptr;
    send_len_ = 0;
    send_offset_ = 0;
    arena_.reset();
}

} // namespace voie::detail
