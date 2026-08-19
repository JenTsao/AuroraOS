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
#define ST7789_MADCTL   LCD_MADCTL
#define ST7789_COLMOD   LCD_COLMOD
#define ST7789_INVON    LCD_INVON
#define ST7789_NORON    LCD_NORON

// =============================================================================
// St7789Driver：ST7789 系列 AMOLED/IPS 彩色 LCD 驱动（MiBand 8）
//
// 目标硬件：小米手环 8 的 1.62 英寸 192x490 AMOLED（ST7789H2 主控，SPI）
//
// 设计原则（遵循 AGENTS.md）：
//   - 零动态内存分配：整行流式缓冲为对象内静态数组
//   - 继承 SpiLcdDriverBase 复用 SPI 原语 / 窗口 / 复位 / 偏移
//   - 完整上电初始化序列（软件复位 → 像素格式 → 时序 → Gamma → 反相 → 开显示）
//   - 通过 open()/close() 对接 CharDevice 生命周期，sleep/wake 供 PowerManager 联动
//
// 注意：Porch/帧率/Gamma/VCOM 等参数取自 Sitronix ST7789V 通用初始化基准，
// 针对 192x490 这一非标长条面板，实际量产前需按面板规格书做最终标定，
// 尤其是 MADCTL（旋转方向）与 x/y 显示偏移。
// =============================================================================
class St7789Driver : public SpiLcdDriverBase {
public:
    static constexpr uint16_t kWidth = DISPLAY_WIDTH;
    static constexpr uint16_t kHeight = DISPLAY_HEIGHT;

    // MADCTL (0x36) 内存访问控制位域：
    //   MY(bit7) 行地址顺序 · MX(bit6) 列地址顺序 · MV(bit5) 行列交换
    //   ML(bit4) 垂直刷新顺序 · RGB(bit3) 像素顺序 · MH(bit2) 水平刷新顺序
    // 0x00 = 默认方向；纵向长条面板通常需 MV=1 并配合 MX/MY 确定上下。
    // 具体取值以面板规格书为准，可通过 set_rotation() 运行时调整。
    static constexpr uint8_t kMadctlDefault = 0x00;

    // 每行像素数（192），用于整行流式刷新/清屏（384 字节静态缓冲）
    static constexpr uint16_t kRowBufPixels = kWidth;

private:
    uint8_t current_brightness_;
    uint8_t madctl_;

    // 整行流式缓冲：fill_screen / 逐行刷新复用，避免为整帧分配 188KB 堆内存。
    uint16_t row_buf_[kRowBufPixels];

    // ST7789V 初始化参数表（静态常量，置于 Flash 只读段）
    static constexpr uint8_t kPorch[5] = {0x0C, 0x0C, 0x00, 0x33, 0x33};          // 0xB2 前后沿
    static constexpr uint8_t kGateCtrl[1] = {0x35};                               // 0xB7 Gate 控制
    static constexpr uint8_t kVcom[1] = {0x19};                                   // 0xBB VCOM
    static constexpr uint8_t kLcmCtrl[1] = {0x2C};                                // 0xC0 LCM 控制
    static constexpr uint8_t kVdvVrhEn[1] = {0x01};                               // 0xC2 VDV/VRH 使能
    static constexpr uint8_t kVrh[1] = {0x12};                                    // 0xC3 VRH
    static constexpr uint8_t kVdv[1] = {0x20};                                    // 0xC4 VDV
    static constexpr uint8_t kFrameRate[1] = {0x0F};                              // 0xC6 帧率
    static constexpr uint8_t kPowerCtrl[2] = {0xA4, 0xA1};                        // 0xD0 电源控制
    static constexpr uint8_t kPvGamma[14] = {0xD0, 0x04, 0x0D, 0x11, 0x13, 0x2B, 0x3F, 0x54, 0x4C, 0x18, 0x0D,
                                             0x0B, 0x1F, 0x23};                    // 0xE0 正 Gamma
    static constexpr uint8_t kNvGamma[14] = {0xD0, 0x04, 0x0C, 0x11, 0x13, 0x2C, 0x3F, 0x44, 0x51, 0x2F, 0x1F,
                                             0x1F, 0x20, 0x23};                    // 0xE1 负 Gamma

    St7789Driver()
        : SpiLcdDriverBase("st7789", kWidth, kHeight), current_brightness_(100), madctl_(kMadctlDefault) {
        for (uint16_t i = 0; i < kRowBufPixels; ++i)
            row_buf_[i] = 0;
    }

    // 以系统主频标定的忙等延时（覆盖基类粗略实现）。
    // SYSTEM_CORE_CLOCK 由 board.h 提供；主机测试等无该宏的环境回退到固定循环。
    void delay_ms(uint32_t ms) override {
#ifdef SYSTEM_CORE_CLOCK
        volatile uint32_t count = ms * (SYSTEM_CORE_CLOCK / 3000UL);
#else
        volatile uint32_t count = ms * 1000u;
#endif
        while (count != 0u) {
            --count;
        }
    }

public:
    static St7789Driver& instance() {
        static St7789Driver driver;
        return driver;
    }

    // ========================================================
    // CharDevice 生命周期：完整上电初始化
    // ========================================================
    int open() override {
        if (!spi_)
            return -1;

        // DC 引脚为输出；复位引脚在 hardware_reset 内一并初始化
        if (gpio_) {
            gpio_->init_pin(dc_pin_, auroraos::hal::GpioMode::Output, auroraos::hal::GpioPull::None);
        }

        // 1. 硬件复位（若连接了 RST 引脚）
        hardware_reset();

        // 2. 软件复位 + 退出睡眠
        spi_send_cmd(LCD_SWRESET);
        delay_ms(150);
        spi_send_cmd(LCD_SLPOUT);
        delay_ms(120);

        // 3. 像素格式：16-bit RGB565（0x55 = 65K 色）
        spi_send_cmd(LCD_COLMOD);
        spi_send_data(0x55);

        // 4. 显示方向（内存访问控制）
        spi_send_cmd(LCD_MADCTL);
        spi_send_data(madctl_);

        // 5. Porch / Gate / VCOM / LCM 时序与电源
        spi_send_cmd_data(LCD_PORCTRL, kPorch, sizeof(kPorch));
        spi_send_cmd_data(LCD_GCTRL, kGateCtrl, sizeof(kGateCtrl));
        spi_send_cmd_data(LCD_VCOMS, kVcom, sizeof(kVcom));
        spi_send_cmd_data(LCD_LCMCTRL, kLcmCtrl, sizeof(kLcmCtrl));
        spi_send_cmd_data(LCD_VDVVRHEN, kVdvVrhEn, sizeof(kVdvVrhEn));
        spi_send_cmd_data(LCD_VRHS, kVrh, sizeof(kVrh));
        spi_send_cmd_data(LCD_VDVS, kVdv, sizeof(kVdv));
        spi_send_cmd_data(LCD_FRCTRL2, kFrameRate, sizeof(kFrameRate));
        spi_send_cmd_data(LCD_PWCTRL1, kPowerCtrl, sizeof(kPowerCtrl));

        // 6. Gamma 校正
        spi_send_cmd_data(LCD_PVGAMCTRL, kPvGamma, sizeof(kPvGamma));
        spi_send_cmd_data(LCD_NVGAMCTRL, kNvGamma, sizeof(kNvGamma));

        // 7. AMOLED/IPS 需反相显示；进入正常显示模式后开屏
        spi_send_cmd(LCD_INVON);
        spi_send_cmd(LCD_NORON);
        spi_send_cmd(LCD_DISPON);
        delay_ms(20);
        is_sleeping_ = false; // 先解除睡眠态，使后续 set_window/write_patch 生效

        // 8. 清屏为纯黑并恢复默认亮度
        fill_screen(0x0000);
        set_brightness(current_brightness_);
        return 0;
    }

    int close() override {
        enter_sleep();
        return 0;
    }

    // 兼容旧调用入口（watch_app 通过 init() 唤醒驱动）
    void init() {
        open();
    }

    // ========================================================
    // 旋转 / 内存访问控制
    // ========================================================
    void set_rotation(uint8_t madctl) {
        madctl_ = madctl;
        if (!is_sleeping_) {
            spi_send_cmd(LCD_MADCTL);
            spi_send_data(madctl_);
        }
    }

    uint8_t get_rotation() const {
        return madctl_;
    }

    // ========================================================
    // 整屏填充（逐行流式写入，内存占用固定为一行）
    // ========================================================
    void fill_screen(ColorRGB565 color) {
        for (uint16_t i = 0; i < kRowBufPixels; ++i)
            row_buf_[i] = color;

        set_window(0, 0, static_cast<uint16_t>(kWidth - 1), static_cast<uint16_t>(kHeight - 1));
        for (uint16_t y = 0; y < kHeight; ++y) {
            write_patch(row_buf_, kRowBufPixels);
        }
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
        delay_ms(120);
        spi_send_cmd(LCD_DISPON);
        delay_ms(20);
        is_sleeping_ = false;
    }

    // 设置显示亮度 (0-100)，映射到 8-bit 显示亮度寄存器
    void set_brightness(uint8_t level) {
        if (level > 100)
            level = 100;
        current_brightness_ = level;

        const uint8_t hw_val = static_cast<uint8_t>((level * 255) / 100);
        spi_send_cmd(LCD_WRDISBV);
        spi_send_data(hw_val);
    }

    void set_low_brightness() {
        set_brightness(30); // Dim 状态亮度 30%
    }

    uint8_t get_brightness() const {
        return current_brightness_;
    }
};

#endif // AURORA_ST7789_DRIVER_HPP
