#ifndef AURORA_GUIX_WIDGET_BUTTON_HPP
#define AURORA_GUIX_WIDGET_BUTTON_HPP

#include "widget.hpp"

namespace auroraos {
namespace guix {

class Button;

typedef void (*ButtonClickHandler)(Button* btn, void* user_data);

class Button : public Widget {
public:
    Button(const char* label = nullptr);
    ~Button() override = default;

    void set_text(const char* label);
    const char* get_text() const { return text_; }

    void set_colors(uint16_t bg_color, uint16_t text_color, uint16_t pressed_color, uint16_t border_color = Color::White);
    void set_bg_color(uint16_t color) { bg_color_ = color; }
    void set_text_color(uint16_t color) { text_color_ = color; }
    void set_pressed_color(uint16_t color) { pressed_color_ = color; }
    void set_border_color(uint16_t color) { border_color_ = color; }

    void set_click_handler(ButtonClickHandler handler, void* user_data = nullptr);

    bool on_event(Window* win, const InputEvent& event) override;
    void paint(Window* win) override;

    bool is_pressed() const { return is_pressed_; }

private:
    char text_[32];
    uint16_t bg_color_;
    uint16_t text_color_;
    uint16_t pressed_color_;
    uint16_t border_color_;

    bool is_pressed_;
    ButtonClickHandler click_handler_;
    void* click_user_data_;
};

} // namespace guix
} // namespace auroraos

#endif // AURORA_GUIX_WIDGET_BUTTON_HPP
