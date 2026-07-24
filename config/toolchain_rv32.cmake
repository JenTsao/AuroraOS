set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR RISC-V)

set(CMAKE_C_COMPILER riscv64-unknown-elf-gcc)
set(CMAKE_CXX_COMPILER riscv64-unknown-elf-g++)
set(CMAKE_ASM_COMPILER riscv64-unknown-elf-gcc)
set(CMAKE_AR riscv64-unknown-elf-ar)
set(CMAKE_OBJCOPY riscv64-unknown-elf-objcopy)
set(CMAKE_OBJDUMP riscv64-unknown-elf-objdump)
set(CMAKE_SIZE riscv64-unknown-elf-size)

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# RV32 flags: 32-bit RISC-V with Integer, Multiply, Atomic, Compressed + Zicsr + Zifencei extensions
# Use picolibc specs for standard C library headers (errno.h, string.h, etc.)
set(CMAKE_C_FLAGS_INIT "-march=rv32imac_zicsr_zifencei -mabi=ilp32 -mcmodel=medany -ffreestanding -fno-builtin -fno-common --specs=picolibc.specs")
set(CMAKE_CXX_FLAGS_INIT "-march=rv32imac_zicsr_zifencei -mabi=ilp32 -mcmodel=medany -ffreestanding -fno-builtin -fno-common --specs=picolibc.specs")
set(CMAKE_ASM_FLAGS_INIT "-march=rv32imac_zicsr_zifencei -mabi=ilp32")
