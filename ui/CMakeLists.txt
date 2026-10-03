# =============================================================================
# ui/CMakeLists.txt — AuroraOS UI 子系统（header-only 契约）
#
# 定位
# ----
# 板级接入的唯一钩子是 boards/*/board.cmake 的 BOARD_SOURCES / BOARD_INCLUDE_DIRS
# （合规项 B-02）。ui/ 目前是纯 header-only，真正编译 UI 的只有 miband8，其余
# 板卡仅通过 apps/kernel.cpp / apps/lua_ui_binding.cpp / kernel/core/symbol_export.cpp
# 传递包含少量 ui 头文件。
#
# 因此这里只建 INTERFACE 库：**不产出任何目标文件** —— 不编译 UI 的板卡
# （尤其 nucleo-l031k6 的 64KB Flash 硬限）固件体积零增量。
# 若将来 ui/ 出现 .cpp，请加到 boards/<board>/board.cmake 的 BOARD_SOURCES，
# 不要在这里无条件 add_library(... STATIC/OBJECT)，否则 6 个板卡会全部吃进 UI 代码。
# =============================================================================

add_library(aurora_ui INTERFACE)

# INTERFACE：任何 target_link_libraries(<tgt> aurora_ui) 的 target 自动获得这些 -I。
target_include_directories(aurora_ui INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}          # ui/         → #include "ui_manager.hpp"
    ${CMAKE_CURRENT_SOURCE_DIR}/widgets  # ui/widgets/ → #include "button.hpp"
)

# 钉死 C++17。固件构建本身没有设 CMAKE_CXX_STANDARD（吃编译器默认 gnu++17），
# 显式钉住可与 tests/CMakeLists.txt 的 C++17 对齐，并防止工具链升级
# （GCC 15+/16 默认 gnu++20）造成的行为漂移。
target_compile_features(aurora_ui INTERFACE cxx_std_17)
