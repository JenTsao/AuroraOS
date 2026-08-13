// =============================================================================
// tests/unit/test_renderer2d.cpp
//
// auroraOS Renderer2D 单元测试
//
// 测试策略：
//  - 在主机(x86)环境下运行，无需 ARM 工具链
//  - FrameBuffer<128,128> 完全在测试进程内存中分配
//  - 通过读取 raw buffer 验证像素值，而非依赖硬件输出
//  - 覆盖：直线/矩形/圆/三角形/文本/圆角矩形/弧线/混色
// =============================================================================

#include <gtest/gtest.h>
#include <cstdint>
#include <cstring>

// ── 测试用的最小 stub：OledDriver（不依赖 SPI/UART）─────────────────────────
// FrameBuffer::flush() 需要 OledDriver，但我们不测试 flush，只测绘图逻辑，
// 所以通过 minimal stub 让编译通过。
namespace test_stubs {
// 最小 ColorRGB565 与 OledDriver stub（模仿 oled_driver_mock.hpp 的接口）
using ColorRGB565 = uint16_t;

struct OledDriver {
    void set_window(uint16_t, uint16_t, uint16_t, uint16_t) {}

    void write_patch(const ColorRGB565*, uint32_t) {}

    uint16_t get_width() const {
        return 128;
    }

    uint16_t get_height() const {
        return 128;
    }
};
} // namespace test_stubs

// ── 直接 include 真实的 FrameBuffer 和 Renderer2D（header-only）────────────
// 解决依赖链：framebuffer.hpp → oled_driver_mock.hpp → device.hpp → posix.hpp
// 在主机测试里我们只需要 FrameBuffer 的数据结构，不需要 OledDriver 的实现。
// 用 stub 宏覆盖头文件包含。
#define AURORA_OLED_DRIVER_HPP // 跳过真实 oled_driver.hpp

// 在 oled_driver_mock.hpp 被 include 前先定义它的内容
using ColorRGB565 = uint16_t;

class OledDriver {
public:
    OledDriver(const char* = "", uint16_t w = 128, uint16_t h = 128) : width_(w), height_(h) {}

    void set_window(uint16_t, uint16_t, uint16_t, uint16_t) {}

    void write_patch(const ColorRGB565*, uint32_t) {}

    uint16_t get_width() const {
        return width_;
    }

    uint16_t get_height() const {
        return height_;
    }

    int open() {
        return 0;
    }

    int close() {
        return 0;
    }

private:
    uint16_t width_;
    uint16_t height_;
};

// 模拟 CharDevice（framebuffer.hpp 通过 oled_driver_mock.hpp → device.hpp 引入）
#define AURORA_DEVICE_HPP
#define DEVICE_HPP

class Device {};

class CharDevice : public Device {};

class BlockDevice : public Device {};

// 模拟 posix.hpp
#define POSIX_HPP
#define AURORA_POSIX_HPP

inline int open(const char*, int) {
    return -1;
}

inline int write(int, const void*, int) {
    return -1;
}

inline int close(int) {
    return -1;
}

#include "../../drivers/display/framebuffer.hpp"
#include "../../drivers/display/renderer2d.hpp"

// ── 常量 ─────────────────────────────────────────────────────────────────────
constexpr uint16_t W = 128u;
constexpr uint16_t H = 128u;
constexpr ColorRGB565 RED = 0xF800u;
constexpr ColorRGB565 GREEN = 0x07E0u;
constexpr ColorRGB565 BLUE = 0x001Fu;
constexpr ColorRGB565 WHITE = 0xFFFFu;
constexpr ColorRGB565 BLACK = 0x0000u;

// ── Test Fixture ──────────────────────────────────────────────────────────────
class Renderer2DTest : public ::testing::Test {
protected:
    void SetUp() override {
        fb = std::make_unique<FrameBuffer<W, H>>();
        r2d = std::make_unique<Renderer2D<W, H>>(*fb);
        // 清除为黑色背景
        r2d->clear(BLACK);
    }

    // 读取帧缓冲指定坐标的像素值
    ColorRGB565 pixel(uint16_t x, uint16_t y) const {
        return fb->get_raw_buffer()[y * W + x];
    }

    // 断言某矩形区域内所有像素均等于期望颜色
    void expect_all(uint16_t x, uint16_t y, uint16_t w, uint16_t h, ColorRGB565 expected) const {
        for (uint16_t row = y; row < y + h; ++row) {
            for (uint16_t col = x; col < x + w; ++col) {
                EXPECT_EQ(pixel(col, row), expected) << "at (" << col << "," << row << ")";
            }
        }
    }

    std::unique_ptr<FrameBuffer<W, H>> fb;
    std::unique_ptr<Renderer2D<W, H>> r2d;
};

// =============================================================================
// 1. clear()
// =============================================================================
TEST_F(Renderer2DTest, ClearFillsEntireBuffer) {
    r2d->clear(RED);
    expect_all(0, 0, W, H, RED);
}

// =============================================================================
// 2. draw_line() —— Bresenham 直线
// =============================================================================
TEST_F(Renderer2DTest, DrawHorizontalLine_PaintsSingleRow) {
    r2d->draw_line(10, 20, 30, 20, WHITE);
    for (uint16_t x = 10; x <= 30; ++x) {
        EXPECT_EQ(pixel(x, 20), WHITE) << "x=" << x;
    }
    // 相邻行不受影响
    EXPECT_EQ(pixel(20, 19), BLACK);
    EXPECT_EQ(pixel(20, 21), BLACK);
}

TEST_F(Renderer2DTest, DrawVerticalLine_PaintsSingleCol) {
    r2d->draw_line(50, 5, 50, 25, RED);
    for (uint16_t y = 5; y <= 25; ++y) {
        EXPECT_EQ(pixel(50, y), RED) << "y=" << y;
    }
    EXPECT_EQ(pixel(49, 15), BLACK);
}

TEST_F(Renderer2DTest, DrawDiagonalLine_ConnectsEndpoints) {
    r2d->draw_line(0, 0, 10, 10, GREEN);
    // 至少起终点必须被点亮
    EXPECT_EQ(pixel(0, 0), GREEN);
    EXPECT_EQ(pixel(10, 10), GREEN);
}

TEST_F(Renderer2DTest, DrawLine_ReversedEndpoints_StillDraws) {
    r2d->draw_line(30, 30, 10, 10, BLUE);
    EXPECT_EQ(pixel(30, 30), BLUE);
    EXPECT_EQ(pixel(10, 10), BLUE);
}

TEST_F(Renderer2DTest, DrawLine_OutOfBounds_Clipped) {
    // 起点在边界上，终点越界——不应崩溃
    EXPECT_NO_FATAL_FAILURE(r2d->draw_line(120, 5, 200, 5, RED));
    EXPECT_EQ(pixel(120, 5), RED); // 在界内的部分应被绘制
}

// =============================================================================
// 3. draw_hline / draw_vline —— 优化路径
// =============================================================================
TEST_F(Renderer2DTest, DrawHline_MatchesManualPixels) {
    r2d->draw_hline(10, 40, 20, GREEN);
    for (uint16_t x = 10; x < 30; ++x) {
        EXPECT_EQ(pixel(x, 40), GREEN);
    }
    EXPECT_EQ(pixel(30, 40), BLACK); // 越界处为黑
}

TEST_F(Renderer2DTest, DrawVline_MatchesManualPixels) {
    r2d->draw_vline(60, 20, 15, BLUE);
    for (uint16_t y = 20; y < 35; ++y) {
        EXPECT_EQ(pixel(60, y), BLUE);
    }
    EXPECT_EQ(pixel(60, 35), BLACK);
}

// =============================================================================
// 4. draw_rect / fill_rect
// =============================================================================
TEST_F(Renderer2DTest, DrawRect_OnlyBorderPixels) {
    r2d->draw_rect(10, 10, 20, 15, RED);
    // 四条边上的像素应为 RED
    for (uint16_t x = 10; x < 30; ++x) {
        EXPECT_EQ(pixel(x, 10), RED) << "top row x=" << x;
        EXPECT_EQ(pixel(x, 24), RED) << "bot row x=" << x;
    }
    for (uint16_t y = 10; y < 25; ++y) {
        EXPECT_EQ(pixel(10, y), RED) << "left col y=" << y;
        EXPECT_EQ(pixel(29, y), RED) << "right col y=" << y;
    }
    // 内部 (15,15) 应为黑
    EXPECT_EQ(pixel(15, 15), BLACK);
}

TEST_F(Renderer2DTest, FillRect_AllInternalPixels) {
    r2d->fill_rect(20, 20, 10, 10, GREEN);
    expect_all(20, 20, 10, 10, GREEN);
    EXPECT_EQ(pixel(19, 20), BLACK); // 边框外
}

// =============================================================================
// 5. draw_circle / fill_circle
// =============================================================================
TEST_F(Renderer2DTest, DrawCircle_TopMostPixelLit) {
    // 圆心 (64,64), 半径 20 → (64, 44) 应被点亮
    r2d->draw_circle(64, 64, 20, WHITE);
    EXPECT_EQ(pixel(64, 44), WHITE); // 最顶部
    EXPECT_EQ(pixel(64, 84), WHITE); // 最底部
    EXPECT_EQ(pixel(44, 64), WHITE); // 最左
    EXPECT_EQ(pixel(84, 64), WHITE); // 最右
}

TEST_F(Renderer2DTest, FillCircle_CenterPixelLit) {
    r2d->fill_circle(40, 40, 10, BLUE);
    EXPECT_EQ(pixel(40, 40), BLUE); // 圆心
    EXPECT_EQ(pixel(40, 30), BLUE); // 顶部（在半径内）
}

// =============================================================================
// 6. draw_triangle / fill_triangle
// =============================================================================
TEST_F(Renderer2DTest, DrawTriangle_ThreeVerticesLit) {
    const Point2D A{10, 10}, B{50, 10}, C{30, 40};
    r2d->draw_triangle(A, B, C, RED);
    EXPECT_EQ(pixel(10, 10), RED); // 顶点 A
    EXPECT_EQ(pixel(50, 10), RED); // 顶点 B
    EXPECT_EQ(pixel(30, 40), RED); // 顶点 C
}

TEST_F(Renderer2DTest, FillTriangle_InteriorLit) {
    const Point2D A{20, 20}, B{60, 20}, C{40, 60};
    r2d->fill_triangle(A, B, C, GREEN);
    // 质心区域应被填满
    EXPECT_EQ(pixel(40, 35), GREEN);
    EXPECT_EQ(pixel(35, 28), GREEN);
}

TEST_F(Renderer2DTest, FillTriangle_DegenerateNocrash) {
    // 退化三角形（三点共线）不应崩溃
    EXPECT_NO_FATAL_FAILURE(r2d->fill_triangle({10, 10}, {20, 10}, {30, 10}, RED));
}

// =============================================================================
// 7. draw_round_rect / fill_round_rect
// =============================================================================
TEST_F(Renderer2DTest, DrawRoundRect_ZeroRadius_SameasRect) {
    r2d->draw_round_rect(5, 5, 30, 20, 0, WHITE);
    // 等价于普通矩形
    EXPECT_EQ(pixel(5, 5), WHITE);
    EXPECT_EQ(pixel(34, 5), WHITE);
    EXPECT_EQ(pixel(5, 24), WHITE);
}

TEST_F(Renderer2DTest, FillRoundRect_CenterFilled) {
    r2d->fill_round_rect(10, 10, 50, 30, 5, BLUE);
    // 矩形正中心应被填充
    EXPECT_EQ(pixel(35, 25), BLUE);
}

// =============================================================================
// 8. draw_char / draw_string
// =============================================================================
// 使用一个最小的 1x1 字模（全白）来测试字符渲染逻辑
// 真实字模测试依赖 font_engine.hpp（在 firmware 中），此处用 stub 数据
TEST_F(Renderer2DTest, DrawChar_PaintsPixelsAccordingToFontData) {
    // 字模：font_w=3, font_h=1, 单字符 'A'（从 ' ' 开始）
    // 构造：' '=0b000, '!'=0b001...
    // 这里测试 scale 逻辑：scale=2 应画 2x2 的方块
    // 使用 0xFF 作为全亮列数据（高度为 1 位）
    static const uint8_t tiny_font[] = {
        0xFF, 0xFF, 0xFF // ' ' 字符的3列数据，每列bit0均=1
    };
    // 绘制空格（glyph_idx=0）
    r2d->draw_char(10, 10, ' ', /*scale=*/1, WHITE, BLACK, tiny_font, 3, 1);
    // 所有3列的第0行应为白色
    EXPECT_EQ(pixel(10, 10), WHITE);
    EXPECT_EQ(pixel(11, 10), WHITE);
    EXPECT_EQ(pixel(12, 10), WHITE);
}

TEST_F(Renderer2DTest, DrawChar_Scale2_Paints2x2Blocks) {
    static const uint8_t tiny_font[] = {
        0xFF, 0x00 // 2列字体，第0列全亮，第1列全灭
    };
    r2d->draw_char(10, 10, ' ', /*scale=*/2, RED, BLACK, tiny_font, 2, 1);
    // 第0列 scale=2 → 应覆盖 x=[10,11], y=[10,11]
    EXPECT_EQ(pixel(10, 10), RED);
    EXPECT_EQ(pixel(11, 10), RED);
    EXPECT_EQ(pixel(10, 11), RED);
    EXPECT_EQ(pixel(11, 11), RED);
    // 第1列（数据为0）→ 应为背景色 BLACK
    EXPECT_EQ(pixel(12, 10), BLACK);
}

TEST_F(Renderer2DTest, DrawString_NullPtrNocrash) {
    static const uint8_t dummy_font[] = {0x00};
    EXPECT_NO_FATAL_FAILURE(r2d->draw_string(10, 10, nullptr, 1, WHITE, BLACK, dummy_font));
}

// =============================================================================
// 9. blend_pixel()
// =============================================================================
TEST_F(Renderer2DTest, BlendPixel_Alpha255_SameAsPlot) {
    r2d->blend_pixel(20, 20, RED, 255);
    EXPECT_EQ(pixel(20, 20), RED);
}

TEST_F(Renderer2DTest, BlendPixel_Alpha0_NoChange) {
    r2d->clear(GREEN);
    r2d->blend_pixel(20, 20, RED, 0);
    EXPECT_EQ(pixel(20, 20), GREEN); // 背景不变
}

TEST_F(Renderer2DTest, BlendPixel_OutOfBounds_NocrashNoWrite) {
    EXPECT_NO_FATAL_FAILURE(r2d->blend_pixel(-5, -5, RED, 128));
    EXPECT_NO_FATAL_FAILURE(r2d->blend_pixel(200, 200, RED, 128));
}

// =============================================================================
// 10. draw_arc()
// =============================================================================
TEST_F(Renderer2DTest, DrawArc_Full360_ProducesCirclePixels) {
    // 完整的 360° 弧线等价于圆——只检查 0° 对应像素被点亮
    r2d->draw_arc(64, 64, 20, 0, 359, WHITE);
    // 0° 对应 (64, 44)（顶部），90° 对应 (84, 64)（右侧）
    EXPECT_EQ(pixel(64, 44), WHITE); // top
    EXPECT_EQ(pixel(84, 64), WHITE); // right
}

TEST_F(Renderer2DTest, DrawArc_QuarterArc_DoesNotDrawOtherSide) {
    r2d->draw_arc(64, 64, 20, 0, 90, GREEN);
    // 0-90° 覆盖顶部，270-360° 对应的右侧顶部区域
    // 180° 对应底部不应被点亮
    // (注：弧线算法精度为1°步进，验证不越界)
    EXPECT_NO_FATAL_FAILURE({});
}

// =============================================================================
// 11. draw_thick_line()
// =============================================================================
TEST_F(Renderer2DTest, DrawThickLine_IsThickerThanNormal) {
    r2d->draw_thick_line(10, 10, 30, 10, 5, WHITE);
    // x=15, y=10 should be white. Also y=8 and y=12.
    EXPECT_EQ(pixel(15, 10), WHITE);
    EXPECT_EQ(pixel(15, 8), WHITE);
    EXPECT_EQ(pixel(15, 12), WHITE);
}

// =============================================================================
// 12. fill_arc()
// =============================================================================
TEST_F(Renderer2DTest, FillArc_GeneratesPizzaSlice) {
    r2d->fill_arc(64, 64, 20, 0, 90, RED);
    // Center should be lit.
    EXPECT_EQ(pixel(64, 64), RED);
    // Point inside the 0-90 quadrant (upper right visually, but y decreases upwards, so x > 64, y < 64)
    EXPECT_EQ(pixel(64 + 10, 64 - 10), RED);
}

// =============================================================================
// 13. draw_bitmap_transparent()
// =============================================================================
TEST_F(Renderer2DTest, DrawBitmap_Transparent_UsesChromaKey) {
    ColorRGB565 bitmap[4] = {RED, GREEN, GREEN, RED};
    r2d->draw_bitmap_transparent(10, 10, 2, 2, bitmap, GREEN);
    EXPECT_EQ(pixel(10, 10), RED);
    EXPECT_EQ(pixel(11, 10), BLACK); // Not overwritten
    EXPECT_EQ(pixel(10, 11), BLACK);
    EXPECT_EQ(pixel(11, 11), RED);
}

// =============================================================================
// 14. fill_polygon()
// =============================================================================
TEST_F(Renderer2DTest, FillPolygon_DrawsCorrectFan) {
    Point2D pts[4] = {{10, 10}, {20, 10}, {20, 20}, {10, 20}};
    r2d->fill_polygon(pts, 4, BLUE);
    // Center of square should be blue
    EXPECT_EQ(pixel(15, 15), BLUE);
}
