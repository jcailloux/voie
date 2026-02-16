include(FetchContent)

FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.7.1
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(Catch2)
list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)

function(voie_add_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE voie Catch2::Catch2WithMain picohttpparser)
    target_include_directories(${name} PRIVATE ${CMAKE_SOURCE_DIR}/src)
    catch_discover_tests(${name})
endfunction()

# Unit tests — no network, safe to split into one process per TEST_CASE
voie_add_test(test_arena)
voie_add_test(test_http_parser)
voie_add_test(test_router)
voie_add_test(test_ctx)
voie_add_test(test_handler)
voie_add_test(test_security)

# Integration / RFC tests bind TCP ports.  They MUST run as a single
# process so the in-process atomic port counter guarantees unique ports.
# Using catch_discover_tests() would spawn one process per TEST_CASE,
# each resetting the counter, causing port collisions via SO_REUSEPORT.
foreach(name test_integration test_rfc)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE voie Catch2::Catch2WithMain picohttpparser)
    target_include_directories(${name} PRIVATE ${CMAKE_SOURCE_DIR}/src)
    add_test(NAME ${name} COMMAND ${name})
endforeach()