// MOCK: not for production (Placeholder driver for OLED displays without physical SPI/DMA implementation)
#ifndef AURORA_OLED_DRIVER_MOCK_HPP
#define AURORA_OLED_DRIVER_MOCK_HPP

#include <stdint.h>
#include "device.hpp"

// 16位 RGB565 颜色定义
using ColorRGB565 = uint16_t;

class OledDriver : public CharDevice {
private:
    uint16_t width_;
    uint16_t height_;
    int      console_fd_;

public:
    OledDriver(const char* name, uint16_t width = 128, uint16_t height = 128) 
        : CharDevice(name), width_(width), height_(height), console_fd_(-1) {}

    int open() override {
        // 在实际物理硬件中，这里会初始化 SPI0 控制器、复位引脚及 OLED 驱动 IC 寄存器
        // console_fd_ = ::open("/dev/uart0", 0);
        return 0;
    }

    // ========================================================
    // OLED 硬件核心能力：设定局部显存写入窗口 (x0, y0) -> (x1, y1)
    // ========================================================
    void set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
        // 物理硬件：发送指令 0x2A 设定列地址，发送 0x2B 设定行地址，最后发 0x2C 准备写像素
        // if (console_fd_ >= 0) { ... }
        // std::cout << "[SPI CMD] Set Window: (" << x0 << ", " << y0 << ") -> (" << x1 << ", " << y1 << ")" << std::endl;
    }

    // ========================================================
    // 局域数据推送：仅将变动矩形内的显存补丁以 DMA/SPI 传输
    // ========================================================
    void write_patch(const ColorRGB565* buffer, uint32_t pixel_count) {
        // 物理硬件：启动 DMA 传输 pixel_count * 2 个字节到 SPI 数据寄存器
        // std::cout << "    ⚡ [SPI DMA] Flushed Patch Size: " << pixel_count * sizeof(ColorRGB565) << " Bytes" << std::endl;
    }

    uint16_t get_width() const { return width_; }
    uint16_t get_height() const { return height_; }
};

#endif
