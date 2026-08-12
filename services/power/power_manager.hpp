#ifndef AURORAOS_POWER_MANAGER_HPP
#define AURORAOS_POWER_MANAGER_HPP

#include "power_state.hpp"
#include <stdint.h>

namespace auroraos {
namespace power_service {

class PowerManager {
public:
    static PowerManager& instance() {
        static PowerManager pm;
        return pm;
    }

    void acquire_wake_lock(uint32_t holder_id);
    void release_wake_lock(uint32_t holder_id);
    
    // Called by the system (or idle hook) to determine if sleep is allowed
    bool can_sleep() const;
    
    PowerState get_current_state() const { return current_state_; }
    void set_state(PowerState state) { current_state_ = state; }

    uint8_t get_battery_level() const { return battery_level_; }
    void update_battery_level(uint8_t level) { battery_level_ = level; }

private:
    PowerManager() = default;

    PowerState current_state_ = PowerState::RUN;
    uint8_t battery_level_ = 100;
    
    // Simple wake lock counting or tracking
    // For simplicity, we just keep a counter of active locks
    int active_wake_locks_ = 0;
};

} // namespace power_service
} // namespace auroraos

#endif // AURORAOS_POWER_MANAGER_HPP
