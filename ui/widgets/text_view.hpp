#ifndef AURORA_UI_TEXT_VIEW_HPP
#define AURORA_UI_TEXT_VIEW_HPP

#include "../view.hpp"
#include "../../apps/watch/font_engine.hpp" // 获取字体数据

namespace UI {

class TextView : public View {
private:
    const char* text_;
    ColorRGB565 fg_color_;
    ColorRGB565 bg_color_;
    uint8_t scale_;

public:
    TextView(int16_t x, int16_t y, const char* text, ColorRGB565 fg, ColorRGB565 bg = 0, uint8_t scale = 2)
        : View(x, y, 0, 0), text_(text), fg_color_(fg), bg_color_(bg), scale_(scale) {
        
        // 自动计算宽高度 (基于 5x7 默认字体)
        if (text) {
            int len = 0;
            while(text[len]) len++;
            width_ = len * (5 + 1) * scale; 
            height_ = 7 * scale;
        }
    }

    void set_text(const char* text) {
        text_ = text;
        invalidate(); // 标记自己需要重绘
    }

    void draw(UIRenderer& renderer) override {
        if (!text_) return;
        renderer.draw_string(x_, y_, text_, scale_, fg_color_, bg_color_, font5x7_data, 5, 7);
    }
};

} // namespace UI

#endif // AURORA_UI_TEXT_VIEW_HPP
