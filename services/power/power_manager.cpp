#include "power_manager.hpp"

namespace auroraos {
namespace power_service {

void PowerManager::acquire_wake_lock(uint32_t holder_id) {
    (void)holder_id; // In a full implementation, track specific owners to avoid leaks
    active_wake_locks_++;
}

void PowerManager::release_wake_lock(uint32_t holder_id) {
    (void)holder_id;
    if (active_wake_locks_ > 0) {
        active_wake_locks_--;
    }
}

bool PowerManager::can_sleep() const {
    // Cannot sleep if there are active wake locks
    if (active_wake_locks_ > 0) {
        return false;
    }
    
    // Additional heuristics: is the battery critically low but we need to power off?
    // Are there active DMA transfers pending?
    // For now, only depend on wake locks.
    return true;
}

} // namespace power_service
} // namespace auroraos
