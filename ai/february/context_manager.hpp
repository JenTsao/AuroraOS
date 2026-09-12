/**
 * @file context_manager.hpp
 * @brief Maintains the single UserContext for February
 *
 * Event policy: mutators mark the context dirty instead of publishing on every
 * write. A single coalesced ContextChanged event is emitted from tick() (and
 * from flush_change() for callers that need it immediately). This prevents a
 * burst of full UserContext payloads from flooding the 16-deep EventBus queue
 * and evicting real SensorFused / IntentDetected events.
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
        return storage_;
    }

    const UserContext& get() const { return ctx_; }

    void set_activity(ActivityState s) {
        if (ctx_.activity != s) {
            ctx_.activity = s;
            mark_dirty();
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
            mark_dirty();
        } else {
            ctx_.steps_delta = 0;
        }
        ctx_.timestamp_ms = now_ms;
    }

    void set_heart_rate(uint16_t hr) {
        if (ctx_.heart_rate != hr) {
            ctx_.heart_rate = hr;
            mark_dirty();
        }
    }

    void set_battery(uint8_t pct) {
        if (pct > 100) pct = 100;
        if (ctx_.battery_pct == pct) return;
        ctx_.battery_pct = pct;
        if (pct <= 15) ctx_.power = PowerMode::Critical;
        else if (pct <= 30) ctx_.power = PowerMode::Idle;
        else if (ctx_.power == PowerMode::Critical || ctx_.power == PowerMode::Idle)
            ctx_.power = PowerMode::Active;
        mark_dirty();
    }

    void set_wrist_raised(bool v) {
        if (ctx_.wrist_raised != v) {
            ctx_.wrist_raised = v;
            mark_dirty();
        }
    }

    void set_ble_connected(bool v) {
        if (ctx_.ble_connected != v) {
            ctx_.ble_connected = v;
            mark_dirty();
        }
    }

    void set_dnd(bool v) {
        if (ctx_.dnd != v) {
            ctx_.dnd = v;
            mark_dirty();
        }
    }

    void set_power_mode(PowerMode m) {
        if (ctx_.power != m) {
            ctx_.power = m;
            mark_dirty();
        }
    }

    void set_sensor_conf(unsigned kind_index, ConfidenceQ8 q8) {
        if (kind_index < static_cast<unsigned>(SensorKind::Count)) {
            ctx_.sensor_conf[kind_index] = q8;
        }
    }

    void set_accel(int16_t x_mg, int16_t y_mg, int16_t z_mg,
                   ConfidenceQ8 conf, uint32_t now_ms) {
        ctx_.accel_x_mg = x_mg;
        ctx_.accel_y_mg = y_mg;
        ctx_.accel_z_mg = z_mg;
        set_sensor_conf(static_cast<unsigned>(SensorKind::Accelerometer), conf);
        const int32_t ax = x_mg, ay = y_mg, az = z_mg;
        const int32_t mag2 = ax * ax + ay * ay + az * az;
        if (mag2 > (1200 * 1200) && ctx_.activity == ActivityState::Idle) {
            ctx_.activity = ActivityState::Walking;
            last_activity_ms_ = now_ms;
            ctx_.idle_seconds = 0;
            mark_dirty();
        }
        ctx_.timestamp_ms = now_ms;
    }

    void set_posture(int8_t pitch, int8_t roll, bool raised,
                     ConfidenceQ8 conf, uint32_t now_ms) {
        ctx_.posture_pitch = pitch;
        ctx_.posture_roll = roll;
        ctx_.wrist_raised = raised;
        set_sensor_conf(static_cast<unsigned>(SensorKind::Posture), conf);
        if (raised) {
            last_activity_ms_ = now_ms;
            ctx_.idle_seconds = 0;
        }
        ctx_.timestamp_ms = now_ms;
    }

    void set_ble(int16_t rssi_dbm, bool connected, ConfidenceQ8 conf) {
        ctx_.ble_rssi_dbm = rssi_dbm;
        ctx_.ble_connected = connected;
        set_sensor_conf(static_cast<unsigned>(SensorKind::BleRssi), conf);
    }

    void set_wifi(uint8_t ap_count, int16_t strongest_rssi, ConfidenceQ8 conf) {
        ctx_.wifi_ap_count = ap_count;
        ctx_.wifi_rssi_dbm = strongest_rssi;
        set_sensor_conf(static_cast<unsigned>(SensorKind::WifiScan), conf);
    }

    void set_rf_interference(uint8_t level_q8, ConfidenceQ8 conf) {
        ctx_.rf_interference_q8 = level_q8;
        set_sensor_conf(static_cast<unsigned>(SensorKind::RfInterference), conf);
    }

    void set_time_context(const TimeContext& tc, uint32_t now_ms) {
        const bool changed =
            ctx_.time_ctx.tod != tc.tod ||
            ctx_.time_ctx.day != tc.day ||
            ctx_.time_ctx.hour != tc.hour ||
            ctx_.time_ctx.weekday != tc.weekday;
        ctx_.time_ctx = tc;
        ctx_.timestamp_ms = now_ms;
        if (changed) {
            mark_dirty();
        }
    }

    void clear() {
        ctx_ = UserContext{};
        last_activity_ms_ = 0;
        dirty_ = false;
    }

    void tick(uint32_t now_ms) {
        if (last_activity_ms_ == 0) last_activity_ms_ = now_ms;
        ctx_.timestamp_ms = now_ms;
        const uint32_t idle_ms = (now_ms >= last_activity_ms_)
            ? (now_ms - last_activity_ms_) : 0u;
        ctx_.idle_seconds = idle_ms / 1000u;
        if (ctx_.idle_seconds > 30 && ctx_.activity == ActivityState::Walking) {
            ctx_.activity = ActivityState::Idle;
            mark_dirty();
        }
        // Coalesced publish: at most one ContextChanged per tick cycle.
        flush_change();
    }

    UserContext snapshot() const { return ctx_; }

private:
    constexpr ContextManager() = default;
    static ContextManager storage_;

    void mark_dirty() { dirty_ = true; }

    /** Emit a single coalesced ContextChanged if anything changed since last flush. */
    void flush_change() {
        if (!dirty_) return;
        dirty_ = false;
        Event ev;
        ev.type = EventType::ContextChanged;
        ev.timestamp_ms = ctx_.timestamp_ms;
        ev.payload.context = ctx_;
        EventBus::instance().publish(ev);
    }

    UserContext ctx_{};
    uint32_t    last_activity_ms_ = 0;
    bool        dirty_ = false;
};

inline ContextManager ContextManager::storage_{};

}  // namespace february
}  // namespace aurora

#endif  // AURORA_FEBRUARY_CONTEXT_MANAGER_HPP
