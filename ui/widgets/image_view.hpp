#ifndef AURORA_UI_IMAGE_VIEW_HPP
#define AURORA_UI_IMAGE_VIEW_HPP

#include "../view.hpp"

namespace UI {

// ========================================================
// ImageView: 位图/图标显示控件
//
// 支持 16 位 RGB565 原生显存位图、透明通道键（ChromaKey）过滤
// 与居中自适应布局，适用于表盘图标、电量图标、状态栏符号等。
// ========================================================
class ImageView : public View {
private:
    const uint16_t* bitmap_;
    uint16_t img_width_;
    uint16_t img_height_;
    bool has_chroma_key_;
    uint16_t chroma_key_;

public:
    ImageView(int16_t x, int16_t y, uint16_t w, uint16_t h,
              const uint16_t* bitmap = nullptr,
              bool has_key = false, uint16_t key = 0x0000)
        : View(x, y, w, h),
          bitmap_(bitmap),
          img_width_(w),
          img_height_(h),
          has_chroma_key_(has_key),
          chroma_key_(key) {}

    void set_bitmap(const uint16_t* bitmap, uint16_t w, uint16_t h) {
        bitmap_ = bitmap;
        img_width_ = w;
        img_height_ = h;
        invalidate();
    }

    void set_chroma_key(bool enable, uint16_t key = 0x0000) {
        has_chroma_key_ = enable;
        chroma_key_ = key;
        invalidate();
    }

    const uint16_t* get_bitmap() const noexcept {
        return bitmap_;
    }

    void draw(UIRenderer& renderer) override {
        if (visibility_ != Visibility::VISIBLE || !bitmap_)
            return;

        // 居中偏移计算
        const int16_t draw_x = x_ + static_cast<int16_t>((width_ > img_width_) ? ((width_ - img_width_) / 2) : 0);
        const int16_t draw_y = y_ + static_cast<int16_t>((height_ > img_height_) ? ((height_ - img_height_) / 2) : 0);

        if (has_chroma_key_) {
            renderer.draw_bitmap_transparent(draw_x, draw_y, img_width_, img_height_, bitmap_, chroma_key_);
        } else {
            renderer.draw_bitmap(draw_x, draw_y, img_width_, img_height_, bitmap_);
        }
    }
};

} // namespace UI

#endif // AURORA_UI_IMAGE_VIEW_HPP
