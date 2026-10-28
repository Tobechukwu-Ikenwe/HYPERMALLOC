cmake_minimum_required(VERSION 3.16)

project(HyperMalloc LANGUAGES C)

include(CTest)

find_package(Threads REQUIRED)

# Use the C11 language standard.
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Compiler warnings shared by every target.
add_library(project_warnings INTERFACE)

target_compile_options(project_warnings INTERFACE
    -Wall
    -Wextra
    -Wpedantic
)

# HyperMalloc library.
add_library(hypermalloc STATIC
    src/heap/heap.c
    src/block/block.c
    src/freelist/free_list.c
    src/hypermalloc/hypermalloc.c
    src/hypermalloc/free.c
    src/hypermalloc/realloc.c
)

# Apply shared compiler configuration.
target_link_libraries(hypermalloc PUBLIC
    project_warnings
    Threads::Threads
)

# Public headers.
target_include_directories(hypermalloc PUBLIC
    ${PROJECT_SOURCE_DIR}/include
)

# Private headers.
target_include_directories(hypermalloc PRIVATE
    ${PROJECT_SOURCE_DIR}/src
)

# Build the malloc benchmark.
add_executable(benchmark_malloc
    benchmarks/benchmark_malloc.c
)

target_link_libraries(benchmark_malloc PRIVATE
    hypermalloc
)

# Build the full allocator benchmark.
add_executable(full_benchmark
    benchmarks/full_benchmark.c
)

target_link_libraries(full_benchmark PRIVATE
    hypermalloc
)

add_executable(benchmark_mixed
    tests/benchmark_mixed.c
)

target_link_libraries(benchmark_mixed PRIVATE
    hypermalloc
)

add_executable(benchmark_random
    tests/benchmark_random.c
)

target_link_libraries(benchmark_random PRIVATE
    hypermalloc
)

add_executable(benchmark_reuse_mixed
    tests/benchmark_reuse_mixed.c
)

target_link_libraries(benchmark_reuse_mixed PRIVATE
    hypermalloc
)

add_executable(test_realloc_shrink tests/test_realloc_shrink.c)
target_link_libraries(test_realloc_shrink PRIVATE hypermalloc)
add_test(NAME realloc_shrink COMMAND test_realloc_shrink)

# Use Debug unless explicitly selected.
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()
