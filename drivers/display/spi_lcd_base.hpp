// =============================================================================
// drivers/display/spi_lcd_base.hpp
//
// SPI 彩色 LCD 驱动公共基类（ST7789 / SSD1351 / 类 ILI9341 指令集兼容）
//
// OledDriver 与 St7789Driver 的 SPI 指令序列几乎逐行相同（SWRESET / SLPOUT /
// DISPON / CASET / RASET / RAMWR 等），此前各自复制了一份。本基类：
//   - 收敛 SPI 传输原语（DC 引脚切换、字节/16 位数据发送）
//   - 收敛局部窗口写入 (set_window) 与脏区域补丁推送 (write_patch)
//   - 收敛硬件复位、显示地址偏移与命令+数据序列发送
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
// 指令编号以 Sitronix ST7789V datasheet 为准，其余同类驱动 IC
// (SSD1351 / ILI9341) 大多复用同一套基本指令。
// ========================================================
#define LCD_NOP      0x00 // 空操作
#define LCD_SWRESET  0x01 // 软件复位
#define LCD_RDDID    0x04 // 读设备 ID
#define LCD_RDDST    0x09 // 读显示状态
#define LCD_SLPIN    0x10 // 进入睡眠
#define LCD_SLPOUT   0x11 // 退出睡眠
#define LCD_PTLON    0x12 // 局部显示开
#define LCD_NORON    0x13 // 正常显示模式
#define LCD_INVOFF   0x20 // 关闭反相
#define LCD_INVON    0x21 // 打开反相（AMOLED/IPS 需开启）
#define LCD_DISPOFF  0x28 // 关闭显示
#define LCD_DISPON   0x29 // 打开显示
#define LCD_CASET    0x2A // 列地址设置
#define LCD_RASET    0x2B // 行地址设置
#define LCD_RAMWR    0x2C // 显存写入
#define LCD_RAMRD    0x2E // 显存读取
#define LCD_PTLAR    0x30 // 局部显示区域
#define LCD_VSCRDEF  0x33 // 垂直滚动定义
#define LCD_TEOFF    0x34 // 撕裂效果关
#define LCD_TEON     0x35 // 撕裂效果开
#define LCD_MADCTL   0x36 // 内存访问控制（旋转/扫描方向）
#define LCD_VSCSAD   0x37 // 垂直滚动起始地址
#define LCD_IDMOFF   0x38 // 空闲模式关
#define LCD_IDMON    0x39 // 空闲模式开
#define LCD_COLMOD   0x3A // 像素格式（RGB565/RGB666 等）
#define LCD_WRMEMC   0x3C // 连续显存写
#define LCD_RSTCTL   0x44 // 复位控制
#define LCD_WRDISBV  0x51 // 写入显示亮度指令
#define LCD_WRCTRLD  0x53 // 写 CTRL 显示
#define LCD_PORCTRL  0xB2 // Porch 设置（前/后沿）
#define LCD_GCTRL    0xB7 // Gate 控制
#define LCD_VCOMS    0xBB // VCOM 设置
#define LCD_LCMCTRL  0xC0 // LCM 控制
#define LCD_VDVVRHEN 0xC2 // VDV/VRH 命令使能
#define LCD_VRHS     0xC3 // VRH 设置
#define LCD_VDVS     0xC4 // VDV 设置
#define LCD_FRCTRL2  0xC6 // 帧率控制（正常模式）
#define LCD_PWCTRL1  0xD0 // 电源控制 1（AVDD/VRH/AVCL）
#define LCD_PVGAMCTRL 0xE0 // 正电压 Gamma 校正
#define LCD_NVGAMCTRL 0xE1 // 负电压 Gamma 校正

// ========================================================
// SpiLcdDriverBase：SPI 彩色 LCD 驱动公共基类
// ========================================================
class SpiLcdDriverBase : public CharDevice {
protected:
    // 表示“未连接”的引脚号（复位/背光等可选信号）
    static constexpr uint32_t kInvalidPin = 0xFFFFFFFFu;

    uint16_t width_;
    uint16_t height_;
    bool is_sleeping_;

    // 非拥有裸指针，生命周期由板级 HAL 工厂 (get_spi_hal/get_gpio_hal) 持有。
    auroraos::hal::ISpiHal* spi_;
    auroraos::hal::IGpioHal* gpio_;
    uint32_t dc_pin_; // 数据/命令选择引脚
    uint32_t rst_pin_; // 硬件复位引脚（低有效，kInvalidPin 表示未连接）

    // 显示地址偏移：部分面板的有效显示区并不从 GRAM 的 (0,0) 开始，
    // 需在 set_window 时加上该偏移（典型由面板规格书的“列/行偏移”决定）。
    uint16_t x_offset_;
    uint16_t y_offset_;

    void set_dc_pin(bool data) {
        if (gpio_) {
            gpio_->set_pin(dc_pin_, data);
        }
    }

    void spi_transmit_byte(uint8_t byte) {
        if (spi_)
            spi_->transmit_byte(byte);
    }

    // 命令阶段：DC 拉低，发送单字节命令
    void spi_send_cmd(uint8_t cmd) {
        set_dc_pin(false);
        spi_transmit_byte(cmd);
    }

    // 数据阶段：DC 拉高，发送单字节数据
    void spi_send_data(uint8_t data) {
        set_dc_pin(true);
        spi_transmit_byte(data);
    }

    void spi_send_data_16(uint16_t data) {
        set_dc_pin(true);
        spi_transmit_byte(data >> 8);
        spi_transmit_byte(data & 0xFF);
    }

    // 命令 + 参数序列（用于 Porch/Gamma 等多参数命令）
    void spi_send_cmd_data(uint8_t cmd, const uint8_t* data, uint32_t len) {
        spi_send_cmd(cmd);
        if (!data || len == 0)
            return;
        set_dc_pin(true);
        spi_->transmit(data, len);
    }

public:
    SpiLcdDriverBase(const char* name, uint16_t width, uint16_t height)
        : CharDevice(name), width_(width), height_(height), is_sleeping_(true), spi_(nullptr), gpio_(nullptr),
          dc_pin_(0), rst_pin_(kInvalidPin), x_offset_(0), y_offset_(0) {}

    // 绑定 SPI/GPIO 实例与引脚。rst_pin 可选（未接复位引脚时留默认值即可）。
    void configure(auroraos::hal::ISpiHal* spi, auroraos::hal::IGpioHal* gpio, uint32_t dc_pin,
                   uint32_t rst_pin = kInvalidPin) {
        spi_ = spi;
        gpio_ = gpio;
        dc_pin_ = dc_pin;
        rst_pin_ = rst_pin;
    }

    // 设置显示地址偏移（见 x_offset_/y_offset_ 注释）
    void set_display_offset(uint16_t x_offset, uint16_t y_offset) {
        x_offset_ = x_offset;
        y_offset_ = y_offset;
    }

    // ========================================================
    // 硬件复位：RST 拉低 10ms → 拉高 120ms（低有效）
    // ========================================================
    void hardware_reset() {
        if (rst_pin_ == kInvalidPin || !gpio_)
            return;
        gpio_->init_pin(rst_pin_, auroraos::hal::GpioMode::Output, auroraos::hal::GpioPull::None);
        gpio_->set_pin(rst_pin_, false);
        delay_ms(10);
        gpio_->set_pin(rst_pin_, true);
        delay_ms(120);
    }

    // ========================================================
    // 设定局部显存写入窗口 (x0, y0) -> (x1, y1)
    // 应用显示地址偏移后写入 CASET/RASET/RAMWR。
    // ========================================================
    void set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
        if (is_sleeping_)
            return;

        // 边界钳制 + 偏移
        if (x1 >= width_)
            x1 = static_cast<uint16_t>(width_ - 1);
        if (y1 >= height_)
            y1 = static_cast<uint16_t>(height_ - 1);
        const uint16_t xs = static_cast<uint16_t>(x0 + x_offset_);
        const uint16_t xe = static_cast<uint16_t>(x1 + x_offset_);
        const uint16_t ys = static_cast<uint16_t>(y0 + y_offset_);
        const uint16_t ye = static_cast<uint16_t>(y1 + y_offset_);

        spi_send_cmd(LCD_CASET);
        spi_send_data_16(xs);
        spi_send_data_16(xe);

        spi_send_cmd(LCD_RASET);
        spi_send_data_16(ys);
        spi_send_data_16(ye);

        spi_send_cmd(LCD_RAMWR);
    }

    // ========================================================
    // 局域数据推送：仅将变动矩形内的显存补丁以 DMA/SPI 传输
    // ========================================================
    void write_patch(const ColorRGB565* buffer, uint32_t pixel_count) {
        if (is_sleeping_ || pixel_count == 0 || !spi_)
            return;

        set_dc_pin(true);
        // 异步启动 DMA，等待由 wait_transmit_complete 负责（HAL 内可用 WFI 而非纯忙等）
        spi_->transmit_dma(reinterpret_cast<const uint8_t*>(buffer), pixel_count * 2);
        spi_->wait_transmit_complete();
    }

    uint16_t get_width() const {
        return width_;
    }

    uint16_t get_height() const {
        return height_;
    }

protected:
    // 粗略忙等延时。默认按固定循环估算，精确时序应由板级用
    // SysTick/RTC 定时器实现，驱动子类可 override（见 St7789Driver）。
    virtual void delay_ms(uint32_t ms) {
        volatile uint32_t count = ms * 1000u;
        while (count != 0u) {
            --count;
        }
    }
};

#endif // AURORA_SPI_LCD_BASE_HPP
