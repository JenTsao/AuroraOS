# Cortex-M4F Architecture Configuration
set(ARCH_CPU_FLAGS "-mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16 -ffreestanding -fno-builtin -fno-common -Wall -Wextra")

set(ARCH_SOURCES
    ${CMAKE_SOURCE_DIR}/arch/arm/cortex-m/cm4f/context_switch.S
    ${CMAKE_SOURCE_DIR}/boot/boot.S
    ${CMAKE_SOURCE_DIR}/boot/interrupts.cpp
)
