#ifndef AURORA_DRIVERS_CAMERA_DRIVER_HPP
#define AURORA_DRIVERS_CAMERA_DRIVER_HPP

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "../../kernel/core/device.hpp"
#include "../../kernel/core/semaphore.hpp"
#include "../../kernel/task/task.hpp"
#include "../../hal/camera_hal.hpp"
#include "camera_types.hpp"

namespace auroraos {
namespace drivers {
namespace camera {

// 默认单帧缓冲区最大上限
#ifndef AURORA_CAMERA_MAX_BUFFER_SIZE
#define AURORA_CAMERA_MAX_BUFFER_SIZE (320 * 240 * 2)
#endif

class CameraDriverBase : public CharDevice {
public:
    explicit CameraDriverBase(const char* name = "video0",
                              uint8_t* ping = nullptr,
                              uint8_t* pong = nullptr,
                              size_t buf_cap = AURORA_CAMERA_MAX_BUFFER_SIZE)
        : CharDevice(name),
          hal_(nullptr),
          format_(PixelFormat::RGB565),
          width_(160),
          height_(120),
          fps_(30),
          capturing_(false),
          ping_buffer_(nullptr),
          pong_buffer_(nullptr),
          buffer_capacity_(buf_cap),
          owns_buffers_(false),
          active_buffer_idx_(0),
          frame_seq_(0),
          last_frame_tick_(0),
          stats_{},
          controls_{},
          frame_sem_(0) {
        if (ping && pong) {
            ping_buffer_ = ping;
            pong_buffer_ = pong;
            owns_buffers_ = false;
        } else {
            ping_buffer_ = new uint8_t[buffer_capacity_];
            pong_buffer_ = new uint8_t[buffer_capacity_];
            owns_buffers_ = true;
        }
    }

    virtual ~CameraDriverBase() override {
        capturing_ = false;
        if (hal_) {
            hal_->stop_dma_capture();
        }
        if (owns_buffers_) {
            delete[] ping_buffer_;
            delete[] pong_buffer_;
            ping_buffer_ = nullptr;
            pong_buffer_ = nullptr;
        }
    }

    // 绑定底层硬件 HAL
    void bind_hal(hal::ICameraHal* hal) {
        hal_ = hal;
    }

    // 绑定外部指定的连续 DMA 物理缓冲区 (例如专用 SRAM / PSRAM 区域)
    void bind_dma_buffers(uint8_t* ping, uint8_t* pong, size_t cap) {
        if (capturing_ || !ping || !pong || cap == 0) return;
        if (owns_buffers_) {
            delete[] ping_buffer_;
            delete[] pong_buffer_;
            owns_buffers_ = false;
        }
        ping_buffer_ = ping;
        pong_buffer_ = pong;
        buffer_capacity_ = cap;
    }

    // =========================================================================
    // 设备生命周期 (VFS / Device 接口实现)
    // =========================================================================
    int open() override {
        IrqGuard guard;
        if (!probe_sensor()) {
            return -1;
        }
        if (!apply_format(width_, height_, format_)) {
            return -1;
        }
        apply_controls(controls_);
        return 0;
    }

    int close() override {
        stop();
        return 0;
    }

    // 从摄像头读取一帧数据 (阻塞直到下一帧 DMA 传输就绪或超时)
    int read(char* buf, int len, int /*offset*/, void* /*priv*/) override {
        if (!buf || len <= 0 || !capturing_) {
            return -1;
        }

        // 等待新帧到达 (超时 1000ms)
        if (!frame_sem_.wait(1000)) {
            stats_.error_count++;
            return -1; // 超时
        }

        IrqGuard guard;
        size_t frame_sz = get_frame_byte_size(width_, height_, format_);
        size_t copy_sz = static_cast<size_t>(len) < frame_sz ? static_cast<size_t>(len) : frame_sz;

        // 提取后台已完成的乒乓缓冲区
        uint8_t* ready_buf = (active_buffer_idx_ == 0) ? pong_buffer_ : ping_buffer_;
        if (ready_buf) {
            memcpy(buf, ready_buf, copy_sz);
        }

        return static_cast<int>(copy_sz);
    }

    // 控制接口
    int ioctl(int request, void* arg, void* /*priv*/) override {
        switch (request) {
            case CAMERA_IOCTL_START_CAPTURE:
                return start() ? 0 : -1;

            case CAMERA_IOCTL_STOP_CAPTURE:
                return stop() ? 0 : -1;

            case CAMERA_IOCTL_SET_FMT: {
                if (!arg) return -1;
                auto* cfg = static_cast<const CameraFormatConfig*>(arg);
                return set_format(cfg->width, cfg->height, cfg->format, cfg->fps) ? 0 : -1;
            }

            case CAMERA_IOCTL_GET_FMT: {
                if (!arg) return -1;
                auto* cfg = static_cast<CameraFormatConfig*>(arg);
                cfg->width = width_;
                cfg->height = height_;
                cfg->format = format_;
                cfg->fps = fps_;
                return 0;
            }

            case CAMERA_IOCTL_SET_FPS: {
                if (!arg) return -1;
                uint8_t new_fps = *static_cast<const uint8_t*>(arg);
                fps_ = (new_fps > 0 && new_fps <= 60) ? new_fps : 30;
                return apply_format(width_, height_, format_) ? 0 : -1;
            }

            case CAMERA_IOCTL_GET_FPS: {
                if (!arg) return -1;
                *static_cast<uint8_t*>(arg) = fps_;
                return 0;
            }

            case CAMERA_IOCTL_SET_CONTROLS: {
                if (!arg) return -1;
                auto* ctrl = static_cast<const CameraControls*>(arg);
                controls_ = *ctrl;
                return apply_controls(controls_) ? 0 : -1;
            }

            case CAMERA_IOCTL_GET_CONTROLS: {
                if (!arg) return -1;
                *static_cast<CameraControls*>(arg) = controls_;
                return 0;
            }

            case CAMERA_IOCTL_GET_FRAME: {
                if (!arg || !capturing_) return -1;
                auto* frame = static_cast<CameraFrame*>(arg);

                if (!frame_sem_.wait(1000)) {
                    stats_.error_count++;
                    return -1;
                }

                IrqGuard guard;
                uint8_t* ready_buf = (active_buffer_idx_ == 0) ? pong_buffer_ : ping_buffer_;
                frame->buffer = ready_buf;
                frame->length = get_frame_byte_size(width_, height_, format_);
                frame->width = width_;
                frame->height = height_;
                frame->format = format_;
                frame->sequence = frame_seq_;
                frame->timestamp_ms = last_frame_tick_;
                frame->valid = true;
                return 0;
            }

            case CAMERA_IOCTL_RELEASE_FRAME:
                return 0;

            case CAMERA_IOCTL_GET_STATS: {
                if (!arg) return -1;
                *static_cast<CameraStats*>(arg) = stats_;
                return 0;
            }

            default:
                return -1;
        }
    }

    // =========================================================================
    // 基础控制 API
    // =========================================================================
    bool start() {
        IrqGuard guard;
        if (capturing_) return true;

        if (!start_hardware_capture()) {
            return false;
        }

        capturing_ = true;
        stats_.frames_captured = 0;
        stats_.frames_dropped = 0;
        return true;
    }

    bool stop() {
        IrqGuard guard;
        if (!capturing_) return true;

        stop_hardware_capture();
        capturing_ = false;
        return true;
    }

    bool set_format(uint16_t width, uint16_t height, PixelFormat fmt, uint8_t fps = 30) {
        if (width == 0 || height == 0) return false;

        size_t required_sz = get_frame_byte_size(width, height, fmt);
        if (required_sz > buffer_capacity_) {
            return false;
        }

        bool was_capturing = capturing_;
        if (was_capturing) {
            stop();
        }

        width_ = width;
        height_ = height;
        format_ = fmt;
        fps_ = (fps > 0 && fps <= 60) ? fps : 30;

        bool ok = apply_format(width_, height_, format_);
        if (ok && was_capturing) {
            start();
        }
        return ok;
    }

    // =========================================================================
    // DMA / 中断完成钩子 (由底层 HAL 或 ISR 调用)
    // =========================================================================
    void on_dma_frame_completed(uint8_t* /*completed_buffer*/, size_t /*length*/) {
        stats_.frames_captured++;
        frame_seq_++;
        last_frame_tick_ = Arch::get_cycle() / 1000;

        // 乒乓缓冲翻转
        active_buffer_idx_ = (active_buffer_idx_ == 0) ? 1 : 0;

        // 唤醒等待帧数据的任务
        frame_sem_.signal();
    }

    // 状态查询
    bool is_capturing() const { return capturing_; }
    uint16_t get_width() const { return width_; }
    uint16_t get_height() const { return height_; }
    PixelFormat get_format() const { return format_; }
    const CameraStats& get_stats() const { return stats_; }
    const CameraControls& get_controls() const { return controls_; }

protected:
    virtual bool probe_sensor() { return true; }
    virtual bool apply_format(uint16_t /*width*/, uint16_t /*height*/, PixelFormat /*fmt*/) { return true; }
    virtual bool apply_controls(const CameraControls& /*controls*/) { return true; }

    virtual bool start_hardware_capture() {
        if (!hal_) return true;
        size_t buf_sz = get_frame_byte_size(width_, height_, format_);
        return hal_->start_dma_capture(
            ping_buffer_, pong_buffer_, buf_sz,
            [](uint8_t* completed_buf, size_t len, void* ctx) {
                auto* self = static_cast<CameraDriverBase*>(ctx);
                self->on_dma_frame_completed(completed_buf, len);
            },
            this
        );
    }

    virtual bool stop_hardware_capture() {
        if (!hal_) return true;
        hal_->stop_dma_capture();
        return true;
    }

    hal::ICameraHal* hal_;

    PixelFormat format_;
    uint16_t width_;
    uint16_t height_;
    uint8_t fps_;
    bool capturing_;

    // 乒乓双缓冲区指针 (支持外部 DMA 映射或默认堆分配)
    uint8_t* ping_buffer_;
    uint8_t* pong_buffer_;
    size_t buffer_capacity_;
    bool owns_buffers_;
    uint8_t active_buffer_idx_; // 0: ping is DMA target, pong is ready; 1: pong is DMA target, ping is ready

    uint32_t frame_seq_;
    uint32_t last_frame_tick_;
    CameraStats stats_;
    CameraControls controls_;

    Semaphore frame_sem_;
};

} // namespace camera
} // namespace drivers
} // namespace auroraos

#endif // AURORA_DRIVERS_CAMERA_DRIVER_HPP
