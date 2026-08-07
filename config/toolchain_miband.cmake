# ============================================================
# Toolchain file for Xiaomi Mi Band 8 (Apollo3 Blue / Cortex-M4F)
# ============================================================
# 使用方式:
#   cmake -DBOARD=miband8 \
#     -DCMAKE_TOOLCHAIN_FILE=../config/toolchain_miband.cmake \
#     -DCMAKE_BUILD_TYPE=MinSizeRel ..
# ============================================================

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR ARM)

# 交叉编译工具链
set(CMAKE_C_COMPILER   arm-none-eabi-gcc)
set(CMAKE_CXX_COMPILER arm-none-eabi-g++)
set(CMAKE_ASM_COMPILER arm-none-eabi-gcc)
set(CMAKE_AR           arm-none-eabi-ar)
set(CMAKE_OBJCOPY      arm-none-eabi-objcopy)
set(CMAKE_OBJDUMP      arm-none-eabi-objdump)
set(CMAKE_SIZE         arm-none-eabi-size)

# 跳过编译器检测阶段的链接测试（交叉编译环境下无法运行目标代码）
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# ============================================================
# Cortex-M4F 硬件浮点单元 (FPU) 编译标志
# -mfloat-abi=hard  : 使用硬件浮点 ABI（与 CMakeLists.txt 中 miband8 的 CPU_FLAGS 一致）
# -mfpu=fpv4-sp-d16 : Apollo3 Blue 的 FPU 是 FPv4-SP (16个双精度寄存器)
# ============================================================
set(CPU_FLAGS "-mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16 -ffreestanding -fno-builtin -fno-common")

set(CMAKE_C_FLAGS_INIT   "${CPU_FLAGS}")
set(CMAKE_CXX_FLAGS_INIT "${CPU_FLAGS} -fno-exceptions -fno-rtti -fno-threadsafe-statics")
set(CMAKE_ASM_FLAGS_INIT "${CPU_FLAGS}")

# 链接选项统一在 CMakeLists.txt 的 target_link_options 中管理，避免重复指定
