#include "soft_gpu_device.hpp"

namespace auroraos {
namespace gpu {

bool SoftGpuDevice::submit(GpuCommand* cmds, size_t count) {
    if (!cmds) return false;
    
    // In a real hardware GPU, this would just push to a DMA ring buffer.
    // Here we execute synchronously on the CPU.
    for (size_t i = 0; i < count; ++i) {
        const GpuCommand& cmd = cmds[i];
        if (!cmd.dst_surface) continue;

        switch (cmd.opcode) {
            case GpuOpcode::FillRect:
                do_fill_rect(cmd);
                break;
            case GpuOpcode::Blit:
                do_blit(cmd);
                break;
            case GpuOpcode::Blend:
                do_blend(cmd);
                break;
            default:
                break;
        }
    }
    return true;
}

void SoftGpuDevice::do_fill_rect(const GpuCommand& cmd) {
    uint32_t dst_w = cmd.dst_surface->get_width();
    uint32_t dst_h = cmd.dst_surface->get_height();
    uint16_t* dst_buf = static_cast<uint16_t*>(cmd.dst_surface->get_buffer());
    
    uint32_t x = cmd.dst_x;
    uint32_t y = cmd.dst_y;
    uint32_t w = cmd.width;
    uint32_t h = cmd.height;
    
    if (x >= dst_w || y >= dst_h) return;
    if (w > dst_w - x) w = dst_w - x;
    if (h > dst_h - y) h = dst_h - y;
    
    uint16_t color = cmd.args.fill.color;
    
    for (uint32_t row = 0; row < h; ++row) {
        uint32_t dst_idx = (y + row) * dst_w + x;
        for (uint32_t col = 0; col < w; ++col) {
            dst_buf[dst_idx + col] = color;
        }
    }
}

void SoftGpuDevice::do_blit(const GpuCommand& cmd) {
    if (!cmd.args.blit.src_surface) return;
    
    uint32_t dst_w = cmd.dst_surface->get_width();
    uint32_t dst_h = cmd.dst_surface->get_height();
    uint16_t* dst_buf = static_cast<uint16_t*>(cmd.dst_surface->get_buffer());
    
    uint32_t src_w = cmd.args.blit.src_surface->get_width();
    uint32_t src_h = cmd.args.blit.src_surface->get_height();
    uint16_t* src_buf = static_cast<uint16_t*>(cmd.args.blit.src_surface->get_buffer());
    
    uint32_t dst_x = cmd.dst_x;
    uint32_t dst_y = cmd.dst_y;
    uint32_t src_x = cmd.args.blit.src_x;
    uint32_t src_y = cmd.args.blit.src_y;
    uint32_t w = cmd.width;
    uint32_t h = cmd.height;
    
    // Bounds checking
    if (dst_x >= dst_w || dst_y >= dst_h) return;
    if (src_x >= src_w || src_y >= src_h) return;
    
    if (w > dst_w - dst_x) w = dst_w - dst_x;
    if (h > dst_h - dst_y) h = dst_h - dst_y;
    if (w > src_w - src_x) w = src_w - src_x;
    if (h > src_h - src_y) h = src_h - src_y;
    
    for (uint32_t row = 0; row < h; ++row) {
        uint32_t dst_idx = (dst_y + row) * dst_w + dst_x;
        uint32_t src_idx = (src_y + row) * src_w + src_x;
        for (uint32_t col = 0; col < w; ++col) {
            dst_buf[dst_idx + col] = src_buf[src_idx + col];
        }
    }
}

void SoftGpuDevice::do_blend(const GpuCommand& cmd) {
    // Simplified alpha blend for RGB565 (assuming alpha is applied to source)
    if (!cmd.args.blend.src_surface) return;
    
    uint32_t dst_w = cmd.dst_surface->get_width();
    uint32_t dst_h = cmd.dst_surface->get_height();
    uint16_t* dst_buf = static_cast<uint16_t*>(cmd.dst_surface->get_buffer());
    
    uint32_t src_w = cmd.args.blend.src_surface->get_width();
    uint32_t src_h = cmd.args.blend.src_surface->get_height();
    uint16_t* src_buf = static_cast<uint16_t*>(cmd.args.blend.src_surface->get_buffer());
    
    uint32_t dst_x = cmd.dst_x;
    uint32_t dst_y = cmd.dst_y;
    uint32_t src_x = cmd.args.blend.src_x;
    uint32_t src_y = cmd.args.blend.src_y;
    uint32_t w = cmd.width;
    uint32_t h = cmd.height;
    uint8_t alpha = cmd.args.blend.alpha; // 0-255
    
    // Bounds checking
    if (dst_x >= dst_w || dst_y >= dst_h) return;
    if (src_x >= src_w || src_y >= src_h) return;
    
    if (w > dst_w - dst_x) w = dst_w - dst_x;
    if (h > dst_h - dst_y) h = dst_h - dst_y;
    if (w > src_w - src_x) w = src_w - src_x;
    if (h > src_h - src_y) h = src_h - src_y;
    
    for (uint32_t row = 0; row < h; ++row) {
        uint32_t dst_idx = (dst_y + row) * dst_w + dst_x;
        uint32_t src_idx = (src_y + row) * src_w + src_x;
        for (uint32_t col = 0; col < w; ++col) {
            uint16_t src_c = src_buf[src_idx + col];
            uint16_t dst_c = dst_buf[dst_idx + col];
            
            // Very simplified blending (extract R, G, B, blend, repack)
            // Real implementation would optimize this
            uint8_t sr = (src_c >> 11) & 0x1F;
            uint8_t sg = (src_c >> 5) & 0x3F;
            uint8_t sb = src_c & 0x1F;
            
            uint8_t dr = (dst_c >> 11) & 0x1F;
            uint8_t dg = (dst_c >> 5) & 0x3F;
            uint8_t db = dst_c & 0x1F;
            
            uint8_t rr = (sr * alpha + dr * (255 - alpha)) / 255;
            uint8_t rg = (sg * alpha + dg * (255 - alpha)) / 255;
            uint8_t rb = (sb * alpha + db * (255 - alpha)) / 255;
            
            dst_buf[dst_idx + col] = (rr << 11) | (rg << 5) | rb;
        }
    }
}

} // namespace gpu
} // namespace auroraos
