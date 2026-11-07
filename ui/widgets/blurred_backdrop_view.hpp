// =============================================================================
// ui/widgets/blurred_backdrop_view.hpp
//
// 跨条带无缝的毛玻璃背板（RGB565）
//
// 为什么不使用 Renderer2D::blur_rect()：
//   分带渲染下帧缓冲只有 AURORA_UI_BAND_H 行（默认 35），UiManager 每带的
//   裁剪区是 damage ∩ band。跨带的 ROI 无法在任一带内取得完整邻域，逐带各自
//   clamp-to-edge 会在每 35 行处产生可见接缝。
//
// 本控件的做法：把整块背板放在**调用方提供的离屏缓冲**里，blur() 一次模糊
// 完整 ROI（无条带概念），draw() 再按当前可见行逐行 blit 到条带帧缓冲。
// 因此模糊质量与条带划分无关，且不需要修改 UiManager 分带循环。
//
// 内存契约（AGENTS §12 / §36）：
//  - backdrop 缓冲与 scratch 全部由调用方静态/栈上提供，本类借用、非拥有，
//    零动态分配、零静态大数组；未实例化本控件的板卡零体积增量
//  - backdrop 容量 = width * height 个 uint16_t
//  - scratch 容量 = aurora::gpu::scratch_words_for(radius, width) 个 uint16_t
//  - 模糊输出只落在 ROI 内（clamp-to-edge 原地卷积），故 damage 就是本控件
//    包围盒，无需按半径外扩
// =============================================================================
#ifndef AURORA_UI_BLURRED_BACKDROP_VIEW_HPP
#define AURORA_UI_BLURRED_BACKDROP_VIEW_HPP

#include <stdint.h>
#include <stddef.h>
#include "../view.hpp"
#include "../../drivers/gpu/gaussian_blur.hpp"

namespace UI {

class BlurredBackdropView : public View {
public:
    // backdrop 为借用缓冲，须可容纳 w*h 个 RGB565 像素，生命周期覆盖本控件。
    BlurredBackdropView(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* backdrop,
                        size_t backdrop_pixels) noexcept
        : View(x, y, w, h), backdrop_(backdrop), capacity_(backdrop_pixels), prepared_(false) {}

    // 可直接写入源内容（程序化背景 / 上一帧快照）。写入后须重新 blur()。
    [[nodiscard]] uint16_t* backdrop() noexcept {
        return backdrop_;
    }

    [[nodiscard]] const uint16_t* backdrop() const noexcept {
        return backdrop_;
    }

    [[nodiscard]] bool is_prepared() const noexcept {
        return prepared_;
    }

    // 从已有位图复制源内容；容量不足时拒绝且不改动缓冲。
    [[nodiscard]] bool set_source(const uint16_t* src) noexcept {
        if (!src || !backdrop_ || capacity_ < pixel_count()) {
            return false;
        }
        for (size_t i = 0; i < pixel_count(); ++i) {
            backdrop_[i] = src[i];
        }
        prepared_ = false;
        return true;
    }

    // 对整块离屏背板做一次模糊。成功后 invalidate() 触发下一帧重绘。
    [[nodiscard]] aurora::gpu::BlurResult blur(const aurora::gpu::BlurKernel& kernel, uint16_t* scratch,
                                               size_t scratch_words) noexcept {
        if (!backdrop_ || capacity_ < pixel_count()) {
            return aurora::gpu::BlurResult::invalid_surface;
        }
        aurora::gpu::Surface surface(backdrop_, width_, height_);
        const aurora::gpu::BlurResult result =
            aurora::gpu::blur_rgb565(surface, 0, 0, width_, height_, kernel, scratch, scratch_words);
        if (result == aurora::gpu::BlurResult::ok) {
            prepared_ = true;
            invalidate();
        }
        return result;
    }

    void draw(UIRenderer& renderer) override {
        if (visibility_ != Visibility::VISIBLE || !backdrop_ || !prepared_) {
            return;
        }

        // get_visible_row_range 返回「视图坐标系」的可见行闭区间，正是为全屏
        // 遮罩/背板这类逐行绘制准备的（见 dialog_view.hpp）。只 blit 当前条带
        // 覆盖的行，避免每带都遍历整块背板后被 clip 静默丢弃。
        int32_t vis_lo = 0;
        int32_t vis_hi = 0;
        renderer.get_visible_row_range(vis_lo, vis_hi);

        int32_t first = vis_lo - y_;
        int32_t last = vis_hi - y_;
        if (first < 0) {
            first = 0;
        }
        if (last > static_cast<int32_t>(height_) - 1) {
            last = static_cast<int32_t>(height_) - 1;
        }
        for (int32_t row = first; row <= last; ++row) {
            const int32_t view_y = static_cast<int32_t>(y_) + row;
            if (view_y > 0x7FFF) {
                break; // draw_bitmap 的 y 为 int16，防极端坐标窄化
            }
            renderer.draw_bitmap(x_, static_cast<int16_t>(view_y), width_, 1,
                                 &backdrop_[static_cast<size_t>(row) * width_]);
        }
    }

private:
    [[nodiscard]] size_t pixel_count() const noexcept {
        return static_cast<size_t>(width_) * static_cast<size_t>(height_);
    }

    uint16_t* backdrop_;
    size_t capacity_;
    bool prepared_;
};

} // namespace UI

#endif // AURORA_UI_BLURRED_BACKDROP_VIEW_HPP
