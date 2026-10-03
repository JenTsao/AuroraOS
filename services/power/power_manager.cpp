#include "power_manager.hpp"

namespace auroraos {
namespace power_service {

namespace {

WakeLockType to_registry_type(WakeLockKind kind) {
    return (kind == WakeLockKind::SCREEN) ? WakeLockType::SCREEN : WakeLockType::PARTIAL;
}

} // namespace

void PowerManager::reset() {
    current_state_ = PowerState::RUN;
    profile_ = PowerProfile::BALANCED;
    battery_level_ = 100;
    battery_voltage_mv_ = 0;
    battery_temperature_c_ = 25;
    battery_health_ = 0;
    battery_charge_state_ = 0;
    battery_plugged_ = false;
    expired_lock_count_ = 0;
    WakeLockRegistry::instance().reset();
}

bool PowerManager::acquire_wake_lock(uint32_t holder_id, WakeLockKind kind, uint32_t timeout_ms) {
    return WakeLockRegistry::instance().acquire(holder_id, to_registry_type(kind), timeout_ms);
}

bool PowerManager::release_wake_lock(uint32_t holder_id) {
    return WakeLockRegistry::instance().release(holder_id);
}

void PowerManager::release_all_wake_locks(uint32_t holder_id) {
    WakeLockRegistry::instance().release_all(holder_id);
}

bool PowerManager::has_wake_lock(uint32_t holder_id) const {
    return WakeLockRegistry::instance().is_held(holder_id);
}

int PowerManager::get_wake_lock_count() const {
    return WakeLockRegistry::instance().lock_count();
}

void PowerManager::on_tick(uint32_t delta_ms) {
    expired_lock_count_ += static_cast<uint32_t>(WakeLockRegistry::instance().on_tick(delta_ms));
}

bool PowerManager::can_sleep() const {
    return !WakeLockRegistry::instance().holds_any();
}

bool PowerManager::is_screen_held() const {
    return WakeLockRegistry::instance().holds_screen();
}

} // namespace power_service
} // namespace auroraos
