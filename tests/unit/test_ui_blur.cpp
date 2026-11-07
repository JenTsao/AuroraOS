// =============================================================================
// tests/unit/test_ui_blur.cpp
//
// UI 渲染管线的高斯模糊接入测试
//
// 覆盖两层：
//  1. Renderer2D::blur_rect —— 条带帧缓冲内的局部模糊：坐标变换、裁剪区约束、
//     拒绝跨带（避免接缝）、写后补脏标记、失败路径不写像素
//  2. BlurredBackdropView —— 离屏整块模糊 + 按条带 blit：跨带逐像素等价于
//     整块模糊结果（无接缝），以及容量/准备态/失败路径
//
// 测试沿用 test_ui_band_render.cpp 的分带模型：帧缓冲只有 AURORA_UI_BAND_H 行，
// 用 set_band_origin 模拟逐带渲染，不依赖真实显示硬件。
// =============================================================================

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <vector>

#include "../../ui/blur_ops.hpp"
#include "../../ui/widgets/blurred_backdrop_view.hpp"
#include "../../drivers/gpu/gaussian_blur.hpp"

namespace {

using UI::BlurRectResult;

constexpr uint16_t BLACK = 0x0000u;
constexpr uint16_t WHITE = 0xFFFFu;
constexpr uint16_t TEST_COLOR = 0x1234u;
constexpr uint16_t kW = static_cast<uint16_t>(DISPLAY_WIDTH);
constexpr uint16_t kBandH = static_cast<uint16_t>(AURORA_UI_BAND_H);

using BandFrameBuffer = FrameBuffer<kW, kBandH>;

aurora::gpu::BlurKernel make_kernel(uint8_t radius = 2) {
    aurora::gpu::BlurKernel kernel{};
    EXPECT_TRUE(aurora::gpu::make_binomial_kernel(radius, kernel));
    return kernel;
}

// 记录 flush 的窗口，用来验证「直写 raw buffer 后是否补了脏标记」。
struct RecordingDriver {
    uint16_t x0{0}, y0{0}, x1{0}, y1{0};
    int windows{0};

    void set_window(uint16_t a, uint16_t b, uint16_t c, uint16_t d) {
        x0 = a;
        y0 = b;
        x1 = c;
        y1 = d;
        ++windows;
    }

    void write_patch(const uint16_t*, uint32_t) {}
};

// =============================================================================
// 1. Renderer2D::blur_rect
// =============================================================================
class RendererBlurTest : public ::testing::Test {
protected:
    void SetUp() override {
        // 直接写 raw buffer：不触发 set_pixel/fill_rect 的脏标记，
        // 便于单独验证 blur_rect 自己补的 mark_dirty。
        std::fill(fb_.get_raw_buffer(), fb_.get_raw_buffer() + kW * kBandH, BLACK);
    }

    uint16_t pixel(uint16_t x, uint16_t y) {
        return fb_.get_raw_buffer()[static_cast<size_t>(y) * kW + x];
    }

    BlurRectResult do_blur(int16_t x, int16_t y, uint16_t w, uint16_t h, const aurora::gpu::BlurKernel& kernel,
                           uint16_t* scratch, size_t words) {
        return UI::blur_rect(renderer_, fb_, x, y, w, h, kernel, scratch, words);
    }

    BandFrameBuffer fb_{};
    UI::UIRenderer renderer_{fb_};
};

TEST_F(RendererBlurTest, UniformFieldUnchangedAndDirtyRectMarked) {
    renderer_.fill_rect(4, 3, 16, 12, TEST_COLOR);
    RecordingDriver warmup;
    fb_.flush(warmup); // 消费掉 fill_rect 产生的脏区

    const auto kernel = make_kernel(2);
    std::vector<uint16_t> scratch(aurora::gpu::scratch_words_for(kernel.radius, 16));
    ASSERT_EQ(do_blur(4, 3, 16, 12, kernel, scratch.data(), scratch.size()), BlurRectResult::ok);

    for (uint16_t y = 3; y < 15; ++y) {
        for (uint16_t x = 4; x < 20; ++x) {
            EXPECT_EQ(pixel(x, y), TEST_COLOR) << "(" << x << "," << y << ")";
        }
    }

    RecordingDriver flush;
    fb_.flush(flush);
    EXPECT_EQ(flush.windows, 1);
    EXPECT_EQ(flush.x0, 4);
    EXPECT_EQ(flush.y0, 3);
    EXPECT_EQ(flush.x1, 19);
    EXPECT_EQ(flush.y1, 14);
}

TEST_F(RendererBlurTest, ImpulseSpreadsOnlyInsideRoi) {
    fb_.get_raw_buffer()[static_cast<size_t>(10) * kW + 10] = WHITE;

    const auto kernel = make_kernel(2);
    std::vector<uint16_t> scratch(aurora::gpu::scratch_words_for(kernel.radius, 9));
    ASSERT_EQ(do_blur(6, 6, 9, 9, kernel, scratch.data(), scratch.size()), BlurRectResult::ok);

    EXPECT_LT((pixel(10, 10) >> 11u) & 0x1Fu, 0x1Fu);
    EXPECT_GT((pixel(11, 10) >> 11u) & 0x1Fu, 0u);
    EXPECT_EQ(pixel(5, 10), BLACK); // ROI 外未被触及
}

TEST_F(RendererBlurTest, HonoursOffsetAndBandOrigin) {
    // 第二带：屏幕行 35..69 映射进帧缓冲行 0..34；offset_x=8 再右移 8 列。
    renderer_.set_offset(8, 0);
    renderer_.set_band_origin(static_cast<uint16_t>(kBandH));
    fb_.get_raw_buffer()[static_cast<size_t>(8) * kW + 16] = WHITE; // 帧缓冲 (16,8)

    const auto kernel = make_kernel(2);
    std::vector<uint16_t> scratch(aurora::gpu::scratch_words_for(kernel.radius, 16));
    ASSERT_EQ(do_blur(0, static_cast<int16_t>(kBandH), 16, 16, kernel, scratch.data(), scratch.size()),
              BlurRectResult::ok);

    EXPECT_LT((pixel(16, 8) >> 11u) & 0x1Fu, 0x1Fu);
    EXPECT_GT((pixel(17, 8) >> 11u) & 0x1Fu, 0u);
    EXPECT_EQ(pixel(0, 8), BLACK); // 未因 offset 二次平移而画到 x=0
}

TEST_F(RendererBlurTest, RejectsRoiCrossingBandClipWithoutWriting) {
    renderer_.set_clip_rect(0, 0, kW, 5); // 只放行本带前 5 行
    fb_.get_raw_buffer()[static_cast<size_t>(3) * kW + 3] = WHITE;

    const auto kernel = make_kernel(2);
    std::vector<uint16_t> scratch(aurora::gpu::scratch_words_for(kernel.radius, 16));
    EXPECT_EQ(do_blur(0, 0, 16, 16, kernel, scratch.data(), scratch.size()), BlurRectResult::crosses_band_edge);
    EXPECT_EQ(pixel(3, 3), WHITE); // 拒绝时不写任何像素
}

TEST_F(RendererBlurTest, RoiExactlyInsideClipIsAccepted) {
    renderer_.set_clip_rect(0, 0, 16, 16);
    std::fill(fb_.get_raw_buffer(), fb_.get_raw_buffer() + kW * kBandH, TEST_COLOR);

    const auto kernel = make_kernel(1);
    std::vector<uint16_t> scratch(aurora::gpu::scratch_words_for(kernel.radius, 16));
    EXPECT_EQ(do_blur(0, 0, 16, 16, kernel, scratch.data(), scratch.size()), BlurRectResult::ok);
    EXPECT_EQ(pixel(8, 8), TEST_COLOR);
}

TEST_F(RendererBlurTest, InvalidInputsDoNotWritePixels) {
    fb_.get_raw_buffer()[static_cast<size_t>(5) * kW + 5] = WHITE;
    const auto kernel = make_kernel(2);
    std::vector<uint16_t> scratch(aurora::gpu::scratch_words_for(kernel.radius, 16));

    EXPECT_EQ(do_blur(0, 0, 16, 16, aurora::gpu::BlurKernel{}, scratch.data(), scratch.size()),
              BlurRectResult::invalid_kernel);
    EXPECT_EQ(do_blur(0, 0, 0, 16, kernel, scratch.data(), scratch.size()), BlurRectResult::empty_region);
    EXPECT_EQ(do_blur(0, 0, 16, 16, kernel, scratch.data(), 1u), BlurRectResult::insufficient_scratch);
    EXPECT_EQ(do_blur(0, 0, 16, 16, kernel, nullptr, 0u), BlurRectResult::insufficient_scratch);
    EXPECT_EQ(pixel(5, 5), WHITE);
}

// =============================================================================
// 2. BlurredBackdropView：跨条带无缝
// =============================================================================
class BackdropTest : public ::testing::Test {
protected:
    static constexpr uint16_t BW = 32;
    static constexpr uint16_t BH = 70; // 跨两条带（35 + 35）

    void SetUp() override {
        src_.assign(static_cast<size_t>(BW) * BH, BLACK);
        // 一条跨越 35 行带边界的水平亮带：若逐带各自 clamp 模糊，
        // y=34/35 交界处必然出现亮度突变。
        for (uint16_t y = 33; y <= 37; ++y) {
            for (uint16_t x = 0; x < BW; ++x) {
                src_[static_cast<size_t>(y) * BW + x] = WHITE;
            }
        }
        buf_ = src_;
        view_ = std::make_unique<UI::BlurredBackdropView>(0, 0, BW, BH, buf_.data(), buf_.size());
    }

    // 模拟 UiManager 的一条带：返回该带帧缓冲，调用方逐像素比对。
    static BandFrameBuffer render_band(UI::BlurredBackdropView& view, uint16_t top) {
        BandFrameBuffer fb{};
        UI::UIRenderer r(fb);
        std::fill(fb.get_raw_buffer(), fb.get_raw_buffer() + kW * kBandH, BLACK);
        r.clear_clip_rect();
        r.set_clip_rect(0, 0, kW, kBandH);
        r.set_band_origin(top);
        view.draw(r);
        return fb;
    }

    std::vector<uint16_t> src_;
    std::vector<uint16_t> buf_;
    std::unique_ptr<UI::BlurredBackdropView> view_;
};

TEST_F(BackdropTest, CrossBandBlitMatchesWholeBufferBlurExactly) {
    ASSERT_TRUE(view_->set_source(src_.data()));
    const auto kernel = make_kernel(2);
    std::vector<uint16_t> scratch(aurora::gpu::scratch_words_for(kernel.radius, BW));
    ASSERT_EQ(view_->blur(kernel, scratch.data(), scratch.size()), aurora::gpu::BlurResult::ok);
    ASSERT_TRUE(view_->is_prepared());

    const std::vector<uint16_t> expected = buf_; // 整块离屏模糊的 golden

    for (uint16_t band = 0; band < 2u; ++band) {
        const uint16_t top = static_cast<uint16_t>(band * kBandH);
        BandFrameBuffer fb = render_band(*view_, top);
        for (uint16_t row = 0; row < kBandH; ++row) {
            const uint16_t src_row = static_cast<uint16_t>(top + row);
            for (uint16_t x = 0; x < BW; ++x) {
                EXPECT_EQ(fb.get_raw_buffer()[static_cast<size_t>(row) * kW + x],
                          expected[static_cast<size_t>(src_row) * BW + x])
                    << "band=" << band << " row=" << row << " x=" << x;
            }
            EXPECT_EQ(fb.get_raw_buffer()[static_cast<size_t>(row) * kW + BW], BLACK)
                << "backdrop must not paint beyond its width";
        }
    }
}

TEST_F(BackdropTest, UniformFieldSurvivesBlurAndBothBands) {
    src_.assign(static_cast<size_t>(BW) * BH, TEST_COLOR);
    ASSERT_TRUE(view_->set_source(src_.data()));
    EXPECT_FALSE(view_->is_prepared()); // 换源后必须重新 blur

    const auto kernel = make_kernel(4);
    std::vector<uint16_t> scratch(aurora::gpu::scratch_words_for(kernel.radius, BW));
    ASSERT_EQ(view_->blur(kernel, scratch.data(), scratch.size()), aurora::gpu::BlurResult::ok);

    for (uint16_t band = 0; band < 2u; ++band) {
        BandFrameBuffer fb = render_band(*view_, static_cast<uint16_t>(band * kBandH));
        for (uint16_t row = 0; row < kBandH; ++row) {
            for (uint16_t x = 0; x < BW; ++x) {
                EXPECT_EQ(fb.get_raw_buffer()[static_cast<size_t>(row) * kW + x], TEST_COLOR);
            }
        }
    }
}

TEST_F(BackdropTest, NotPreparedDrawsNothing) {
    BandFrameBuffer fb = render_band(*view_, 0);
    for (size_t i = 0; i < static_cast<size_t>(kW) * kBandH; ++i) {
        EXPECT_EQ(fb.get_raw_buffer()[i], BLACK);
    }
}

TEST_F(BackdropTest, InsufficientBackdropCapacityIsRejected) {
    std::vector<uint16_t> tiny(8u);
    UI::BlurredBackdropView view(0, 0, BW, BH, tiny.data(), tiny.size());
    EXPECT_FALSE(view.set_source(src_.data()));

    const auto kernel = make_kernel(2);
    std::vector<uint16_t> scratch(aurora::gpu::scratch_words_for(kernel.radius, BW));
    EXPECT_EQ(view.blur(kernel, scratch.data(), scratch.size()), aurora::gpu::BlurResult::invalid_surface);
    EXPECT_FALSE(view.is_prepared());
}

TEST_F(BackdropTest, ScratchShortageLeavesViewUnprepared) {
    ASSERT_TRUE(view_->set_source(src_.data()));
    const auto kernel = make_kernel(2);
    std::vector<uint16_t> tiny(1u);
    EXPECT_EQ(view_->blur(kernel, tiny.data(), tiny.size()), aurora::gpu::BlurResult::insufficient_scratch);
    EXPECT_FALSE(view_->is_prepared());

    BandFrameBuffer fb = render_band(*view_, 0);
    for (size_t i = 0; i < static_cast<size_t>(kW) * kBandH; ++i) {
        EXPECT_EQ(fb.get_raw_buffer()[i], BLACK);
    }
}

} // namespace
