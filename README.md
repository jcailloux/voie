# voie

High-performance HTTP/1.1 framework for Linux, built on io_uring.

## Features

- **io_uring** event loop with multishot accept and SQPOLL support
- **AVX2-accelerated** HTTP parsing (vendored picohttpparser with Cloudflare patch)
- **Radix trie** router with zero-allocation path matching
- **Arena allocator** for per-request memory (zero malloc in the hot path)
- **Prebuilt responses** for static content (zero-copy send)
- **C++23**, no dependencies beyond liburing

## Quick start

```cpp
#include <voie/voie.h>

int main() {
    voie::app app;

    app.get("/", voie::prebuilt("hello, world", "text/plain"));

    app.get("/users/:id", [](voie::ctx& c) {
        c.json(std::format(R"({{"id": {}}})", c.param("id")));
    });

    app.listen(8080);
}
```

## Build

Requires Linux with liburing-dev (kernel 5.19+), CMake 3.25+, and a C++23 compiler.

```bash
# Fedora / RHEL
sudo dnf install liburing-devel

# Ubuntu / Debian
sudo apt install liburing-dev
```

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)

# Run the hello example
./build/examples/hello
```

### Build options

| Option | Default | Description |
|---|---|---|
| `VOIE_BUILD_EXAMPLES` | ON | Build example programs |
| `VOIE_BUILD_TESTS` | ON | Build test suite |
| `VOIE_BUILD_BENCHMARKS` | OFF | Build benchmarks |

## API

### Configuration

```cpp
voie::app app;

app.threads(6);          // Worker threads (0 = auto-detect)
app.max_body(1 << 20);   // Max request body size (default 1 MB)
app.backlog(512);         // Listen backlog
app.sqpoll(true);         // Enable io_uring SQPOLL (opt-in, see below)
```

SQPOLL is disabled by default. It dedicates one kernel thread per worker to poll the submission queue, eliminating `io_uring_submit()` syscalls. This is only beneficial on machines with many spare cores (e.g., 2x the worker thread count). On typical hardware, the CPU cost of the polling threads outweighs the saved syscalls. Requires `CAP_SYS_NICE` or elevated `RLIMIT_MEMLOCK`.

### Routes

```cpp
app.get("/path",    handler);
app.post("/path",   handler);
app.put("/path",    handler);
app.del("/path",    handler);   // DELETE
app.patch("/path",  handler);
app.all("/path",    handler);   // All methods
```

Path parameters use `:name` syntax. Wildcards use `*name`:

```cpp
app.get("/users/:id", [](voie::ctx& c) {
    auto id = c.param("id");
    // ...
});

app.get("/static/*filepath", [](voie::ctx& c) {
    auto path = c.param("filepath");
    // ...
});
```

Multiple handlers form a middleware chain (executed left to right):

```cpp
app.get("/admin", auth_check, rate_limit, admin_handler);
```

### Context (`voie::ctx`)

**Request access:**

```cpp
c.method()              // "GET", "POST", ...
c.path()                // "/users/42"
c.param("id")           // Path parameter
c.query("page")         // Query string parameter
c.header("Authorization")
c.has_header("X-Custom") // true if header exists
c.body()                // Request body

// Iterate over all request headers
for (std::size_t i = 0; i < c.request_header_count(); ++i) {
    auto [name, value] = c.request_header_at(i);
}
```

**Response building (chainable):**

```cpp
c.status(201).set("X-Custom", "value").json(R"({"ok": true})");
```

**Terminal response methods:**

```cpp
c.text("hello");                     // text/plain
c.json(R"({"ok": true})");          // application/json
c.html("<h1>hello</h1>");           // text/html
c.send(data, "image/png");          // custom content type
c.no_content();                      // 204 No Content
c.redirect("/login");               // 302 redirect
c.redirect("/new-url", 301);        // permanent redirect
```

**Per-request storage:**

```cpp
// In middleware
MySession* session = authenticate(c);
c.store("session", session);
c.next();

// In handler
auto* session = c.load<MySession>("session");
```

### Middleware

Global middleware applies to all routes:

```cpp
app.use([](voie::ctx& c) {
    // runs before every handler
    c.next();  // call next middleware / handler
});
```

Call `c.next()` to continue the chain. Omit it to short-circuit (e.g., return 401).

### Groups

```cpp
auto api = app.group("/api/v1");
api.use(auth_middleware);

api.get("/users", list_users);
api.get("/users/:id", get_user);
api.post("/users", create_user);
api.del("/users/:id", delete_user);
```

Groups inherit middleware from their parent. Subgroups can be nested:

```cpp
auto admin = api.subgroup("/admin");
admin.use(admin_only_middleware);
admin.get("/stats", stats_handler);
// matches /api/v1/admin/stats
```

### Prebuilt responses

For static content, pre-compute the full HTTP response at startup:

```cpp
app.get("/health", voie::prebuilt(R"({"status":"ok"})", "application/json"));
```

At request time, the pre-built bytes are sent directly with zero parsing and zero allocation.

### Error handling

```cpp
app.not_found([](voie::ctx& c) {
    c.status(404).json(R"({"error": "not found"})");
});

app.on_error([](voie::ctx& c, std::exception_ptr ep) {
    c.status(500).json(R"({"error": "internal server error"})");
});
```

### Lifecycle

```cpp
// Listen on all interfaces
app.listen(8080);

// Listen on a specific address
app.listen("127.0.0.1", 8080);

// For programmatic use (e.g., tests), run in a thread and wait for readiness
std::thread t([&]() { app.listen(8080); });
app.wait_ready();  // blocks until all worker threads are initialized

// Graceful shutdown (from another thread or signal handler)
app.shutdown();
t.join();
```

## Performance

### Architecture

Each worker thread runs its own io_uring instance and listen socket (`SO_REUSEPORT`). There is no cross-thread synchronization in the request path.

The hot path for a prebuilt response:

1. io_uring multishot accept (no syscall per connection)
2. `recv` into per-connection 4 KB buffer
3. AVX2 HTTP parse (picohttpparser)
4. Radix trie route lookup
5. Direct `send` of pre-built bytes (no `build_response`)

### OS tuning

For production or benchmarking, apply these settings:

```bash
# CPU governor: maximum frequency
sudo cpupower frequency-set -g performance

# Network stack
sudo sysctl -w net.core.somaxconn=65535
sudo sysctl -w net.core.netdev_max_backlog=65535
sudo sysctl -w net.ipv4.tcp_max_syn_backlog=65535
sudo sysctl -w net.ipv4.tcp_tw_reuse=1

# io_uring SQPOLL requires higher RLIMIT_MEMLOCK
ulimit -l unlimited
```

### Benchmarking with wrk

```bash
# Install wrk
git clone https://github.com/wg/wrk.git /tmp/wrk
cd /tmp/wrk && make -j$(nproc)

# Start the server
./build/examples/hello &

# Run benchmark (12 threads, 400 connections, 10 seconds)
/tmp/wrk/wrk -t12 -c400 -d10s http://127.0.0.1:8080/
```

For reproducible single-core latency measurements, pin server and client to separate physical cores:

```bash
# Server on core 0
taskset -c 0 ./build/examples/hello &

# Client on core 3 (different physical core)
taskset -c 3 /tmp/wrk/wrk -t1 -c10 -d10s http://127.0.0.1:8080/
```

## Tests

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
```

Test suites: router, arena, HTTP parser, context, handler, security, integration, RFC compliance.

## License

MIT
