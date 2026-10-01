#ifndef AURORAOS_POWER_MANAGER_HPP
#define AURORAOS_POWER_MANAGER_HPP

#include "power_state.hpp"
#include "../../kernel/core/power/wake_lock_registry.hpp"
#include <stdint.h>

namespace auroraos {
namespace power_service {

enum class PowerProfile : uint8_t {
    PERFORMANCE = 0,
    BALANCED = 1,
    POWER_SAVE = 2,
    ULTRA_SAVER = 3
};

// WakeLockKind / BatteryInfo 定义在 power_state.hpp，客户端与服务端共享。

// 服务层电源管理器：面向 App 的策略门面。
// WakeLock 账本委托给内核共享的 WakeLockRegistry，避免内核 idle 决策与
// 服务层各持一份状态（两份状态不一致是"明明有锁却睡死"这类 bug 的根源）。
class PowerManager {
public:
    static constexpr int MAX_WAKE_LOCK_HOLDERS = WakeLockRegistry::MAX_HOLDERS;
    // 默认 WakeLock 超时：30s。App 忘记释放时系统仍可回到省电态。
    static constexpr uint32_t DEFAULT_WAKE_LOCK_TIMEOUT_MS = 30000;

    static PowerManager& instance() {
        static PowerManager pm;
        return pm;
    }

    bool acquire_wake_lock(uint32_t holder_id, WakeLockKind kind = WakeLockKind::PARTIAL,
                           uint32_t timeout_ms = DEFAULT_WAKE_LOCK_TIMEOUT_MS);
    bool release_wake_lock(uint32_t holder_id);
    void release_all_wake_locks(uint32_t holder_id);
    bool has_wake_lock(uint32_t holder_id) const;
    int get_wake_lock_count() const;
    void on_tick(uint32_t delta_ms);
    void reset();

    // Called by the system (or idle hook) to determine if sleep is allowed
    bool can_sleep() const;
    // 是否有 App 要求屏幕常亮
    bool is_screen_held() const;

    PowerState get_current_state() const {
        return current_state_;
    }

    void set_state(PowerState state) {
        current_state_ = state;
    }

    PowerProfile get_profile() const {
        return profile_;
    }

    void set_profile(PowerProfile profile) {
        profile_ = profile;
    }

    uint8_t get_battery_level() const {
        return battery_level_;
    }

    void update_battery_level(uint8_t level) {
        battery_level_ = (level > 100) ? 100 : level;
    }

    // 由板级电池驱动周期性推送遥测；未推送时保留上一次快照
    void update_battery_info(uint8_t level, uint16_t voltage_mv, int16_t temperature_c, uint8_t health,
                             uint8_t charge_state, bool plugged) {
        update_battery_level(level);
        battery_voltage_mv_ = voltage_mv;
        battery_temperature_c_ = temperature_c;
        battery_health_ = health;
        battery_charge_state_ = charge_state;
        battery_plugged_ = plugged;
    }

    BatteryInfo get_battery_info() const {
        BatteryInfo info;
        info.level = battery_level_;
        info.health = battery_health_;
        info.charge_state = battery_charge_state_;
        info.plugged = battery_plugged_ ? 1 : 0;
        info.voltage_mv = battery_voltage_mv_;
        info.temperature_c = battery_temperature_c_;
        return info;
    }

    uint32_t get_expired_lock_count() const {
        return expired_lock_count_;
    }

private:
    PowerManager() {
        reset();
    }

    PowerManager(const PowerManager&) = delete;
    PowerManager& operator=(const PowerManager&) = delete;

    PowerState current_state_ = PowerState::RUN;
    PowerProfile profile_ = PowerProfile::BALANCED;
    uint8_t battery_level_ = 100;
    uint16_t battery_voltage_mv_ = 0;
    int16_t battery_temperature_c_ = 25;
    uint8_t battery_health_ = 0;
    uint8_t battery_charge_state_ = 0;
    bool battery_plugged_ = false;
    uint32_t expired_lock_count_ = 0;
};

} // namespace power_service
} // namespace auroraos

#endif // AURORAOS_POWER_MANAGER_HPP
