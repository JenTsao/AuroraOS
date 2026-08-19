set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

# Prefer aarch64-none-elf if available, fallback to aarch64-linux-gnu
if(NOT DEFINED CMAKE_C_COMPILER)
    find_program(AARCH64_NONE_ELF_GCC aarch64-none-elf-gcc)
    if(AARCH64_NONE_ELF_GCC)
        set(TOOLCHAIN_PREFIX aarch64-none-elf-)
    else()
        set(TOOLCHAIN_PREFIX aarch64-linux-gnu-)
    endif()

    set(CMAKE_C_COMPILER ${TOOLCHAIN_PREFIX}gcc)
    set(CMAKE_CXX_COMPILER ${TOOLCHAIN_PREFIX}g++)
    set(CMAKE_ASM_COMPILER ${TOOLCHAIN_PREFIX}gcc)
    set(CMAKE_AR ${TOOLCHAIN_PREFIX}ar)
    set(CMAKE_OBJCOPY ${TOOLCHAIN_PREFIX}objcopy)
    set(CMAKE_OBJDUMP ${TOOLCHAIN_PREFIX}objdump)
    set(CMAKE_SIZE ${TOOLCHAIN_PREFIX}size)
endif()

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
