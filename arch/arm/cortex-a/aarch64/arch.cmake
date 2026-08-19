# AArch64 (ARMv8-A Cortex-A) Architecture Configuration
set(ARCH_CPU_FLAGS "-march=armv8-a -mcpu=cortex-a53 -ffreestanding -fno-builtin -fno-common -Wall -Wextra")

set(ARCH_SOURCES
    ${CMAKE_SOURCE_DIR}/arch/arm/cortex-a/aarch64/boot.S
    ${CMAKE_SOURCE_DIR}/arch/arm/cortex-a/aarch64/exceptions.S
    ${CMAKE_SOURCE_DIR}/arch/arm/cortex-a/aarch64/aarch64_exceptions.cpp
    ${CMAKE_SOURCE_DIR}/arch/arm/cortex-a/gic/gic.cpp
    ${CMAKE_SOURCE_DIR}/arch/arm/cortex-a/mmu/mmu_manager.cpp
    ${CMAKE_SOURCE_DIR}/boot/interrupts.cpp
)

set(ARCH_COMPILE_DEFINITIONS ARCH_AARCH64=1)
