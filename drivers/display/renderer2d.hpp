// =============================================================================
// drivers/display/renderer2d.hpp
//
// auroraOS 2D 绘图引擎 (Renderer2D)
//
// 设计原则：
//  - 所有绘制操作均作用于 FrameBuffer<W,H>，不直接写硬件寄存器（与驱动解耦）
//  - 零动态内存分配：所有算法均在栈上运算
//  - 遵循 C++ Core Guidelines：Con.1 const/constexpr；ES.45 无魔数；
//    C.46 explicit 构造；F.6 noexcept 纯几何计算
//  - Bresenham 算法用于直线/圆弧（整数运算，无浮点/sqrt 依赖）
//  - 中点算法用于圆和弧
// =============================================================================
#ifndef AURORA_RENDERER2D_HPP
#define AURORA_RENDERER2D_HPP

#include <stdint.h>
#include "framebuffer.hpp"

// =============================================================================
// 基础几何类型
// =============================================================================

struct Point2D {
    int16_t x;
    int16_t y;
};

struct Rect2D {
    int16_t x;
    int16_t y;
    uint16_t w;
    uint16_t h;
};

// =============================================================================
// Renderer2D：模板化 2D 渲染器，绑定到具体尺寸的 FrameBuffer
// =============================================================================

template <uint16_t Width, uint16_t Height> class Renderer2D {
public:
    // Con.1: 构造时绑定帧缓冲，不持有所有权（R.3 raw pointer = non-owning）
    explicit Renderer2D(FrameBuffer<Width, Height>& fb) noexcept : fb_(fb) {}

    // 禁用拷贝/移动（绑定了硬件引用，语义上不可复制）
    Renderer2D(const Renderer2D&) = delete;
    Renderer2D& operator=(const Renderer2D&) = delete;

    // 坐标视口偏移（用于全局平滑转场动画）
    void set_offset(int16_t dx, int16_t dy) noexcept {
        offset_x_ = dx;
        offset_y_ = dy;
    }

    [[nodiscard]] int16_t get_offset_x() const noexcept {
        return offset_x_;
    }

    [[nodiscard]] int16_t get_offset_y() const noexcept {
        return offset_y_;
    }

    // =========================================================================
    // 1. 基础原语：直线（Bresenham 算法，整数运算）
    //    借鉴自经典 Breshenham's Line Drawing Algorithm
    //    ES.45: 避免魔数；F.6: noexcept（纯几何计算不抛出）
    // =========================================================================
    void draw_line(int16_t x0, int16_t y0, int16_t x1, int16_t y1, ColorRGB565 color) noexcept {
        const int16_t dx = abs(x1 - x0);
        const int16_t dy = abs(y1 - y0);
        const int16_t sx = (x0 < x1) ? 1 : -1;
        const int16_t sy = (y0 < y1) ? 1 : -1;
        int16_t err = dx - dy;

        while (true) {
            plot(x0, y0, color);
            if (x0 == x1 && y0 == y1)
                break;
            const int16_t e2 = err * 2;
            if (e2 > -dy) {
                err -= dy;
                x0 += sx;
            }
            if (e2 < dx) {
                err += dx;
                y0 += sy;
            }
        }
    }

    // 垂直线（优化版本，避免 Bresenham 循环额外开销）
    void draw_vline(int16_t x, int16_t y, uint16_t h, ColorRGB565 color) noexcept {
        for (uint16_t i = 0; i < h; ++i) {
            plot(x, static_cast<int16_t>(y + i), color);
        }
    }

    // 水平线（优化版本）
    void draw_hline(int16_t x, int16_t y, uint16_t w, ColorRGB565 color) noexcept {
        for (uint16_t i = 0; i < w; ++i) {
            plot(static_cast<int16_t>(x + i), y, color);
        }
    }

    // =========================================================================
    // 2. 矩形（空心 / 实心）
    // =========================================================================
    void draw_rect(int16_t x, int16_t y, uint16_t w, uint16_t h, ColorRGB565 color) noexcept {
        draw_hline(x, y, w, color);         // 上边
        draw_hline(x, y + h - 1, w, color); // 下边
        draw_vline(x, y, h, color);         // 左边
        draw_vline(x + w - 1, y, h, color); // 右边
    }

    void fill_rect(int16_t x, int16_t y, uint16_t w, uint16_t h, ColorRGB565 color) noexcept {
        x += offset_x_;
        y += offset_y_;

        // 委托给 FrameBuffer 的原生填充（已做越界裁剪）
        if (x >= 0 && y >= 0) {
            fb_.fill_rect(static_cast<uint16_t>(x), static_cast<uint16_t>(y), w, h, color);
        } else {
            // 负坐标时裁剪
            int16_t cx = x, cy = y;
            uint16_t cw = w, ch = h;
            clip_rect(cx, cy, cw, ch);
            if (cw > 0 && ch > 0) {
                fb_.fill_rect(static_cast<uint16_t>(cx), static_cast<uint16_t>(cy), cw, ch, color);
            }
        }
    }

    // =========================================================================
    // 3. 圆角矩形（空心 / 实心）
    //    采用中点圆算法绘制四个角，再连以直线
    // =========================================================================
    void draw_round_rect(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t r, ColorRGB565 color) noexcept {
        if (r == 0) {
            draw_rect(x, y, w, h, color);
            return;
        }
        // 钳制半径：若 r 超过 w/2 或 h/2，w-2*r 和 h-2*r 会发生无符号下溢，
        // 导致 draw_hline/draw_vline 接收到巨大的宽度/高度值。
        if (r > w / 2)
            r = w / 2;
        if (r > h / 2)
            r = h / 2;
        // 上下两条横线
        draw_hline(x + r, y, w - 2 * r, color);
        draw_hline(x + r, y + h - 1, w - 2 * r, color);
        // 左右两条竖线
        draw_vline(x, y + r, h - 2 * r, color);
        draw_vline(x + w - 1, y + r, h - 2 * r, color);
        // 四个圆角（中点圆算法的1/4弧段）
        draw_corner(x + r, y + r, r, 0b0001, color);                 // 左上
        draw_corner(x + w - 1 - r, y + r, r, 0b0010, color);         // 右上
        draw_corner(x + r, y + h - 1 - r, r, 0b0100, color);         // 左下
        draw_corner(x + w - 1 - r, y + h - 1 - r, r, 0b1000, color); // 右下
    }

    void fill_round_rect(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t r, ColorRGB565 color) noexcept {
        if (r == 0) {
            fill_rect(x, y, w, h, color);
            return;
        }
        // 钳制半径，防止 w-2*r / h-2*r 无符号下溢
        if (r > w / 2)
            r = w / 2;
        if (r > h / 2)
            r = h / 2;
        // 中心矩形主体
        fill_rect(x, y + r, w, h - 2 * r, color);
        // 上下两条延伸矩形（连接圆角）
        fill_rect(x + r, y, w - 2 * r, r, color);
        fill_rect(x + r, y + h - r, w - 2 * r, r, color);
        // 四个实心圆角区域
        fill_corner(x + r, y + r, r, 0b0001, color);
        fill_corner(x + w - 1 - r, y + r, r, 0b0010, color);
        fill_corner(x + r, y + h - 1 - r, r, 0b0100, color);
        fill_corner(x + w - 1 - r, y + h - 1 - r, r, 0b1000, color);
    }

    // =========================================================================
    // 4. 圆（空心 / 实心）——中点圆算法（Midpoint Circle Algorithm）
    // =========================================================================
    void draw_circle(int16_t cx, int16_t cy, uint16_t r, ColorRGB565 color) noexcept {
        int16_t xi = 0;
        int16_t yi = static_cast<int16_t>(r);
        int16_t d = static_cast<int16_t>(3 - 2 * static_cast<int16_t>(r));
        while (xi <= yi) {
            plot_circle_octants(cx, cy, xi, yi, color);
            if (d < 0) {
                d += 4 * xi + 6;
            } else {
                d += 4 * (xi - yi) + 10;
                --yi;
            }
            ++xi;
        }
    }

    void fill_circle(int16_t cx, int16_t cy, uint16_t r, ColorRGB565 color) noexcept {
        int16_t xi = 0;
        int16_t yi = static_cast<int16_t>(r);
        int16_t d = static_cast<int16_t>(3 - 2 * static_cast<int16_t>(r));
        while (xi <= yi) {
            // 用对称水平扫描线填充
            draw_hline(cx - yi, cy + xi, static_cast<uint16_t>(2 * yi + 1), color);
            draw_hline(cx - xi, cy + yi, static_cast<uint16_t>(2 * xi + 1), color);
            draw_hline(cx - yi, cy - xi, static_cast<uint16_t>(2 * yi + 1), color);
            draw_hline(cx - xi, cy - yi, static_cast<uint16_t>(2 * xi + 1), color);
            if (d < 0) {
                d += 4 * xi + 6;
            } else {
                d += 4 * (xi - yi) + 10;
                --yi;
            }
            ++xi;
        }
    }

    // =========================================================================
    // 5. 三角形（空心 / 实心）
    //    实心三角形使用扫描线填充算法（Flat-top/Flat-bottom 分解）
    // =========================================================================
    void draw_triangle(Point2D a, Point2D b, Point2D c, ColorRGB565 color) noexcept {
        draw_line(a.x, a.y, b.x, b.y, color);
        draw_line(b.x, b.y, c.x, c.y, color);
        draw_line(c.x, c.y, a.x, a.y, color);
    }

    void fill_triangle(Point2D a, Point2D b, Point2D c, ColorRGB565 color) noexcept {
        // 按 y 坐标排序（冒泡 3 次即可）
        if (a.y > b.y)
            swap(a, b);
        if (b.y > c.y)
            swap(b, c);
        if (a.y > b.y)
            swap(a, b);

        if (a.y == c.y)
            return; // 退化为直线

        // 上半部分（a.y → b.y）
        if (a.y != b.y) {
            scan_fill_triangle_half(a, b, c, true, color);
        }
        // 下半部分（b.y → c.y）
        if (b.y != c.y) {
            scan_fill_triangle_half(a, b, c, false, color);
        }
    }

    // =========================================================================
    // 6. 弧线（Arc）——基于中点圆算法，只绘制特定象限内的弧
    //    octant_mask: bit0=右上, bit1=右下, bit2=左下, bit3=左上
    //    弧线段以两个角度参数定义（整数度，0-359）
    // =========================================================================
    void draw_arc(int16_t cx, int16_t cy, uint16_t r, uint16_t start_deg, uint16_t end_deg,
                  ColorRGB565 color) noexcept {
        // 简化实现：按角度步进采样（精度约 1°），裸机环境避免 sin/cos
        constexpr uint16_t STEPS = 360u;

        const uint16_t norm_start = start_deg % STEPS;
        const uint16_t norm_end = end_deg % STEPS;

        for (uint16_t deg = 0; deg < STEPS; ++deg) {
            // 检查是否在绘制区间内（支持跨 0° 的弧）
            bool in_range = (norm_start <= norm_end) ? (deg >= norm_start && deg <= norm_end)
                                                     : (deg >= norm_start || deg <= norm_end);
            if (!in_range)
                continue;

            const int16_t px = static_cast<int16_t>(cx + sin_approx(deg + 90u, r, SIN_Q8));
            const int16_t py = static_cast<int16_t>(cy - sin_approx(deg, r, SIN_Q8));
            plot(px, py, color);
        }
    }

    // =========================================================================
    // 7. 文本渲染（单字符，支持 scale 放大）
    //    字模格式：5×7 列字模，每字节低位为顶部像素
    //    使用外部 font5x7_data 点阵数据，与 font_engine.hpp 解耦（只依赖数组）
    // =========================================================================
    void draw_char(int16_t x, int16_t y, char c, uint8_t scale, ColorRGB565 fg, ColorRGB565 bg,
                   const uint8_t* font_data, uint8_t font_w = 5u, uint8_t font_h = 7u) noexcept {
        if (c < ' ' || c > '~')
            return; // 只支持可打印 ASCII

        const uint8_t glyph_idx = static_cast<uint8_t>(c - ' ');
        const uint8_t* glyph = font_data + glyph_idx * font_w;

        for (uint8_t col = 0; col < font_w; ++col) {
            uint8_t line = glyph[col];
            for (uint8_t row = 0; row < font_h; ++row) {
                const ColorRGB565 pixel_color = (line & 0x01u) ? fg : bg;
                if (scale == 1u) {
                    plot(x + col, y + row, pixel_color);
                } else {
                    fill_rect(static_cast<int16_t>(x + col * scale), static_cast<int16_t>(y + row * scale), scale,
                              scale, pixel_color);
                }
                line >>= 1u;
            }
        }
    }

    // 字符串渲染（自动换行不支持——RTOS 应用层处理布局）
    void draw_string(int16_t x, int16_t y, const char* str, uint8_t scale, ColorRGB565 fg, ColorRGB565 bg,
                     const uint8_t* font_data, uint8_t font_w = 5u, uint8_t font_h = 7u) noexcept {
        if (!str)
            return;
        constexpr uint8_t CHAR_SPACING = 1u; // ES.45 命名常量
        int16_t cursor_x = x;
        while (*str) {
            draw_char(cursor_x, y, *str, scale, fg, bg, font_data, font_w, font_h);
            cursor_x += static_cast<int16_t>((font_w + CHAR_SPACING) * scale);
            ++str;
        }
    }

    // =========================================================================
    // 8. 像素级操作：混色（Alpha-blending，Q8 定点，无浮点）
    // =========================================================================

    // 将 RGB565 颜色按透明度 alpha(0=全透，255=不透明)混合到帧缓冲
    // F.6: noexcept — 纯整数运算，无异常路径
    void blend_pixel(int16_t x, int16_t y, ColorRGB565 src_color, uint8_t alpha) noexcept {
        x += offset_x_;
        y += offset_y_;
        if (!in_bounds(x, y))
            return;
        if (alpha == 255u) {
            plot(x, y, src_color);
            return;
        }
        if (alpha == 0u)
            return;

        // 从帧缓冲读出目标像素（使用已经应用 offset 的坐标）
        const ColorRGB565 dst = fb_pixel(static_cast<uint16_t>(x), static_cast<uint16_t>(y));
        plot_raw(x, y, blend_rgb565(dst, src_color, alpha));
    }

    // =========================================================================
    // 9. 高级扩展：位图、椭圆、多边形、粗线、扇形填充
    // =========================================================================

    // 位图绘制
    void draw_bitmap(int16_t x, int16_t y, uint16_t w, uint16_t h, const ColorRGB565* bitmap) noexcept {
        if (!bitmap)
            return;
        for (uint16_t row = 0; row < h; ++row) {
            for (uint16_t col = 0; col < w; ++col) {
                plot(x + col, y + row, bitmap[row * w + col]);
            }
        }
    }

    void draw_bitmap_transparent(int16_t x, int16_t y, uint16_t w, uint16_t h, const ColorRGB565* bitmap,
                                 ColorRGB565 key) noexcept {
        if (!bitmap)
            return;
        for (uint16_t row = 0; row < h; ++row) {
            for (uint16_t col = 0; col < w; ++col) {
                ColorRGB565 c = bitmap[row * w + col];
                if (c != key)
                    plot(x + col, y + row, c);
            }
        }
    }

    void draw_bitmap_alpha(int16_t x, int16_t y, uint16_t w, uint16_t h, const ColorRGB565* bitmap,
                           uint8_t alpha) noexcept {
        if (!bitmap)
            return;
        if (alpha == 0)
            return;
        for (uint16_t row = 0; row < h; ++row) {
            for (uint16_t col = 0; col < w; ++col) {
                blend_pixel(static_cast<int16_t>(x + col), static_cast<int16_t>(y + row), bitmap[row * w + col], alpha);
            }
        }
    }

    // 粗线
    void draw_thick_line(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint8_t thickness,
                         ColorRGB565 color) noexcept {
        if (thickness <= 1) {
            draw_line(x0, y0, x1, y1, color);
            return;
        }
        const int16_t dx = abs(x1 - x0);
        const int16_t dy = abs(y1 - y0);
        const int16_t sx = (x0 < x1) ? 1 : -1;
        const int16_t sy = (y0 < y1) ? 1 : -1;
        int16_t err = dx - dy;
        const int16_t half_t = thickness / 2;

        while (true) {
            if (dx > dy)
                draw_vline(x0, static_cast<int16_t>(y0 - half_t), thickness, color);
            else
                draw_hline(static_cast<int16_t>(x0 - half_t), y0, thickness, color);
            if (x0 == x1 && y0 == y1)
                break;
            const int16_t e2 = err * 2;
            if (e2 > -dy) {
                err -= dy;
                x0 += sx;
            }
            if (e2 < dx) {
                err += dx;
                y0 += sy;
            }
        }
    }

    // 多边形
    void draw_polygon(const Point2D* points, uint16_t count, ColorRGB565 color) noexcept {
        if (!points || count < 3)
            return;
        for (uint16_t i = 0; i < count - 1; ++i) {
            draw_line(points[i].x, points[i].y, points[i + 1].x, points[i + 1].y, color);
        }
        draw_line(points[count - 1].x, points[count - 1].y, points[0].x, points[0].y, color);
    }

    void fill_polygon(const Point2D* points, uint16_t count, ColorRGB565 color) noexcept {
        if (!points || count < 3)
            return;
        for (uint16_t i = 1; i < count - 1; ++i) {
            fill_triangle(points[0], points[i], points[i + 1], color);
        }
    }

    // 扇形填充
    void fill_arc(int16_t cx, int16_t cy, uint16_t r, uint16_t start_deg, uint16_t end_deg,
                  ColorRGB565 color) noexcept {
        if (r == 0)
            return;
        uint16_t span = (end_deg >= start_deg) ? (end_deg - start_deg) : (360 - start_deg + end_deg);
        if (span > 360)
            span = 360;
        if (span == 0)
            return;

        Point2D center = {cx, cy};
        Point2D last_p = {static_cast<int16_t>(cx + sin_approx((start_deg + 90u) % 360u, r, SIN_Q8)),
                          static_cast<int16_t>(cy - sin_approx(start_deg % 360u, r, SIN_Q8))};

        const uint16_t step = 5;
        for (uint16_t i = step; i < span + step; i += step) {
            uint16_t d = (i > span) ? span : i;
            uint16_t current_deg = (start_deg + d) % 360u;
            Point2D next_p = {static_cast<int16_t>(cx + sin_approx((current_deg + 90u) % 360u, r, SIN_Q8)),
                              static_cast<int16_t>(cy - sin_approx(current_deg, r, SIN_Q8))};
            fill_triangle(center, last_p, next_p, color);
            last_p = next_p;
            if (d == span)
                break;
        }
    }

    // 椭圆
    void draw_ellipse(int16_t cx, int16_t cy, uint16_t rx, uint16_t ry, ColorRGB565 color) noexcept {
        if (rx == 0 || ry == 0)
            return;
        int32_t rx2 = static_cast<int32_t>(rx) * rx;
        int32_t ry2 = static_cast<int32_t>(ry) * ry;
        int32_t two_rx2 = 2 * rx2;
        int32_t two_ry2 = 2 * ry2;
        int16_t x = 0;
        int16_t y = ry;
        int32_t px = 0;
        int32_t py = two_rx2 * y;

        int32_t p = ry2 - (rx2 * ry) + (rx2 / 4);
        while (px < py) {
            plot(cx + x, cy + y, color);
            plot(cx - x, cy + y, color);
            plot(cx + x, cy - y, color);
            plot(cx - x, cy - y, color);
            x++;
            px += two_ry2;
            if (p < 0) {
                p += ry2 + px;
            } else {
                y--;
                py -= two_rx2;
                p += ry2 + px - py;
            }
        }

        p = ry2 * x * x + ry2 * x + ry2 / 4 + rx2 * (y - 1) * (y - 1) - rx2 * ry2;
        while (y > 0) {
            y--;
            py -= two_rx2;
            if (p > 0) {
                p += rx2 - py;
            } else {
                x++;
                px += two_ry2;
                p += rx2 - py + px;
            }
            plot(cx + x, cy + y, color);
            plot(cx - x, cy + y, color);
            plot(cx + x, cy - y, color);
            plot(cx - x, cy - y, color);
        }
    }

    void fill_ellipse(int16_t cx, int16_t cy, uint16_t rx, uint16_t ry, ColorRGB565 color) noexcept {
        if (rx == 0 || ry == 0)
            return;
        int32_t rx2 = static_cast<int32_t>(rx) * rx;
        int32_t ry2 = static_cast<int32_t>(ry) * ry;
        int32_t two_rx2 = 2 * rx2;
        int32_t two_ry2 = 2 * ry2;
        int16_t x = 0;
        int16_t y = ry;
        int32_t px = 0;
        int32_t py = two_rx2 * y;

        int32_t p = ry2 - (rx2 * ry) + (rx2 / 4);
        int16_t last_y = y;
        while (px < py) {
            x++;
            px += two_ry2;
            if (p < 0) {
                p += ry2 + px;
            } else {
                draw_hline(cx - x, cy + y, x * 2 + 1, color);
                draw_hline(cx - x, cy - y, x * 2 + 1, color);
                y--;
                py -= two_rx2;
                p += ry2 + px - py;
                last_y = y;
            }
        }

        if (y == last_y && y > 0) {
            draw_hline(cx - x, cy + y, x * 2 + 1, color);
            draw_hline(cx - x, cy - y, x * 2 + 1, color);
        }

        p = ry2 * x * x + ry2 * x + ry2 / 4 + rx2 * (y - 1) * (y - 1) - rx2 * ry2;
        while (y > 0) {
            y--;
            py -= two_rx2;
            if (p > 0) {
                p += rx2 - py;
            } else {
                x++;
                px += two_ry2;
                p += rx2 - py + px;
            }
            draw_hline(cx - x, cy + y, x * 2 + 1, color);
            if (y != 0)
                draw_hline(cx - x, cy - y, x * 2 + 1, color);
        }
    }

    // =========================================================================
    // 10. 清屏（委托 FrameBuffer）
    // =========================================================================
    void clear(ColorRGB565 color = 0x0000u) noexcept {
        fb_.clear(color);
    }

private:
    FrameBuffer<Width, Height>& fb_; // 非拥有引用
    int16_t offset_x_{0};
    int16_t offset_y_{0};

    // sin 表：sin(i°) * 256，i=0..90（基于 Q8.8 定点数）
    static constexpr uint16_t SIN_Q8[91] = {
        0,   4,   9,   13,  18,  22,  27,  31,  36,  40,  44,  49,  53,  58,  62,  66,  71,  75,  79,
        83,  88,  92,  96,  100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 139, 143, 147, 150, 154,
        158, 161, 165, 168, 171, 175, 178, 181, 184, 187, 190, 193, 196, 199, 202, 204, 207, 210, 212,
        215, 217, 219, 222, 224, 226, 228, 230, 232, 234, 236, 237, 239, 241, 242, 243, 245, 246, 247,
        248, 249, 250, 251, 252, 253, 254, 254, 255, 255, 255, 256, 256, 256, 256};

    // ── 内部工具函数 ──────────────────────────────────────────────────────────

    // Con.2: const member function（不修改对象状态）
    [[nodiscard]] bool in_bounds(int16_t x, int16_t y) const noexcept {
        return x >= 0 && y >= 0 && static_cast<uint16_t>(x) < Width && static_cast<uint16_t>(y) < Height;
    }

    // 安全打点（越界自动丢弃，不调用 FrameBuffer::set_pixel 的越界检查）
    void plot(int16_t x, int16_t y, ColorRGB565 color) noexcept {
        x += offset_x_;
        y += offset_y_;
        plot_raw(x, y, color);
    }

    // 内部底层无偏液体打点
    void plot_raw(int16_t x, int16_t y, ColorRGB565 color) noexcept {
        if (in_bounds(x, y)) {
            fb_.set_pixel(static_cast<uint16_t>(x), static_cast<uint16_t>(y), color);
        }
    }

    // 读取帧缓冲某像素（用于 blend_pixel）
    [[nodiscard]] ColorRGB565 fb_pixel(uint16_t x, uint16_t y) const noexcept {
        return fb_.get_raw_buffer()[y * Width + x];
    }

    // 裁剪负坐标矩形到可见区域
    void clip_rect(int16_t& x, int16_t& y, uint16_t& w, uint16_t& h) const noexcept {
        if (x < 0) {
            const int16_t cut = -x;
            if (static_cast<int16_t>(w) <= cut) {
                w = 0;
                return;
            }
            w -= static_cast<uint16_t>(cut);
            x = 0;
        }
        if (y < 0) {
            const int16_t cut = -y;
            if (static_cast<int16_t>(h) <= cut) {
                h = 0;
                return;
            }
            h -= static_cast<uint16_t>(cut);
            y = 0;
        }
    }

    // 中点圆算法的 8 个对称点
    void plot_circle_octants(int16_t cx, int16_t cy, int16_t xi, int16_t yi, ColorRGB565 color) noexcept {
        plot(cx + xi, cy + yi, color);
        plot(cx - xi, cy + yi, color);
        plot(cx + xi, cy - yi, color);
        plot(cx - xi, cy - yi, color);
        plot(cx + yi, cy + xi, color);
        plot(cx - yi, cy + xi, color);
        plot(cx + yi, cy - xi, color);
        plot(cx - yi, cy - xi, color);
    }

    // 绘制指定象限的圆角弧线
    // corner_mask bit: 0=左上 1=右上 2=左下 3=右下
    void draw_corner(int16_t cx, int16_t cy, uint16_t r, uint8_t corner_mask, ColorRGB565 color) noexcept {
        int16_t xi = 0;
        int16_t yi = static_cast<int16_t>(r);
        int16_t d = static_cast<int16_t>(3 - 2 * static_cast<int16_t>(r));
        while (xi <= yi) {
            if (corner_mask & 0x01u) {
                plot(cx - xi, cy - yi, color);
                plot(cx - yi, cy - xi, color);
            }
            if (corner_mask & 0x02u) {
                plot(cx + xi, cy - yi, color);
                plot(cx + yi, cy - xi, color);
            }
            if (corner_mask & 0x04u) {
                plot(cx - xi, cy + yi, color);
                plot(cx - yi, cy + xi, color);
            }
            if (corner_mask & 0x08u) {
                plot(cx + xi, cy + yi, color);
                plot(cx + yi, cy + xi, color);
            }
            if (d < 0) {
                d += 4 * xi + 6;
            } else {
                d += 4 * (xi - yi) + 10;
                --yi;
            }
            ++xi;
        }
    }

    // 填充指定象限的实心圆角（扫描线）
    void fill_corner(int16_t cx, int16_t cy, uint16_t r, uint8_t corner_mask, ColorRGB565 color) noexcept {
        int16_t xi = 0;
        int16_t yi = static_cast<int16_t>(r);
        int16_t d = static_cast<int16_t>(3 - 2 * static_cast<int16_t>(r));
        while (xi <= yi) {
            if (corner_mask & 0x01u) { // 左上
                draw_vline(cx - xi, cy - yi, static_cast<uint16_t>(yi - xi + 1), color);
                draw_vline(cx - yi, cy - xi, static_cast<uint16_t>(xi + 1), color);
            }
            if (corner_mask & 0x02u) { // 右上
                draw_vline(cx + xi, cy - yi, static_cast<uint16_t>(yi - xi + 1), color);
                draw_vline(cx + yi, cy - xi, static_cast<uint16_t>(xi + 1), color);
            }
            if (corner_mask & 0x04u) { // 左下
                draw_vline(cx - xi, cy, static_cast<uint16_t>(yi + 1), color);
                draw_vline(cx - yi, cy, static_cast<uint16_t>(xi + 1), color);
            }
            if (corner_mask & 0x08u) { // 右下
                draw_vline(cx + xi, cy, static_cast<uint16_t>(yi + 1), color);
                draw_vline(cx + yi, cy, static_cast<uint16_t>(xi + 1), color);
            }
            if (d < 0) {
                d += 4 * xi + 6;
            } else {
                d += 4 * (xi - yi) + 10;
                --yi;
            }
            ++xi;
        }
    }

    // 扫描线三角形填充（上半/下半分治）
    void scan_fill_triangle_half(Point2D a, Point2D b, Point2D c, bool upper_half, ColorRGB565 color) noexcept {
        // upper: 从 a.y 到 b.y；lower: 从 b.y 到 c.y
        const int16_t y_start = upper_half ? a.y : b.y;
        const int16_t y_end = upper_half ? b.y : c.y;
        const int16_t total_dy = c.y - a.y;
        if (total_dy == 0)
            return;

        for (int16_t y = y_start; y <= y_end; ++y) {
            // 求左右交点
            const int32_t t_all = (y - a.y);
            const int32_t t_half = upper_half ? (y - a.y) : (y - b.y);
            const int32_t dy_all = total_dy;
            const int32_t dy_half = upper_half ? (b.y - a.y) : (c.y - b.y);

            if (dy_all == 0 || dy_half == 0)
                continue;

            int16_t x_left = static_cast<int16_t>(a.x + (c.x - a.x) * t_all / dy_all);
            int16_t x_right;
            if (upper_half) {
                x_right = static_cast<int16_t>(a.x + (b.x - a.x) * t_half / dy_half);
            } else {
                x_right = static_cast<int16_t>(b.x + (c.x - b.x) * t_half / dy_half);
            }
            if (x_left > x_right) {
                int16_t tmp = x_left;
                x_left = x_right;
                x_right = tmp;
            }
            draw_hline(x_left, y, static_cast<uint16_t>(x_right - x_left + 1), color);
        }
    }

    // 弧线近似：根据角度和半径计算坐标偏移（定点 sin 查表）
    // SIN_Q8: sin(i°)*256, i=0..90
    [[nodiscard]] static int16_t sin_approx(uint16_t deg, uint16_t radius, const uint16_t sin_q8[91]) noexcept {
        const uint16_t d = deg % 360u;
        uint16_t lookup;
        if (d <= 90u)
            lookup = d;
        else if (d <= 180u)
            lookup = 180u - d;
        else if (d <= 270u)
            lookup = d - 180u;
        else
            lookup = 360u - d;

        const int32_t raw = static_cast<int32_t>(sin_q8[lookup]) * radius;
        int16_t val = static_cast<int16_t>(raw / 256);

        if (d > 180u)
            val = -val;
        return val;
    }

    // RGB565 双颜色线性混合（alpha: 0=全 dst, 255=全 src）
    [[nodiscard]] static ColorRGB565 blend_rgb565(ColorRGB565 dst, ColorRGB565 src, uint8_t alpha) noexcept {
        // 拆分 R5 G6 B5 分量
        const uint8_t inv = static_cast<uint8_t>(255u - alpha);
        const uint16_t dr = (dst >> 11u) & 0x1Fu;
        const uint16_t dg = (dst >> 5u) & 0x3Fu;
        const uint16_t db = dst & 0x1Fu;
        const uint16_t sr = (src >> 11u) & 0x1Fu;
        const uint16_t sg = (src >> 5u) & 0x3Fu;
        const uint16_t sb = src & 0x1Fu;
        const uint16_t r_out = (dr * inv + sr * alpha) / 255u;
        const uint16_t g_out = (dg * inv + sg * alpha) / 255u;
        const uint16_t b_out = (db * inv + sb * alpha) / 255u;
        return static_cast<ColorRGB565>(((r_out & 0x1Fu) << 11u) | ((g_out & 0x3Fu) << 5u) | (b_out & 0x1Fu));
    }

    // Point2D 原地交换（避免引入 <algorithm>）
    static void swap(Point2D& lhs, Point2D& rhs) noexcept {
        Point2D tmp = lhs;
        lhs = rhs;
        rhs = tmp;
    }

    // 绝对值（避免引入 <cstdlib>）
    [[nodiscard]] static int16_t abs(int16_t v) noexcept {
        return (v < 0) ? -v : v;
    }
};

#endif // AURORA_RENDERER2D_HPP
