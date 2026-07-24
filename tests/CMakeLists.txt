# =============================================================================
# tests/CMakeLists.txt — Host-native unit test build for auroraOS
#
# Usage (from project root):
#   cmake -S tests -B build_tests -DCMAKE_BUILD_TYPE=Debug
#   cmake --build build_tests -j
#   ctest --test-dir build_tests --output-on-failure
#
# This CMake file is INTENTIONALLY separate from the main CMakeLists.txt.
# The main build targets ARM Cortex-M4 bare-metal (-ffreestanding, ARM toolchain).
# Tests must compile for the host OS (x86/x64 Linux / macOS) so GoogleTest
# can execute them natively.
#
# Key mechanism: target_include_directories places tests/stubs/ BEFORE kernel/
# so stub headers (arch_api.hpp, mutex.hpp, syscall.hpp) shadow the real ones.
# =============================================================================

cmake_minimum_required(VERSION 3.20)
project(aurora_tests LANGUAGES CXX C)

# ---------------------------------------------------------------------------
# 1. Language standard — C++17 to match the kernel codebase
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)  # Enforce strict ISO C++, no GNU extensions

# ---------------------------------------------------------------------------
# 2. Fetch GoogleTest (v1.17.0 — pinned for reproducibility)
# ---------------------------------------------------------------------------
include(FetchContent)

set(GTEST_VERSION "v1.17.0")
FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/${GTEST_VERSION}.zip
    DOWNLOAD_EXTRACT_TIMESTAMP ON
)

# Do not install gtest/gmock system-wide when building tests
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)  # Required on Windows MSVC

FetchContent_MakeAvailable(googletest)

# ---------------------------------------------------------------------------
# 3. Project root (one level up from tests/)
# ---------------------------------------------------------------------------
set(AURORA_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")

# ---------------------------------------------------------------------------
# 4. Source files under test
#    Only include the .cpp files that are free of ARM assembly / linker symbols.
#    kernel/memory.cpp defines operator new/delete — we exclude it to avoid
#    overriding the host allocator; KernelHeap is tested via its header only.
# ---------------------------------------------------------------------------
file(GLOB LUA_SOURCES "${AURORA_ROOT}/3rdparty/lua/*.c")
list(REMOVE_ITEM LUA_SOURCES 
    "${AURORA_ROOT}/3rdparty/lua/lua.c" 
    "${AURORA_ROOT}/3rdparty/lua/luac.c"
    "${AURORA_ROOT}/3rdparty/lua/loslib.c"
    "${AURORA_ROOT}/3rdparty/lua/liolib.c"
    "${AURORA_ROOT}/3rdparty/lua/lmathlib.c"
    "${AURORA_ROOT}/3rdparty/lua/loadlib.c"
    "${AURORA_ROOT}/3rdparty/lua/lstrlib.c"
    "${AURORA_ROOT}/3rdparty/lua/linit.c"
)

set(AURORA_SOURCES_UNDER_TEST
    "${AURORA_ROOT}/vfs/vfs.cpp"
    "${AURORA_ROOT}/apps/lua_ui_binding.cpp"
    "${AURORA_ROOT}/3rdparty/ed25519/ed25519.c"
    "${AURORA_ROOT}/kernel/ota.cpp"
    "${AURORA_ROOT}/kernel/cspace.cpp"
    "${AURORA_ROOT}/kernel/ipc.cpp"
    ${LUA_SOURCES}
)

# Stub .cpp files that provide linker-level definitions for symbols declared
# in kernel headers but normally implemented in firmware .cpp files.
set(STUB_SOURCES
    stubs/kernel_stubs.cpp
    "${AURORA_ROOT}/kernel/watchdog_stub.cpp"
)

# ---------------------------------------------------------------------------
# 5. Test source files
# ---------------------------------------------------------------------------
set(TEST_SOURCES
    unit/test_memory.cpp
    unit/test_scheduler.cpp
    unit/test_vfs.cpp
    unit/test_renderer2d.cpp
    unit/test_power.cpp
    unit/test_lua_vm.cpp
    unit/test_mutex.cpp
    unit/test_mutex_pip.cpp
    unit/test_memory_pool.cpp
    unit/test_ui_engine.cpp
    unit/test_softbus_security.cpp
    unit/test_firmware_header.cpp
    unit/test_lua_ui.cpp
    unit/test_elf_loader.cpp
    unit/test_ota_unpack.cpp
    unit/test_health_algo.cpp
    unit/test_notification_center.cpp
    unit/test_nfc_service.cpp
    unit/test_screen_navigator.cpp
    unit/test_ble_signature.cpp
    unit/test_cspace.cpp
    unit/test_ipc.cpp
    integration/test_system_boot.cpp
    stress/test_oom_stress.cpp
)

# ---------------------------------------------------------------------------
# 6. Build the test executable
# ---------------------------------------------------------------------------
add_executable(aurora_tests
    ${TEST_SOURCES}
    ${AURORA_SOURCES_UNDER_TEST}
    ${STUB_SOURCES}
)

# Stub headers MUST come first so they shadow the real kernel headers.
# The order is: stubs/ → kernel/ → vfs/ → syscall/
target_include_directories(aurora_tests
    BEFORE PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/stubs"   # Overrides arch_api.hpp, mutex.hpp, syscall.hpp
    PRIVATE
        "${AURORA_ROOT}/kernel"               # memory.hpp, task.hpp, device.hpp, ...
        "${AURORA_ROOT}/vfs"                  # vfs.hpp
        "${AURORA_ROOT}/syscall"              # syscall.hpp (shadowed by stubs)
        "${AURORA_ROOT}/drivers/display"      # framebuffer.hpp, renderer2d.hpp
        "${AURORA_ROOT}/drivers/power"        # charging_manager.hpp
)

target_link_libraries(aurora_tests
    GTest::gtest
    GTest::gmock
    GTest::gtest_main
)

# Suppress ARM-specific defines / flags that leak from the source under test.
target_compile_definitions(aurora_tests PRIVATE
    # Prevent autoconf.h inclusion (normally injected via -include in the
    # firmware build; tests run without it).
    AURORA_HOST_TEST=1
    # Allow mock Ed25519 + placeholder OTA root public key in test builds.
    # Without this, kernel/ota.cpp and ed25519/ed25519.c will fail to compile.
    CONFIG_OTA_DEV_MODE=1
    # Suppress LittleFS debug output if any LFS headers are transitively included.
    LFS_NO_DEBUG LFS_NO_WARN LFS_NO_ERROR LFS_NO_ASSERT
)

# Warn aggressively to catch portability issues early.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(aurora_tests PRIVATE
        -Wall -Wextra
        -Wno-unused-parameter  # Many stub implementations intentionally ignore args
        # task.hpp uses reinterpret_cast<uint32_t>(ptr) which is fine on 32-bit
        # ARM targets but triggers -Wno-error on 64-bit hosts.  Accept this
        # in test builds; the cast only affects stack_base (unused in tests).
        -fpermissive
    )
endif()

# Force-include host_prelude.hpp BEFORE every translation unit to handle
# POSIX signal macro conflicts.  host_prelude.hpp:
#   1. Includes <signal.h> first, satisfying its include guard.
#   2. Immediately #undef's SIGINT, SIGKILL, SIGALRM, SIGUSR1.
# Because <signal.h>'s include guard is now set, any subsequent inclusion
# (via gtest.h → pthread.h → signal.h) is a no-op, and the undefs persist.
# This cleanly resolves the "constexpr int SIGINT = 2" conflict in task.hpp.
set(HOST_PRELUDE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/stubs/host_prelude.hpp")
target_compile_options(aurora_tests PRIVATE
    -include "${HOST_PRELUDE_PATH}"
)

# ---------------------------------------------------------------------------
# 7. Optional: AddressSanitizer + UndefinedBehaviorSanitizer
#    Enable with:  cmake -DENABLE_ASAN=ON -DENABLE_UBSAN=ON ...
# ---------------------------------------------------------------------------
option(ENABLE_ASAN  "Enable AddressSanitizer"           OFF)
option(ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)

if(ENABLE_ASAN AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(aurora_tests PRIVATE -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(aurora_tests PRIVATE -fsanitize=address)
endif()

if(ENABLE_UBSAN AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(aurora_tests PRIVATE -fsanitize=undefined -fno-omit-frame-pointer)
    target_link_options(aurora_tests PRIVATE -fsanitize=undefined)
endif()

# ---------------------------------------------------------------------------
# 8. Optional: Code coverage (GCC / Clang)
#    Enable with:  cmake -DENABLE_COVERAGE=ON ...
# ---------------------------------------------------------------------------
option(ENABLE_COVERAGE "Enable code coverage instrumentation" OFF)

if(ENABLE_COVERAGE)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
        target_compile_options(aurora_tests PRIVATE --coverage)
        target_link_options(aurora_tests PRIVATE --coverage)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(aurora_tests PRIVATE -fprofile-instr-generate -fcoverage-mapping)
        target_link_options(aurora_tests PRIVATE -fprofile-instr-generate)
    endif()
endif()

# ---------------------------------------------------------------------------
# 9. CTest integration
# ---------------------------------------------------------------------------
enable_testing()
include(GoogleTest)

# gtest_discover_tests() runs aurora_tests --gtest_list_tests at cmake time
# to register individual tests with CTest for fine-grained reporting.
gtest_discover_tests(aurora_tests
    PROPERTIES TIMEOUT 30   # Per-test timeout: 30 seconds
)
