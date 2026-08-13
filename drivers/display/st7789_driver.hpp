#ifndef AURORA_ST7789_DRIVER_HPP
#define AURORA_ST7789_DRIVER_HPP

#include <stdint.h>
#include "board.h" // 引入板级引脚定义 (DISPLAY_WIDTH, DISPLAY_HEIGHT)
#include "../../hal/spi_hal.hpp"
#include "../../hal/gpio_hal.hpp"

// ========================================================
// ST7789 核心硬件指令集
// ========================================================
#define ST7789_SWRESET 0x01
#define ST7789_SLPIN 0x10
#define ST7789_SLPOUT 0x11
#define ST7789_DISPOFF 0x28
#define ST7789_DISPON 0x29
#define ST7789_CASET 0x2A
#define ST7789_RASET 0x2B
#define ST7789_RAMWR 0x2C
#define ST7789_WRDISBV 0x51 // 写入显示屏背光亮度指令

class St7789Driver {
private:
    uint16_t width_;
    uint16_t height_;
    bool is_sleeping_;
    uint8_t current_brightness_;

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

    St7789Driver()
        : width_(192), height_(490), is_sleeping_(false), current_brightness_(100), spi_(nullptr), gpio_(nullptr),
          dc_pin_(0) {} // 适配 192x490 分辨率

public:
    static St7789Driver& instance() {
        static St7789Driver driver;
        return driver;
    }

    void configure(auroraos::hal::ISpiHal* spi, auroraos::hal::IGpioHal* gpio, uint32_t dc_pin) {
        spi_ = spi;
        gpio_ = gpio;
        dc_pin_ = dc_pin;
    }

    // ========================================================
    // 硬件初始化
    // ========================================================
    void init() {
        if (gpio_) {
            gpio_->init_pin(dc_pin_, auroraos::hal::GpioMode::Output, auroraos::hal::GpioPull::None);
        }

        spi_send_cmd(ST7789_SWRESET);

        spi_send_cmd(ST7789_SLPOUT);

        spi_send_cmd(ST7789_DISPON);
        set_brightness(100);
        is_sleeping_ = false;
    }

    // ========================================================
    // 核心电源管理接口 (供 PowerManager 联动调用)
    // ========================================================

    // 进入深度休眠：关闭显示面板，关闭内部升压电路
    void enter_sleep() {
        if (is_sleeping_)
            return;
        spi_send_cmd(ST7789_DISPOFF);
        spi_send_cmd(ST7789_SLPIN);
        is_sleeping_ = true;
    }

    // 退出深度休眠：唤醒显示面板
    void exit_sleep() {
        if (!is_sleeping_)
            return;
        spi_send_cmd(ST7789_SLPOUT);
        spi_send_cmd(ST7789_DISPON);
        is_sleeping_ = false;
    }

    // 设置屏幕背光亮度 (1-100)
    void set_brightness(uint8_t level) {
        if (level > 100)
            level = 100;
        current_brightness_ = level;

        uint8_t hw_val = (level * 255) / 100;
        spi_send_cmd(ST7789_WRDISBV);
        spi_send_data(hw_val);
    }

    void set_low_brightness() {
        set_brightness(30); // Dim 状态亮度 30%
    }

    // ========================================================
    // 动态脏区域渲染引擎接口
    // ========================================================

    void set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
        spi_send_cmd(ST7789_CASET);
        spi_send_data_16(x0);
        spi_send_data_16(x1);

        spi_send_cmd(ST7789_RASET);
        spi_send_data_16(y0);
        spi_send_data_16(y1);

        spi_send_cmd(ST7789_RAMWR);
    }

    void write_patch(const uint16_t* buffer, uint32_t pixel_count) {
        if (is_sleeping_ || pixel_count == 0 || !spi_)
            return;

        set_dc_pin(true);

        // 核心优化：启动 SPI DMA 异步传输
        spi_->transmit_dma(reinterpret_cast<const uint8_t*>(buffer), pixel_count * 2);

        // 等待 DMA 结束
        spi_->wait_transmit_complete();
    }

    uint16_t get_width() const {
        return width_;
    }

    uint16_t get_height() const {
        return height_;
    }
};

#endif // AURORA_ST7789_DRIVER_HPP
