#include "epoll_loop.h"

#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include <cerrno>
#include <stdexcept>

namespace voie::detail {

epoll_loop::~epoll_loop() {
    if (timer_fd_ >= 0) ::close(timer_fd_);
    if (epfd_ >= 0) ::close(epfd_);
}

void epoll_loop::start_recv(std::uint32_t conn_id) {
    auto& conn = *connections_[conn_id];
    struct epoll_event ev{};
    ev.events = EPOLLIN | EPOLLET;
    ev.data.u32 = conn_id;
    epoll_ctl(epfd_, EPOLL_CTL_MOD, conn.fd(), &ev);
    send_break_ = true;
}

void epoll_loop::start_send(std::uint32_t conn_id) {
    auto& conn = *connections_[conn_id];
    struct epoll_event ev{};
    ev.events = EPOLLOUT | EPOLLET;
    ev.data.u32 = conn_id;
    epoll_ctl(epfd_, EPOLL_CTL_MOD, conn.fd(), &ev);
    recv_break_ = true;
}

void epoll_loop::start_close(std::uint32_t conn_id) {
    auto& conn = *connections_[conn_id];
    epoll_ctl(epfd_, EPOLL_CTL_DEL, conn.fd(), nullptr);
    ::shutdown(conn.fd(), SHUT_WR);
    ::close(conn.fd());
    free_conn_id(conn_id);
    recv_break_ = true;
    send_break_ = true;
}

void epoll_loop::flush_io() {
    // epoll operations are immediate — nothing to flush
}

void epoll_loop::run(std::string_view address, std::uint16_t port,
                     std::atomic<unsigned>* ready_signal) {
    listen_fd_ = create_listen_socket(address, port);

    // Make listen socket non-blocking
    int flags = ::fcntl(listen_fd_, F_GETFL, 0);
    ::fcntl(listen_fd_, F_SETFL, flags | O_NONBLOCK);

    epfd_ = ::epoll_create1(0);
    if (epfd_ < 0) throw std::runtime_error("epoll_create1 failed");

    // Register listen socket
    struct epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.u32 = ACCEPT_EVENT_ID;
    if (epoll_ctl(epfd_, EPOLL_CTL_ADD, listen_fd_, &ev) < 0)
        throw std::runtime_error("epoll_ctl(listen) failed");

    // Create timerfd for periodic idle-connection sweep
    timer_fd_ = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
    if (timer_fd_ < 0) throw std::runtime_error("timerfd_create failed");

    struct itimerspec ts{};
    ts.it_interval = {5, 0};
    ts.it_value = {5, 0};
    ::timerfd_settime(timer_fd_, 0, &ts, nullptr);

    ev.events = EPOLLIN;
    ev.data.u32 = TIMEOUT_EVENT_ID;
    if (epoll_ctl(epfd_, EPOLL_CTL_ADD, timer_fd_, &ev) < 0)
        throw std::runtime_error("epoll_ctl(timer) failed");

    running_ = true;

    if (ready_signal) {
        ready_signal->fetch_add(1, std::memory_order_release);
    }

    while (running_) {
        int n = ::epoll_wait(epfd_, events_, MAX_EVENTS, 1000);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }

        for (int i = 0; i < n; ++i) {
            std::uint32_t id = events_[i].data.u32;

            if (id == ACCEPT_EVENT_ID) {
                // Accept new connections in a loop until EAGAIN
                for (;;) {
                    int client_fd = ::accept4(listen_fd_, nullptr, nullptr, SOCK_NONBLOCK);
                    if (client_fd < 0) break; // EAGAIN or error

                    int opt = 1;
                    ::setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
                    ::setsockopt(client_fd, IPPROTO_TCP, TCP_QUICKACK, &opt, sizeof(opt));

                    auto conn_id = alloc_conn_id();
                    if (conn_id == max_connections) {
                        ::close(client_fd);
                        continue;
                    }

                    connections_[conn_id].emplace(client_fd, pool_.acquire());

                    // Register for reading (edge-triggered)
                    struct epoll_event cev{};
                    cev.events = EPOLLIN | EPOLLET;
                    cev.data.u32 = conn_id;
                    epoll_ctl(epfd_, EPOLL_CTL_ADD, client_fd, &cev);
                }

            } else if (id == TIMEOUT_EVENT_ID) {
                // Drain the timerfd
                std::uint64_t expirations;
                ::read(timer_fd_, &expirations, sizeof(expirations));
                on_timeout();

            } else {
                // Client connection event
                auto ev_flags = events_[i].events;

                if (ev_flags & (EPOLLERR | EPOLLHUP)) {
                    // Connection error or hangup
                    start_close(id);
                    continue;
                }

                if (!connections_[id].has_value()) continue;

                if (ev_flags & EPOLLIN) {
                    // Edge-triggered: drain all available data
                    recv_break_ = false;
                    for (;;) {
                        if (!connections_[id].has_value()) break;
                        auto& conn = *connections_[id];
                        char* buf = conn.recv_buf() + conn.recv_len();
                        std::size_t remaining = conn.recv_capacity() - conn.recv_len();
                        if (remaining == 0) break;
                        ssize_t res = ::recv(conn.fd(), buf, remaining, 0);
                        if (res < 0) {
                            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                            on_recv(id, static_cast<int>(res));
                            break;
                        }
                        if (res == 0) { on_recv(id, 0); break; }
                        on_recv(id, static_cast<int>(res));
                        if (recv_break_) break;
                    }

                } else if (ev_flags & EPOLLOUT) {
                    // Edge-triggered: send as much as possible
                    send_break_ = false;
                    for (;;) {
                        if (!connections_[id].has_value()) break;
                        auto& conn = *connections_[id];
                        const char* buf = conn.send_buf() + conn.send_offset();
                        std::size_t len = conn.send_len() - conn.send_offset();
                        if (len == 0) break;
                        ssize_t res = ::send(conn.fd(), buf, len, MSG_NOSIGNAL);
                        if (res < 0) {
                            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                            on_send(id, static_cast<int>(res));
                            break;
                        }
                        on_send(id, static_cast<int>(res));
                        if (send_break_) break;
                    }
                }
            }
        }
    }
}

} // namespace voie::detail
