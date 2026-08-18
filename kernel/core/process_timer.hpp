#ifndef PROCESS_TIMER_HPP
#define PROCESS_TIMER_HPP

#include <stdint.h>
#include <stddef.h>
#include "../task/task.hpp"

namespace auroraos::kernel {

// 定时器触发标志位定义
namespace TimerFlags {
    constexpr uint32_t OneShot      = 0x00; // 单次定时器 (默认)
    constexpr uint32_t Periodic     = 0x01; // 周期性定时器
    constexpr uint32_t Relative     = 0x00; // 相对时间 (从当前 tick 起算)
    constexpr uint32_t Absolute     = 0x02; // 绝对时间 (基于系统全局 tick)
    constexpr uint32_t NotifySignal = 0x10; // 到期发送 POSIX 信号 (默认 14: SIGALRM)
    constexpr uint32_t NotifyIpc    = 0x20; // 到期向任务/端点传递 IPC 通知位
    constexpr uint32_t NotifyEvent  = 0x40; // 到期唤醒任务并置位 notify_value
}

// 用户态传入/传出的定时器描述符
struct ProcessTimerDesc {
    uint32_t flags;            // TimerFlags 组合
    uint32_t initial_delay_ms; // 首次到期时间 (相对延迟毫秒 或 绝对时间戳毫秒)
    uint32_t interval_ms;      // 周期时间 (毫秒，Periodic 模式有效)
    uint32_t notify_param;     // 通知参数: 信号号 (如 14/SIGALRM)、Endpoint 槽位号、或 Notify 掩码
};

struct ProcessTimer {
    bool allocated{false};
    bool active{false};
    uint32_t owner_task_id{0};
    uint32_t flags{0};
    uint32_t expire_tick{0};
    uint32_t period_ticks{0};
    uint32_t notify_param{0};
};

class ProcessTimerManager {
public:
    static constexpr size_t MAX_TIMERS = 16;

    static ProcessTimerManager& instance();

    void init();

    // 进程级定时器操作
    int create_timer(TaskControlBlock* owner, const ProcessTimerDesc* desc);
    int start_timer(TaskControlBlock* owner, uint32_t timer_id, const ProcessTimerDesc* desc);
    int stop_timer(TaskControlBlock* owner, uint32_t timer_id);
    int delete_timer(TaskControlBlock* owner, uint32_t timer_id);
    int get_time(TaskControlBlock* owner, uint32_t timer_id, uint32_t* out_remaining_ms);

    // 任务终止时的定时器资源回收
    void cleanup_task_timers(uint32_t task_id);

    // 时钟节拍钩子与 Tickless 协同
    void on_tick();
    uint32_t get_next_expire_ticks() const;
    void fast_forward_ticks(uint32_t skipped_ticks);

    const ProcessTimer* get_timer(size_t index) const {
        if (index < MAX_TIMERS) return &timers_[index];
        return nullptr;
    }

private:
    ProcessTimerManager();
    ProcessTimer timers_[MAX_TIMERS];
};

} // namespace auroraos::kernel

#endif // PROCESS_TIMER_HPP
