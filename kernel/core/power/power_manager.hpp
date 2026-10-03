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
#include "wake_lock_registry.hpp"
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
// 唤醒原因 (供 /proc/power 与功耗归因分析使用)
// ========================================================
enum class WakeReason : uint8_t {
    NONE = 0,        // 尚未发生过唤醒
    BOOT,            // 上电初始化
    USER_ACTIVITY,   // 触摸 / 按键 / 手势
    WRIST_RAISE,     // 抬腕检测命中
    CHARGER_PLUG,    // VBUS 插入
    BATTERY_RECOVER, // 电量从 CRITICAL 阈值以上恢复
    EXTERNAL         // 外部子系统显式拉起 (通知、OTA 等)
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

    // Tickless 睡眠上限的外部约束提供者。
    // 返回值语义：距离下一个必须醒来的时刻还有多少 tick (ms)；
    //   0            -> 禁止进入 tickless 深睡，仅执行普通 WFI；
    //   0xFFFFFFFF   -> 该来源当前无唤醒约束。
    // 所有权：单一注册者（板级/应用的电源策略层）。内核不依赖任何具体
    // 子系统（BLE、OTA 等），由注册方在自己的回调里聚合多路输入。
    using WakeDeadlineProvider = uint32_t (*)();

    // 各电源状态下的传感器功耗档位 (Hz)。
    // ACTIVE/DIM 全速采样；IDLE 保留低频加速度计用于抬腕；
    // SLEEP 进一步降频；CRITICAL 只保留 RTC，加速度计断电。
    static constexpr uint16_t ACCEL_HZ_ACTIVE = 25;
    static constexpr uint16_t ACCEL_HZ_IDLE = 10;
    static constexpr uint16_t ACCEL_HZ_SLEEP = 10;

private:
    PowerState current_state_;
    Profile profile_;             // 生效中的 Profile（可能被低电量/热保护强制降档）
    Profile user_profile_;        // 用户/上层显式选择的 Profile，条件恢复后据此回退
    uint32_t state_ticks_;        // 当前状态已维持的时间 (ms)，超时判定用（可被加速）
    uint32_t state_elapsed_ms_;   // 当前状态真实流逝时间 (ms)，仅用于功耗归因
    WakeReason last_wake_reason_; // 最近一次显式唤醒的原因（超时降档不覆盖）
    bool thermal_throttled_;      // 热保护是否正在限制状态上限
    WakeDeadlineProvider wake_deadline_provider_;
    WristWakeDetector wake_detector_;
    uint32_t deep_sleep_count_ = 0; // 进入过 tickless 深睡的次数（功耗账本）

    // 状态机超时降级阈值 (单位: ms，可动态配置)
    uint32_t timeout_active_to_dim_ = 5000;  // 默认5秒无交互变暗
    uint32_t timeout_dim_to_idle_ = 3000;    // 默认暗屏3秒后息屏
    uint32_t timeout_idle_to_sleep_ = 10000; // 默认息屏10秒后进入深度睡眠

    // Tickless 的极限安全边界参数
    static constexpr uint32_t TICKLESS_MIN_THRESHOLD = 5;
    static constexpr uint32_t TICKLESS_MAX_SLEEP = 0x00FFFFFF;

    PowerManager()
        : current_state_(PowerState::ACTIVE), profile_(Profile::BALANCED), user_profile_(Profile::BALANCED),
          state_ticks_(0), state_elapsed_ms_(0), last_wake_reason_(WakeReason::BOOT), thermal_throttled_(false),
          wake_deadline_provider_(nullptr), timeout_active_to_dim_(5000), timeout_dim_to_idle_(3000),
          timeout_idle_to_sleep_(10000) {}

    // 低电量自动降档策略阈值 (带 5% 滞回，防止在边界反复切换 Profile)
    static constexpr uint8_t LOW_BATTERY_ENGAGE_SOC = 20;
    static constexpr uint8_t LOW_BATTERY_RELEASE_SOC = 25;

    // 热保护滞回：ChargingManager 在 50°C 报 OVERHEAT，此处回落到 40°C 才解除限流
    static constexpr int16_t THERMAL_RELEASE_C = 40;

    bool low_battery_throttled_ = false;

    // 当前 Profile 允许的最高电源状态（热保护限流时压制到 DIM，避免满亮度加剧发热）
    PowerState clamp_state(PowerState state) const {
        if (thermal_throttled_ && state == PowerState::ACTIVE) {
            return PowerState::DIM;
        }
        return state;
    }

    // 传感器供电轨路由：PPG (LED) 与加速度计是息屏期最主要的功耗来源。
    // 分档策略严格对应 roadmap 的功耗模型：
    //   ACTIVE/DIM -> 全速采样 (accel 25Hz + PPG)
    //   IDLE       -> 低频采集 (accel 10Hz + 背景 PPG)，屏幕已熄但仍可穿戴监测
    //   SLEEP      -> 仅保留 accel 做抬腕检测，切断 PPG LED
    //   CRITICAL   -> 仅保留 RTC，accel 与 PPG 全部断电（主动放弃抬腕唤醒）
    void apply_sensor_rails(PowerState state) {
        SensorManager& sensors = SensorManager::instance();
        switch (state) {
        case PowerState::ACTIVE:
        case PowerState::DIM:
            sensors.get_hr_sensor().power_up();
            sensors.get_accel_sensor().power_up();
            sensors.get_accel_sensor().set_sample_rate(ACCEL_HZ_ACTIVE);
            break;
        case PowerState::IDLE:
            sensors.get_hr_sensor().power_up();
            sensors.get_accel_sensor().power_up();
            sensors.get_accel_sensor().set_sample_rate(ACCEL_HZ_IDLE);
            break;
        case PowerState::SLEEP:
            sensors.get_hr_sensor().power_down();
            sensors.get_accel_sensor().power_up();
            sensors.get_accel_sensor().set_sample_rate(ACCEL_HZ_SLEEP);
            break;
        case PowerState::CRITICAL:
            sensors.get_hr_sensor().power_down();
            sensors.get_accel_sensor().power_down();
            break;
        }
    }

    // 状态驻留时长归因：切换前把上一个状态的真实停留时间记入功耗分析器，
    // 否则 /proc/power 的平均电流与 sleep_ratio 永远为 0。
    void record_state_duration(PowerState state, uint32_t duration_ms) {
        Metrics::get_power_profiler().record_state_duration(static_cast<uint8_t>(state), duration_ms);
    }

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

        apply_sensor_rails(state);
    }

    // 依据电量与热状态推导生效 Profile（用户选择的 Profile 始终保留，
    // 条件恢复后自动回退，不覆盖用户意图）
    Profile derive_effective_profile(Profile user_profile) const {
        Profile profile = user_profile;
        if (low_battery_throttled_ && profile < Profile::POWER_SAVE) {
            profile = Profile::POWER_SAVE;
        }
        if (thermal_throttled_ && profile < Profile::ULTRA_LOW_POWER) {
            profile = Profile::ULTRA_LOW_POWER;
        }
        return profile;
    }

    void apply_profile(Profile profile) {
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

    Profile get_user_profile() const {
        return user_profile_;
    }

    // 用户选择的 Profile；若正处于低电量/热保护降档期，实际生效档位可能更低
    void set_profile(Profile profile) {
        user_profile_ = profile;
        apply_profile(derive_effective_profile(user_profile_));
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

    WakeReason get_last_wake_reason() const {
        return last_wake_reason_;
    }

    bool is_thermal_throttled() const {
        return thermal_throttled_;
    }

    bool is_low_battery_throttled() const {
        return low_battery_throttled_;
    }

    uint32_t get_state_elapsed_ms() const {
        return state_elapsed_ms_;
    }

    // 注册 tickless 睡眠上限提供者（见 WakeDeadlineProvider 语义）
    void set_wake_deadline_provider(WakeDeadlineProvider provider) {
        wake_deadline_provider_ = provider;
    }

    // 用户交互事件（触摸屏、按键、外设唤醒）触发时刷新活跃状态
    void reset_idle_timer(WakeReason reason = WakeReason::USER_ACTIVITY) {
        state_ticks_ = 0;
        if (current_state_ != PowerState::ACTIVE && current_state_ != PowerState::CRITICAL) {
            transition_to(PowerState::ACTIVE, reason);
        }
    }

    void notify_user_activity() {
        reset_idle_timer(WakeReason::USER_ACTIVITY);
    }

    // 通知类唤醒（消息推送、闹钟等）：拉起屏幕但不重置低电/热保护策略
    void notify_external_wakeup() {
        reset_idle_timer(WakeReason::EXTERNAL);
    }

    WristWakeDetector& get_wrist_detector() {
        return wake_detector_;
    }

    // 强制状态转换 (供触控按键中断、手势引擎或外部通知调用)
    // reason 默认 NONE：纯超时降档不算“唤醒”，不应覆盖上一次真实唤醒原因；
    // 只有调用方显式给出唤醒原因（抬腕/插电/电量恢复/用户交互/外部拉起）时才记录。
    void transition_to(PowerState new_state, WakeReason reason = WakeReason::NONE) {
        new_state = clamp_state(new_state);
        if (current_state_ == new_state) {
            return;
        }

        // 离开 IDLE 或 SLEEP 时重置抬腕检测器，防止上一轮息屏期
        // 积累的 steady_ticks_ 残留到下一轮，导致虚假唤醒触发。
        if (current_state_ == PowerState::IDLE || current_state_ == PowerState::SLEEP) {
            wake_detector_.reset();
        }

        // 把上一个状态的真实驻留时间归因到功耗分析器
        record_state_duration(current_state_, state_elapsed_ms_);

        if (reason != WakeReason::NONE) {
            last_wake_reason_ = reason;
        }

        current_state_ = new_state;
        state_ticks_ = 0;
        state_elapsed_ms_ = 0;
        apply_state_hardware(current_state_);
    }

    // 系统主心跳守护：处理超时降级与休眠期意图检测
    void on_tick(uint32_t delta_ticks) {
        state_ticks_ += delta_ticks;
        state_elapsed_ms_ += delta_ticks;

        // 0. 回收超时未释放的 WakeLock：泄漏的锁会让设备永久无法省电
        WakeLockRegistry::instance().on_tick(delta_ticks);

        // WakeLock 语义（与 Android 对齐）：
        //   SCREEN  锁 -> 屏幕必须保持点亮，禁止一切自动降档；
        //   PARTIAL 锁 -> 允许变暗/息屏，但禁止进入 tickless 深睡与 CPU 断电。
        const bool keep_screen_on = WakeLockRegistry::instance().holds_screen();
        const bool keep_cpu_awake = WakeLockRegistry::instance().holds_any();

        // 1. 状态机超时自动降级机制
        switch (current_state_) {
        case PowerState::ACTIVE:
            if (state_ticks_ >= timeout_active_to_dim_ && !keep_screen_on) {
                transition_to(PowerState::DIM);
            }
            break;
        case PowerState::DIM:
            if (state_ticks_ >= timeout_dim_to_idle_ && !keep_screen_on) {
                transition_to(PowerState::IDLE);
            }
            break;
        case PowerState::IDLE:
            if (state_ticks_ >= timeout_idle_to_sleep_ && !keep_cpu_awake) {
                transition_to(PowerState::SLEEP);
            }
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
                transition_to(PowerState::ACTIVE, WakeReason::WRIST_RAISE);
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
        ChargingManager& charging = ChargingManager::instance();
        charging.on_tick(delta_ticks);

        // 4. 热保护：过温时压制状态上限并强制最低功耗档位，降温后自动解除（带滞回）
        update_thermal_policy(charging);

        // 5. 低电量自动降档：20% 以下强制省电档，回到 25% 以上解除
        update_low_battery_policy(charging);

        // 如果检测到 VBUS 刚刚插入，强制唤醒屏幕并转入活跃状态
        if (charging.has_just_plugged()) {
            transition_to(PowerState::ACTIVE, WakeReason::CHARGER_PLUG);
        }

        // 极低电量且未插电时，强制切断非必要外设，进入 CRITICAL 状态自保
        if (charging.is_critical_low()) {
            if (current_state_ != PowerState::CRITICAL) {
                transition_to(PowerState::CRITICAL);
            }
        } else if (current_state_ == PowerState::CRITICAL) {
            // 电量恢复到 CRITICAL 阈值之上（已接充电器或已换电）：解除自保。
            // 充电中点亮屏幕提示充电状态，纯换电则回到息屏待机等待交互。
            transition_to(charging.is_plugged() ? PowerState::ACTIVE : PowerState::IDLE, WakeReason::BATTERY_RECOVER);
        }
    }

    // 热保护：电池温度超过 ChargingManager 的 OVERHEAT 阈值即压制功耗；
    // 回落到 THERMAL_RELEASE_C 以下才解除，避免在阈值附近抖动切换档位。
    void update_thermal_policy(const ChargingManager& charging) {
        const bool overheating = (charging.get_battery_health() == BatteryHealth::OVERHEAT);
        if (overheating) {
            thermal_throttled_ = true;
        } else if (thermal_throttled_ && charging.get_temperature_c() <= THERMAL_RELEASE_C) {
            thermal_throttled_ = false;
        }

        if (thermal_throttled_) {
            // 限流期间强制最低功耗档位，并立刻把已经亮着的屏幕压到 DIM
            const Profile throttled = derive_effective_profile(user_profile_);
            if (throttled != profile_) {
                apply_profile(throttled);
            }
            if (current_state_ == PowerState::ACTIVE) {
                transition_to(PowerState::DIM);
            }
        } else {
            const Profile restored = derive_effective_profile(user_profile_);
            if (restored != profile_) {
                apply_profile(restored);
                apply_state_hardware(current_state_);
            }
        }
    }

    // 低电量降档：SOC 低于 20% 时强制省电档以延长续航，
    // 回到 25% 以上（或接上充电器）后恢复用户选择的档位。
    void update_low_battery_policy(const ChargingManager& charging) {
        const uint8_t soc = charging.get_soc();
        if (charging.is_plugged()) {
            low_battery_throttled_ = false;
        } else if (soc < LOW_BATTERY_ENGAGE_SOC) {
            low_battery_throttled_ = true;
        } else if (soc >= LOW_BATTERY_RELEASE_SOC) {
            low_battery_throttled_ = false;
        }

        if (!thermal_throttled_) {
            const Profile effective = derive_effective_profile(user_profile_);
            if (effective != profile_) {
                apply_profile(effective);
                apply_state_hardware(current_state_);
            }
        }
    }

    // 内核 Idle 线程的最后一道屏障，切断 CPU 供电
    // 返回本次实际睡眠的 tick 数（走普通 WFI 快路径时返回 0），
    // 便于上层 idle 线程按需驱动补偿逻辑或统计。
    uint32_t execute_wfi_if_needed() {
        if (current_state_ != PowerState::SLEEP && current_state_ != PowerState::CRITICAL) {
            return 0;
        }

        // PARTIAL WakeLock 要求 CPU 保持可调度（不得停跳 SysTick），
        // 否则事务会在深睡期间失去时间推进与调度权。此处退化为普通 WFI：
        // 仍然省电，但任意中断都能立即恢复调度。
        if (WakeLockRegistry::instance().holds_any()) {
            Arch::wait_for_interrupt();
            return 0;
        }

        uint32_t expected_task_ticks = Scheduler::instance().get_expected_idle_ticks();
        uint32_t expected_timer_ticks = TimerManager::instance().get_next_expire_ticks();

        uint32_t expected_idle_ticks =
            expected_task_ticks < expected_timer_ticks ? expected_task_ticks : expected_timer_ticks;

        // 加入帧调度器的自适应 VSync 动态测量剩余时间限制
        // expected_idle_ticks = min(task, timer, provider, next_vsync)
        uint32_t fps = FrameSchedulerV2::instance().get_fps();
        if (fps > 0) {
            uint32_t next_vsync = FrameSchedulerV2::instance().get_ticks_to_next_vsync();
            if (next_vsync < expected_idle_ticks) {
                expected_idle_ticks = next_vsync;
            }
        }

        // 外部唤醒约束（BLE 连接事件、OTA、闹钟等）。
        // 提供者返回 0 表示"马上有事要做"，此时只做普通 WFI，不停跳 SysTick。
        if (wake_deadline_provider_ != nullptr) {
            uint32_t deadline = wake_deadline_provider_();
            if (deadline < expected_idle_ticks) {
                expected_idle_ticks = deadline;
            }
        }

        // 硬件寄存器防溢出保护
        if (expected_idle_ticks > TICKLESS_MAX_SLEEP) {
            expected_idle_ticks = TICKLESS_MAX_SLEEP;
        }

        // 如果睡眠时间太短，切换时钟源的开销大于收益，直接普通 WFI
        if (expected_idle_ticks < TICKLESS_MIN_THRESHOLD) {
            Arch::wait_for_interrupt();
            return 0;
        }

        // 1. 关闭全局中断，防止在切换硬件时钟的临界区被强行打断。
        // （保存/恢复式：此路径可能被外层 IrqGuard 包裹，无条件重开中断
        //   会破坏外层临界区 —— 采 main 分支的 irq_save/irq_restore 修复）
        uint32_t saved_flags = Arch::irq_save();

        // 2. 停跳！关闭 Cortex-M4F 的内核 SysTick
        Arch::disable_systick();

        // 3. 将预计睡眠时间转换为低功耗时钟源 (RTC/CTIMER) 的匹配值并启动
        Arch::start_wakeup_timer(expected_idle_ticks);

        // 4. 进入带状态保持的深度睡眠 (Deep Sleep)
        uint32_t sleep_enter = Arch::get_cycle();
        Arch::wait_for_interrupt();
        uint32_t slept = Arch::get_cycle() - sleep_enter;

        // ================= CPU 在此被硬件定时器或外部事件唤醒 =================

        // 5. 立即停止硬件唤醒定时器，并读取它【真实】跑过的周期数
        uint32_t actual_sleep_ticks = Arch::stop_wakeup_timer();

        // 6. 时间补偿：将睡觉期间错失的时间一次性补给系统
        Scheduler::instance().compensate_ticks(actual_sleep_ticks);
        TimerManager::instance().fast_forward_ticks(actual_sleep_ticks);

        // 7. 恢复高频 SysTick 心跳，继续常规调度
        Arch::enable_systick();

        // 8. 恢复进入临界区前的中断状态，系统继续运行
        Arch::irq_restore(saved_flags);

        // 睡眠时间是功耗账本的主数据，不随 Metrics 采样开关丢弃；
        // 只有 cycle 级的活跃时间统计才受 Metrics::is_active() 控制。
        Metrics::get_power_profiler().add_sleep_time(slept);
        deep_sleep_count_++;

        return actual_sleep_ticks;
    }

    uint32_t get_deep_sleep_count() const {
        return deep_sleep_count_;
    }
};

#endif // AURORA_POWER_MANAGER_HPP
