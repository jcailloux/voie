# picohttpparser — SIMD HTTP parser with AVX2 support (vendored)
# Upstream: https://github.com/h2o/picohttpparser
# AVX2 patch: https://blog.cloudflare.com/improving-picohttpparser-further-with-avx2/
#
# Compile-time SIMD dispatch via -march=native:
#   __AVX2__   → bitmap-based 128-byte scanner (~1.8x faster than SSE4.2)
#   __SSE4_2__ → PCMPESTRI 16-byte scanner (upstream default)
#   neither    → scalar fallback
add_library(picohttpparser STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/picohttpparser/picohttpparser.c
)
target_include_directories(picohttpparser PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/picohttpparser
)
target_compile_options(picohttpparser PRIVATE -march=native)

# liburing — prefer system package
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(URING IMPORTED_TARGET liburing)
endif()

if(URING_FOUND)
    add_library(uring ALIAS PkgConfig::URING)
    message(STATUS "Using system liburing: ${URING_VERSION}")
else()
    message(FATAL_ERROR
        "liburing not found. Install it:\n"
        "  Fedora: sudo dnf install liburing-devel\n"
        "  Debian/Ubuntu: sudo apt install liburing-dev\n"
    )
endif()
