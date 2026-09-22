#ifndef DISPLAY_IPC_HPP
#define DISPLAY_IPC_HPP

#include <stdint.h>

namespace auroraos {
namespace display {

enum class DisplayOpcode : uint32_t {
    GetInfo = 1,
    Clear = 2,
    DrawPixel = 3,
    FillRect = 4,
    DrawLine = 5,
    Present = 6
};

struct DisplayInfoReply {
    uint16_t width;
    uint16_t height;
    uint8_t bpp;
};

struct DrawPixelReq {
    int16_t x;
    int16_t y;
    uint16_t color; // RGB565
};

struct FillRectReq {
    int16_t x;
    int16_t y;
    uint16_t w;
    uint16_t h;
    uint16_t color;
};

struct DrawLineReq {
    int16_t x0;
    int16_t y0;
    int16_t x1;
    int16_t y1;
    uint16_t color;
};

struct DisplayRequest {
    DisplayOpcode opcode;

    union {
        uint16_t clear_color;
        DrawPixelReq draw_pixel;
        FillRectReq fill_rect;
        DrawLineReq draw_line;
    };
};

struct DisplayReply {
    int status;

    union {
        DisplayInfoReply info;
    };
};

extern int g_display_service_ep;

} // namespace display
} // namespace auroraos

#endif // DISPLAY_IPC_HPP
