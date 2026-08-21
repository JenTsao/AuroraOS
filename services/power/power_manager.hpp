#ifndef AURORAOS_POWER_MANAGER_HPP
#define AURORAOS_POWER_MANAGER_HPP

#include "power_state.hpp"
#include <stdint.h>

namespace auroraos {
namespace power_service {

enum class PowerProfile : uint8_t {
    PERFORMANCE = 0,
    BALANCED = 1,
    POWER_SAVE = 2,
    ULTRA_SAVER = 3
};

class PowerManager {
public:
    static constexpr int MAX_WAKE_LOCK_HOLDERS = 16;

    static PowerManager& instance() {
        static PowerManager pm;
        return pm;
    }

    bool acquire_wake_lock(uint32_t holder_id);
    bool release_wake_lock(uint32_t holder_id);
    void release_all_wake_locks(uint32_t holder_id);
    bool has_wake_lock(uint32_t holder_id) const;
    int get_wake_lock_count() const;
    void reset();

    // Called by the system (or idle hook) to determine if sleep is allowed
    bool can_sleep() const;

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
        battery_level_ = level;
    }

private:
    PowerManager() {
        reset();
    }

    struct WakeLockEntry {
        uint32_t holder_id;
        uint16_t ref_count;
        bool active;
    };

    PowerState current_state_ = PowerState::RUN;
    PowerProfile profile_ = PowerProfile::BALANCED;
    uint8_t battery_level_ = 100;

    WakeLockEntry lock_table_[MAX_WAKE_LOCK_HOLDERS]{};
    int total_active_locks_ = 0;
};

} // namespace power_service
} // namespace auroraos

#endif // AURORAOS_POWER_MANAGER_HPP

