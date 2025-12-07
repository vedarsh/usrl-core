cmake_minimum_required(VERSION 3.16)
project(usrl_core C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_FLAGS_RELEASE "-O3 -march=native -Wall -Wextra")

# Export includes so sub-projects find headers automatically
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/core/includes)

# --- Library Definition ---
add_library(usrl_core STATIC
    core/src/usrl_core.c
    core/src/ring_swmr.c
    core/src/ring_mwmr.c
)

# Link required system libs
find_package(Threads REQUIRED)

# Always link pthreads
target_link_libraries(usrl_core PUBLIC Threads::Threads)

# Link rt and atomic only on non-Apple (Linux, etc.)
if(NOT APPLE)
    target_link_libraries(usrl_core PUBLIC rt atomic)
endif()

# Expose library and headers to subdirectories
target_include_directories(usrl_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# --- Add Subdirectories ---
add_subdirectory(benchmarks)
add_subdirectory(tools)
