#ifndef AURORA_GUIX_COMPOSITOR_HPP
#define AURORA_GUIX_COMPOSITOR_HPP

#include "window.hpp"
#include "../drivers/gpu/gpu_device.hpp"
#include "../drivers/gpu/surface.hpp"

namespace auroraos {
namespace guix {

struct Rect {
    int32_t x, y;
    int32_t w, h;
    
    bool is_empty() const { return w <= 0 || h <= 0; }
    void clear() { x = y = 0; w = h = 0; }
    void union_rect(const Rect& other);
};

class Compositor {
public:
    Compositor(gpu::Surface* screen_surface, gpu::GpuDevice* gpu);
    ~Compositor();
    
    void add_window(Window* win);
    void remove_window(Window* win);
    
    // Add a dirty rectangle to the screen
    void add_damage(const Rect& rect);
    
    // Composite all windows to the screen surface
    void composite();

private:

    gpu::Surface* screen_;
    gpu::GpuDevice* gpu_;
    
    Window* window_head_;
    Window* window_tail_;
    
    Rect damage_rect_;
};

} // namespace guix
} // namespace auroraos

#endif // AURORA_GUIX_COMPOSITOR_HPP
