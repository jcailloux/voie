#include <catch2/catch_test_macros.hpp>
#include "arena.h"
#include "arena_pool.h"

#include <cstdint>

using namespace voie::detail;

// ============================================================================
// Basic allocation
// ============================================================================

TEST_CASE("arena: initial state", "[arena]") {
    arena a(1024);
    REQUIRE(a.capacity() == 1024);
    REQUIRE(a.used() == 0);
}

TEST_CASE("arena: basic allocation", "[arena]") {
    arena a(1024);
    void* p = a.alloc(100);
    REQUIRE(p != nullptr);
    REQUIRE(a.used() >= 100);
}

TEST_CASE("arena: multiple allocations are contiguous", "[arena]") {
    arena a(1024);
    auto* p1 = static_cast<char*>(a.alloc(8, 1));
    auto* p2 = static_cast<char*>(a.alloc(8, 1));
    REQUIRE(p2 == p1 + 8);
}

TEST_CASE("arena: alignment", "[arena]") {
    arena a(1024);
    (void)a.alloc(1, 1); // misalign by 1 byte

    auto* p = static_cast<char*>(a.alloc(16, 16));
    REQUIRE(reinterpret_cast<std::uintptr_t>(p) % 16 == 0);

    (void)a.alloc(3, 1); // misalign again
    auto* p32 = static_cast<char*>(a.alloc(32, 32));
    REQUIRE(reinterpret_cast<std::uintptr_t>(p32) % 32 == 0);
}

// ============================================================================
// dup
// ============================================================================

TEST_CASE("arena: dup copies string", "[arena]") {
    arena a(1024);
    std::string_view original = "hello, world";
    std::string_view copy = a.dup(original);

    REQUIRE(copy == original);
    REQUIRE(copy.data() != original.data());
}

TEST_CASE("arena: dup empty string", "[arena]") {
    arena a(1024);
    std::string_view copy = a.dup("");
    REQUIRE(copy.empty());
}

// ============================================================================
// Reset
// ============================================================================

TEST_CASE("arena: reset reclaims memory", "[arena]") {
    arena a(1024);
    (void)a.alloc(100);
    REQUIRE(a.used() > 0);

    a.reset();
    REQUIRE(a.used() == 0);
}

TEST_CASE("arena: reset frees overflow blocks", "[arena]") {
    arena a(64);
    (void)a.alloc(32);
    (void)a.alloc(32);
    (void)a.alloc(64); // overflow

    a.reset();
    REQUIRE(a.used() == 0);

    // Should be able to allocate again from the main block
    void* p = a.alloc(32);
    REQUIRE(p != nullptr);
}

// ============================================================================
// Overflow
// ============================================================================

TEST_CASE("arena: overflow chains new block", "[arena]") {
    arena a(64);
    void* p1 = a.alloc(32);
    void* p2 = a.alloc(32);
    void* p3 = a.alloc(64); // overflow block

    REQUIRE(p1 != nullptr);
    REQUIRE(p2 != nullptr);
    REQUIRE(p3 != nullptr);
}

TEST_CASE("arena: multiple overflows", "[arena]") {
    arena a(32);
    for (int i = 0; i < 10; ++i) {
        void* p = a.alloc(32);
        REQUIRE(p != nullptr);
    }
}

TEST_CASE("arena: large allocation exceeding block size", "[arena]") {
    arena a(64);
    void* p = a.alloc(1024);
    REQUIRE(p != nullptr);
}

// ============================================================================
// make<T>
// ============================================================================

TEST_CASE("arena: make constructs object", "[arena]") {
    arena a(1024);
    struct point { int x; int y; };
    auto* p = a.make<point>(10, 20);

    REQUIRE(p->x == 10);
    REQUIRE(p->y == 20);
}

// ============================================================================
// Move semantics
// ============================================================================

TEST_CASE("arena: move constructor", "[arena]") {
    arena a(1024);
    auto* p = static_cast<char*>(a.alloc(8, 1));
    p[0] = 'X';

    arena b(std::move(a));
    REQUIRE(b.capacity() == 1024);
    REQUIRE(b.used() == 8);
    REQUIRE(a.capacity() == 0);
}

TEST_CASE("arena: move assignment", "[arena]") {
    arena a(1024);
    (void)a.alloc(8);

    arena b(512);
    b = std::move(a);
    REQUIRE(b.capacity() == 1024);
    REQUIRE(a.capacity() == 0);
}

// ============================================================================
// Pool
// ============================================================================

TEST_CASE("arena_pool: acquire and release", "[arena_pool]") {
    arena_pool pool(4, 1024);
    REQUIRE(pool.available() == 4);

    auto a = pool.acquire();
    REQUIRE(pool.available() == 3);

    (void)a.alloc(100);
    pool.release(std::move(a));
    REQUIRE(pool.available() == 4);
}

TEST_CASE("arena_pool: release resets arena", "[arena_pool]") {
    arena_pool pool(1, 1024);
    auto a = pool.acquire();
    (void)a.alloc(500);
    pool.release(std::move(a));

    // Re-acquire — should be reset
    auto b = pool.acquire();
    REQUIRE(b.used() == 0);
}

TEST_CASE("arena_pool: creates new arena when empty", "[arena_pool]") {
    arena_pool pool(1, 1024);
    auto a1 = pool.acquire();
    REQUIRE(pool.available() == 0);

    auto a2 = pool.acquire(); // creates new
    REQUIRE(a2.capacity() == 1024);
}

// ============================================================================
// Edge cases
// ============================================================================

TEST_CASE("arena: zero-size allocation", "[arena]") {
    arena a(1024);
    void* p = a.alloc(0);
    REQUIRE(p != nullptr);
    // Should not advance offset meaningfully
    REQUIRE(a.used() <= 8); // at most alignment padding
}

TEST_CASE("arena: large alignment", "[arena]") {
    arena a(8192);
    auto* p = static_cast<char*>(a.alloc(64, 4096));
    REQUIRE(reinterpret_cast<std::uintptr_t>(p) % 4096 == 0);
}

TEST_CASE("arena: dup large string triggers overflow", "[arena]") {
    arena a(64);
    std::string large(200, 'X');
    auto copy = a.dup(large);
    REQUIRE(copy == large);
    REQUIRE(copy.data() != large.data());
}

TEST_CASE("arena: alloc after reset reuses main block", "[arena]") {
    arena a(1024);
    auto* p1 = a.alloc(100);
    a.reset();
    auto* p2 = a.alloc(100);
    // Should reuse the same block (pointers may be equal)
    REQUIRE(p1 == p2);
}

TEST_CASE("arena: make multiple objects", "[arena]") {
    arena a(1024);
    struct pair { int x; int y; };
    auto* a1 = a.make<pair>(1, 2);
    auto* a2 = a.make<pair>(3, 4);
    auto* a3 = a.make<pair>(5, 6);
    REQUIRE(a1->x == 1);
    REQUIRE(a2->x == 3);
    REQUIRE(a3->x == 5);
    // All should be in the same arena block
    REQUIRE(reinterpret_cast<char*>(a3) > reinterpret_cast<char*>(a1));
}

// ============================================================================
// Pool stress
// ============================================================================

TEST_CASE("arena_pool: acquire all and release all", "[arena_pool]") {
    arena_pool pool(8, 512);
    std::vector<arena> arenas;
    for (int i = 0; i < 8; ++i) {
        arenas.push_back(pool.acquire());
    }
    REQUIRE(pool.available() == 0);

    for (auto& a : arenas) {
        pool.release(std::move(a));
    }
    REQUIRE(pool.available() == 8);
}

TEST_CASE("arena_pool: acquire more than initial count", "[arena_pool]") {
    arena_pool pool(2, 512);
    auto a1 = pool.acquire();
    auto a2 = pool.acquire();
    auto a3 = pool.acquire(); // dynamically created
    auto a4 = pool.acquire(); // dynamically created
    REQUIRE(a3.capacity() == 512);
    REQUIRE(a4.capacity() == 512);
}