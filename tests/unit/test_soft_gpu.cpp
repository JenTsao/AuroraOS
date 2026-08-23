// =============================================================================
// tests/unit/test_soft_gpu.cpp
//
// auroraOS SoftGpuDevice（软件 GPU）单元测试
//
// 重点覆盖毕业改进项：
//  - int64 裁剪数学：负坐标、uint32 极值尺寸不回绕
//  - 自拷贝 memmove 语义：同缓冲重叠 blit 不破坏未读像素
//  - 资源上限与参数级拒绝
// =============================================================================

#include <gtest/gtest.h>
#include <cstdint>
#include <cstring>
#include <vector>

#include "../../drivers/gpu/soft_gpu.hpp"

using aurora::gpu::GpuCommand;
using aurora::gpu::GpuOpcode;
using aurora::gpu::SoftGpuDevice;
using aurora::gpu::Surface;

namespace {

constexpr uint16_t RED = 0xF800u;
constexpr uint16_t GREEN = 0x07E0u;
constexpr uint16_t BLUE = 0x001Fu;
constexpr uint16_t BLACK = 0x0000u;
constexpr uint16_t WHITE = 0xFFFFu;

class SoftGpuTest : public ::testing::Test {
protected:
    static constexpr uint32_t kW = 32u;
    static constexpr uint32_t kH = 16u;

    void SetUp() override {
        std::memset(dst_buf_, 0, sizeof(dst_buf_));
        std::memset(src_buf_, 0, sizeof(src_buf_));
        dst_ = Surface(dst_buf_, kW, kH);
        src_ = Surface(src_buf_, kW, kH);
    }

    uint16_t pixel(const Surface& s, uint32_t x, uint32_t y) const {
        return s.get_buffer()[y * s.get_width() + x];
    }

    uint16_t dst_buf_[kW * kH]{};
    uint16_t src_buf_[kW * kH]{};
    Surface dst_{};
    Surface src_{};
    SoftGpuDevice gpu_;
};

GpuCommand make_fill(Surface* dst, int32_t x, int32_t y, uint32_t w, uint32_t h, uint16_t color) {
    GpuCommand cmd{};
    cmd.opcode = GpuOpcode::FillRect;
    cmd.dst_surface = dst;
    cmd.dst_x = x;
    cmd.dst_y = y;
    cmd.width = w;
    cmd.height = h;
    cmd.args.fill.color = color;
    return cmd;
}

GpuCommand make_blit(Surface* dst, int32_t dx, int32_t dy, Surface* src, int32_t sx, int32_t sy,
                     uint32_t w, uint32_t h) {
    GpuCommand cmd{};
    cmd.opcode = GpuOpcode::Blit;
    cmd.dst_surface = dst;
    cmd.dst_x = dx;
    cmd.dst_y = dy;
    cmd.width = w;
    cmd.height = h;
    cmd.args.blit.src_surface = src;
    cmd.args.blit.src_x = sx;
    cmd.args.blit.src_y = sy;
    return cmd;
}

GpuCommand make_blend(Surface* dst, int32_t dx, int32_t dy, Surface* src, int32_t sx, int32_t sy,
                      uint32_t w, uint32_t h, uint8_t alpha) {
    GpuCommand cmd = make_blit(dst, dx, dy, src, sx, sy, w, h);
    cmd.opcode = GpuOpcode::Blend;
    cmd.args.blend.alpha = alpha;
    return cmd;
}

} // namespace

// =============================================================================
// FillRect
// =============================================================================
TEST_F(SoftGpuTest, FillRect_BasicAndExactBounds) {
    GpuCommand cmd = make_fill(&dst_, 2, 1, 4, 3, RED);
    ASSERT_TRUE(gpu_.submit(&cmd, 1));

    // 精确区域着色
    for (uint32_t y = 1; y < 4; ++y) {
        for (uint32_t x = 2; x < 6; ++x) {
            ASSERT_EQ(pixel(dst_, x, y), RED) << "at (" << x << "," << y << ")";
        }
    }
    // 边界外不受影响
    EXPECT_EQ(pixel(dst_, 1, 1), BLACK);
    EXPECT_EQ(pixel(dst_, 6, 1), BLACK);
    EXPECT_EQ(pixel(dst_, 2, 0), BLACK);
    EXPECT_EQ(pixel(dst_, 2, 4), BLACK);
}

TEST_F(SoftGpuTest, FillRect_NegativeOriginClippedExactly) {
    GpuCommand cmd = make_fill(&dst_, -5, -3, 10, 10, GREEN);
    ASSERT_TRUE(gpu_.submit(&cmd, 1));

    // 有效交集为 (0,0)-(4,6)
    EXPECT_EQ(pixel(dst_, 0, 0), GREEN);
    EXPECT_EQ(pixel(dst_, 4, 6), GREEN);
    EXPECT_EQ(pixel(dst_, 5, 6), BLACK);
    EXPECT_EQ(pixel(dst_, 4, 7), BLACK);
}

TEST_F(SoftGpuTest, FillRect_OffscreenIsNoOp) {
    GpuCommand cmd = make_fill(&dst_, 100, 100, 8, 8, BLUE);
    ASSERT_TRUE(gpu_.submit(&cmd, 1));
    // 全缓冲保持黑色
    for (uint32_t y = 0; y < kH; ++y) {
        for (uint32_t x = 0; x < kW; ++x) {
            ASSERT_EQ(pixel(dst_, x, y), BLACK);
        }
    }
}

// 极值尺寸不得回绕（int64 中间量验证）
TEST_F(SoftGpuTest, FillRect_HugeSizeClampedSafely) {
    GpuCommand cmd = make_fill(&dst_, 30, 14, 0xFFFFFFFFu, 0xFFFFFFFFu, WHITE);
    ASSERT_TRUE(gpu_.submit(&cmd, 1));
    EXPECT_EQ(pixel(dst_, 31, 15), WHITE); // 右下角被填充
    EXPECT_EQ(pixel(dst_, 29, 13), BLACK); // 区域外未动
    EXPECT_EQ(pixel(dst_, 0, 0), BLACK);
}

// =============================================================================
// Blit（不同缓冲）
// =============================================================================
TEST_F(SoftGpuTest, Blit_DifferentBuffersBasic) {
    for (uint32_t i = 0; i < kW * kH; ++i) {
        src_buf_[i] = static_cast<uint16_t>(i & 0xFF);
    }

    GpuCommand cmd = make_blit(&dst_, 5, 3, &src_, 0, 0, 4, 2);
    ASSERT_TRUE(gpu_.submit(&cmd, 1));

    EXPECT_EQ(pixel(dst_, 5, 3), src_buf_[0]);
    EXPECT_EQ(pixel(dst_, 8, 3), src_buf_[3]);
    EXPECT_EQ(pixel(dst_, 5, 4), src_buf_[kW]);
    EXPECT_EQ(pixel(dst_, 9, 3), BLACK); // 区域外
}

TEST_F(SoftGpuTest, Blit_NegativeCoordsClippedBothSides) {
    // 源图样：整屏渐变
    for (uint32_t i = 0; i < kW * kH; ++i) {
        src_buf_[i] = static_cast<uint16_t>((i * 7) & 0xFFFF);
    }

    // 目标 (-3,-2)，源 (28,14)，尺寸 8x6 → 双侧裁剪后核对逐点映射：
    // dst(x,y) = src(28 + x + 3, 14 + y + 2)
    GpuCommand cmd = make_blit(&dst_, -3, -2, &src_, 28, 14, 8, 6);
    ASSERT_TRUE(gpu_.submit(&cmd, 1));

    for (uint32_t y = 0; y < 3; ++y) {
        for (uint32_t x = 0; x < 4; ++x) {
            const uint32_t sx = 28 + x + 3;
            const uint32_t sy = 14 + y + 2;
            if (sx < kW && sy < kH) {
                ASSERT_EQ(pixel(dst_, x, y), src_buf_[sy * kW + sx]) << "at (" << x << "," << y << ")";
            }
        }
    }
}

// =============================================================================
// Blit 自拷贝 memmove 语义（毕业核心项）
// =============================================================================
TEST_F(SoftGpuTest, BlitSelfOverlap_MoveDownRequiresReverseRows) {
    // 建立唯一图案，向下移动 4 行（重叠 12 行）。
    // 朴素正向逐行拷贝会先用第 0 行覆盖第 4 行，导致数据重复。
    std::vector<uint16_t> before(kW * kH);
    for (uint32_t i = 0; i < kW * kH; ++i) {
        before[i] = static_cast<uint16_t>(i + 1);
        dst_buf_[i] = before[i];
    }

    Surface self(dst_buf_, kW, kH);
    GpuCommand cmd = make_blit(&self, 0, 4, &self, 0, 0, kW, 12);
    ASSERT_TRUE(gpu_.submit(&cmd, 1));

    // 参考结果：memmove 语义
    std::vector<uint16_t> expect = before;
    std::memmove(expect.data() + 4 * kW, expect.data(), 12 * kW * sizeof(uint16_t));

    for (uint32_t i = 0; i < kW * kH; ++i) {
        ASSERT_EQ(dst_buf_[i], expect[i]) << "index " << i;
    }
}

TEST_F(SoftGpuTest, BlitSelfOverlap_MoveUpForwardRowsOk) {
    std::vector<uint16_t> before(kW * kH);
    for (uint32_t i = 0; i < kW * kH; ++i) {
        before[i] = static_cast<uint16_t>(i * 3 + 7);
        dst_buf_[i] = before[i];
    }

    Surface self(dst_buf_, kW, kH);
    GpuCommand cmd = make_blit(&self, 0, 0, &self, 0, 4, kW, 12);
    ASSERT_TRUE(gpu_.submit(&cmd, 1));

    std::vector<uint16_t> expect = before;
    std::memmove(expect.data(), expect.data() + 4 * kW, 12 * kW * sizeof(uint16_t));

    for (uint32_t i = 0; i < kW * kH; ++i) {
        ASSERT_EQ(dst_buf_[i], expect[i]) << "index " << i;
    }
}

TEST_F(SoftGpuTest, BlitSelfOverlap_SameRowShiftRightRequiresReverseCols) {
    std::vector<uint16_t> before(kW * kH);
    for (uint32_t i = 0; i < kW * kH; ++i) {
        before[i] = static_cast<uint16_t>(i ^ 0x55);
        dst_buf_[i] = before[i];
    }

    Surface self(dst_buf_, kW, kH);
    // 同一行内右移 3 像素（列重叠）
    GpuCommand cmd = make_blit(&self, 3, 5, &self, 0, 5, kW - 3, 1);
    ASSERT_TRUE(gpu_.submit(&cmd, 1));

    std::vector<uint16_t> expect = before;
    std::memmove(expect.data() + 5 * kW + 3, expect.data() + 5 * kW, (kW - 3) * sizeof(uint16_t));

    for (uint32_t i = 0; i < kW * kH; ++i) {
        ASSERT_EQ(dst_buf_[i], expect[i]) << "index " << i;
    }
}

// =============================================================================
// Blend
// =============================================================================
TEST_F(SoftGpuTest, Blend_Alpha255MatchesBlit_Alpha0NoChange) {
    for (uint32_t i = 0; i < kW * kH; ++i) {
        src_buf_[i] = RED;
    }

    GpuCommand opaque = make_blend(&dst_, 0, 0, &src_, 0, 0, 8, 8, 255);
    ASSERT_TRUE(gpu_.submit(&opaque, 1));
    EXPECT_EQ(pixel(dst_, 4, 4), RED);

    // alpha=0 全透：无任何写入
    GpuCommand transparent = make_blend(&dst_, 10, 10, &src_, 0, 0, 8, 4, 0);
    ASSERT_TRUE(gpu_.submit(&transparent, 1));
    EXPECT_EQ(pixel(dst_, 12, 12), BLACK);
}

TEST_F(SoftGpuTest, Blend_MidAlphaKnownValue) {
    // dst=黑(0,0,0)，src=纯红(31,0,0)，alpha=128 → r=(31*128+0*127)/255 ≈ 15
    dst_buf_[0] = BLACK;
    src_buf_[0] = RED;

    GpuCommand cmd = make_blend(&dst_, 0, 0, &src_, 0, 0, 1, 1, 128);
    ASSERT_TRUE(gpu_.submit(&cmd, 1));

    const uint16_t out = pixel(dst_, 0, 0);
    EXPECT_EQ((out >> 11) & 0x1F, 15u);
    EXPECT_EQ((out >> 5) & 0x3F, 0u);
    EXPECT_EQ(out & 0x1F, 0u);
}

// =============================================================================
// 批处理 / 参数拒绝 / 容错
// =============================================================================
TEST_F(SoftGpuTest, Submit_ParameterRejection) {
    EXPECT_FALSE(gpu_.submit(nullptr, 1));       // 空数组
    GpuCommand cmd = make_fill(&dst_, 0, 0, 1, 1, RED);
    EXPECT_FALSE(gpu_.submit(&cmd, 0));          // 零数量
    EXPECT_FALSE(gpu_.submit(&cmd, SoftGpuDevice::kMaxBatchCommands + 1)); // 超上限
}

TEST_F(SoftGpuTest, Submit_InvalidCommandsSkippedBatchContinues) {
    GpuCommand cmds[3];
    cmds[0] = make_fill(&dst_, 0, 0, 2, 2, RED);

    GpuCommand bad = make_fill(nullptr, 0, 0, 2, 2, GREEN); // 空 dst
    cmds[1] = bad;

    GpuCommand unknown = make_fill(&dst_, 0, 0, 2, 2, BLUE);
    unknown.opcode = static_cast<GpuOpcode>(0xEE); // 未知操作码
    cmds[2] = unknown;

    // 第 0 条应生效，坏命令被跳过不崩溃
    ASSERT_TRUE(gpu_.submit(cmds, 3));
    EXPECT_EQ(pixel(dst_, 1, 1), RED);
}

TEST_F(SoftGpuTest, InvalidSurfaceIsNoOp) {
    Surface empty{}; // 无效表面
    GpuCommand cmd = make_fill(&empty, 0, 0, 8, 8, RED);
    ASSERT_TRUE(gpu_.submit(&cmd, 1)); // 提交成功，操作为无操作
}
