#ifndef AURORA_FRAMEBUFFER_HPP
#define AURORA_FRAMEBUFFER_HPP

#include <stdint.h>
#include "oled_driver.hpp"
#include "../../metrics/metrics.hpp"

// 脏区域包围盒类
struct DirtyRect {
    uint16_t x0, y0, x1, y1;
    bool     is_dirty;

    void reset() {
        x0 = y0 = x1 = y1 = 0;
        is_dirty = false;
    }

    // 自动求取区域并集：每次绘制图形，自动扩张脏矩形边界
    void union_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
        if (w == 0 || h == 0) return;
        uint16_t right = x + w - 1;
        uint16_t bottom = y + h - 1;

        if (!is_dirty) {
            x0 = x; y0 = y;
            x1 = right; y1 = bottom;
            is_dirty = true;
        } else {
            if (x < x0) x0 = x;
            if (y < y0) y0 = y;
            if (right > x1) x1 = right;
            if (bottom > y1) y1 = bottom;
        }
    }
};

// 带有脏区域跟踪能力的超级渲染缓冲树
// ⚠️ 注意：由于内部含有庞大的 buffer_ 数组，本对象严禁作为局部变量在栈上创建，必须静态分配或放于专用大内存区！
template <uint16_t Width, uint16_t Height>
class FrameBuffer {
private:
    ColorRGB565 buffer_[Width * Height];
    DirtyRect   dirty_;

public:
    FrameBuffer() {
        clear(0x0000); // 默认清屏纯黑
        dirty_.reset();
    }

    // ⚠️ 警告：绕过图形接口直接写入显存后，必须手动调用 mark_dirty()，否则将不会被刷新！
    ColorRGB565* get_raw_buffer() { return buffer_; }
    
    // 提供给绕过封装直接写内存的场景，强制标记脏区域
    void mark_dirty(uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
        dirty_.union_rect(x, y, w, h);
    }

    // ========================================================
    // 2D 图形基础原语：打点 (自动追踪脏点)
    // ========================================================
    void set_pixel(uint16_t x, uint16_t y, ColorRGB565 color) {
        if (x >= Width || y >= Height) return;
        
        buffer_[y * Width + x] = color;
        dirty_.union_rect(x, y, 1, 1); // 自动跟踪变更！
    }

    // ========================================================
    // 2D 图形原语：矩形填充 (自动追踪区域)
    // ========================================================
    void fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, ColorRGB565 color) {
        if (x >= Width || y >= Height) return;
        if (x + w > Width) w = Width - x;
        if (y + h > Height) h = Height - y;

        for (uint16_t row = y; row < y + h; row++) {
            for (uint16_t col = x; col < x + w; col++) {
                buffer_[row * Width + col] = color;
            }
        }
        dirty_.union_rect(x, y, w, h); // 自动合围脏矩形！
    }

    // 简单绘制 ASCII 字符 (6x8 点阵放大简化示意)
    void draw_char(uint16_t x, uint16_t y, char c, ColorRGB565 color) {
        // 为模拟 UI 组件，直接在字符相应区域画一个带有高亮的色块替代复杂字模
        fill_rect(x, y, 8, 12, color);
    }

    // ========================================================
    // 核心输出引擎：将脏区域同步给物理 OLED 屏
    // ========================================================
    void flush(OledDriver& driver) {
        if (!dirty_.is_dirty) {
            Metrics::record(METRIC_DIRTY_RATIO, 0);
            return; // 如果画面没有任何变动，0 耗时跳过传输！
        }

        uint16_t patch_width = dirty_.x1 - dirty_.x0 + 1;
        uint16_t patch_height = dirty_.y1 - dirty_.y0 + 1;
        uint32_t dirty_pixels = patch_width * patch_height;
        uint32_t total = Width * Height;
        Metrics::record(METRIC_DIRTY_RATIO, dirty_pixels * 100 / total);

        // 1. 设定 OLED 驱动 IC 的硬件局部显示窗口
        driver.set_window(dirty_.x0, dirty_.y0, dirty_.x1, dirty_.y1);

        // 2. 将脏区域内的像素逐行提取并以 DMA/SPI 传输给硬件
        
        if (patch_width == Width) {
            // 优化：如果是全屏幕宽度，内存依然是连续的，可以直接一把梭哈
            uint32_t total_patch_pixels = patch_width * patch_height;
            driver.write_patch(&buffer_[dirty_.y0 * Width + dirty_.x0], total_patch_pixels);
        } else {
            // 脏区域跨行错位，必须逐行发送 (或利用 line_buffer_ 拼装多行)
            // 这里为了简单安全，直接逐行发送
            for (uint16_t y = dirty_.y0; y <= dirty_.y1; ++y) {
                // 如果单行非常窄，理论上可以通过 line_buffer_ 收集多行再发送
                // 但考虑到 write_patch 会自己处理 SPI 发送，直接逐行发也足够稳定
                driver.write_patch(&buffer_[y * Width + dirty_.x0], patch_width);
            }
        }

        // 3. 画面同步完毕，重置脏矩形树！
        dirty_.reset();
    }

    void clear(ColorRGB565 color) {
        for (uint32_t i = 0; i < Width * Height; i++) {
            buffer_[i] = color;
        }
        dirty_.union_rect(0, 0, Width, Height);
    }
};

#endif
