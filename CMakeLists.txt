cmake_minimum_required(VERSION 3.25)
project(voie VERSION 0.1.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(VOIE_BUILD_EXAMPLES "Build example programs" ON)
option(VOIE_BUILD_TESTS "Build test suite" ON)
option(VOIE_BUILD_BENCHMARKS "Build benchmarks" OFF)

add_subdirectory(third_party)

add_library(voie
    src/app.cpp
    src/ctx.cpp
    src/router.cpp
    src/io_loop.cpp
    src/connection.cpp
    src/http_parser.cpp
    src/handler.cpp
)

target_include_directories(voie
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(voie
    PRIVATE
        picohttpparser
        uring
)

target_compile_options(voie PRIVATE
    -Wall -Wextra -Wpedantic
    -march=native
)

if(VOIE_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(VOIE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(VOIE_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()
