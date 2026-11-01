cmake_minimum_required(VERSION 3.16)

project(HyperMalloc VERSION 0.1.0 LANGUAGES C)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "HyperMalloc currently supports Linux only")
endif()

include(CTest)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
option(HM_DEBUG "Check heap invariants after public operations" OFF)
option(HM_STATS "Track allocator operation counters" ON)
set(HM_ARENAS 8 CACHE STRING "Number of independent allocator arenas (1-8)")
if(NOT HM_ARENAS MATCHES "^[1-8]$")
    message(FATAL_ERROR "HM_ARENAS must be an integer from 1 to 8")
endif()
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

function(hm_enable_warnings target)
    target_compile_options(${target} PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
endfunction()

# HyperMalloc library.
add_library(hypermalloc STATIC
    src/core.c
)

add_library(hypermalloc_preload SHARED src/core.c src/preload.c)
set_target_properties(hypermalloc_preload PROPERTIES OUTPUT_NAME hypermalloc_preload)
set_target_properties(hypermalloc_preload PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR})
target_compile_definitions(hypermalloc_preload PRIVATE
    HM_DEBUG=$<BOOL:${HM_DEBUG}>
    HM_STATS=$<BOOL:${HM_STATS}>
    HM_ACTIVE_ARENAS=${HM_ARENAS}
)
target_link_libraries(hypermalloc_preload PRIVATE Threads::Threads)
hm_enable_warnings(hypermalloc_preload)
target_include_directories(hypermalloc_preload PRIVATE include)

target_compile_definitions(hypermalloc PRIVATE
    HM_DEBUG=$<BOOL:${HM_DEBUG}>
    HM_STATS=$<BOOL:${HM_STATS}>
    HM_ACTIVE_ARENAS=${HM_ARENAS}
)

# Apply shared compiler configuration.
hm_enable_warnings(hypermalloc)
target_link_libraries(hypermalloc PUBLIC
    Threads::Threads
)

# Public headers.
target_include_directories(hypermalloc PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

# Private headers.
target_include_directories(hypermalloc PRIVATE
    ${PROJECT_SOURCE_DIR}/src
)

install(TARGETS hypermalloc hypermalloc_preload
    EXPORT HyperMallocTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES include/hypermalloc.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT HyperMallocTargets NAMESPACE HyperMalloc::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/HyperMalloc)
configure_package_config_file(cmake/HyperMallocConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/HyperMallocConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/HyperMalloc)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/HyperMallocConfigVersion.cmake
    VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/HyperMallocConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/HyperMallocConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/HyperMalloc)

add_executable(bench_compare benchmarks/bench_compare.c)
target_link_libraries(bench_compare PRIVATE hypermalloc)
hm_enable_warnings(bench_compare)

add_executable(bench_threads benchmarks/bench_threads.c)
target_link_libraries(bench_threads PRIVATE hypermalloc Threads::Threads)
hm_enable_warnings(bench_threads)

add_executable(bench_direct_resize benchmarks/bench_direct_resize.c)
target_link_libraries(bench_direct_resize PRIVATE hypermalloc)
hm_enable_warnings(bench_direct_resize)

add_executable(bench_workloads benchmarks/bench_workloads.c)
target_link_libraries(bench_workloads PRIVATE hypermalloc)
hm_enable_warnings(bench_workloads)

add_executable(test_realloc_shrink tests/test_realloc_shrink.c)
target_link_libraries(test_realloc_shrink PRIVATE hypermalloc)
add_test(NAME realloc_shrink COMMAND test_realloc_shrink)

add_executable(test_direct_shrink tests/test_direct_shrink.c)
target_link_libraries(test_direct_shrink PRIVATE hypermalloc)
add_test(NAME direct_shrink COMMAND test_direct_shrink)

add_executable(test_core tests/test_core.c)
target_link_libraries(test_core PRIVATE hypermalloc)
add_test(NAME core COMMAND test_core)

add_executable(test_checker tests/test_checker.c)
target_link_libraries(test_checker PRIVATE hypermalloc)
add_test(NAME checker COMMAND test_checker)

add_executable(test_threads tests/test_threads.c)
target_link_libraries(test_threads PRIVATE hypermalloc Threads::Threads)
add_test(NAME threads COMMAND test_threads)

add_executable(test_fork tests/test_fork.c)
target_compile_definitions(test_fork PRIVATE HM_FORK_STATIC=1)
target_link_libraries(test_fork PRIVATE hypermalloc Threads::Threads)
add_test(NAME fork COMMAND test_fork)

add_executable(test_trim tests/test_trim.c)
target_link_libraries(test_trim PRIVATE hypermalloc)
add_test(NAME trim COMMAND test_trim)

add_executable(test_invalid tests/test_invalid.c)
target_compile_definitions(test_invalid PRIVATE HM_DEBUG=$<BOOL:${HM_DEBUG}>)
target_link_libraries(test_invalid PRIVATE hypermalloc)
add_test(NAME invalid COMMAND test_invalid)

add_executable(test_size_classes tests/test_size_classes.c)
target_include_directories(test_size_classes PRIVATE include)
target_compile_definitions(test_size_classes PRIVATE
    HM_DEBUG=$<BOOL:${HM_DEBUG}>
    HM_STATS=$<BOOL:${HM_STATS}>
    HM_ACTIVE_ARENAS=${HM_ARENAS}
)
target_link_libraries(test_size_classes PRIVATE Threads::Threads)
add_test(NAME size_classes COMMAND test_size_classes)

add_executable(fuzz_ops tests/fuzz_ops.c)
target_compile_definitions(fuzz_ops PRIVATE HM_FUZZ_STANDALONE=1)
target_link_libraries(fuzz_ops PRIVATE hypermalloc)
add_test(NAME fuzz_quick COMMAND fuzz_ops --seed 1 2 1000)
add_test(NAME fuzz_corpus COMMAND fuzz_ops --file
    ${PROJECT_SOURCE_DIR}/tests/corpus/basic.txt)

add_executable(test_preload tests/test_preload.c)
hm_enable_warnings(test_preload)
if(HM_SANITIZE STREQUAL "")
    add_test(NAME preload COMMAND ${CMAKE_COMMAND} -E env
        LD_PRELOAD=$<TARGET_FILE:hypermalloc_preload> $<TARGET_FILE:test_preload>)
    add_executable(test_preload_fork tests/test_fork.c)
    target_link_libraries(test_preload_fork PRIVATE Threads::Threads)
    add_test(NAME preload_fork COMMAND ${CMAKE_COMMAND} -E env
        LD_PRELOAD=$<TARGET_FILE:hypermalloc_preload>
        $<TARGET_FILE:test_preload_fork>)
endif()

option(HM_LIBFUZZER "Build a libFuzzer target when using Clang" OFF)
if(HM_LIBFUZZER)
    if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "HM_LIBFUZZER requires Clang")
    endif()
    add_executable(fuzz_ops_libfuzzer tests/fuzz_ops.c)
    target_link_libraries(fuzz_ops_libfuzzer PRIVATE hypermalloc)
    target_compile_options(fuzz_ops_libfuzzer PRIVATE -fsanitize=fuzzer)
    target_link_options(fuzz_ops_libfuzzer PRIVATE -fsanitize=fuzzer)
endif()
