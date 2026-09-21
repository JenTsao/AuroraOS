#ifndef AURORA_UI_BUTTON_HPP
#define AURORA_UI_BUTTON_HPP

#include "../view_group.hpp"
#include "text_view.hpp"

namespace UI {

class Button : public ViewGroup {
private:
    ColorRGB565 bg_color_;
    ColorRGB565 pressed_color_;
    bool is_pressed_;

    // 函数指针回调
    void (*on_click_callback_)(void*);
    void* callback_context_;

public:
    Button(int16_t x, int16_t y, uint16_t w, uint16_t h, ColorRGB565 bg, ColorRGB565 pressed)
        : ViewGroup(x, y, w, h), bg_color_(bg), pressed_color_(pressed), is_pressed_(false),
          on_click_callback_(nullptr), callback_context_(nullptr) {}

    void set_on_click(void (*callback)(void*), void* context) {
        on_click_callback_ = callback;
        callback_context_ = context;
    }

    void draw(UIRenderer& renderer) override {
        // 画背景色
        ColorRGB565 current_bg = is_pressed_ ? pressed_color_ : bg_color_;
        renderer.fill_rect(x_, y_, width_, height_, current_bg);

        // 渲染子节点 (例如里面的文本)
        ViewGroup::draw(renderer);
    }

    bool is_pressed() const noexcept {
        return is_pressed_;
    }

    bool handle_gesture(const GestureEvent& event) override {
        // 先让子节点处理（比如里面如果套了更复杂的组件）
        if (ViewGroup::handle_gesture(event)) {
            return true;
        }

        // 1. 触控按下瞬态：进入高亮按压态
        if (event.type == GestureType::TOUCH_DOWN && contains(event.x, event.y)) {
            is_pressed_ = true;
            invalidate();
            return true;
        }

        // 2. 触控抬起或取消：离开按压态
        if (event.type == GestureType::TOUCH_UP || event.type == GestureType::CANCEL) {
            if (is_pressed_) {
                is_pressed_ = false;
                invalidate();
                return true;
            }
        }

        // 3. 点击完成触发回调
        if (event.type == GestureType::TAP && contains(event.x, event.y)) {
            is_pressed_ = true;
            invalidate();
            if (on_click_callback_) {
                on_click_callback_(callback_context_);
            }
            is_pressed_ = false;
            invalidate();
            return true;
        }

        return false;
    }
};

} // namespace UI

#endif // AURORA_UI_BUTTON_HPP
