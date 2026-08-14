# ST Nucleo-L031K6 (Cortex-M0+, 64KB Flash, 8KB RAM)
include(${CMAKE_SOURCE_DIR}/arch/${ARCH_DIR}/arch.cmake)

set(CPU_FLAGS "${ARCH_CPU_FLAGS}")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CPU_FLAGS} -Os -flto -g -include autoconf.h")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${CPU_FLAGS} -fno-exceptions -fno-rtti -fno-threadsafe-statics -Os -flto -g -include autoconf.h -DLUA_32BITS=1")
set(CMAKE_ASM_FLAGS "${CMAKE_ASM_FLAGS} ${CPU_FLAGS}")

set(BOARD_SOURCES
    ${ARCH_SOURCES}
    apps/m0plus_main.cpp
    apps/shell.cpp
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
    ${CMAKE_SOURCE_DIR}/metrics
    ${CMAKE_SOURCE_DIR}/drivers/display
    ${CMAKE_SOURCE_DIR}/drivers/sensor
    ${CMAKE_SOURCE_DIR}/drivers/input
    ${CMAKE_SOURCE_DIR}/arch/${ARCH_DIR}
    ${CMAKE_SOURCE_DIR}/boards/${BOARD_DIR}
)

set(BOARD_LINK_OPTIONS
    -T ${CMAKE_SOURCE_DIR}/config/linker_m0plus.ld
    -nostartfiles
    -nostdlib++
    -Wl,--gc-sections
    -Wl,-Map=auroraOS.map
    -Os
    --specs=nano.specs
    --specs=nosys.specs
)

set(BOARD_COMPILE_DEFINITIONS
    BOARD_MCU_STM32L031K6
    AURORA_METRICS_HIST_SIZE=16
)

function(board_post_build target)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND arm-none-eabi-objcopy -O binary ${target} auroraOS.bin
        COMMAND arm-none-eabi-objcopy -O ihex ${target} auroraOS.hex
        COMMAND arm-none-eabi-size ${target}
        COMMENT "Building M0+ binary and hex files..."
    )
endfunction()
