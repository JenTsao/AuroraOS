# Xiaomi Smart Band 8 (Ambiq Apollo3 Blue / Cortex-M4F)
include(${CMAKE_SOURCE_DIR}/arch/${ARCH_DIR}/arch.cmake)

set(CPU_FLAGS "${ARCH_CPU_FLAGS}")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CPU_FLAGS} -O2 -g -include autoconf.h -DLUA_32BITS=1")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${CPU_FLAGS} -fno-exceptions -fno-rtti -fno-threadsafe-statics -O2 -g -include autoconf.h -DLUA_32BITS=1")
set(CMAKE_ASM_FLAGS "${CMAKE_ASM_FLAGS} ${CPU_FLAGS}")

set(BOARD_SOURCES
    ${ARCH_SOURCES}
    apps/watch/miband_main.cpp
    apps/watch/watch_app.cpp
    boards/xiaomi/miband8/board.cpp
    boards/xiaomi/miband8/hal_impl.cpp
    apps/lua_ui_binding.cpp
    kernel/core/symbol_export.cpp
    kernel/core/ota.cpp
    3rdparty/ed25519/ed25519.c
)

# miband8 requires Lua VM for mini program engine
file(GLOB MIBAND_LUA_SOURCES "${CMAKE_SOURCE_DIR}/3rdparty/lua/*.c")
list(REMOVE_ITEM MIBAND_LUA_SOURCES
    "${CMAKE_SOURCE_DIR}/3rdparty/lua/lua.c"
    "${CMAKE_SOURCE_DIR}/3rdparty/lua/luac.c"
    "${CMAKE_SOURCE_DIR}/3rdparty/lua/loslib.c"
    "${CMAKE_SOURCE_DIR}/3rdparty/lua/liolib.c"
    "${CMAKE_SOURCE_DIR}/3rdparty/lua/lmathlib.c"
    "${CMAKE_SOURCE_DIR}/3rdparty/lua/loadlib.c"
    "${CMAKE_SOURCE_DIR}/3rdparty/lua/lstrlib.c"
    "${CMAKE_SOURCE_DIR}/3rdparty/lua/linit.c"
)
list(APPEND BOARD_SOURCES ${MIBAND_LUA_SOURCES})

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
    ${CMAKE_SOURCE_DIR}/kernel/power
    ${CMAKE_SOURCE_DIR}/syscall
    ${CMAKE_SOURCE_DIR}/vfs
    ${CMAKE_SOURCE_DIR}/apps
    ${CMAKE_SOURCE_DIR}/3rdparty/littlefs
    ${CMAKE_SOURCE_DIR}/3rdparty/lua
    ${CMAKE_SOURCE_DIR}/drivers/display
    ${CMAKE_SOURCE_DIR}/drivers/sensor
    ${CMAKE_SOURCE_DIR}/drivers/input
    ${CMAKE_SOURCE_DIR}/drivers/storage
    ${CMAKE_SOURCE_DIR}/experimental/net/ble
    ${CMAKE_SOURCE_DIR}/net/ble
    ${CMAKE_SOURCE_DIR}/apps/watch
    ${CMAKE_SOURCE_DIR}/ui
    ${CMAKE_SOURCE_DIR}/ui/widgets
    ${CMAKE_SOURCE_DIR}/metrics
    ${CMAKE_SOURCE_DIR}/arch/${ARCH_DIR}
    ${CMAKE_SOURCE_DIR}/boards/${BOARD_DIR}
)

set(BOARD_LINK_OPTIONS
    -T ${CMAKE_SOURCE_DIR}/config/linker_miband.ld
    -nostartfiles
    -Wl,--gc-sections
    --specs=nosys.specs
)

set(BOARD_LINK_LIBRARIES gcc)

set(BOARD_COMPILE_DEFINITIONS
    AURORA_METRICS_HIST_SIZE=16
    AURORA_FB_CHUNK_HEIGHT=30
    CONFIG_OTA_DEV_MODE=1
    CONFIG_BOARD_MIBAND8=1
    # Dev/QEMU target: bypass the SoftBus per-device key provisioning
    # fail-closed guard in net/distributed_bus.hpp. Real hardware must wire
    # a key from Secure Element / encrypted OTP and drop this define.
    DEBUG_BYPASS_SOFTBUS_KEY
)

function(board_post_build target)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND arm-none-eabi-size ${target}
        COMMENT "Building raw binary and hex files, and printing size..."
    )
endfunction()
