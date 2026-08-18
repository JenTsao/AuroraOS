/**
 * @file february_core.hpp
 * @brief February facade — single entry point for the AI runtime
 */
#ifndef AURORA_FEBRUARY_CORE_HPP
#define AURORA_FEBRUARY_CORE_HPP

#include "types.hpp"
#include "event_bus.hpp"
#include "context_manager.hpp"
#include "intent_engine.hpp"
#include "persona.hpp"
#include "action_executor.hpp"
#include "wake_word.hpp"
#include "memory.hpp"
#include "log.hpp"
#include "config.hpp"
#include "planner.hpp"
#include "sensor_aggregator.hpp"

namespace aurora {
namespace february {

constexpr int32_t kAppIdFitness       = 1;
constexpr int32_t kAppStateForeground = 1;
constexpr int32_t kAppStateBackground = 2;

class FebruaryCore {
public:
    static FebruaryCore& instance() {
        static FebruaryCore core;
        return core;
    }

    void init() {
        if (ready_) return;
        EventBus::instance().clear();
        SensorAggregator::instance().clear();
        ContextManager::instance().clear();
        EventBus::instance().subscribe(EventType::IntentDetected, &FebruaryCore::on_intent_static, this,
                                       SubPriority::Normal);
        EventBus::instance().subscribe(EventType::ProactiveTrigger, &FebruaryCore::on_intent_static, this,
                                       SubPriority::Normal);
        IntentEngine::instance().bind_sensor_bus();
        Persona::instance().set_name("February");
        SessionMemory::instance().clear();
        IntentEngine::instance().reset_proactive();
        ready_ = true;
    }

    bool ready() const { return ready_; }

    void feed_steps(uint32_t steps, uint32_t now_ms) {
        IntentEngine::instance().on_steps(steps, now_ms);
    }

    void feed_battery(uint8_t pct, uint32_t now_ms) {
        IntentEngine::instance().on_battery(pct, now_ms);
    }

    void feed_heart_rate(uint16_t hr, uint32_t now_ms) {
        IntentEngine::instance().on_heart_rate(hr, now_ms);
    }

    void feed_wrist_gesture(bool raised, uint32_t now_ms) {
        IntentEngine::instance().on_wrist_gesture(raised, now_ms);
        SensorAggregator::instance().feed_posture(0, 0, raised, 200, now_ms);
    }

    void feed_accel(int16_t x_mg, int16_t y_mg, int16_t z_mg,
                    uint8_t conf_q8, uint32_t now_ms) {
        SensorAggregator::instance().feed_accel(x_mg, y_mg, z_mg, conf_q8, now_ms);
    }
    void feed_activity(ActivityState act, uint8_t conf_q8, uint32_t now_ms) {
        SensorAggregator::instance().feed_activity(act, conf_q8, now_ms);
    }
    void feed_time(uint8_t hour, uint8_t minute, uint8_t weekday, uint32_t now_ms) {
        SensorAggregator::instance().feed_time(hour, minute, weekday, 255, now_ms);
    }
    void feed_ble_rssi(int16_t rssi_dbm, bool connected, uint32_t now_ms) {
        SensorAggregator::instance().feed_ble_rssi(rssi_dbm, connected, 200, now_ms);
    }
    void feed_wifi_scan(uint8_t ap_count, int16_t strongest_rssi, uint32_t now_ms) {
        SensorAggregator::instance().feed_wifi_scan(ap_count, strongest_rssi, 180, now_ms);
    }
    void feed_rf_interference(uint8_t level_q8, int16_t noise_dbm_q8, uint32_t now_ms) {
        SensorAggregator::instance().feed_rf_interference(level_q8, noise_dbm_q8, 200, now_ms);
    }
    void feed_posture(int8_t pitch, int8_t roll, bool raised, uint32_t now_ms) {
        SensorAggregator::instance().feed_posture(pitch, roll, raised, 200, now_ms);
        IntentEngine::instance().on_wrist_gesture(raised, now_ms);
    }

    void feed_text(const char* utterance, uint32_t now_ms) {
        IntentEngine::instance().parse_text(utterance, now_ms);
    }

    void inject_intent(const Intent& in, uint32_t now_ms) {
        IntentEngine::instance().inject(in, now_ms);
    }

    void tick(uint32_t now_ms) {
        ContextManager::instance().tick(now_ms);
        SessionMemory::instance().tick(now_ms);
        now_ms_ = now_ms;
    }

    unsigned process_events(unsigned max_events = 8) {
        return EventBus::instance().process(max_events);
    }

    const UserContext& context() const {
        return ContextManager::instance().get();
    }

    void set_action_hooks(const ActionHooks& h) {
        ActionExecutor::instance().set_hooks(h);
    }

    void set_manage_app_transitions(bool enable) {
        manage_app_transitions_ = enable;
    }

    bool manage_app_transitions() const { return manage_app_transitions_; }

    void set_wake_word(const char* word) {
        WakeWordConfig::instance().set(word);
    }

    const char* wake_word() const {
        return WakeWordConfig::instance().get();
    }

    SessionMemory& memory() {
        return SessionMemory::instance();
    }

    const SessionMemory& memory() const {
        return SessionMemory::instance();
    }

    void set_log_sink(FebruaryLogFn fn, void* user = nullptr) {
        FebruaryLog::set_sink(fn, user);
    }

    void speak(const char* msg, uint32_t now_ms = 0) {
        Action a;
        a.type = ActionType::Speak;
        if (msg) {
            unsigned i = 0;
            for (; msg[i] && i + 1 < sizeof(a.message); ++i) a.message[i] = msg[i];
            a.message[i] = '\0';
        }
        ActionExecutor::instance().execute(a, now_ms ? now_ms : now_ms_);
    }

private:
    FebruaryCore() = default;

    static void on_intent_static(const Event& ev, void* user) {
        static_cast<FebruaryCore*>(user)->on_intent(ev);
    }

    void on_intent(const Event& ev) {
        if (ev.type != EventType::IntentDetected &&
            ev.type != EventType::ProactiveTrigger) {
            return;
        }
        const Intent& in = ev.payload.intent;
        const UserContext& ctx = ContextManager::instance().get();

        Action speak_act;
        Persona::instance().reply_for_intent(in, ctx, speak_act);

#if FEBRUARY_ENABLE_PLANNER
        Plan plan;
        Planner::instance().plan_for(in, ctx, plan);
        for (unsigned i = 0; i < plan.count; ++i) {
            const PlanStep& step = plan.steps[i];
            if (step.type == ActionType::Speak) continue;
            if (step.type == ActionType::TransitionApp && !manage_app_transitions_) continue;
            Action act;
            Planner::step_to_action(step, act);
            ActionExecutor::instance().execute(act, ev.timestamp_ms);
        }
        if (speak_act.type == ActionType::Speak && speak_act.message[0]) {
            ActionExecutor::instance().execute(speak_act, ev.timestamp_ms);
        }
#else
        switch (in.type) {
        case IntentType::SetDoNotDisturb: {
            Action dnd;
            dnd.type = ActionType::SetDnd;
            dnd.arg0 = in.param0 ? 1 : 0;
            ActionExecutor::instance().execute(dnd, ev.timestamp_ms);
            break;
        }
        case IntentType::StartFitness:
        case IntentType::PromoteApp: {
            if (manage_app_transitions_) {
                Action tr;
                tr.type = ActionType::TransitionApp;
                tr.arg0 = kAppIdFitness;
                tr.arg1 = kAppStateForeground;
                ActionExecutor::instance().execute(tr, ev.timestamp_ms);
            }
            break;
        }
        default:
            break;
        }
        if (speak_act.type == ActionType::Speak && speak_act.message[0]) {
            ActionExecutor::instance().execute(speak_act, ev.timestamp_ms);
        }
#endif
    }

    bool     ready_  = false;
    bool     manage_app_transitions_ = true;
    uint32_t now_ms_ = 0;
};

}  // namespace february
}  // namespace aurora

#endif  // AURORA_FEBRUARY_CORE_HPP
