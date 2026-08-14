# TI LM3S6965-QB (Cortex-M3 QEMU Target)
include(${CMAKE_SOURCE_DIR}/arch/${ARCH_DIR}/arch.cmake)

set(CPU_FLAGS "${ARCH_CPU_FLAGS}")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CPU_FLAGS} -Oz -flto -g -include autoconf.h -DLUA_32BITS=1")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${CPU_FLAGS} -fno-exceptions -fno-rtti -fno-threadsafe-statics -Oz -flto -g -include autoconf.h -DLUA_32BITS=1")
set(CMAKE_ASM_FLAGS "${CMAKE_ASM_FLAGS} ${CPU_FLAGS}")

set(BOARD_SOURCES
    ${ARCH_SOURCES}
    apps/shell.cpp
    apps/kernel.cpp
    apps/net_app.cpp
    kernel/core/symbol_export.cpp
    kernel/core/ota.cpp
    3rdparty/ed25519/ed25519.c
)

if(CONFIG_ELF_LOADER)
    list(APPEND BOARD_SOURCES apps/elf_loader.cpp)
endif()
if(CONFIG_LUA_VM)
    list(APPEND BOARD_SOURCES apps/lua_ui_binding.cpp)
endif()

set(BOARD_INCLUDE_DIRS
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/config
    ${CMAKE_SOURCE_DIR}/boot
    ${CMAKE_SOURCE_DIR}/kernel
    ${CMAKE_SOURCE_DIR}/kernel/core
    ${CMAKE_SOURCE_DIR}/kernel/mm
    ${CMAKE_SOURCE_DIR}/kernel/task
    ${CMAKE_SOURCE_DIR}/kernel/scheduler
    ${CMAKE_SOURCE_DIR}/kernel/interrupt
    ${CMAKE_SOURCE_DIR}/syscall
    ${CMAKE_SOURCE_DIR}/vfs
    ${CMAKE_SOURCE_DIR}/apps
    ${CMAKE_SOURCE_DIR}/net
    ${CMAKE_SOURCE_DIR}/net/firewall
    ${CMAKE_SOURCE_DIR}/net/scanner
    ${CMAKE_SOURCE_DIR}/net/wireless
    ${CMAKE_SOURCE_DIR}/drivers/usb
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

set(BOARD_LINK_OPTIONS
    -T ${CMAKE_SOURCE_DIR}/config/linker_qemu.ld
    -nostartfiles
    -Wl,--gc-sections
    -flto
    --specs=nosys.specs
)

set(BOARD_COMPILE_DEFINITIONS
    CONFIG_OTA_DEV_MODE=1
)

function(board_post_build target)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND arm-none-eabi-objcopy -O binary ${target} auroraOS.bin
        COMMAND arm-none-eabi-size ${target}
        COMMENT "Building App binary..."
    )
    if(TARGET bootloader.elf)
        add_custom_command(TARGET bootloader.elf POST_BUILD
            COMMAND arm-none-eabi-objcopy -O binary bootloader.elf bootloader.bin
            COMMAND arm-none-eabi-size bootloader.elf
            COMMENT "Building Bootloader binary..."
        )
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND python ${CMAKE_SOURCE_DIR}/scripts/build_image.py bootloader.bin auroraOS.bin flash.bin
            COMMENT "Packing Secure Boot image: flash.bin"
        )
    endif()
endfunction()
