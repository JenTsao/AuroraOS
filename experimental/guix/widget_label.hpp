#ifndef AURORA_GUIX_WIDGET_LABEL_HPP
#define AURORA_GUIX_WIDGET_LABEL_HPP

#include "widget.hpp"

namespace auroraos {
namespace guix {

class Label : public Widget {
public:
    Label(const char* text = nullptr);
    ~Label() override = default;

    void set_text(const char* text);
    const char* get_text() const { return text_; }

    void set_text_color(uint16_t color) { text_color_ = color; }
    uint16_t get_text_color() const { return text_color_; }

    void set_bg_color(uint16_t color) { bg_color_ = color; transparent_bg_ = false; }
    void set_transparent(bool transparent) { transparent_bg_ = transparent; }

    void set_alignment(Alignment align) { align_ = align; }
    Alignment get_alignment() const { return align_; }

    void set_scale(uint8_t scale) { scale_ = (scale > 0) ? scale : 1; }
    uint8_t get_scale() const { return scale_; }

    void paint(Window* win) override;

private:
    char text_[64];
    uint16_t text_color_;
    uint16_t bg_color_;
    bool transparent_bg_;
    Alignment align_;
    uint8_t scale_;
};

} // namespace guix
} // namespace auroraos

#endif // AURORA_GUIX_WIDGET_LABEL_HPP
