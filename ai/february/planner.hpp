/**
 * @file planner.hpp
 * @brief Fixed-depth light planner for February Phase 2
 *
 * One Intent → ordered Action sequence (max FEBRUARY_PLANNER_MAX_STEPS).
 * Zero heap. Rules are compile-time tables; runtime can swap table pointer.
 *
 * Cross-device: sequences may include PublishEvent / custom remote steps;
 * execution still goes through ActionExecutor / CapabilityHooks.
 */
#ifndef AURORA_FEBRUARY_PLANNER_HPP
#define AURORA_FEBRUARY_PLANNER_HPP

#include "config.hpp"
#include "types.hpp"
#include "string_util.hpp"

namespace aurora {
namespace february {

#if FEBRUARY_ENABLE_PLANNER

constexpr unsigned kPlannerMaxSteps = FEBRUARY_PLANNER_MAX_STEPS;

struct PlanStep {
    ActionType type = ActionType::None;
    int32_t    arg0 = 0;
    int32_t    arg1 = 0;
    const char* message = nullptr;  // static string or null
};

struct Plan {
    PlanStep steps[kPlannerMaxSteps]{};
    unsigned count = 0;

    void clear() {
        count = 0;
        for (unsigned i = 0; i < kPlannerMaxSteps; ++i) {
            steps[i] = PlanStep{};
        }
    }

    bool push(ActionType t, int32_t a0 = 0, int32_t a1 = 0, const char* msg = nullptr) {
        if (count >= kPlannerMaxSteps) {
            return false;
        }
        steps[count].type = t;
        steps[count].arg0 = a0;
        steps[count].arg1 = a1;
        steps[count].message = msg;
        ++count;
        return true;
    }
};

/**
 * Map IntentType → default multi-step plan.
 * Persona still owns spoken text; planner owns side-effect ordering.
 */
class Planner {
public:
    static Planner& instance() {
        static Planner p;
        return p;
    }

    /** Build plan for intent + context. Returns step count. */
    unsigned plan_for(const Intent& in, const UserContext& ctx, Plan& out) {
        out.clear();
        (void)ctx;

        switch (in.type) {
        case IntentType::BatteryLow:
            // Notify user, then suggest power mode (arg0 = Critical ordinal)
            out.push(ActionType::NotifyUser, 0, 0, "Battery low");
            out.push(ActionType::SetPower, static_cast<int32_t>(PowerMode::Critical));
            break;

        case IntentType::SetDoNotDisturb:
            out.push(ActionType::SetDnd, in.param0 ? 1 : 0);
            break;

        case IntentType::StartFitness:
        case IntentType::PromoteApp:
            out.push(ActionType::TransitionApp, 1 /*fitness*/, 1 /*foreground*/);
            break;

        case IntentType::RemindRest:
            out.push(ActionType::NotifyUser, 0, 0, "Rest reminder");
            break;

        case IntentType::QueryStatus:
        case IntentType::QueryHealth:
        case IntentType::Greeting:
        case IntentType::Help:
        case IntentType::UnknownCommand:
            // Speak-only; Persona fills message in core
            out.push(ActionType::Speak);
            break;

        default:
            if (in.valid()) {
                out.push(ActionType::Speak);
            }
            break;
        }
        return out.count;
    }

    /** Materialize PlanStep into Action (copies static message if present). */
    static void step_to_action(const PlanStep& step, Action& act) {
        act.clear();
        act.type = step.type;
        act.arg0 = step.arg0;
        act.arg1 = step.arg1;
        if (step.message) {
            copy_cstr(act.message, sizeof(act.message), step.message);
        }
    }
};

#else  // !FEBRUARY_ENABLE_PLANNER

struct Plan {
    void clear() {}
    unsigned count = 0;
};

class Planner {
public:
    static Planner& instance() {
        static Planner p;
        return p;
    }
    unsigned plan_for(const Intent&, const UserContext&, Plan& out) {
        out.clear();
        return 0;
    }
};

#endif  // FEBRUARY_ENABLE_PLANNER

}  // namespace february
}  // namespace aurora

#endif  // AURORA_FEBRUARY_PLANNER_HPP
