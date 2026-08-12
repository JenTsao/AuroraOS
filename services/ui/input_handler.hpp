#ifndef AURORAOS_UI_INPUT_HANDLER_HPP
#define AURORAOS_UI_INPUT_HANDLER_HPP

#include <stdint.h>

namespace auroraos {
namespace ui_service {

enum class InputEventType : uint8_t {
    TouchDown = 1,
    TouchUp = 2,
    TouchMove = 3,
    ButtonPress = 4,
    ButtonRelease = 5
};

struct InputEvent {
    InputEventType type;
    int x;
    int y;
    int key_code;
};

class InputHandler {
public:
    static InputHandler& instance() {
        static InputHandler handler;
        return handler;
    }

    void register_listener(uint32_t app_cap_id);
    void dispatch_event(const InputEvent& event);

private:
    InputHandler() = default;
    
    uint32_t active_listener_cap_ = 0;
};

} // namespace ui_service
} // namespace auroraos

#endif // AURORAOS_UI_INPUT_HANDLER_HPP
