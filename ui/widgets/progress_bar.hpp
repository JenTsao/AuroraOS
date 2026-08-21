#ifndef AURORA_UI_PROGRESS_BAR_HPP
#define AURORA_UI_PROGRESS_BAR_HPP

#include "../view.hpp"

namespace UI {

class ProgressBar : public View {
private:
    int32_t min_value_;
    int32_t max_value_;
    int32_t current_value_;
    ColorRGB565 bg_color_;
    ColorRGB565 fill_color_;

public:
    ProgressBar(int16_t x, int16_t y, uint16_t w, uint16_t h,
                int32_t min_v = 0, int32_t max_v = 100, int32_t cur_v = 0,
                ColorRGB565 bg = 0x18C3 /* dark slate */,
                ColorRGB565 fill = 0x001F /* blue */)
        : View(x, y, w, h), min_value_(min_v), max_value_(max_v), current_value_(cur_v),
          bg_color_(bg), fill_color_(fill) {
        clamp_value();
    }

    void set_range(int32_t min_v, int32_t max_v) {
        min_value_ = min_v;
        max_value_ = max_v;
        clamp_value();
        invalidate();
    }

    void set_progress(int32_t val) {
        if (current_value_ != val) {
            current_value_ = val;
            clamp_value();
            invalidate();
        }
    }

    int32_t get_progress() const {
        return current_value_;
    }

    uint8_t get_percentage() const {
        if (max_value_ <= min_value_) return 0;
        return static_cast<uint8_t>(((current_value_ - min_value_) * 100) / (max_value_ - min_value_));
    }

    void set_colors(ColorRGB565 bg, ColorRGB565 fill) {
        bg_color_ = bg;
        fill_color_ = fill;
        invalidate();
    }

    void draw(UIRenderer& renderer) override {
        if (visibility_ != Visibility::VISIBLE) return;

        // Draw track background
        renderer.fill_rect(x_, y_, width_, height_, bg_color_);

        // Draw filled progress
        if (max_value_ > min_value_ && current_value_ > min_value_) {
            uint32_t fill_w = static_cast<uint32_t>((current_value_ - min_value_) * width_) /
                              static_cast<uint32_t>(max_value_ - min_value_);
            if (fill_w > width_) fill_w = width_;
            if (fill_w > 0) {
                renderer.fill_rect(x_, y_, fill_w, height_, fill_color_);
            }
        }
    }

private:
    void clamp_value() {
        if (current_value_ < min_value_) current_value_ = min_value_;
        if (current_value_ > max_value_) current_value_ = max_value_;
    }
};

} // namespace UI

#endif // AURORA_UI_PROGRESS_BAR_HPP
