// =============================================================================
// drivers/gpu/gpu_device.hpp
//
// AuroraOS GPU 设备抽象 — 命令批处理接口
//
// 设计原则：
//  - 命令 ABI 与 experimental/drivers/gpu/gpu_device.hpp 保持兼容
//    （操作码取值一致），差异：坐标字段为 int32_t，支持负坐标裁剪；
//    稳定版使用 aurora::gpu 命名空间与实验版 auroraos::gpu 隔离
//  - submit() 为唯一入口；硬件 GPU 实现可将其转发到 DMA 环形缓冲，
//    SoftGpuDevice 则同步 CPU 执行
//  - 批内单条命令非法（空表面/越界）仅跳过该条，不中止整批——调用方
//    无需预裁剪；submit 仅在参数级错误（cmds 为空 / 超批上限）时返回 false
//  - GaussianBlur 是 CPU 密集操作，工作缓冲一律由调用方借用提供；
//    单条失败不改变像素，结果可经 out_result 回读
// =============================================================================
#ifndef AURORA_DRIVERS_GPU_DEVICE_HPP
#define AURORA_DRIVERS_GPU_DEVICE_HPP

#include <stdint.h>
#include <stddef.h>
#include "surface.hpp"

namespace aurora {
namespace gpu {

// 定义见 gaussian_blur.hpp；抽象层只需指针，避免把算法实现拖进命令 ABI 头。
enum class BlurResult : uint8_t;

enum class GpuOpcode : uint8_t {
    FillRect = 1,
    Blit = 2,
    Blend = 3,
    GaussianBlur = 4, // 追加取值，既有操作码与字段偏移不变
};

struct GpuCommand {
    GpuOpcode opcode;
    Surface* dst_surface; // 目标表面（借用，非拥有）
    int32_t dst_x;
    int32_t dst_y;
    uint32_t width;
    uint32_t height;

    union {
        struct {
            uint16_t color; // RGB565
        } fill;

        struct {
            Surface* src_surface; // 源表面（借用，非拥有）
            int32_t src_x;
            int32_t src_y;
        } blit;

        struct {
            Surface* src_surface; // 源表面（借用，非拥有）
            int32_t src_x;
            int32_t src_y;
            uint8_t alpha; // 0=全透 255=不透明
        } blend;

        struct {
            uint16_t sigma_q8; // σ×256；非 0 → 真高斯核 + 3σ 推导半径
            uint8_t radius;    // sigma_q8 == 0 时生效，走二项式近似核 (1..8)
            uint8_t reserved;
            uint16_t* scratch;      // 借用工作缓冲，零动态分配
            uint32_t scratch_words; // 容量，单位 uint16_t 字数
            BlurResult* out_result; // 可选出参；nullptr 表示沿用「批内跳过」语义
        } blur;
    } args;
};

class GpuDevice {
public:
    virtual ~GpuDevice() = default;

    // 提交一批命令。返回 false 仅表示参数级拒绝（cmds 为空或超批上限），
    // 不代表批内某条命令被跳过。
    virtual bool submit(GpuCommand* cmds, size_t count) = 0;
};

} // namespace gpu
} // namespace aurora

#endif // AURORA_DRIVERS_GPU_DEVICE_HPP
