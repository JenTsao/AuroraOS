#ifndef AURORA_ST7789_DRIVER_HPP
#define AURORA_ST7789_DRIVER_HPP

#include <stdint.h>
#include "board.h" // 引入板级引脚定义 (DISPLAY_WIDTH, DISPLAY_HEIGHT)
#include "spi_lcd_base.hpp"

// ========================================================
// ST7789 核心硬件指令集
// 与 OledDriver 共用同一套指令序列，此处仅保留旧宏名作为兼容别名，
// 具体数值统一收敛于 spi_lcd_base.hpp。
// ========================================================
#define ST7789_SWRESET  LCD_SWRESET
#define ST7789_SLPIN    LCD_SLPIN
#define ST7789_SLPOUT   LCD_SLPOUT
#define ST7789_DISPOFF  LCD_DISPOFF
#define ST7789_DISPON   LCD_DISPON
#define ST7789_CASET    LCD_CASET
#define ST7789_RASET    LCD_RASET
#define ST7789_RAMWR    LCD_RAMWR
#define ST7789_WRDISBV  LCD_WRDISBV // 写入显示屏背光亮度指令

class St7789Driver : public SpiLcdDriverBase {
private:
    uint8_t current_brightness_;

    St7789Driver()
        : SpiLcdDriverBase("st7789", DISPLAY_WIDTH, DISPLAY_HEIGHT), current_brightness_(100) {}

public:
    static St7789Driver& instance() {
        static St7789Driver driver;
        return driver;
    }

    // ========================================================
    // 硬件初始化
    // ========================================================
    void init() {
        if (gpio_) {
            gpio_->init_pin(dc_pin_, auroraos::hal::GpioMode::Output, auroraos::hal::GpioPull::None);
        }

        spi_send_cmd(LCD_SWRESET);
        spi_send_cmd(LCD_SLPOUT);
        spi_send_cmd(LCD_DISPON);
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
        spi_send_cmd(LCD_DISPOFF);
        spi_send_cmd(LCD_SLPIN);
        is_sleeping_ = true;
    }

    // 退出深度休眠：唤醒显示面板
    void exit_sleep() {
        if (!is_sleeping_)
            return;
        spi_send_cmd(LCD_SLPOUT);
        spi_send_cmd(LCD_DISPON);
        is_sleeping_ = false;
    }

    // 设置屏幕背光亮度 (1-100)
    void set_brightness(uint8_t level) {
        if (level > 100)
            level = 100;
        current_brightness_ = level;

        uint8_t hw_val = (level * 255) / 100;
        spi_send_cmd(LCD_WRDISBV);
        spi_send_data(hw_val);
    }

    void set_low_brightness() {
        set_brightness(30); // Dim 状态亮度 30%
    }
};

#endif // AURORA_ST7789_DRIVER_HPP
