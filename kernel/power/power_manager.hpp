#ifndef AURORA_POWER_MANAGER_HPP
#define AURORA_POWER_MANAGER_HPP

#include <stdint.h>

// 引入板级配置与传感器框架的空声明（实际项目中应引入真实头文件）
#include "board.h"
#include "../../drivers/sensor/sensor_framework.hpp"
#include "../../drivers/display/st7789_driver.hpp"
#include "../../drivers/power/charging_manager.hpp"
#include "../frame_scheduler_v2.hpp"
#include "../task.hpp"
#include "../timer.hpp"
#ifdef CONFIG_NETWORKING
#include "../../net/ble/ble_stack.hpp"
#endif
#include "../../metrics/metrics.hpp"

// ========================================================
// 5 级电源状态定义
// ========================================================
enum class PowerState : uint8_t {
    ACTIVE,     // 亮屏 (100% 亮度)，30fps，传感器全开，功耗 ~15mA
    DIM,        // 暗屏 (30% 亮度)，15fps，传感器全开，功耗 ~8mA
    IDLE,       // 息屏，1fps，传感器低频采集，功耗 ~1mA
    SLEEP,      // 息屏，0fps (暂停调度)，仅保留 Accel 进行抬腕检测，功耗 ~0.1mA
    CRITICAL    // 息屏，0fps，仅保留 RTC 时钟，功耗 ~0.05mA
};

// ========================================================
// 抬腕唤醒检测器 (WristWakeDetector)
// ========================================================
class WristWakeDetector {
private:
    bool     is_looking_at_watch_;
    uint32_t steady_ticks_;

public:
    WristWakeDetector() : is_looking_at_watch_(false), steady_ticks_(0) {}

    // 核心算法：输入 Z 轴加速度值 (单位: mg，1g = 1000mg)
    bool process_accel_z(int32_t z_mg, uint32_t delta_ticks) {
        // 模式识别: 手腕平放看表时，Z轴重力分量从近 0 变为接近 +1g (假设阈值为 800mg ~ 1200mg)
        if (z_mg > 800 && z_mg < 1200) {
            steady_ticks_ += delta_ticks;
            // 1秒防抖过滤，防止手臂日常摆动误触发
            if (steady_ticks_ >= 1000) {
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

    // 显式重置检测器状态。
    // 必须在设备离开 IDLE/SLEEP 进入 ACTIVE/DIM 时调用，防止上一段休眠期
    // 积累的 steady_ticks_ 残留到下一轮休眠，导致虚假抬腕唤醒。
    void reset() {
        steady_ticks_        = 0;
        is_looking_at_watch_ = false;
    }
};

// ========================================================
// 电源管理器核心
// ========================================================
class PowerManager {
private:
    PowerState        current_state_;
    uint32_t          state_ticks_;      // 当前状态已维持的时间 (ms)
    WristWakeDetector wake_detector_;

    // 状态机超时降级阈值 (单位: ms)
    static constexpr uint32_t TIMEOUT_ACTIVE_TO_DIM = 5000;  // 5秒无交互变暗
    static constexpr uint32_t TIMEOUT_DIM_TO_IDLE   = 3000;  // 暗屏3秒后息屏
    static constexpr uint32_t TIMEOUT_IDLE_TO_SLEEP = 10000; // 息屏10秒后进入深度睡眠
    
    // Tickless 的极限安全边界参数
    static constexpr uint32_t TICKLESS_MIN_THRESHOLD = 5; 
    static constexpr uint32_t TICKLESS_MAX_SLEEP = 0x00FFFFFF; 

    PowerManager() : current_state_(PowerState::ACTIVE), state_ticks_(0) {}

    // 硬件降级与恢复路由机制
    void apply_state_hardware(PowerState state) {
        switch (state) {
            case PowerState::ACTIVE:
                // St7789Driver::instance().set_brightness(100);
                FrameSchedulerV2::instance().set_fps(30);
                break;
            case PowerState::DIM:
                // St7789Driver::instance().set_brightness(30);
                FrameSchedulerV2::instance().set_fps(15);
                break;
            case PowerState::IDLE:
                // St7789Driver::instance().enter_sleep();
                FrameSchedulerV2::instance().set_fps(1);
                break;
            case PowerState::SLEEP:
                FrameSchedulerV2::instance().set_fps(0); // 暂停帧推进
                break;
            case PowerState::CRITICAL:
                FrameSchedulerV2::instance().set_fps(0);
                // 关断除 RTC 外所有外设供电
                break;
        }
    }

public:
    static PowerManager& instance() {
        static PowerManager pm;
        return pm;
    }

    PowerState get_state() const { return current_state_; }

    // 强制状态转换 (供触控按键中断、手势引擎或外部通知调用)
    void transition_to(PowerState new_state) {
        if (current_state_ == new_state) return;

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
                if (state_ticks_ >= TIMEOUT_ACTIVE_TO_DIM) transition_to(PowerState::DIM);
                break;
            case PowerState::DIM:
                if (state_ticks_ >= TIMEOUT_DIM_TO_IDLE) transition_to(PowerState::IDLE);
                break;
            case PowerState::IDLE:
                if (state_ticks_ >= TIMEOUT_IDLE_TO_SLEEP) transition_to(PowerState::SLEEP);
                break;
            case PowerState::SLEEP:
            case PowerState::CRITICAL:
                break; // 最低功耗状态，由外部中断唤醒
        }

        // 2. 息屏深睡期的抬腕唤醒检测联动
        if (current_state_ == PowerState::IDLE || current_state_ == PowerState::SLEEP) {
            // 通过传感器框架非阻塞读取或在守护任务中读取。为了防止阻塞 on_tick，
            // 真实情况应该是传感器通过外部中断或 WorkQueue 提交给这里的唤醒逻辑。
            // 假设这里的 read 是立刻返回缓存值（非阻塞），我们保留它，
            // 但建议将 I2C 读取彻底移入后台任务。
            int32_t z_mg = 0;
            SensorData acc_data;
            if (SensorManager::instance().get_accel_sensor().read(&acc_data)) {
                z_mg = acc_data.payload.accel.z;
            }

            // 如果满足防抖抬腕模式识别，瞬间拉起系统到 Active
            if (wake_detector_.process_accel_z(z_mg, delta_ticks)) {
                transition_to(PowerState::ACTIVE);
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
            
            uint32_t expected_idle_ticks = expected_task_ticks < expected_timer_ticks ? expected_task_ticks : expected_timer_ticks;
            
            // 加入帧调度器的剩余时间限制
            uint32_t fps = FrameSchedulerV2::instance().get_fps();
            if (fps > 0) {
                uint32_t frame_ticks = FrameSchedulerV2::instance().get_ticks_to_next_frame();
                if (frame_ticks < expected_idle_ticks) {
                    expected_idle_ticks = frame_ticks;
                }
            }
            
            // 加入 BLE 广播/连接间隔
#ifdef CONFIG_NETWORKING
            uint32_t ble_interval = (BleManager::instance().get_state() == BleConnectionState::CONNECTED) ? 30 : 100;
            if (ble_interval < expected_idle_ticks) {
                expected_idle_ticks = ble_interval;
            }
#else
            uint32_t ble_interval = 100; // 无 BLE 时使用默认间隔
#endif

            // 硬件寄存器防溢出保护
            if (expected_idle_ticks > TICKLESS_MAX_SLEEP) {
                expected_idle_ticks = TICKLESS_MAX_SLEEP;
            }

#ifdef CONFIG_NETWORKING
            bool is_ble_connected = (BleManager::instance().get_state() == BleConnectionState::CONNECTED);
#else
            bool is_ble_connected = false;
#endif

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
