cmake_minimum_required(VERSION 3.16)

project(HyperMalloc LANGUAGES C)

include(CTest)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
option(HM_DEBUG "Check heap invariants after public operations" OFF)
option(HM_THREADSAFE "Protect allocator state with a mutex" ON)
option(HM_STATS "Track allocator operation counters" ON)
set(HM_SANITIZE "" CACHE STRING "Sanitizer: address, undefined, or thread")
set_property(CACHE HM_SANITIZE PROPERTY STRINGS "" address undefined thread)
if(HM_SANITIZE STREQUAL "address")
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
elseif(HM_SANITIZE STREQUAL "undefined")
    add_compile_options(-fsanitize=undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=undefined)
elseif(HM_SANITIZE STREQUAL "thread")
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
    add_link_options(-fsanitize=thread)
elseif(NOT HM_SANITIZE STREQUAL "")
    message(FATAL_ERROR "HM_SANITIZE must be address, undefined, or thread")
endif()

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
    -Wconversion
    -Wshadow
)

# HyperMalloc library.
add_library(hypermalloc STATIC
    src/core.c
)

target_compile_definitions(hypermalloc PRIVATE
    HM_DEBUG=$<BOOL:${HM_DEBUG}>
    HM_THREADSAFE=$<BOOL:${HM_THREADSAFE}>
    HM_STATS=$<BOOL:${HM_STATS}>
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

add_executable(test_core tests/test_core.c)
target_link_libraries(test_core PRIVATE hypermalloc)
add_test(NAME core COMMAND test_core)

add_executable(test_checker tests/test_checker.c)
target_link_libraries(test_checker PRIVATE hypermalloc)
add_test(NAME checker COMMAND test_checker)
