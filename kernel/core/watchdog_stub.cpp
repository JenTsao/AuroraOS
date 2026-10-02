// 全工程唯一的 watchdog_feed 定义点。
//
// ⚠️ 为什么这里必须是"唯一且非 weak"的定义：
//   1) 若本文件只提供 weak no-op，而 watchdog_manager.hpp 又提供 inline
//      实现（COMDAT，链接器视角同样是弱符号），两个弱定义并存时链接器按
//      目标文件顺序任选其一 —— Cortex-M7 实测选中 no-op，内核永不喂狗，
//      SoftWdt 在 5000 tick 后误报超时停机。
//   2) 反之，若在 CONFIG_WATCHDOG=y 时整文件裁掉、只留头文件里的 inline，
//      该符号就只在"某个 TU 恰好 odr-use 了它"时才被发射。rv32/aarch64
//      （-O2，无 LTO）没有任何 TU 调用 watchdog_feed → undefined reference；
//      只有启用 -flto 的 lm3s/m7 才侥幸由 LTO 兜出定义。
//   因此改为：无论 CONFIG_WATCHDOG 取何值，本文件都发射一次该符号，行为由
//   编译期开关决定。链接结果与目标文件顺序、是否启用 LTO 均无关。
#include "task.hpp"

#if defined(CONFIG_WATCHDOG)
#include "watchdog_manager.hpp"

void watchdog_feed(uint32_t task_priority) {
    WatchdogManager::instance().on_schedule(task_priority);
}

#else
// 未启用看门狗：空实现，保证符号始终存在。
void watchdog_feed(uint32_t) {}

#endif

__attribute__((weak)) void kernel_cleanup_task_timers(uint32_t) {}
