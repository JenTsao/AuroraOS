#ifndef AURORA_OLED_DRIVER_HPP
#define AURORA_OLED_DRIVER_HPP

#include <stdint.h>
#include "device.hpp"
#include "../../hal/spi_hal.hpp"
#include "../../hal/gpio_hal.hpp"

// ========================================================
// OLED 核心硬件指令集 (类 ST7789 / SSD1351)
// ========================================================
#define OLED_SWRESET 0x01
#define OLED_SLPIN 0x10
#define OLED_SLPOUT 0x11
#define OLED_DISPOFF 0x28
#define OLED_DISPON 0x29
#define OLED_CASET 0x2A
#define OLED_RASET 0x2B
#define OLED_RAMWR 0x2C
#define OLED_WRDISBV 0x51

// 16位 RGB565 颜色定义
using ColorRGB565 = uint16_t;

class OledDriver : public CharDevice {
private:
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
    OledDriver(const char* name, uint16_t width = 128, uint16_t height = 128)
        : CharDevice(name), width_(width), height_(height), is_sleeping_(true), spi_(nullptr), gpio_(nullptr),
          dc_pin_(0) {}

    void configure(auroraos::hal::ISpiHal* spi, auroraos::hal::IGpioHal* gpio, uint32_t dc_pin) {
        spi_ = spi;
        gpio_ = gpio;
        dc_pin_ = dc_pin;
    }

    int open() override {
        if (gpio_) {
            gpio_->init_pin(dc_pin_, auroraos::hal::GpioMode::Output, auroraos::hal::GpioPull::None);
        }

        // 初始化 SPI0 控制器、复位引脚及 OLED 驱动 IC 寄存器
        spi_send_cmd(OLED_SWRESET);
        spi_send_cmd(OLED_SLPOUT);
        spi_send_cmd(OLED_DISPON);
        is_sleeping_ = false;
        return 0;
    }

    // ========================================================
    // OLED 硬件核心能力：设定局部显存写入窗口 (x0, y0) -> (x1, y1)
    // ========================================================
    void set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
        if (is_sleeping_)
            return;

        spi_send_cmd(OLED_CASET);
        spi_send_data_16(x0);
        spi_send_data_16(x1);

        spi_send_cmd(OLED_RASET);
        spi_send_data_16(y0);
        spi_send_data_16(y1);

        spi_send_cmd(OLED_RAMWR);
    }

    // ========================================================
    // 局域数据推送：仅将变动矩形内的显存补丁以 DMA/SPI 传输
    // ========================================================
    void write_patch(const ColorRGB565* buffer, uint32_t pixel_count) {
        if (is_sleeping_ || pixel_count == 0 || !spi_)
            return;

        set_dc_pin(true);

        // 启动 SPI DMA 异步传输
        spi_->transmit_dma(reinterpret_cast<const uint8_t*>(buffer), pixel_count * 2);

        // 等待 DMA 传输完成
        spi_->wait_transmit_complete();
    }

    uint16_t get_width() const {
        return width_;
    }

    uint16_t get_height() const {
        return height_;
    }
};

#endif // AURORA_OLED_DRIVER_HPP
