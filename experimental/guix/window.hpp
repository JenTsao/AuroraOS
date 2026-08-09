#ifndef AURORA_GUIX_WINDOW_HPP
#define AURORA_GUIX_WINDOW_HPP

#include <stdint.h>
#include "../drivers/gpu/surface.hpp"
#include "../drivers/gpu/gpu_device.hpp"

namespace auroraos {
namespace guix {

class Compositor;

class Window {
    friend class Compositor;
public:
    Window(uint32_t width, uint32_t height, gpu::GpuDevice* gpu, Compositor* compositor);
    ~Window();

    // Geometry
    void move(int32_t x, int32_t y);
    void set_z_order(int32_t z);
    
    int32_t get_x() const { return x_; }
    int32_t get_y() const { return y_; }
    uint32_t get_width() const { return backing_store_->get_width(); }
    uint32_t get_height() const { return backing_store_->get_height(); }
    int32_t get_z_order() const { return z_order_; }
    
    gpu::Surface* get_surface() const { return backing_store_; }
    
    // Tells the compositor that this window has changed (needs redraw)
    void invalidate();

    // Drawing API (for the window itself)
    void fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t color);

    // Linked list pointers for Compositor
    Window* next;
    Window* prev;

private:
    gpu::Surface* backing_store_;
    gpu::GpuDevice* gpu_;
    Compositor* compositor_;
    
    int32_t x_, y_;
    int32_t z_order_;
};

} // namespace guix
} // namespace auroraos

#endif // AURORA_GUIX_WINDOW_HPP
