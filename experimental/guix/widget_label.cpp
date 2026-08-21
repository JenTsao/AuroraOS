#include "widget_label.hpp"
#include "window.hpp"
#include <string.h>

namespace auroraos {
namespace guix {

Label::Label(const char* text)
    : text_color_(Color::White),
      bg_color_(Color::Black),
      transparent_bg_(true),
      align_(Alignment::Left),
      scale_(1) {
    set_text(text);
}

void Label::set_text(const char* text) {
    if (!text) {
        text_[0] = '\0';
        return;
    }
    size_t i = 0;
    while (text[i] && i < sizeof(text_) - 1) {
        text_[i] = text[i];
        ++i;
    }
    text_[i] = '\0';
}

void Label::paint(Window* win) {
    if (!visible_ || !win || w_ == 0 || h_ == 0)
        return;

    Rect wb = get_window_bounds();

    if (!transparent_bg_) {
        win->fill_rect(wb.x, wb.y, wb.w, wb.h, bg_color_);
    }

    if (text_[0] == '\0')
        return;

    size_t len = strlen(text_);
    int32_t char_width = 6 * scale_;
    int32_t char_height = 8 * scale_;
    int32_t text_total_w = static_cast<int32_t>(len * char_width);

    int32_t text_x = wb.x;
    if (align_ == Alignment::Center) {
        text_x = wb.x + (wb.w - text_total_w) / 2;
    } else if (align_ == Alignment::Right) {
        text_x = wb.x + wb.w - text_total_w;
    }

    int32_t text_y = wb.y + (wb.h - char_height) / 2;
    if (text_x < wb.x) text_x = wb.x;
    if (text_y < wb.y) text_y = wb.y;

    win->draw_text(static_cast<uint32_t>(text_x), static_cast<uint32_t>(text_y),
                   text_, text_color_, bg_color_, transparent_bg_, scale_);
}

} // namespace guix
} // namespace auroraos
