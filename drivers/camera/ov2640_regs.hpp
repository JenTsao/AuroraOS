#ifndef AURORA_DRIVERS_OV2640_REGS_HPP
#define AURORA_DRIVERS_OV2640_REGS_HPP

#include <stdint.h>

namespace auroraos {
namespace drivers {
namespace camera {

// OV2640 I2C / SCCB 从机设备地址
constexpr uint8_t OV2640_I2C_ADDR = 0x30; // 7-bit (0x60 write / 0x61 read)

// 寄存器选择 Bank (RA_DLMT = 0xFF)
constexpr uint8_t BANK_SEL = 0xFF;
constexpr uint8_t BANK_DSP    = 0x00; // Bank 0: DSP 控制
constexpr uint8_t BANK_SENSOR = 0x01; // Bank 1: 传感器核心控制

// =============================================================================
// Bank 1 (Sensor Core) 寄存器定义
// =============================================================================
constexpr uint8_t SENSOR_GAIN       = 0x00; // AGC 增益控制
constexpr uint8_t SENSOR_COM1       = 0x03; // 通用控制 1
constexpr uint8_t SENSOR_REG04      = 0x04; // 硬件镜像与翻转控制 (bit7: VFLIP, bit6: HFLIP)
constexpr uint8_t SENSOR_PIDH       = 0x0A; // 产品 ID 高字节 (0x26)
constexpr uint8_t SENSOR_PIDL       = 0x0B; // 产品 ID 低字节 (0x40 ~ 0x42)
constexpr uint8_t SENSOR_COM7       = 0x12; // 通用控制 7 (bit7: 软复位, bit0-2: 分辨率)
constexpr uint8_t SENSOR_COM10      = 0x15; // 通用控制 10 (PCLK/HREF/VSYNC 极性)
constexpr uint8_t SENSOR_HREFST     = 0x17; // HREF 起始
constexpr uint8_t SENSOR_HREFEND    = 0x18; // HREF 结束
constexpr uint8_t SENSOR_VSTART     = 0x19; // VSYNC 起始
constexpr uint8_t SENSOR_VEND       = 0x1A; // VSYNC 结束
constexpr uint8_t SENSOR_MIDH       = 0x1C; // 厂商 ID 高字节 (0x7F)
constexpr uint8_t SENSOR_MIDL       = 0x1D; // 厂商 ID 低字节 (0xA2)
constexpr uint8_t SENSOR_COM24      = 0x24; // AGC/AEC 控制
constexpr uint8_t SENSOR_CLKRC      = 0x11; // 内部时钟分频器

// =============================================================================
// Bank 0 (DSP) 寄存器定义
// =============================================================================
constexpr uint8_t DSP_R_BYPASS      = 0x05; // 绕过 DSP 处理
constexpr uint8_t DSP_QS            = 0x44; // 量化标量 (JPEG 质量控制)
constexpr uint8_t DSP_CTRLI         = 0x50; // 格式控制
constexpr uint8_t DSP_HSIZE         = 0x51; // 水平尺寸低 8 位
constexpr uint8_t DSP_VSIZE         = 0x52; // 垂直尺寸低 8 位
constexpr uint8_t DSP_XOFFL         = 0x53; // X 偏移低 8 位
constexpr uint8_t DSP_YOFFL         = 0x54; // Y 偏移低 8 位
constexpr uint8_t DSP_VHYX          = 0x55; // 尺寸与偏移高位
constexpr uint8_t DSP_TEST          = 0x57; // 测试图案 (bit0: 彩条测试使能)
constexpr uint8_t DSP_ZMOW          = 0x5A; // 缩放输出宽度
constexpr uint8_t DSP_ZMOH          = 0x5B; // 缩放输出高度
constexpr uint8_t DSP_BPADDR        = 0x7C; // 图像特效地址
constexpr uint8_t DSP_BPDATA        = 0x7D; // 图像特效数据
constexpr uint8_t DSP_CTRL2         = 0x86; // 模块使能 2
constexpr uint8_t DSP_CTRL3         = 0x87; // 模块使能 3
constexpr uint8_t DSP_SIZEL         = 0x8C; // 缩放尺寸低字节
constexpr uint8_t DSP_IMAGE_MODE    = 0xDA; // 图像输出模式 (bit4: JPEG, bit0-2: RGB/YUV)
constexpr uint8_t DSP_RESET         = 0xE0; // 复位 DSP

// 寄存器配置键值对结构
struct RegVal {
    uint8_t reg;
    uint8_t val;
};

// 结束标记
constexpr uint8_t REG_TABLE_END = 0xFF;

// =============================================================================
// OV2640 基础初始化默认配置序列表 (Sensor Core & DSP Defaults)
// =============================================================================
static const RegVal ov2640_init_regs[] = {
    {BANK_SEL, BANK_SENSOR},
    {SENSOR_COM7, 0x80}, // 软件复位
    {0x12, 0x80},
    {BANK_SEL, BANK_SENSOR},
    {0x11, 0x01},
    {0x12, 0x00},
    {0x03, 0x0F},
    {0x32, 0x36},
    {0x2A, 0x00},
    {0x13, 0xE5},
    {0x14, 0x48},
    {0x15, 0x00},
    {BANK_SEL, BANK_DSP},
    {0xE0, 0x04},
    {0xC0, 0xC8},
    {0xC1, 0x96},
    {0x86, 0x3D},
    {0x50, 0x89}, // RGB565 / YUV422 默认模式
    {0x87, 0x00},
    {0x00, 0x00},
    {REG_TABLE_END, REG_TABLE_END}
};

// RGB565 输出格式配置表
static const RegVal ov2640_rgb565_regs[] = {
    {BANK_SEL, BANK_DSP},
    {DSP_IMAGE_MODE, 0x08}, // RGB565
    {DSP_CTRLI, 0x00},
    {0x51, 0x00},
    {REG_TABLE_END, REG_TABLE_END}
};

// YUV422 输出格式配置表
static const RegVal ov2640_yuv422_regs[] = {
    {BANK_SEL, BANK_DSP},
    {DSP_IMAGE_MODE, 0x00}, // YUV422
    {DSP_CTRLI, 0x00},
    {REG_TABLE_END, REG_TABLE_END}
};

// JPEG 压缩输出格式配置表
static const RegVal ov2640_jpeg_regs[] = {
    {BANK_SEL, BANK_DSP},
    {DSP_IMAGE_MODE, 0x10}, // Enable JPEG compression
    {DSP_CTRLI, 0x00},
    {DSP_QS, 0x0C},         // Default JPEG quality
    {REG_TABLE_END, REG_TABLE_END}
};

} // namespace camera
} // namespace drivers
} // namespace auroraos

#endif // AURORA_DRIVERS_OV2640_REGS_HPP
