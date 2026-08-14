# RISC-V RV32IMAC Architecture Configuration
set(ARCH_CPU_FLAGS "-march=rv32imac_zicsr_zifencei -mabi=ilp32 -mcmodel=medany -ffreestanding -fno-builtin -fno-common -Wall -Wextra")

set(ARCH_SOURCES
    ${CMAKE_SOURCE_DIR}/arch/riscv/rv32imac/boot.S
    ${CMAKE_SOURCE_DIR}/arch/riscv/rv32imac/trap.cpp
    ${CMAKE_SOURCE_DIR}/arch/riscv/rv32imac/trap_vector.S
    ${CMAKE_SOURCE_DIR}/boot/interrupts.cpp
)

set(ARCH_COMPILE_DEFINITIONS ARCH_RISCV32=1)
