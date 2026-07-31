#include "window.hpp"
#include "compositor.hpp"

namespace auroraos {
namespace guix {

Window::Window(uint32_t width, uint32_t height, gpu::GpuDevice* gpu, Compositor* compositor)
    : gpu_(gpu), compositor_(compositor), x_(0), y_(0), z_order_(0), next(nullptr), prev(nullptr) {
    backing_store_ = new gpu::Surface(width, height);
    if (compositor_) {
        compositor_->add_window(this);
    }
}

Window::~Window() {
    if (compositor_) {
        invalidate();               // 必须在 delete backing_store_ 之前调用，否则无宽高
        compositor_->remove_window(this);
    }
    delete backing_store_;
}

void Window::move(int32_t x, int32_t y) {
    // Invalidate old area
    invalidate();
    x_ = x;
    y_ = y;
    // Invalidate new area
    invalidate();
}

void Window::set_z_order(int32_t z) {
    if (z_order_ != z) {
        z_order_ = z;
        if (compositor_) {
            // Remove and re-add to re-sort
            compositor_->remove_window(this);
            compositor_->add_window(this);
        }
        invalidate();
    }
}

void Window::invalidate() {
    if (compositor_) {
        Rect r;
        r.x = x_;
        r.y = y_;
        r.w = backing_store_->get_width();
        r.h = backing_store_->get_height();
        compositor_->add_damage(r);
    }
}

void Window::fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t color) {
    gpu::GpuCommand cmd;
    cmd.opcode = gpu::GpuOpcode::FillRect;
    cmd.dst_surface = backing_store_;
    cmd.dst_x = x;
    cmd.dst_y = y;
    cmd.width = w;
    cmd.height = h;
    cmd.args.fill.color = color;
    
    if (gpu_) {
        gpu_->submit(&cmd, 1);
        invalidate(); // Mark window as dirty
    }
}

} // namespace guix
} // namespace auroraos
