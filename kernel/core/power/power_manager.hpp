#ifndef AURORA_POWER_MANAGER_HPP
#define AURORA_POWER_MANAGER_HPP

#include <stdint.h>

// 引入板级配置与传感器框架的空声明（实际项目中应引入真实头文件）
#include "board.h"
#include "../../drivers/sensor/sensor_framework.hpp"
#include "../../drivers/display/st7789_driver.hpp"
#include "../../drivers/power/charging_manager.hpp"
#include "../../scheduler/frame_scheduler_v2.hpp"
#include "../../task/task.hpp"
#include "../../interrupt/timer.hpp"
#ifdef CONFIG_NETWORKING
#endif
#include "../../metrics/metrics.hpp"

// ========================================================
// 5 级电源状态定义
// ========================================================
enum class PowerState : uint8_t {
    ACTIVE,  // 亮屏 (100% 亮度)，30fps，传感器全开，功耗 ~15mA
    DIM,     // 暗屏 (30% 亮度)，15fps，传感器全开，功耗 ~8mA
    IDLE,    // 息屏，1fps，传感器低频采集，功耗 ~1mA
    SLEEP,   // 息屏，0fps (暂停调度)，仅保留 Accel 进行抬腕检测，功耗 ~0.1mA
    CRITICAL // 息屏，0fps，仅保留 RTC 时钟，功耗 ~0.05mA
};

// ========================================================
// ========================================================
// 抬腕唤醒检测器 (WristWakeDetector)
// ========================================================
class WristWakeDetector {
private:
    bool is_looking_at_watch_;
    uint32_t steady_ticks_;
    uint32_t steady_threshold_ms_;

public:
    WristWakeDetector() : is_looking_at_watch_(false), steady_ticks_(0), steady_threshold_ms_(1000) {}

    // 核心多轴算法：验证 Z 轴朝向以及 X/Y 水平倾角
    bool process_accel(int32_t x_mg, int32_t y_mg, int32_t z_mg, uint32_t delta_ticks) {
        // 模式识别: 手腕平放看表时，Z轴重力分量 800mg ~ 1200mg，且 X/Y 轴在合理倾角内 (|x| <= 700, |y| <= 700)
        int32_t abs_x = x_mg < 0 ? -x_mg : x_mg;
        int32_t abs_y = y_mg < 0 ? -y_mg : y_mg;

        if (z_mg > 800 && z_mg < 1200 && abs_x <= 700 && abs_y <= 700) {
            steady_ticks_ += delta_ticks;
            // 防抖过滤，防止手臂日常摆动误触发
            if (steady_ticks_ >= steady_threshold_ms_) {
                if (!is_looking_at_watch_) {
                    is_looking_at_watch_ = true;
                    return true; // 成功触发抬腕！
                }
            }
        } else {
            // 姿态破坏，状态重置
            steady_ticks_ = 0;
            is_looking_at_watch_ = false;
        }
        return false;
    }

    // 兼容单 Z 轴输入接口
    bool process_accel_z(int32_t z_mg, uint32_t delta_ticks) {
        return process_accel(0, 0, z_mg, delta_ticks);
    }

    // 落腕快速熄屏检测：手臂自然下垂（Z 轴重力极低，Y 轴或 X 轴承受主重力）
    bool is_wrist_dropped(int32_t x_mg, int32_t y_mg, int32_t z_mg) const {
        int32_t abs_x = x_mg < 0 ? -x_mg : x_mg;
        int32_t abs_y = y_mg < 0 ? -y_mg : y_mg;
        return (z_mg < 300) && (abs_y > 750 || abs_x > 750);
    }

    void set_steady_threshold(uint32_t ms) {
        steady_threshold_ms_ = ms;
    }

    uint32_t get_steady_threshold() const {
        return steady_threshold_ms_;
    }

    // 显式重置检测器状态
    void reset() {
        steady_ticks_ = 0;
        is_looking_at_watch_ = false;
    }
};

// ========================================================
// 电源管理器核心
// ========================================================
class PowerManager {
public:
    enum class Profile : uint8_t {
        PERFORMANCE = 0,
        BALANCED = 1,
        POWER_SAVE = 2,
        ULTRA_LOW_POWER = 3
    };

private:
    PowerState current_state_;
    Profile profile_;
    uint32_t state_ticks_; // 当前状态已维持的时间 (ms)
    WristWakeDetector wake_detector_;

    // 状态机超时降级阈值 (单位: ms，可动态配置)
    uint32_t timeout_active_to_dim_ = 5000;  // 默认5秒无交互变暗
    uint32_t timeout_dim_to_idle_ = 3000;    // 默认暗屏3秒后息屏
    uint32_t timeout_idle_to_sleep_ = 10000; // 默认息屏10秒后进入深度睡眠

    // Tickless 的极限安全边界参数
    static constexpr uint32_t TICKLESS_MIN_THRESHOLD = 5;
    static constexpr uint32_t TICKLESS_MAX_SLEEP = 0x00FFFFFF;

    PowerManager()
        : current_state_(PowerState::ACTIVE), profile_(Profile::BALANCED), state_ticks_(0),
          timeout_active_to_dim_(5000), timeout_dim_to_idle_(3000), timeout_idle_to_sleep_(10000) {}

    // 硬件降级与恢复路由机制
    void apply_state_hardware(PowerState state) {
        switch (state) {
        case PowerState::ACTIVE:
            St7789Driver::instance().exit_sleep();
            St7789Driver::instance().set_brightness(profile_ == Profile::POWER_SAVE ? 60 : 100);
            FrameSchedulerV2::instance().set_fps(profile_ == Profile::POWER_SAVE ? 15 : 30);
            break;
        case PowerState::DIM:
            St7789Driver::instance().exit_sleep();
            St7789Driver::instance().set_brightness(profile_ == Profile::POWER_SAVE ? 15 : 30);
            FrameSchedulerV2::instance().set_fps(profile_ == Profile::POWER_SAVE ? 10 : 15);
            break;
        case PowerState::IDLE:
            St7789Driver::instance().enter_sleep();
            FrameSchedulerV2::instance().set_fps(1);
            break;
        case PowerState::SLEEP:
        case PowerState::CRITICAL:
            // 暂停帧推进；CRITICAL 时还需关断除 RTC 外所有外设供电
            St7789Driver::instance().enter_sleep();
            FrameSchedulerV2::instance().set_fps(0);
            break;
        }
    }

public:
    static PowerManager& instance() {
        static PowerManager pm;
        return pm;
    }

    PowerState get_state() const {
        return current_state_;
    }

    Profile get_profile() const {
        return profile_;
    }

    void set_profile(Profile profile) {
        profile_ = profile;
        switch (profile_) {
        case Profile::PERFORMANCE:
            set_timeouts(10000, 5000, 15000);
            break;
        case Profile::BALANCED:
            set_timeouts(5000, 3000, 10000);
            break;
        case Profile::POWER_SAVE:
            set_timeouts(3000, 2000, 5000);
            break;
        case Profile::ULTRA_LOW_POWER:
            set_timeouts(2000, 1000, 3000);
            break;
        }
        apply_state_hardware(current_state_);
    }

    void set_timeouts(uint32_t active_to_dim_ms, uint32_t dim_to_idle_ms, uint32_t idle_to_sleep_ms) {
        timeout_active_to_dim_ = active_to_dim_ms;
        timeout_dim_to_idle_ = dim_to_idle_ms;
        timeout_idle_to_sleep_ = idle_to_sleep_ms;
    }

    uint32_t get_timeout_active_to_dim() const { return timeout_active_to_dim_; }
    uint32_t get_timeout_dim_to_idle() const { return timeout_dim_to_idle_; }
    uint32_t get_timeout_idle_to_sleep() const { return timeout_idle_to_sleep_; }

    // 用户交互事件（触摸屏、按键、外设唤醒）触发时刷新活跃状态
    void reset_idle_timer() {
        state_ticks_ = 0;
        if (current_state_ != PowerState::ACTIVE && current_state_ != PowerState::CRITICAL) {
            transition_to(PowerState::ACTIVE);
        }
    }

    void notify_user_activity() {
        reset_idle_timer();
    }

    WristWakeDetector& get_wrist_detector() {
        return wake_detector_;
    }

    // 强制状态转换 (供触控按键中断、手势引擎或外部通知调用)
    void transition_to(PowerState new_state) {
        if (current_state_ == new_state)
            return;

        // 离开 IDLE 或 SLEEP 时重置抬腕检测器，防止上一轮息屏期
        // 积累的 steady_ticks_ 残留到下一轮，导致虚假唤醒触发。
        if (current_state_ == PowerState::IDLE || current_state_ == PowerState::SLEEP) {
            wake_detector_.reset();
        }

        current_state_ = new_state;
        state_ticks_ = 0;
        apply_state_hardware(current_state_);
    }

    // 系统主心跳守护：处理超时降级与休眠期意图检测
    void on_tick(uint32_t delta_ticks) {
        state_ticks_ += delta_ticks;

        // 1. 状态机超时自动降级机制
        switch (current_state_) {
        case PowerState::ACTIVE:
            if (state_ticks_ >= timeout_active_to_dim_)
                transition_to(PowerState::DIM);
            break;
        case PowerState::DIM:
            if (state_ticks_ >= timeout_dim_to_idle_)
                transition_to(PowerState::IDLE);
            break;
        case PowerState::IDLE:
            if (state_ticks_ >= timeout_idle_to_sleep_)
                transition_to(PowerState::SLEEP);
            break;
        case PowerState::SLEEP:
        case PowerState::CRITICAL:
            break; // 最低功耗状态，由外部中断唤醒
        }

        // 2. 息屏深睡期的抬腕唤醒与落腕速息联动
        if (current_state_ == PowerState::IDLE || current_state_ == PowerState::SLEEP) {
            int32_t x_mg = 0, y_mg = 0, z_mg = 0;
            SensorData acc_data;
            if (SensorManager::instance().get_accel_sensor().read(&acc_data)) {
                x_mg = acc_data.payload.accel.x;
                y_mg = acc_data.payload.accel.y;
                z_mg = acc_data.payload.accel.z;
            }

            // 如果满足防抖抬腕模式识别，瞬间拉起系统到 Active
            if (wake_detector_.process_accel(x_mg, y_mg, z_mg, delta_ticks)) {
                transition_to(PowerState::ACTIVE);
            }
        } else if (current_state_ == PowerState::ACTIVE || current_state_ == PowerState::DIM) {
            // 落腕快速灭屏优化：在亮屏阶段若检测到手臂垂下，立即切入 DIM/IDLE
            int32_t x_mg = 0, y_mg = 0, z_mg = 0;
            SensorData acc_data;
            if (SensorManager::instance().get_accel_sensor().read(&acc_data)) {
                x_mg = acc_data.payload.accel.x;
                y_mg = acc_data.payload.accel.y;
                z_mg = acc_data.payload.accel.z;
                if (wake_detector_.is_wrist_dropped(x_mg, y_mg, z_mg)) {
                    // 若快速下垂，加速超时过渡
                    state_ticks_ += delta_ticks * 3;
                }
            }
        }

        // 3. 充电管理器级联轮询与低电量保护
        ChargingManager::instance().on_tick(delta_ticks);

        // 如果检测到 VBUS 刚刚插入，强制唤醒屏幕并转入活跃状态
        if (ChargingManager::instance().has_just_plugged()) {
            transition_to(PowerState::ACTIVE);
        }

        // 极低电量且未插电时，强制切断非必要外设，进入 CRITICAL 状态自保
        if (ChargingManager::instance().is_critical_low()) {
            if (current_state_ != PowerState::CRITICAL) {
                transition_to(PowerState::CRITICAL);
            }
        }
    }

    // 内核 Idle 线程的最后一道屏障，切断 CPU 供电
    void execute_wfi_if_needed() {
        if (current_state_ == PowerState::SLEEP || current_state_ == PowerState::CRITICAL) {
            uint32_t expected_task_ticks = Scheduler::instance().get_expected_idle_ticks();
            uint32_t expected_timer_ticks = TimerManager::instance().get_next_expire_ticks();

            uint32_t expected_idle_ticks =
                expected_task_ticks < expected_timer_ticks ? expected_task_ticks : expected_timer_ticks;

            // 加入帧调度器的自适应 VSync 动态测量剩余时间限制
            // expected_idle_ticks = min(task, timer, ble_interval, next_vsync)
            uint32_t fps = FrameSchedulerV2::instance().get_fps();
            if (fps > 0) {
                uint32_t next_vsync = FrameSchedulerV2::instance().get_ticks_to_next_vsync();
                if (next_vsync < expected_idle_ticks) {
                    expected_idle_ticks = next_vsync;
                }
            }

#ifdef CONFIG_NETWORKING
            uint32_t ble_interval = 100; // 临时默认间隔，后续接入实际 BLE 协议栈
            if (ble_interval < expected_idle_ticks) {
                expected_idle_ticks = ble_interval;
            }
#endif

            // 硬件寄存器防溢出保护
            if (expected_idle_ticks > TICKLESS_MAX_SLEEP) {
                expected_idle_ticks = TICKLESS_MAX_SLEEP;
            }

            bool is_ble_connected = false;

            // 如果睡眠时间太短，或者 BLE 处于高频连接态，直接普通 WFI
            if (expected_idle_ticks < TICKLESS_MIN_THRESHOLD || is_ble_connected) {
                Arch::wait_for_interrupt();
                return;
            }

            // 1. 关闭全局中断，防止在切换硬件时钟的临界区被强行打断
            Arch::disable_interrupts();

            // 2. 停跳！关闭 Cortex-M4F 的内核 SysTick
            Arch::disable_systick();

            // 3. 将预计睡眠时间转换为低功耗时钟源 (RTC/CTIMER) 的匹配值并启动
            Arch::start_wakeup_timer(expected_idle_ticks);

            // 4. 进入带状态保持的深度睡眠 (Deep Sleep)
            uint32_t sleep_enter = Arch::get_cycle();
            Arch::wait_for_interrupt();
            uint32_t slept = Arch::get_cycle() - sleep_enter;
            if (Metrics::is_active()) {
                Metrics::get_power_profiler().add_sleep_time(slept);
            }

            // ================= CPU 在此被硬件定时器或外部事件唤醒 =================

            // 5. 立即停止硬件唤醒定时器，并读取它【真实】跑过的周期数
            uint32_t actual_sleep_ticks = Arch::stop_wakeup_timer();

            // 6. 时间补偿：将睡觉期间错失的时间一次性补给系统
            Scheduler::instance().compensate_ticks(actual_sleep_ticks);
            TimerManager::instance().fast_forward_ticks(actual_sleep_ticks);

            // 7. 恢复高频 SysTick 心跳，继续常规调度
            Arch::enable_systick();

            // 8. 重新开启全局中断，系统继续运行
            Arch::enable_interrupts();
        }
    }
};

#endif // AURORA_POWER_MANAGER_HPP
