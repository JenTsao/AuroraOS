cmake_minimum_required(VERSION 3.10)
project(AuroraAppTemplate)

# Default to the host build for testing if AURORA_SDK_DIR is not set
if (NOT DEFINED ENV{AURORA_SDK_DIR})
    message(WARNING "AURORA_SDK_DIR environment variable not set, using local path if possible.")
endif()

# Include AuroraOS SDK CMake helpers
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/aurora_sdk.cmake)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_aurora_app(my_aurora_app src/main.cpp)

# Link against AuroraOS application libraries
# target_link_libraries(my_aurora_app aurora_runtime aurora_ui aurora_sensor)

