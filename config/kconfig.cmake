# =============================================================================
# config/kconfig.cmake — Kconfig generation, overlay and -DCONFIG_* guard
#
# Included from the root CMakeLists.txt after BOARD resolution. Kept out of the
# root file so it stays small (AGENTS.md §24) while the whole configuration
# contract lives next to the Kconfig sources it describes.
# =============================================================================

# -----------------------------------------------------------------------------
# 2. Kconfig Integration
# -----------------------------------------------------------------------------
# 三件套（autoconf.h / autoconf.cmake / autoconf.config）生成进**构建目录**，
# 每次 configure 完整重算，单一事实源 = Kconfig。生成物不入 git、不跨板共享，
# 从机制上杜绝"上一块板的配置投毒下一块板"。
# 目录内含 autoconf.h，后续 board.cmake 用 -include autoconf.h 即可命中。
#
# AURORA_KCONFIG_OVERLAY 允许叠加一份 .config（menuconfig 产物）覆盖板级默认，
# 供 CI 的 Kconfig 组合矩阵使用。未设置时行为与纯板级默认完全一致。
set(AURORA_KCONFIG_OVERLAY "" CACHE FILEPATH
    "Optional .config overlay layered on top of the board defaults")
set(_aurora_genconfig_args --board ${BOARD} --out ${CMAKE_BINARY_DIR})
if(AURORA_KCONFIG_OVERLAY)
    if(NOT EXISTS "${AURORA_KCONFIG_OVERLAY}")
        message(FATAL_ERROR "AURORA_KCONFIG_OVERLAY does not exist: ${AURORA_KCONFIG_OVERLAY}")
    endif()
    list(APPEND _aurora_genconfig_args --config "${AURORA_KCONFIG_OVERLAY}")
endif()
execute_process(
    COMMAND python ${CMAKE_SOURCE_DIR}/scripts/genconfig.py ${_aurora_genconfig_args}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    RESULT_VARIABLE GENCONFIG_RESULT
)
if(NOT GENCONFIG_RESULT EQUAL 0)
    message(FATAL_ERROR "genconfig.py failed! Please ensure kconfiglib is installed and Kconfig exists.")
endif()

# CMake 的 if(CONFIG_XXX) 分支与 C 预处理读同一份生成物，天然同源一致。
include(${CMAKE_BINARY_DIR}/autoconf.cmake)

# -----------------------------------------------------------------------------
# 2b. 拒绝命令行 -DCONFIG_* 空操作
# -----------------------------------------------------------------------------
# 配置的单一事实源是 Kconfig（见 §2）。历史上 MiBand 构建传过
# -DCONFIG_ARCH_CORTEX_M4F=y / -DCONFIG_SYSTEM_CORE_CLOCK=96000000，这两个
# 符号实际由 Kconfig.miband 提供，命令行传值没有任何消费方——看起来配置了，
# 实际零效果，属于最难发现的"假配置"。这里把静默变成硬错误：任何 CONFIG_*
# 缓存变量一律拒绝，配置只能经由 Kconfig 或 AURORA_KCONFIG_OVERLAY 表达。
get_cmake_property(_aurora_cache_vars CACHE_VARIABLES)
foreach(_var IN LISTS _aurora_cache_vars)
    if(_var MATCHES "^CONFIG_")
        message(FATAL_ERROR
            "-D${_var}=... is not a supported way to configure AuroraOS: "
            "CONFIG_* values come from Kconfig only (AGENTS.md §25).\n"
            "Historically such flags were silently ignored, so a build could look "
            "configured while running the board defaults.\n"
            "To change configuration: edit the Kconfig default for board '${BOARD}', "
            "or pass a .config overlay with -DAURORA_KCONFIG_OVERLAY=<file>.")
    endif()
endforeach()

# Defense-in-depth: production hardware targets MUST NOT use mock OTA crypto.
if(CONFIG_OTA_DEV_MODE AND NOT (BOARD STREQUAL "lm3s6965-qb" OR BOARD STREQUAL "qemu_rv32_virt" OR BOARD STREQUAL "qemu_aarch64_virt" OR BOARD STREQUAL "miband8" OR BOARD STREQUAL "nucleo_l031k6" OR BOARD STREQUAL "qemu_mps2_an500"))
    message(FATAL_ERROR
        "CONFIG_OTA_DEV_MODE=y is enabled, but target board '${BOARD}' DOES NOT "
        "appear to be a QEMU virtual board or whitelisted development target.\n"
        "Mock Ed25519 signatures and placeholder root public keys are NOT allowed "
        "on production real hardware.\n"
        "To fix: run 'make menuconfig' and disable 'OTA Development Mode' "
        "under 'System Core Configuration'."
    )
endif()
