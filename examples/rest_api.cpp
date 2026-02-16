#include <voie/voie.h>
#include <format>
#include <string>

static void auth_middleware(voie::ctx& c) {
    auto token = c.header("Authorization");
    if (token.empty()) {
        c.status(401).json(R"({"error": "unauthorized"})");
        return;
    }
    c.next();
}

static void list_users(voie::ctx& c) {
    c.json(R"([{"id": 1, "name": "Alice"}, {"id": 2, "name": "Bob"}])");
}

static void get_user(voie::ctx& c) {
    auto id = c.param("id");
    auto body = std::format(R"({{"id": {}, "name": "Alice"}})", id);
    c.json(body);
}

static void create_user(voie::ctx& c) {
    c.status(201).json(R"({"created": true})");
}

static void delete_user(voie::ctx& c) {
    c.status(204).text("");
}

int main() {
    voie::app app;

    app.get("/health", [](voie::ctx& c) {
        c.json(R"({"status": "ok"})");
    });

    auto api = app.group("/api/v1");
    api.use(auth_middleware);

    api.get("/users", list_users);
    api.get("/users/:id", get_user);
    api.post("/users", create_user);
    api.del("/users/:id", delete_user);

    app.not_found([](voie::ctx& c) {
        c.status(404).json(R"({"error": "not found"})");
    });

    app.listen(3000);
}
