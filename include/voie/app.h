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

class app {
public:
    app();
    ~app();

    app(const app&) = delete;
    app& operator=(const app&) = delete;

    // -- Configuration --
    app& threads(unsigned n);
    app& max_body(std::size_t bytes);
    app& backlog(int n);
    app& sqpoll(bool enable);

    // -- Route registration --
    template <typename... Fns>
    app& get(std::string_view pattern, Fns&&... fns);

    template <typename... Fns>
    app& post(std::string_view pattern, Fns&&... fns);

    template <typename... Fns>
    app& put(std::string_view pattern, Fns&&... fns);

    template <typename... Fns>
    app& del(std::string_view pattern, Fns&&... fns);

    template <typename... Fns>
    app& patch(std::string_view pattern, Fns&&... fns);

    template <typename... Fns>
    app& all(std::string_view pattern, Fns&&... fns);

    // -- Middleware --
    app& use(handler mw);

    // -- Groups --
    [[nodiscard]] voie::group group(std::string_view prefix);

    // -- Error handling --
    app& not_found(handler h);
    app& on_error(std::function<void(ctx&, std::exception_ptr)> h);

    // -- Start server (blocking) --
    void listen(std::uint16_t port);
    void listen(std::string_view address, std::uint16_t port);

    // -- Graceful shutdown --
    void shutdown();

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
