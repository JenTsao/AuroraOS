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
    uint32_t target_fps_;                  // 目标帧率配置 (30, 15, 1, 0)
    uint32_t frame_period_ticks_;          // 默认标称单帧周期 (1000 / target_fps)
    uint32_t current_frame_tick_;          // 软件定时器计时累加器

    // ── 动态自适应 VSync 测量体系 ──
    uint32_t last_vsync_tick_;             // 上一次硬件 VSync / 帧刷新完成的绝对 Tick
    uint32_t measured_vsync_period_ticks_; // 动态测量的真实 VSync 周期 (ms/ticks)
    uint32_t vsync_sample_count_;          // 采样次数
    uint32_t system_ticks_;                // 系统累计流逝的 Tick 计数
    volatile bool has_dynamic_vsync_;      // 是否接收到了显示驱动/硬件 TE 的真实 VSync 信号

    // 单核裸机：volatile 足够保证可见性，所有写入均在关中断保护下进行
    // （无需 std::atomic，newlib-nano 也不提供 <atomic>）
    volatile bool in_active_render_window_;
    uint32_t render_task_id_;              // 绑定的表盘 UI 主任务 TID

    inline void disable_interrupts() {
        Arch::disable_interrupts();
    }

    inline void enable_interrupts() {
        Arch::enable_interrupts();
    }

    FrameSchedulerV2()
        : target_fps_(30), frame_period_ticks_(33), current_frame_tick_(0),
          last_vsync_tick_(0), measured_vsync_period_ticks_(33), vsync_sample_count_(0),
          system_ticks_(0), has_dynamic_vsync_(false), in_active_render_window_(true),
          render_task_id_(0) {}

public:
    static FrameSchedulerV2& instance() {
        static FrameSchedulerV2 fs;
        return fs;
    }

    void init(uint32_t initial_fps, uint32_t render_task_id) {
        set_fps(initial_fps);
        render_task_id_ = render_task_id;
        current_frame_tick_ = 0;
        last_vsync_tick_ = 0;
        vsync_sample_count_ = 0;
        system_ticks_ = 0;
        has_dynamic_vsync_ = false;
        in_active_render_window_ = (initial_fps > 0);
    }

    // ========================================================
    // 动态调整帧率：由 PowerManager 在状态切换时实时调用
    // Active(30fps) -> Dim(15fps) -> Idle(1fps) -> Sleep(0fps)
    // ========================================================
    void set_fps(uint32_t fps) {
        disable_interrupts();
        target_fps_ = fps;
        if (fps > 0) {
            frame_period_ticks_ = 1000 / fps; // 标称帧周期
            if (measured_vsync_period_ticks_ == 0 || !has_dynamic_vsync_) {
                measured_vsync_period_ticks_ = frame_period_ticks_;
            }
            // Wake up render task if it was waiting
            TaskNotify::give(render_task_id_, 1, false);
        } else {
            // 0fps 状态：息屏深度睡眠，彻底关闭 UI 帧率推进机制
            frame_period_ticks_ = 0xFFFFFFFF;
            measured_vsync_period_ticks_ = 0xFFFFFFFF;
            in_active_render_window_ = false;
        }
        enable_interrupts();
    }

    uint32_t get_fps() const {
        return target_fps_;
    }

    // 获取当前动态测量的真实帧率
    uint32_t get_measured_fps() const {
        if (target_fps_ == 0 || measured_vsync_period_ticks_ == 0 || measured_vsync_period_ticks_ == 0xFFFFFFFF) {
            return 0;
        }
        return 1000 / measured_vsync_period_ticks_;
    }

    uint32_t get_measured_period_ticks() const {
        return measured_vsync_period_ticks_;
    }

    // ========================================================
    // 动态自适应 VSync 计算：获取距离下一次 VSync 的真实动态预测 Tick 数
    // 用于 Tickless WFI 计算：expected_idle_ticks = min(task, timer, ble, next_vsync)
    // ========================================================
    uint32_t get_ticks_to_next_vsync() const {
        if (target_fps_ == 0)
            return 0xFFFFFFFF; // 息屏睡眠期，无 VSync 唤醒事件

        if (has_dynamic_vsync_ && measured_vsync_period_ticks_ > 0 && measured_vsync_period_ticks_ != 0xFFFFFFFF) {
            uint32_t elapsed = (system_ticks_ >= last_vsync_tick_) ? (system_ticks_ - last_vsync_tick_) : 0;
            if (elapsed < measured_vsync_period_ticks_) {
                return measured_vsync_period_ticks_ - elapsed;
            }
            return 0; // VSync 已就绪或即将到达
        }

        // 回退到软件标称时钟测量
        if (frame_period_ticks_ <= current_frame_tick_)
            return 0;
        return frame_period_ticks_ - current_frame_tick_;
    }

    uint32_t get_ticks_to_next_frame() const {
        return get_ticks_to_next_vsync();
    }

    // ========================================================
    // 硬件显示驱动 VSync / TE (Tearing Effect) 脉冲回调
    // 由显示驱动或 TE 外部中断触发，记录真实硬件 VSync 时间戳并自适应平滑周期
    // ========================================================
    void on_hardware_vsync(uint32_t now_ticks = 0) {
        if (target_fps_ == 0)
            return;

        disable_interrupts();
        if (now_ticks == 0) {
            now_ticks = system_ticks_;
        } else if (now_ticks > system_ticks_) {
            system_ticks_ = now_ticks;
        }

        if (last_vsync_tick_ > 0 && now_ticks > last_vsync_tick_) {
            uint32_t delta = now_ticks - last_vsync_tick_;
            // 过滤异常毛刺 (有效范围 5ms ~ 2000ms)
            if (delta >= 5 && delta <= 2000) {
                // 指数移动平均平滑滤波 (首次直接收敛，后续 EMA: 75% 历史 + 25% 新采样)
                if (vsync_sample_count_ == 0 || measured_vsync_period_ticks_ == 0 || measured_vsync_period_ticks_ == 0xFFFFFFFF) {
                    measured_vsync_period_ticks_ = delta;
                } else {
                    measured_vsync_period_ticks_ = (measured_vsync_period_ticks_ * 3 + delta) / 4;
                }
                vsync_sample_count_++;
            }
        }

        last_vsync_tick_ = now_ticks;
        has_dynamic_vsync_ = true;
        current_frame_tick_ = 0;
        in_active_render_window_ = true;
        enable_interrupts();

        // 唤醒 UI 任务准时开始新一帧的渲染与脏区域计算
        TaskNotify::give(render_task_id_, 1);
    }

    // 兼容别名
    void on_vsync(uint32_t now_ticks = 0) {
        on_hardware_vsync(now_ticks);
    }

    // 接入硬件 SysTick 心跳
    void on_tick(uint32_t delta_ticks) {
        system_ticks_ += delta_ticks;

        if (target_fps_ == 0)
            return; // 息屏睡眠期，冻结图形管线时间轴

        // 如果没有硬件 VSync 脉冲持续驱动，通过系统定时器回退推进
        current_frame_tick_ += delta_ticks;
        uint32_t period = (has_dynamic_vsync_ && measured_vsync_period_ticks_ > 0 && measured_vsync_period_ticks_ != 0xFFFFFFFF)
                              ? measured_vsync_period_ticks_
                              : frame_period_ticks_;

        if (current_frame_tick_ >= period) {
            current_frame_tick_ %= period;
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
        // 0. 如果未绑定任何渲染任务，直接放行，禁用帧感知调度限制
        if (render_task_id_ == 0)
            return true;

        // 1. 息屏深度睡眠保护：仅放行传感器采集和蓝牙通信 (HIGH 级及以上)
        if (target_fps_ == 0) {
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
        return tcb ? tcb->scheduler.id : 0;
    }
};

#endif // AURORA_FRAME_SCHEDULER_V2_HPP
