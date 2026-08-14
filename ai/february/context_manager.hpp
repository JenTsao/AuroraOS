/**
 * @file context_manager.hpp
 * @brief Maintains the single UserContext for February
 */
#ifndef AURORA_FEBRUARY_CONTEXT_MANAGER_HPP
#define AURORA_FEBRUARY_CONTEXT_MANAGER_HPP

#include "types.hpp"
#include "event_bus.hpp"

namespace aurora {
namespace february {

class ContextManager {
public:
    static ContextManager& instance() {
        static ContextManager cm;
        return cm;
    }

    const UserContext& get() const { return ctx_; }

    void set_activity(ActivityState s) {
        if (ctx_.activity != s) {
            ctx_.activity = s;
            publish_change();
        }
    }

    void update_steps(uint32_t steps, uint32_t now_ms) {
        if (steps != ctx_.steps) {
            ctx_.steps_delta = (steps >= ctx_.steps) ? (steps - ctx_.steps) : 0;
            ctx_.steps = steps;
            last_activity_ms_ = now_ms;
            ctx_.idle_seconds = 0;
            if (ctx_.steps_delta > 0 &&
                (ctx_.activity == ActivityState::Idle ||
                 ctx_.activity == ActivityState::Unknown)) {
                ctx_.activity = ActivityState::Walking;
            }
        } else {
            ctx_.steps_delta = 0;
        }
        ctx_.timestamp_ms = now_ms;
        publish_change();
    }

    void set_heart_rate(uint16_t hr) {
        ctx_.heart_rate = hr;
    }

    void set_battery(uint8_t pct) {
        if (pct > 100) pct = 100;
        ctx_.battery_pct = pct;
        if (pct <= 15) {
            ctx_.power = PowerMode::Critical;
        } else if (pct <= 30) {
            ctx_.power = PowerMode::Idle;
        } else if (ctx_.power == PowerMode::Critical ||
                   ctx_.power == PowerMode::Idle) {
            ctx_.power = PowerMode::Active;
        }
    }

    void set_wrist_raised(bool v) { ctx_.wrist_raised = v; }
    void set_ble_connected(bool v) { ctx_.ble_connected = v; }
    void set_dnd(bool v) { ctx_.dnd = v; }
    void set_power_mode(PowerMode m) { ctx_.power = m; }

    void tick(uint32_t now_ms) {
        if (last_activity_ms_ == 0) {
            last_activity_ms_ = now_ms;
        }
        ctx_.timestamp_ms = now_ms;
        const uint32_t idle_ms = now_ms - last_activity_ms_;
        ctx_.idle_seconds = idle_ms / 1000u;
        if (ctx_.idle_seconds > 30 && ctx_.activity == ActivityState::Walking) {
            ctx_.activity = ActivityState::Idle;
            publish_change();
        }
    }

    UserContext snapshot() const { return ctx_; }

private:
    ContextManager() = default;

    void publish_change() {
        Event ev;
        ev.type = EventType::ContextChanged;
        ev.timestamp_ms = ctx_.timestamp_ms;
        ev.payload.context = ctx_;
        EventBus::instance().publish(ev);
    }

    UserContext ctx_{};
    uint32_t    last_activity_ms_ = 0;
};

}  // namespace february
}  // namespace aurora

#endif  // AURORA_FEBRUARY_CONTEXT_MANAGER_HPP
