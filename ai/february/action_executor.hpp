/**
 * @file action_executor.hpp
 * @brief Executes Actions. Phase 1 uses callbacks / weak hooks so the
 *        rest of the OS can bind real implementations later (Capability,
 *        UI, TTS, app lifecycle …).
 */
#ifndef AURORA_FEBRUARY_ACTION_EXECUTOR_HPP
#define AURORA_FEBRUARY_ACTION_EXECUTOR_HPP

#include "types.hpp"
#include "event_bus.hpp"
#include "context_manager.hpp"

namespace aurora {
namespace february {

struct ActionHooks {
    void (*on_speak)(const char* msg, void* user) = nullptr;
    void (*on_notify)(const char* msg, void* user) = nullptr;
    void (*on_set_dnd)(bool enable, void* user) = nullptr;
    void (*on_transition_app)(int32_t app_id, int32_t state, void* user) = nullptr;
    void (*on_log)(const char* msg, void* user) = nullptr;
    void* user = nullptr;
};

class ActionExecutor {
public:
    static ActionExecutor& instance() {
        static ActionExecutor ex;
        return ex;
    }

    void set_hooks(const ActionHooks& h) { hooks_ = h; }

    bool execute(const Action& act, uint32_t now_ms = 0) {
        bool ok = true;
        switch (act.type) {
        case ActionType::Speak:
            if (hooks_.on_speak) {
                hooks_.on_speak(act.message, hooks_.user);
            }
            break;
        case ActionType::NotifyUser:
            if (hooks_.on_notify) {
                hooks_.on_notify(act.message, hooks_.user);
            }
            break;
        case ActionType::SetDnd:
            ContextManager::instance().set_dnd(act.arg0 != 0);
            if (hooks_.on_set_dnd) {
                hooks_.on_set_dnd(act.arg0 != 0, hooks_.user);
            }
            break;
        case ActionType::TransitionApp:
            if (hooks_.on_transition_app) {
                hooks_.on_transition_app(act.arg0, act.arg1, hooks_.user);
            }
            break;
        case ActionType::Log:
            if (hooks_.on_log) {
                hooks_.on_log(act.message, hooks_.user);
            }
            break;
        case ActionType::None:
        default:
            ok = false;
            break;
        }

        Event ev;
        ev.type = EventType::ActionExecuted;
        ev.timestamp_ms = now_ms;
        ev.payload.action = act;
        EventBus::instance().publish(ev);
        return ok;
    }

private:
    ActionExecutor() = default;
    ActionHooks hooks_{};
};

}  // namespace february
}  // namespace aurora

#endif  // AURORA_FEBRUARY_ACTION_EXECUTOR_HPP
