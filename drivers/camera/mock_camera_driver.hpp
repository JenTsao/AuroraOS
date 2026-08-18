#ifndef AURORA_DRIVERS_MOCK_CAMERA_DRIVER_HPP
#define AURORA_DRIVERS_MOCK_CAMERA_DRIVER_HPP

#include "camera_driver.hpp"

namespace auroraos {
namespace drivers {
namespace camera {

enum class MockPattern : uint8_t {
    ColorBars    = 0, // SMPTE 标准 8 色垂直彩条
    Gradient     = 1, // 对角线性动态渐变
    BouncingBall = 2, // 动态移动弹跳小球 (用于动作检测和帧率测试)
    SolidColor   = 3  // 纯色填充 (按帧循环变换)
};

class MockCameraDriver : public CameraDriverBase {
public:
    explicit MockCameraDriver(const char* name = "video0")
        : CameraDriverBase(name),
          pattern_(MockPattern::ColorBars),
          ball_x_(10),
          ball_y_(10),
          ball_vx_(2),
          ball_vy_(3),
          simulated_error_(false) {}

    void set_pattern(MockPattern pattern) {
        pattern_ = pattern;
    }

    void inject_error(bool enable) {
        simulated_error_ = enable;
    }

    // 手动推进单帧生成 (模拟硬件 DMA 传输完成并触发中断)
    void step_frame() {
        if (!capturing_) return;

        uint8_t* target_buf = (active_buffer_idx_ == 0) ? ping_buffer_ : pong_buffer_;
        generate_pattern(target_buf);

        size_t frame_len = get_frame_byte_size(width_, height_, format_);
        on_dma_frame_completed(target_buf, frame_len);
    }

protected:
    bool probe_sensor() override {
        return !simulated_error_;
    }

    bool apply_format(uint16_t /*width*/, uint16_t /*height*/, PixelFormat /*fmt*/) override {
        return !simulated_error_;
    }

    bool apply_controls(const CameraControls& /*controls*/) override {
        return !simulated_error_;
    }

    bool start_hardware_capture() override {
        return !simulated_error_;
    }

    bool stop_hardware_capture() override {
        return true;
    }

private:
    void generate_pattern(uint8_t* buf) {
        if (!buf) return;

        switch (pattern_) {
            case MockPattern::ColorBars:
                render_color_bars(buf);
                break;
            case MockPattern::Gradient:
                render_gradient(buf);
                break;
            case MockPattern::BouncingBall:
                render_bouncing_ball(buf);
                break;
            case MockPattern::SolidColor:
                render_solid_color(buf);
                break;
        }

        // 若开启了测试彩条控制，强制叠加彩条
        if (controls_.test_pattern && pattern_ != MockPattern::ColorBars) {
            render_color_bars(buf);
        }
    }

    // 渲染 SMPTE 8 色彩条 (白、黄、青、绿、品红、红、蓝、黑)
    void render_color_bars(uint8_t* buf) {
        // 8 种标准 RGB565 颜色
        static constexpr uint16_t smpte_rgb565[8] = {
            0xFFFF, // 白色
            0xFFE0, // 黄色
            0x07FF, // 青色
            0x07E0, // 绿色
            0xF81F, // 品红
            0xF800, // 红色
            0x001F, // 蓝色
            0x0000  // 黑色
        };

        if (format_ == PixelFormat::RGB565) {
            uint16_t* ptr = reinterpret_cast<uint16_t*>(buf);
            for (uint16_t y = 0; y < height_; ++y) {
                for (uint16_t x = 0; x < width_; ++x) {
                    uint8_t bar_idx = (static_cast<uint32_t>(x) * 8) / width_;
                    if (bar_idx >= 8) bar_idx = 7;
                    ptr[y * width_ + x] = smpte_rgb565[bar_idx];
                }
            }
        } else if (format_ == PixelFormat::GRAYSCALE) {
            static constexpr uint8_t smpte_gray[8] = {255, 226, 179, 150, 105, 76, 29, 0};
            for (uint16_t y = 0; y < height_; ++y) {
                for (uint16_t x = 0; x < width_; ++x) {
                    uint8_t bar_idx = (static_cast<uint32_t>(x) * 8) / width_;
                    if (bar_idx >= 8) bar_idx = 7;
                    buf[y * width_ + x] = smpte_gray[bar_idx];
                }
            }
        }
    }

    // 渲染动态移动小球
    void render_bouncing_ball(uint8_t* buf) {
        // 先背景清零
        size_t total_bytes = get_frame_byte_size(width_, height_, format_);
        memset(buf, 0x20, total_bytes);

        // 更新小球物理坐标
        ball_x_ += ball_vx_;
        ball_y_ += ball_vy_;
        if (ball_x_ <= 6 || ball_x_ >= width_ - 6) ball_vx_ = -ball_vx_;
        if (ball_y_ <= 6 || ball_y_ >= height_ - 6) ball_vy_ = -ball_vy_;

        int radius = 5;
        if (format_ == PixelFormat::RGB565) {
            uint16_t* ptr = reinterpret_cast<uint16_t*>(buf);
            for (int dy = -radius; dy <= radius; ++dy) {
                for (int dx = -radius; dx <= radius; ++dx) {
                    if (dx * dx + dy * dy <= radius * radius) {
                        int px = ball_x_ + dx;
                        int py = ball_y_ + dy;
                        if (px >= 0 && px < width_ && py >= 0 && py < height_) {
                            ptr[py * width_ + px] = 0xF800; // 亮红小球
                        }
                    }
                }
            }
        }
    }

    // 渲染对角动态渐变
    void render_gradient(uint8_t* buf) {
        if (format_ == PixelFormat::RGB565) {
            uint16_t* ptr = reinterpret_cast<uint16_t*>(buf);
            for (uint16_t y = 0; y < height_; ++y) {
                for (uint16_t x = 0; x < width_; ++x) {
                    uint8_t r = ((x + frame_seq_) & 0x1F);
                    uint8_t g = ((y + frame_seq_) & 0x3F);
                    uint8_t b = (((x + y) / 2) & 0x1F);
                    ptr[y * width_ + x] = static_cast<uint16_t>((r << 11) | (g << 5) | b);
                }
            }
        }
    }

    // 渲染纯色翻转
    void render_solid_color(uint8_t* buf) {
        if (format_ == PixelFormat::RGB565) {
            uint16_t* ptr = reinterpret_cast<uint16_t*>(buf);
            uint16_t color = static_cast<uint16_t>((frame_seq_ % 3 == 0) ? 0xF800 : (frame_seq_ % 3 == 1) ? 0x07E0 : 0x001F);
            for (uint32_t i = 0; i < static_cast<uint32_t>(width_ * height_); ++i) {
                ptr[i] = color;
            }
        }
    }

    MockPattern pattern_;
    int ball_x_;
    int ball_y_;
    int ball_vx_;
    int ball_vy_;
    bool simulated_error_;
};

} // namespace camera
} // namespace drivers
} // namespace auroraos

#endif // AURORA_DRIVERS_MOCK_CAMERA_DRIVER_HPP
