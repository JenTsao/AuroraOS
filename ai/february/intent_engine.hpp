/**
 * @file intent_engine.hpp
 * @brief Rule-based + sensor-driven Intent Engine (February Phase 1)
 *
 * Text intents: static IntentRule table (intent_rules.hpp).
 * Proactive: CooldownGate + LevelLatch (cooldown.hpp).
 * Emits via EventBus; SessionMemory records last intent.
 */
#ifndef AURORA_FEBRUARY_INTENT_ENGINE_HPP
#define AURORA_FEBRUARY_INTENT_ENGINE_HPP

#include "types.hpp"
#include "context_manager.hpp"
#include "event_bus.hpp"
#include "wake_word.hpp"
#include "intent_rules.hpp"
#include "cooldown.hpp"
#include "memory.hpp"
#include "log.hpp"
#include "string_util.hpp"

namespace aurora {
namespace february {

constexpr uint32_t kFitnessStepDeltaThreshold = 50;
constexpr uint32_t kFitnessCooldownMs         = 60000;
constexpr uint32_t kRestIdleSeconds           = 45;
constexpr uint32_t kRestCooldownMs            = 120000;
constexpr uint8_t  kBatteryLowPct             = 15;

class IntentEngine {
public:
    static IntentEngine& instance() {
        static IntentEngine eng;
        return eng;
    }

    /** Optional: replace default rule table (must be null-terminated). */
    void set_rules(const IntentRule* rules) {
        rules_ = rules ? rules : default_intent_rules();
    }

    void on_steps(uint32_t steps, uint32_t now_ms) {
        ContextManager::instance().update_steps(steps, now_ms);
        const UserContext& ctx = ContextManager::instance().get();

        if (ctx.steps_delta >= kFitnessStepDeltaThreshold) {
            if (fitness_cd_.try_fire(now_ms)) {
                FEBRUARY_LOG("intent: StartFitness");
                emit(IntentType::StartFitness, 800, now_ms);
            }
        }

        if (ctx.idle_seconds > kRestIdleSeconds &&
            ctx.activity == ActivityState::Idle) {
            if (rest_cd_.try_fire(now_ms)) {
                FEBRUARY_LOG("intent: RemindRest");
                emit(IntentType::RemindRest, 700, now_ms);
            }
        }
    }

    void on_battery(uint8_t pct, uint32_t now_ms) {
        ContextManager::instance().set_battery(pct);
        if (battery_latch_.rising(pct <= kBatteryLowPct)) {
            FEBRUARY_LOG("intent: BatteryLow");
            emit(IntentType::BatteryLow, 900, now_ms);
        }
    }

    void on_heart_rate(uint16_t hr, uint32_t /*now_ms*/) {
        ContextManager::instance().set_heart_rate(hr);
    }

    void on_wrist_gesture(bool raised, uint32_t now_ms) {
        ContextManager::instance().set_wrist_raised(raised);
        if (raised && wrist_latch_.rising(true)) {
            FEBRUARY_LOG("intent: WristRaised");
            emit(IntentType::Greeting, 600, now_ms);
        } else if (!raised) {
            wrist_latch_.reset();
        }
    }

    Intent parse_text(const char* utterance, uint32_t now_ms) {
        Intent in;
        in.source_id = 1;
        if (!utterance || !*utterance) {
            in.type = IntentType::UnknownCommand;
            in.confidence_x1000 = 100;
            emit(in, now_ms);
            return in;
        }

        // Wake-word gate
        if (WakeWordConfig::instance().configured() &&
            !WakeWordConfig::instance().matches(utterance)) {
            in.type = IntentType::UnknownCommand;
            in.confidence_x1000 = 100;
            FEBRUARY_LOGV("intent: wake gate drop");
            return in;  // ignore background speech
        }

        // Rule table scan (first match wins)
        bool matched = false;
        for (const IntentRule* r = rules_; r && r->keyword; ++r) {
            if (contains_ci(utterance, r->keyword)) {
                // Special-case bare "hi" → only if token-like
                if (r->keyword[0] == 'h' && r->keyword[1] == 'i' &&
                    r->keyword[2] == '\0' && !is_hi(utterance)) {
                    continue;
                }
                in.type = r->type;
                in.confidence_x1000 = r->confidence_x1000;
                in.param0 = r->param0;
                matched = true;
                break;
            }
        }

        if (!matched) {
            // Bare wake word alone → Greeting
            if (WakeWordConfig::instance().configured() &&
                WakeWordConfig::instance().matches(utterance)) {
                in.type = IntentType::Greeting;
                in.confidence_x1000 = 900;
            } else {
                in.type = IntentType::UnknownCommand;
                in.confidence_x1000 = 300;
            }
        }

        unsigned i = 0;
        for (; utterance[i] && i + 1 < sizeof(in.text); ++i) {
            in.text[i] = utterance[i];
        }
        in.text[i] = '\0';

        FEBRUARY_LOG("intent: text matched");
        emit(in, now_ms);
        return in;
    }

    void inject(const Intent& in, uint32_t now_ms) {
        emit(in, now_ms);
    }

    void reset_proactive() {
        fitness_cd_.reset();
        rest_cd_.reset();
        battery_latch_.reset();
        wrist_latch_.reset();
    }

private:
    IntentEngine()
        : rules_(default_intent_rules()),
          fitness_cd_(kFitnessCooldownMs),
          rest_cd_(kRestCooldownMs) {}

    void emit(IntentType t, uint32_t conf, uint32_t now_ms) {
        Intent in;
        in.type = t;
        in.confidence_x1000 = conf;
        emit(in, now_ms);
    }

    void emit(const Intent& in, uint32_t now_ms) {
        SessionMemory::instance().note_intent(in, now_ms);
        Event ev;
        ev.type = EventType::IntentDetected;
        ev.timestamp_ms = now_ms;
        ev.payload.intent = in;
        EventBus::instance().publish(ev);
    }

    static bool is_hi(const char* s) {
        if (!s) return false;
        if (tolower_ascii(s[0]) != 'h' || tolower_ascii(s[1]) != 'i') return false;
        return s[2] == '\0' || s[2] == ' ' || s[2] == ',' || s[2] == '!' || s[2] == '.';
    }

    const IntentRule* rules_;
    CooldownGate fitness_cd_;
    CooldownGate rest_cd_;
    LevelLatch   battery_latch_;
    LevelLatch   wrist_latch_;
};

}  // namespace february
}  // namespace aurora

#endif  // AURORA_FEBRUARY_INTENT_ENGINE_HPP
