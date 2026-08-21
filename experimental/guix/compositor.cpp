#include "compositor.hpp"
#include "window.hpp"
#include <algorithm>

namespace auroraos {
namespace guix {

void Rect::union_rect(const Rect& other) {
    if (other.is_empty())
        return;
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
    : screen_(screen_surface), gpu_(gpu), window_head_(nullptr), window_tail_(nullptr),
      focused_window_(nullptr), pointer_captured_window_(nullptr), background_color_(0x0000),
      background_surface_(nullptr), frame_count_(0) {
    damage_rect_.clear();
}

Compositor::~Compositor() {
    // 解除所有窗口的绑定，防止它们在析构时出现 Use-After-Free
    Window* curr = window_head_;
    while (curr) {
        curr->compositor_ = nullptr;
        curr = curr->next;
    }
    window_head_ = window_tail_ = focused_window_ = pointer_captured_window_ = nullptr;
}

void Compositor::add_window(Window* win) {
    if (!win)
        return;

    // 确保从任意原有链表中剥离
    win->next = nullptr;
    win->prev = nullptr;

    if (!window_head_) {
        window_head_ = window_tail_ = win;
        win->invalidate();
        return;
    }

    // 按 z_order 从小到大升序插入
    Window* curr = window_head_;
    while (curr && curr->get_z_order() <= win->get_z_order()) {
        curr = curr->next;
    }

    if (!curr) {
        // 插入到末尾（最高层）
        window_tail_->next = win;
        win->prev = window_tail_;
        window_tail_ = win;
    } else if (curr == window_head_) {
        // 插入到头部（最底层）
        win->next = window_head_;
        window_head_->prev = win;
        window_head_ = win;
    } else {
        // 插入到 curr 之前
        win->prev = curr->prev;
        win->next = curr;
        curr->prev->next = win;
        curr->prev = win;
    }

    win->invalidate();
}

void Compositor::remove_window(Window* win) {
    if (!win)
        return;

    if (focused_window_ == win) {
        focused_window_ = nullptr;
    }
    if (pointer_captured_window_ == win) {
        pointer_captured_window_ = nullptr;
    }

    win->invalidate();

    if (win->prev)
        win->prev->next = win->next;
    else
        window_head_ = win->next;

    if (win->next)
        win->next->prev = win->prev;
    else
        window_tail_ = win->prev;

    win->next = win->prev = nullptr;
}

void Compositor::raise_to_top(Window* win) {
    if (!win)
        return;

    int32_t max_z = window_tail_ ? window_tail_->get_z_order() : 0;
    if (win != window_tail_ || win->get_z_order() <= max_z) {
        win->set_z_order(max_z + 1);
    }
}

void Compositor::send_to_back(Window* win) {
    if (!win)
        return;

    int32_t min_z = window_head_ ? window_head_->get_z_order() : 0;
    if (win != window_head_ || win->get_z_order() >= min_z) {
        win->set_z_order(min_z - 1);
    }
}

size_t Compositor::get_window_count() const {
    size_t count = 0;
    Window* curr = window_head_;
    while (curr) {
        count++;
        curr = curr->next;
    }
    return count;
}

Window* Compositor::get_window_by_id(uint32_t id) const {
    Window* curr = window_head_;
    while (curr) {
        if (curr->get_id() == id)
            return curr;
        curr = curr->next;
    }
    return nullptr;
}

Window* Compositor::find_window_at(int32_t x, int32_t y) const {
    // 从顶层（tail）向底层（head）反向遍历查找首个命中可见窗口
    Window* curr = window_tail_;
    while (curr) {
        if (curr->is_visible() && curr->hit_test(x, y)) {
            return curr;
        }
        curr = curr->prev;
    }
    return nullptr;
}

void Compositor::set_focused_window(Window* win) {
    if (focused_window_ == win)
        return;

    if (focused_window_) {
        InputEvent ev{};
        ev.type = InputEventType::FocusOut;
        focused_window_->handle_event(ev);
    }

    focused_window_ = win;

    if (focused_window_) {
        InputEvent ev{};
        ev.type = InputEventType::FocusIn;
        focused_window_->handle_event(ev);
    }
}

void Compositor::add_damage(const Rect& rect) {
    damage_rect_.union_rect(rect);
}

void Compositor::invalidate_all() {
    if (screen_) {
        Rect r{0, 0, static_cast<int32_t>(screen_->get_width()), static_cast<int32_t>(screen_->get_height())};
        add_damage(r);
    }
}

bool Compositor::dispatch_input_event(const InputEvent& event) {
    if (event.type == InputEventType::PointerDown ||
        event.type == InputEventType::PointerUp ||
        event.type == InputEventType::PointerMove) {

        Window* target = nullptr;

        if (event.type == InputEventType::PointerDown) {
            target = find_window_at(event.x, event.y);
            pointer_captured_window_ = target;
            if (target) {
                set_focused_window(target);
            }
        } else if (event.type == InputEventType::PointerMove) {
            target = pointer_captured_window_ ? pointer_captured_window_ : find_window_at(event.x, event.y);
        } else if (event.type == InputEventType::PointerUp) {
            target = pointer_captured_window_ ? pointer_captured_window_ : find_window_at(event.x, event.y);
            pointer_captured_window_ = nullptr;
        }

        if (!target)
            return false;

        InputEvent local_ev = event;
        local_ev.x = event.x - target->get_x();
        local_ev.y = event.y - target->get_y();
        return target->handle_event(local_ev);
    } else if (event.type == InputEventType::KeyDown || event.type == InputEventType::KeyUp) {
        if (focused_window_) {
            return focused_window_->handle_event(event);
        }
    }
    return false;
}

void Compositor::composite() {
    if (damage_rect_.is_empty() || !screen_ || !gpu_) {
        return;
    }

    // 裁剪脏矩形至屏幕边界
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

    // 步骤 1: 渲染背景至脏区域
    if (background_surface_) {
        gpu::GpuCommand bg_blit;
        bg_blit.opcode = gpu::GpuOpcode::Blit;
        bg_blit.dst_surface = screen_;
        bg_blit.dst_x = damage_rect_.x;
        bg_blit.dst_y = damage_rect_.y;
        bg_blit.width = damage_rect_.w;
        bg_blit.height = damage_rect_.h;
        bg_blit.args.blit.src_surface = background_surface_;
        bg_blit.args.blit.src_x = damage_rect_.x;
        bg_blit.args.blit.src_y = damage_rect_.y;
        gpu_->submit(&bg_blit, 1);
    } else {
        gpu::GpuCommand clear_cmd;
        clear_cmd.opcode = gpu::GpuOpcode::FillRect;
        clear_cmd.dst_surface = screen_;
        clear_cmd.dst_x = damage_rect_.x;
        clear_cmd.dst_y = damage_rect_.y;
        clear_cmd.width = damage_rect_.w;
        clear_cmd.height = damage_rect_.h;
        clear_cmd.args.fill.color = background_color_;
        gpu_->submit(&clear_cmd, 1);
    }

    // 步骤 2: 自底向上（head 到 tail）遍历并合成可见窗口
    Window* curr = window_head_;
    while (curr) {
        if (curr->is_visible() && curr->get_surface()) {
            int32_t wx = curr->get_x();
            int32_t wy = curr->get_y();
            int32_t ww = static_cast<int32_t>(curr->get_width());
            int32_t wh = static_cast<int32_t>(curr->get_height());

            int32_t ix = std::max(damage_rect_.x, wx);
            int32_t iy = std::max(damage_rect_.y, wy);
            int32_t iw = std::min(damage_rect_.x + damage_rect_.w, wx + ww) - ix;
            int32_t ih = std::min(damage_rect_.y + damage_rect_.h, wy + wh) - iy;

            if (iw > 0 && ih > 0) {
                gpu::GpuCommand cmd;
                cmd.dst_surface = screen_;
                cmd.dst_x = ix;
                cmd.dst_y = iy;
                cmd.width = iw;
                cmd.height = ih;

                if (curr->get_alpha() == 255 && !curr->is_transparent()) {
                    cmd.opcode = gpu::GpuOpcode::Blit;
                    cmd.args.blit.src_surface = curr->get_surface();
                    cmd.args.blit.src_x = ix - wx;
                    cmd.args.blit.src_y = iy - wy;
                } else {
                    cmd.opcode = gpu::GpuOpcode::Blend;
                    cmd.args.blend.src_surface = curr->get_surface();
                    cmd.args.blend.src_x = ix - wx;
                    cmd.args.blend.src_y = iy - wy;
                    cmd.args.blend.alpha = curr->get_alpha();
                }

                gpu_->submit(&cmd, 1);
            }
        }
        curr = curr->next;
    }

    frame_count_++;
    damage_rect_.clear();
}

} // namespace guix
} // namespace auroraos
