# SPDX-License-Identifier: Apache-2.0 OR MIT
cmake_minimum_required(VERSION 3.20)

project(aparajita
    VERSION 0.1.0
    DESCRIPTION "Cache-aligned SIMD MemTable for RocksDB-style LSM-trees"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Native builds are for local measurement only. Anything that goes in the paper
# must also be reported for the portable baseline, since -march=native is not
# what ships and can silently change which kernel the compiler emits.
option(APARAJITA_NATIVE "Build with -march=native (local measurement only)" OFF)

add_library(aparajita INTERFACE)
target_include_directories(aparajita INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_features(aparajita INTERFACE cxx_std_20)

add_library(aparajita_flags INTERFACE)
target_compile_options(aparajita_flags INTERFACE
    -O3 -g -fno-omit-frame-pointer
    -Wall -Wextra -Wpedantic)
if(APARAJITA_NATIVE)
    target_compile_options(aparajita_flags INTERFACE -march=native)
endif()

# The SIMD kernels carry per-function target attributes, so no global -mavx2 is
# needed and the dispatch stays a runtime CPUID decision.

enable_testing()

add_executable(test_search tests/test_search.cpp)
target_link_libraries(test_search PRIVATE aparajita aparajita_flags)
add_test(NAME search_kernels COMMAND test_search)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(counter_report bench/counter_report.cpp)
    target_include_directories(counter_report PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/bench)
    target_link_libraries(counter_report PRIVATE aparajita aparajita_flags)
else()
    message(STATUS "counter_report needs perf_event_open(2); skipped on ${CMAKE_SYSTEM_NAME}")
endif()

option(APARAJITA_BENCHMARKS "Fetch Google Benchmark and build timing benchmarks" ON)
if(APARAJITA_BENCHMARKS)
    include(FetchContent)
    set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_WERROR OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(benchmark
        GIT_REPOSITORY https://github.com/google/benchmark.git
        GIT_TAG v1.9.5
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(benchmark)

    add_executable(bench_search bench/bench_search.cpp)
    target_link_libraries(bench_search PRIVATE aparajita aparajita_flags benchmark::benchmark)
endif()
