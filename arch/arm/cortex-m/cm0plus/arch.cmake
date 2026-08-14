# Cortex-M0+ Architecture Configuration
set(ARCH_CPU_FLAGS "-mcpu=cortex-m0plus -mthumb -mfloat-abi=soft -ffreestanding -fno-builtin -fno-common -Wall -Wextra -ffunction-sections -fdata-sections")

set(ARCH_SOURCES
    ${CMAKE_SOURCE_DIR}/arch/arm/cortex-m/cm0plus/boot.S
    ${CMAKE_SOURCE_DIR}/boot/interrupts.cpp
)
