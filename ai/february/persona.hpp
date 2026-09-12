/**
 * @file persona.hpp
 * @brief February persona — voice, tone, fixed replies
 *
 * Zero-libc: replies are built with a tiny local formatter (append_str /
 * append_uint) instead of vsnprintf. This keeps February linkable on
 * Cortex-M0+ targets where a full stdio printf pulls in ~8-10 KB and is often
 * stripped from newlib-nano, and it removes the va_list / <cstdarg> dependency.
 */
#ifndef AURORA_FEBRUARY_PERSONA_HPP
#define AURORA_FEBRUARY_PERSONA_HPP

#include "types.hpp"
#include "string_util.hpp"

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
    static Persona& instance();

    void set_name(const char* name) {
        if (!name) return;
        copy_cstr(name_, sizeof(name_), name);
    }

    const char* name() const { return name_; }

    void set_tone(PersonaTone t) { tone_ = t; }
    PersonaTone tone() const { return tone_; }

    int reply_for_intent(const Intent& intent, const UserContext& ctx, Action& out) {
        out.clear();
        out.type = ActionType::Speak;

        switch (intent.type) {
        case IntentType::Greeting:
            return begin(out, name()) && append_str(out, " online. How can I help?");
        case IntentType::QueryStatus:
            return begin(out, "Steps ") && append_uint(out, ctx.steps) &&
                   append_str(out, ", HR ") && append_uint(out, ctx.heart_rate) &&
                   append_str(out, ", battery ") && append_uint(out, ctx.battery_pct) &&
                   append_str(out, "%.");
        case IntentType::QueryHealth:
            return begin(out, "Activity ") && append_str(out, activity_str(ctx.activity)) &&
                   append_str(out, ", heart rate ") && append_uint(out, ctx.heart_rate) &&
                   append_str(out, ".");
        case IntentType::RemindRest:
            return begin(out, "You have been idle. Consider a short walk.");
        case IntentType::BatteryLow:
            return begin(out, "Battery at ") && append_uint(out, ctx.battery_pct) &&
                   append_str(out, " percent. Consider charging.");
        case IntentType::Emergency:
            return begin(out, "Emergency detected. Alerting your emergency contact.");
        case IntentType::Help:
            return begin(out, "I can check status, manage focus mode, and watch your health.");
        case IntentType::SetDoNotDisturb:
            return begin(out, intent.param0 ? "Do-not-disturb enabled."
                                            : "Do-not-disturb cleared.");
        case IntentType::UnknownCommand:
            return begin(out, "I did not understand. Try asking for status or help.");
        default:
            return begin(out, "Acknowledged.");
        }
    }

    int proactive_message(IntentType t, const UserContext& ctx, Action& out) {
        Intent tmp;
        tmp.type = t;
        return reply_for_intent(tmp, ctx, out);
    }

private:
    constexpr Persona() : name_{}, tone_(PersonaTone::Calm) {}

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

    // --- tiny zero-libc string builder -------------------------------------

    static unsigned len(const char* s) {
        unsigned n = 0;
        if (s) { while (s[n]) ++n; }
        return n;
    }

    /** Reset `out.message` for writing. Always returns true (chainable). */
    static bool begin(Action& out, const char* text) {
        return append_str_at(out, 0, text);
    }

    static bool append_str(Action& out, const char* text) {
        return append_str_at(out, len(out.message), text);
    }

    static bool append_str_at(Action& out, unsigned at, const char* text) {
        if (at >= sizeof(out.message)) {
            out.message[sizeof(out.message) - 1] = '\0';
            return false;
        }
        unsigned i = at;
        if (text) {
            for (unsigned j = 0; text[j] && i + 1 < sizeof(out.message); ++j) {
                out.message[i++] = text[j];
            }
        }
        out.message[i] = '\0';
        return true;
    }

    static bool append_uint(Action& out, uint32_t v) {
        char tmp[10];
        unsigned n = 0;
        if (v == 0) {
            tmp[n++] = '0';
        } else {
            while (v > 0 && n < sizeof(tmp)) {
                tmp[n++] = static_cast<char>('0' + (v % 10u));
                v /= 10u;
            }
        }
        return append_reversed(out, tmp, n);
    }

    static bool append_reversed(Action& out, const char* digits, unsigned n) {
        unsigned i = len(out.message);
        if (i >= sizeof(out.message)) {
            out.message[sizeof(out.message) - 1] = '\0';
            return false;
        }
        for (unsigned k = n; k > 0 && i + 1 < sizeof(out.message); --k) {
            out.message[i++] = digits[k - 1];
        }
        out.message[i] = '\0';
        return true;
    }

    // Out-of-line singleton storage: declared here, defined below the class.
    // Using a static data member instead of a function-local static avoids the
    // __cxa_guard_acquire/__cxa_guard_release pair that a function-local static
    // would emit (not ISR-safe, and pulls in libsupc++).
    static Persona storage_;

    char        name_[16];
    PersonaTone tone_;
};

inline Persona Persona::storage_{};

inline Persona& Persona::instance() {
    return storage_;
}

}  // namespace february
}  // namespace aurora

#endif  // AURORA_FEBRUARY_PERSONA_HPP
