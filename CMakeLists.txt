cmake_minimum_required(VERSION 3.20)
project(auroraOS C CXX ASM)

# =====================================================================
# 0. Global Compiler Options Overrides
# =====================================================================
# Prevent CMake from appending -O3 in Release mode, which blows up firmware size!
set(CMAKE_C_FLAGS_RELEASE "-Os -DNDEBUG")
set(CMAKE_CXX_FLAGS_RELEASE "-Os -DNDEBUG")
set(CMAKE_ASM_FLAGS_RELEASE "-DNDEBUG")

# =====================================================================
# 1. Kconfig Integration
# =====================================================================
execute_process(
    COMMAND python ${CMAKE_SOURCE_DIR}/scripts/genconfig.py
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    RESULT_VARIABLE GENCONFIG_RESULT
)
if(NOT GENCONFIG_RESULT EQUAL 0)
    message(FATAL_ERROR "genconfig.py failed! Please ensure kconfiglib is installed and Kconfig exists.")
endif()

include(${CMAKE_SOURCE_DIR}/config/autoconf.cmake)

# Defense-in-depth: production hardware targets MUST NOT use mock OTA crypto.
if(CONFIG_OTA_DEV_MODE AND NOT (BOARD STREQUAL "lm3s6965-qb" OR BOARD STREQUAL "qemu_rv32_virt" OR BOARD STREQUAL "qemu_aarch64_virt" OR BOARD STREQUAL "miband8" OR BOARD STREQUAL "nucleo_l031k6"))
    message(FATAL_ERROR
        "CONFIG_OTA_DEV_MODE=y is enabled, but target board '${BOARD}' DOES NOT "
        "appear to be a QEMU virtual board or whitelisted development target.\n"
        "Mock Ed25519 signatures and placeholder root public keys are NOT allowed "
        "on production real hardware.\n"
        "To fix: run 'make menuconfig' and disable 'OTA Development Mode' "
        "under 'System Core Configuration'."
    )
endif()

# =====================================================================
# 2. Board & Architecture Resolution
# =====================================================================
if(NOT DEFINED BOARD)
    if(DEFINED CONFIG_BOARD)
        set(BOARD ${CONFIG_BOARD})
    else()
        set(BOARD "lm3s6965-qb" CACHE STRING "Target Hardware Board")
    endif()
endif()

if(BOARD STREQUAL "lm3s6965-qb")
    set(BOARD_DIR "ti/lm3s6965-qb")
    set(ARCH_DIR  "arm/cortex-m/cm3")
    set(QEMU_MACH "lm3s6965evb")
elseif(BOARD STREQUAL "miband8")
    set(BOARD_DIR "xiaomi/miband8")
    set(ARCH_DIR  "arm/cortex-m/cm4f")
elseif(BOARD STREQUAL "qemu_rv32_virt")
    set(BOARD_DIR "qemu/rv32_virt")
    set(ARCH_DIR  "riscv/rv32imac")
    set(QEMU_MACH "virt")
elseif(BOARD STREQUAL "nucleo_l031k6")
    set(BOARD_DIR "st/nucleo-l031k6")
    set(ARCH_DIR  "arm/cortex-m/cm0plus")
elseif(BOARD STREQUAL "qemu_aarch64_virt")
    set(BOARD_DIR "qemu/aarch64_virt")
    set(ARCH_DIR  "arm/cortex-a/aarch64")
    set(QEMU_MACH "virt")
else()
    message(FATAL_ERROR "Unsupported BOARD: ${BOARD}! Please check boards/ directory.")
endif()

message(STATUS "---------------------------------------------")
message(STATUS "  auroraOS Build Configuration")
message(STATUS "  Target Board : ${BOARD} (${BOARD_DIR})")
message(STATUS "  Target Arch  : ${ARCH_DIR}")
message(STATUS "---------------------------------------------")

# Load modular board configuration (CPU flags, compiler options, sources, link flags)
include(${CMAKE_SOURCE_DIR}/boards/${BOARD_DIR}/board.cmake)

# =====================================================================
# 3. Kernel Core & Subsystem Sources
# =====================================================================
set(SOURCES
    boot/uart.c
    kernel/mm/memory.cpp
    kernel/core/libc.cpp
    kernel/core/posix.cpp
    kernel/core/ipc.cpp
    kernel/core/cspace.cpp
    kernel/core/device.cpp
    kernel/core/syscalls.c
    kernel/core/syscall_dispatcher.cpp
    kernel/core/syscall_validator.cpp
    kernel/core/watchdog_stub.cpp
    kernel/task/wait_queue.cpp
    kernel/scheduler/frame_scheduler_v2.cpp
    metrics/metrics.cpp
    hal/secure_storage_stub.cpp
    ${BOARD_SOURCES}
)

if(CONFIG_VFS)
    list(APPEND SOURCES vfs/vfs.cpp services/vfs/vfs_service.cpp)
    if(NOT BOARD STREQUAL "nucleo_l031k6")
        list(APPEND SOURCES vfs/softbus.cpp)
    endif()
    if(CONFIG_FS_RAMFS AND NOT BOARD STREQUAL "nucleo_l031k6")
        list(APPEND SOURCES vfs/ramfs.cpp)
    endif()
    if(NOT BOARD STREQUAL "nucleo_l031k6")
        list(APPEND SOURCES 3rdparty/littlefs/lfs.c 3rdparty/littlefs/lfs_util.c)
    endif()
endif()

if(CONFIG_NETWORKING AND CONFIG_NET_LWIP)
    file(GLOB LWIP_SOURCES 
        "3rdparty/lwip/src/api/*.c"
        "3rdparty/lwip/src/core/*.c"
        "3rdparty/lwip/src/core/ipv4/*.c"
        "3rdparty/lwip/src/netif/ethernet.c"
    )
    list(APPEND SOURCES
        adapter/net/sys_arch.cpp
        adapter/net/aurora_dhcp_opts.c
        net/net_client.cpp
        ${LWIP_SOURCES}
        net/firewall/rule_table.cpp
        net/firewall/stateful_inspector.cpp
        net/firewall/traffic_shaper.cpp
        net/firewall/firewall_engine.cpp
        net/firewall/rule_parser.cpp
        services/firewall/firewall_service.cpp
        services/firewall/firewall_client.cpp
        services/firewall/firewall_audit.cpp
        runtime/app_base.cpp
        runtime/aurora_runtime.cpp
        runtime/app_sandbox.cpp
        net/ble/ble_signature.cpp
    )
    if(CONFIG_BLE_ENABLED)
        list(APPEND SOURCES
            net/ble/hal_ble_impl.cpp
            net/ble/hci/hci_uart_transport.cpp
        )
    endif()
    if(NOT BOARD STREQUAL "qemu_rv32_virt" AND NOT BOARD STREQUAL "miband8" AND NOT BOARD STREQUAL "qemu_aarch64_virt")
        list(APPEND SOURCES
            net/eth_driver.cpp
            net/packet_capture.cpp
            adapter/net/ethernetif.cpp
            drivers/usb/usb_host.cpp
            net/wireless/rtl8187l_monitor.cpp
            net/wireless/rtl8812au_monitor.cpp
            net/wireless/wifi_monitor_task.cpp
            net/scanner/host_discovery.cpp
            net/scanner/port_scanner.cpp
            net/scanner/scan_engine.cpp
            net/scanner/service_detector.cpp
            net/scanner/vuln_probe.cpp
        )
        if(CONFIG_LUA_VM)
            list(APPEND SOURCES net/wireless/wireless_lua_binding.cpp)
            list(APPEND SOURCES net/scanner/scan_lua_binding.cpp)
        endif()
    endif()
endif()

if(CONFIG_LUA_VM AND NOT BOARD STREQUAL "miband8")
    file(GLOB LUA_SOURCES "3rdparty/lua/*.c")
    list(REMOVE_ITEM LUA_SOURCES 
        "${CMAKE_SOURCE_DIR}/3rdparty/lua/lua.c" 
        "${CMAKE_SOURCE_DIR}/3rdparty/lua/luac.c"
        "${CMAKE_SOURCE_DIR}/3rdparty/lua/loslib.c"
        "${CMAKE_SOURCE_DIR}/3rdparty/lua/liolib.c"
        "${CMAKE_SOURCE_DIR}/3rdparty/lua/lmathlib.c"
        "${CMAKE_SOURCE_DIR}/3rdparty/lua/loadlib.c"
        "${CMAKE_SOURCE_DIR}/3rdparty/lua/lstrlib.c"
        "${CMAKE_SOURCE_DIR}/3rdparty/lua/linit.c"
    )
    list(APPEND SOURCES ${LUA_SOURCES})
endif()

# Optional 3rdparty NimBLE port for targets with BLE
if(NOT BOARD STREQUAL "lm3s6965-qb" AND NOT BOARD STREQUAL "qemu_aarch64_virt")
    add_subdirectory(3rdparty/nimble_port)
endif()

# =====================================================================
# 4. Target Definition & Configuration
# =====================================================================
add_executable(auroraOS.elf ${SOURCES})

target_include_directories(auroraOS.elf PRIVATE ${BOARD_INCLUDE_DIRS})
target_link_options(auroraOS.elf PRIVATE ${BOARD_LINK_OPTIONS})

if(BOARD_LINK_LIBRARIES)
    target_link_libraries(auroraOS.elf PRIVATE ${BOARD_LINK_LIBRARIES})
endif()

if(NOT BOARD STREQUAL "lm3s6965-qb" AND NOT BOARD STREQUAL "qemu_aarch64_virt")
    target_link_libraries(auroraOS.elf PRIVATE nimble_host)
endif()

target_compile_definitions(auroraOS.elf PRIVATE
    LFS_NO_DEBUG LFS_NO_WARN LFS_NO_ERROR LFS_NO_ASSERT
    ${BOARD_COMPILE_DEFINITIONS}
)

# Optional Bootloader Target (Secure Boot with Ed25519)
if(BOARD STREQUAL "lm3s6965-qb")
    add_executable(bootloader.elf bootloader/boot_main.cpp bootloader/bootloader_boot.S 3rdparty/ed25519/ed25519.c)
    target_include_directories(bootloader.elf PRIVATE
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/config
    )
    target_compile_definitions(bootloader.elf PRIVATE CONFIG_OTA_DEV_MODE=1)
    target_link_options(bootloader.elf PRIVATE
        -T ${CMAKE_SOURCE_DIR}/config/linker_bootloader.ld
        -nostartfiles
        -Wl,--gc-sections
        -flto
        --specs=nosys.specs
    )
    add_dependencies(auroraOS.elf bootloader.elf)
endif()

# Post-build artifacts generation
if(COMMAND board_post_build)
    board_post_build(auroraOS.elf)
endif()
