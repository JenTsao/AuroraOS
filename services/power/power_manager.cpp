#include "power_manager.hpp"

namespace auroraos {
namespace power_service {

void PowerManager::reset() {
    current_state_ = PowerState::RUN;
    profile_ = PowerProfile::BALANCED;
    battery_level_ = 100;
    total_active_locks_ = 0;
    for (int i = 0; i < MAX_WAKE_LOCK_HOLDERS; ++i) {
        lock_table_[i].holder_id = 0;
        lock_table_[i].ref_count = 0;
        lock_table_[i].active = false;
    }
}

bool PowerManager::acquire_wake_lock(uint32_t holder_id) {
    // Check if holder already exists
    for (int i = 0; i < MAX_WAKE_LOCK_HOLDERS; ++i) {
        if (lock_table_[i].active && lock_table_[i].holder_id == holder_id) {
            lock_table_[i].ref_count++;
            total_active_locks_++;
            return true;
        }
    }

    // Allocate free slot
    for (int i = 0; i < MAX_WAKE_LOCK_HOLDERS; ++i) {
        if (!lock_table_[i].active) {
            lock_table_[i].holder_id = holder_id;
            lock_table_[i].ref_count = 1;
            lock_table_[i].active = true;
            total_active_locks_++;
            return true;
        }
    }

    // Slot table full
    return false;
}

bool PowerManager::release_wake_lock(uint32_t holder_id) {
    for (int i = 0; i < MAX_WAKE_LOCK_HOLDERS; ++i) {
        if (lock_table_[i].active && lock_table_[i].holder_id == holder_id) {
            if (lock_table_[i].ref_count > 0) {
                lock_table_[i].ref_count--;
                if (total_active_locks_ > 0) {
                    total_active_locks_--;
                }
            }
            if (lock_table_[i].ref_count == 0) {
                lock_table_[i].active = false;
                lock_table_[i].holder_id = 0;
            }
            return true;
        }
    }
    return false;
}

void PowerManager::release_all_wake_locks(uint32_t holder_id) {
    for (int i = 0; i < MAX_WAKE_LOCK_HOLDERS; ++i) {
        if (lock_table_[i].active && lock_table_[i].holder_id == holder_id) {
            total_active_locks_ -= lock_table_[i].ref_count;
            if (total_active_locks_ < 0) {
                total_active_locks_ = 0;
            }
            lock_table_[i].ref_count = 0;
            lock_table_[i].active = false;
            lock_table_[i].holder_id = 0;
            break;
        }
    }
}

bool PowerManager::has_wake_lock(uint32_t holder_id) const {
    for (int i = 0; i < MAX_WAKE_LOCK_HOLDERS; ++i) {
        if (lock_table_[i].active && lock_table_[i].holder_id == holder_id && lock_table_[i].ref_count > 0) {
            return true;
        }
    }
    return false;
}

int PowerManager::get_wake_lock_count() const {
    return total_active_locks_;
}

bool PowerManager::can_sleep() const {
    // Cannot sleep if there are active wake locks
    if (total_active_locks_ > 0) {
        return false;
    }
    return true;
}

} // namespace power_service
} // namespace auroraos

