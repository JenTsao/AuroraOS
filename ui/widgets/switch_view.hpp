#ifndef AURORA_UI_SWITCH_VIEW_HPP
#define AURORA_UI_SWITCH_VIEW_HPP

#include "../view.hpp"

namespace UI {

// ========================================================
// SwitchView: 胶囊型开关控件 (Toggle Switch)
//
// 专为可穿戴与 IoT 设置页设计 (蓝牙开/关、Wi-Fi 开/关等)。
// 尺寸自适应，支持点击与滑动手势切换，包含按压反馈与状态监听。
// ========================================================
class SwitchView : public View {
public:
    using OnCheckedChangedCallback = void (*)(SwitchView* sw, bool checked, void* ctx);

private:
    bool checked_;
    ColorRGB565 track_on_color_;
    ColorRGB565 track_off_color_;
    ColorRGB565 thumb_color_;
    OnCheckedChangedCallback on_checked_changed_;
    void* callback_ctx_;

public:
    SwitchView(int16_t x, int16_t y, uint16_t w = 50, uint16_t h = 28,
               bool initial_checked = false,
               ColorRGB565 on_color = 0x07E0 /* green */,
               ColorRGB565 off_color = 0x39E7 /* dark gray */,
               ColorRGB565 thumb = 0xFFFF /* white */)
        : View(x, y, w, h),
          checked_(initial_checked),
          track_on_color_(on_color),
          track_off_color_(off_color),
          thumb_color_(thumb),
          on_checked_changed_(nullptr),
          callback_ctx_(nullptr) {}

    bool is_checked() const noexcept {
        return checked_;
    }

    void set_checked(bool checked, bool trigger_callback = true) {
        if (checked_ != checked) {
            checked_ = checked;
            invalidate();
            if (trigger_callback && on_checked_changed_) {
                on_checked_changed_(this, checked_, callback_ctx_);
            }
        }
    }

    void toggle() {
        set_checked(!checked_);
    }

    void set_on_checked_changed_listener(OnCheckedChangedCallback cb, void* ctx) noexcept {
        on_checked_changed_ = cb;
        callback_ctx_ = ctx;
    }

    void set_colors(ColorRGB565 on_color, ColorRGB565 off_color, ColorRGB565 thumb_color) noexcept {
        track_on_color_ = on_color;
        track_off_color_ = off_color;
        thumb_color_ = thumb_color;
        invalidate();
    }

    void draw(UIRenderer& renderer) override {
        if (visibility_ != Visibility::VISIBLE)
            return;

        const uint16_t radius = height_ / 2;
        const ColorRGB565 track_color = checked_ ? track_on_color_ : track_off_color_;

        // 1. 绘制胶囊外轨
        renderer.fill_round_rect(x_, y_, width_, height_, radius, track_color);

        // 2. 绘制圆形滑块
        const uint16_t thumb_margin = (height_ > 8) ? 3 : 1;
        const uint16_t thumb_radius = (radius > thumb_margin) ? (radius - thumb_margin) : 1;
        const int16_t cy = y_ + radius;
        const int16_t cx = checked_ ? (x_ + width_ - radius) : (x_ + radius);

        renderer.fill_circle(cx, cy, thumb_radius, thumb_color_);
    }

    bool handle_gesture(const GestureEvent& event) override {
        if (!enabled_ || visibility_ != Visibility::VISIBLE)
            return false;

        if (event.type == GestureType::TAP && contains(event.x, event.y)) {
            toggle();
            return true;
        }

        // 也支持左滑关、右滑开
        if (event.type == GestureType::SWIPE_RIGHT && contains(event.x, event.y)) {
            if (!checked_) {
                set_checked(true);
                return true;
            }
        } else if (event.type == GestureType::SWIPE_LEFT && contains(event.x, event.y)) {
            if (checked_) {
                set_checked(false);
                return true;
            }
        }

        return false;
    }
};

} // namespace UI

#endif // AURORA_UI_SWITCH_VIEW_HPP
