cmake_minimum_required(VERSION 3.16)
project(usrl_core C)

set(CMAKE_C_STANDARD 11)

# Default to Release for performance, but allow Debug override
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_C_FLAGS_RELEASE "-O3 -march=native")

# Define DEBUG macro only for Debug builds (controls printfs in headers)
if(CMAKE_BUILD_TYPE MATCHES "Debug")
    add_compile_definitions(DEBUG)
endif()

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/core/includes
)

# -----------------------------------------------------------------------------
# Core Library
# -----------------------------------------------------------------------------
add_library(usrl_core
    core/src/usrl_core.c
    core/src/ring_swmr.c
    core/src/ring_mwmr.c
)
find_package(Threads REQUIRED)
target_link_libraries(usrl_core PUBLIC Threads::Threads rt atomic)

# -----------------------------------------------------------------------------
# Standard Modules (Demos)
# -----------------------------------------------------------------------------
# Main initializer (reads usrl_config.json)
add_executable(init_core modules/main_init_core.c)
target_link_libraries(init_core usrl_core)

# Demo Publisher
add_executable(pub modules/publisher.c)
target_link_libraries(pub usrl_core)

# Demo Subscriber
add_executable(sub modules/subscriber.c)
target_link_libraries(sub usrl_core)

# -----------------------------------------------------------------------------
# Benchmarks Suite
# -----------------------------------------------------------------------------
# Dedicated initializer for benchmarks (reads usrl_config_bench.json)
add_executable(init_bench benchmarks/init_bench.c)
target_link_libraries(init_bench usrl_core)

# Single-Writer Throughput Test
add_executable(bench_pub_swmr benchmarks/bench_pub_swmr.c)
target_link_libraries(bench_pub_swmr usrl_core)

# Multi-Writer Contention Test
add_executable(bench_pub_mwmr benchmarks/bench_pub_mwmr.c)
target_link_libraries(bench_pub_mwmr usrl_core)

# Latency/Rate Subscriber
add_executable(bench_sub benchmarks/bench_sub.c)
target_link_libraries(bench_sub usrl_core)
