// =============================================================================
// drivers/display/ssd1306_driver.hpp
//
// SSD1306 0.96" I2C 单色 OLED 驱动
// 目标硬件：深圳汉昇 HS96L03W2C03 (SSD1306 主控)
//
//   型号        : HS96L03W2C03
//   尺寸        : 0.96 英寸
//   分辨率      : 128 x 64 dots
//   显示颜色    : 单色 (白色)
//   接口        : I2C (4 Pin: GND / VCC / SCL / SDA)
//   驱动 Duty   : 1/64
//   工作电压    : VCC 2.8 ~ 3.3 V (逻辑 VDD 1.65 ~ 3.3 V)
//   从机地址    : 0x3C (7-bit) / 写地址 0x78 (8-bit)
//
// 引脚定义 (规格书 1.5 Pin Definition):
//   Pin1 GND   — 电源地
//   Pin2 VCC   — 电源正 (2.8 ~ 3.3 V)
//   Pin3 SCL   — I2C 串行时钟
//   Pin4 SDA   — I2C 串行数据
//   SCL / SDA 均需外接上拉电阻 (规格书 1.7)
//
// 设计原则（遵循 AGENTS.md 内核/驱动规范）：
//   - 零动态内存分配：1KB 显存 + 129B 传输缓冲均为对象内静态数组
//   - 显存布局为页式 (page-major)：1 字节 = 8 个纵向像素 (bit0 = 顶部)
//   - 脏页位图跟踪，refresh() 仅推送发生变化的页，降低 I2C 总线占用
//   - 通过 auroraos::hal::II2cHal 抽象接口访问总线，与具体 MCU 解耦
// =============================================================================
#ifndef AURORA_SSD1306_DRIVER_HPP
#define AURORA_SSD1306_DRIVER_HPP

#include <stdint.h>
#include "../../kernel/core/device.hpp"
#include "../../hal/i2c_hal.hpp"

// =============================================================================
// SSD1306 核心指令集（依据 HS96L03W2C03 规格书 4.1 Commands）
// =============================================================================
#define SSD1306_SETLOWCOLUMN   0x00  // 设置低列地址
#define SSD1306_SETHIGHCOLUMN  0x10  // 设置高列地址
#define SSD1306_MEMORYMODE     0x20  // 显存寻址模式 (0x00 水平 / 0x01 垂直 / 0x02 页式)
#define SSD1306_COLUMNADDR     0x21  // 设置列地址范围
#define SSD1306_PAGEADDR       0x22  // 设置页地址范围
#define SSD1306_SETSTARTLINE   0x40  // 设置显示起始行 (0x40~0x7F)
#define SSD1306_SETCONTRAST    0x81  // 设置对比度 (双字节，复位值 0x7F)
#define SSD1306_CHARGEPUMP     0x8D  // 电荷泵设置 (0x10 关 / 0x14 开)
#define SSD1306_SEGREMAP       0xA0  // 段重映射 (0xA0 正常 / 0xA1 翻转)
#define SSD1306_DISPLAYALLON_RESUME 0xA4 // 输出 RAM 内容
#define SSD1306_DISPLAYALLON  0xA5   // 全屏点亮 (忽略 RAM)
#define SSD1306_NORMALDISPLAY 0xA6   // 正常显示 (RAM=1 点亮)
#define SSD1306_INVERTDISPLAY 0xA7   // 反相显示 (RAM=0 点亮)
#define SSD1306_SETMULTIPLEX  0xA8   // 复用比 (1/64 duty -> 0x3F)
#define SSD1306_DISPLAYOFF    0xAE   // 关闭显示 (进入睡眠)
#define SSD1306_DISPLAYON     0xAF   // 打开显示
#define SSD1306_SETCOMPINS    0xDA   // COM 引脚硬件配置 (0x02 / 0x12)
#define SSD1306_SETVCOMDETECT 0xDB   // VCOMH 去饱和电平 (0x00/0x20/0x30)
#define SSD1306_SETDISPLAYOFFSET 0xD3 // 显示偏移
#define SSD1306_SETDISPLAYCLOCKDIV 0xD5 // 显示时钟分频比 / 振荡器频率
#define SSD1306_SETPRECHARGE  0xD9   // 预充电周期
#define SSD1306_SETCOMSCANDEC  0xC8  // COM 扫描方向 (0xC0 正常 / 0xC8 翻转)
#define SSD1306_PAGEADDR_BASE 0xB0   // 页地址基址 (0xB0 ~ 0xB7)

namespace auroraos {
namespace display {

// 5x7 ASCII 点阵字模 (95 个字符，ASCII 0x20 ~ 0x7E)
// 每个字符 5 字节，每字节为一列，bit0 为顶部像素（与 renderer2d 约定一致）
static constexpr uint8_t kFont5x7[95 * 5] = {
    0x00, 0x00, 0x00, 0x00, 0x00, // (space)
    0x00, 0x00, 0x5F, 0x00, 0x00, // !
    0x00, 0x07, 0x00, 0x07, 0x00, // "
    0x14, 0x7F, 0x14, 0x7F, 0x14, // #
    0x24, 0x2A, 0x7F, 0x2A, 0x12, // $
    0x23, 0x13, 0x08, 0x64, 0x62, // %
    0x36, 0x49, 0x55, 0x22, 0x50, // &
    0x00, 0x05, 0x03, 0x00, 0x00, // '
    0x00, 0x1C, 0x22, 0x41, 0x00, // (
    0x00, 0x41, 0x22, 0x1C, 0x00, // )
    0x08, 0x2A, 0x1C, 0x2A, 0x08, // *
    0x08, 0x08, 0x3E, 0x08, 0x08, // +
    0x00, 0x50, 0x30, 0x00, 0x00, // ,
    0x08, 0x08, 0x08, 0x08, 0x08, // -
    0x00, 0x60, 0x60, 0x00, 0x00, // .
    0x20, 0x10, 0x08, 0x04, 0x02, // /
    0x3E, 0x51, 0x49, 0x45, 0x3E, // 0
    0x00, 0x42, 0x7F, 0x40, 0x00, // 1
    0x42, 0x61, 0x51, 0x49, 0x46, // 2
    0x21, 0x41, 0x45, 0x4B, 0x31, // 3
    0x18, 0x14, 0x12, 0x7F, 0x10, // 4
    0x27, 0x45, 0x45, 0x45, 0x39, // 5
    0x3C, 0x4A, 0x49, 0x49, 0x30, // 6
    0x01, 0x71, 0x09, 0x05, 0x03, // 7
    0x36, 0x49, 0x49, 0x49, 0x36, // 8
    0x06, 0x49, 0x49, 0x29, 0x1E, // 9
    0x00, 0x36, 0x36, 0x00, 0x00, // :
    0x00, 0x56, 0x36, 0x00, 0x00, // ;
    0x00, 0x08, 0x14, 0x22, 0x41, // <
    0x14, 0x14, 0x14, 0x14, 0x14, // =
    0x41, 0x22, 0x14, 0x08, 0x00, // >
    0x02, 0x01, 0x51, 0x09, 0x06, // ?
    0x32, 0x49, 0x79, 0x41, 0x3E, // @
    0x7E, 0x11, 0x11, 0x11, 0x7E, // A
    0x7F, 0x49, 0x49, 0x49, 0x36, // B
    0x3E, 0x41, 0x41, 0x41, 0x22, // C
    0x7F, 0x41, 0x41, 0x22, 0x1C, // D
    0x7F, 0x49, 0x49, 0x49, 0x41, // E
    0x7F, 0x09, 0x09, 0x01, 0x01, // F
    0x3E, 0x41, 0x41, 0x51, 0x32, // G
    0x7F, 0x08, 0x08, 0x08, 0x7F, // H
    0x00, 0x41, 0x7F, 0x41, 0x00, // I
    0x20, 0x40, 0x41, 0x3F, 0x01, // J
    0x7F, 0x08, 0x14, 0x22, 0x41, // K
    0x7F, 0x40, 0x40, 0x40, 0x40, // L
    0x7F, 0x02, 0x04, 0x02, 0x7F, // M
    0x7F, 0x04, 0x08, 0x10, 0x7F, // N
    0x3E, 0x41, 0x41, 0x41, 0x3E, // O
    0x7F, 0x09, 0x09, 0x09, 0x06, // P
    0x3E, 0x41, 0x51, 0x21, 0x5E, // Q
    0x7F, 0x09, 0x19, 0x29, 0x46, // R
    0x46, 0x49, 0x49, 0x49, 0x31, // S
    0x01, 0x01, 0x7F, 0x01, 0x01, // T
    0x3F, 0x40, 0x40, 0x40, 0x3F, // U
    0x1F, 0x20, 0x40, 0x20, 0x1F, // V
    0x7F, 0x20, 0x18, 0x20, 0x7F, // W
    0x63, 0x14, 0x08, 0x14, 0x63, // X
    0x03, 0x04, 0x78, 0x04, 0x03, // Y
    0x61, 0x51, 0x49, 0x45, 0x43, // Z
    0x00, 0x00, 0x7F, 0x41, 0x41, // [
    0x02, 0x04, 0x08, 0x10, 0x20, // backslash
    0x41, 0x41, 0x7F, 0x00, 0x00, // ]
    0x04, 0x02, 0x01, 0x02, 0x04, // ^
    0x40, 0x40, 0x40, 0x40, 0x40, // _
    0x00, 0x01, 0x02, 0x04, 0x00, // `
    0x20, 0x54, 0x54, 0x54, 0x78, // a
    0x7F, 0x48, 0x44, 0x44, 0x38, // b
    0x38, 0x44, 0x44, 0x44, 0x20, // c
    0x38, 0x44, 0x44, 0x48, 0x7F, // d
    0x38, 0x54, 0x54, 0x54, 0x18, // e
    0x08, 0x7E, 0x09, 0x01, 0x02, // f
    0x08, 0x14, 0x54, 0x54, 0x3C, // g
    0x7F, 0x08, 0x04, 0x04, 0x78, // h
    0x00, 0x44, 0x7D, 0x40, 0x00, // i
    0x20, 0x40, 0x44, 0x3D, 0x00, // j
    0x00, 0x7F, 0x10, 0x28, 0x44, // k
    0x00, 0x41, 0x7F, 0x40, 0x00, // l
    0x7C, 0x04, 0x18, 0x04, 0x78, // m
    0x7C, 0x08, 0x04, 0x04, 0x78, // n
    0x38, 0x44, 0x44, 0x44, 0x38, // o
    0x7C, 0x14, 0x14, 0x14, 0x08, // p
    0x08, 0x14, 0x14, 0x18, 0x7C, // q
    0x7C, 0x08, 0x04, 0x04, 0x08, // r
    0x48, 0x54, 0x54, 0x54, 0x20, // s
    0x04, 0x3F, 0x44, 0x40, 0x20, // t
    0x3C, 0x40, 0x40, 0x20, 0x7C, // u
    0x1C, 0x20, 0x40, 0x20, 0x1C, // v
    0x3C, 0x40, 0x30, 0x40, 0x3C, // w
    0x44, 0x28, 0x10, 0x28, 0x44, // x
    0x0C, 0x50, 0x50, 0x50, 0x3C, // y
    0x44, 0x64, 0x54, 0x4C, 0x44, // z
    0x00, 0x08, 0x36, 0x41, 0x00, // {
    0x00, 0x00, 0x7F, 0x00, 0x00, // |
    0x00, 0x41, 0x36, 0x08, 0x00, // }
    0x10, 0x08, 0x08, 0x10, 0x08, // ~
};

} // namespace display
} // namespace auroraos

// =============================================================================
// Ssd1306Driver：SSD1306 I2C OLED 字符设备驱动
// =============================================================================
class Ssd1306Driver : public CharDevice {
public:
    static constexpr uint16_t kWidth = 128;
    static constexpr uint16_t kHeight = 64;
    static constexpr uint8_t kPages = kHeight / 8;           // 8 页
    static constexpr uint16_t kBufferSize = kWidth * kPages; // 1024 字节
    static constexpr uint8_t kDefaultI2cAddr = 0x3C;         // 7-bit 从机地址 (写地址 0x78)

    Ssd1306Driver(const char* name)
        : CharDevice(name), i2c_(nullptr), i2c_addr_(kDefaultI2cAddr), is_sleeping_(true), dirty_pages_(0) {}

    // 绑定 I2C 总线实例与从机地址（7-bit，默认 0x3C）
    void configure(auroraos::hal::II2cHal* i2c, uint8_t i2c_addr = kDefaultI2cAddr) {
        i2c_ = i2c;
        i2c_addr_ = i2c_addr;
    }

    // =========================================================================
    // 设备生命周期：open() 执行完整初始化序列（规格书 4.2 / 参考实现 §15）
    // =========================================================================
    int open() override {
        if (!i2c_)
            return -1;

        send_cmd(SSD1306_DISPLAYOFF);           // 0xAE 关闭显示
        send_cmd2(SSD1306_SETDISPLAYCLOCKDIV, 0x80); // 0xD5 时钟分频，约 100 帧/秒
        send_cmd2(SSD1306_SETMULTIPLEX, 0x3F);  // 0xA8 复用比 1/64 duty
        send_cmd2(SSD1306_SETDISPLAYOFFSET, 0x00);   // 0xD3 显示偏移 0
        send_cmd(SSD1306_SETSTARTLINE | 0x00);  // 0x40 显示起始行 0
        send_cmd2(SSD1306_CHARGEPUMP, 0x14);    // 0x8D 电荷泵使能（I2C 供电必须开启）
        send_cmd(SSD1306_MEMORYMODE);           // 0x20 寻址模式
        send_cmd(0x02);                         // 页式寻址 (Page Addressing)
        send_cmd(SSD1306_SEGREMAP | 0x01);      // 0xA1 段重映射（左右正常）
        send_cmd(SSD1306_SETCOMSCANDEC);        // 0xC8 COM 扫描方向（上下正常）
        send_cmd2(SSD1306_SETCOMPINS, 0x12);    // 0xDA COM 引脚硬件配置
        send_cmd2(SSD1306_SETCONTRAST, 0xCF);   // 0x81 对比度
        send_cmd2(SSD1306_SETPRECHARGE, 0xF1);  // 0xD9 预充电 15 时钟 / 放电 1 时钟
        send_cmd2(SSD1306_SETVCOMDETECT, 0x30); // 0xDB VCOMH 去饱和 0.83 x VCC
        send_cmd(SSD1306_DISPLAYALLON_RESUME);  // 0xA4 输出 RAM 内容
        send_cmd(SSD1306_NORMALDISPLAY);        // 0xA6 正常显示（非反相）
        send_cmd(SSD1306_DISPLAYON);            // 0xAF 打开显示

        clear();       // 清空显存
        refresh();     // 首帧推送
        is_sleeping_ = false;
        return 0;
    }

    int close() override {
        sleep();
        return 0;
    }

    // =========================================================================
    // 绘制接口（均操作本地显存，需调用 refresh() 推送到硬件）
    // =========================================================================

    void clear(bool on = false) {
        const uint8_t fill = on ? 0xFF : 0x00;
        for (uint16_t i = 0; i < kBufferSize; ++i) {
            buffer_[i] = fill;
        }
        dirty_pages_ = 0xFF; // 全部页标记脏
    }

    void set_pixel(uint16_t x, uint16_t y, bool on = true) {
        if (x >= kWidth || y >= kHeight)
            return;
        const uint8_t page = static_cast<uint8_t>(y >> 3);
        const uint8_t bit = static_cast<uint8_t>(y & 0x07);
        uint8_t& byte = buffer_[page * kWidth + x];
        if (on)
            byte |= static_cast<uint8_t>(1u << bit);
        else
            byte &= static_cast<uint8_t>(~(1u << bit));
        dirty_pages_ |= static_cast<uint8_t>(1u << page);
    }

    void draw_hline(uint16_t x, uint16_t y, uint16_t w, bool on = true) {
        for (uint16_t i = 0; i < w; ++i)
            set_pixel(static_cast<uint16_t>(x + i), y, on);
    }

    void draw_vline(uint16_t x, uint16_t y, uint16_t h, bool on = true) {
        for (uint16_t i = 0; i < h; ++i)
            set_pixel(x, static_cast<uint16_t>(y + i), on);
    }

    void fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, bool on = true) {
        for (uint16_t row = 0; row < h; ++row)
            draw_hline(x, static_cast<uint16_t>(y + row), w, on);
    }

    // 单字符绘制（5x7 字模，支持整数倍放大）
    void draw_char(uint16_t x, uint16_t y, char c, uint8_t scale = 1, bool on = true) {
        if (scale == 0)
            scale = 1;
        const uint8_t uc = static_cast<uint8_t>(c);
        if (uc < 0x20 || uc > 0x7E)
            return; // 仅支持可打印 ASCII
        const uint8_t idx = static_cast<uint8_t>(uc - 0x20);
        const uint8_t* glyph = &auroraos::display::kFont5x7[idx * 5];
        for (uint8_t col = 0; col < 5; ++col) {
            uint8_t line = glyph[col];
            for (uint8_t row = 0; row < 7; ++row) {
                if (line & 0x01u) {
                    if (scale == 1) {
                        set_pixel(static_cast<uint16_t>(x + col), static_cast<uint16_t>(y + row), on);
                    } else {
                        fill_rect(static_cast<uint16_t>(x + col * scale),
                                  static_cast<uint16_t>(y + row * scale), scale, scale, on);
                    }
                }
                line >>= 1;
            }
        }
    }

    void draw_string(uint16_t x, uint16_t y, const char* str, uint8_t scale = 1, bool on = true) {
        if (!str)
            return;
        uint16_t cursor = x;
        while (*str) {
            draw_char(cursor, y, *str, scale, on);
            cursor = static_cast<uint16_t>(cursor + 6 * scale); // 5 + 1 字符间距
            ++str;
        }
    }

    // =========================================================================
    // 刷新：仅推送脏页到硬件（页式寻址 + 自动列递增）
    // =========================================================================
    void refresh() {
        if (is_sleeping_ || !i2c_)
            return;

        for (uint8_t page = 0; page < kPages; ++page) {
            if (!(dirty_pages_ & (1u << page)))
                continue;
            send_cmd(static_cast<uint8_t>(SSD1306_PAGEADDR_BASE | page)); // 设置页地址
            send_cmd(SSD1306_SETLOWCOLUMN);                              // 列低 4 位
            send_cmd(SSD1306_SETHIGHCOLUMN);                             // 列高 4 位
            send_data(&buffer_[page * kWidth], kWidth);                  // 推送整页 128 字节
        }
        dirty_pages_ = 0;
    }

    // 无条件整屏刷新（抗噪声恢复用，规格书 7.5 建议周期性重发）
    void refresh_full() {
        if (is_sleeping_ || !i2c_)
            return;
        for (uint8_t page = 0; page < kPages; ++page) {
            send_cmd(static_cast<uint8_t>(SSD1306_PAGEADDR_BASE | page));
            send_cmd(SSD1306_SETLOWCOLUMN);
            send_cmd(SSD1306_SETHIGHCOLUMN);
            send_data(&buffer_[page * kWidth], kWidth);
        }
        dirty_pages_ = 0;
    }

    // =========================================================================
    // 电源与显示控制（规格书 4.2 Power Down / Sleep 时序）
    // =========================================================================
    void sleep() {
        if (is_sleeping_)
            return;
        send_cmd(SSD1306_DISPLAYOFF);  // 0xAE 关闭面板
        send_cmd2(SSD1306_CHARGEPUMP, 0x10); // 0x8D 关闭电荷泵
        is_sleeping_ = true;
    }

    void wake() {
        if (!is_sleeping_)
            return;
        send_cmd2(SSD1306_CHARGEPUMP, 0x14); // 0x8D 开启电荷泵
        send_cmd(SSD1306_DISPLAYON);         // 0xAF 打开面板
        is_sleeping_ = false;
    }

    void set_display(bool on) {
        send_cmd(on ? SSD1306_DISPLAYON : SSD1306_DISPLAYOFF);
        is_sleeping_ = !on;
    }

    void set_contrast(uint8_t contrast) {
        send_cmd2(SSD1306_SETCONTRAST, contrast);
    }

    void invert(bool invert) {
        send_cmd(invert ? SSD1306_INVERTDISPLAY : SSD1306_NORMALDISPLAY);
    }

    uint16_t get_width() const {
        return kWidth;
    }

    uint16_t get_height() const {
        return kHeight;
    }

    // 直接访问显存（供外部位图/字体引擎写入后手动 refresh）
    uint8_t* get_buffer() {
        return buffer_;
    }

    // 手动标记某个区域为脏（外部绕过 set_pixel 直接写 buffer 后调用）
    void mark_dirty(uint16_t y0, uint16_t y1) {
        if (y1 >= kHeight)
            y1 = kHeight - 1;
        for (uint16_t y = y0; y <= y1; ++y)
            dirty_pages_ |= static_cast<uint8_t>(1u << (y >> 3));
    }

private:
    // 发送单字节命令（I2C 控制字节 0x00 表示命令流）
    void send_cmd(uint8_t cmd) {
        if (i2c_)
            i2c_->write_reg(i2c_addr_, 0x00, &cmd, 1);
    }

    // 发送双字节命令（命令 + 参数）
    void send_cmd2(uint8_t cmd, uint8_t arg) {
        if (!i2c_)
            return;
        uint8_t seq[2] = {cmd, arg};
        i2c_->write_reg(i2c_addr_, 0x00, seq, 2);
    }

    // 发送数据流（I2C 控制字节 0x40 表示数据流）
    void send_data(const uint8_t* data, uint16_t len) {
        if (!i2c_ || !data)
            return;
        i2c_->write_reg(i2c_addr_, 0x40, data, len);
    }

    uint8_t buffer_[kBufferSize];       // 1KB 页式显存（静态分配，无堆）
    uint8_t dirty_pages_;               // 脏页位图 (bit0~bit7)
    auroraos::hal::II2cHal* i2c_;       // I2C 抽象接口
    uint8_t i2c_addr_;                  // 7-bit 从机地址（默认 0x3C）
    bool is_sleeping_;                  // 显示开关状态
};

#endif // AURORA_SSD1306_DRIVER_HPP
