/**
 * @file compat_intent_engine.hpp
 * @brief Drop-in bridge from the original IntentEngine API to FebruaryCore.
 *
 * AppControlBlock transitions: owned HERE (legacy behavior).
 * February still gets sensor samples; TransitionApp hooks suppressed during feed.
 * Do NOT include together with ai/intent_engine.hpp.
 */
#ifndef AURORA_FEBRUARY_COMPAT_INTENT_ENGINE_HPP
#define AURORA_FEBRUARY_COMPAT_INTENT_ENGINE_HPP

#if defined(AURORA_INTENT_ENGINE_HPP)
#error "Include either ai/intent_engine.hpp OR ai/february/compat_intent_engine.hpp, not both"
#endif
#define AURORA_INTENT_ENGINE_HPP

#include "february_core.hpp"
#include "../drivers/sensor/sensor_framework.hpp"
#include "../kernel/task/app_lifecycle.hpp"

namespace aurora {
namespace february {

inline void process_sensors_compat(uint32_t steps, uint32_t now_ms) {
    auto& f = FebruaryCore::instance();
    if (!f.ready()) f.init();
    f.feed_steps(steps, now_ms);
    f.tick(now_ms);
    f.process_events();
}

}  // namespace february
}  // namespace aurora

#ifndef AURORA_DISABLE_LEGACY_INTENT
class IntentEngine {
public:
    struct Context {
        uint32_t last_steps  = 0;
        uint32_t idle_counts = 0;
        uint32_t now_ms      = 0;
    };

    static void process_sensors(AppControlBlock& fitness_app, Context& ctx) {
        const uint32_t steps =
            SensorManager::instance().get_accel_sensor().get_steps();

        static uint32_t fallback_ms = 0;
        const uint32_t now =
            ctx.now_ms ? ctx.now_ms : (fallback_ms += 500);

        if (steps != ctx.last_steps) {
            ctx.last_steps  = steps;
            ctx.idle_counts = 0;
            if (fitness_app.state != AppState::FOREGROUND) {
                fitness_app.transition_to(AppState::FOREGROUND);
            }
        } else {
            ctx.idle_counts++;
            if (ctx.idle_counts > 10) {
                if (fitness_app.state == AppState::FOREGROUND) {
                    fitness_app.transition_to(AppState::BACKGROUND);
                }
            }
        }

        auto& f = aurora::february::FebruaryCore::instance();
        if (!f.ready()) f.init();
        const bool prev = f.manage_app_transitions();
        f.set_manage_app_transitions(false);
        aurora::february::process_sensors_compat(steps, now);
        f.set_manage_app_transitions(prev);
    }
};
#endif

#endif
