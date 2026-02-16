#include <catch2/catch_test_macros.hpp>
#include "router.h"

using namespace voie;
using namespace voie::detail;

// Helper: create a dummy handler chain of size 1
static std::vector<handler> make_chain() {
    std::vector<handler> chain;
    chain.emplace_back([](ctx&) {});
    return chain;
}

TEST_CASE("router: root route /", "[router]") {
    router r;
    r.add_route(http_method::GET, "/", make_chain());
    r.freeze();

    auto result = r.lookup(http_method::GET, "/");
    REQUIRE(result.has_value());
    REQUIRE(result->handler_count == 1);
    REQUIRE(result->param_count == 0);
}

TEST_CASE("router: simple static routes", "[router]") {
    router r;
    r.add_route(http_method::GET, "/hello", make_chain());
    r.add_route(http_method::GET, "/world", make_chain());
    r.freeze();

    REQUIRE(r.lookup(http_method::GET, "/hello").has_value());
    REQUIRE(r.lookup(http_method::GET, "/world").has_value());
    REQUIRE(!r.lookup(http_method::GET, "/missing").has_value());
}

TEST_CASE("router: nested static routes", "[router]") {
    router r;
    r.add_route(http_method::GET, "/api/users", make_chain());
    r.add_route(http_method::GET, "/api/posts", make_chain());
    r.add_route(http_method::POST, "/api/users", make_chain());
    r.freeze();

    REQUIRE(r.lookup(http_method::GET, "/api/users").has_value());
    REQUIRE(r.lookup(http_method::GET, "/api/posts").has_value());
    REQUIRE(r.lookup(http_method::POST, "/api/users").has_value());
    REQUIRE(!r.lookup(http_method::POST, "/api/posts").has_value());
    REQUIRE(!r.lookup(http_method::GET, "/api").has_value());
}

TEST_CASE("router: route with parameter", "[router]") {
    router r;
    r.add_route(http_method::GET, "/users/:id", make_chain());
    r.freeze();

    auto result = r.lookup(http_method::GET, "/users/42");
    REQUIRE(result.has_value());
    REQUIRE(result->param_count == 1);
    REQUIRE(result->params[0].key == "id");
    REQUIRE(result->params[0].value == "42");
}

TEST_CASE("router: multiple parameters", "[router]") {
    router r;
    r.add_route(http_method::GET, "/users/:user_id/posts/:post_id", make_chain());
    r.freeze();

    auto result = r.lookup(http_method::GET, "/users/5/posts/99");
    REQUIRE(result.has_value());
    REQUIRE(result->param_count == 2);
    REQUIRE(result->params[0].key == "user_id");
    REQUIRE(result->params[0].value == "5");
    REQUIRE(result->params[1].key == "post_id");
    REQUIRE(result->params[1].value == "99");
}

TEST_CASE("router: catch-all wildcard", "[router]") {
    router r;
    r.add_route(http_method::GET, "/static/*filepath", make_chain());
    r.freeze();

    auto result = r.lookup(http_method::GET, "/static/css/style.css");
    REQUIRE(result.has_value());
    REQUIRE(result->param_count == 1);
    REQUIRE(result->params[0].key == "filepath");
    REQUIRE(result->params[0].value == "css/style.css");
}

TEST_CASE("router: static route takes priority over param", "[router]") {
    router r;
    r.add_route(http_method::GET, "/users/me", make_chain());
    r.add_route(http_method::GET, "/users/:id", make_chain());
    r.freeze();

    // /users/me should match the static route
    auto result_me = r.lookup(http_method::GET, "/users/me");
    REQUIRE(result_me.has_value());
    REQUIRE(result_me->param_count == 0);

    // /users/42 should match the param route
    auto result_42 = r.lookup(http_method::GET, "/users/42");
    REQUIRE(result_42.has_value());
    REQUIRE(result_42->param_count == 1);
    REQUIRE(result_42->params[0].value == "42");
}

TEST_CASE("router: different methods same path", "[router]") {
    router r;
    r.add_route(http_method::GET, "/users", make_chain());
    r.add_route(http_method::POST, "/users", make_chain());
    r.add_route(http_method::DELETE, "/users", make_chain());
    r.freeze();

    REQUIRE(r.lookup(http_method::GET, "/users").has_value());
    REQUIRE(r.lookup(http_method::POST, "/users").has_value());
    REQUIRE(r.lookup(http_method::DELETE, "/users").has_value());
    REQUIRE(!r.lookup(http_method::PUT, "/users").has_value());
}

TEST_CASE("router: 404 for unknown path", "[router]") {
    router r;
    r.add_route(http_method::GET, "/exists", make_chain());
    r.freeze();

    REQUIRE(!r.lookup(http_method::GET, "/not-exists").has_value());
    REQUIRE(!r.lookup(http_method::GET, "").has_value());
    REQUIRE(!r.lookup(http_method::GET, "no-slash").has_value());
}

TEST_CASE("router: edge splitting with overlapping prefixes", "[router]") {
    router r;
    r.add_route(http_method::GET, "/api/users", make_chain());
    r.add_route(http_method::GET, "/api/updates", make_chain());
    r.freeze();

    REQUIRE(r.lookup(http_method::GET, "/api/users").has_value());
    REQUIRE(r.lookup(http_method::GET, "/api/updates").has_value());
    REQUIRE(!r.lookup(http_method::GET, "/api/u").has_value());
}

TEST_CASE("router: handler chain size preserved", "[router]") {
    router r;
    std::vector<handler> chain;
    chain.emplace_back([](ctx&) {}); // middleware
    chain.emplace_back([](ctx&) {}); // middleware
    chain.emplace_back([](ctx&) {}); // handler
    r.add_route(http_method::GET, "/test", std::move(chain));
    r.freeze();

    auto result = r.lookup(http_method::GET, "/test");
    REQUIRE(result.has_value());
    REQUIRE(result->handler_count == 3);
}

TEST_CASE("router: param route does not match empty segment", "[router]") {
    router r;
    r.add_route(http_method::GET, "/users/:id", make_chain());
    r.freeze();

    // /users/ has an empty segment for :id — should not match
    REQUIRE(!r.lookup(http_method::GET, "/users/").has_value());
}

// ============================================================================
// Error handling
// ============================================================================

TEST_CASE("router: add_route after freeze throws", "[router]") {
    router r;
    r.add_route(http_method::GET, "/", make_chain());
    r.freeze();

    REQUIRE_THROWS_AS(
        r.add_route(http_method::GET, "/new", make_chain()),
        std::logic_error
    );
}

TEST_CASE("router: pattern not starting with / throws", "[router]") {
    router r;
    REQUIRE_THROWS_AS(
        r.add_route(http_method::GET, "no-slash", make_chain()),
        std::invalid_argument
    );
    REQUIRE_THROWS_AS(
        r.add_route(http_method::GET, "", make_chain()),
        std::invalid_argument
    );
}

TEST_CASE("router: duplicate route same method throws", "[router]") {
    router r;
    r.add_route(http_method::GET, "/dup", make_chain());
    REQUIRE_THROWS_AS(
        r.add_route(http_method::GET, "/dup", make_chain()),
        std::logic_error
    );
}

TEST_CASE("router: conflicting param names throws", "[router]") {
    router r;
    r.add_route(http_method::GET, "/users/:id/profile", make_chain());
    REQUIRE_THROWS_AS(
        r.add_route(http_method::GET, "/users/:user_id/settings", make_chain()),
        std::logic_error
    );
}

TEST_CASE("router: duplicate catch-all throws", "[router]") {
    router r;
    r.add_route(http_method::GET, "/files/*path", make_chain());
    REQUIRE_THROWS_AS(
        r.add_route(http_method::GET, "/files/*other", make_chain()),
        std::logic_error
    );
}

// ============================================================================
// Additional edge cases
// ============================================================================

TEST_CASE("router: empty router returns 404 for everything", "[router]") {
    router r;
    r.freeze();

    REQUIRE(!r.lookup(http_method::GET, "/").has_value());
    REQUIRE(!r.lookup(http_method::GET, "/anything").has_value());
}

TEST_CASE("router: catch-all matches single segment", "[router]") {
    router r;
    r.add_route(http_method::GET, "/files/*path", make_chain());
    r.freeze();

    auto result = r.lookup(http_method::GET, "/files/readme.txt");
    REQUIRE(result.has_value());
    REQUIRE(result->params[0].value == "readme.txt");
}

TEST_CASE("router: deeply nested routes", "[router]") {
    router r;
    r.add_route(http_method::GET, "/a/b/c/d/e", make_chain());
    r.freeze();

    REQUIRE(r.lookup(http_method::GET, "/a/b/c/d/e").has_value());
    REQUIRE(!r.lookup(http_method::GET, "/a/b/c/d").has_value());
    REQUIRE(!r.lookup(http_method::GET, "/a/b/c/d/e/f").has_value());
}

TEST_CASE("router: param at root level", "[router]") {
    router r;
    r.add_route(http_method::GET, "/:slug", make_chain());
    r.freeze();

    auto result = r.lookup(http_method::GET, "/about");
    REQUIRE(result.has_value());
    REQUIRE(result->params[0].key == "slug");
    REQUIRE(result->params[0].value == "about");
}

TEST_CASE("router: static and param siblings", "[router]") {
    router r;
    r.add_route(http_method::GET, "/users/:id", make_chain());
    r.add_route(http_method::GET, "/users/count", make_chain());
    r.add_route(http_method::GET, "/users/search", make_chain());
    r.freeze();

    // Static routes should match exactly
    auto count = r.lookup(http_method::GET, "/users/count");
    REQUIRE(count.has_value());
    REQUIRE(count->param_count == 0);

    auto search = r.lookup(http_method::GET, "/users/search");
    REQUIRE(search.has_value());
    REQUIRE(search->param_count == 0);

    // Non-matching goes to param
    auto id = r.lookup(http_method::GET, "/users/123");
    REQUIRE(id.has_value());
    REQUIRE(id->param_count == 1);
    REQUIRE(id->params[0].value == "123");
}

TEST_CASE("router: same param name reused across different levels", "[router]") {
    router r;
    r.add_route(http_method::GET, "/a/:id", make_chain());
    r.add_route(http_method::GET, "/b/:id", make_chain());
    r.freeze();

    auto a = r.lookup(http_method::GET, "/a/1");
    REQUIRE(a.has_value());
    REQUIRE(a->params[0].value == "1");

    auto b = r.lookup(http_method::GET, "/b/2");
    REQUIRE(b.has_value());
    REQUIRE(b->params[0].value == "2");
}

TEST_CASE("router: method isolation — GET does not match POST", "[router]") {
    router r;
    r.add_route(http_method::GET, "/only-get", make_chain());
    r.freeze();

    REQUIRE(r.lookup(http_method::GET, "/only-get").has_value());
    REQUIRE(!r.lookup(http_method::POST, "/only-get").has_value());
    REQUIRE(!r.lookup(http_method::PUT, "/only-get").has_value());
    REQUIRE(!r.lookup(http_method::DELETE, "/only-get").has_value());
    REQUIRE(!r.lookup(http_method::PATCH, "/only-get").has_value());
}

// ============================================================================
// methods_for_path()
// ============================================================================

TEST_CASE("router: methods_for_path multi-method", "[router]") {
    router r;
    r.add_route(http_method::GET, "/foo", make_chain());
    r.add_route(http_method::POST, "/foo", make_chain());
    r.freeze();

    auto mask = r.methods_for_path("/foo");
    REQUIRE((mask & (1u << static_cast<int>(http_method::GET))) != 0);
    REQUIRE((mask & (1u << static_cast<int>(http_method::POST))) != 0);
    REQUIRE((mask & (1u << static_cast<int>(http_method::DELETE))) == 0);
}

TEST_CASE("router: methods_for_path nonexistent path", "[router]") {
    router r;
    r.add_route(http_method::GET, "/exists", make_chain());
    r.freeze();

    REQUIRE(r.methods_for_path("/nonexistent") == 0);
}

TEST_CASE("router: methods_for_path all registered methods", "[router]") {
    router r;
    r.add_route(http_method::GET, "/res", make_chain());
    r.add_route(http_method::PUT, "/res", make_chain());
    r.add_route(http_method::DELETE, "/res", make_chain());
    r.freeze();

    auto mask = r.methods_for_path("/res");
    REQUIRE((mask & (1u << static_cast<int>(http_method::GET))) != 0);
    REQUIRE((mask & (1u << static_cast<int>(http_method::PUT))) != 0);
    REQUIRE((mask & (1u << static_cast<int>(http_method::DELETE))) != 0);
    REQUIRE((mask & (1u << static_cast<int>(http_method::POST))) == 0);
    REQUIRE((mask & (1u << static_cast<int>(http_method::PATCH))) == 0);
}
