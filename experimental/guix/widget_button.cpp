#include "widget_button.hpp"
#include "window.hpp"
#include <string.h>
#include <algorithm>

namespace auroraos {
namespace guix {

Button::Button(const char* label)
    : bg_color_(Color::DarkGray),
      text_color_(Color::White),
      pressed_color_(Color::Blue),
      border_color_(Color::LightGray),
      is_pressed_(false),
      click_handler_(nullptr),
      click_user_data_(nullptr) {
    set_text(label);
}

void Button::set_text(const char* label) {
    if (!label) {
        text_[0] = '\0';
        return;
    }
    size_t i = 0;
    while (label[i] && i < sizeof(text_) - 1) {
        text_[i] = label[i];
        ++i;
    }
    text_[i] = '\0';
}

void Button::set_colors(uint16_t bg_color, uint16_t text_color, uint16_t pressed_color, uint16_t border_color) {
    bg_color_ = bg_color;
    text_color_ = text_color;
    pressed_color_ = pressed_color;
    border_color_ = border_color;
}

void Button::set_click_handler(ButtonClickHandler handler, void* user_data) {
    click_handler_ = handler;
    click_user_data_ = user_data;
}

bool Button::on_event(Window* win, const InputEvent& event) {
    if (!enabled_ || !visible_)
        return false;

    if (event.type == InputEventType::PointerDown) {
        is_pressed_ = true;
        invalidate(win);
        return true;
    } else if (event.type == InputEventType::PointerUp) {
        if (is_pressed_) {
            is_pressed_ = false;
            invalidate(win);

            // 触发点击回调
            if (click_handler_) {
                click_handler_(this, click_user_data_);
            }
            return true;
        }
    } else if (event.type == InputEventType::PointerMove) {
        // 如果移出当前按钮边界，取消按下状态
        Rect wb = get_window_bounds();
        bool inside = wb.contains(event.x, event.y);
        if (is_pressed_ != inside) {
            is_pressed_ = inside;
            invalidate(win);
        }
        return is_pressed_;
    }
    return false;
}

void Button::paint(Window* win) {
    if (!visible_ || !win || w_ == 0 || h_ == 0)
        return;

    Rect wb = get_window_bounds();
    uint16_t current_bg = is_pressed_ ? pressed_color_ : bg_color_;

    // 绘制主体背景
    win->fill_rect(wb.x, wb.y, wb.w, wb.h, current_bg);

    // 绘制外边框
    win->draw_rect(wb.x, wb.y, wb.w, wb.h, border_color_);

    // 居中绘制文本
    if (text_[0] != '\0') {
        size_t len = strlen(text_);
        int32_t text_width = static_cast<int32_t>(len * 6); // 5x7 font + 1px spacing
        int32_t text_height = 8;

        int32_t text_x = wb.x + (wb.w - text_width) / 2;
        int32_t text_y = wb.y + (wb.h - text_height) / 2;

        if (text_x < wb.x) text_x = wb.x + 2;
        if (text_y < wb.y) text_y = wb.y + 2;

        win->draw_text(static_cast<uint32_t>(text_x), static_cast<uint32_t>(text_y),
                       text_, text_color_, current_bg, true, 1);
    }
}

} // namespace guix
} // namespace auroraos
