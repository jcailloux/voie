#pragma once

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string_view>
#include <utility>

namespace voie::detail {

class arena {
public:
    explicit arena(std::size_t block_size = 8192)
        : block_size_{block_size} {
        base_ = static_cast<char*>(std::malloc(block_size));
        if (!base_) throw std::bad_alloc{};
        cap_ = block_size;
    }

    ~arena() {
        // Free overflow blocks (chained via first 8 bytes)
        block* b = overflow_;
        while (b) {
            block* next = b->next;
            std::free(b);
            b = next;
        }
        std::free(base_);
    }

    arena(const arena&) = delete;
    arena& operator=(const arena&) = delete;

    arena(arena&& other) noexcept
        : base_{other.base_}, cap_{other.cap_}, offset_{other.offset_},
          block_size_{other.block_size_}, overflow_{other.overflow_} {
        other.base_ = nullptr;
        other.cap_ = 0;
        other.offset_ = 0;
        other.overflow_ = nullptr;
    }

    arena& operator=(arena&& other) noexcept {
        if (this != &other) {
            block* b = overflow_;
            while (b) { block* next = b->next; std::free(b); b = next; }
            std::free(base_);

            base_ = other.base_;
            cap_ = other.cap_;
            offset_ = other.offset_;
            block_size_ = other.block_size_;
            overflow_ = other.overflow_;

            other.base_ = nullptr;
            other.cap_ = 0;
            other.offset_ = 0;
            other.overflow_ = nullptr;
        }
        return *this;
    }

    [[nodiscard]] void* alloc(std::size_t n, std::size_t align = 8) {
        // Align the absolute address, not just the offset
        std::size_t addr = reinterpret_cast<std::size_t>(base_) + offset_;
        std::size_t aligned_addr = (addr + align - 1) & ~(align - 1);
        std::size_t aligned_offset = aligned_addr - reinterpret_cast<std::size_t>(base_);
        if (aligned_offset + n <= cap_) {
            offset_ = aligned_offset + n;
            return reinterpret_cast<char*>(aligned_addr);
        }
        return alloc_overflow(n, align);
    }

    template <typename T, typename... Args>
    [[nodiscard]] T* make(Args&&... args) {
        void* mem = alloc(sizeof(T), alignof(T));
        return ::new (mem) T(std::forward<Args>(args)...);
    }

    [[nodiscard]] std::string_view dup(std::string_view sv) {
        if (sv.empty()) return {};
        char* p = static_cast<char*>(alloc(sv.size(), 1));
        std::memcpy(p, sv.data(), sv.size());
        return {p, sv.size()};
    }

    void reset() noexcept {
        offset_ = 0;
        // Free overflow blocks but keep the main block
        block* b = overflow_;
        while (b) {
            block* next = b->next;
            std::free(b);
            b = next;
        }
        overflow_ = nullptr;
    }

    [[nodiscard]] std::size_t used() const noexcept { return offset_; }
    [[nodiscard]] std::size_t capacity() const noexcept { return cap_; }

private:
    struct block {
        block* next;
        // data follows
    };

    [[nodiscard]] void* alloc_overflow(std::size_t n, std::size_t align) {
        std::size_t block_data_size = n > block_size_ ? n : block_size_;
        std::size_t total = sizeof(block) + align + block_data_size;
        auto* b = static_cast<block*>(std::malloc(total));
        if (!b) throw std::bad_alloc{};
        b->next = overflow_;
        overflow_ = b;

        char* data = reinterpret_cast<char*>(b) + sizeof(block);
        std::size_t data_addr = reinterpret_cast<std::size_t>(data);
        std::size_t aligned_addr = (data_addr + align - 1) & ~(align - 1);
        return reinterpret_cast<void*>(aligned_addr);
    }

    char* base_ = nullptr;
    std::size_t cap_ = 0;
    std::size_t offset_ = 0;
    std::size_t block_size_;
    block* overflow_ = nullptr;
};

} // namespace voie::detail
