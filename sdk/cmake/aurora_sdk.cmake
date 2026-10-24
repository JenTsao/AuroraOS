# AuroraOS SDK CMake Integration
# Include this file in your application's CMakeLists.txt

if (NOT DEFINED ENV{AURORA_SDK_DIR})
    set(AURORA_SDK_DIR ${CMAKE_CURRENT_LIST_DIR}/..)
else()
    set(AURORA_SDK_DIR $ENV{AURORA_SDK_DIR})
endif()

message(STATUS "Using Aurora SDK at: ${AURORA_SDK_DIR}")

# SDK 运行时源文件：显式清单（与 sdk/scripts/package_sdk.py 的 SOURCE_MANIFEST 对应），
# 不使用 file(GLOB) —— 让源集合的任何变化在评审中可见。
set(AURORA_SDK_RUNTIME_SOURCES
    ${AURORA_SDK_DIR}/src/auroraos/runtime/app_base.cpp
    ${AURORA_SDK_DIR}/src/auroraos/runtime/app_sandbox.cpp
    ${AURORA_SDK_DIR}/src/auroraos/runtime/aurora_runtime.cpp
    ${AURORA_SDK_DIR}/src/auroraos/services/vfs/vfs_client.cpp
    ${AURORA_SDK_DIR}/src/auroraos/services/power/power_client.cpp
    ${AURORA_SDK_DIR}/src/auroraos/services/display/display_client.cpp
    ${AURORA_SDK_DIR}/src/auroraos/services/sensor/sensor_client.cpp
)

add_library(aurora_runtime STATIC ${AURORA_SDK_RUNTIME_SOURCES})

# include/auroraos/<sub> 目录是 PUBLIC 的：SDK 源码与源码树保持逐字一致，内部使用
# "syscall.hpp"、"app_base.hpp"、"../service_registry.hpp" 等相对包含；把这些目录
# 暴露给下游，等价于源码树构建里把 runtime/、syscall/、services/<svc>/ 加入 -I。
target_include_directories(aurora_runtime
    PUBLIC
        ${AURORA_SDK_DIR}/include
        ${AURORA_SDK_DIR}/include/auroraos/runtime
        ${AURORA_SDK_DIR}/include/auroraos/syscall
        ${AURORA_SDK_DIR}/include/auroraos/services/vfs
        ${AURORA_SDK_DIR}/include/auroraos/services/power
        ${AURORA_SDK_DIR}/include/auroraos/services/display
        ${AURORA_SDK_DIR}/include/auroraos/services/sensor
        ${AURORA_SDK_DIR}/include/auroraos/services/net
        ${AURORA_SDK_DIR}/include/auroraos/services/firewall
)

# (Optional) If cross-compiling, you would set CMAKE_SYSTEM_NAME, compilers, etc. here
# NOTE: toolchain 变量必须在 project() 之前设置才完全生效，故这里仅作最简记录。
if (AURORA_TARGET_ARCH STREQUAL "arm")
    set(CMAKE_SYSTEM_NAME Generic)
    set(CMAKE_SYSTEM_PROCESSOR arm)
    set(CMAKE_C_COMPILER arm-none-eabi-gcc)
    set(CMAKE_CXX_COMPILER arm-none-eabi-g++)
endif()

# Provide a macro to easily create an Aurora App
macro(add_aurora_app TARGET_NAME SRC_FILES)
    add_executable(${TARGET_NAME} ${SRC_FILES})

    # Link against the compiled SDK runtime library
    target_link_libraries(${TARGET_NAME} aurora_runtime)

    # Generate binary for QEMU/Hardware
    if (AURORA_TARGET_ARCH)
        if (NOT CMAKE_OBJCOPY)
            set(CMAKE_OBJCOPY arm-none-eabi-objcopy)
        endif()
        add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
            COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${TARGET_NAME}> ${TARGET_NAME}.bin
            COMMENT "Generating raw binary for ${TARGET_NAME}"
        )
    endif()
endmacro()
