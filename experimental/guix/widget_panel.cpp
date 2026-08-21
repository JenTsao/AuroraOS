#include "widget_panel.hpp"
#include "window.hpp"

namespace auroraos {
namespace guix {

Panel::Panel(uint16_t bg_color, bool transparent, uint16_t border_color)
    : bg_color_(bg_color),
      transparent_(transparent),
      border_color_(border_color),
      draw_border_(false) {
}

void Panel::paint(Window* win) {
    if (!visible_ || !win || w_ == 0 || h_ == 0)
        return;

    Rect wb = get_window_bounds();

    if (!transparent_) {
        win->fill_rect(wb.x, wb.y, wb.w, wb.h, bg_color_);
    }

    if (draw_border_) {
        win->draw_rect(wb.x, wb.y, wb.w, wb.h, border_color_);
    }
}

} // namespace guix
} // namespace auroraos
