// =============================================================================
// drivers/gpu/surface.hpp
//
// AuroraOS GPU Surface — 非拥有 RGB565 像素缓冲视图
//
// 设计原则：
//  - 非拥有（R.3）：Surface 只包装调用方提供的内存（FrameBuffer 存储、
//    静态窗口缓冲等），不分配、不释放——零动态内存，符合 AGENTS §12
//  - 所有权归调用方；Surface 仅为借用视图，生命周期必须覆盖其使用期
//  - 与 experimental/drivers/gpu/surface.hpp 的差异：稳定版不接受
//    自行堆分配的构造路径，杜绝内核热路径隐式 new[]
// =============================================================================
#ifndef AURORA_DRIVERS_GPU_SURFACE_HPP
#define AURORA_DRIVERS_GPU_SURFACE_HPP

#include <stdint.h>

namespace aurora {
namespace gpu {

class Surface {
public:
    // 无效表面（宽高为 0），所有绘制操作对其为无操作
    constexpr Surface() noexcept : buffer_(nullptr), width_(0), height_(0) {}

    // 包装外部像素缓冲。buffer 须可容纳 width*height 个 RGB565 像素，
    // 且对齐到 2 字节（uint16_t 天然满足）。
    constexpr Surface(uint16_t* buffer, uint32_t width, uint32_t height) noexcept
        : buffer_(buffer), width_(buffer ? width : 0), height_(buffer ? height : 0) {}

    [[nodiscard]] constexpr bool valid() const noexcept {
        return buffer_ != nullptr && width_ != 0 && height_ != 0;
    }

    [[nodiscard]] constexpr uint32_t get_width() const noexcept {
        return width_;
    }

    [[nodiscard]] constexpr uint32_t get_height() const noexcept {
        return height_;
    }

    // 裸缓冲访问（非拥有）。行主序：pixel(x,y) = data()[y * width + x]
    [[nodiscard]] constexpr uint16_t* get_buffer() const noexcept {
        return buffer_;
    }

private:
    uint16_t* buffer_;
    uint32_t width_;
    uint32_t height_;
};

} // namespace gpu
} // namespace aurora

#endif // AURORA_DRIVERS_GPU_SURFACE_HPP
