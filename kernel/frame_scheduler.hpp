#ifndef AURORA_FRAME_SCHEDULER_HPP
#define AURORA_FRAME_SCHEDULER_HPP

#include <stdint.h>
#include "task.hpp"
#include "task_notify.hpp"
#include "frame_scheduler_v2.hpp"

// FramePriority is now defined in frame_scheduler_v2.hpp

class [[deprecated("Use FrameSchedulerV2 instead")]] FrameScheduler {
private:
    uint32_t target_fps_;
    uint32_t frame_period_ticks_;     // 单帧周期（30fps = 33个 1ms 嘀嗒）
    uint32_t current_frame_tick_;     // 当前帧内已消耗的时间 [0 ~ 33]
    uint32_t total_frames_rendered_;  // 系统累计渲染帧数
    
    bool     in_active_render_window_; // 当前是否处于“帧内 (Intra-Frame)”高优渲染窗口
    uint32_t render_task_id_;         // 注册的 UI 渲染主任务 ID

    FrameScheduler() : target_fps_(30), frame_period_ticks_(33), current_frame_tick_(0), 
                       total_frames_rendered_(0), in_active_render_window_(true), render_task_id_(0) {}

public:
    static FrameScheduler& instance() {
        static FrameScheduler fs;
        return fs;
    }

    // 1. 初始化帧率参数 (默认：手表配置 30FPS = 33ms 窗口)
    void init(uint32_t fps = 30, uint32_t render_task_id = 0) {
        target_fps_ = fps;
        frame_period_ticks_ = 1000 / fps; // 假定系统底层的 SysTick 为 1ms
        current_frame_tick_ = 0;
        total_frames_rendered_ = 0;
        render_task_id_ = render_task_id;
        in_active_render_window_ = true;
    }

    // 2. 核心时钟驱动钩子 (必须由 SysTick_Handler 每次心跳调用)
    void on_tick() {
        // 【修复 BUG #4】使用临界区保护共享变量，避免在 SysTick ISR 外部
        // 读取 total_frames_rendered_ 或 in_active_render_window_ 时数据竞争。
        IrqGuard guard;
        current_frame_tick_++;

        // ========================================================
        // 触发新的 V-Sync 帧周期边界 (每 33ms 触发一次)
        // ========================================================
        if (current_frame_tick_ >= frame_period_ticks_) {
            current_frame_tick_ = 0;
            total_frames_rendered_++;
            
            // 新一帧开始！重新拉起高优窗口，压制所有后台任务
            in_active_render_window_ = true;

            // 零开销发通知唤醒等待中的 UI 渲染任务，准时开工！
            TaskNotify::give(render_task_id_, 1);
        }
    }

    // 3. 供 UI 渲染引擎调用：上报本帧绘制完成 (Render Complete)
    void notify_render_complete() {
        Arch::disable_interrupts();
        // 关键动作：关闭帧内渲染窗口，正式切入“帧间 (Inter-Frame)”空闲期
        in_active_render_window_ = false;
        Arch::enable_interrupts();

        // 立即触发调度，将剩余的 CPU 时间片释放给 NORMAL 和 LOW 的后台任务
        Scheduler::instance().schedule();
    }

    // 4. 供 UI 渲染引擎调用：绘制完后挂起休眠，等待下一次 V-Sync 信号
    void wait_for_next_frame() {
        notify_render_complete();
        // 利用上面写好的零开销任务通知，阻塞直到下个 33ms 边界到位
        TaskNotify::take(true);
    }

    // 5. 任务创建辅助接口：自动将任务归类到蓝河优先级
    uint32_t create_frame_task(void (*entry)(void), uint32_t* stack, uint32_t stack_size, TaskPriority prio) {
        Scheduler::instance().create_task(entry, stack, stack_size, prio);
        return Scheduler::instance().get_task_count() - 1; // 返回当前注册的 TID
    }

    // 6. 调度器过滤钩子：在 schedule() 挑选下一个任务时应用“预算约束”
    bool is_task_allowed(uint8_t task_priority) const {
        // 如果当前处于“帧内 (Intra-Frame)”渲染期，且任务优先级低于 CRITICAL/HIGH，无情屏蔽！
        if (in_active_render_window_) {
            if (task_priority < static_cast<uint8_t>(TaskPriority::High)) {
                return false; // 非关键任务不得抢占 UI 渲染算力
            }
        }
        return true;
    }

    uint32_t get_fps() const { return target_fps_; }
    uint32_t get_current_frame_tick() const { return current_frame_tick_; }
    bool is_in_render_window() const { return in_active_render_window_; }
};

#endif
