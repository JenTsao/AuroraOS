#ifndef AURORA_OLED_DRIVER_HPP
#define AURORA_OLED_DRIVER_HPP

#include "spi_lcd_base.hpp"

// ========================================================
// OLED 核心硬件指令集（类 ST7789 / SSD1351）
// 与 ST7789 驱动共用同一套指令序列，此处仅保留旧宏名作为兼容别名，
// 具体数值统一收敛于 spi_lcd_base.hpp。
// ========================================================
#define OLED_SWRESET  LCD_SWRESET
#define OLED_SLPIN    LCD_SLPIN
#define OLED_SLPOUT   LCD_SLPOUT
#define OLED_DISPOFF  LCD_DISPOFF
#define OLED_DISPON   LCD_DISPON
#define OLED_CASET    LCD_CASET
#define OLED_RASET    LCD_RASET
#define OLED_RAMWR    LCD_RAMWR
#define OLED_WRDISBV  LCD_WRDISBV

class OledDriver : public SpiLcdDriverBase {
public:
    OledDriver(const char* name, uint16_t width = 128, uint16_t height = 128)
        : SpiLcdDriverBase(name, width, height) {}

    int open() override {
        if (gpio_) {
            gpio_->init_pin(dc_pin_, auroraos::hal::GpioMode::Output, auroraos::hal::GpioPull::None);
        }

        // 初始化 SPI0 控制器、复位引脚及 OLED 驱动 IC 寄存器
        spi_send_cmd(LCD_SWRESET);
        spi_send_cmd(LCD_SLPOUT);
        spi_send_cmd(LCD_DISPON);
        is_sleeping_ = false;
        return 0;
    }
};

#endif // AURORA_OLED_DRIVER_HPP
