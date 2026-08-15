// =============================================================================
// drivers/display/spi_lcd_base.hpp
//
// SPI 彩色 LCD 驱动公共基类（ST7789 / SSD1351 / 类 ILI9341 指令集兼容）
//
// OledDriver 与 St7789Driver 的 SPI 指令序列几乎逐行相同（SWRESET / SLPOUT /
// DISPON / CASET / RASET / RAMWR 等），此前各自复制了一份。本基类：
//   - 收敛 SPI 传输原语（DC 引脚切换、字节/16 位数据发送）
//   - 收敛局部窗口写入 (set_window) 与脏区域补丁推送 (write_patch)
//   - 统一继承 CharDevice，使所有彩色 LCD 驱动都能挂载到 DeviceRegistry
//
// 遵循 AGENTS.md：
//   - 避免复制粘贴（OledDriver/St7789Driver 不再各自维护相同指令序列）
//   - 显式所有权（spi_/gpio_ 均为非拥有裸指针，由板级 HAL 持有）
//   - 零动态内存分配
// =============================================================================
#ifndef AURORA_SPI_LCD_BASE_HPP
#define AURORA_SPI_LCD_BASE_HPP

#include <stdint.h>
#include "../../kernel/core/device.hpp"
#include "../../hal/spi_hal.hpp"
#include "../../hal/gpio_hal.hpp"

// 16 位 RGB565 颜色定义（统一颜色类型，避免各驱动重复声明）
using ColorRGB565 = uint16_t;

// ========================================================
// SPI 并行彩色 LCD 通用指令集
// ========================================================
#define LCD_SWRESET 0x01
#define LCD_SLPIN   0x10
#define LCD_SLPOUT  0x11
#define LCD_DISPOFF 0x28
#define LCD_DISPON  0x29
#define LCD_CASET   0x2A
#define LCD_RASET   0x2B
#define LCD_RAMWR   0x2C
#define LCD_WRDISBV 0x51 // 写入显示亮度指令

// ========================================================
// SpiLcdDriverBase：SPI 彩色 LCD 驱动公共基类
// ========================================================
class SpiLcdDriverBase : public CharDevice {
protected:
    uint16_t width_;
    uint16_t height_;
    bool is_sleeping_;

    auroraos::hal::ISpiHal* spi_;
    auroraos::hal::IGpioHal* gpio_;
    uint32_t dc_pin_;

    void set_dc_pin(bool data) {
        if (gpio_) {
            gpio_->set_pin(dc_pin_, data);
        }
    }

    void spi_transmit_byte(uint8_t byte) {
        if (spi_)
            spi_->transmit_byte(byte);
    }

    void spi_send_cmd(uint8_t cmd) {
        set_dc_pin(false);
        spi_transmit_byte(cmd);
    }

    void spi_send_data(uint8_t data) {
        set_dc_pin(true);
        spi_transmit_byte(data);
    }

    void spi_send_data_16(uint16_t data) {
        set_dc_pin(true);
        spi_transmit_byte(data >> 8);
        spi_transmit_byte(data & 0xFF);
    }

public:
    SpiLcdDriverBase(const char* name, uint16_t width, uint16_t height)
        : CharDevice(name), width_(width), height_(height), is_sleeping_(true), spi_(nullptr), gpio_(nullptr),
          dc_pin_(0) {}

    void configure(auroraos::hal::ISpiHal* spi, auroraos::hal::IGpioHal* gpio, uint32_t dc_pin) {
        spi_ = spi;
        gpio_ = gpio;
        dc_pin_ = dc_pin;
    }

    // ========================================================
    // 设定局部显存写入窗口 (x0, y0) -> (x1, y1)
    // ========================================================
    void set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
        if (is_sleeping_)
            return;

        spi_send_cmd(LCD_CASET);
        spi_send_data_16(x0);
        spi_send_data_16(x1);

        spi_send_cmd(LCD_RASET);
        spi_send_data_16(y0);
        spi_send_data_16(y1);

        spi_send_cmd(LCD_RAMWR);
    }

    // ========================================================
    // 局域数据推送：仅将变动矩形内的显存补丁以 DMA/SPI 传输
    // ========================================================
    void write_patch(const ColorRGB565* buffer, uint32_t pixel_count) {
        if (is_sleeping_ || pixel_count == 0 || !spi_)
            return;

        set_dc_pin(true);
        spi_->transmit_dma(reinterpret_cast<const uint8_t*>(buffer), pixel_count * 2);
        spi_->wait_transmit_complete();
    }

    uint16_t get_width() const {
        return width_;
    }

    uint16_t get_height() const {
        return height_;
    }
};

#endif // AURORA_SPI_LCD_BASE_HPP
