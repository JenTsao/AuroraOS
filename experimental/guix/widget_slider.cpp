#include "widget_slider.hpp"
#include "window.hpp"
#include <algorithm>

namespace auroraos {
namespace guix {

Slider::Slider(int32_t min_val, int32_t max_val, int32_t cur_val)
    : min_val_(min_val),
      max_val_(max_val),
      value_(cur_val),
      track_color_(Color::DarkGray),
      active_color_(Color::Cyan),
      thumb_color_(Color::White),
      is_dragging_(false),
      value_handler_(nullptr),
      value_user_data_(nullptr) {
    if (max_val_ <= min_val_) {
        max_val_ = min_val_ + 1;
    }
    set_value(cur_val);
}

void Slider::set_range(int32_t min_val, int32_t max_val) {
    min_val_ = min_val;
    max_val_ = (max_val > min_val) ? max_val : min_val + 1;
    set_value(value_);
}

void Slider::set_value(int32_t val) {
    int32_t clamped = std::max(min_val_, std::min(max_val_, val));
    if (value_ != clamped) {
        value_ = clamped;
        if (value_handler_) {
            value_handler_(this, value_, value_user_data_);
        }
    }
}

void Slider::set_colors(uint16_t track_color, uint16_t active_color, uint16_t thumb_color) {
    track_color_ = track_color;
    active_color_ = active_color;
    thumb_color_ = thumb_color;
}

void Slider::set_value_changed_handler(SliderValueChangedHandler handler, void* user_data) {
    value_handler_ = handler;
    value_user_data_ = user_data;
}

void Slider::update_value_from_x(Window* win, int32_t window_x) {
    Rect wb = get_window_bounds();
    int32_t rel_x = window_x - wb.x;
    rel_x = std::max<int32_t>(0, std::min<int32_t>(wb.w, rel_x));

    int64_t range = static_cast<int64_t>(max_val_) - min_val_;
    int32_t new_val = static_cast<int32_t>(min_val_ + (range * rel_x) / (wb.w > 0 ? wb.w : 1));
    set_value(new_val);
    invalidate(win);
}

bool Slider::on_event(Window* win, const InputEvent& event) {
    if (!enabled_ || !visible_)
        return false;

    if (event.type == InputEventType::PointerDown) {
        is_dragging_ = true;
        update_value_from_x(win, event.x);
        return true;
    } else if (event.type == InputEventType::PointerMove) {
        if (is_dragging_) {
            update_value_from_x(win, event.x);
            return true;
        }
    } else if (event.type == InputEventType::PointerUp) {
        if (is_dragging_) {
            is_dragging_ = false;
            update_value_from_x(win, event.x);
            return true;
        }
    }
    return false;
}

void Slider::paint(Window* win) {
    if (!visible_ || !win || w_ == 0 || h_ == 0)
        return;

    Rect wb = get_window_bounds();

    // 1. 绘制轨道 (高 4px 的水平条)
    int32_t track_h = 4;
    int32_t track_y = wb.y + (wb.h - track_h) / 2;

    int64_t range = static_cast<int64_t>(max_val_) - min_val_;
    int64_t current = static_cast<int64_t>(value_) - min_val_;
    int32_t thumb_x = wb.x;
    if (range > 0 && wb.w > 0) {
        thumb_x = wb.x + static_cast<int32_t>((current * wb.w) / range);
    }
    thumb_x = std::max(wb.x, std::min(wb.x + static_cast<int32_t>(wb.w), thumb_x));

    // 激活段与未激活段
    if (thumb_x > wb.x) {
        win->fill_rect(wb.x, track_y, static_cast<uint32_t>(thumb_x - wb.x), track_h, active_color_);
    }
    if (thumb_x < wb.x + wb.w) {
        win->fill_rect(thumb_x, track_y, static_cast<uint32_t>(wb.x + wb.w - thumb_x), track_h, track_color_);
    }

    // 2. 绘制滑块旋钮 (Thumb Knob: 8x(h-2) 矩形或圆)
    int32_t thumb_w = 6;
    int32_t knob_x = thumb_x - thumb_w / 2;
    if (knob_x < wb.x) knob_x = wb.x;
    if (knob_x + thumb_w > wb.x + wb.w) knob_x = wb.x + wb.w - thumb_w;

    win->fill_rect(knob_x, wb.y + 1, thumb_w, wb.h - 2, thumb_color_);
    win->draw_rect(knob_x, wb.y + 1, thumb_w, wb.h - 2, Color::Black);
}

} // namespace guix
} // namespace auroraos
