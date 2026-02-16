#pragma once

#include "arena.h"
#include <cstddef>
#include <vector>

namespace voie::detail {

// Thread-local pool of pre-allocated arenas.
// Since each io_loop thread owns its own pool, no locking is needed.
class arena_pool {
public:
    explicit arena_pool(std::size_t initial_count = 256,
                        std::size_t arena_block_size = 8192)
        : block_size_{arena_block_size} {
        pool_.reserve(initial_count);
        for (std::size_t i = 0; i < initial_count; ++i) {
            pool_.emplace_back(arena_block_size);
        }
    }

    // Acquire an arena from the pool. If empty, creates a new one (rare).
    [[nodiscard]] arena acquire() {
        if (pool_.empty()) {
            return arena{block_size_};
        }
        arena a = std::move(pool_.back());
        pool_.pop_back();
        return a;
    }

    // Return an arena to the pool after reset.
    void release(arena&& a) {
        a.reset();
        pool_.push_back(std::move(a));
    }

    [[nodiscard]] std::size_t available() const noexcept { return pool_.size(); }

private:
    std::vector<arena> pool_;
    std::size_t block_size_;
};

} // namespace voie::detail
