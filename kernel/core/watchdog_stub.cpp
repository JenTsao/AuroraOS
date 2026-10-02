// Default no-op watchdog_feed when CONFIG_WATCHDOG is disabled.
// Weak symbol — overridden by the inline in watchdog_manager.hpp when enabled.
//
// ⚠️ 必须在 CONFIG_WATCHDOG=y 时整体裁掉本文件：watchdog_manager.hpp 中的
//    "实现"是 inline 函数（COMDAT，链接器视角同样是弱符号），与本文件的
//    weak no-op 竞争同一符号。两个弱定义并存时链接器按对象顺序任意择一，
//    Cortex-M7 构建实测选中了 no-op → 内核永不喂狗 → SoftWdt 5000 tick
//    后误报超时停机（rv32/lm3s 构建仅因链接顺序侥幸不同）。裁掉本文件后
//    符号唯一，行为确定。
#include "task.hpp"

#if !defined(CONFIG_WATCHDOG)
__attribute__((weak)) void watchdog_feed(uint32_t) {}
#endif
__attribute__((weak)) void kernel_cleanup_task_timers(uint32_t) {}
