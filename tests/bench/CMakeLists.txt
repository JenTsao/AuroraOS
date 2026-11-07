# =============================================================================
# tests/bench/CMakeLists.txt — AuroraOS host microbenchmark build
#
#   cmake -S tests/bench -B build_perf -DCMAKE_BUILD_TYPE=Release
#   cmake --build build_perf -j
#   ./build_perf/aurora_perf_bench --csv perf-results.csv
#
# Deliberately a separate CMake project from tests/CMakeLists.txt:
#   - the benchmark needs no GoogleTest, so CI does not pay for the
#     FetchContent download;
#   - it is never registered with CTest, so `ctest` in the aurora_tests
#     build keeps exactly the same pass/fail set as before.
# Only the kernel translation units the measured hot paths live in are
# compiled. tests/stubs/ is listed BEFORE everything else so host-safe
# arch/syscall/task headers shadow the real bare-metal ones.
# =============================================================================

cmake_minimum_required(VERSION 3.20)
project(aurora_perf LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(AURORA_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set(HOST_PRELUDE_PATH "${AURORA_ROOT}/tests/stubs/host_prelude.hpp")

# Explicit source list (AGENTS.md §24: no file(GLOB) for AuroraOS sources).
add_executable(aurora_perf_bench
    bench_main.cpp
    "${AURORA_ROOT}/tests/stubs/kernel_stubs.cpp"
    "${AURORA_ROOT}/kernel/core/cspace.cpp"
    "${AURORA_ROOT}/kernel/core/ipc.cpp"
    "${AURORA_ROOT}/kernel/core/mutex.cpp"
    "${AURORA_ROOT}/kernel/core/process_timer.cpp"
    "${AURORA_ROOT}/kernel/task/wait_queue.cpp"
)

target_include_directories(aurora_perf_bench BEFORE PRIVATE
    "${AURORA_ROOT}/tests/stubs"
)

target_include_directories(aurora_perf_bench PRIVATE
    "${AURORA_ROOT}"
    "${AURORA_ROOT}/kernel"
    "${AURORA_ROOT}/kernel/core"
    "${AURORA_ROOT}/kernel/mm"
    "${AURORA_ROOT}/kernel/task"
    "${AURORA_ROOT}/kernel/scheduler"
    "${AURORA_ROOT}/kernel/interrupt"
    "${AURORA_ROOT}/syscall"
    "${AURORA_ROOT}/metrics"
)

target_compile_definitions(aurora_perf_bench PRIVATE
    AURORA_HOST_TEST=1
    CONFIG_NO_PTHREAD=1
)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(aurora_perf_bench PRIVATE
        -Wall -Wextra
        -Wno-unused-parameter
        $<$<COMPILE_LANGUAGE:CXX>:-fpermissive>
        -include "${HOST_PRELUDE_PATH}"
    )
endif()

add_custom_target(perf
    COMMAND $<TARGET_FILE:aurora_perf_bench> --csv "${CMAKE_CURRENT_BINARY_DIR}/perf-results.csv"
    DEPENDS aurora_perf_bench
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    COMMENT "Running AuroraOS host microbenchmarks"
)
