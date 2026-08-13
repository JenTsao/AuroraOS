#ifndef AURORAOS_UI_IPC_HPP
#define AURORAOS_UI_IPC_HPP

#include <stdint.h>

namespace auroraos {
namespace ui_service {

enum class UiOpcode : uint32_t {
    CreateWindow = 1,
    DestroyWindow = 2,
    DrawRect = 3,
    DrawText = 4,
    UpdateScreen = 5,
    RegisterInput = 6
};

struct WindowCreateReq {
    int x;
    int y;
    int width;
    int height;
    char title[16];
};

struct DrawRectReq {
    int win_id;
    int x;
    int y;
    int width;
    int height;
    uint32_t color; // e.g. RGB565 or ARGB8888
};

struct DrawTextReq {
    int win_id;
    int x;
    int y;
    uint32_t color;
    char text[32];
};

struct UiRequest {
    UiOpcode opcode;

    union {
        WindowCreateReq create_window;
        int destroy_window_id;
        DrawRectReq draw_rect;
        DrawTextReq draw_text;
    };
};

struct UiReply {
    int status; // 0 for success, negative for error. For CreateWindow, returns win_id.
};

} // namespace ui_service
} // namespace auroraos

#endif // AURORAOS_UI_IPC_HPP
