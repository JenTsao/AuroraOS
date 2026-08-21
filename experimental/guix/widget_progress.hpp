#ifndef AURORA_GUIX_WIDGET_PROGRESS_HPP
#define AURORA_GUIX_WIDGET_PROGRESS_HPP

#include "widget.hpp"

namespace auroraos {
namespace guix {

class ProgressBar : public Widget {
public:
    ProgressBar(int32_t min_val = 0, int32_t max_val = 100, int32_t cur_val = 0);
    ~ProgressBar() override = default;

    void set_range(int32_t min_val, int32_t max_val);
    void set_value(int32_t val);
    int32_t get_value() const { return value_; }
    int32_t get_min() const { return min_val_; }
    int32_t get_max() const { return max_val_; }

    void set_colors(uint16_t track_color, uint16_t fill_color, uint16_t border_color = Color::LightGray);
    void set_track_color(uint16_t color) { track_color_ = color; }
    void set_fill_color(uint16_t color) { fill_color_ = color; }
    void set_border_color(uint16_t color) { border_color_ = color; }

    void set_show_text(bool show) { show_text_ = show; }
    bool get_show_text() const { return show_text_; }

    void paint(Window* win) override;

private:
    int32_t min_val_;
    int32_t max_val_;
    int32_t value_;

    uint16_t track_color_;
    uint16_t fill_color_;
    uint16_t border_color_;
    bool show_text_;
};

} // namespace guix
} // namespace auroraos

#endif // AURORA_GUIX_WIDGET_PROGRESS_HPP
