#include "widget.hpp"
#include "window.hpp"

namespace auroraos {
namespace guix {

Widget::Widget()
    : x_(0), y_(0), w_(0), h_(0), id_(0),
      visible_(true), enabled_(true), focused_(false), user_data_(nullptr),
      parent_(nullptr), first_child_(nullptr), next_sibling_(nullptr), prev_sibling_(nullptr) {
}

Widget::~Widget() {
    remove_from_parent();

    // 级联解绑所有子节点
    Widget* child = first_child_;
    while (child) {
        Widget* next = child->next_sibling_;
        child->parent_ = nullptr;
        child->prev_sibling_ = nullptr;
        child->next_sibling_ = nullptr;
        child = next;
    }
    first_child_ = nullptr;
}

void Widget::set_position(int32_t x, int32_t y) {
    x_ = x;
    y_ = y;
}

void Widget::set_size(uint32_t w, uint32_t h) {
    w_ = w;
    h_ = h;
}

void Widget::set_bounds(int32_t x, int32_t y, uint32_t w, uint32_t h) {
    x_ = x;
    y_ = y;
    w_ = w;
    h_ = h;
}

Rect Widget::get_window_bounds() const {
    int32_t abs_x = x_;
    int32_t abs_y = y_;
    const Widget* cur = parent_;
    while (cur) {
        abs_x += cur->x_;
        abs_y += cur->y_;
        cur = cur->parent_;
    }
    return {abs_x, abs_y, static_cast<int32_t>(w_), static_cast<int32_t>(h_)};
}

void Widget::set_visible(bool visible) {
    visible_ = visible;
}

void Widget::set_enabled(bool enabled) {
    enabled_ = enabled;
}

void Widget::set_focused(bool focused) {
    focused_ = focused;
}

void Widget::add_child(Widget* child) {
    if (!child || child == this || child->parent_ == this)
        return;

    child->remove_from_parent();
    child->parent_ = this;
    child->next_sibling_ = nullptr;

    if (!first_child_) {
        child->prev_sibling_ = nullptr;
        first_child_ = child;
    } else {
        Widget* tail = first_child_;
        while (tail->next_sibling_) {
            tail = tail->next_sibling_;
        }
        tail->next_sibling_ = child;
        child->prev_sibling_ = tail;
    }
}

void Widget::remove_child(Widget* child) {
    if (!child || child->parent_ != this)
        return;

    if (child->prev_sibling_) {
        child->prev_sibling_->next_sibling_ = child->next_sibling_;
    } else {
        first_child_ = child->next_sibling_;
    }

    if (child->next_sibling_) {
        child->next_sibling_->prev_sibling_ = child->prev_sibling_;
    }

    child->parent_ = nullptr;
    child->prev_sibling_ = nullptr;
    child->next_sibling_ = nullptr;
}

void Widget::remove_from_parent() {
    if (parent_) {
        parent_->remove_child(this);
    }
}

bool Widget::on_event(Window* win, const InputEvent& event) {
    (void)win;
    (void)event;
    return false;
}

Widget* Widget::hit_test(int32_t x, int32_t y) {
    if (!visible_ || !enabled_)
        return nullptr;

    Rect wb = get_window_bounds();
    if (!wb.contains(x, y))
        return nullptr;

    // 从最顶层子节点 (末尾) 向最底层 (首部) 反向遍历命中
    Widget* tail = first_child_;
    while (tail && tail->next_sibling_) {
        tail = tail->next_sibling_;
    }

    Widget* child = tail;
    while (child) {
        Widget* hit = child->hit_test(x, y);
        if (hit)
            return hit;
        child = child->prev_sibling_;
    }

    return this;
}

void Widget::paint(Window* win) {
    (void)win;
}

void Widget::paint_tree(Window* win) {
    if (!visible_ || !win)
        return;

    paint(win);

    // 递归绘制子控件
    Widget* child = first_child_;
    while (child) {
        child->paint_tree(win);
        child = child->next_sibling_;
    }
}

void Widget::invalidate(Window* win) {
    if (win) {
        Rect wb = get_window_bounds();
        win->invalidate(wb);
    }
}

} // namespace guix
} // namespace auroraos
