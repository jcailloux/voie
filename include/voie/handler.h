#pragma once

#include <cstddef>
#include <cstring>
#include <new>
#include <type_traits>
#include <utility>

namespace voie {

class ctx;

class handler {
public:
    using invoke_fn = void(*)(void*, ctx&);
    using destroy_fn = void(*)(void*);

    handler() noexcept : invoke_{nullptr}, destroy_{nullptr} {}

    template <typename F>
        requires std::is_invocable_v<F, ctx&> && (!std::is_same_v<std::decay_t<F>, handler>)
    handler(F&& f) {
        using T = std::decay_t<F>;
        if constexpr (sizeof(T) <= buf_size && alignof(T) <= alignof(std::max_align_t)
                      && std::is_nothrow_move_constructible_v<T>) {
            ::new (buf_) T(std::forward<F>(f));
            invoke_ = [](void* p, ctx& c) { (*static_cast<T*>(p))(c); };
            destroy_ = [](void* p) { static_cast<T*>(p)->~T(); };
            heap_ = false;
        } else {
            auto* ptr = new T(std::forward<F>(f));
            std::memcpy(buf_, &ptr, sizeof(ptr));
            invoke_ = [](void* p, ctx& c) {
                T* real;
                std::memcpy(&real, p, sizeof(real));
                (*real)(c);
            };
            destroy_ = [](void* p) {
                T* real;
                std::memcpy(&real, p, sizeof(real));
                delete real;
            };
            heap_ = true;
        }
    }

    ~handler() {
        if (destroy_) {
            destroy_(buf_);
        }
    }

    handler(handler&& other) noexcept
        : invoke_{other.invoke_}, destroy_{other.destroy_}, heap_{other.heap_} {
        std::memcpy(buf_, other.buf_, buf_size);
        other.invoke_ = nullptr;
        other.destroy_ = nullptr;
    }

    handler& operator=(handler&& other) noexcept {
        if (this != &other) {
            if (destroy_) destroy_(buf_);
            invoke_ = other.invoke_;
            destroy_ = other.destroy_;
            heap_ = other.heap_;
            std::memcpy(buf_, other.buf_, buf_size);
            other.invoke_ = nullptr;
            other.destroy_ = nullptr;
        }
        return *this;
    }

    handler(const handler&) = delete;
    handler& operator=(const handler&) = delete;

    void operator()(ctx& c) const {
        invoke_(const_cast<void*>(static_cast<const void*>(buf_)), c);
    }

    explicit operator bool() const noexcept { return invoke_ != nullptr; }

private:
    static constexpr std::size_t buf_size = 24;
    alignas(std::max_align_t) char buf_[buf_size]{};
    invoke_fn invoke_;
    destroy_fn destroy_;
    bool heap_ = false;
};

} // namespace voie
