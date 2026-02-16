#include "io_loop.h"
#include "http_parser.h"
#include <voie/ctx.h>

#include <liburing.h>
#include <picohttpparser.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>

namespace voie::detail {

// Fast integer-to-string: writes digits and returns pointer past end
static char* uint_to_str(char* p, std::size_t val) noexcept {
    char tmp[20];
    int len = 0;
    if (val == 0) {
        *p = '0';
        return p + 1;
    }
    while (val > 0) {
        tmp[len++] = '0' + static_cast<char>(val % 10);
        val /= 10;
    }
    for (int i = len - 1; i >= 0; --i)
        *p++ = tmp[i];
    return p;
}

static char* int_to_str(char* p, int val) noexcept {
    return uint_to_str(p, static_cast<std::size_t>(val));
}

static constexpr std::uint32_t ACCEPT_EVENT_ID = 10240; // max_connections

io_loop::io_loop(const router& router, const app_config& config,
                 const handler* not_found,
                 const std::function<void(ctx&, std::exception_ptr)>* error_handler)
    : router_{router}, config_{config}, not_found_{not_found},
      error_handler_{error_handler},
      connections_{std::make_unique<std::optional<connection>[]>(max_connections)},
      event_store_{std::make_unique<event_data[]>(max_connections + 1)} {
    // Initialize free list
    free_list_.reserve(max_connections);
    for (std::uint32_t i = max_connections; i > 0; --i) {
        free_list_.push_back(i - 1);
    }
}

io_loop::~io_loop() {
    if (ring_) {
        io_uring_queue_exit(ring_);
        delete ring_;
    }
    if (listen_fd_ >= 0) {
        ::close(listen_fd_);
    }
}

int io_loop::create_listen_socket(std::string_view address, std::uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) throw std::runtime_error("socket() failed");

    int opt = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));

    // Disable Nagle's algorithm for lower latency
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    std::string addr_str{address};
    if (::inet_pton(AF_INET, addr_str.c_str(), &addr.sin_addr) <= 0) {
        ::close(fd);
        throw std::runtime_error("invalid address: " + addr_str);
    }

    if (::bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        throw std::runtime_error("bind() failed: " + std::string(std::strerror(errno)));
    }

    if (::listen(fd, config_.listen_backlog) < 0) {
        ::close(fd);
        throw std::runtime_error("listen() failed");
    }

    return fd;
}

std::uint32_t io_loop::alloc_conn_id() {
    if (free_list_.empty()) return max_connections; // sentinel: full
    auto id = free_list_.back();
    free_list_.pop_back();
    return id;
}

void io_loop::free_conn_id(std::uint32_t id) {
    connections_[id].reset();
    free_list_.push_back(id);
}

void io_loop::submit_accept() {
    auto* sqe = io_uring_get_sqe(ring_);
    if (!sqe) return;

    io_uring_prep_multishot_accept(sqe, listen_fd_, nullptr, nullptr, 0);
    event_store_[ACCEPT_EVENT_ID] = {event_type::accept, 0};
    io_uring_sqe_set_data(sqe, &event_store_[ACCEPT_EVENT_ID]);
}

void io_loop::submit_recv(std::uint32_t conn_id) {
    auto& conn = *connections_[conn_id];
    auto* sqe = io_uring_get_sqe(ring_);
    if (!sqe) return;

    char* buf = conn.recv_buf() + conn.recv_len();
    std::size_t remaining = conn.recv_capacity() - conn.recv_len();

    io_uring_prep_recv(sqe, conn.fd(), buf, remaining, 0);
    event_store_[conn_id] = {event_type::recv, conn_id};
    io_uring_sqe_set_data(sqe, &event_store_[conn_id]);
}

void io_loop::submit_send(std::uint32_t conn_id) {
    auto& conn = *connections_[conn_id];
    auto* sqe = io_uring_get_sqe(ring_);
    if (!sqe) return;

    const char* buf = conn.send_buf() + conn.send_offset();
    std::size_t len = conn.send_len() - conn.send_offset();

    io_uring_prep_send(sqe, conn.fd(), buf, len, MSG_NOSIGNAL);
    event_store_[conn_id] = {event_type::send, conn_id};
    io_uring_sqe_set_data(sqe, &event_store_[conn_id]);
}

void io_loop::submit_close(std::uint32_t conn_id) {
    auto& conn = *connections_[conn_id];
    auto* sqe = io_uring_get_sqe(ring_);
    if (!sqe) return;

    io_uring_prep_close(sqe, conn.fd());
    event_store_[conn_id] = {event_type::close, conn_id};
    io_uring_sqe_set_data(sqe, &event_store_[conn_id]);
}

void io_loop::handle_accept(int res, unsigned cqe_flags) {
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
    submit_recv(conn_id);
    if (!more) submit_accept();
    io_uring_submit(ring_);
}

void io_loop::handle_recv(std::uint32_t conn_id, int res) {
    if (res <= 0) {
        submit_close(conn_id);
        io_uring_submit(ring_);
        return;
    }

    auto& conn = *connections_[conn_id];
    conn.advance_recv(static_cast<std::size_t>(res));
    conn.set_state(conn_state::processing);
    process_request(conn_id);
}

void io_loop::handle_send(std::uint32_t conn_id, int res) {
    if (res <= 0) {
        submit_close(conn_id);
        io_uring_submit(ring_);
        return;
    }

    auto& conn = *connections_[conn_id];
    conn.advance_send(static_cast<std::size_t>(res));

    if (!conn.send_complete()) {
        submit_send(conn_id);
        io_uring_submit(ring_);
        return;
    }

    conn.reset_for_next_request();
    submit_recv(conn_id);
    io_uring_submit(ring_);
}

void io_loop::process_request(std::uint32_t conn_id) {
    auto& conn = *connections_[conn_id];

    // Parse HTTP request
    phr_header headers[64];
    auto result = parse_request(conn.recv_buf(), conn.recv_len(), 0, headers, 64);

    if (!result.has_value()) {
        if (result.error() == parse_error::incomplete) {
            conn.set_state(conn_state::reading);
            submit_recv(conn_id);
            io_uring_submit(ring_);
            return;
        }
        send_error_response(conn_id, 400, "Bad Request");
        return;
    }

    auto& req = result.value();

    // Check body size limit
    if (req.body.size() > config_.max_body_size) {
        send_error_response(conn_id, 413, "Payload Too Large");
        return;
    }

    // Route lookup
    auto method = string_to_method(req.method);
    auto match_result = router_.lookup(method, req.path);
    if (!match_result.has_value()) {
        // 404 — use custom handler if set
        if (not_found_ && *not_found_) {
            route_match empty_match{};
            empty_match.handlers = nullptr;
            empty_match.handler_count = 0;
            ctx c(conn, req, empty_match);
            (*not_found_)(c);
            if (!c.is_prebuilt())
                build_response(conn, c);
        } else {
            send_error_response(conn_id, 404, "Not Found");
            return;
        }
    } else {
        auto& match = match_result.value();
        ctx c(conn, req, match);

        try {
            c.next();
        } catch (...) {
            if (error_handler_ && *error_handler_) {
                route_match err_match{};
                ctx err_ctx(conn, req, err_match);
                (*error_handler_)(err_ctx, std::current_exception());
                if (!err_ctx.is_prebuilt())
                    build_response(conn, err_ctx);
                conn.set_state(conn_state::writing);
                submit_send(conn_id);
                io_uring_submit(ring_);
                return;
            }
            send_error_response(conn_id, 500, "Internal Server Error");
            return;
        }

        if (!c.is_prebuilt())
            build_response(conn, c);
    }

    conn.set_state(conn_state::writing);
    submit_send(conn_id);
    io_uring_submit(ring_);
}

void io_loop::build_response(connection& conn, ctx& c) {
    auto& a = conn.alloc();

    // Status line
    const char* status_text = "OK";
    int code = c.status_code();
    switch (code) {
        case 200: status_text = "OK"; break;
        case 201: status_text = "Created"; break;
        case 204: status_text = "No Content"; break;
        case 301: status_text = "Moved Permanently"; break;
        case 302: status_text = "Found"; break;
        case 304: status_text = "Not Modified"; break;
        case 400: status_text = "Bad Request"; break;
        case 401: status_text = "Unauthorized"; break;
        case 403: status_text = "Forbidden"; break;
        case 404: status_text = "Not Found"; break;
        case 405: status_text = "Method Not Allowed"; break;
        case 413: status_text = "Payload Too Large"; break;
        case 500: status_text = "Internal Server Error"; break;
    }

    auto body = c.resp_body();
    auto content_type = c.resp_content_type();

    // Calculate response size
    // "HTTP/1.1 XXX Status\r\n" + headers + "\r\n" + body
    std::size_t estimated = 128 + body.size();
    for (std::uint8_t i = 0; i < c.resp_header_count(); ++i) {
        estimated += c.resp_headers()[i].name.size() + c.resp_headers()[i].value.size() + 4;
    }
    if (!content_type.empty()) estimated += 32 + content_type.size();
    estimated += 32; // Content-Length header

    char* buf = static_cast<char*>(a.alloc(estimated, 1));
    char* p = buf;

    // Status line: "HTTP/1.1 XXX Status\r\n"
    std::memcpy(p, "HTTP/1.1 ", 9);
    p += 9;
    p = int_to_str(p, code);
    *p++ = ' ';
    auto st_len = std::strlen(status_text);
    std::memcpy(p, status_text, st_len);
    p += st_len;
    *p++ = '\r'; *p++ = '\n';

    // Content-Type
    if (!content_type.empty()) {
        std::memcpy(p, "Content-Type: ", 14);
        p += 14;
        std::memcpy(p, content_type.data(), content_type.size());
        p += content_type.size();
        *p++ = '\r'; *p++ = '\n';
    }

    // Content-Length
    std::memcpy(p, "Content-Length: ", 16);
    p += 16;
    p = uint_to_str(p, body.size());
    *p++ = '\r'; *p++ = '\n';

    // Connection: keep-alive
    std::memcpy(p, "Connection: keep-alive\r\n", 24);
    p += 24;

    // Custom headers
    for (std::uint8_t i = 0; i < c.resp_header_count(); ++i) {
        auto& h = c.resp_headers()[i];
        std::memcpy(p, h.name.data(), h.name.size());
        p += h.name.size();
        std::memcpy(p, ": ", 2);
        p += 2;
        std::memcpy(p, h.value.data(), h.value.size());
        p += h.value.size();
        std::memcpy(p, "\r\n", 2);
        p += 2;
    }

    // End of headers
    std::memcpy(p, "\r\n", 2);
    p += 2;

    // Body
    if (!body.empty()) {
        std::memcpy(p, body.data(), body.size());
        p += body.size();
    }

    auto total = static_cast<std::size_t>(p - buf);
    conn.set_send(buf, total);
}

void io_loop::send_error_response(std::uint32_t conn_id, int status, std::string_view body) {
    auto& conn = *connections_[conn_id];
    auto& a = conn.alloc();

    const char* status_text = "Error";
    switch (status) {
        case 400: status_text = "Bad Request"; break;
        case 404: status_text = "Not Found"; break;
        case 413: status_text = "Payload Too Large"; break;
        case 500: status_text = "Internal Server Error"; break;
    }

    std::size_t estimated = 128 + body.size();
    char* buf = static_cast<char*>(a.alloc(estimated, 1));
    char* p = buf;

    std::memcpy(p, "HTTP/1.1 ", 9); p += 9;
    p = int_to_str(p, status);
    *p++ = ' ';
    auto st_len = std::strlen(status_text);
    std::memcpy(p, status_text, st_len); p += st_len;
    std::memcpy(p, "\r\nContent-Type: text/plain\r\nContent-Length: ", 44); p += 44;
    p = uint_to_str(p, body.size());
    std::memcpy(p, "\r\nConnection: close\r\n\r\n", 23); p += 23;

    std::memcpy(p, body.data(), body.size());
    p += body.size();
    conn.set_send(buf, static_cast<std::size_t>(p - buf));

    conn.set_state(conn_state::writing);
    submit_send(conn_id);
    io_uring_submit(ring_);
}

void io_loop::run(std::string_view address, std::uint16_t port) {
    listen_fd_ = create_listen_socket(address, port);

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
    io_uring_submit(ring_);

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
                    handle_recv(ev->conn_id, cqe->res);
                    break;
                case event_type::send:
                    handle_send(ev->conn_id, cqe->res);
                    break;
                case event_type::close:
                    free_conn_id(ev->conn_id);
                    break;
            }
        }

        io_uring_cqe_seen(ring_, cqe);
    }
}

void io_loop::stop() noexcept {
    running_ = false;
}

} // namespace voie::detail
