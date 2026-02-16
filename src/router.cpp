#include "router.h"
#include <algorithm>
#include <cassert>
#include <stdexcept>

namespace voie::detail {

// Each node stores one edge label (path segment) and optionally handlers per method.
struct router::trie_node {
    std::string path;             // edge label
    std::vector<std::unique_ptr<trie_node>> children;
    std::string param_name;       // name of :param or *wildcard (empty for static)

    enum class type : std::uint8_t { static_node, param, catch_all };
    type node_type = type::static_node;

    // Handler chains per HTTP method (indexed by http_method enum)
    struct method_entry {
        std::vector<handler> chain;
    };
    method_entry methods[http_method_count]{};

    bool has_handler(http_method m) const {
        return !methods[static_cast<int>(m)].chain.empty();
    }
};

router::router() : root_(std::make_unique<trie_node>()) {
    root_->path = "/";
}

router::~router() = default;
router::router(router&&) noexcept = default;
router& router::operator=(router&&) noexcept = default;

// Find the length of the common prefix between two strings
static std::size_t common_prefix_len(std::string_view a, std::string_view b) {
    std::size_t n = std::min(a.size(), b.size());
    for (std::size_t i = 0; i < n; ++i) {
        if (a[i] != b[i]) return i;
    }
    return n;
}

void router::add_route(http_method method, std::string_view pattern,
                        std::vector<handler>&& chain) {
    if (frozen_) throw std::logic_error("cannot add routes after freeze()");
    if (pattern.empty() || pattern[0] != '/') {
        throw std::invalid_argument("route pattern must start with /");
    }
    insert(root_.get(), pattern.substr(1), method, std::move(chain));
}

void router::insert(trie_node* node, std::string_view path,
                     http_method method, std::vector<handler>&& chain) {
    // If path is empty, we've arrived at the target node
    if (path.empty()) {
        if (node->has_handler(method)) {
            throw std::logic_error("duplicate route for method");
        }
        node->methods[static_cast<int>(method)].chain = std::move(chain);
        return;
    }

    // Check for parameter segment (:name)
    if (path[0] == ':') {
        auto slash = path.find('/');
        std::string param_name{path.substr(1, slash == std::string_view::npos ? std::string_view::npos : slash - 1)};
        std::string_view remaining = slash != std::string_view::npos ? path.substr(slash + 1) : "";

        // Look for existing param child
        for (auto& child : node->children) {
            if (child->node_type == trie_node::type::param) {
                if (child->param_name != param_name) {
                    throw std::logic_error("conflicting parameter names: :" +
                                           child->param_name + " vs :" + param_name);
                }
                insert(child.get(), remaining, method, std::move(chain));
                return;
            }
        }

        // Create new param node
        auto child = std::make_unique<trie_node>();
        child->node_type = trie_node::type::param;
        child->param_name = std::move(param_name);
        auto* ptr = child.get();
        node->children.push_back(std::move(child));
        insert(ptr, remaining, method, std::move(chain));
        return;
    }

    // Check for catch-all (*name)
    if (path[0] == '*') {
        std::string param_name{path.substr(1)};

        for (auto& child : node->children) {
            if (child->node_type == trie_node::type::catch_all) {
                throw std::logic_error("duplicate catch-all route");
            }
        }

        auto child = std::make_unique<trie_node>();
        child->node_type = trie_node::type::catch_all;
        child->param_name = std::move(param_name);
        child->methods[static_cast<int>(method)].chain = std::move(chain);
        node->children.push_back(std::move(child));
        return;
    }

    // Static segment — find matching child by common prefix
    for (auto& child : node->children) {
        if (child->node_type != trie_node::type::static_node) continue;
        if (child->path.empty()) continue;

        std::size_t cp = common_prefix_len(child->path, path);
        if (cp == 0) continue;

        // Full match of child's path
        if (cp == child->path.size()) {
            std::string_view remaining = path.substr(cp);
            // Skip the '/' separator if present
            if (!remaining.empty() && remaining[0] == '/') {
                remaining = remaining.substr(1);
            }
            insert(child.get(), remaining, method, std::move(chain));
            return;
        }

        // Partial match — need to split the edge
        // Create a new node for the suffix of the existing child
        auto split_child = std::make_unique<trie_node>();
        split_child->path = child->path.substr(cp);
        split_child->children = std::move(child->children);
        split_child->param_name = std::move(child->param_name);
        split_child->node_type = child->node_type;
        for (int i = 0; i < http_method_count; ++i) {
            split_child->methods[i] = std::move(child->methods[i]);
        }

        // Current child becomes the common prefix
        child->path = std::string(child->path.substr(0, cp));
        child->children.clear();
        child->param_name.clear();
        child->node_type = trie_node::type::static_node;
        for (auto& m : child->methods) m.chain.clear();
        child->children.push_back(std::move(split_child));

        // Insert the new path suffix
        std::string_view remaining = path.substr(cp);
        if (!remaining.empty() && remaining[0] == '/') {
            remaining = remaining.substr(1);
        }
        if (remaining.empty()) {
            child->methods[static_cast<int>(method)].chain = std::move(chain);
        } else {
            insert(child.get(), remaining, method, std::move(chain));
        }
        return;
    }

    // No matching child — create a new one
    // Build the full static path up to next param/wildcard/slash
    auto next_special = path.find_first_of(":*/");
    std::string_view static_part;
    std::string_view remaining;

    if (next_special == std::string_view::npos) {
        static_part = path;
    } else if (path[next_special] == '/') {
        static_part = path.substr(0, next_special);
        remaining = path.substr(next_special + 1);
    } else {
        // param or catch-all right at start would have been caught above
        static_part = path.substr(0, next_special);
        remaining = path.substr(next_special);
    }

    auto child = std::make_unique<trie_node>();
    child->path = std::string(static_part);
    child->node_type = trie_node::type::static_node;
    auto* ptr = child.get();
    node->children.push_back(std::move(child));

    if (remaining.empty()) {
        ptr->methods[static_cast<int>(method)].chain = std::move(chain);
    } else {
        insert(ptr, remaining, method, std::move(chain));
    }
}

void router::freeze() {
    frozen_ = true;
}

std::expected<route_match, std::monostate> router::lookup(
    http_method method, std::string_view path) const noexcept {
    if (path.empty() || path[0] != '/') return std::unexpected(std::monostate{});

    route_match match{};
    if (search(root_.get(), path.substr(1), method, match)) {
        return match;
    }
    return std::unexpected(std::monostate{});
}

bool router::search(const trie_node* node, std::string_view path,
                     http_method method, route_match& match) const noexcept {
    // If path is consumed, check for handler at this node
    if (path.empty()) {
        auto& entry = node->methods[static_cast<int>(method)];
        if (!entry.chain.empty()) {
            match.handlers = entry.chain.data();
            match.handler_count = static_cast<std::uint8_t>(entry.chain.size());
            return true;
        }
        return false;
    }

    // Try static children first (highest priority)
    for (const auto& child : node->children) {
        if (child->node_type != trie_node::type::static_node) continue;

        if (path.starts_with(child->path)) {
            std::string_view remaining = path.substr(child->path.size());
            if (!remaining.empty() && remaining[0] == '/') {
                remaining = remaining.substr(1);
            }
            if (search(child.get(), remaining, method, match)) {
                return true;
            }
        }
    }

    // Try param children (:name)
    for (const auto& child : node->children) {
        if (child->node_type != trie_node::type::param) continue;

        // Consume up to next '/'
        auto slash = path.find('/');
        std::string_view value = path.substr(0, slash);
        std::string_view remaining = slash != std::string_view::npos ? path.substr(slash + 1) : "";

        if (value.empty()) continue;

        // Temporarily add param
        if (match.param_count < 8) {
            auto idx = match.param_count++;
            match.params[idx].key = child->param_name;
            match.params[idx].value = value;

            if (search(child.get(), remaining, method, match)) {
                return true;
            }

            // Backtrack
            match.param_count--;
        }
    }

    // Try catch-all children (*name) — lowest priority
    for (const auto& child : node->children) {
        if (child->node_type != trie_node::type::catch_all) continue;

        auto& entry = child->methods[static_cast<int>(method)];
        if (!entry.chain.empty()) {
            if (match.param_count < 8) {
                match.params[match.param_count].key = child->param_name;
                match.params[match.param_count].value = path;
                match.param_count++;
            }
            match.handlers = entry.chain.data();
            match.handler_count = static_cast<std::uint8_t>(entry.chain.size());
            return true;
        }
    }

    return false;
}

} // namespace voie::detail
