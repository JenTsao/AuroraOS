cmake_minimum_required(VERSION 3.20)
project(auroraOS C CXX ASM)

# =====================================================================
# 0. Kconfig Integration
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

# =====================================================================
# 1. 动态板级与架构自动映射
# =====================================================================
if(DEFINED CONFIG_BOARD)
    set(BOARD ${CONFIG_BOARD})
else()
    set(BOARD "lm3s6965-qb" CACHE STRING "Target Hardware Board")
endif()

if(BOARD STREQUAL "lm3s6965-qb")
    set(BOARD_DIR "ti/lm3s6965-qb")
    set(ARCH_DIR  "arm/cortex-m/cm4")
    set(QEMU_MACH "lm3s6965evb")
elseif(BOARD STREQUAL "miband8")
    set(BOARD_DIR "xiaomi/miband8")
    set(ARCH_DIR  "arm/cortex-m/cm4f")
elseif(BOARD STREQUAL "qemu_rv32_virt")
    set(BOARD_DIR "qemu/rv32_virt")
    set(ARCH_DIR  "riscv/rv32imac")
    set(QEMU_MACH "virt")
else()
    message(FATAL_ERROR "Unsupported BOARD: ${BOARD}! Please check boards/ directory.")
endif()

message(STATUS "---------------------------------------------")
message(STATUS "  auroraOS Build Configuration")
message(STATUS "  Target Board : ${BOARD} (${BOARD_DIR})")
message(STATUS "  Target Arch  : ${ARCH_DIR}")
message(STATUS "---------------------------------------------")

# =====================================================================
# 2. 编译标志配置
# =====================================================================
if(BOARD STREQUAL "lm3s6965-qb")
    set(CPU_FLAGS "-mcpu=cortex-m4 -mthumb -mfloat-abi=soft -ffreestanding -fno-builtin -fno-common -Wall -Wextra")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CPU_FLAGS} -O2 -g -include autoconf.h -DLUA_32BITS=1")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${CPU_FLAGS} -fno-exceptions -fno-rtti -fno-threadsafe-statics -O2 -g -include autoconf.h -DLUA_32BITS=1")
    set(CMAKE_ASM_FLAGS "${CMAKE_ASM_FLAGS} ${CPU_FLAGS}")
elseif(BOARD STREQUAL "miband8")
    set(CPU_FLAGS "-mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16 -ffreestanding -fno-builtin -fno-common -Wall -Wextra")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CPU_FLAGS} -O2 -g -include autoconf.h -DLUA_32BITS=1")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${CPU_FLAGS} -fno-exceptions -fno-rtti -fno-threadsafe-statics -O2 -g -include autoconf.h -DLUA_32BITS=1")
    set(CMAKE_ASM_FLAGS "${CMAKE_ASM_FLAGS} ${CPU_FLAGS}")
elseif(BOARD STREQUAL "qemu_rv32_virt")
    set(CPU_FLAGS "-march=rv32imac -mabi=ilp32 -mcmodel=medany -ffreestanding -fno-builtin -fno-common -Wall -Wextra")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CPU_FLAGS} -O2 -g -include autoconf.h -DLUA_32BITS=1")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${CPU_FLAGS} -fno-exceptions -fno-rtti -fno-threadsafe-statics -O2 -g -include autoconf.h -DLUA_32BITS=1")
    set(CMAKE_ASM_FLAGS "${CMAKE_ASM_FLAGS} ${CPU_FLAGS}")
endif()

# =====================================================================
# 3. 源码收集
# =====================================================================
set(SOURCES
    boot/uart.c
    kernel/memory.cpp
    kernel/libc.cpp
    kernel/posix.cpp
    kernel/symbol_export.cpp
    kernel/ota.cpp
    metrics/metrics.cpp
)

if(BOARD STREQUAL "qemu_rv32_virt")
    list(APPEND SOURCES
        arch/riscv/rv32imac/boot.S
        arch/riscv/rv32imac/trap.cpp
        arch/riscv/rv32imac/trap_vector.S
    )
else()
    list(APPEND SOURCES
        boot/boot.S
        boot/interrupts.cpp
    )
endif()

if(BOARD STREQUAL "lm3s6965-qb")
    list(APPEND SOURCES
        apps/shell.cpp
        apps/elf_loader.cpp
        apps/lua_ui_binding.cpp
        apps/kernel.cpp
        apps/net_app.cpp
    )
elseif(BOARD STREQUAL "miband8")
    list(APPEND SOURCES
        arch/arm/cortex-m/cm4f/context_switch.S
        apps/watch/miband_main.cpp
        apps/watch/watch_app.cpp
    )
elseif(BOARD STREQUAL "qemu_rv32_virt")
    list(APPEND SOURCES
        apps/shell.cpp
        apps/elf_loader.cpp
        apps/lua_ui_binding.cpp
        apps/kernel.cpp
        apps/net_app.cpp
    )
endif()

if(CONFIG_VFS)
    list(APPEND SOURCES vfs/vfs.cpp)
    list(APPEND SOURCES vfs/softbus.cpp)   # SerialRpcBus UART RPC 总线
    if(CONFIG_FS_RAMFS)
        list(APPEND SOURCES vfs/ramfs.cpp)
    endif()
    list(APPEND SOURCES 3rdparty/littlefs/lfs.c 3rdparty/littlefs/lfs_util.c)
endif()

if(CONFIG_NETWORKING AND CONFIG_NET_LWIP)
    file(GLOB LWIP_SOURCES 
        "3rdparty/lwip/src/api/*.c"
        "3rdparty/lwip/src/core/*.c"
        "3rdparty/lwip/src/core/ipv4/*.c"
        "3rdparty/lwip/src/netif/ethernet.c"
    )
    list(APPEND SOURCES
        net/eth_driver.cpp
        adapter/net/sys_arch.cpp
        adapter/net/ethernetif.cpp
        ${LWIP_SOURCES}
    )
endif()

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
list(APPEND SOURCES 3rdparty/ed25519/ed25519.c)

add_executable(auroraOS.elf ${SOURCES})

# Bootloader target (TEMPORARILY DISABLED for diagnosis rebuild)
if(BOARD STREQUAL "lm3s6965-qb")
    if(FALSE)
    add_executable(bootloader.elf bootloader/boot_main.cpp boot/boot.S 3rdparty/ed25519/ed25519.c)
    target_include_directories(bootloader.elf PRIVATE 
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/config
    )
    target_link_options(bootloader.elf PRIVATE
        -T ${CMAKE_SOURCE_DIR}/config/linker_bootloader.ld
        -nostartfiles
        -Wl,--gc-sections
        --specs=nano.specs
        --specs=nosys.specs
    )
    add_dependencies(auroraOS.elf bootloader.elf)
    endif()
endif()

# =====================================================================
# 4. 头文件包含路径
# =====================================================================
if(BOARD STREQUAL "lm3s6965-qb")
    target_include_directories(auroraOS.elf PRIVATE 
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/config
        ${CMAKE_SOURCE_DIR}/boot
        ${CMAKE_SOURCE_DIR}/kernel
        ${CMAKE_SOURCE_DIR}/syscall
        ${CMAKE_SOURCE_DIR}/vfs
        ${CMAKE_SOURCE_DIR}/apps
        ${CMAKE_SOURCE_DIR}/net
        ${CMAKE_SOURCE_DIR}/adapter/net
        ${CMAKE_SOURCE_DIR}/3rdparty/lwip/src/include
        ${CMAKE_SOURCE_DIR}/3rdparty/littlefs
        ${CMAKE_SOURCE_DIR}/drivers/display
        ${CMAKE_SOURCE_DIR}/drivers/sensor
        ${CMAKE_SOURCE_DIR}/drivers/input
        ${CMAKE_SOURCE_DIR}/apps/watch
        ${CMAKE_SOURCE_DIR}/arch/${ARCH_DIR}
        ${CMAKE_SOURCE_DIR}/boards/${BOARD_DIR}
    )
elseif(BOARD STREQUAL "qemu_rv32_virt")
    target_include_directories(auroraOS.elf PRIVATE 
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/config
        ${CMAKE_SOURCE_DIR}/boot
        ${CMAKE_SOURCE_DIR}/kernel
        ${CMAKE_SOURCE_DIR}/syscall
        ${CMAKE_SOURCE_DIR}/vfs
        ${CMAKE_SOURCE_DIR}/apps
        ${CMAKE_SOURCE_DIR}/net
        ${CMAKE_SOURCE_DIR}/adapter/net
        ${CMAKE_SOURCE_DIR}/3rdparty/lwip/src/include
        ${CMAKE_SOURCE_DIR}/3rdparty/littlefs
        ${CMAKE_SOURCE_DIR}/drivers/display
        ${CMAKE_SOURCE_DIR}/drivers/sensor
        ${CMAKE_SOURCE_DIR}/drivers/input
        ${CMAKE_SOURCE_DIR}/arch/${ARCH_DIR}
        ${CMAKE_SOURCE_DIR}/boards/${BOARD_DIR}
    )
elseif(BOARD STREQUAL "miband8")
    target_include_directories(auroraOS.elf PRIVATE 
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/config
        ${CMAKE_SOURCE_DIR}/boot
        ${CMAKE_SOURCE_DIR}/kernel
        ${CMAKE_SOURCE_DIR}/kernel/power
        ${CMAKE_SOURCE_DIR}/syscall
        ${CMAKE_SOURCE_DIR}/vfs
        ${CMAKE_SOURCE_DIR}/apps
        ${CMAKE_SOURCE_DIR}/drivers/display
        ${CMAKE_SOURCE_DIR}/drivers/sensor
        ${CMAKE_SOURCE_DIR}/drivers/input
        ${CMAKE_SOURCE_DIR}/net/ble
        ${CMAKE_SOURCE_DIR}/apps/watch
        ${CMAKE_SOURCE_DIR}/arch/${ARCH_DIR}
        ${CMAKE_SOURCE_DIR}/boards/${BOARD_DIR}
    )
endif()

# =====================================================================
# 5. 链接器与库配置
# =====================================================================
if(BOARD STREQUAL "lm3s6965-qb")
    target_link_options(auroraOS.elf PRIVATE
        -T ${CMAKE_SOURCE_DIR}/config/linker.ld
        -nostartfiles
        -Wl,--gc-sections
        --specs=nano.specs
        --specs=nosys.specs
    )
elseif(BOARD STREQUAL "qemu_rv32_virt")
    target_link_options(auroraOS.elf PRIVATE
        -T ${CMAKE_SOURCE_DIR}/config/linker_rv32.ld
        -nostartfiles
        -Wl,--gc-sections
        --specs=nano.specs
        --specs=nosys.specs
    )
elseif(BOARD STREQUAL "miband8")
    target_link_options(auroraOS.elf PRIVATE
        -T ${CMAKE_SOURCE_DIR}/config/linker_miband.ld
        -nostartfiles
        -Wl,--gc-sections
        --specs=nano.specs
        --specs=nosys.specs
    )
endif()

target_compile_definitions(auroraOS.elf PRIVATE LFS_NO_DEBUG LFS_NO_WARN LFS_NO_ERROR LFS_NO_ASSERT)

# =====================================================================
# 6. 生成固件产物与内存占用报告
# =====================================================================
if(BOARD STREQUAL "miband8")
    add_custom_command(TARGET auroraOS.elf POST_BUILD
        COMMAND arm-none-eabi-objcopy -O binary auroraOS.elf auroraOS.bin
        COMMAND arm-none-eabi-objcopy -O ihex auroraOS.elf auroraOS.hex
        COMMAND arm-none-eabi-size auroraOS.elf
        COMMENT "Building raw binary and hex files, and printing size..."
    )
elseif(BOARD STREQUAL "lm3s6965-qb")
    add_custom_command(TARGET auroraOS.elf POST_BUILD
        COMMAND arm-none-eabi-objcopy -O binary auroraOS.elf auroraOS.bin
        COMMAND arm-none-eabi-size auroraOS.elf
        COMMENT "Building App binary..."
    )
    if(FALSE)
    add_custom_command(TARGET bootloader.elf POST_BUILD
        COMMAND arm-none-eabi-objcopy -O binary bootloader.elf bootloader.bin
        COMMAND arm-none-eabi-size bootloader.elf
        COMMENT "Building Bootloader binary..."
    )
    add_custom_command(TARGET auroraOS.elf POST_BUILD
        COMMAND python ${CMAKE_SOURCE_DIR}/scripts/build_image.py bootloader.bin auroraOS.bin flash.bin
        COMMENT "Packing Secure Boot image: flash.bin"
    )
    endif()
endif()
