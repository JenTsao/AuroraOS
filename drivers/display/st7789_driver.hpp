#ifndef AURORA_ST7789_DRIVER_HPP
#define AURORA_ST7789_DRIVER_HPP

#include <stdint.h>
#include "board.h"   // 引入板级引脚定义 (DISPLAY_WIDTH, DISPLAY_HEIGHT)

// ========================================================
// ST7789 核心硬件指令集
// ========================================================
#define ST7789_SWRESET 0x01
#define ST7789_SLPIN   0x10
#define ST7789_SLPOUT  0x11
#define ST7789_DISPOFF 0x28
#define ST7789_DISPON  0x29
#define ST7789_CASET   0x2A
#define ST7789_RASET   0x2B
#define ST7789_RAMWR   0x2C
#define ST7789_WRDISBV 0x51 // 写入显示屏背光亮度指令

// ========================================================
// Apollo3 Blue IOM (SPI) 寄存器定义
// ========================================================
#define AM_HAL_IOM_BASE     0x50004000
#define AM_HAL_IOM_FIFO     (AM_HAL_IOM_BASE + 0x200)
#define AM_HAL_IOM_CMD      (AM_HAL_IOM_BASE + 0x108)
#define AM_HAL_IOM_STATUS   (AM_HAL_IOM_BASE + 0x104)

// GPIO Data/Set/Clear (伪寄存器，代替 gpio_set)
#define AM_HAL_GPIO_BASE    0x40010000
#define AM_HAL_GPIO_WT_EN   (AM_HAL_GPIO_BASE + 0x04)
#define AM_HAL_GPIO_WT_DIS  (AM_HAL_GPIO_BASE + 0x08)
#define PIN_DISP_DC         12 // Data/Command Pin

class St7789Driver {
private:
    uint16_t width_;
    uint16_t height_;
    bool     is_sleeping_;
    uint8_t  current_brightness_;

    // 内部底层接口：发送命令与数据 (对接 Apollo3 SPI 寄存器)
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

    St7789Driver() : width_(192), height_(490), is_sleeping_(false), current_brightness_(100) {} // 适配 192x490 分辨率

public:
    static St7789Driver& instance() {
        static St7789Driver driver;
        return driver;
    }

    // ========================================================
    // 硬件初始化
    // ========================================================
    void init() {
        // 1. 硬件复位
        // gpio_clear(PIN_DISP_RST); delay_ms(10);
        // gpio_set(PIN_DISP_RST); delay_ms(120);

        // 2. 发送出厂初始化序列
        spi_send_cmd(ST7789_SWRESET);
        // delay_ms(150);
        
        spi_send_cmd(ST7789_SLPOUT);
        // delay_ms(120);

        // (此处省略具体的颜色格式、方向扫描等长串配置指令...)

        spi_send_cmd(ST7789_DISPON);
        set_brightness(100);
        is_sleeping_ = false;
    }

    // ========================================================
    // 核心电源管理接口 (供 PowerManager 联动调用)
    // ========================================================
    
    // 进入深度休眠：关闭显示面板，关闭内部升压电路
    void enter_sleep() {
        if (is_sleeping_) return;
        spi_send_cmd(ST7789_DISPOFF);
        spi_send_cmd(ST7789_SLPIN);
        is_sleeping_ = true;
    }

    // 退出深度休眠：唤醒显示面板
    void exit_sleep() {
        if (!is_sleeping_) return;
        spi_send_cmd(ST7789_SLPOUT);
        // delay_ms(120); // 必须等待内部振荡器稳定
        spi_send_cmd(ST7789_DISPON);
        is_sleeping_ = false;
    }

    // 设置屏幕背光亮度 (1-100)
    void set_brightness(uint8_t level) {
        if (level > 100) level = 100;
        current_brightness_ = level;
        
        // ST7789 硬件支持直接通过指令调节 AMOLED 亮度
        uint8_t hw_val = (level * 255) / 100;
        spi_send_cmd(ST7789_WRDISBV);
        spi_send_data(hw_val);
    }

    // 供 Dim 状态快速调用
    void set_low_brightness() {
        set_brightness(30); // Dim 状态亮度 30%
    }

    // ========================================================
    // 动态脏区域渲染引擎接口
    // ========================================================
    
    // 设定硬件写入窗口
    void set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
        spi_send_cmd(ST7789_CASET);
        spi_send_data_16(x0);
        spi_send_data_16(x1);

        spi_send_cmd(ST7789_RASET);
        spi_send_data_16(y0);
        spi_send_data_16(y1);

        spi_send_cmd(ST7789_RAMWR);
    }

    // 通过 DMA 进行大块显存补丁推送
    void write_patch(const uint16_t* buffer, uint32_t pixel_count) {
        if (is_sleeping_ || pixel_count == 0) return;

        set_dc_pin(true);
        
        // 核心优化：启动 SPI DMA 异步传输 (假定 Apollo3 DMA 控制器)
        #define AM_HAL_IOM_DMA_CFG     (AM_HAL_IOM_BASE + 0x2A0)
        #define AM_HAL_IOM_DMA_TARG    (AM_HAL_IOM_BASE + 0x2A4)
        #define AM_HAL_IOM_DMA_TOTLEN  (AM_HAL_IOM_BASE + 0x2A8)
        
        volatile uint32_t* dma_cfg    = reinterpret_cast<uint32_t*>(AM_HAL_IOM_DMA_CFG);
        volatile uint32_t* dma_targ   = reinterpret_cast<uint32_t*>(AM_HAL_IOM_DMA_TARG);
        volatile uint32_t* dma_totlen = reinterpret_cast<uint32_t*>(AM_HAL_IOM_DMA_TOTLEN);
        volatile uint32_t* iom_status = reinterpret_cast<uint32_t*>(AM_HAL_IOM_STATUS);
        
        *dma_targ   = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(buffer));
        *dma_totlen = pixel_count * 2;
        *dma_cfg    = 0x1; // Enable DMA
        
        // 传输期间，通知 CPU 直接陷入 WFI 浅度睡眠节省电能，直到 DMA 传输完成中断唤醒 CPU
        // 模拟等待 DMA 结束
        uint32_t timeout = 5000000;
        while ((*iom_status & 0x2) == 0 && --timeout) {
            __asm__ volatile ("wfi" : : : "memory"); 
        }
    }

    uint16_t get_width() const { return width_; }
    uint16_t get_height() const { return height_; }
};

#endif // AURORA_ST7789_DRIVER_HPP
