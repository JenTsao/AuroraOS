# =====================================================================
# NimBLE Host Integration for AuroraOS
# =====================================================================

# Enable NimBLE Host
add_library(nimble_host STATIC)

target_include_directories(nimble_host PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/npl_auroraos/include
    ${CMAKE_SOURCE_DIR}/config
    ${CMAKE_SOURCE_DIR}/3rdparty/nimble/nimble/host/include
    ${CMAKE_SOURCE_DIR}/3rdparty/nimble/porting/nimble/include
    ${CMAKE_SOURCE_DIR}/3rdparty/nimble/nimble/include
    ${CMAKE_SOURCE_DIR}/3rdparty/nimble/nimble/transport/include
    ${CMAKE_SOURCE_DIR}/3rdparty/nimble/porting/npl/freertos/include
)

target_compile_definitions(nimble_host PUBLIC
    NIMBLE_CFG_CONTROLLER=0
    NIMBLE_CFG_HOST=1
    MYNEWT_VAL_BLE_HOST=1
    OS_ALIGNMENT=4
)

# Core Host sources
file(GLOB NIMBLE_SRC 
    "${CMAKE_SOURCE_DIR}/3rdparty/nimble/nimble/host/src/*.c"
    "${CMAKE_SOURCE_DIR}/3rdparty/nimble/porting/nimble/src/*.c"
)
target_sources(nimble_host PRIVATE 
    ${NIMBLE_SRC}
    ${CMAKE_CURRENT_SOURCE_DIR}/npl_auroraos/src/nimble_npl_os.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/hci_auroraos/hci_transport_mem.cpp
)

# Disable warnings for 3rdparty code
target_compile_options(nimble_host PRIVATE -w)
