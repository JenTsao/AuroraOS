#ifndef AURORA_GUIX_WINDOW_HPP
#define AURORA_GUIX_WINDOW_HPP

#include <stdint.h>
#include <stddef.h>
#include "compositor.hpp"
#include "../drivers/gpu/surface.hpp"
#include "../drivers/gpu/gpu_device.hpp"

namespace auroraos {
namespace guix {

class Compositor;
class Widget;

typedef void (*WindowEventHandler)(Window* win, const InputEvent& event, void* user_data);

class Window {
    friend class Compositor;

public:
    Window(uint32_t width, uint32_t height, gpu::GpuDevice* gpu, Compositor* compositor);
    ~Window();

    // 禁用拷贝与赋值
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    // ---- 几何与位置管理 ----
    void move(int32_t x, int32_t y);
    bool resize(uint32_t width, uint32_t height);
    void set_z_order(int32_t z);
    void bring_to_front();
    void send_to_back();

    int32_t get_x() const { return x_; }
    int32_t get_y() const { return y_; }
    uint32_t get_width() const { return backing_store_ ? backing_store_->get_width() : 0; }
    uint32_t get_height() const { return backing_store_ ? backing_store_->get_height() : 0; }
    int32_t get_z_order() const { return z_order_; }
    Rect get_bounds() const { return {x_, y_, static_cast<int32_t>(get_width()), static_cast<int32_t>(get_height())}; }
    Rect get_local_bounds() const { return {0, 0, static_cast<int32_t>(get_width()), static_cast<int32_t>(get_height())}; }

    gpu::Surface* get_surface() const { return backing_store_; }

    // ---- 窗口属性 ----
    void set_visible(bool visible);
    bool is_visible() const { return visible_; }

    void set_alpha(uint8_t alpha);
    uint8_t get_alpha() const { return alpha_; }

    void set_transparent(bool transparent);
    bool is_transparent() const { return transparent_; }

    void set_id(uint32_t id) { id_ = id; }
    uint32_t get_id() const { return id_; }

    void set_title(const char* title);
    const char* get_title() const { return title_; }

    void set_user_data(void* data) { user_data_ = data; }
    void* get_user_data() const { return user_data_; }

    bool is_focused() const;
    void focus();

    // ---- 脏矩形与重绘通知 ----
    void invalidate();
    void invalidate(const Rect& sub_rect);

    // ---- 命中检测与事件处理 ----
    bool hit_test(int32_t screen_x, int32_t screen_y) const;
    void set_event_handler(WindowEventHandler handler, void* user_data = nullptr);
    bool handle_event(const InputEvent& event);

    // ---- 控件树支持 ----
    void set_root_widget(Widget* root);
    Widget* get_root_widget() const { return root_widget_; }
    void paint_widgets();

    // ---- 窗口内绘图 API (2D 原语) ----
    void clear(uint16_t color = 0x0000);
    void draw_pixel(uint32_t x, uint32_t y, uint16_t color);
    void fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t color);
    void draw_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t color);
    void draw_line(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint16_t color);
    void draw_circle(int32_t xc, int32_t yc, int32_t r, uint16_t color);
    void fill_circle(int32_t xc, int32_t yc, int32_t r, uint16_t color);
    void draw_text(uint32_t x, uint32_t y, const char* text, uint16_t color, uint16_t bg_color = 0, bool transparent_bg = true, uint8_t scale = 1);

    void blit(uint32_t dst_x, uint32_t dst_y, const gpu::Surface* src, uint32_t src_x, uint32_t src_y, uint32_t w, uint32_t h);
    void blend(uint32_t dst_x, uint32_t dst_y, const gpu::Surface* src, uint32_t src_x, uint32_t src_y, uint32_t w, uint32_t h, uint8_t alpha);

    // 双向链表指针 (由 Compositor 维护)
    Window* next;
    Window* prev;

private:
    gpu::Surface* backing_store_;
    gpu::GpuDevice* gpu_;
    Compositor* compositor_;

    int32_t x_;
    int32_t y_;
    int32_t z_order_;
    uint32_t id_;
    char title_[32];

    bool visible_;
    bool transparent_;
    uint8_t alpha_;
    void* user_data_;

    WindowEventHandler event_handler_;
    void* event_user_data_;

    Widget* root_widget_;
    Widget* captured_widget_;
};

} // namespace guix
} // namespace auroraos

#endif // AURORA_GUIX_WINDOW_HPP
