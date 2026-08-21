#ifndef AURORA_GUIX_WIDGET_PANEL_HPP
#define AURORA_GUIX_WIDGET_PANEL_HPP

#include "widget.hpp"

namespace auroraos {
namespace guix {

class Panel : public Widget {
public:
    Panel(uint16_t bg_color = Color::Black, bool transparent = true, uint16_t border_color = Color::Black);
    ~Panel() override = default;

    void set_bg_color(uint16_t color) { bg_color_ = color; transparent_ = false; }
    uint16_t get_bg_color() const { return bg_color_; }

    void set_transparent(bool transparent) { transparent_ = transparent; }
    bool is_transparent() const { return transparent_; }

    void set_border_color(uint16_t color) { border_color_ = color; draw_border_ = true; }
    void set_draw_border(bool draw) { draw_border_ = draw; }

    void paint(Window* win) override;

private:
    uint16_t bg_color_;
    bool transparent_;
    uint16_t border_color_;
    bool draw_border_;
};

} // namespace guix
} // namespace auroraos

#endif // AURORA_GUIX_WIDGET_PANEL_HPP
