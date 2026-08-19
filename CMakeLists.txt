cmake_minimum_required(VERSION 3.16)


# Use the C11 language standard.
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)


project(HyperMalloc LANGUAGES C)


#library that every target will use for the compiler configuration
add_library(project_warnings INTERFACE)

# Apply strict warnings to every target that uses this configuration.
target_compile_options(project_warnings INTERFACE
    -Wall
    -Wextra
    -Wpedantic
)



# to be used as a library for the benchmarks and tests
add_library(hypermalloc STATIC
    src/hyper_malloc.c
)

#apply configs to the hypermalloc library
target_link_libraries(hypermalloc PUBLIC
    project_warnings
    )




#for the header files
# Make the public HyperMalloc headers available to this library
# and to targets that link against it.
target_include_directories(hypermalloc PUBLIC
    ${PROJECT_SOURCE_DIR}/include
)

# Build the benchmark program.
add_executable(full_benchmark
    benchmarks/full_benchmark.c
)

# Link the benchmark against the HyerMalloc allocator.
target_link_libraries(full_benchmark PRIVATE
    hypermalloc
)

# Use Debug unless a build type is explicitly selected.
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

