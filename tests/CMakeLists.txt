# =============================================================================
# tests/CMakeLists.txt — Host-native unit test build for auroraOS
#
#   cmake -S tests -B build_tests -DCMAKE_BUILD_TYPE=Debug
#   cmake --build build_tests -j
#   ctest --test-dir build_tests --output-on-failure
#
# Intentionally separate from the root CMakeLists.txt: the firmware build uses
# a freestanding cross toolchain, while GoogleTest must run natively on the
# host. tests/stubs/ is listed first in the include path so stub headers
# (arch_api.hpp, syscall.hpp, task.hpp) shadow the real kernel ones.
# =============================================================================

cmake_minimum_required(VERSION 3.20)
project(aurora_tests LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

set(GTEST_VERSION "v1.17.0")
FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/${GTEST_VERSION}.zip
    DOWNLOAD_EXTRACT_TIMESTAMP ON
)

set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(googletest)

set(AURORA_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")

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
    "${AURORA_ROOT}/services/vfs/vfs_service.cpp"
    "${AURORA_ROOT}/services/vfs/vfs_client.cpp"
    "${AURORA_ROOT}/services/sensor/sensor_service.cpp"
    "${AURORA_ROOT}/services/sensor/sensor_client.cpp"
    "${AURORA_ROOT}/services/power/power_service.cpp"
    "${AURORA_ROOT}/services/power/power_client.cpp"
    "${AURORA_ROOT}/services/power/power_manager.cpp"
    "${AURORA_ROOT}/net/net_client.cpp"
    "${AURORA_ROOT}/services/net/net_service.cpp"
    "${AURORA_ROOT}/services/display/display_service.cpp"
    "${AURORA_ROOT}/services/display/display_client.cpp"
    "${AURORA_ROOT}/services/service_manager.cpp"
    "${AURORA_ROOT}/apps/lua_ui_binding.cpp"
    "${AURORA_ROOT}/apps/elf_loader.cpp"
    "${AURORA_ROOT}/3rdparty/ed25519/ed25519.c"
    "${AURORA_ROOT}/kernel/task/wait_queue.cpp"
    "${AURORA_ROOT}/kernel/core/semaphore.cpp"
    "${AURORA_ROOT}/kernel/core/mutex.cpp"
    "${AURORA_ROOT}/kernel/core/process_timer.cpp"
    "${AURORA_ROOT}/kernel/core/posix.cpp"
    "${AURORA_ROOT}/kernel/core/symbol_export.cpp"
    "${AURORA_ROOT}/kernel/core/ota.cpp"
    "${AURORA_ROOT}/kernel/core/cspace.cpp"
    "${AURORA_ROOT}/kernel/core/device.cpp"
    "${AURORA_ROOT}/kernel/core/syscall_dispatcher.cpp"
    "${AURORA_ROOT}/kernel/core/syscall_validator.cpp"
    "${AURORA_ROOT}/kernel/core/ipc.cpp"
    "${AURORA_ROOT}/kernel/mm/mmu.cpp"
    "${AURORA_ROOT}/arch/arm/cortex-a/mmu/mmu_manager.cpp"
    "${AURORA_ROOT}/experimental/net/ble/ble_stack.cpp"
    "${AURORA_ROOT}/net/ble/hal_ble_impl.cpp"
    "${AURORA_ROOT}/net/ble/ble_signature.cpp"
    "${AURORA_ROOT}/net/ble/hci/hci_uart_transport.cpp"
    "${AURORA_ROOT}/net/ble/nimble_bridge.cpp"
    "${AURORA_ROOT}/experimental/apps/notification_center.cpp"
    "${AURORA_ROOT}/experimental/drivers/nfc/nfc_controller.cpp"
    "${AURORA_ROOT}/experimental/apps/nfc/nfc_service.cpp"
    "${AURORA_ROOT}/vfs/ramfs.cpp"
    "${AURORA_ROOT}/drivers/gpu/soft_gpu.cpp"
    "${AURORA_ROOT}/net/firewall/firewall_engine.cpp"
    "${AURORA_ROOT}/net/firewall/rule_table.cpp"
    "${AURORA_ROOT}/net/firewall/rule_parser.cpp"
    "${AURORA_ROOT}/net/firewall/stateful_inspector.cpp"
    "${AURORA_ROOT}/net/firewall/traffic_shaper.cpp"
    "${AURORA_ROOT}/services/firewall/firewall_audit.cpp"
    "${AURORA_ROOT}/services/firewall/firewall_client.cpp"
    "${AURORA_ROOT}/services/firewall/firewall_service.cpp"
    "${AURORA_ROOT}/runtime/app_base.cpp"
    "${AURORA_ROOT}/runtime/aurora_runtime.cpp"
    "${AURORA_ROOT}/runtime/app_sandbox.cpp"
    ${LUA_SOURCES}
    "${AURORA_ROOT}/experimental/drivers/gpu/soft_gpu_device.cpp"
    "${AURORA_ROOT}/experimental/drivers/camera/dummy_camera.cpp"
    "${AURORA_ROOT}/experimental/guix/window.cpp"
    "${AURORA_ROOT}/experimental/guix/compositor.cpp"
    "${AURORA_ROOT}/experimental/guix/widget.cpp"
    "${AURORA_ROOT}/experimental/guix/widget_button.cpp"
    "${AURORA_ROOT}/experimental/guix/widget_label.cpp"
    "${AURORA_ROOT}/experimental/guix/widget_progress.cpp"
    "${AURORA_ROOT}/experimental/guix/widget_slider.cpp"
    "${AURORA_ROOT}/experimental/guix/widget_panel.cpp"
    "${AURORA_ROOT}/experimental/net/wifi_driver.cpp"
)

set(STUB_SOURCES
    stubs/kernel_stubs.cpp
    stubs/usb_host_stub.cpp
)

set(TEST_SOURCES
    unit/test_memory.cpp
    unit/test_mpu.cpp
    unit/test_scheduler.cpp
    unit/test_vfs.cpp
    unit/test_ramfs.cpp
    unit/test_renderer2d.cpp
    unit/test_soft_gpu.cpp
    unit/test_power.cpp
    unit/test_power_management.cpp
    unit/test_lua_vm.cpp
    unit/test_libc_math.cpp
    unit/test_mutex.cpp
    unit/test_mutex_pip.cpp
    unit/test_memory_pool.cpp
    unit/test_ui_engine.cpp
    unit/test_widgets.cpp
    unit/test_ui_band_render.cpp
    unit/test_ui_view_ownership.cpp
    unit/test_gt316_driver.cpp
    unit/test_gt316_touch_race.cpp
    unit/test_i2c_bus.cpp
    unit/test_softbus_security.cpp
    unit/test_firmware_header.cpp
    unit/test_lua_ui.cpp
    unit/test_elf_loader.cpp
    unit/test_ota_unpack.cpp
    unit/test_health_algo.cpp
    unit/test_screen_navigator.cpp
    unit/test_ble_signature.cpp
    unit/test_hci_uart_transport.cpp
    unit/test_ble_stack_integration.cpp
    unit/test_cspace.cpp
    unit/test_device_registry.cpp
    unit/test_ipc.cpp
    unit/test_ipc_typed.cpp
    unit/test_syscall_ipc.cpp
    unit/test_syscall_validator.cpp
    unit/test_procfs.cpp
    unit/test_page_allocator.cpp
    unit/test_mmu_pte.cpp
    unit/test_mmu_manager.cpp
    unit/test_mmu_subsystem.cpp
    unit/test_process_timer.cpp
    unit/test_july_kernel_core.cpp
    unit/test_app_manifest_sandbox.cpp
    unit/test_microservices_client.cpp
    unit/test_system_services.cpp
    unit/test_dynamic_watch_face.cpp
    unit/debug_test.cpp
    unit/test_firewall.cpp
    unit/test_net_security_fixes.cpp
    unit/test_sensor_framework.cpp
    unit/test_bhy2_driver.cpp
    unit/test_ds3231_driver.cpp
    unit/test_wireless_ids.cpp
    unit/test_rf_spectrum.cpp
    unit/test_spectrum_monitor.cpp
    unit/test_ids.cpp
    unit/test_hids.cpp
    unit/test_response.cpp
    unit/test_february.cpp
    # February AI runtime integration tests (previously orphaned standalone
    # programs with their own main()/hand-written asserts — now GoogleTest).
    unit/test_february_core.cpp
    unit/test_february_phase2.cpp
    unit/test_february_phase22.cpp
    unit/test_february_reliability.cpp
    unit/test_february_softbus.cpp
    unit/test_memory_tiers.cpp
    unit/test_sensor_aggregator.cpp
    unit/test_camera_driver.cpp
    unit/test_usb_host.cpp
    unit/test_storage.cpp
    unit/test_stealth_identity.cpp
    unit/test_scanner.cpp
    integration/test_system_boot.cpp
    stress/test_heap_stress.cpp
    "${AURORA_ROOT}/experimental/tests/unit/test_notification_center.cpp"
    "${AURORA_ROOT}/experimental/tests/unit/test_nfc_service.cpp"
    "${AURORA_ROOT}/experimental/tests/unit/test_camera_usb_arch.cpp"
    "${AURORA_ROOT}/experimental/tests/unit/test_gpu_pipeline.cpp"
    "${AURORA_ROOT}/experimental/tests/unit/test_guix_compositor.cpp"
    "${AURORA_ROOT}/experimental/tests/unit/test_guix_widgets.cpp"
    "${AURORA_ROOT}/experimental/tests/unit/test_wifi_driver.cpp"
)

add_executable(aurora_tests
    ${TEST_SOURCES}
    ${AURORA_SOURCES_UNDER_TEST}
    ${STUB_SOURCES}
)

target_include_directories(aurora_tests
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/stubs"
        "${AURORA_ROOT}"
        "${AURORA_ROOT}/kernel"
        "${AURORA_ROOT}/kernel/core"
        "${AURORA_ROOT}/kernel/mm"
        "${AURORA_ROOT}/kernel/task"
        "${AURORA_ROOT}/kernel/scheduler"
        "${AURORA_ROOT}/kernel/interrupt"
        "${AURORA_ROOT}/vfs"
        "${AURORA_ROOT}/syscall"
        "${AURORA_ROOT}/drivers/display"
        "${AURORA_ROOT}/drivers/power"
        "${AURORA_ROOT}/net"
        "${AURORA_ROOT}/3rdparty/lwip/src/include"
        "${AURORA_ROOT}/adapter/net"
)

target_link_libraries(aurora_tests
    GTest::gtest
    GTest::gmock
    GTest::gtest_main
)

if(WIN32)
    target_link_libraries(aurora_tests ws2_32)
endif()

target_compile_definitions(aurora_tests PRIVATE
    AURORA_HOST_TEST=1
    CONFIG_OTA_DEV_MODE=1
    LFS_NO_DEBUG LFS_NO_WARN LFS_NO_ERROR LFS_NO_ASSERT
)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(aurora_tests PRIVATE
        -Wall -Wextra
        -Wno-unused-parameter
        $<$<COMPILE_LANGUAGE:CXX>:-fpermissive>
    )
    target_compile_options(aurora_tests PRIVATE
        $<$<COMPILE_LANGUAGE:C>:
            -Wno-error=implicit-function-declaration
            -Wno-error=int-conversion
            -Wno-error=incompatible-pointer-types
        >
    )
endif()

set(HOST_PRELUDE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/stubs/host_prelude.hpp")
target_compile_options(aurora_tests PRIVATE
    -include "${HOST_PRELUDE_PATH}"
)

option(ENABLE_ASAN  "Enable AddressSanitizer"           OFF)
option(ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(ENABLE_TSAN  "Enable ThreadSanitizer"            OFF)

if(ENABLE_ASAN AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(aurora_tests PRIVATE -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(aurora_tests PRIVATE -fsanitize=address)
endif()

if(ENABLE_UBSAN AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(aurora_tests PRIVATE -fsanitize=undefined -fno-omit-frame-pointer)
    target_link_options(aurora_tests PRIVATE -fsanitize=undefined)
endif()

if(ENABLE_TSAN AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(aurora_tests PRIVATE -fsanitize=thread -fno-omit-frame-pointer)
    target_link_options(aurora_tests PRIVATE -fsanitize=thread)
endif()

option(ENABLE_COVERAGE "Enable code coverage instrumentation" OFF)

if(ENABLE_COVERAGE)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
        target_compile_options(aurora_tests PRIVATE --coverage)
        target_link_options(aurora_tests PRIVATE --coverage)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(aurora_tests PRIVATE -fprofile-instr-generate -fcoverage-mapping)
        target_link_options(aurora_tests PRIVATE -fprofile-instr-generate)
    endif()

    find_program(GCOVR_PATH gcovr)
    if(GCOVR_PATH)
        add_custom_target(coverage
            COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
            COMMAND ${GCOVR_PATH} --root ${AURORA_ROOT} --filter "${AURORA_ROOT}/kernel/.*" --filter "${AURORA_ROOT}/vfs/.*" --filter "${AURORA_ROOT}/ai/.*" --html-details coverage.html
            WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
            COMMENT "Running tests and generating HTML coverage report..."
        )
    endif()
endif()

enable_testing()
include(GoogleTest)

gtest_discover_tests(aurora_tests
    PROPERTIES TIMEOUT 30
)

add_custom_target(check
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "Running all AuroraOS tests..."
)

add_custom_target(check_unit
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
            -E "^(StressKernelHeap|SystemBootIntegration)\\."
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "Running unit test suite (excludes stress + integration)..."
)

add_custom_target(check_stress
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
            -R "^StressKernelHeap\\."
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "Running heap stress suite..."
)

add_custom_target(check_integration
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
            -R "^SystemBootIntegration\\."
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "Running system boot integration suite..."
)

add_custom_target(check_february
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure -R "February"
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "Running February AI test suite..."
)

option(ENABLE_QEMU_HIL "Register QEMU HIL test in CTest" OFF)

if(ENABLE_QEMU_HIL)
    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_FOUND)
        add_test(NAME QemuHilTest
            COMMAND ${Python3_EXECUTABLE} ${AURORA_ROOT}/scripts/hil_runner.py
            WORKING_DIRECTORY ${AURORA_ROOT}/build
        )
        set_tests_properties(QemuHilTest PROPERTIES TIMEOUT 60)
    endif()
endif()

option(ENABLE_FUZZING "Enable libFuzzer" OFF)

if(ENABLE_FUZZING AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    add_executable(aurora_fuzzer
        fuzzing/fuzzer_main.cpp
        fuzzing/network_fuzzer.cpp
        fuzzing/ipc_fuzzer.cpp
        fuzzing/elf_fuzzer.cpp
        ${AURORA_SOURCES_UNDER_TEST}
        ${STUB_SOURCES}
    )
    target_include_directories(aurora_fuzzer
        PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/stubs"
            "${AURORA_ROOT}"
            "${AURORA_ROOT}/kernel"
            "${AURORA_ROOT}/kernel/core"
            "${AURORA_ROOT}/kernel/mm"
            "${AURORA_ROOT}/kernel/task"
            "${AURORA_ROOT}/kernel/scheduler"
            "${AURORA_ROOT}/kernel/interrupt"
            "${AURORA_ROOT}/vfs"
            "${AURORA_ROOT}/syscall"
            "${AURORA_ROOT}/drivers/display"
            "${AURORA_ROOT}/drivers/power"
            "${AURORA_ROOT}/net"
            "${AURORA_ROOT}/3rdparty/lwip/src/include"
            "${AURORA_ROOT}/adapter/net"
    )
    target_compile_definitions(aurora_fuzzer PRIVATE AURORA_HOST_TEST=1 CONFIG_OTA_DEV_MODE=1)
    target_compile_options(aurora_fuzzer PRIVATE -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer -include "${HOST_PRELUDE_PATH}")
    target_link_options(aurora_fuzzer PRIVATE -fsanitize=fuzzer,address,undefined)
endif()
