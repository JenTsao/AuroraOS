/**
 * @file intent_engine.hpp
 * @brief Rule-based + sensor-driven Intent Engine (February Phase 1)
 */
#ifndef AURORA_FEBRUARY_INTENT_ENGINE_HPP
#define AURORA_FEBRUARY_INTENT_ENGINE_HPP

#include "types.hpp"
#include "context_manager.hpp"
#include "event_bus.hpp"
#include "wake_word.hpp"

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

    void on_steps(uint32_t steps, uint32_t now_ms) {
        ContextManager::instance().update_steps(steps, now_ms);
        const UserContext& ctx = ContextManager::instance().get();

        if (ctx.steps_delta >= kFitnessStepDeltaThreshold) {
            if (last_fitness_ms_ == 0 ||
                now_ms - last_fitness_ms_ >= kFitnessCooldownMs) {
                emit(IntentType::StartFitness, 800, now_ms);
                last_fitness_ms_ = now_ms;
            }
        }

        if (ctx.idle_seconds > kRestIdleSeconds &&
            ctx.activity == ActivityState::Idle) {
            if (last_rest_remind_ms_ == 0 ||
                now_ms - last_rest_remind_ms_ >= kRestCooldownMs) {
                emit(IntentType::RemindRest, 700, now_ms);
                last_rest_remind_ms_ = now_ms;
            }
        }
    }

    void on_battery(uint8_t pct, uint32_t now_ms) {
        ContextManager::instance().set_battery(pct);
        if (pct <= kBatteryLowPct) {
            if (!battery_low_latched_) {
                emit(IntentType::BatteryLow, 900, now_ms);
                battery_low_latched_ = true;
            }
        } else {
            battery_low_latched_ = false;
        }
    }

    void on_heart_rate(uint16_t hr, uint32_t /*now_ms*/) {
        ContextManager::instance().set_heart_rate(hr);
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

        if (WakeWordConfig::instance().configured() &&
            !WakeWordConfig::instance().matches(utterance)) {
            in.type = IntentType::UnknownCommand;
            in.confidence_x1000 = 100;
            return in;
        }

        if (contains_ci(utterance, "status") || contains_ci(utterance, "how am i")) {
            in.type = IntentType::QueryStatus;
            in.confidence_x1000 = 900;
        } else if (contains_ci(utterance, "health") || contains_ci(utterance, "heart")) {
            in.type = IntentType::QueryHealth;
            in.confidence_x1000 = 850;
        } else if (contains_ci(utterance, "cancel dnd") ||
                   contains_ci(utterance, "disable dnd") ||
                   contains_ci(utterance, "dnd off") ||
                   contains_ci(utterance, "clear focus")) {
            in.type = IntentType::SetDoNotDisturb;
            in.confidence_x1000 = 850;
            in.param0 = 0;
        } else if (contains_ci(utterance, "dnd") ||
                   contains_ci(utterance, "do not disturb") ||
                   contains_ci(utterance, "focus mode") ||
                   contains_ci(utterance, "focus")) {
            in.type = IntentType::SetDoNotDisturb;
            in.confidence_x1000 = 800;
            in.param0 = 1;
        } else if (contains_ci(utterance, "help")) {
            in.type = IntentType::Help;
            in.confidence_x1000 = 950;
        } else if (contains_ci(utterance, "hello") || is_hi(utterance) ||
                   (WakeWordConfig::instance().configured() &&
                    WakeWordConfig::instance().matches(utterance))) {
            in.type = IntentType::Greeting;
            in.confidence_x1000 = 900;
        } else {
            in.type = IntentType::UnknownCommand;
            in.confidence_x1000 = 300;
        }

        unsigned i = 0;
        for (; utterance[i] && i + 1 < sizeof(in.text); ++i) {
            in.text[i] = utterance[i];
        }
        in.text[i] = '\0';

        emit(in, now_ms);
        return in;
    }

    void inject(const Intent& in, uint32_t now_ms) {
        emit(in, now_ms);
    }

private:
    IntentEngine() = default;

    void emit(IntentType t, uint32_t conf, uint32_t now_ms) {
        Intent in;
        in.type = t;
        in.confidence_x1000 = conf;
        emit(in, now_ms);
    }

    void emit(const Intent& in, uint32_t now_ms) {
        Event ev;
        ev.type = EventType::IntentDetected;
        ev.timestamp_ms = now_ms;
        ev.payload.intent = in;
        EventBus::instance().publish(ev);
    }

    static char tolower_ascii(char c) {
        if (c >= 'A' && c <= 'Z') return static_cast<char>(c - 'A' + 'a');
        return c;
    }

    static bool contains_ci(const char* hay, const char* needle) {
        if (!hay || !needle || !*needle) return false;
        for (const char* p = hay; *p; ++p) {
            const char* h = p;
            const char* n = needle;
            while (*h && *n && tolower_ascii(*h) == tolower_ascii(*n)) {
                ++h;
                ++n;
            }
            if (!*n) return true;
        }
        return false;
    }

    static bool is_hi(const char* s) {
        if (!s) return false;
        if (tolower_ascii(s[0]) != 'h' || tolower_ascii(s[1]) != 'i') return false;
        return s[2] == '\0' || s[2] == ' ' || s[2] == ',' || s[2] == '!' || s[2] == '.';
    }

    uint32_t last_rest_remind_ms_ = 0;
    uint32_t last_fitness_ms_     = 0;
    bool     battery_low_latched_ = false;
};

}  // namespace february
}  // namespace aurora

#endif  // AURORA_FEBRUARY_INTENT_ENGINE_HPP
