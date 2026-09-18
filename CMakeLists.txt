cmake_minimum_required(VERSION 3.16)

project(HyperMalloc LANGUAGES C)

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

# Use Debug unless explicitly selected.
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()
