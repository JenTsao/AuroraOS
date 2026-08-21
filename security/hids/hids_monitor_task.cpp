// =============================================================================
// security/hids/hids_monitor_task.cpp
//
// HIDS 监控任务：低优先级后台任务，周期性驱动 HidsEngine::tick() 执行
// 文件完整性 / 进程监控 / 权限审计 / Rootkit 扫描。
// =============================================================================

#include "hids_engine.hpp"
#include "../../kernel/task/task.hpp"

namespace aurora {
namespace hids {

namespace {

alignas(8) static uint32_t g_hids_task_stack[256];

// 两次扫描之间的间隔（ms）
constexpr uint32_t kScanIntervalMs = 500;

} // namespace

// ---- HIDS 监控任务入口 ----
void hids_monitor_task_entry() {
    HidsEngine& hids = HidsEngine::instance();
    hids.init();

    while (true) {
        hids.tick();
        Scheduler::instance().sleep_ms(kScanIntervalMs);
    }
}

// ---- 创建 HIDS 监控任务 ----
bool create_hids_monitor_task() {
    HidsEngine::instance().init();

    TaskControlBlock* tcb = Scheduler::instance().create_task(
        hids_monitor_task_entry,
        g_hids_task_stack,
        sizeof(g_hids_task_stack),
        TaskPriority::Low, // 后台扫描，不阻塞交互
        10,                // size_pow2: 2^10 = 1024 字节栈区域
        TaskPrivilege::Kernel
    );

    return tcb != nullptr;
}

} // namespace hids
} // namespace aurora
