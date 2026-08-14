# AuroraOS SDK CMake Toolchain Integration
# Include this file in your application's CMakeLists.txt

if (NOT DEFINED ENV{AURORA_SDK_DIR})
    set(AURORA_SDK_DIR ${CMAKE_CURRENT_LIST_DIR}/..)
else()
    set(AURORA_SDK_DIR $ENV{AURORA_SDK_DIR})
endif()

message(STATUS "Using Aurora SDK at: ${AURORA_SDK_DIR}")

# Add SDK headers to include path
include_directories(${AURORA_SDK_DIR}/include)

# Compile the SDK runtime sources into a static library
file(GLOB_RECURSE SDK_RUNTIME_SOURCES 
    "${AURORA_SDK_DIR}/src/auroraos/runtime/*.cpp"
    "${AURORA_SDK_DIR}/src/auroraos/syscall/*.c"
    "${AURORA_SDK_DIR}/src/auroraos/syscall/*.cpp"
    "${AURORA_SDK_DIR}/src/auroraos/syscall/*.S"
)
add_library(aurora_runtime STATIC ${SDK_RUNTIME_SOURCES})
target_include_directories(aurora_runtime 
    PUBLIC 
        ${AURORA_SDK_DIR}/include
    PRIVATE
        ${AURORA_SDK_DIR}/include/auroraos/runtime
        ${AURORA_SDK_DIR}/include/auroraos/syscall
)

# (Optional) If cross-compiling, you would set CMAKE_SYSTEM_NAME, compilers, etc. here
# For QEMU/LM3S, it's typically arm-none-eabi-gcc
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
        add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
            COMMAND arm-none-eabi-objcopy -O binary $<TARGET_FILE:${TARGET_NAME}> ${TARGET_NAME}.bin
            COMMENT "Generating raw binary for ${TARGET_NAME}"
        )
    endif()
endmacro()
