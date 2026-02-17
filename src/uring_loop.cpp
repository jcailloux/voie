#include "uring_loop.h"

#include <liburing.h>

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <stdexcept>

namespace voie::detail {

uring_loop::~uring_loop() {
    if (ring_) {
        io_uring_queue_exit(ring_);
        delete ring_;
    }
}

bool uring_loop::probe() noexcept {
    io_uring ring{};
    if (io_uring_queue_init(1, &ring, 0) < 0)
        return false;
    io_uring_queue_exit(&ring);
    return true;
}

void uring_loop::start_recv(std::uint32_t conn_id) {
    auto& conn = *connections_[conn_id];
    auto* sqe = io_uring_get_sqe(ring_);
    if (!sqe) return;

    char* buf = conn.recv_buf() + conn.recv_len();
    std::size_t remaining = conn.recv_capacity() - conn.recv_len();

    io_uring_prep_recv(sqe, conn.fd(), buf, remaining, 0);
    event_store_[conn_id] = {event_type::recv, conn_id};
    io_uring_sqe_set_data(sqe, &event_store_[conn_id]);
}

void uring_loop::start_send(std::uint32_t conn_id) {
    auto& conn = *connections_[conn_id];
    auto* sqe = io_uring_get_sqe(ring_);
    if (!sqe) return;

    const char* buf = conn.send_buf() + conn.send_offset();
    std::size_t len = conn.send_len() - conn.send_offset();

    io_uring_prep_send(sqe, conn.fd(), buf, len, MSG_NOSIGNAL);
    event_store_[conn_id] = {event_type::send, conn_id};
    io_uring_sqe_set_data(sqe, &event_store_[conn_id]);
}

void uring_loop::start_close(std::uint32_t conn_id) {
    auto& conn = *connections_[conn_id];

    // Ensure pending send data is flushed before the async close
    ::shutdown(conn.fd(), SHUT_WR);

    auto* sqe = io_uring_get_sqe(ring_);
    if (!sqe) return;

    io_uring_prep_close(sqe, conn.fd());
    event_store_[conn_id] = {event_type::close, conn_id};
    io_uring_sqe_set_data(sqe, &event_store_[conn_id]);
}

void uring_loop::flush_io() {
    io_uring_submit(ring_);
}

void uring_loop::submit_accept() {
    auto* sqe = io_uring_get_sqe(ring_);
    if (!sqe) return;

    io_uring_prep_multishot_accept(sqe, listen_fd_, nullptr, nullptr, 0);
    event_store_[ACCEPT_EVENT_ID] = {event_type::accept, 0};
    io_uring_sqe_set_data(sqe, &event_store_[ACCEPT_EVENT_ID]);
}

void uring_loop::submit_timeout() {
    auto* sqe = io_uring_get_sqe(ring_);
    if (!sqe) return;

    struct __kernel_timespec ts = {.tv_sec = 5, .tv_nsec = 0};
    io_uring_prep_timeout(sqe, &ts, 0, 0);
    event_store_[TIMEOUT_EVENT_ID] = {event_type::timeout, 0};
    io_uring_sqe_set_data(sqe, &event_store_[TIMEOUT_EVENT_ID]);
}

void uring_loop::handle_accept(int res, unsigned cqe_flags) {
    bool more = (cqe_flags & IORING_CQE_F_MORE);

    if (res < 0) {
        if (!more) submit_accept();
        io_uring_submit(ring_);
        return;
    }

    int client_fd = res;

    int opt = 1;
    ::setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
    ::setsockopt(client_fd, IPPROTO_TCP, TCP_QUICKACK, &opt, sizeof(opt));

    auto conn_id = alloc_conn_id();
    if (conn_id == max_connections) {
        ::close(client_fd);
        if (!more) submit_accept();
        io_uring_submit(ring_);
        return;
    }

    connections_[conn_id].emplace(client_fd, pool_.acquire());
    start_recv(conn_id);
    if (!more) submit_accept();
    io_uring_submit(ring_);
}

void uring_loop::run(std::string_view address, std::uint16_t port,
                     std::atomic<unsigned>* ready_signal) {
    listen_fd_ = create_listen_socket(address, port);

    event_store_ = std::make_unique<event_data[]>(max_connections + 2);

    ring_ = new io_uring{};

    bool try_sqpoll = (config_.sqpoll == sqpoll_mode::on);

    struct io_uring_params params{};
    params.flags = IORING_SETUP_SINGLE_ISSUER;
    if (try_sqpoll) {
        params.flags |= IORING_SETUP_SQPOLL;
        params.sq_thread_idle = 2000;
    }

    int init_ret = io_uring_queue_init_params(256, ring_, &params);
    if (init_ret < 0) {
        // Fallback for older kernels without SINGLE_ISSUER
        if (io_uring_queue_init(256, ring_, 0) < 0) {
            throw std::runtime_error("io_uring_queue_init failed");
        }
    }

    running_ = true;
    submit_accept();
    submit_timeout();
    io_uring_submit(ring_);

    if (ready_signal) {
        ready_signal->fetch_add(1, std::memory_order_release);
    }

    while (running_) {
        struct io_uring_cqe* cqe = nullptr;
        int ret = io_uring_wait_cqe(ring_, &cqe);
        if (ret < 0) {
            if (ret == -EINTR) continue;
            break;
        }

        auto* ev = static_cast<event_data*>(io_uring_cqe_get_data(cqe));
        if (ev) {
            switch (ev->type) {
                case event_type::accept:
                    handle_accept(cqe->res, cqe->flags);
                    break;
                case event_type::recv:
                    on_recv(ev->conn_id, cqe->res);
                    break;
                case event_type::send:
                    on_send(ev->conn_id, cqe->res);
                    break;
                case event_type::close:
                    free_conn_id(ev->conn_id);
                    break;
                case event_type::timeout:
                    on_timeout();
                    submit_timeout();
                    io_uring_submit(ring_);
                    break;
            }
        }

        io_uring_cqe_seen(ring_, cqe);
    }
}

} // namespace voie::detail
