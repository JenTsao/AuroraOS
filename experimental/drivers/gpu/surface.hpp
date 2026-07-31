#ifndef AURORA_GPU_SURFACE_HPP
#define AURORA_GPU_SURFACE_HPP

#include <stdint.h>
#include "../../kernel/page_allocator.hpp"

namespace auroraos {
namespace gpu {

class Surface {
public:
    Surface(uint32_t width, uint32_t height)
        : width_(width), height_(height), buffer_(nullptr) {
        
        uint64_t total_pixels = static_cast<uint64_t>(width_) * height_;
        // Prevent integer overflow and unreasonably large allocations
        if (total_pixels == 0 || total_pixels > (0xFFFFFFFFULL / sizeof(uint16_t))) {
            width_ = 0;
            height_ = 0;
            return;
        }

        // For simplicity in this iteration, we use new[] in kernel heap 
        // for soft rasterizer compatibility until physical mem manager is fully continuous.
        buffer_ = new uint16_t[static_cast<size_t>(total_pixels)];
        if (!buffer_) {
            width_ = 0;
            height_ = 0;
        }
    }
    
    ~Surface() {
        if (buffer_) {
            delete[] buffer_;
        }
    }

    Surface(const Surface&) = delete;
    Surface& operator=(const Surface&) = delete;

    uint32_t get_width() const { return width_; }
    uint32_t get_height() const { return height_; }
    void* get_buffer() const { return buffer_; }
    
private:
    uint32_t width_;
    uint32_t height_;
    uint16_t* buffer_;
};

} // namespace gpu
} // namespace auroraos

#endif // AURORA_GPU_SURFACE_HPP
