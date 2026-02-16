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

voie_add_test(test_arena)
voie_add_test(test_http_parser)
voie_add_test(test_router)
voie_add_test(test_ctx)
voie_add_test(test_handler)
voie_add_test(test_security)
# Integration tests (each TEST_CASE uses its own port via atomic counter)
voie_add_test(test_integration)
voie_add_test(test_rfc)
