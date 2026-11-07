#ifndef AURORA_FONT_ENGINE_HPP
#define AURORA_FONT_ENGINE_HPP

#include <stdint.h>
#include "../../drivers/display/st7789_driver.hpp"
#include "../../drivers/display/framebuffer.hpp"

// ========================================================
// 颜色与尺寸定义 (RGB565 格式)
// ========================================================
enum class FontColor : uint16_t {
    WHITE = 0xFFFF,
    BLACK = 0x0000,
    RED = 0xF800,
    GREEN = 0x07E0,
    GRAY = 0x8410,
    BLUE = 0x001F
};

enum class FontSize : uint8_t {
    SMALL,      // 约 8px 高度 (scale 1)
    MEDIUM,     // 约 16px 高度 (scale 2)
    EXTRA_LARGE // 约 28px 高度 (scale 4)
};

// ========================================================
// 字体字模描述结构体 (对齐 LVGL 导出格式)
// ========================================================
struct GlyphInfo {
    uint8_t width;       // 字符宽度
    uint8_t height;      // 字符高度
    int8_t x_offset;     // X 轴绘制偏移
    int8_t y_offset;     // Y 轴绘制偏移
    uint32_t bitmap_idx; // 在一维字模数组中的起始索引
};

struct FontDef {
    const uint8_t* bitmap_data; // 存储在 Flash 中的字模像素点阵数据 (1bpp 或 4bpp)
    const GlyphInfo* glyphs;    // 字符映射表
    uint8_t line_height;        // 行高
    char start_char;            // 支持的起始 ASCII 码 (如 ' ')
    char end_char;              // 支持的结束 ASCII 码 (如 '~')
};

// 5x7 字模点阵数据已拆至 ui/font5x7_data.hpp（text_view 等 UI 层只需数据表，
// 直接包含数据头即可，不必依赖本文件传递引入显示驱动层）。
#include "../../ui/font5x7_data.hpp"

// ========================================================
// 位图渲染引擎核心
// ========================================================
class FontEngine {
public:
    // ========================================================
    // 渲染单个字符 (支持 Buffer 写入)
    // buffer_height: 缓冲区行数，用于 Y 轴上界裁剪；0 表示不裁剪
    // （调用方必须保证传入真实高度，否则低位越界写会踩坏相邻内存）
    // ========================================================
    static uint16_t draw_char(uint16_t x, int16_t y, char c, FontColor color, FontSize size = FontSize::MEDIUM,
                              uint16_t* buffer = nullptr, uint16_t buffer_width = 0, uint16_t buffer_height = 0) {
        if (c < ' ' || c > '~')
            return 0;
        if (!buffer || buffer_width == 0)
            return 0;

        uint16_t char_idx = c - ' ';
        const uint8_t* char_data = &font5x7_data[char_idx * 5];

        int scale = (size == FontSize::EXTRA_LARGE) ? 4 : ((size == FontSize::MEDIUM) ? 2 : 1);

        for (int col = 0; col < 5; col++) {
            uint8_t col_data = char_data[col];
            for (int row = 0; row < 7; row++) {
                if (col_data & (1 << row)) {
                    for (int sx = 0; sx < scale; sx++) {
                        for (int sy = 0; sy < scale; sy++) {
                            int16_t px = x + col * scale + sx;
                            int16_t py = y + row * scale + sy;
                            // 同时约束上下边界：py 超出缓冲行数会越界写坏相邻内存
                            if (px >= 0 && px < buffer_width && py >= 0 &&
                                (buffer_height == 0 || py < static_cast<int16_t>(buffer_height))) {
                                buffer[py * buffer_width + px] = static_cast<uint16_t>(color);
                            }
                        }
                    }
                }
            }
        }
        return 5 * scale + 2; // 返回宽度 + 间距
    }

    // ========================================================
    // FrameBuffer 引用重载：写入后自动标记脏区域。
    // 直写 raw buffer 会绕过 FrameBuffer 的脏矩形追踪，若调用方忘记
    // 手动 mark_dirty()，flush() 会因 dirty 为空而跳过，导致文字永不显示。
    // 此重载在内部完成标记，从根源消除该隐患（见 framebuffer.hpp 注释）。
    // ========================================================
    template <uint16_t W, uint16_t H>
    static uint16_t draw_char(uint16_t x, int16_t y, char c, FontColor color, FontSize size, FrameBuffer<W, H>& fb) {
        const uint16_t adv = draw_char(x, y, c, color, size, fb.get_raw_buffer(), W, H);
        if (adv > 0 && y >= 0) {
            const int scale = (size == FontSize::EXTRA_LARGE) ? 4 : ((size == FontSize::MEDIUM) ? 2 : 1);
            uint16_t w = static_cast<uint16_t>(5 * scale);
            uint16_t h = static_cast<uint16_t>(7 * scale);
            // 裁剪到帧缓冲边界，避免脏矩形越界
            if (x >= W)
                return adv;
            if (x + w > W)
                w = W - x;
            if (static_cast<uint16_t>(y) + h > H)
                h = H - static_cast<uint16_t>(y);
            if (w > 0 && h > 0)
                fb.mark_dirty(x, static_cast<uint16_t>(y), w, h);
        }
        return adv;
    }

    // ========================================================
    // DMA 分片直接推送渲染 (无整块 buffer)
    // ========================================================
    static uint16_t draw_char_dma(uint16_t x, int16_t y, char c, FontColor fg_color, FontColor bg_color,
                                  const FontDef& font) {
        if (c < font.start_char || c > font.end_char)
            return 0;

        uint16_t char_idx = c - font.start_char;
        const GlyphInfo& glyph = font.glyphs[char_idx];
        const uint8_t* bitmap = &font.bitmap_data[glyph.bitmap_idx];

        uint16_t width = glyph.width;
        uint16_t height = glyph.height;
        // 如果是空格或不可见字符，直接返回步进宽度
        if (width == 0 || height == 0)
            return glyph.width + (font.line_height / 4);

        // 处理坐标偏移
        uint16_t draw_x = x + glyph.x_offset;
        uint16_t draw_y = y + glyph.y_offset;

        // 获取驱动实例并设定硬件写入窗口
        auto& driver = St7789Driver::instance();
        driver.set_window(draw_x, draw_y, draw_x + width - 1, draw_y + height - 1);

        // 分片缓冲区：每次处理一行 (安全分配在栈上)
        uint16_t line_buffer[192];

        uint32_t bit_idx = 0;
        for (int row = 0; row < height; row++) {
            for (int col = 0; col < width; col++) {
                uint32_t byte_idx = bit_idx / 8;
                uint8_t bit_offset = bit_idx % 8;
                // cppcheck-suppress objectIndex // 静态字模数据，边界由 glyph 位图表保证
                bool is_set = (bitmap[byte_idx] & (0x80 >> bit_offset)) != 0;
                line_buffer[col] = is_set ? static_cast<uint16_t>(fg_color) : static_cast<uint16_t>(bg_color);
                bit_idx++;
            }
            // 将一行数据推送到显存补丁区
            driver.write_patch(line_buffer, width);
        }

        return width + 2; // 返回字符真实宽度 + 固定字符间距
    }

    // ========================================================
    // 渲染字符串
    // ========================================================
    static void draw_string(uint16_t x, int16_t y, const char* str, FontColor color, FontSize size = FontSize::MEDIUM,
                            uint16_t* buffer = nullptr, uint16_t buffer_width = 0, uint16_t buffer_height = 0) {
        if (!str)
            return;
        uint16_t cursor_x = x;

        while (*str) {
            cursor_x += draw_char(cursor_x, y, *str, color, size, buffer, buffer_width, buffer_height);
            str++;
        }
    }

    // FrameBuffer 引用重载：逐字符调用自动标脏版本
    template <uint16_t W, uint16_t H>
    static void draw_string(uint16_t x, int16_t y, const char* str, FontColor color, FontSize size, FrameBuffer<W, H>& fb) {
        if (!str)
            return;
        uint16_t cursor_x = x;
        while (*str) {
            cursor_x += draw_char(cursor_x, y, *str, color, size, fb);
            str++;
        }
    }

    // ========================================================
    // 渲染整数数值 (零内存分配)
    // ========================================================
    static void draw_number(uint16_t x, int16_t y, int32_t num, FontColor color, FontSize size = FontSize::MEDIUM,
                            uint16_t* buffer = nullptr, uint16_t buffer_width = 0, uint16_t buffer_height = 0) {
        char buf[16];
        format_number(buf, num);
        draw_string(x, y, buf, color, size, buffer, buffer_width, buffer_height);
    }

    // FrameBuffer 引用重载
    template <uint16_t W, uint16_t H>
    static void draw_number(uint16_t x, int16_t y, int32_t num, FontColor color, FontSize size, FrameBuffer<W, H>& fb) {
        char buf[16];
        format_number(buf, num);
        draw_string(x, y, buf, color, size, fb);
    }

private:
    // 整数转十进制字符串（结果正序，零内存分配）
    static void format_number(char* buf, int32_t num) {
        int i = 0;
        bool is_neg = false;

        if (num == 0) {
            buf[i++] = '0';
        } else {
            if (num < 0) {
                is_neg = true;
                num = -num;
            }
            while (num > 0) {
                buf[i++] = (num % 10) + '0';
                num /= 10;
            }
            if (is_neg)
                buf[i++] = '-';
        }
        buf[i] = '\0';

        // 逆序排列
        int start = 0;
        int end = i - 1;
        while (start < end) {
            char temp = buf[start];
            buf[start] = buf[end];
            buf[end] = temp;
            start++;
            end--;
        }
    }
};

#endif // AURORA_FONT_ENGINE_HPP
