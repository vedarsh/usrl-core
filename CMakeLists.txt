cmake_minimum_required(VERSION 3.16)
project(usrl_core C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ============================================================
# Build Type Handling
# ============================================================
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

message(STATUS "USRL Build Type = ${CMAKE_BUILD_TYPE}")

# ============================================================
# Global Compiler Optimizations (Release Mode)
# ============================================================
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    add_compile_options(
        -O3 -march=native -mtune=native
        -flto
        -fstrict-aliasing
        -ffast-math
        -fno-plt
        -fdata-sections -ffunction-sections
        -fno-omit-frame-pointer
        -DNDEBUG
    )

    add_link_options(
        -Wl,--gc-sections
        -flto
    )
endif()

# ============================================================
# Debug Mode Enhancements
# ============================================================
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_definitions(DEBUG)
    add_compile_options(
        -O0 -g3
        -Wall -Wextra -Wpedantic
        -fsanitize=address,undefined
    )
    add_link_options(
        -fsanitize=address,undefined
    )
endif()

# ============================================================
# Include Paths
# ============================================================
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/core/includes
)

# ============================================================
# Core Library
# ============================================================
add_library(usrl_core
    core/src/usrl_core.c
    core/src/ring_swmr.c
    core/src/ring_mwmr.c
)

find_package(Threads REQUIRED)
target_link_libraries(usrl_core PUBLIC Threads::Threads rt atomic)

# For release builds: reduce binary size & improve I-cache performance
target_link_options(usrl_core PRIVATE -Wl,--gc-sections)

# ============================================================
# Executables: Init + Demo Modules
# ============================================================

add_executable(init_core modules/main_init_core.c)
target_link_libraries(init_core usrl_core)

add_executable(pub modules/publisher.c)
target_link_libraries(pub usrl_core)

add_executable(sub modules/subscriber.c)
target_link_libraries(sub usrl_core)

# Each executable should also benefit from gc-sections
target_link_options(init_core PRIVATE -Wl,--gc-sections)
target_link_options(pub       PRIVATE -Wl,--gc-sections)
target_link_options(sub       PRIVATE -Wl,--gc-sections)

# ============================================================
# Benchmark Suite
# ============================================================

add_executable(init_bench benchmarks/init_bench.c)
target_link_libraries(init_bench usrl_core)

add_executable(bench_pub_swmr benchmarks/bench_pub_swmr.c)
target_link_libraries(bench_pub_swmr usrl_core)

add_executable(bench_pub_mwmr benchmarks/bench_pub_mwmr.c)
target_link_libraries(bench_pub_mwmr usrl_core)

add_executable(bench_sub benchmarks/bench_sub.c)
target_link_libraries(bench_sub usrl_core)

# Optimize benchmark binaries heavily as well
target_link_options(init_bench       PRIVATE -Wl,--gc-sections)
target_link_options(bench_pub_swmr   PRIVATE -Wl,--gc-sections)
target_link_options(bench_pub_mwmr   PRIVATE -Wl,--gc-sections)
target_link_options(bench_sub        PRIVATE -Wl,--gc-sections)
