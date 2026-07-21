#ifndef AURORA_FRAME_SCHEDULER_V2_HPP
#define AURORA_FRAME_SCHEDULER_V2_HPP

#include <stdint.h>
#include "task.hpp"
#include "task_notify.hpp"
#include "arch_api.hpp"

// ========================================================
// 我们现已统一使用 kernel/task.hpp 中的 TaskPriority。
// 映射关系：
// CRITICAL -> Realtime (最高优，UI 渲染、触控与手势识别)
// HIGH     -> High     (传感器后台采样、BLE 协议栈心跳)
// NORMAL   -> Normal   (运动算法复杂运算、文件系统日志落盘)
// LOW      -> Low      (极低优内存清理与垃圾回收)
// ========================================================

class FrameSchedulerV2 {
private:
    uint32_t current_fps_;
    uint32_t frame_period_ticks_;     // 单帧对应的系统 Tick 数
    uint32_t current_frame_tick_;     // 当前帧内已流逝的 Tick 数
    
    // 单核裸机：volatile 足够保证可见性，所有写入均在关中断保护下进行
    // （无需 std::atomic，newlib-nano 也不提供 <atomic>）
    volatile bool in_active_render_window_;
    uint32_t render_task_id_;         // 绑定的表盘 UI 主任务 TID

    inline void disable_interrupts() { Arch::disable_interrupts(); }
    inline void enable_interrupts()  { Arch::enable_interrupts(); }

    FrameSchedulerV2() : current_fps_(30), frame_period_ticks_(33), current_frame_tick_(0), 
                         in_active_render_window_(true), render_task_id_(0) {}

public:
    static FrameSchedulerV2& instance() {
        static FrameSchedulerV2 fs;
        return fs;
    }

    void init(uint32_t initial_fps, uint32_t render_task_id) {
        set_fps(initial_fps);
        render_task_id_ = render_task_id;
        current_frame_tick_ = 0;
        in_active_render_window_ = true;
    }

    // ========================================================
    // 动态调整帧率：由 PowerManager 在状态切换时实时调用
    // Active(30fps) -> Dim(15fps) -> Idle(1fps) -> Sleep(0fps)
    // ========================================================
    void set_fps(uint32_t fps) {
        disable_interrupts();
        current_fps_ = fps;
        if (fps > 0) {
            frame_period_ticks_ = 1000 / fps; // 动态重算帧时间窗
            // Wake up render task if it was waiting
            TaskNotify::give(render_task_id_, 1, false);
        } else {
            // 0fps 状态：彻底关闭 UI 帧率推进机制
            frame_period_ticks_ = 0xFFFFFFFF; 
            in_active_render_window_ = false;
        }
        enable_interrupts();
    }

    uint32_t get_fps() const { return current_fps_; }

    uint32_t get_ticks_to_next_frame() const {
        if (current_fps_ == 0 || frame_period_ticks_ <= current_frame_tick_) return 0;
        return frame_period_ticks_ - current_frame_tick_;
    }

    // 接入硬件 SysTick 心跳
    void on_tick(uint32_t delta_ticks) {
        if (current_fps_ == 0) return; // 息屏睡眠期，冻结图形管线时间轴

        current_frame_tick_ += delta_ticks;

        if (current_frame_tick_ >= frame_period_ticks_) {
            current_frame_tick_ = 0;
            in_active_render_window_ = true;
            
            // 唤醒 UI 任务开始新一帧的脏区域计算
            TaskNotify::give(render_task_id_, 1);
        }
    }

    void notify_render_complete() {
        disable_interrupts();
        in_active_render_window_ = false;
        enable_interrupts();
        Scheduler::instance().schedule();
    }

    void wait_for_next_frame() {
        notify_render_complete();
        TaskNotify::take(true);
    }

    // ========================================================
    // 核心拦截钩子：深度植入内核 Schedule() 轮询环节
    // ========================================================
    bool is_task_allowed(uint8_t task_priority) const {
        // 1. 息屏深度睡眠保护：仅放行传感器采集和蓝牙通信 (HIGH 级及以上)
        if (current_fps_ == 0) {
            if (task_priority < static_cast<uint8_t>(TaskPriority::High)) {
                return false; 
            }
        }

        // 2. 亮屏渲染特权保护：帧内绝对优先绘制，拒绝低优先级的数学与日志运算干扰
        if (in_active_render_window_) {
            if (task_priority < static_cast<uint8_t>(TaskPriority::High)) {
                return false;
            }
        }

        return true;
    }

    uint32_t create_frame_task(void (*entry)(void), uint32_t* stack, uint32_t stack_size, TaskPriority prio) {
        TaskControlBlock* tcb = Scheduler::instance().create_task(entry, stack, stack_size, prio);
        return tcb ? tcb->id : 0;
    }
};

#endif // AURORA_FRAME_SCHEDULER_V2_HPP
