#pragma once

#include <voie/handler.h>
#include <voie/types.h>

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace voie {

class app;

// Middleware stored as copyable function pointer type for group reuse
using middleware_fn = std::function<void(ctx&)>;

class group {
public:
    template <typename... Fns>
    group& get(std::string_view pattern, Fns&&... fns);

    template <typename... Fns>
    group& post(std::string_view pattern, Fns&&... fns);

    template <typename... Fns>
    group& put(std::string_view pattern, Fns&&... fns);

    template <typename... Fns>
    group& del(std::string_view pattern, Fns&&... fns);

    template <typename... Fns>
    group& patch(std::string_view pattern, Fns&&... fns);

    template <typename... Fns>
    group& all(std::string_view pattern, Fns&&... fns);

    template <typename F>
    group& use(F&& mw);

    [[nodiscard]] voie::group subgroup(std::string_view prefix);

private:
    friend class app;

    struct impl;
    impl* impl_;
    std::string prefix_;
    std::vector<middleware_fn> middleware_;

    group(impl& impl, std::string prefix, std::vector<middleware_fn> middleware);

    void add_route(http_method method, std::string_view pattern,
                   std::vector<handler>&& chain);
    void add_route_all(std::string_view pattern, std::vector<handler>&& chain);

    template <typename... Fns>
    std::vector<handler> build_chain(Fns&&... fns);

    template <typename... Fns>
    group& route(http_method method, std::string_view pattern, Fns&&... fns);
};

// Template implementations

template <typename F>
group& group::use(F&& mw) {
    middleware_.emplace_back(std::forward<F>(mw));
    return *this;
}

template <typename... Fns>
std::vector<handler> group::build_chain(Fns&&... fns) {
    std::vector<handler> chain;
    chain.reserve(middleware_.size() + sizeof...(Fns));
    for (auto& mw : middleware_) {
        chain.emplace_back(handler{mw});
    }
    (chain.emplace_back(std::forward<Fns>(fns)), ...);
    return chain;
}

template <typename... Fns>
group& group::route(http_method method, std::string_view pattern, Fns&&... fns) {
    auto chain = build_chain(std::forward<Fns>(fns)...);
    add_route(method, pattern, std::move(chain));
    return *this;
}

template <typename... Fns>
group& group::get(std::string_view pattern, Fns&&... fns) {
    return route(http_method::GET, pattern, std::forward<Fns>(fns)...);
}

template <typename... Fns>
group& group::post(std::string_view pattern, Fns&&... fns) {
    return route(http_method::POST, pattern, std::forward<Fns>(fns)...);
}

template <typename... Fns>
group& group::put(std::string_view pattern, Fns&&... fns) {
    return route(http_method::PUT, pattern, std::forward<Fns>(fns)...);
}

template <typename... Fns>
group& group::del(std::string_view pattern, Fns&&... fns) {
    return route(http_method::DELETE, pattern, std::forward<Fns>(fns)...);
}

template <typename... Fns>
group& group::patch(std::string_view pattern, Fns&&... fns) {
    return route(http_method::PATCH, pattern, std::forward<Fns>(fns)...);
}

template <typename... Fns>
group& group::all(std::string_view pattern, Fns&&... fns) {
    auto chain = build_chain(std::forward<Fns>(fns)...);
    add_route_all(pattern, std::move(chain));
    return *this;
}

} // namespace voie
