#include "input_handler.hpp"
#include "syscall.hpp"

namespace auroraos {
namespace ui_service {

void InputHandler::register_listener(uint32_t app_cap_id) {
    active_listener_cap_ = app_cap_id;
}

void InputHandler::dispatch_event(const InputEvent& event) {
    if (active_listener_cap_ != 0) {
        // Send async IPC message to the listener
        // The message type could be designated for input events
        struct {
            uint32_t type;
            InputEvent event;
        } input_msg;
        
        input_msg.type = 2; // Input Event type
        input_msg.event = event;
        
        // This should ideally be an asynchronous send, or a non-blocking send,
        // so the UI Service doesn't hang if the app is unresponsive.
        // For now we assume sys_ipc_send exists or we use a call with short timeout.
        uint32_t dummy_res;
        sys_ipc_call(active_listener_cap_, &input_msg, sizeof(input_msg), &dummy_res, sizeof(dummy_res));
    }
}

} // namespace ui_service
} // namespace auroraos
