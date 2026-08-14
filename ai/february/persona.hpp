/**
 * @file persona.hpp
 * @brief February persona — voice, tone, fixed replies
 */
#ifndef AURORA_FEBRUARY_PERSONA_HPP
#define AURORA_FEBRUARY_PERSONA_HPP

#include "types.hpp"
#include <cstdio>
#include <cstdarg>

namespace aurora {
namespace february {

enum class PersonaTone : uint8_t {
    Calm = 0,
    Friendly,
    Professional,
    Minimal
};

class Persona {
public:
    static Persona& instance() {
        static Persona p;
        return p;
    }

    void set_name(const char* name) {
        if (!name) return;
        std::snprintf(name_, sizeof(name_), "%s", name);
    }

    const char* name() const { return name_; }

    void set_tone(PersonaTone t) { tone_ = t; }
    PersonaTone tone() const { return tone_; }

    int reply_for_intent(const Intent& intent, const UserContext& ctx, Action& out) {
        out.clear();
        out.type = ActionType::Speak;

        switch (intent.type) {
        case IntentType::Greeting:
            return fmt(out, "%s online. How can I help?", name());
        case IntentType::QueryStatus:
            return fmt(out, "Steps %u, HR %u, battery %u%%.",
                       (unsigned)ctx.steps, (unsigned)ctx.heart_rate,
                       (unsigned)ctx.battery_pct);
        case IntentType::QueryHealth:
            return fmt(out, "Activity %s, heart rate %u.",
                       activity_str(ctx.activity), (unsigned)ctx.heart_rate);
        case IntentType::RemindRest:
            return fmt(out, "You have been idle. Consider a short walk.");
        case IntentType::BatteryLow:
            return fmt(out, "Battery at %u percent. Consider charging.",
                       (unsigned)ctx.battery_pct);
        case IntentType::Help:
            return fmt(out, "I can check status, manage focus mode, and watch your health.");
        case IntentType::SetDoNotDisturb:
            return fmt(out, intent.param0 ? "Do-not-disturb enabled."
                                          : "Do-not-disturb cleared.");
        case IntentType::UnknownCommand:
            return fmt(out, "I did not understand. Try asking for status or help.");
        default:
            return fmt(out, "Acknowledged.");
        }
    }

    int proactive_message(IntentType t, const UserContext& ctx, Action& out) {
        Intent tmp;
        tmp.type = t;
        return reply_for_intent(tmp, ctx, out);
    }

private:
    Persona() {
        std::snprintf(name_, sizeof(name_), "February");
        tone_ = PersonaTone::Calm;
    }

    static const char* activity_str(ActivityState s) {
        switch (s) {
        case ActivityState::Idle:      return "idle";
        case ActivityState::Walking:   return "walking";
        case ActivityState::Running:   return "running";
        case ActivityState::Sleeping:  return "sleeping";
        case ActivityState::Working:   return "working";
        case ActivityState::Exercising:return "exercising";
        default:                       return "unknown";
        }
    }

    int fmt(Action& out, const char* fmt, ...) {
        va_list ap;
        va_start(ap, fmt);
        int n = std::vsnprintf(out.message, sizeof(out.message), fmt, ap);
        va_end(ap);
        if (n < 0) {
            out.message[0] = '\0';
            return 0;
        }
        if (static_cast<size_t>(n) >= sizeof(out.message)) {
            n = static_cast<int>(sizeof(out.message) - 1);
        }
        return n;
    }

    char        name_[16];
    PersonaTone tone_;
};

}  // namespace february
}  // namespace aurora

#endif  // AURORA_FEBRUARY_PERSONA_HPP
