#include "ui_ipc.hpp"
#include "window_manager.hpp"
#include "input_handler.hpp"
#include "../../syscall/syscall.hpp"
#include "../../drivers/display/renderer2d.hpp"

// We need a dummy FB for the UI service if it doesn't have one globally linked.
// The real implementation connects to the display driver.
extern "C" {
    void ui_service_entry();
}

namespace auroraos {
namespace ui_service {

// Global Endpoint ID for UI Service
int g_ui_service_ep = 5; // Placeholder capability ID

} // namespace ui_service
} // namespace auroraos

extern "C" void ui_service_entry() {
    using namespace auroraos::ui_service;
    
    uint32_t ep_cap = g_ui_service_ep;

    while (true) {
        struct {
            uint32_t msg_type;
            UiRequest req;
        } ipc_msg;
        
        uint32_t caller_cap = 0;
        sys_ipc_receive(ep_cap, &ipc_msg, sizeof(ipc_msg), &caller_cap);
        
        UiReply reply;
        reply.status = -1;

        if (ipc_msg.msg_type == 1) { // UI Request
            switch (ipc_msg.req.opcode) {
                case UiOpcode::CreateWindow: {
                    reply.status = WindowManager::instance().create_window(
                        ipc_msg.req.create_window.x,
                        ipc_msg.req.create_window.y,
                        ipc_msg.req.create_window.width,
                        ipc_msg.req.create_window.height,
                        ipc_msg.req.create_window.title,
                        caller_cap
                    );
                    break;
                }
                case UiOpcode::DestroyWindow: {
                    bool success = WindowManager::instance().destroy_window(
                        ipc_msg.req.destroy_window_id, 
                        caller_cap
                    );
                    reply.status = success ? 0 : -1;
                    break;
                }
                case UiOpcode::DrawRect: {
                    Window* win = WindowManager::instance().get_window(ipc_msg.req.draw_rect.win_id);
                    if (win && win->owner_cap == caller_cap) {
                        // Forward to Renderer (clip to window bounds)
                        reply.status = 0;
                    }
                    break;
                }
                case UiOpcode::DrawText: {
                    Window* win = WindowManager::instance().get_window(ipc_msg.req.draw_text.win_id);
                    if (win && win->owner_cap == caller_cap) {
                        // Forward to Renderer (clip to window bounds)
                        reply.status = 0;
                    }
                    break;
                }
                case UiOpcode::UpdateScreen: {
                    WindowManager::instance().render_all();
                    reply.status = 0;
                    break;
                }
                case UiOpcode::RegisterInput: {
                    InputHandler::instance().register_listener(caller_cap);
                    reply.status = 0;
                    break;
                }
            }
        }
        
        sys_ipc_reply(caller_cap, &reply, sizeof(reply));
    }
}


