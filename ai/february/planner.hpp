/**
 * @file planner.hpp
 * @brief Fixed-depth light planner for February Phase 2.2
 *
 * One Intent -> ordered Action sequence (max FEBRUARY_PLANNER_MAX_STEPS).
 * Zero heap. Default rules live in a static PlanRule table; runtime may
 * swap the table pointer via set_rules().
 *
 * Persona still owns spoken text; planner owns side-effect ordering.
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
    ActionType  type    = ActionType::None;
    int32_t     arg0    = 0;
    int32_t     arg1    = 0;
    const char* message = nullptr;
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

    bool push(ActionType t, int32_t a0 = 0, int32_t a1 = 0,
              const char* msg = nullptr) {
        if (count >= kPlannerMaxSteps) {
            return false;
        }
        steps[count].type    = t;
        steps[count].arg0    = a0;
        steps[count].arg1    = a1;
        steps[count].message = msg;
        ++count;
        return true;
    }
};

struct PlanRule {
    IntentType  intent      = IntentType::None;
    bool        use_param0  = false;
    int32_t     param0      = 0;
    unsigned    n_steps     = 0;
    PlanStep    steps[kPlannerMaxSteps]{};
};

inline const PlanRule* default_plan_rules() {
    static const PlanRule kRules[] = {
        {IntentType::BatteryLow, false, 0, 2,
         {{ActionType::NotifyUser, 0, 0, "Battery low"},
          {ActionType::SetPower,
           static_cast<int32_t>(PowerMode::Critical), 0, nullptr}}},
        {IntentType::SetDoNotDisturb, false, 0, 1,
         {{ActionType::SetDnd, 0, 0, nullptr}}},
        {IntentType::StartFitness, false, 0, 1,
         {{ActionType::TransitionApp, 1, 1, nullptr}}},
        {IntentType::PromoteApp, false, 0, 1,
         {{ActionType::TransitionApp, 1, 1, nullptr}}},
        {IntentType::RemindRest, false, 0, 1,
         {{ActionType::NotifyUser, 0, 0, "Rest reminder"}}},
        {IntentType::QueryStatus, false, 0, 1, {{ActionType::Speak}}},
        {IntentType::QueryHealth, false, 0, 1, {{ActionType::Speak}}},
        {IntentType::Greeting, false, 0, 1, {{ActionType::Speak}}},
        {IntentType::Help, false, 0, 1, {{ActionType::Speak}}},
        {IntentType::UnknownCommand, false, 0, 1, {{ActionType::Speak}}},
        {IntentType::None, false, 0, 0, {}},
    };
    return kRules;
}

class Planner {
public:
    static Planner& instance() {
        static Planner p;
        return p;
    }

    void set_rules(const PlanRule* rules) {
        rules_ = rules ? rules : default_plan_rules();
    }

    const PlanRule* rules() const { return rules_; }

    unsigned plan_for(const Intent& in, const UserContext& ctx, Plan& out) {
        out.clear();
        (void)ctx;

        for (const PlanRule* r = rules_; r && r->intent != IntentType::None; ++r) {
            if (r->intent != in.type) {
                continue;
            }
            if (r->use_param0 && r->param0 != in.param0) {
                continue;
            }
            for (unsigned i = 0; i < r->n_steps && i < kPlannerMaxSteps; ++i) {
                PlanStep step = r->steps[i];
                if (step.type == ActionType::SetDnd && step.arg0 == 0 &&
                    step.message == nullptr) {
                    step.arg0 = in.param0 ? 1 : 0;
                }
                out.push(step.type, step.arg0, step.arg1, step.message);
            }
            return out.count;
        }

        if (in.valid()) {
            out.push(ActionType::Speak);
        }
        return out.count;
    }

    static void step_to_action(const PlanStep& step, Action& act) {
        act.clear();
        act.type = step.type;
        act.arg0 = step.arg0;
        act.arg1 = step.arg1;
        if (step.message) {
            copy_cstr(act.message, sizeof(act.message), step.message);
        }
    }

private:
    Planner() : rules_(default_plan_rules()) {}

    const PlanRule* rules_;
};

#else  // !FEBRUARY_ENABLE_PLANNER

struct PlanStep {
    ActionType type = ActionType::None;
};

struct Plan {
    void clear() {}
    unsigned count = 0;
};

struct PlanRule {
    IntentType intent = IntentType::None;
};

class Planner {
public:
    static Planner& instance() {
        static Planner p;
        return p;
    }
    void set_rules(const PlanRule*) {}
    unsigned plan_for(const Intent&, const UserContext&, Plan& out) {
        out.clear();
        return 0;
    }
    static void step_to_action(const PlanStep&, Action& act) { act.clear(); }
};

#endif  // FEBRUARY_ENABLE_PLANNER

}  // namespace february
}  // namespace aurora

#endif  // AURORA_FEBRUARY_PLANNER_HPP
