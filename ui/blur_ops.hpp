// =============================================================================
// ui/blur_ops.hpp
//
// 条带帧缓冲内的局部高斯模糊（UI 渲染层入口）
//
// 为什么不把 blur_rect 塞进 Renderer2D：
//  - Renderer2D 已承担光栅化职责，模糊是「读回帧缓冲 + 后处理」，属于上层算子；
//    用 public getter 足以复刻坐标变换，无需给渲染器开后门或加 friend
//  - 保持 drivers/display/renderer2d.hpp 零改动：该文件一旦进入 CI 的
//    cppcheck 增量清单，其 include 链会带出 kernel/ 头的历史告警
//
// 条带接缝约束：
//  模糊是 clamp-to-edge 到 ROI 的原地卷积。分带渲染下裁剪区是 damage ∩ band，
//  若把 ROI 裁成其中一部分，带边界会被当成图像边缘复制，逐带输出会在每
//  AURORA_UI_BAND_H 行产生可见接缝。因此 ROI 必须**完整落在当前裁剪区内**，
//  否则返回 crosses_band_edge；跨带毛玻璃请改用 widgets/BlurredBackdropView。
//
// 内存与脏标记：
//  - scratch 由调用方借用，容量 = aurora::gpu::scratch_words_for(kernel.radius, w)
//  - blur_rgb565 直写 raw buffer（绕过 set_pixel），故必须补 FrameBuffer::mark_dirty
//  - 输出只落在 ROI 内，damage 即 ROI 本身，无需按半径外扩
// =============================================================================
#ifndef AURORA_UI_BLUR_OPS_HPP
#define AURORA_UI_BLUR_OPS_HPP

#include <stdint.h>
#include <stddef.h>
#include "ui_config.hpp"
#include "../drivers/gpu/gaussian_blur.hpp"

namespace UI {

enum class BlurRectResult : uint8_t {
    ok = 0,
    empty_region,
    invalid_kernel,
    insufficient_scratch,
    crosses_band_edge,
};

// renderer 与 fb 必须是构造时绑定的那一对（Renderer2D 持有 FrameBuffer 引用）。
// x/y/w/h 为视图坐标，与 fill_rect 等绘制 API 一致。
template <uint16_t W, uint16_t H>
[[nodiscard]] BlurRectResult blur_rect(Renderer2D<W, H>& renderer, FrameBuffer<W, H>& fb, int16_t x, int16_t y,
                                       uint16_t w, uint16_t h, const aurora::gpu::BlurKernel& kernel, uint16_t* scratch,
                                       size_t scratch_words) noexcept {
    if (!kernel.valid()) {
        return BlurRectResult::invalid_kernel;
    }
    if (w == 0u || h == 0u) {
        return BlurRectResult::empty_region;
    }

    const Rect2D clip = renderer.get_clip_rect();
    if (clip.w == 0u || clip.h == 0u) {
        return BlurRectResult::empty_region;
    }

    // 与 Renderer2D::plot / fill_rect 相同的合成平移：offset - band_origin。
    const int32_t l = static_cast<int32_t>(x) + renderer.get_offset_x();
    const int32_t t =
        static_cast<int32_t>(y) + renderer.get_offset_y() - static_cast<int32_t>(renderer.get_band_origin());
    const int32_t r = l + static_cast<int32_t>(w) - 1;
    const int32_t b = t + static_cast<int32_t>(h) - 1;
    const int32_t cr = static_cast<int32_t>(clip.x) + clip.w - 1;
    const int32_t cb = static_cast<int32_t>(clip.y) + clip.h - 1;
    if (l < clip.x || t < clip.y || r > cr || b > cb) {
        return BlurRectResult::crosses_band_edge;
    }

    aurora::gpu::Surface surface(fb.get_raw_buffer(), W, H);
    const aurora::gpu::BlurResult result =
        aurora::gpu::blur_rgb565(surface, l, t, w, h, kernel, scratch, scratch_words);
    if (result == aurora::gpu::BlurResult::insufficient_scratch) {
        return BlurRectResult::insufficient_scratch;
    }
    if (result != aurora::gpu::BlurResult::ok) {
        return BlurRectResult::empty_region;
    }

    fb.mark_dirty(static_cast<uint16_t>(l), static_cast<uint16_t>(t), w, h);
    return BlurRectResult::ok;
}

} // namespace UI

#endif // AURORA_UI_BLUR_OPS_HPP
