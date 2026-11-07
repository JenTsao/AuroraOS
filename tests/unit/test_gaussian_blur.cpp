// =============================================================================
// tests/unit/test_gaussian_blur.cpp
//
// RGB565 高斯模糊算子 + SoftGpuDevice GaussianBlur 命令的主机测试
//
// 测试策略：
//  - 纯算法，x86 上跑，不需要 ARM 工具链或显示硬件
//  - 用独立的「朴素二维浮点卷积」参考实现逐像素比对，验证可分离优化与
//    环形行窗口的等价性（含 dst==src 原地路径）
//  - 失败路径同样覆盖：非法表面/核/ROI、scratch 不足且不得改像素
// =============================================================================

#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "../../drivers/gpu/gaussian_blur.hpp"
#include "../../drivers/gpu/soft_gpu.hpp"

using aurora::gpu::BlurKernel;
using aurora::gpu::BlurResult;
using aurora::gpu::GpuCommand;
using aurora::gpu::GpuOpcode;
using aurora::gpu::SoftGpuDevice;
using aurora::gpu::Surface;
using aurora::gpu::blur_rgb565;
using aurora::gpu::kBlurWeightOne;
using aurora::gpu::make_binomial_kernel;
using aurora::gpu::make_gaussian_kernel;
using aurora::gpu::radius_from_sigma;
using aurora::gpu::scratch_words_for;

namespace {

constexpr uint16_t BLACK = 0x0000u;
constexpr uint16_t WHITE = 0xFFFFu;
constexpr uint16_t RED = 0xF800u;

struct Channels {
    int r;
    int g;
    int b;
};

Channels unpack(uint16_t px) {
    return {static_cast<int>((px >> 11u) & 0x1Fu), static_cast<int>((px >> 5u) & 0x3Fu),
            static_cast<int>(px & 0x1Fu)};
}

int clamp_int(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

// 与产品实现完全独立的参考路径：不可分离的二维卷积 + double 累加。
std::vector<uint16_t> naive_reference(const std::vector<uint16_t>& src, int stride, int x0, int y0, int w, int h,
                                      const BlurKernel& kernel) {
    std::vector<uint16_t> dst = src;
    const int r = kernel.radius;
    for (int y = y0; y < y0 + h; ++y) {
        for (int x = x0; x < x0 + w; ++x) {
            double acc[3] = {0.0, 0.0, 0.0};
            for (int dy = -r; dy <= r; ++dy) {
                const int sy = clamp_int(y + dy, y0, y0 + h - 1);
                for (int dx = -r; dx <= r; ++dx) {
                    const int sx = clamp_int(x + dx, x0, x0 + w - 1);
                    const double weight = (static_cast<double>(kernel.weights[dy + r]) / kBlurWeightOne) *
                                          (static_cast<double>(kernel.weights[dx + r]) / kBlurWeightOne);
                    const Channels c = unpack(src[static_cast<size_t>(sy) * stride + sx]);
                    acc[0] += weight * c.r;
                    acc[1] += weight * c.g;
                    acc[2] += weight * c.b;
                }
            }
            const uint16_t packed = static_cast<uint16_t>((clamp_int(static_cast<int>(std::lround(acc[0])), 0, 0x1F)
                                                           << 11u) |
                                                          (clamp_int(static_cast<int>(std::lround(acc[1])), 0, 0x3F)
                                                           << 5u) |
                                                          clamp_int(static_cast<int>(std::lround(acc[2])), 0, 0x1F));
            dst[static_cast<size_t>(y) * stride + x] = packed;
        }
    }
    return dst;
}

// 确定性伪随机图案，避免依赖 <random>，同时保证 RGB 三通道都被充分激励。
std::vector<uint16_t> make_pattern(int w, int h, uint32_t seed) {
    std::vector<uint16_t> buf(static_cast<size_t>(w) * h, BLACK);
    uint32_t state = seed;
    for (size_t i = 0; i < buf.size(); ++i) {
        state = state * 1664525u + 1013904223u;
        buf[i] = static_cast<uint16_t>(state & 0xFFFFu);
    }
    return buf;
}

class GaussianBlurTest : public ::testing::Test {
protected:
    static constexpr int kW = 16;
    static constexpr int kH = 12;

    void SetUp() override {
        buf_.assign(static_cast<size_t>(kW) * kH, BLACK);
        surface_ = Surface(buf_.data(), kW, kH);
    }

    uint16_t pixel(int x, int y) const {
        return buf_[static_cast<size_t>(y) * kW + x];
    }

    std::vector<uint16_t> buf_;
    Surface surface_{};
};

// =============================================================================
// 1. 核生成
// =============================================================================
TEST(GaussianBlurKernelTest, GaussianKernelIsNormalizedAndSymmetric) {
    BlurKernel kernel{};
    ASSERT_TRUE(make_gaussian_kernel(512u, 6u, kernel)); // σ=2.0
    ASSERT_TRUE(kernel.valid());

    uint64_t sum = 0;
    for (uint8_t i = 0; i < kernel.taps; ++i) {
        sum += kernel.weights[i];
        EXPECT_EQ(kernel.weights[i], kernel.weights[kernel.taps - 1u - i]) << "tap " << i;
    }
    EXPECT_EQ(sum, kBlurWeightOne);
    EXPECT_EQ(kernel.taps, 13u);

    // 高斯核中心最大、向两侧单调不增
    for (uint8_t i = 0; i < kernel.radius; ++i) {
        EXPECT_GE(kernel.weights[i + 1u], kernel.weights[i]) << "tap " << i;
        EXPECT_GE(kernel.weights[kernel.taps - 2u - i], kernel.weights[kernel.taps - 1u - i]);
    }
    EXPECT_GT(kernel.weights[kernel.radius], kernel.weights[0]);
}

TEST(GaussianBlurKernelTest, GaussianKernelMatchesFloatReference) {
    BlurKernel kernel{};
    const uint16_t sigma_q8 = 512u; // σ = 2.0
    ASSERT_TRUE(make_gaussian_kernel(sigma_q8, 6u, kernel));

    const double sigma = static_cast<double>(sigma_q8) / 256.0;
    double raw[13] = {};
    double sum = 0.0;
    for (int t = 0; t < 13; ++t) {
        const int d = t - 6;
        raw[t] = std::exp(-static_cast<double>(d) * d / (2.0 * sigma * sigma));
        sum += raw[t];
    }
    for (int t = 0; t < 13; ++t) {
        const double expected = raw[t] / sum * kBlurWeightOne;
        // 整数 exp 近似 + 归一化残差补给中心，整体误差按 Q16 的 1% 容忍
        EXPECT_NEAR(static_cast<double>(kernel.weights[t]), expected, kBlurWeightOne * 0.01)
            << "tap " << t;
    }
}

TEST(GaussianBlurKernelTest, BinomialKernelHasExactPascalCoefficients) {
    BlurKernel kernel{};
    ASSERT_TRUE(make_binomial_kernel(2u, kernel));
    ASSERT_TRUE(kernel.valid());
    ASSERT_EQ(kernel.taps, 5u);
    // C(4,k) = 1,4,6,4,1；和 16 → Q16 权重
    const uint32_t expected[5] = {4096u, 16384u, 24576u, 16384u, 4096u};
    for (uint8_t i = 0; i < 5u; ++i) {
        EXPECT_EQ(kernel.weights[i], expected[i]) << "tap " << i;
    }
    uint64_t sum = 0;
    for (uint8_t i = 0; i < 5u; ++i) sum += kernel.weights[i];
    EXPECT_EQ(sum, kBlurWeightOne);
}

TEST(GaussianBlurKernelTest, RadiusFromSigmaClampsToResourceLimit) {
    EXPECT_EQ(radius_from_sigma(0u), 0u);
    EXPECT_EQ(radius_from_sigma(256u), 3u);  // 3σ = 3
    EXPECT_EQ(radius_from_sigma(512u), 6u);  // 3σ = 6
    EXPECT_EQ(radius_from_sigma(2048u), 8u); // σ=8 被钳制到 kMaxBlurRadius
}

TEST(GaussianBlurKernelTest, InvalidKernelArgumentsAreRejected) {
    BlurKernel kernel{};
    EXPECT_FALSE(make_gaussian_kernel(0u, 3u, kernel));
    EXPECT_FALSE(make_gaussian_kernel(512u, 0u, kernel));
    EXPECT_FALSE(make_gaussian_kernel(512u, 9u, kernel));
    EXPECT_FALSE(make_binomial_kernel(0u, kernel));
    EXPECT_FALSE(make_binomial_kernel(9u, kernel));
    EXPECT_FALSE(BlurKernel{}.valid());
}

TEST(GaussianBlurKernelTest, ScratchWordsScalesWithKernelAndWidth) {
    EXPECT_EQ(scratch_words_for(2u, 16u), 5u * 16u * 3u);
    EXPECT_EQ(scratch_words_for(8u, 1u), 17u * 3u);
    EXPECT_EQ(scratch_words_for(0u, 16u), 0u);
    EXPECT_EQ(scratch_words_for(2u, 0u), 0u);
    EXPECT_EQ(scratch_words_for(2u, 0xFFFFFFFFu), 0u); // 溢出必须显式失败
}

// =============================================================================
// 2. 算法本体
// =============================================================================
TEST_F(GaussianBlurTest, UniformFieldIsPreservedExactly) {
    // 非饱和、三通道都非零的颜色，最容易暴露归一化/量化误差
    const uint16_t color = 0x1234u;
    std::fill(buf_.begin(), buf_.end(), color);

    BlurKernel kernel{};
    ASSERT_TRUE(make_gaussian_kernel(512u, 6u, kernel));
    std::vector<uint16_t> scratch(scratch_words_for(kernel.radius, kW));

    EXPECT_EQ(blur_rgb565(surface_, 0, 0, kW, kH, kernel, scratch.data(), scratch.size()), BlurResult::ok);
    for (uint16_t px : buf_) {
        EXPECT_EQ(px, color);
    }
}

TEST_F(GaussianBlurTest, ImpulseSpreadsToNeighbours) {
    buf_[static_cast<size_t>(6) * kW + 8] = WHITE;

    BlurKernel kernel{};
    ASSERT_TRUE(make_binomial_kernel(2u, kernel));
    std::vector<uint16_t> scratch(scratch_words_for(kernel.radius, kW));
    ASSERT_EQ(blur_rgb565(surface_, 0, 0, kW, kH, kernel, scratch.data(), scratch.size()), BlurResult::ok);

    const Channels center = unpack(pixel(8, 6));
    const Channels right = unpack(pixel(9, 6));
    const Channels below = unpack(pixel(8, 7));
    EXPECT_LT(center.r, 0x1F);
    EXPECT_GT(right.r, 0);
    EXPECT_GT(below.g, 0);
    EXPECT_EQ(pixel(0, 0), BLACK); // 远离脉冲处仍为黑
}

TEST_F(GaussianBlurTest, RoiEdgesUseClampAndNeverTouchOutside) {
    const int x0 = 4, y0 = 3, w = 7, h = 6;
    for (int y = y0; y < y0 + h; ++y) {
        for (int x = x0; x < x0 + w; ++x) {
            buf_[static_cast<size_t>(y) * kW + x] = WHITE;
        }
    }
    const std::vector<uint16_t> before = buf_;

    BlurKernel kernel{};
    ASSERT_TRUE(make_binomial_kernel(3u, kernel));
    std::vector<uint16_t> scratch(scratch_words_for(kernel.radius, w));
    ASSERT_EQ(blur_rgb565(surface_, x0, y0, w, h, kernel, scratch.data(), scratch.size()), BlurResult::ok);

    // ROI 内是均匀白场，clamp-to-edge 后仍应全白；ROI 外一像素不变
    for (int y = y0; y < y0 + h; ++y) {
        for (int x = x0; x < x0 + w; ++x) {
            EXPECT_EQ(pixel(x, y), WHITE) << "inside (" << x << "," << y << ")";
        }
    }
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            const bool inside = (x >= x0 && x < x0 + w && y >= y0 && y < y0 + h);
            if (!inside) {
                EXPECT_EQ(pixel(x, y), before[static_cast<size_t>(y) * kW + x])
                    << "outside (" << x << "," << y << ")";
            }
        }
    }
}

TEST_F(GaussianBlurTest, NegativeOriginAndOversizedRectAreClipped) {
    buf_ = make_pattern(kW, kH, 0xC0FFEEu);
    surface_ = Surface(buf_.data(), kW, kH);

    BlurKernel kernel{};
    ASSERT_TRUE(make_binomial_kernel(2u, kernel));
    std::vector<uint16_t> scratch(scratch_words_for(kernel.radius, kW));

    BlurResult clipped = BlurResult::invalid_kernel;
    EXPECT_NO_FATAL_FAILURE(clipped = blur_rgb565(surface_, -5, -5, 0xFFFFFFFFu, 0xFFFFFFFFu, kernel,
                                                  scratch.data(), scratch.size()));
    EXPECT_EQ(clipped, BlurResult::ok);
}

// 核心等价性验证：随机图案 × 多种核 × 多种 ROI，产品实现必须逐像素匹配
// 独立的朴素二维浮点卷积（允许 Q8 中间量化带来的 ±1 通道误差）。
TEST_F(GaussianBlurTest, SeparableInPlaceMatchesNaive2DReference) {
    struct Case {
        bool gaussian;
        uint16_t sigma_q8;
        uint8_t radius;
        int x0, y0, w, h;
    };
    const Case cases[] = {
        {true, 256u, 3u, 0, 0, kW, kH},      // σ=1，整屏
        {true, 512u, 6u, 2, 1, 11, 9},      // σ=2，内部 ROI
        {false, 0u, 1u, 0, 0, kW, kH},      // 二项式 r=1
        {false, 0u, 4u, 3, 2, 9, 8},        // 二项式 r=4，ROI
        {false, 0u, 8u, 0, 0, kW, kH},      // 最大核，ROI 小于 2r+1 的极端情形
    };

    for (const Case& c : cases) {
        BlurKernel kernel{};
        if (c.gaussian) {
            ASSERT_TRUE(make_gaussian_kernel(c.sigma_q8, c.radius, kernel));
        } else {
            ASSERT_TRUE(make_binomial_kernel(c.radius, kernel));
        }

        buf_ = make_pattern(kW, kH, 0x1234ABCDu + c.radius);
        surface_ = Surface(buf_.data(), kW, kH);
        const std::vector<uint16_t> expected = naive_reference(buf_, kW, c.x0, c.y0, c.w, c.h, kernel);
        std::vector<uint16_t> scratch(scratch_words_for(kernel.radius, c.w));
        ASSERT_EQ(blur_rgb565(surface_, c.x0, c.y0, c.w, c.h, kernel, scratch.data(), scratch.size()),
                  BlurResult::ok);

        for (int y = c.y0; y < c.y0 + c.h; ++y) {
            for (int x = c.x0; x < c.x0 + c.w; ++x) {
                const Channels got = unpack(pixel(x, y));
                const Channels want = unpack(expected[static_cast<size_t>(y) * kW + x]);
                EXPECT_LE(std::abs(got.r - want.r), 1) << "R case r=" << c.radius << " (" << x << "," << y << ")";
                EXPECT_LE(std::abs(got.g - want.g), 1) << "G case r=" << c.radius << " (" << x << "," << y << ")";
                EXPECT_LE(std::abs(got.b - want.b), 1) << "B case r=" << c.radius << " (" << x << "," << y << ")";
            }
        }
    }
}

// =============================================================================
// 3. 失败路径：明确错误码且不污染像素
// =============================================================================
TEST_F(GaussianBlurTest, InvalidInputsReturnExplicitResultAndDoNotWrite) {
    BlurKernel kernel{};
    ASSERT_TRUE(make_binomial_kernel(2u, kernel));
    std::vector<uint16_t> scratch(scratch_words_for(kernel.radius, kW));
    buf_[static_cast<size_t>(5) * kW + 5] = RED;

    Surface invalid{};
    EXPECT_EQ(blur_rgb565(invalid, 0, 0, kW, kH, kernel, scratch.data(), scratch.size()),
              BlurResult::invalid_surface);

    EXPECT_EQ(blur_rgb565(surface_, 0, 0, kW, kH, BlurKernel{}, scratch.data(), scratch.size()),
              BlurResult::invalid_kernel);

    EXPECT_EQ(blur_rgb565(surface_, 0, 0, 0u, 10u, kernel, scratch.data(), scratch.size()),
              BlurResult::empty_region);
    EXPECT_EQ(blur_rgb565(surface_, 1000, 1000, 10u, 10u, kernel, scratch.data(), scratch.size()),
              BlurResult::empty_region);

    EXPECT_EQ(blur_rgb565(surface_, 0, 0, kW, kH, kernel, nullptr, scratch.size()),
              BlurResult::insufficient_scratch);
    EXPECT_EQ(blur_rgb565(surface_, 0, 0, kW, kH, kernel, scratch.data(), 1u),
              BlurResult::insufficient_scratch);

    EXPECT_EQ(pixel(5, 5), RED); // 所有失败路径都未改动缓冲
}

// =============================================================================
// 4. SoftGpuDevice 命令集成
// =============================================================================
TEST(GaussianBlurCommandTest, OpcodeValuesRemainBackwardCompatible) {
    EXPECT_EQ(static_cast<uint8_t>(GpuOpcode::FillRect), 1u);
    EXPECT_EQ(static_cast<uint8_t>(GpuOpcode::Blit), 2u);
    EXPECT_EQ(static_cast<uint8_t>(GpuOpcode::Blend), 3u);
    EXPECT_EQ(static_cast<uint8_t>(GpuOpcode::GaussianBlur), 4u);
}

TEST(GaussianBlurCommandTest, GaussianCommandBlursTargetSurface) {
    constexpr int W = 16, H = 12;
    std::vector<uint16_t> buf(static_cast<size_t>(W) * H, BLACK);
    buf[static_cast<size_t>(6) * W + 8] = WHITE;
    Surface dst(buf.data(), W, H);

    const uint8_t radius = radius_from_sigma(512u);
    std::vector<uint16_t> scratch(scratch_words_for(radius, W));
    BlurResult result = BlurResult::invalid_kernel;

    GpuCommand cmd{};
    cmd.opcode = GpuOpcode::GaussianBlur;
    cmd.dst_surface = &dst;
    cmd.dst_x = 0;
    cmd.dst_y = 0;
    cmd.width = W;
    cmd.height = H;
    cmd.args.blur.sigma_q8 = 512u;
    cmd.args.blur.scratch = scratch.data();
    cmd.args.blur.scratch_words = static_cast<uint32_t>(scratch.size());
    cmd.args.blur.out_result = &result;

    SoftGpuDevice gpu;
    ASSERT_TRUE(gpu.submit(&cmd, 1));
    EXPECT_EQ(result, BlurResult::ok);
    EXPECT_LT(unpack(buf[static_cast<size_t>(6) * W + 8]).r, 0x1F);
    EXPECT_GT(unpack(buf[static_cast<size_t>(6) * W + 9]).r, 0);
}

TEST(GaussianBlurCommandTest, BinomialCommandUsesExplicitRadius) {
    constexpr int W = 8, H = 8;
    std::vector<uint16_t> buf(static_cast<size_t>(W) * H, 0x1234u);
    Surface dst(buf.data(), W, H);
    std::vector<uint16_t> scratch(scratch_words_for(2u, W));
    BlurResult result = BlurResult::invalid_kernel;

    GpuCommand cmd{};
    cmd.opcode = GpuOpcode::GaussianBlur;
    cmd.dst_surface = &dst;
    cmd.width = W;
    cmd.height = H;
    cmd.args.blur.radius = 2u;
    cmd.args.blur.scratch = scratch.data();
    cmd.args.blur.scratch_words = static_cast<uint32_t>(scratch.size());
    cmd.args.blur.out_result = &result;

    SoftGpuDevice gpu;
    ASSERT_TRUE(gpu.submit(&cmd, 1));
    EXPECT_EQ(result, BlurResult::ok);
    for (uint16_t px : buf) {
        EXPECT_EQ(px, 0x1234u); // 均匀场不变
    }
}

TEST(GaussianBlurCommandTest, ScratchShortageIsReportedAndPixelsUntouched) {
    constexpr int W = 8, H = 8;
    std::vector<uint16_t> buf(static_cast<size_t>(W) * H, RED);
    Surface dst(buf.data(), W, H);
    std::vector<uint16_t> scratch(8u, 0u);
    BlurResult result = BlurResult::ok;

    GpuCommand cmd{};
    cmd.opcode = GpuOpcode::GaussianBlur;
    cmd.dst_surface = &dst;
    cmd.width = W;
    cmd.height = H;
    cmd.args.blur.radius = 2u;
    cmd.args.blur.scratch = scratch.data();
    cmd.args.blur.scratch_words = static_cast<uint32_t>(scratch.size());
    cmd.args.blur.out_result = &result;

    SoftGpuDevice gpu;
    ASSERT_TRUE(gpu.submit(&cmd, 1)); // 单条失败不中止整批
    EXPECT_EQ(result, BlurResult::insufficient_scratch);
    for (uint16_t px : buf) {
        EXPECT_EQ(px, RED);
    }
}

TEST(GaussianBlurCommandTest, SkippedInvalidSurfaceStillReportsViaOutResult) {
    constexpr int W = 8, H = 8;
    std::vector<uint16_t> buf(static_cast<size_t>(W) * H, RED);
    Surface invalid{};
    std::vector<uint16_t> scratch(scratch_words_for(2u, W));
    BlurResult result = BlurResult::ok;

    GpuCommand cmds[2]{};
    cmds[0].opcode = GpuOpcode::GaussianBlur;
    cmds[0].dst_surface = nullptr; // 守卫跳过路径
    cmds[0].width = W;
    cmds[0].height = H;
    cmds[0].args.blur.radius = 2u;
    cmds[0].args.blur.scratch = scratch.data();
    cmds[0].args.blur.scratch_words = static_cast<uint32_t>(scratch.size());
    cmds[0].args.blur.out_result = &result;

    cmds[1].opcode = GpuOpcode::GaussianBlur;
    cmds[1].dst_surface = &invalid; // 无效表面同样被守卫跳过
    cmds[1].width = W;
    cmds[1].height = H;
    cmds[1].args.blur.radius = 2u;
    cmds[1].args.blur.scratch = scratch.data();
    cmds[1].args.blur.scratch_words = static_cast<uint32_t>(scratch.size());
    cmds[1].args.blur.out_result = &result;

    SoftGpuDevice gpu;
    ASSERT_TRUE(gpu.submit(cmds, 2));
    EXPECT_EQ(result, BlurResult::invalid_surface); // 不是初值 ok——跳过必须可回读
    for (uint16_t px : buf) {
        EXPECT_EQ(px, RED);
    }

    // out_result == nullptr 时守卫跳过不得崩溃，像素同样不动
    cmds[0].args.blur.out_result = nullptr;
    EXPECT_TRUE(gpu.submit(&cmds[0], 1));
    for (uint16_t px : buf) {
        EXPECT_EQ(px, RED);
    }
}

TEST(GaussianBlurCommandTest, BatchWithOtherCommandsKeepsExistingSemantics) {
    constexpr int W = 16, H = 12;
    std::vector<uint16_t> buf(static_cast<size_t>(W) * H, BLACK);
    Surface dst(buf.data(), W, H);
    std::vector<uint16_t> scratch(scratch_words_for(1u, W));
    BlurResult result = BlurResult::invalid_kernel;

    GpuCommand cmds[3]{};
    cmds[0].opcode = GpuOpcode::FillRect;
    cmds[0].dst_surface = &dst;
    cmds[0].width = W;
    cmds[0].height = H;
    cmds[0].args.fill.color = RED;

    cmds[1].opcode = GpuOpcode::GaussianBlur;
    cmds[1].dst_surface = &dst;
    cmds[1].width = W;
    cmds[1].height = H;
    cmds[1].args.blur.radius = 1u;
    cmds[1].args.blur.scratch = scratch.data();
    cmds[1].args.blur.scratch_words = static_cast<uint32_t>(scratch.size());
    cmds[1].args.blur.out_result = &result;

    cmds[2].opcode = static_cast<GpuOpcode>(0xFEu); // 未知操作码：跳过

    SoftGpuDevice gpu;
    ASSERT_TRUE(gpu.submit(cmds, 3));
    EXPECT_EQ(result, BlurResult::ok);
    for (uint16_t px : buf) {
        EXPECT_EQ(px, RED); // 均匀红场模糊后仍是红
    }
}

} // namespace
