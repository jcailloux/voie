include(FetchContent)

FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.7.1
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(Catch2)

add_executable(voie_bench bench_router.cpp)
target_link_libraries(voie_bench PRIVATE voie Catch2::Catch2WithMain picohttpparser)
target_include_directories(voie_bench PRIVATE ${CMAKE_SOURCE_DIR}/src)