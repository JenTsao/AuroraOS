#ifndef AURORA_FONT_ENGINE_HPP
#define AURORA_FONT_ENGINE_HPP

#include <stdint.h>
#include "../../drivers/display/st7789_driver.hpp"

// ========================================================
// 颜色与尺寸定义 (RGB565 格式)
// ========================================================
enum class FontColor : uint16_t {
    WHITE = 0xFFFF,
    BLACK = 0x0000,
    RED   = 0xF800,
    GREEN = 0x07E0,
    GRAY  = 0x8410,
    BLUE  = 0x001F
};

enum class FontSize : uint8_t {
    SMALL,  // 约 8px 高度 (scale 1)
    MEDIUM, // 约 16px 高度 (scale 2)
    EXTRA_LARGE    // 约 28px 高度 (scale 4)
};

// ========================================================
// 字体字模描述结构体 (对齐 LVGL 导出格式)
// ========================================================
struct GlyphInfo {
    uint8_t  width;       // 字符宽度
    uint8_t  height;      // 字符高度
    int8_t   x_offset;    // X 轴绘制偏移
    int8_t   y_offset;    // Y 轴绘制偏移
    uint32_t bitmap_idx;  // 在一维字模数组中的起始索引
};

struct FontDef {
    const uint8_t*   bitmap_data; // 存储在 Flash 中的字模像素点阵数据 (1bpp 或 4bpp)
    const GlyphInfo* glyphs;      // 字符映射表
    uint8_t          line_height; // 行高
    char             start_char;  // 支持的起始 ASCII 码 (如 ' ')
    char             end_char;    // 支持的结束 ASCII 码 (如 '~')
};

// ========================================================
// 5x7 ASCII 字符集数据定义 (从 ' ' (32) 到 '~' (126))
// ========================================================
static const uint8_t font5x7_data[] = {
    0x00, 0x00, 0x00, 0x00, 0x00, // (space)
    0x00, 0x00, 0x5f, 0x00, 0x00, // !
    0x00, 0x07, 0x00, 0x07, 0x00, // "
    0x14, 0x7f, 0x14, 0x7f, 0x14, // #
    0x24, 0x2a, 0x7f, 0x2a, 0x12, // $
    0x23, 0x13, 0x08, 0x64, 0x62, // %
    0x36, 0x49, 0x55, 0x22, 0x50, // &
    0x00, 0x05, 0x03, 0x00, 0x00, // '
    0x00, 0x1c, 0x22, 0x41, 0x00, // (
    0x00, 0x41, 0x22, 0x1c, 0x00, // )
    0x14, 0x08, 0x3e, 0x08, 0x14, // *
    0x08, 0x08, 0x3e, 0x08, 0x08, // +
    0x00, 0x50, 0x30, 0x00, 0x00, // ,
    0x08, 0x08, 0x08, 0x08, 0x08, // -
    0x00, 0x60, 0x60, 0x00, 0x00, // .
    0x20, 0x10, 0x08, 0x04, 0x02, // /
    0x3e, 0x51, 0x49, 0x45, 0x3e, // 0
    0x00, 0x42, 0x7f, 0x40, 0x00, // 1
    0x42, 0x61, 0x51, 0x49, 0x46, // 2
    0x21, 0x41, 0x45, 0x4b, 0x31, // 3
    0x18, 0x14, 0x12, 0x7f, 0x10, // 4
    0x27, 0x45, 0x45, 0x45, 0x39, // 5
    0x3c, 0x4a, 0x49, 0x49, 0x30, // 6
    0x01, 0x71, 0x09, 0x05, 0x03, // 7
    0x36, 0x49, 0x49, 0x49, 0x36, // 8
    0x06, 0x49, 0x49, 0x29, 0x1e, // 9
    0x00, 0x36, 0x36, 0x00, 0x00, // :
    0x00, 0x56, 0x36, 0x00, 0x00, // ;
    0x08, 0x14, 0x22, 0x41, 0x00, // <
    0x14, 0x14, 0x14, 0x14, 0x14, // =
    0x00, 0x41, 0x22, 0x14, 0x08, // >
    0x02, 0x01, 0x51, 0x09, 0x06, // ?
    0x32, 0x49, 0x79, 0x41, 0x3e, // @
    0x7e, 0x11, 0x11, 0x11, 0x7e, // A
    0x7f, 0x49, 0x49, 0x49, 0x36, // B
    0x3e, 0x41, 0x41, 0x41, 0x22, // C
    0x7f, 0x41, 0x41, 0x22, 0x1c, // D
    0x7f, 0x49, 0x49, 0x49, 0x41, // E
    0x7f, 0x09, 0x09, 0x09, 0x01, // F
    0x3e, 0x41, 0x49, 0x49, 0x7a, // G
    0x7f, 0x08, 0x08, 0x08, 0x7f, // H
    0x00, 0x41, 0x7f, 0x41, 0x00, // I
    0x20, 0x40, 0x41, 0x3f, 0x01, // J
    0x7f, 0x08, 0x14, 0x22, 0x41, // K
    0x7f, 0x40, 0x40, 0x40, 0x40, // L
    0x7f, 0x02, 0x0c, 0x02, 0x7f, // M
    0x7f, 0x04, 0x08, 0x10, 0x7f, // N
    0x3e, 0x41, 0x41, 0x41, 0x3e, // O
    0x7f, 0x09, 0x09, 0x09, 0x06, // P
    0x3e, 0x41, 0x51, 0x21, 0x5e, // Q
    0x7f, 0x09, 0x19, 0x29, 0x46, // R
    0x46, 0x49, 0x49, 0x49, 0x31, // S
    0x01, 0x01, 0x7f, 0x01, 0x01, // T
    0x3f, 0x40, 0x40, 0x40, 0x3f, // U
    0x1f, 0x20, 0x40, 0x20, 0x1f, // V
    0x3f, 0x40, 0x38, 0x40, 0x3f, // W
    0x63, 0x14, 0x08, 0x14, 0x63, // X
    0x07, 0x08, 0x70, 0x08, 0x07, // Y
    0x61, 0x51, 0x49, 0x45, 0x43, // Z
    0x00, 0x7f, 0x41, 0x41, 0x00, // [
    0x02, 0x04, 0x08, 0x10, 0x20, // backslash
    0x00, 0x41, 0x41, 0x7f, 0x00, // ]
    0x04, 0x02, 0x01, 0x02, 0x04, // ^
    0x40, 0x40, 0x40, 0x40, 0x40, // _
    0x00, 0x01, 0x02, 0x04, 0x00, // `
    0x20, 0x54, 0x54, 0x54, 0x78, // a
    0x7f, 0x48, 0x44, 0x44, 0x38, // b
    0x38, 0x44, 0x44, 0x44, 0x20, // c
    0x38, 0x44, 0x44, 0x48, 0x7f, // d
    0x38, 0x54, 0x54, 0x54, 0x18, // e
    0x08, 0x7e, 0x09, 0x01, 0x02, // f
    0x0c, 0x52, 0x52, 0x52, 0x3e, // g
    0x7f, 0x08, 0x04, 0x04, 0x78, // h
    0x00, 0x44, 0x7d, 0x40, 0x00, // i
    0x20, 0x40, 0x44, 0x3d, 0x00, // j
    0x7f, 0x10, 0x28, 0x44, 0x00, // k
    0x00, 0x41, 0x7f, 0x40, 0x00, // l
    0x7c, 0x04, 0x18, 0x04, 0x78, // m
    0x7c, 0x08, 0x04, 0x04, 0x78, // n
    0x38, 0x44, 0x44, 0x44, 0x38, // o
    0x7c, 0x14, 0x14, 0x14, 0x08, // p
    0x08, 0x14, 0x14, 0x18, 0x7c, // q
    0x7c, 0x08, 0x04, 0x04, 0x08, // r
    0x48, 0x54, 0x54, 0x54, 0x20, // s
    0x04, 0x3f, 0x44, 0x40, 0x20, // t
    0x3c, 0x40, 0x40, 0x20, 0x7c, // u
    0x1c, 0x20, 0x40, 0x20, 0x1c, // v
    0x3c, 0x40, 0x30, 0x40, 0x3c, // w
    0x44, 0x28, 0x10, 0x28, 0x44, // x
    0x0c, 0x50, 0x50, 0x50, 0x3c, // y
    0x44, 0x64, 0x54, 0x4c, 0x44, // z
    0x00, 0x08, 0x36, 0x41, 0x00, // {
    0x00, 0x00, 0x7f, 0x00, 0x00, // |
    0x00, 0x41, 0x36, 0x08, 0x00, // }
    0x08, 0x08, 0x2a, 0x1c, 0x08  // ~
};

// ========================================================
// 位图渲染引擎核心
// ========================================================
class FontEngine {
public:
    // ========================================================
    // 渲染单个字符 (支持 Buffer 写入)
    // ========================================================
    static uint16_t draw_char(uint16_t x, int16_t y, char c, FontColor color, FontSize size = FontSize::MEDIUM, uint16_t* buffer = nullptr, uint16_t buffer_width = 0) {
        if (c < ' ' || c > '~') return 0;
        if (!buffer || buffer_width == 0) return 0;

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
                            if (px >= 0 && px < buffer_width && py >= 0) {
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
    // DMA 分片直接推送渲染 (无整块 buffer)
    // ========================================================
    static uint16_t draw_char_dma(uint16_t x, int16_t y, char c, FontColor fg_color, FontColor bg_color, const FontDef& font) {
        if (c < font.start_char || c > font.end_char) return 0;
        
        uint16_t char_idx = c - font.start_char;
        const GlyphInfo& glyph = font.glyphs[char_idx];
        const uint8_t* bitmap = &font.bitmap_data[glyph.bitmap_idx];

        uint16_t width = glyph.width;
        uint16_t height = glyph.height;
        // 如果是空格或不可见字符，直接返回步进宽度
        if (width == 0 || height == 0) return glyph.width + (font.line_height / 4);

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
    static void draw_string(uint16_t x, int16_t y, const char* str, FontColor color, FontSize size = FontSize::MEDIUM, uint16_t* buffer = nullptr, uint16_t buffer_width = 0) {
        if (!str) return;
        uint16_t cursor_x = x;
        
        while (*str) {
            cursor_x += draw_char(cursor_x, y, *str, color, size, buffer, buffer_width);
            str++;
        }
    }

    // ========================================================
    // 渲染整数数值 (零内存分配)
    // ========================================================
    static void draw_number(uint16_t x, int16_t y, int32_t num, FontColor color, FontSize size = FontSize::MEDIUM, uint16_t* buffer = nullptr, uint16_t buffer_width = 0) {
        char buf[16];
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
            if (is_neg) buf[i++] = '-';
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

        draw_string(x, y, buf, color, size, buffer, buffer_width);
    }
};

#endif // AURORA_FONT_ENGINE_HPP
