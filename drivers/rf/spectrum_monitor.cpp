// =============================================================================
// drivers/rf/spectrum_monitor.cpp
//
// 射频频谱感知守护任务
//
// 提供一个低优先级后台任务，周期性地对绑定的 ISpectrumSensor 执行扫频，
// 并将结果单线程推进 SpectrumMonitor（RfAnalyzer + JammingDetector）。
// 任务以 TaskPriority::Low 运行，不阻塞 Shell/Normal 交互。
//
// 接入真实射频前端时：
//   1. 实现 ISpectrumSensor（SDR / 频谱芯片 / 收发器 RSSI 通道）
//   2. 调用 SpectrumMonitor::instance().init(&my_sensor) 绑定
//   3. 调用 create_spectrum_monitor_task() 启动守护任务
// =============================================================================

#include "spectrum_monitor.hpp"
#include "../../kernel/task/task.hpp"

namespace aurora {
namespace rf {

namespace {

// 后台任务栈（1 KB，4 字节对齐）
alignas(8) static uint32_t g_spectrum_task_stack[256];

// 两次扫频之间的间隔（ms）
constexpr uint32_t kSweepIntervalMs = 100;

} // namespace

// ---- 频谱守护任务入口 ----
void spectrum_monitor_task_entry() {
    SpectrumMonitor& mon = SpectrumMonitor::instance();

    // 确保已绑定传感器（默认 NullSpectrumSensor）
    if (!mon.get_sensor()) {
        mon.init();
    }
    ISpectrumSensor* sensor = mon.get_sensor();
    if (!sensor) {
        // 防御：无传感器则空闲，避免空指针解引用
        while (true) {
            Scheduler::instance().sleep_ms(1000);
        }
    }

    sensor->power_up();

    // ---- 主循环：扫频 -> 分析 -> 干扰识别 -> 联动告警 ----
    while (true) {
        SpectrumSweep sweep;
        if (sensor->sweep(&sweep)) {
            mon.process_sweep(sweep);
        }
        Scheduler::instance().sleep_ms(kSweepIntervalMs);
    }
}

// ---- 创建频谱守护任务 ----
bool create_spectrum_monitor_task() {
    SpectrumMonitor& mon = SpectrumMonitor::instance();
    if (!mon.get_sensor()) {
        mon.init();
    }

    TaskControlBlock* tcb = Scheduler::instance().create_task(
        spectrum_monitor_task_entry,
        g_spectrum_task_stack,
        sizeof(g_spectrum_task_stack),
        TaskPriority::Low, // 后台扫描，不阻塞交互
        10,                // size_pow2: 2^10 = 1024 字节栈区域
        TaskPrivilege::Kernel
    );

    return tcb != nullptr;
}

} // namespace rf
} // namespace aurora
