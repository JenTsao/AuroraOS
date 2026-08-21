#include "widget_progress.hpp"
#include "window.hpp"
#include <algorithm>
#include <stdio.h>

namespace auroraos {
namespace guix {

ProgressBar::ProgressBar(int32_t min_val, int32_t max_val, int32_t cur_val)
    : min_val_(min_val),
      max_val_(max_val),
      value_(cur_val),
      track_color_(Color::DarkGray),
      fill_color_(Color::Green),
      border_color_(Color::LightGray),
      show_text_(true) {
    if (max_val_ <= min_val_) {
        max_val_ = min_val_ + 1;
    }
    set_value(cur_val);
}

void ProgressBar::set_range(int32_t min_val, int32_t max_val) {
    min_val_ = min_val;
    max_val_ = (max_val > min_val) ? max_val : min_val + 1;
    set_value(value_);
}

void ProgressBar::set_value(int32_t val) {
    value_ = std::max(min_val_, std::min(max_val_, val));
}

void ProgressBar::set_colors(uint16_t track_color, uint16_t fill_color, uint16_t border_color) {
    track_color_ = track_color;
    fill_color_ = fill_color;
    border_color_ = border_color;
}

void ProgressBar::paint(Window* win) {
    if (!visible_ || !win || w_ == 0 || h_ == 0)
        return;

    Rect wb = get_window_bounds();

    // 1. 绘制底轨背景 (Track)
    win->fill_rect(wb.x, wb.y, wb.w, wb.h, track_color_);

    // 2. 计算填充宽度并绘制进度条填充 (Fill)
    int64_t range = static_cast<int64_t>(max_val_) - min_val_;
    int64_t current = static_cast<int64_t>(value_) - min_val_;
    if (range > 0 && current > 0) {
        int32_t fill_w = static_cast<int32_t>((current * (wb.w - 2)) / range);
        fill_w = std::min(static_cast<int32_t>(wb.w - 2), fill_w);
        if (fill_w > 0 && wb.h > 2) {
            win->fill_rect(wb.x + 1, wb.y + 1, static_cast<uint32_t>(fill_w), wb.h - 2, fill_color_);
        }
    }

    // 3. 绘制外边框
    win->draw_rect(wb.x, wb.y, wb.w, wb.h, border_color_);

    // 4. 可选百分比文字绘制
    if (show_text_ && wb.h >= 10 && wb.w >= 30) {
        int pct = static_cast<int>((current * 100) / (range > 0 ? range : 1));
        char buf[8];
        if (pct >= 100) {
            buf[0] = '1'; buf[1] = '0'; buf[2] = '0'; buf[3] = '%'; buf[4] = '\0';
        } else if (pct >= 10) {
            buf[0] = '0' + (pct / 10);
            buf[1] = '0' + (pct % 10);
            buf[2] = '%';
            buf[3] = '\0';
        } else {
            buf[0] = '0' + (pct % 10);
            buf[1] = '%';
            buf[2] = '\0';
        }

        int32_t text_w = 6 * (pct >= 100 ? 4 : (pct >= 10 ? 3 : 2));
        int32_t tx = wb.x + (wb.w - text_w) / 2;
        int32_t ty = wb.y + (wb.h - 8) / 2;
        win->draw_text(static_cast<uint32_t>(tx), static_cast<uint32_t>(ty),
                       buf, Color::White, 0, true, 1);
    }
}

} // namespace guix
} // namespace auroraos
