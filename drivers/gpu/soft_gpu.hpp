// =============================================================================
// drivers/gpu/soft_gpu.hpp
//
// AuroraOS 软件 GPU（SoftGpuDevice）— CPU 同步光栅化后端
//
// 毕业说明（experimental → stable）：
//   相对 experimental/drivers/gpu/soft_gpu_device.* 的关键改进：
//     1. 非拥有 Surface：零动态内存分配
//     2. int32 有符号坐标 + int64 中间量：负坐标精确裁剪，
//        杜绝 (y+row)*width+x 的 uint32 回绕溢出
//     3. 自拷贝 memmove 语义：dst==src 且区域重叠时按行/列方向
//        选择遍历顺序，重叠搬运不再破坏未读像素
//     4. 批处理上限资源控制（kMaxBatchCommands）
// =============================================================================
#ifndef AURORA_DRIVERS_GPU_SOFT_GPU_HPP
#define AURORA_DRIVERS_GPU_SOFT_GPU_HPP

#include "gpu_device.hpp"

namespace aurora {
namespace gpu {

class SoftGpuDevice final : public GpuDevice {
public:
    // 单批命令上限：防御调用方失控循环提交，约束最坏执行时间
    static constexpr size_t kMaxBatchCommands = 256;

    SoftGpuDevice() = default;
    ~SoftGpuDevice() override = default;

    SoftGpuDevice(const SoftGpuDevice&) = delete;
    SoftGpuDevice& operator=(const SoftGpuDevice&) = delete;

    bool submit(GpuCommand* cmds, size_t count) noexcept override;

private:
    static void do_fill_rect(const GpuCommand& cmd) noexcept;
    static void do_blit(const GpuCommand& cmd) noexcept;
    static void do_blend(const GpuCommand& cmd) noexcept;

    // RGB565 线性混合（Q8 定点整数，无浮点）。alpha: 0=全 dst, 255=全 src
    [[nodiscard]] static uint16_t blend_rgb565(uint16_t dst, uint16_t src, uint8_t alpha) noexcept;
};

} // namespace gpu
} // namespace aurora

#endif // AURORA_DRIVERS_GPU_SOFT_GPU_HPP
