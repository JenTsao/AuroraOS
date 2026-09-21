#ifndef AURORA_UI_TEXT_VIEW_HPP
#define AURORA_UI_TEXT_VIEW_HPP

#include "../view.hpp"
#include "../../apps/watch/font_engine.hpp" // 获取字体数据

namespace UI {

enum class TextAlign : uint8_t {
    LEFT,
    CENTER,
    RIGHT
};

class TextView : public View {
private:
    const char* text_;
    ColorRGB565 fg_color_;
    ColorRGB565 bg_color_;
    uint8_t scale_;
    TextAlign align_;

    uint16_t compute_text_width(const char* str) const noexcept {
        if (!str) return 0;
        int len = 0;
        while (str[len]) len++;
        return static_cast<uint16_t>(len * (5 + 1) * scale_);
    }

public:
    TextView(int16_t x, int16_t y, const char* text, ColorRGB565 fg, ColorRGB565 bg = 0, uint8_t scale = 2,
             TextAlign align = TextAlign::LEFT)
        : View(x, y, 0, 0),
          text_(text),
          fg_color_(fg),
          bg_color_(bg),
          scale_(scale),
          align_(align) {
        // 自动计算初始宽高度 (基于 5x7 默认字体)
        if (text) {
            width_ = compute_text_width(text);
            height_ = static_cast<uint16_t>(7 * scale_);
        }
    }

    TextView(int16_t x, int16_t y, uint16_t w, uint16_t h,
             const char* text, ColorRGB565 fg, ColorRGB565 bg = 0, uint8_t scale = 2,
             TextAlign align = TextAlign::LEFT)
        : View(x, y, w, h),
          text_(text),
          fg_color_(fg),
          bg_color_(bg),
          scale_(scale),
          align_(align) {}

    const char* get_text() const noexcept {
        return text_;
    }

    void set_text(const char* text) {
        text_ = text;
        const uint16_t measured_w = compute_text_width(text);
        if (measured_w > width_) {
            width_ = measured_w;
        }
        if (height_ == 0) {
            height_ = static_cast<uint16_t>(7 * scale_);
        }
        invalidate();
    }

    void set_text_align(TextAlign align) noexcept {
        if (align_ != align) {
            align_ = align;
            invalidate();
        }
    }

    TextAlign get_text_align() const noexcept {
        return align_;
    }

    void set_text_color(ColorRGB565 fg, ColorRGB565 bg = 0) noexcept {
        fg_color_ = fg;
        bg_color_ = bg;
        invalidate();
    }

    void set_scale(uint8_t scale) {
        if (scale_ != scale && scale > 0) {
            scale_ = scale;
            width_ = compute_text_width(text_);
            height_ = static_cast<uint16_t>(7 * scale_);
            invalidate();
        }
    }

    void draw(UIRenderer& renderer) override {
        if (!text_ || visibility_ != Visibility::VISIBLE)
            return;

        int16_t draw_x = x_;
        const uint16_t text_w = compute_text_width(text_);

        if (width_ > text_w) {
            if (align_ == TextAlign::CENTER) {
                draw_x = x_ + static_cast<int16_t>((width_ - text_w) / 2);
            } else if (align_ == TextAlign::RIGHT) {
                draw_x = x_ + static_cast<int16_t>(width_ - text_w);
            }
        }

        renderer.draw_string(draw_x, y_, text_, scale_, fg_color_, bg_color_, font5x7_data, 5, 7);
    }
};

} // namespace UI

#endif // AURORA_UI_TEXT_VIEW_HPP
