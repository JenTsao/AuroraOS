#ifndef AURORA_UI_VIEW_GROUP_HPP
#define AURORA_UI_VIEW_GROUP_HPP

#include <stdint.h>

// Minimal stub: UI rendering abstractions for host-native unit tests.
// The real implementations live in the firmware UI stack.

// Global typedef — needed by notification_center.hpp which uses ColorRGB565
// without namespace qualification inside namespace aurora.
typedef uint16_t ColorRGB565;

namespace UI {

class UIRenderer {
public:
    virtual ~UIRenderer() = default;

    virtual void fill_round_rect(int32_t x, int32_t y, uint32_t w, uint32_t h,
                                 uint32_t radius, uint16_t color) {}

    virtual void fill_rect(int32_t x, int32_t y, uint32_t w, uint32_t h,
                           uint16_t color) {}

    virtual void draw_string(int16_t x, int16_t y, const char* text, uint8_t scale,
                             uint16_t fg, uint16_t bg,
                             const uint8_t* font_data, uint8_t font_w, uint8_t font_h) {}
};

class ViewGroup {
public:
    ViewGroup(int32_t x, int32_t y, uint32_t w, uint32_t h)
        : x_(x), y_(y), width_(w), height_(h) {}

    virtual ~ViewGroup() = default;

    ViewGroup(const ViewGroup&) = delete;
    ViewGroup& operator=(const ViewGroup&) = delete;

    virtual void draw(UIRenderer& renderer) { (void)renderer; }

    void invalidate() {}

protected:
    int32_t x_;
    int32_t y_;
    uint32_t width_;
    uint32_t height_;
};

} // namespace UI

#endif // AURORA_UI_VIEW_GROUP_HPP
