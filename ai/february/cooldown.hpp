/**
 * @file cooldown.hpp
 * @brief Unified cooldown / latch helpers for proactive intents
 */
#ifndef AURORA_FEBRUARY_COOLDOWN_HPP
#define AURORA_FEBRUARY_COOLDOWN_HPP

#include <cstdint>

namespace aurora {
namespace february {

struct CooldownGate {
    uint32_t period_ms    = 0;
    uint32_t last_fire_ms = 0;

    explicit CooldownGate(uint32_t period = 0) : period_ms(period) {}

    bool try_fire(uint32_t now_ms) {
        if (period_ms == 0) {
            last_fire_ms = now_ms;
            return true;
        }
        if (last_fire_ms == 0 || now_ms - last_fire_ms >= period_ms) {
            last_fire_ms = now_ms;
            return true;
        }
        return false;
    }

    void reset() { last_fire_ms = 0; }
};

struct LevelLatch {
    bool latched = false;

    bool rising(bool active) {
        if (active) {
            if (!latched) {
                latched = true;
                return true;
            }
            return false;
        }
        latched = false;
        return false;
    }

    void reset() { latched = false; }
};

}  // namespace february
}  // namespace aurora

#endif
