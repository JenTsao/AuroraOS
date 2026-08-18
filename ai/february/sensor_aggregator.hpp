/**
 * @file sensor_aggregator.hpp
 * @brief PerceptionFusion — unify multi-modal sensors into SensorSample events
 *
 * Sources: accelerometer, activity recognition, heart rate, wall-clock time,
 * device posture, BLE RSSI, WiFi scan, RF interference level.
 *
 * Design:
 *   - Zero heap: static ring of SensorSample + last-value cache
 *   - All fixed-point / integer (confidence_Q8, milli-g, dBm)
 *   - Publishes EventType::SensorFused / SensorUpdate onto EventBus
 *   - TimeContext derived from hour/weekday (morning / deep-night / workday)
 */
#ifndef AURORA_FEBRUARY_SENSOR_AGGREGATOR_HPP
#define AURORA_FEBRUARY_SENSOR_AGGREGATOR_HPP

#include "types.hpp"
#include "event_bus.hpp"
#include "context_manager.hpp"
#include "crit.hpp"

namespace aurora {
namespace february {

#ifndef FEBRUARY_SENSOR_RING_DEPTH
#define FEBRUARY_SENSOR_RING_DEPTH 16
#endif

static_assert((FEBRUARY_SENSOR_RING_DEPTH & (FEBRUARY_SENSOR_RING_DEPTH - 1)) == 0,
              "sensor ring depth must be power of two");

class SensorAggregator {
public:
    static SensorAggregator& instance() {
        static SensorAggregator agg;
        return agg;
    }

    bool push(const SensorSample& sample) {
        if (sample.kind >= SensorKind::Count) {
            return false;
        }
        FebruaryCrit::Guard g;

        last_[static_cast<unsigned>(sample.kind)] = sample;

        const unsigned next = (head_ + 1u) & (FEBRUARY_SENSOR_RING_DEPTH - 1u);
        if (next == tail_) {
            tail_ = (tail_ + 1u) & (FEBRUARY_SENSOR_RING_DEPTH - 1u);
            ++drop_count_;
        }
        ring_[head_] = sample;
        head_ = next;
        ++push_count_;

        apply_to_context(sample);
        publish_sample(sample);
        return true;
    }

    bool feed_accel(int16_t x_mg, int16_t y_mg, int16_t z_mg,
                    ConfidenceQ8 conf, uint32_t now_ms) {
        SensorSample s{};
        s.kind = SensorKind::Accelerometer;
        s.confidence_q8 = conf;
        s.timestamp_ms = now_ms;
        s.data.accel.x_mg = x_mg;
        s.data.accel.y_mg = y_mg;
        s.data.accel.z_mg = z_mg;
        return push(s);
    }

    bool feed_activity(ActivityState act, ConfidenceQ8 conf, uint32_t now_ms) {
        SensorSample s{};
        s.kind = SensorKind::Activity;
        s.confidence_q8 = conf;
        s.timestamp_ms = now_ms;
        s.data.activity.activity = static_cast<uint8_t>(act);
        s.data.activity.intensity_q8 = conf;
        return push(s);
    }

    bool feed_heart_rate(uint16_t bpm, ConfidenceQ8 conf, uint32_t now_ms) {
        SensorSample s{};
        s.kind = SensorKind::HeartRate;
        s.confidence_q8 = conf;
        s.timestamp_ms = now_ms;
        s.data.heart.bpm = bpm;
        return push(s);
    }

    bool feed_time(uint8_t hour, uint8_t minute, uint8_t weekday,
                   ConfidenceQ8 conf, uint32_t now_ms) {
        if (hour > 23) hour = 23;
        if (minute > 59) minute = 59;
        if (weekday > 6) weekday = 6;

        SensorSample s{};
        s.kind = SensorKind::Time;
        s.confidence_q8 = conf;
        s.timestamp_ms = now_ms;
        s.data.time.hour = hour;
        s.data.time.minute = minute;
        s.data.time.weekday = weekday;
        return push(s);
    }

    bool feed_posture(int8_t pitch_deg, int8_t roll_deg, bool raised,
                      ConfidenceQ8 conf, uint32_t now_ms) {
        SensorSample s{};
        s.kind = SensorKind::Posture;
        s.confidence_q8 = conf;
        s.timestamp_ms = now_ms;
        s.data.posture.pitch_deg = pitch_deg;
        s.data.posture.roll_deg = roll_deg;
        s.data.posture.raised = raised ? 1u : 0u;
        return push(s);
    }

    bool feed_ble_rssi(int16_t rssi_dbm, bool connected,
                       ConfidenceQ8 conf, uint32_t now_ms) {
        SensorSample s{};
        s.kind = SensorKind::BleRssi;
        s.confidence_q8 = conf;
        s.timestamp_ms = now_ms;
        s.data.ble.rssi_dbm = rssi_dbm;
        s.data.ble.connected = connected ? 1u : 0u;
        return push(s);
    }

    bool feed_wifi_scan(uint8_t ap_count, int16_t strongest_rssi_dbm,
                        ConfidenceQ8 conf, uint32_t now_ms) {
        SensorSample s{};
        s.kind = SensorKind::WifiScan;
        s.confidence_q8 = conf;
        s.timestamp_ms = now_ms;
        s.data.wifi.ap_count = ap_count;
        s.data.wifi.strongest_rssi_dbm = strongest_rssi_dbm;
        return push(s);
    }

    bool feed_rf_interference(uint8_t level_q8, int16_t noise_dbm_q8,
                              ConfidenceQ8 conf, uint32_t now_ms) {
        SensorSample s{};
        s.kind = SensorKind::RfInterference;
        s.confidence_q8 = conf;
        s.timestamp_ms = now_ms;
        s.data.rf.level_q8 = level_q8;
        s.data.rf.noise_dbm_q8 = noise_dbm_q8;
        return push(s);
    }

    bool feed_steps(uint32_t steps, ConfidenceQ8 conf, uint32_t now_ms) {
        SensorSample s{};
        s.kind = SensorKind::StepCount;
        s.confidence_q8 = conf;
        s.timestamp_ms = now_ms;
        s.data.steps.steps = steps;
        return push(s);
    }

    bool feed_battery(uint8_t pct, ConfidenceQ8 conf, uint32_t now_ms) {
        SensorSample s{};
        s.kind = SensorKind::Battery;
        s.confidence_q8 = conf;
        s.timestamp_ms = now_ms;
        s.data.battery.pct = pct > 100 ? 100 : pct;
        return push(s);
    }

    bool last(SensorKind kind, SensorSample* out) const {
        if (!out || kind >= SensorKind::Count) return false;
        FebruaryCrit::Guard g;
        const SensorSample& s = last_[static_cast<unsigned>(kind)];
        if (s.kind != kind) return false;
        *out = s;
        return true;
    }

    TimeContext time_context() const {
        FebruaryCrit::Guard g;
        return time_ctx_;
    }

    unsigned pending() const {
        FebruaryCrit::Guard g;
        if (head_ >= tail_) return head_ - tail_;
        return FEBRUARY_SENSOR_RING_DEPTH - tail_ + head_;
    }

    uint32_t drop_count() const { return drop_count_; }
    uint32_t push_count() const { return push_count_; }

    void clear() {
        FebruaryCrit::Guard g;
        head_ = tail_ = 0;
        drop_count_ = push_count_ = 0;
        time_ctx_ = TimeContext{};
        for (unsigned i = 0; i < static_cast<unsigned>(SensorKind::Count); ++i) {
            last_[i] = SensorSample{};
        }
    }

    static TimeContext make_time_context(uint8_t hour, uint8_t minute, uint8_t weekday) {
        TimeContext tc{};
        tc.hour = hour;
        tc.minute = minute;
        tc.weekday = weekday;
        tc.minute_of_day = static_cast<uint16_t>(hour) * 60u + minute;

        if (hour < 5) {
            tc.tod = TimeOfDay::DeepNight;
        } else if (hour < 12) {
            tc.tod = TimeOfDay::Morning;
        } else if (hour < 18) {
            tc.tod = TimeOfDay::Afternoon;
        } else if (hour < 22) {
            tc.tod = TimeOfDay::Evening;
        } else {
            tc.tod = TimeOfDay::LateNight;
        }

        tc.day = (weekday >= 5) ? DayClass::Weekend : DayClass::Workday;
        return tc;
    }

private:
    SensorAggregator() = default;

    void apply_to_context(const SensorSample& s) {
        ContextManager& cm = ContextManager::instance();
        const unsigned ki = static_cast<unsigned>(s.kind);

        switch (s.kind) {
        case SensorKind::Accelerometer:
            cm.set_accel(s.data.accel.x_mg, s.data.accel.y_mg, s.data.accel.z_mg,
                         s.confidence_q8, s.timestamp_ms);
            break;
        case SensorKind::Activity:
            if (s.confidence_q8 >= 128) {
                cm.set_activity(static_cast<ActivityState>(s.data.activity.activity));
            }
            cm.set_sensor_conf(ki, s.confidence_q8);
            break;
        case SensorKind::HeartRate:
            cm.set_heart_rate(s.data.heart.bpm);
            cm.set_sensor_conf(ki, s.confidence_q8);
            break;
        case SensorKind::Time: {
            TimeContext tc = make_time_context(s.data.time.hour, s.data.time.minute,
                                               s.data.time.weekday);
            time_ctx_ = tc;
            cm.set_time_context(tc, s.timestamp_ms);
            cm.set_sensor_conf(ki, s.confidence_q8);
            break;
        }
        case SensorKind::Posture:
            cm.set_posture(s.data.posture.pitch_deg, s.data.posture.roll_deg,
                           s.data.posture.raised != 0, s.confidence_q8, s.timestamp_ms);
            break;
        case SensorKind::BleRssi:
            cm.set_ble(s.data.ble.rssi_dbm, s.data.ble.connected != 0, s.confidence_q8);
            break;
        case SensorKind::WifiScan:
            cm.set_wifi(s.data.wifi.ap_count, s.data.wifi.strongest_rssi_dbm,
                        s.confidence_q8);
            break;
        case SensorKind::RfInterference:
            cm.set_rf_interference(s.data.rf.level_q8, s.confidence_q8);
            break;
        case SensorKind::StepCount:
            cm.update_steps(s.data.steps.steps, s.timestamp_ms);
            cm.set_sensor_conf(ki, s.confidence_q8);
            break;
        case SensorKind::Battery:
            cm.set_battery(s.data.battery.pct);
            cm.set_sensor_conf(ki, s.confidence_q8);
            break;
        default:
            break;
        }
    }

    void publish_sample(const SensorSample& s) {
        Event ev;
        ev.type = EventType::SensorFused;
        ev.timestamp_ms = s.timestamp_ms;
        ev.source_id = static_cast<uint32_t>(s.kind);
        ev.payload.sensor = s;
        EventBus::instance().publish(ev);

        if (s.kind == SensorKind::Time) {
            Event tev;
            tev.type = EventType::TimeContextChanged;
            tev.timestamp_ms = s.timestamp_ms;
            tev.payload.time_ctx = time_ctx_;
            EventBus::instance().publish(tev);
        }
    }

    SensorSample ring_[FEBRUARY_SENSOR_RING_DEPTH]{};
    SensorSample last_[static_cast<unsigned>(SensorKind::Count)]{};
    TimeContext  time_ctx_{};
    unsigned     head_ = 0;
    unsigned     tail_ = 0;
    uint32_t     drop_count_ = 0;
    uint32_t     push_count_ = 0;
};

}  // namespace february
}  // namespace aurora

#endif  // AURORA_FEBRUARY_SENSOR_AGGREGATOR_HPP
