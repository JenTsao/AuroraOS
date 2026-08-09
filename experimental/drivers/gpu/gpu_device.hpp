#ifndef AURORA_GPU_DEVICE_HPP
#define AURORA_GPU_DEVICE_HPP

#include <stdint.h>
#include <stddef.h>
#include "surface.hpp"

namespace auroraos {
namespace gpu {

enum class GpuOpcode : uint8_t {
    FillRect = 1,
    Blit = 2,
    Blend = 3
};

struct GpuCommand {
    GpuOpcode opcode;
    Surface* dst_surface;
    uint32_t dst_x, dst_y;
    uint32_t width, height;
    
    union {
        struct {
            uint16_t color; // RGB565
        } fill;
        
        struct {
            Surface* src_surface;
            uint32_t src_x, src_y;
        } blit;
        
        struct {
            Surface* src_surface;
            uint32_t src_x, src_y;
            uint8_t alpha;
        } blend;
    } args;
};

class GpuDevice {
public:
    virtual ~GpuDevice() = default;

    // Asynchronously submit a batch of commands to the GPU.
    // Returns true if successfully enqueued.
    virtual bool submit(GpuCommand* cmds, size_t count) = 0;
};

} // namespace gpu
} // namespace auroraos

#endif // AURORA_GPU_DEVICE_HPP
