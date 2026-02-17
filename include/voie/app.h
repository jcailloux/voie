#pragma once

#include <voie/handler.h>
#include <voie/group.h>
#include <voie/types.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>

namespace voie {

class ctx;

/// I/O backend used by the event loop.
enum class backend : std::uint8_t {
    auto_detect,  ///< io_uring if available, epoll otherwise
    io_uring,     ///< Linux io_uring (kernel 5.19+)
    epoll,        ///< Linux epoll (always available)
};

/// HTTP server application.
///
/// Central object that owns configuration, routes, middleware, and the
/// event-loop threads.  Non-copyable; a single instance should live for
/// the entire lifetime of the server.
///
/// All configuration and route-registration methods return `*this` so
/// calls can be chained:
/// @code
/// voie::app a;
/// a.threads(4)
///  .max_body(4096)
///  .get("/", [](voie::ctx& c) { c.text("hello"); })
///  .listen(8080);
/// @endcode
class app {
public:
    app();
    ~app();

    app(const app&) = delete;
    app& operator=(const app&) = delete;

    // -- Configuration -------------------------------------------------------

    /// Set the number of worker threads.
    /// Each thread runs its own io_uring event loop with a dedicated listen
    /// socket (`SO_REUSEPORT`).  Pass 0 (the default) to use
    /// `std::thread::hardware_concurrency()`.
    /// @param n  Thread count (0 = auto-detect).
    /// @return `*this` for chaining.
    app& threads(unsigned n);

    /// Set the maximum allowed request body size in bytes.
    /// Requests whose `Content-Length` exceeds this limit receive a
    /// `413 Payload Too Large` response.  Default: 1 MiB.
    /// @param bytes  Maximum body size.
    /// @return `*this` for chaining.
    app& max_body(std::size_t bytes);

    /// Set the TCP listen backlog depth.
    /// Passed directly to `listen(2)`.  Default: 512.
    /// @param n  Backlog size.
    /// @return `*this` for chaining.
    app& backlog(int n);

    /// Enable or disable io_uring `SQPOLL` mode.
    /// When enabled the kernel polls the submission queue from a dedicated
    /// thread, reducing syscall overhead at the cost of one busy CPU core.
    /// Ignored when the epoll backend is active.  Default: off.
    /// @param enable  `true` to enable, `false` to disable.
    /// @return `*this` for chaining.
    app& sqpoll(bool enable);

    /// Select the I/O backend.
    /// Default: `backend::auto_detect` (io_uring if available, epoll otherwise).
    /// @param b  The backend to use.
    /// @return `*this` for chaining.
    app& set_backend(voie::backend b);

    /// Check whether a specific backend is available at runtime.
    /// Useful in tests to SKIP when io_uring is unavailable (e.g. Docker).
    [[nodiscard]] static bool backend_available(voie::backend b);

    // -- Route registration --------------------------------------------------

    /// Register a handler chain for GET requests.
    /// @param pattern  Route pattern (e.g. `"/users/:id"`, `"/files/*path"`).
    /// @param fns      One or more callables `void(ctx&)`.  When several are
    ///                 given they form a middleware chain; each must call
    ///                 `ctx::next()` to invoke the following handler.
    /// @return `*this` for chaining.
    template <typename... Fns>
    app& get(std::string_view pattern, Fns&&... fns);

    /// Register a handler chain for POST requests.
    /// @copydetails get()
    template <typename... Fns>
    app& post(std::string_view pattern, Fns&&... fns);

    /// Register a handler chain for PUT requests.
    /// @copydetails get()
    template <typename... Fns>
    app& put(std::string_view pattern, Fns&&... fns);

    /// Register a handler chain for DELETE requests.
    /// @copydetails get()
    template <typename... Fns>
    app& del(std::string_view pattern, Fns&&... fns);

    /// Register a handler chain for PATCH requests.
    /// @copydetails get()
    template <typename... Fns>
    app& patch(std::string_view pattern, Fns&&... fns);

    /// Register a handler chain for all seven HTTP methods.
    /// @copydetails get()
    template <typename... Fns>
    app& all(std::string_view pattern, Fns&&... fns);

    // -- Middleware -----------------------------------------------------------

    /// Append a global middleware that runs before every route handler.
    /// The middleware must call `ctx::next()` to continue the chain.
    /// @param mw  Callable `void(ctx&)`.
    /// @return `*this` for chaining.
    app& use(handler mw);

    // -- Groups --------------------------------------------------------------

    /// Create a route group with a common path prefix.
    /// Routes registered on the returned group are prefixed with @p prefix.
    /// @param prefix  Path prefix (e.g. `"/api/v1"`).
    /// @return A new group object.
    [[nodiscard]] voie::group group(std::string_view prefix);

    // -- Error handling ------------------------------------------------------

    /// Set the handler invoked when no route matches the request.
    /// If not set, the server replies with a plain `404 Not Found`.
    /// @param h  Callable `void(ctx&)`.
    /// @return `*this` for chaining.
    app& not_found(handler h);

    /// Set the handler invoked when a route handler throws an exception.
    /// If not set, the server replies with a plain `500 Internal Server Error`.
    /// @param h  Callable receiving the context and the captured exception.
    /// @return `*this` for chaining.
    app& on_error(std::function<void(ctx&, std::exception_ptr)> h);

    // -- Lifecycle -----------------------------------------------------------

    /// Start the server and block until shutdown.
    /// Binds to `0.0.0.0` on the given port.
    /// @param port  TCP port to listen on.
    void listen(std::uint16_t port);

    /// Start the server and block until shutdown.
    /// @param address  IPv4 address to bind to (e.g. `"127.0.0.1"`).
    /// @param port     TCP port to listen on.
    void listen(std::string_view address, std::uint16_t port);

    /// Request a graceful shutdown of every event-loop thread.
    /// Safe to call from a signal handler or another thread.
    void shutdown();

    /// Block until all worker threads have finished initialization.
    /// Useful in tests to ensure the server is ready before sending requests.
    /// @param timeout_ms  Maximum wait time in milliseconds (default 5 000).
    /// @return `true` if all threads are ready, `false` on timeout.
    bool wait_ready(unsigned timeout_ms = 5000) const;

private:
    struct impl;
    std::unique_ptr<impl> impl_;

    voie::group root_group();

};

// Template implementations
template <typename... Fns>
app& app::get(std::string_view pattern, Fns&&... fns) {
    auto g = root_group();
    g.get(pattern, std::forward<Fns>(fns)...);
    return *this;
}

template <typename... Fns>
app& app::post(std::string_view pattern, Fns&&... fns) {
    auto g = root_group();
    g.post(pattern, std::forward<Fns>(fns)...);
    return *this;
}

template <typename... Fns>
app& app::put(std::string_view pattern, Fns&&... fns) {
    auto g = root_group();
    g.put(pattern, std::forward<Fns>(fns)...);
    return *this;
}

template <typename... Fns>
app& app::del(std::string_view pattern, Fns&&... fns) {
    auto g = root_group();
    g.del(pattern, std::forward<Fns>(fns)...);
    return *this;
}

template <typename... Fns>
app& app::patch(std::string_view pattern, Fns&&... fns) {
    auto g = root_group();
    g.patch(pattern, std::forward<Fns>(fns)...);
    return *this;
}

template <typename... Fns>
app& app::all(std::string_view pattern, Fns&&... fns) {
    auto g = root_group();
    g.all(pattern, std::forward<Fns>(fns)...);
    return *this;
}

} // namespace voie