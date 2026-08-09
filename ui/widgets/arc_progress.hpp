#ifndef AURORA_UI_ARC_PROGRESS_HPP
#define AURORA_UI_ARC_PROGRESS_HPP

#include "../view.hpp"

namespace UI {

class ArcProgress : public View {
private:
    uint16_t radius_;
    uint8_t percentage_; // 0 - 100
    ColorRGB565 color_;

public:
    ArcProgress(int16_t center_x, int16_t center_y, uint16_t radius, uint8_t percentage, ColorRGB565 color)
        : View(center_x - radius, center_y - radius, radius * 2, radius * 2),
          radius_(radius), percentage_(percentage), color_(color) {}

    void set_percentage(uint8_t percentage) {
        if (percentage > 100) percentage = 100;
        if (percentage_ != percentage) {
            percentage_ = percentage;
            invalidate();
        }
    }

    void draw(UIRenderer& renderer) override {
        // 计算结束角度 (0 - 360)
        uint16_t end_angle = (percentage_ * 360) / 100;
        
        // renderer.draw_arc(cx, cy, r, start_angle, end_angle, color)
        // 从顶部 (270度) 顺时针绘制
        int16_t cx = x_ + radius_;
        int16_t cy = y_ + radius_;
        
        // 如果进度为0，不绘制；进度为100，绘制整圆
        if (end_angle > 0) {
            if (end_angle >= 360) {
                renderer.draw_circle(cx, cy, radius_, color_);
            } else {
                uint16_t start = 270;
                uint16_t end = (270 + end_angle) % 360;
                // renderer.draw_arc 可能会有具体的起始与终止角度逻辑
                // 这里我们假设其实现为 draw_arc(x, y, r, start, end, color)
                renderer.draw_arc(cx, cy, radius_, start, end, color_);
            }
        }
    }
};

} // namespace UI

#endif // AURORA_UI_ARC_PROGRESS_HPP
