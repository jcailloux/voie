#pragma once

#include <voie/handler.h>
#include <voie/types.h>

#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace voie::detail {

struct param_entry {
    std::string_view key;
    std::string_view value;
};

struct route_match {
    const handler* handlers = nullptr;
    std::uint8_t handler_count = 0;
    param_entry params[8]{};
    std::uint8_t param_count = 0;
};

class router {
public:
    router();
    ~router();

    router(const router&) = delete;
    router& operator=(const router&) = delete;
    router(router&&) noexcept;
    router& operator=(router&&) noexcept;

    void add_route(http_method method, std::string_view pattern,
                   std::vector<handler>&& chain);

    void freeze();

    [[nodiscard]]
    std::expected<route_match, std::monostate> lookup(
        http_method method, std::string_view path) const noexcept;

private:
    struct trie_node;

    void insert(trie_node* node, std::string_view path,
                http_method method, std::vector<handler>&& chain);

    bool search(const trie_node* node, std::string_view path,
                http_method method, route_match& match) const noexcept;

    std::unique_ptr<trie_node> root_;
    bool frozen_ = false;
};

} // namespace voie::detail
