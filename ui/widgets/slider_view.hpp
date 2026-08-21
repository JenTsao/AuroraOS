#ifndef AURORA_UI_SLIDER_VIEW_HPP
#define AURORA_UI_SLIDER_VIEW_HPP

#include "../view.hpp"

namespace UI {

class SliderView : public View {
private:
    int32_t min_value_;
    int32_t max_value_;
    int32_t current_value_;
    ColorRGB565 track_color_;
    ColorRGB565 fill_color_;
    ColorRGB565 thumb_color_;
    bool is_dragging_;

    void (*on_value_changed_)(SliderView*, int32_t, void*);
    void* callback_ctx_;

public:
    SliderView(int16_t x, int16_t y, uint16_t w, uint16_t h,
               int32_t min_v = 0, int32_t max_v = 100, int32_t cur_v = 50,
               ColorRGB565 track = 0x39E7 /* dark gray */,
               ColorRGB565 fill = 0x07E0 /* green */,
               ColorRGB565 thumb = 0xFFFF /* white */)
        : View(x, y, w, h), min_value_(min_v), max_value_(max_v), current_value_(cur_v),
          track_color_(track), fill_color_(fill), thumb_color_(thumb), is_dragging_(false),
          on_value_changed_(nullptr), callback_ctx_(nullptr) {
        clamp_value();
    }

    void set_range(int32_t min_v, int32_t max_v) {
        min_value_ = min_v;
        max_value_ = max_v;
        clamp_value();
        invalidate();
    }

    void set_value(int32_t val) {
        if (current_value_ != val) {
            current_value_ = val;
            clamp_value();
            invalidate();
            if (on_value_changed_) {
                on_value_changed_(this, current_value_, callback_ctx_);
            }
        }
    }

    int32_t get_value() const {
        return current_value_;
    }

    void set_on_value_changed_listener(void (*cb)(SliderView*, int32_t, void*), void* ctx) {
        on_value_changed_ = cb;
        callback_ctx_ = ctx;
    }

    void draw(UIRenderer& renderer) override {
        if (visibility_ != Visibility::VISIBLE) return;

        // 1. Draw track
        renderer.fill_rect(x_, y_ + height_ / 4, width_, height_ / 2, track_color_);

        // 2. Draw fill portion
        if (max_value_ > min_value_) {
            uint32_t fill_w = static_cast<uint32_t>((current_value_ - min_value_) * width_) /
                              static_cast<uint32_t>(max_value_ - min_value_);
            if (fill_w > width_) fill_w = width_;
            renderer.fill_rect(x_, y_ + height_ / 4, fill_w, height_ / 2, fill_color_);

            // 3. Draw thumb
            int16_t thumb_x = x_ + static_cast<int16_t>(fill_w) - height_ / 2;
            if (thumb_x < x_) thumb_x = x_;
            if (thumb_x + height_ > x_ + width_) thumb_x = x_ + width_ - height_;
            renderer.fill_rect(thumb_x, y_, height_, height_, thumb_color_);
        }
    }

    bool handle_gesture(const GestureEvent& event) override {
        if (!enabled_ || visibility_ != Visibility::VISIBLE) return false;

        if (event.type == GestureType::TAP || event.type == GestureType::SWIPE_RIGHT || event.type == GestureType::SWIPE_LEFT) {
            if (contains(event.x, event.y) || is_dragging_) {
                int16_t rel_x = event.x - x_;
                if (rel_x < 0) rel_x = 0;
                if (rel_x > width_) rel_x = width_;

                if (max_value_ > min_value_) {
                    int32_t new_val = min_value_ + (rel_x * (max_value_ - min_value_)) / width_;
                    set_value(new_val);
                    return true;
                }
            }
        }
        return false;
    }

private:
    void clamp_value() {
        if (current_value_ < min_value_) current_value_ = min_value_;
        if (current_value_ > max_value_) current_value_ = max_value_;
    }
};

} // namespace UI

#endif // AURORA_UI_SLIDER_VIEW_HPP
