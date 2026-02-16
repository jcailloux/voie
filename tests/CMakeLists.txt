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
# Integration tests run in a single process (need unique ports via atomic counter)
add_executable(test_integration test_integration.cpp)
target_link_libraries(test_integration PRIVATE voie Catch2::Catch2WithMain picohttpparser)
target_include_directories(test_integration PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME test_integration COMMAND test_integration)
