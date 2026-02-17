# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.2.0] - 2026-02-17

### Added

- **epoll backend** as fallback when io_uring is unavailable (Docker, K8s with default seccomp)
- Edge-triggered epoll with EAGAIN drain loops for minimal latency
- Runtime backend auto-detection: probes io_uring availability, falls back to epoll
- `voie::backend` enum (`auto_detect`, `io_uring`, `epoll`)
- `app::set_backend()` to explicitly select a backend
- `app::backend_available()` to query backend availability at runtime
- liburing is now optional: voie compiles and runs with epoll-only when liburing is absent
- Integration and RFC tests run on both backends via Catch2 `GENERATE`

### Changed

- Refactored monolithic `io_loop` into abstract `event_loop` base class with two implementations (`uring_loop`, `epoll_loop`)
- liburing missing is now a warning instead of a fatal CMake error
- `VOIE_HAS_IO_URING` compile definition controls io_uring backend inclusion

### Fixed

- `shutdown(SHUT_WR)` before async close to prevent response data loss under high load
- io_uring SQE exhaustion: flush-and-retry with synchronous close fallback instead of silently dropping operations

## [0.1.0] - 2025-05-01

### Added

- io_uring event loop with multishot accept and SQPOLL support
- AVX2-accelerated HTTP parsing (vendored picohttpparser with Cloudflare patch)
- Radix trie router with zero-allocation path matching
- Arena allocator for per-request memory (zero malloc in the hot path)
- Prebuilt responses for static content (zero-copy send)
- Middleware chain with `c.next()` flow control
- Route groups with prefix and inherited middleware
- Path parameters (`:name`) and wildcards (`*name`)
- Query string parsing with URL decoding
- `ctx::text()`, `json()`, `html()`, `send()`, `no_content()`, `redirect()`
- Per-request key-value storage (`ctx::store()` / `ctx::load()`)
- Custom 404 handler and error handler
- `app::all()` for registering routes on all HTTP methods
- `app::listen(address, port)` for binding to a specific interface
- `app::max_body()` for request body size enforcement (413 Payload Too Large)
- `app::wait_ready()` for synchronous startup in tests
- HEAD auto-handling (falls back to GET handler, strips body)
- OPTIONS auto-handling with `Allow` header
- HTTP/1.1 keep-alive and pipelining
- Connection: close and HTTP/1.0 default-close semantics
- Host header validation (RFC 7230)
- Date header caching (per-second)
- CMake install targets and `find_package(voie)` support
- CI workflow (GitHub Actions)
- Integration, RFC compliance, and unit test suites
- MIT license
