cmake_minimum_required(VERSION 3.25)
project(voie VERSION 0.2.0 LANGUAGES CXX C)

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
    src/event_loop.cpp
    src/epoll_loop.cpp
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
)

# io_uring backend (optional — adds uring_loop.cpp when liburing is available)
if(URING_FOUND)
    target_sources(voie PRIVATE src/uring_loop.cpp)
    target_link_libraries(voie PRIVATE uring)
    target_compile_definitions(voie PRIVATE VOIE_HAS_IO_URING)
endif()

target_compile_options(voie PRIVATE
    -Wall -Wextra -Wpedantic
)

# -- Install targets --
include(GNUInstallDirs)

install(TARGETS voie picohttpparser
    EXPORT voieTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
install(DIRECTORY include/voie DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT voieTargets
    FILE voieTargets.cmake
    NAMESPACE voie::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/voie
)

include(CMakePackageConfigHelpers)
configure_package_config_file(
    cmake/voieConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/voieConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/voie
)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/voieConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/voieConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/voieConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/voie
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
