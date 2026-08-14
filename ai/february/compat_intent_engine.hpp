/**
 * @file compat_intent_engine.hpp
 * @brief Drop-in bridge from the original IntentEngine API to FebruaryCore.
 *
 * Original call site (apps/kernel.cpp system_daemon_task):
 *   IntentEngine::Context intent_ctx;
 *   IntentEngine::process_sensors(g_lua_app, intent_ctx);
 *
 * The original implementation samples steps from SensorManager and drives
 * AppControlBlock transitions. This bridge preserves that behavior and also
 * feeds FebruaryCore so the new pipeline stays in sync.
 *
 * Codex review: do NOT use ctx.last_steps as the live sample — it is only
 * bookkeeping written by this function itself; the caller never updates it.
 */
#ifndef AURORA_FEBRUARY_COMPAT_INTENT_ENGINE_HPP
#define AURORA_FEBRUARY_COMPAT_INTENT_ENGINE_HPP

#include "february_core.hpp"

// Same dependencies as the original ai/intent_engine.hpp
#include "../drivers/sensor/sensor_framework.hpp"
#include "../kernel/task/app_lifecycle.hpp"

namespace aurora {
namespace february {

/**
 * Preferred helper when the caller already has a step count and timebase
 * (host tests, SoftBus, unit tests — no SensorManager required).
 */
inline void process_sensors_compat(uint32_t steps, uint32_t now_ms) {
    auto& f = FebruaryCore::instance();
    if (!f.ready()) {
        f.init();
    }
    f.feed_steps(steps, now_ms);
    f.tick(now_ms);
    f.process_events();
}

}  // namespace february
}  // namespace aurora

#ifndef AURORA_DISABLE_LEGACY_INTENT
/**
 * Legacy global IntentEngine name so existing #includes / call sites compile
 * without rewriting apps/kernel.cpp.
 */
class IntentEngine {
public:
    struct Context {
        uint32_t last_steps  = 0;
        uint32_t idle_counts = 0;
        uint32_t now_ms      = 0;  // optional; 0 → local monotonic counter
    };

    /**
     * Drop-in replacement for the original process_sensors.
     * 1) Samples live steps from SensorManager (not ctx.last_steps).
     * 2) Preserves FOREGROUND / BACKGROUND transitions on fitness_app.
     * 3) Feeds FebruaryCore with the same sample.
     */
    static void process_sensors(AppControlBlock& fitness_app, Context& ctx) {
        // Live sample — same source as original ai/intent_engine.hpp
        const uint32_t steps =
            SensorManager::instance().get_accel_sensor().get_steps();

        static uint32_t fallback_ms = 0;
        const uint32_t now =
            ctx.now_ms ? ctx.now_ms : (fallback_ms += 500);  // matches daemon sleep_ms(500)

        // ---- original sliding-window / debounce behavior ------------------
        if (steps != ctx.last_steps) {
            ctx.last_steps  = steps;
            ctx.idle_counts = 0;
            if (fitness_app.state != AppState::FOREGROUND) {
                fitness_app.transition_to(AppState::FOREGROUND);
            }
        } else {
            ctx.idle_counts++;
            if (ctx.idle_counts > 10) {  // ~5 s at 500 ms tick
                if (fitness_app.state == AppState::FOREGROUND) {
                    fitness_app.transition_to(AppState::BACKGROUND);
                }
            }
        }

        // ---- February pipeline --------------------------------------------
        aurora::february::process_sensors_compat(steps, now);
    }
};
#endif

#endif  // AURORA_FEBRUARY_COMPAT_INTENT_ENGINE_HPP
