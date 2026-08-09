#ifndef AURORA_OLED_DRIVER_HPP
#define AURORA_OLED_DRIVER_HPP

#include <stdint.h>
#include "device.hpp"

// ========================================================
// OLED 核心硬件指令集 (类 ST7789 / SSD1351)
// ========================================================
#define OLED_SWRESET 0x01
#define OLED_SLPIN   0x10
#define OLED_SLPOUT  0x11
#define OLED_DISPOFF 0x28
#define OLED_DISPON  0x29
#define OLED_CASET   0x2A
#define OLED_RASET   0x2B
#define OLED_RAMWR   0x2C
#define OLED_WRDISBV 0x51

// ========================================================
// Apollo3 Blue IOM (SPI) 寄存器定义
// ========================================================
#define AM_HAL_IOM_BASE     0x50004000
#define AM_HAL_IOM_FIFO     (AM_HAL_IOM_BASE + 0x200)
#define AM_HAL_IOM_CMD      (AM_HAL_IOM_BASE + 0x108)
#define AM_HAL_IOM_STATUS   (AM_HAL_IOM_BASE + 0x104)

// DMA 控制寄存器
#define AM_HAL_IOM_DMA_CFG     (AM_HAL_IOM_BASE + 0x2A0)
#define AM_HAL_IOM_DMA_TARG    (AM_HAL_IOM_BASE + 0x2A4)
#define AM_HAL_IOM_DMA_TOTLEN  (AM_HAL_IOM_BASE + 0x2A8)

// GPIO Data/Set/Clear
#define AM_HAL_GPIO_BASE    0x40010000
#define AM_HAL_GPIO_WT_EN   (AM_HAL_GPIO_BASE + 0x04)
#define AM_HAL_GPIO_WT_DIS  (AM_HAL_GPIO_BASE + 0x08)
#define PIN_DISP_DC         12 // Data/Command Pin

// 16位 RGB565 颜色定义
using ColorRGB565 = uint16_t;

class OledDriver : public CharDevice {
private:
    uint16_t width_;
    uint16_t height_;
    bool     is_sleeping_;

    void set_dc_pin(bool data) {
        volatile uint32_t* gpio_wt_en  = reinterpret_cast<uint32_t*>(AM_HAL_GPIO_WT_EN);
        volatile uint32_t* gpio_wt_dis = reinterpret_cast<uint32_t*>(AM_HAL_GPIO_WT_DIS);
        if (data) {
            *gpio_wt_en = (1 << PIN_DISP_DC);
        } else {
            *gpio_wt_dis = (1 << PIN_DISP_DC);
        }
    }

    void spi_transmit_byte(uint8_t byte) {
        volatile uint32_t* iom_fifo   = reinterpret_cast<uint32_t*>(AM_HAL_IOM_FIFO);
        volatile uint32_t* iom_cmd    = reinterpret_cast<uint32_t*>(AM_HAL_IOM_CMD);
        volatile uint32_t* iom_status = reinterpret_cast<uint32_t*>(AM_HAL_IOM_STATUS);
        
        *iom_fifo = byte;
        *iom_cmd  = 0x1; // Trigger 1 byte SPI write
        uint32_t timeout = 1000000;
        while ((*iom_status & 0x1) != 0 && --timeout); // Wait until idle
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
        : CharDevice(name), width_(width), height_(height), is_sleeping_(true) {}

    int open() override {
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
        if (is_sleeping_) return;
        
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
        if (is_sleeping_ || pixel_count == 0) return;

        set_dc_pin(true);
        
        // 启动 SPI DMA 异步传输 (Apollo3 DMA 控制器)
        volatile uint32_t* dma_cfg    = reinterpret_cast<uint32_t*>(AM_HAL_IOM_DMA_CFG);
        volatile uint32_t* dma_targ   = reinterpret_cast<uint32_t*>(AM_HAL_IOM_DMA_TARG);
        volatile uint32_t* dma_totlen = reinterpret_cast<uint32_t*>(AM_HAL_IOM_DMA_TOTLEN);
        volatile uint32_t* iom_status = reinterpret_cast<uint32_t*>(AM_HAL_IOM_STATUS);
        
        *dma_targ   = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(buffer));
        *dma_totlen = pixel_count * 2;
        *dma_cfg    = 0x1; // Enable DMA
        
        // 传输期间，等待 DMA 传输完成中断唤醒 CPU
        uint32_t timeout = 5000000;
        while ((*iom_status & 0x2) == 0 && --timeout) {
            __asm__ volatile ("wfi" : : : "memory"); 
        }
    }

    uint16_t get_width() const { return width_; }
    uint16_t get_height() const { return height_; }
};

#endif // AURORA_OLED_DRIVER_HPP
