# Cortex-M7 Architecture Configuration (ARMv7E-M + FPv5-SP + I/D-Cache)
set(ARCH_CPU_FLAGS "-mcpu=cortex-m7 -mthumb -mfloat-abi=hard -mfpu=fpv5-sp-d16 -ffreestanding -fno-builtin -fno-common -Wall -Wextra -ffunction-sections -fdata-sections")

set(ARCH_SOURCES
    ${CMAKE_SOURCE_DIR}/arch/arm/cortex-m/cm7/boot.S
    ${CMAKE_SOURCE_DIR}/arch/arm/cortex-m/cm7/context_switch.S
    ${CMAKE_SOURCE_DIR}/arch/arm/cortex-m/cm7/early_init.cpp
    ${CMAKE_SOURCE_DIR}/boot/interrupts.cpp
)
