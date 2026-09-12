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

    constexpr CooldownGate() = default;
    constexpr explicit CooldownGate(uint32_t period) : period_ms(period) {}

    constexpr bool try_fire(uint32_t now_ms) {
        if (period_ms == 0) {
            last_fire_ms = now_ms;
            return true;
        }
        // Modular uint32 subtraction handles timestamp wrapping and elapsed duration reliably
        const bool elapsed = (last_fire_ms == 0) ||
                             (static_cast<uint32_t>(now_ms - last_fire_ms) >= period_ms);
        if (elapsed) {
            last_fire_ms = now_ms;
            return true;
        }
        return false;
    }

    constexpr void reset() { last_fire_ms = 0; }
};

struct LevelLatch {
    bool latched = false;

    constexpr LevelLatch() = default;

    constexpr bool rising(bool active) {
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

    constexpr void reset() { latched = false; }
};

}  // namespace february
}  // namespace aurora

#endif
