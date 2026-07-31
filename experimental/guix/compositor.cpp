#include "compositor.hpp"
#include <algorithm>

namespace auroraos {
namespace guix {

void Rect::union_rect(const Rect& other) {
    if (other.is_empty()) return;
    if (is_empty()) {
        *this = other;
        return;
    }
    
    int32_t min_x = std::min(x, other.x);
    int32_t min_y = std::min(y, other.y);
    int32_t max_x = std::max(x + w, other.x + other.w);
    int32_t max_y = std::max(y + h, other.y + other.h);
    
    x = min_x;
    y = min_y;
    w = max_x - min_x;
    h = max_y - min_y;
}

Compositor::Compositor(gpu::Surface* screen_surface, gpu::GpuDevice* gpu)
    : screen_(screen_surface), gpu_(gpu), window_head_(nullptr), window_tail_(nullptr) {
    damage_rect_.clear();
}

Compositor::~Compositor() {
    // 解除所有窗口的绑定，防止它们在析构时出现 Use-After-Free
    Window* curr = window_head_;
    while (curr) {
        curr->compositor_ = nullptr;
        curr = curr->next;
    }
}

void Compositor::add_window(Window* win) {
    if (!win) return;
    
    // Insert into sorted linked list based on z_order
    win->next = nullptr;
    win->prev = nullptr;
    
    if (!window_head_) {
        window_head_ = window_tail_ = win;
        return;
    }
    
    // Find insertion point
    Window* curr = window_head_;
    while (curr && curr->get_z_order() <= win->get_z_order()) {
        curr = curr->next;
    }
    
    if (!curr) {
        // Insert at tail
        window_tail_->next = win;
        win->prev = window_tail_;
        window_tail_ = win;
    } else if (curr == window_head_) {
        // Insert at head
        win->next = window_head_;
        window_head_->prev = win;
        window_head_ = win;
    } else {
        // Insert before curr
        win->prev = curr->prev;
        win->next = curr;
        curr->prev->next = win;
        curr->prev = win;
    }
}

void Compositor::remove_window(Window* win) {
    if (!win) return;
    
    if (win->prev) win->prev->next = win->next;
    else window_head_ = win->next;
    
    if (win->next) win->next->prev = win->prev;
    else window_tail_ = win->prev;
    
    win->next = win->prev = nullptr;
}

void Compositor::add_damage(const Rect& rect) {
    damage_rect_.union_rect(rect);
}

void Compositor::composite() {
    if (damage_rect_.is_empty() || !screen_ || !gpu_) {
        return;
    }
    
    // Clip damage rect to screen boundaries
    if (damage_rect_.x < 0) {
        damage_rect_.w += damage_rect_.x;
        damage_rect_.x = 0;
    }
    if (damage_rect_.y < 0) {
        damage_rect_.h += damage_rect_.y;
        damage_rect_.y = 0;
    }
    
    int32_t screen_w = static_cast<int32_t>(screen_->get_width());
    int32_t screen_h = static_cast<int32_t>(screen_->get_height());
    
    if (damage_rect_.x + damage_rect_.w > screen_w) {
        damage_rect_.w = screen_w - damage_rect_.x;
    }
    if (damage_rect_.y + damage_rect_.h > screen_h) {
        damage_rect_.h = screen_h - damage_rect_.y;
    }
    
    if (damage_rect_.is_empty()) {
        damage_rect_.clear();
        return;
    }

    // Step 1: Clear the damaged area to black (or desktop color)
    gpu::GpuCommand clear_cmd;
    clear_cmd.opcode = gpu::GpuOpcode::FillRect;
    clear_cmd.dst_surface = screen_;
    clear_cmd.dst_x = damage_rect_.x;
    clear_cmd.dst_y = damage_rect_.y;
    clear_cmd.width = damage_rect_.w;
    clear_cmd.height = damage_rect_.h;
    clear_cmd.args.fill.color = 0x0000;
    gpu_->submit(&clear_cmd, 1);

    // Step 2: Traverse windows back-to-front and composite
    Window* curr = window_head_;
    while (curr) {
        // Calculate intersection of window and damage_rect
        int32_t wx = curr->get_x();
        int32_t wy = curr->get_y();
        int32_t ww = curr->get_width();
        int32_t wh = curr->get_height();
        
        int32_t ix = std::max(damage_rect_.x, wx);
        int32_t iy = std::max(damage_rect_.y, wy);
        int32_t iw = std::min(damage_rect_.x + damage_rect_.w, wx + ww) - ix;
        int32_t ih = std::min(damage_rect_.y + damage_rect_.h, wy + wh) - iy;
        
        if (iw > 0 && ih > 0) {
            // Submit Blit command for the intersecting region
            gpu::GpuCommand blit;
            blit.opcode = gpu::GpuOpcode::Blit; // Assumes opaque windows for now. Alpha blend can be used later.
            blit.dst_surface = screen_;
            blit.dst_x = ix;
            blit.dst_y = iy;
            blit.width = iw;
            blit.height = ih;
            blit.args.blit.src_surface = curr->get_surface();
            blit.args.blit.src_x = ix - wx;
            blit.args.blit.src_y = iy - wy;
            
            gpu_->submit(&blit, 1);
        }
        
        curr = curr->next;
    }
    
    // Reset damage rect after compositing
    damage_rect_.clear();
}

} // namespace guix
} // namespace auroraos
