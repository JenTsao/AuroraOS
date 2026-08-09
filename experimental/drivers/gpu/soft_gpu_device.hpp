#ifndef AURORA_SOFT_GPU_DEVICE_HPP
#define AURORA_SOFT_GPU_DEVICE_HPP

#include "gpu_device.hpp"

namespace auroraos {
namespace gpu {

class SoftGpuDevice : public GpuDevice {
public:
    SoftGpuDevice() = default;
    ~SoftGpuDevice() override = default;

    bool submit(GpuCommand* cmds, size_t count) override;

private:
    void do_fill_rect(const GpuCommand& cmd);
    void do_blit(const GpuCommand& cmd);
    void do_blend(const GpuCommand& cmd);
};

} // namespace gpu
} // namespace auroraos

#endif // AURORA_SOFT_GPU_DEVICE_HPP
