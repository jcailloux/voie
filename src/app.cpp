#include <voie/app.h>
#include "router.h"
#include "io_loop.h"

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
    std::vector<middleware_fn> global_middleware;
    handler not_found_handler;
    std::function<void(ctx&, std::exception_ptr)> error_handler;
    group::impl group_impl;
    detail::app_config config;
    std::vector<std::unique_ptr<detail::io_loop>> loops;
    std::vector<std::thread> threads;
};

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

    std::fprintf(stderr, "voie: listening on %.*s:%u (%u threads)\n",
                 static_cast<int>(address.size()), address.data(), port, num_threads);

    // For single thread, run on the main thread
    if (num_threads == 1) {
        detail::io_loop loop(impl_->group_impl.router, impl_->config,
                             &impl_->not_found_handler, &impl_->error_handler);
        loop.run(address, port);
        return;
    }

    // Multi-thread: spawn N threads, each with its own io_loop + listen socket (SO_REUSEPORT)
    std::string addr_str{address};
    for (unsigned i = 0; i < num_threads; ++i) {
        impl_->loops.push_back(std::make_unique<detail::io_loop>(
            impl_->group_impl.router, impl_->config,
            &impl_->not_found_handler, &impl_->error_handler));
    }

    for (unsigned i = 0; i < num_threads; ++i) {
        auto* loop = impl_->loops[i].get();
        impl_->threads.emplace_back([loop, addr_str, port]() {
            loop->run(addr_str, port);
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
    // TODO: handler is move-only, so we can only register one method.
    // For a proper all(), handler needs a clone mechanism.
    impl_->router.add_route(http_method::GET, full_pattern, std::move(chain));
}

} // namespace voie
