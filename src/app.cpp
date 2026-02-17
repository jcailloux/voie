#include <voie/app.h>
#include "router.h"
#include "event_loop.h"
#include "epoll_loop.h"
#ifdef VOIE_HAS_IO_URING
#include "uring_loop.h"
#endif

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace voie {

struct group::impl {
    detail::router router;
};

struct app::impl {
    unsigned thread_count = 0;
    std::size_t max_body_size = 1 << 20;
    int listen_backlog = 512;
    voie::backend backend = voie::backend::auto_detect;
    std::vector<middleware_fn> global_middleware;
    handler not_found_handler;
    std::function<void(ctx&, std::exception_ptr)> error_handler;
    group::impl group_impl;
    detail::app_config config;
    std::vector<std::unique_ptr<detail::event_loop>> loops;
    std::vector<std::thread> threads;
    std::atomic<unsigned> ready_count{0};
    std::atomic<unsigned> total_threads{0};
};

static voie::backend resolve_backend(voie::backend b) {
    if (b != voie::backend::auto_detect) return b;
#ifdef VOIE_HAS_IO_URING
    if (app::backend_available(voie::backend::io_uring))
        return voie::backend::io_uring;
#endif
    return voie::backend::epoll;
}

static std::unique_ptr<detail::event_loop>
create_loop(voie::backend b,
            const detail::router& router,
            const detail::app_config& config,
            const handler* not_found,
            const std::function<void(ctx&, std::exception_ptr)>* error_handler) {
    switch (b) {
#ifdef VOIE_HAS_IO_URING
        case voie::backend::io_uring:
            return std::make_unique<detail::uring_loop>(router, config, not_found, error_handler);
#endif
        case voie::backend::epoll:
            return std::make_unique<detail::epoll_loop>(router, config, not_found, error_handler);
        default:
            return std::make_unique<detail::epoll_loop>(router, config, not_found, error_handler);
    }
}

app::app() : impl_(std::make_unique<impl>()) {}
app::~app() {
    shutdown();
    for (auto& t : impl_->threads) {
        if (t.joinable()) t.join();
    }
}

app& app::threads(unsigned n) { impl_->thread_count = n; return *this; }
app& app::max_body(std::size_t bytes) { impl_->max_body_size = bytes; return *this; }
app& app::backlog(int n) { impl_->listen_backlog = n; return *this; }
app& app::sqpoll(bool enable) {
    impl_->config.sqpoll = enable
        ? detail::sqpoll_mode::on
        : detail::sqpoll_mode::off;
    return *this;
}
app& app::set_backend(voie::backend b) { impl_->backend = b; return *this; }

bool app::backend_available(voie::backend b) {
    switch (b) {
        case voie::backend::epoll:
            return true;
        case voie::backend::io_uring: {
#ifdef VOIE_HAS_IO_URING
            return detail::uring_loop::probe();
#else
            return false;
#endif
        }
        case voie::backend::auto_detect:
            return true;
    }
    return false;
}

app& app::use(handler mw) {
    auto shared = std::make_shared<handler>(std::move(mw));
    impl_->global_middleware.emplace_back(
        [shared](ctx& c) { (*shared)(c); }
    );
    return *this;
}

group app::group(std::string_view prefix) {
    return root_group().subgroup(prefix);
}

voie::group app::root_group() {
    return voie::group(impl_->group_impl, "", impl_->global_middleware);
}

app& app::not_found(handler h) {
    impl_->not_found_handler = std::move(h);
    return *this;
}

app& app::on_error(std::function<void(ctx&, std::exception_ptr)> h) {
    impl_->error_handler = std::move(h);
    return *this;
}

void app::listen(std::uint16_t port) {
    listen("0.0.0.0", port);
}

void app::listen(std::string_view address, std::uint16_t port) {
    // Freeze the router
    impl_->group_impl.router.freeze();

    // Build config
    impl_->config.thread_count = impl_->thread_count;
    impl_->config.max_body_size = impl_->max_body_size;
    impl_->config.listen_backlog = impl_->listen_backlog;

    unsigned num_threads = impl_->thread_count;
    if (num_threads == 0) {
        num_threads = std::thread::hardware_concurrency();
        if (num_threads == 0) num_threads = 1;
    }

    auto be = resolve_backend(impl_->backend);
    const char* be_name = (be == voie::backend::io_uring) ? "io_uring" : "epoll";
    std::fprintf(stderr, "voie: listening on %.*s:%u (%u threads, %s)\n",
                 static_cast<int>(address.size()), address.data(), port,
                 num_threads, be_name);

    impl_->total_threads.store(num_threads, std::memory_order_release);

    // Create all event loops
    std::string addr_str{address};
    for (unsigned i = 0; i < num_threads; ++i) {
        impl_->loops.push_back(create_loop(
            be, impl_->group_impl.router, impl_->config,
            &impl_->not_found_handler, &impl_->error_handler));
    }

    if (num_threads == 1) {
        // Single thread: run on the calling thread
        impl_->loops[0]->run(addr_str, port, &impl_->ready_count);
        impl_->loops.clear();
        return;
    }

    // Multi-thread: spawn N threads, each with its own listen socket (SO_REUSEPORT)
    for (unsigned i = 0; i < num_threads; ++i) {
        auto* loop = impl_->loops[i].get();
        auto* ready = &impl_->ready_count;
        impl_->threads.emplace_back([loop, addr_str, port, ready]() {
            loop->run(addr_str, port, ready);
        });
    }

    // Block the main thread — wait for all worker threads
    for (auto& t : impl_->threads) {
        t.join();
    }
    impl_->threads.clear();
}

void app::shutdown() {
    for (auto& loop : impl_->loops) {
        loop->stop();
    }
}

bool app::wait_ready(unsigned timeout_ms) const {
    auto deadline = std::chrono::steady_clock::now()
                    + std::chrono::milliseconds(timeout_ms);

    // Wait for total_threads to be set (listen() may not have started yet)
    while (impl_->total_threads.load(std::memory_order_acquire) == 0) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    unsigned expected = impl_->total_threads.load(std::memory_order_acquire);
    while (impl_->ready_count.load(std::memory_order_acquire) < expected) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

// group non-template methods
group::group(group::impl& impl, std::string prefix, std::vector<middleware_fn> middleware)
    : impl_{&impl}, prefix_{std::move(prefix)}, middleware_{std::move(middleware)} {}

voie::group group::subgroup(std::string_view prefix) {
    std::string full = prefix_;
    full += prefix;
    return voie::group(*impl_, std::move(full), middleware_);
}

void group::add_route(http_method method, std::string_view pattern,
                      std::vector<handler>&& chain) {
    std::string full_pattern = prefix_;
    full_pattern += pattern;
    impl_->router.add_route(method, full_pattern, std::move(chain));
}

void group::add_route_all(std::string_view pattern, std::vector<handler>&& chain) {
    std::string full_pattern = prefix_;
    full_pattern += pattern;

    // Wrap each handler in shared_ptr so we can create cloned chains per method
    std::vector<std::shared_ptr<handler>> shared;
    shared.reserve(chain.size());
    for (auto& h : chain)
        shared.push_back(std::make_shared<handler>(std::move(h)));

    constexpr http_method methods[] = {
        http_method::GET, http_method::POST, http_method::PUT,
        http_method::DELETE, http_method::PATCH, http_method::HEAD,
        http_method::OPTIONS
    };
    for (auto m : methods) {
        std::vector<handler> clone;
        clone.reserve(shared.size());
        for (auto& sp : shared)
            clone.emplace_back([sp](ctx& c) { (*sp)(c); });
        impl_->router.add_route(m, full_pattern, std::move(clone));
    }
}

} // namespace voie
