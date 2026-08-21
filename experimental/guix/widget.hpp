#ifndef AURORA_GUIX_WIDGET_HPP
#define AURORA_GUIX_WIDGET_HPP

#include <stdint.h>
#include <stddef.h>
#include "compositor.hpp"

namespace auroraos {
namespace guix {

class Window;

enum class Alignment : uint8_t {
    Left = 0,
    Center,
    Right
};

class Widget {
public:
    Widget();
    virtual ~Widget();

    // 禁用拷贝与赋值
    Widget(const Widget&) = delete;
    Widget& operator=(const Widget&) = delete;

    // ---- 几何与位置管理 (相对于父控件或窗口) ----
    void set_position(int32_t x, int32_t y);
    void set_size(uint32_t w, uint32_t h);
    void set_bounds(int32_t x, int32_t y, uint32_t w, uint32_t h);

    int32_t get_x() const { return x_; }
    int32_t get_y() const { return y_; }
    uint32_t get_width() const { return w_; }
    uint32_t get_height() const { return h_; }
    Rect get_bounds() const { return {x_, y_, static_cast<int32_t>(w_), static_cast<int32_t>(h_)}; }

    // 计算在所属 Window 坐标系下的绝对 Rect
    Rect get_window_bounds() const;

    // ---- 状态与属性 ----
    void set_visible(bool visible);
    bool is_visible() const { return visible_; }

    void set_enabled(bool enabled);
    bool is_enabled() const { return enabled_; }

    void set_id(uint32_t id) { id_ = id; }
    uint32_t get_id() const { return id_; }

    void set_user_data(void* data) { user_data_ = data; }
    void* get_user_data() const { return user_data_; }

    bool is_focused() const { return focused_; }
    void set_focused(bool focused);

    // ---- 树形层级管理 ----
    void add_child(Widget* child);
    void remove_child(Widget* child);
    void remove_from_parent();

    Widget* get_parent() const { return parent_; }
    Widget* get_first_child() const { return first_child_; }
    Widget* get_next_sibling() const { return next_sibling_; }
    Widget* get_prev_sibling() const { return prev_sibling_; }

    // ---- 事件响应与命中测试 ----
    virtual bool on_event(Window* win, const InputEvent& event);
    virtual Widget* hit_test(int32_t x, int32_t y);

    // ---- 绘制与渲染循环 ----
    virtual void paint(Window* win);
    void paint_tree(Window* win);

    // ---- 脏区域标记 ----
    void invalidate(Window* win);

protected:
    int32_t x_;
    int32_t y_;
    uint32_t w_;
    uint32_t h_;
    uint32_t id_;

    bool visible_;
    bool enabled_;
    bool focused_;
    void* user_data_;

    Widget* parent_;
    Widget* first_child_;
    Widget* next_sibling_;
    Widget* prev_sibling_;
};

} // namespace guix
} // namespace auroraos

#endif // AURORA_GUIX_WIDGET_HPP
