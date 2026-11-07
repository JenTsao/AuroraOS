// =============================================================================
// drivers/virtio/virtio_gpu.hpp
//
// virtio-gpu 2D 显示后端：实现与 drivers/display/oled_driver_mock.hpp 相同的
// set_window()/write_patch(ColorRGB565*) 契约，使 FrameBuffer::flush 可直接
// 刷入 virtio-gpu 资源并 scanout（不 include ui/*，模板鸭子类型无需继承）。
//
// 像素管线：逻辑 128×64 单色（或 RGB565）→ B8G8R8X8 物理显存，整数倍放大
// （scale×scale）。write_patch 累计像素数达到窗口面积时自动发
// TRANSFER_TO_HOST_2D + RESOURCE_FLUSH 收尾（scanout 仅 init 时设置一次）。
// 轮询模式：命令同步提交 + 自旋等待完成（带上限）。
// =============================================================================
#ifndef AURORA_VIRTIO_GPU_HPP
#define AURORA_VIRTIO_GPU_HPP

#include <stdint.h>
#include "virtio_defs.hpp"
#include "virtio_queue.hpp"

namespace auroraos {
namespace virtio {

// 把 count 个 RGB565 源像素按行主序在逻辑窗口 [win_x0..win_x1] 内推进光标，
// 每像素放大 scale×scale 写入 B8G8R8X8 帧缓冲（纯函数，host 可测）。
// (cur_x, cur_y) 为逻辑坐标光标；返回实际写入像素数（到窗口末即停）。
uint32_t gpu_blit_scaled(uint8_t* fb, uint32_t fb_w,
                         const uint16_t* src, uint32_t count,
                         uint16_t win_x0, uint16_t win_y0, uint16_t win_x1,
                         uint32_t scale, bool mono1bit,
                         uint16_t& cur_x, uint16_t& cur_y) noexcept;

// RGB565 像素 → B8G8R8X8 字（纯函数，host 可测）。
// mono1bit：过阈值（≥0x8410）→ 白，否则黑；RGB565：解包 5:6:5。
uint32_t gpu_pixel_to_b8g8r8x8(uint16_t pixel, bool mono1bit) noexcept;

class VirtioGpuDisplay {
public:
    static constexpr uint16_t QUEUE_SIZE = 8;
    static constexpr uint32_t MAX_POLL_SPINS = 1000000;
    static constexpr uint32_t RESOURCE_ID = 1;
    // 物理显存上限（scale=4 于 128×64 → 512×256×4B = 512KB）
    static constexpr uint32_t FB_MAX_W = 512;
    static constexpr uint32_t FB_MAX_H = 256;

    VirtioGpuDisplay(uint16_t logical_w, uint16_t logical_h, uint32_t scale,
                     IVirtioTransport& transport, bool mono1bit = true) noexcept
        : transport_(transport), logical_w_(logical_w), logical_h_(logical_h),
          scale_(scale), mono1bit_(mono1bit) {}

    // 板级工厂：按 BOARD_VIRTIO_* 与 DISPLAY_* 常量构造（见 virtio_gpu.cpp）
    static VirtioGpuDisplay& instance();

    bool init() noexcept;

    bool ready() const noexcept {
        return ready_;
    }

    // probe 是否命中（区分「无设备跳过」与「初始化失败」两类日志）
    bool probed() const noexcept {
        return probed_;
    }

    // ---- FrameBuffer::flush 契约 ----
    void set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1);
    void write_patch(const uint16_t* buffer, uint32_t pixel_count);

    uint16_t get_width() const noexcept {
        return logical_w_;
    }
    uint16_t get_height() const noexcept {
        return logical_h_;
    }

    // 测试观察用：最近一次收尾提交的物理矩形（TRANSFER/FLUSH rect）
    const GpuRect& last_commit_rect() const noexcept {
        return last_rect_;
    }

    // 收尾命令提交失败计数（板上自检校验用，应保持 0）
    uint32_t commit_errors() const noexcept {
        return commit_errors_;
    }

    VirtQueue& queue() noexcept {
        return queue_;
    }

    // 环缓冲存储访问（host 测试在 init 前绑定设备模拟器用：init 会发命令）
    VirtQueue::Rings ring_storage() noexcept {
        return VirtQueue::Rings{desc_, &avail_, &used_};
    }

private:
    // 提交一条 ctrl 命令并等待响应；resp_type 非空时回写响应码
    bool submit_cmd(const void* cmd, uint32_t cmd_len, uint32_t resp_len,
                    uint32_t* resp_type) noexcept;
    void commit_window() noexcept;

    IVirtioTransport& transport_;
    VirtQueue queue_;
    alignas(4096) VirtqDesc desc_[QUEUE_SIZE];
    alignas(4096) AvailRing avail_;
    alignas(4096) UsedRing used_;
    alignas(4096) uint8_t fb_[FB_MAX_W * FB_MAX_H * 4];
    alignas(4) uint8_t cmd_buf_[128];
    alignas(4) uint8_t resp_buf_[GPU_DISPLAY_INFO_RESP_SIZE];

    uint16_t logical_w_;
    uint16_t logical_h_;
    uint32_t scale_;
    bool mono1bit_;
    bool ready_ = false;
    bool probed_ = false;

    // 窗口/光标状态（set_window 重置，write_patch 推进）
    uint16_t win_x0_ = 0, win_y0_ = 0, win_x1_ = 0, win_y1_ = 0;
    uint16_t cur_x_ = 0, cur_y_ = 0;
    uint32_t win_pixels_ = 0;
    uint32_t got_pixels_ = 0;
    uint32_t commit_errors_ = 0;
    GpuRect last_rect_{};
};

} // namespace virtio
} // namespace auroraos

#endif // AURORA_VIRTIO_GPU_HPP
