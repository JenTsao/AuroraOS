# RISC-V RV32 Virt (QEMU Virt Target)
include(${CMAKE_SOURCE_DIR}/arch/${ARCH_DIR}/arch.cmake)

set(CPU_FLAGS "${ARCH_CPU_FLAGS}")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CPU_FLAGS} -O2 -g -include autoconf.h -DLUA_32BITS=1")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${CPU_FLAGS} -fno-exceptions -fno-rtti -fno-threadsafe-statics -O2 -g -include autoconf.h -DLUA_32BITS=1")
set(CMAKE_ASM_FLAGS "${CMAKE_ASM_FLAGS} ${CPU_FLAGS}")

set(BOARD_SOURCES
    ${ARCH_SOURCES}
    apps/shell.cpp
    apps/elf_loader.cpp
    apps/lua_ui_binding.cpp
    apps/kernel.cpp
    apps/net_app.cpp
    kernel/core/symbol_export.cpp
    kernel/core/ota.cpp
    3rdparty/ed25519/ed25519.c
)

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
    ${CMAKE_SOURCE_DIR}/arch/${ARCH_DIR}
    ${CMAKE_SOURCE_DIR}/boards/${BOARD_DIR}
)

set(BOARD_LINK_OPTIONS
    -T ${CMAKE_SOURCE_DIR}/config/linker_rv32.ld
    -nostartfiles
    -nostdlib++
    -Wl,--gc-sections
    -flto
)

set(BOARD_LINK_LIBRARIES gcc)

set(BOARD_COMPILE_DEFINITIONS
    CONFIG_OTA_DEV_MODE=1
    # QEMU/dev target: bypass the SoftBus per-device key provisioning
    # fail-closed guard in net/distributed_bus.hpp. Real hardware must wire
    # a key from Secure Element / encrypted OTP and drop this define.
    DEBUG_BYPASS_SOFTBUS_KEY
    ${ARCH_COMPILE_DEFINITIONS}
)
