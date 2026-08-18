#ifndef AURORA_DRIVERS_CAMERA_TYPES_HPP
#define AURORA_DRIVERS_CAMERA_TYPES_HPP

#include <stdint.h>
#include <stddef.h>

namespace auroraos {
namespace drivers {
namespace camera {

// 像素编码格式
enum class PixelFormat : uint8_t {
    RGB565    = 0,  // 16-bit RGB (5-6-5)
    YUV422    = 1,  // 16-bit YUYV (Y0-U0-Y1-V0)
    GRAYSCALE = 2,  // 8-bit 单通道灰度
    RGB888    = 3,  // 24-bit RGB (8-8-8)
    JPEG      = 4,  // 压缩 JPEG 码流
    BAYER_RAW = 5   // 原始 Bayer 矩阵
};

// 预设分辨率规范
enum class Resolution : uint8_t {
    RES_96X96   = 0,  // 96x96 (微型穿戴)
    RES_QQVGA   = 1,  // 160x120
    RES_QCIF    = 2,  // 176x144
    RES_HQVGA   = 3,  // 240x160
    RES_240X240 = 4,  // 240x240 (圆形/方形表盘)
    RES_QVGA    = 5,  // 320x240
    RES_CIF     = 6,  // 352x288
    RES_HVGA    = 7,  // 480x320
    RES_VGA     = 8,  // 640x480
    RES_SVGA    = 9,  // 800x600
    RES_XGA     = 10, // 1024x768
    RES_SXGA    = 11, // 1280x1024
    RES_UXGA    = 12, // 1600x1200
    RES_CUSTOM  = 13  // 自定义分辨率
};

// 分辨率宽高尺寸信息
struct ResolutionInfo {
    uint16_t width;
    uint16_t height;
};

inline ResolutionInfo get_resolution_info(Resolution res) {
    switch (res) {
        case Resolution::RES_96X96:   return {96, 96};
        case Resolution::RES_QQVGA:   return {160, 120};
        case Resolution::RES_QCIF:    return {176, 144};
        case Resolution::RES_HQVGA:   return {240, 160};
        case Resolution::RES_240X240: return {240, 240};
        case Resolution::RES_QVGA:    return {320, 240};
        case Resolution::RES_CIF:     return {352, 288};
        case Resolution::RES_HVGA:    return {480, 320};
        case Resolution::RES_VGA:     return {640, 480};
        case Resolution::RES_SVGA:    return {800, 600};
        case Resolution::RES_XGA:     return {1024, 768};
        case Resolution::RES_SXGA:    return {1280, 1024};
        case Resolution::RES_UXGA:    return {1600, 1200};
        default:                      return {0, 0};
    }
}

// 计算指定格式和分辨率下单帧图像字节大小
inline size_t get_frame_byte_size(uint16_t width, uint16_t height, PixelFormat fmt) {
    switch (fmt) {
        case PixelFormat::GRAYSCALE: return static_cast<size_t>(width) * height;
        case PixelFormat::RGB565:
        case PixelFormat::YUV422:    return static_cast<size_t>(width) * height * 2;
        case PixelFormat::RGB888:    return static_cast<size_t>(width) * height * 3;
        case PixelFormat::JPEG:      return static_cast<size_t>(width) * height * 2; // 上限预估
        case PixelFormat::BAYER_RAW: return static_cast<size_t>(width) * height;
        default:                     return 0;
    }
}

// 特殊滤镜效果
enum class SpecialEffect : uint8_t {
    None      = 0,
    Negative  = 1,
    Grayscale = 2,
    Reddish   = 3,
    Greenish  = 4,
    Bluish    = 5,
    Sepia     = 6
};

// 白平衡模式
enum class WhiteBalance : uint8_t {
    Auto   = 0,
    Sunny  = 1,
    Cloudy = 2,
    Office = 3,
    Home   = 4
};

// 摄像头图像控制参数
struct CameraControls {
    int8_t brightness = 0;      // -2 ~ +2
    int8_t contrast = 0;        // -2 ~ +2
    int8_t saturation = 0;      // -2 ~ +2
    SpecialEffect effect = SpecialEffect::None;
    WhiteBalance wb = WhiteBalance::Auto;
    bool hflip = false;         // 水平镜像
    bool vflip = false;         // 垂直翻转
    bool test_pattern = false;  // 测试彩条图案
};

// 摄像头帧描述符
struct CameraFrame {
    uint8_t* buffer = nullptr;
    size_t length = 0;
    uint16_t width = 0;
    uint16_t height = 0;
    PixelFormat format = PixelFormat::RGB565;
    uint32_t sequence = 0;
    uint32_t timestamp_ms = 0;
    bool valid = false;
};

// 摄像头运行指标
struct CameraStats {
    uint32_t frames_captured = 0;
    uint32_t frames_dropped = 0;
    uint32_t fps = 0;
    uint32_t error_count = 0;
};

// 格式配置请求结构 (供 IOCTL 使用)
struct CameraFormatConfig {
    uint16_t width;
    uint16_t height;
    PixelFormat format;
    uint8_t fps;
};

// =============================================================================
// VFS IOCTL 指令集 (0xC000 ~ 0xC0FF)
// =============================================================================
constexpr int CAMERA_IOCTL_START_CAPTURE = 0xC001; // 启动捕获流
constexpr int CAMERA_IOCTL_STOP_CAPTURE  = 0xC002; // 停止捕获流
constexpr int CAMERA_IOCTL_SET_FMT       = 0xC003; // 设置格式与分辨率 (CameraFormatConfig*)
constexpr int CAMERA_IOCTL_GET_FMT       = 0xC004; // 获取格式与分辨率 (CameraFormatConfig*)
constexpr int CAMERA_IOCTL_SET_FPS       = 0xC005; // 设置帧率 (uint32_t*)
constexpr int CAMERA_IOCTL_GET_FPS       = 0xC006; // 获取帧率 (uint32_t*)
constexpr int CAMERA_IOCTL_SET_CONTROLS  = 0xC007; // 设置图像控制 (CameraControls*)
constexpr int CAMERA_IOCTL_GET_CONTROLS  = 0xC008; // 获取图像控制 (CameraControls*)
constexpr int CAMERA_IOCTL_GET_FRAME     = 0xC009; // 获取一帧数据 (CameraFrame*)
constexpr int CAMERA_IOCTL_RELEASE_FRAME = 0xC00A; // 释放帧归还给驱动
constexpr int CAMERA_IOCTL_GET_STATS     = 0xC00B; // 获取统计信息 (CameraStats*)

} // namespace camera
} // namespace drivers
} // namespace auroraos

#endif // AURORA_DRIVERS_CAMERA_TYPES_HPP
