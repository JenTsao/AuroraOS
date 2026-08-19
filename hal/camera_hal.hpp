#ifndef AURORA_HAL_CAMERA_HAL_HPP
#define AURORA_HAL_CAMERA_HAL_HPP

#include <stdint.h>
#include <stddef.h>
#include "i2c_hal.hpp"
#include "gpio_hal.hpp"

namespace auroraos {
namespace hal {

// 摄像头物理引脚与时钟配置
struct CameraPinConfig {
    int pin_pwdn;    // Power down pin (-1 if unused)
    int pin_reset;   // Hardware reset pin (-1 if unused)
    int pin_xclk;    // External master clock output pin
    int pin_pclk;    // Pixel clock input pin
    int pin_vsync;   // Vertical sync input pin
    int pin_href;    // Horizontal reference/sync input pin
    int pin_d0;      // Data bus bit 0
    int pin_d1;      // Data bus bit 1
    int pin_d2;      // Data bus bit 2
    int pin_d3;      // Data bus bit 3
    int pin_d4;      // Data bus bit 4
    int pin_d5;      // Data bus bit 5
    int pin_d6;      // Data bus bit 6
    int pin_d7;      // Data bus bit 7
    int i2c_bus_id;  // SCCB/I2C bus ID for sensor register control
};

// DMA 完成回调函数签名
using CameraDmaCallback = void (*)(uint8_t* completed_buffer, size_t length, void* user_data);

// 摄像头底层硬件抽象接口 (DCMI / Parallel DVP / SPI / DMA)
class ICameraHal {
public:
    virtual ~ICameraHal() = default;

    // 硬件引脚与时钟初始化
    virtual bool init_hardware(const CameraPinConfig& config) = 0;

    // 控制传感器供电 (Power Down pin)
    virtual void set_power(bool enable) = 0;

    // 硬件复位脉冲
    virtual void hard_reset() = 0;

    // 配置主时钟 XCLK 频率 (通常 10MHz ~ 24MHz)
    virtual bool set_xclk_frequency(uint32_t freq_hz) = 0;

    // 配置并启动双缓冲 DMA 连续捕获
    virtual bool start_dma_capture(uint8_t* buf0, uint8_t* buf1, size_t buffer_size,
                                   CameraDmaCallback callback, void* user_data) = 0;

    // 停止 DMA 捕获
    virtual void stop_dma_capture() = 0;

    // 查询当前捕获状态
    virtual bool is_capturing() const = 0;
};

// 获取板级摄像头硬件接口
ICameraHal* get_camera_hal(int camera_id = 0);

} // namespace hal
} // namespace auroraos

#endif // AURORA_HAL_CAMERA_HAL_HPP
