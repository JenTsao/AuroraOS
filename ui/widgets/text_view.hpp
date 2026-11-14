#ifndef AURORA_UI_TEXT_VIEW_HPP
#define AURORA_UI_TEXT_VIEW_HPP

#include "../view.hpp"
#include "../font5x7_data.hpp" // 获取字体数据
#include "../../drivers/display/bitmap_font.hpp"

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
    const FontDef* font_def_{nullptr};
    IFontSource* font_source_{nullptr};
    bool transparent_bg_{false};
    bool explicit_transparent_bg_{false};

    [[nodiscard]] uint8_t active_line_height() const noexcept {
        if (font_source_ && font_source_->line_height() > 0u) {
            return font_source_->line_height();
        }
        if (font_def_ && font_def_->line_height > 0u) {
            return font_def_->line_height;
        }
        return 7u;
    }

    uint16_t compute_text_width(const char* str) const noexcept {
        if (!str || !*str) {
            return 0u;
        }
        if (font_source_) {
            return measure_string_width_utf8(str, *font_source_, scale_, true);
        }
        if (font_def_) {
            MemoryFontSource src(font_def_);
            return measure_string_width_utf8(str, src, scale_, true);
        }
        uint16_t glyphs = 0u;
        const char* p = str;
        uint32_t cp = 0u;
        while (decode_utf8(p, cp)) {
            ++glyphs;
        }
        return static_cast<uint16_t>(glyphs * 6u * scale_);
    }

    void refresh_auto_metrics(bool shrink_width) noexcept {
        const uint16_t measured_w = compute_text_width(text_);
        if (shrink_width || measured_w > width_) {
            width_ = measured_w;
        }
        const uint16_t line_h = static_cast<uint16_t>(active_line_height()) * scale_;
        if (shrink_width || height_ == 0u || line_h > height_) {
            height_ = line_h;
        }
    }

public:
    TextView(int16_t x, int16_t y, const char* text, ColorRGB565 fg, ColorRGB565 bg = 0, uint8_t scale = 2,
             TextAlign align = TextAlign::LEFT)
        : View(x, y, 0, 0), text_(text), fg_color_(fg), bg_color_(bg), scale_(scale > 0 ? scale : 1), align_(align) {
        // 自动计算初始宽高度 (基于 5x7 默认字体或 UTF-8 码点数量)
        if (text) {
            width_ = compute_text_width(text);
            height_ = static_cast<uint16_t>(active_line_height() * scale_);
        }
    }

    TextView(int16_t x, int16_t y, uint16_t w, uint16_t h, const char* text, ColorRGB565 fg, ColorRGB565 bg = 0,
             uint8_t scale = 2, TextAlign align = TextAlign::LEFT)
        : View(x, y, w, h), text_(text), fg_color_(fg), bg_color_(bg), scale_(scale > 0 ? scale : 1), align_(align) {}

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
            height_ = static_cast<uint16_t>(active_line_height() * scale_);
        }
        invalidate();
    }

    // 绑定 Flash/RAM 中的 FontDef 字体（支持 Unicode/中文 与 4bpp 灰度字形）
    void set_font(const FontDef* font, bool shrink_to_fit = false) noexcept {
        font_def_ = font;
        font_source_ = nullptr;
        if (!explicit_transparent_bg_) {
            transparent_bg_ = (font != nullptr);
        }
        refresh_auto_metrics(shrink_to_fit);
        invalidate();
    }

    // 绑定流式或组合字库数据源（例如从 LittleFS 按需加载的 StreamFontSource）
    void set_font_source(IFontSource* source, bool shrink_to_fit = false) noexcept {
        font_source_ = source;
        font_def_ = nullptr;
        if (!explicit_transparent_bg_) {
            transparent_bg_ = (source != nullptr);
        }
        refresh_auto_metrics(shrink_to_fit);
        invalidate();
    }

    void set_transparent_background(bool transparent) noexcept {
        explicit_transparent_bg_ = true;
        if (transparent_bg_ != transparent) {
            transparent_bg_ = transparent;
            invalidate();
        }
    }

    [[nodiscard]] bool transparent_background() const noexcept {
        return transparent_bg_;
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
            height_ = static_cast<uint16_t>(active_line_height() * scale_);
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

        if (font_source_) {
            renderer.draw_string_utf8(draw_x, y_, text_, scale_, fg_color_, bg_color_, *font_source_, transparent_bg_,
                                      font5x7_data);
        } else if (font_def_) {
            renderer.draw_string_utf8(draw_x, y_, text_, scale_, fg_color_, bg_color_, *font_def_, transparent_bg_,
                                      font5x7_data);
        } else {
            renderer.draw_string(draw_x, y_, text_, scale_, fg_color_, bg_color_, font5x7_data, 5, 7);
        }
    }
};

} // namespace UI

#endif // AURORA_UI_TEXT_VIEW_HPP
