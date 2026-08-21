#ifndef AURORA_UI_VIEW_HPP
#define AURORA_UI_VIEW_HPP

#include <stdint.h>
#include "ui_config.hpp"
#include "../drivers/input/gesture_recognizer.hpp"

namespace UI {

class ViewGroup;

enum class Visibility : uint8_t {
    VISIBLE = 0,
    INVISIBLE = 1,
    GONE = 2
};

struct Rect {
    int16_t x = 0;
    int16_t y = 0;
    uint16_t width = 0;
    uint16_t height = 0;

    bool contains(int16_t px, int16_t py) const {
        return (px >= x && px < x + width && py >= y && py < y + height);
    }
};

// ========================================================
// View: 所有 UI 组件的基类
// ========================================================
class View {
protected:
    int16_t x_;
    int16_t y_;
    uint16_t width_;
    uint16_t height_;
    bool is_dirty_;
    bool enabled_;
    Visibility visibility_;
    ViewGroup* parent_;

    void (*on_click_)(View*, void*) = nullptr;
    void* on_click_ctx_ = nullptr;

public:
    View(int16_t x, int16_t y, uint16_t w, uint16_t h)
        : x_(x), y_(y), width_(w), height_(h), is_dirty_(true), enabled_(true),
          visibility_(Visibility::VISIBLE), parent_(nullptr) {}

    // Destructor frees on_click_ctx_ if it was heap-allocated
    // (e.g., LuaCallbackCtx from lua_ui_binding.cpp)
    virtual ~View() {
        if (on_click_ctx_) {
            ::operator delete(on_click_ctx_);
            on_click_ctx_ = nullptr;
        }
    }

    // ========================================================
    // 核心生命周期方法
    // ========================================================

    // 渲染方法：必须由子类实现
    virtual void draw(UIRenderer& renderer) = 0;

    void set_on_click_listener(void (*cb)(View*, void*), void* ctx) {
        on_click_ = cb;
        on_click_ctx_ = ctx;
    }

    // Get click context for cleanup (used by Lua bindings)
    void* get_on_click_ctx() const {
        return on_click_ctx_;
    }

    // Clear click listener and context (for cleanup)
    void clear_on_click_listener() {
        on_click_ = nullptr;
        on_click_ctx_ = nullptr;
    }

    // 事件处理：如果子类处理了事件，返回 true；否则返回 false 继续向上传递
    virtual bool handle_gesture(const GestureEvent& event) {
        if (!enabled_ || visibility_ != Visibility::VISIBLE) {
            return false;
        }
        if (event.type == GestureType::TAP && contains(event.x, event.y)) {
            if (on_click_) {
                on_click_(this, on_click_ctx_);
                return true;
            }
        }
        return false;
    }

    // ========================================================
    // 视图层级与状态控制
    // ========================================================

    void set_parent(ViewGroup* parent) {
        parent_ = parent;
    }

    ViewGroup* get_parent() const {
        return parent_;
    }

    // 标记当前组件为“脏”，需要在下一帧重新渲染
    virtual void invalidate(); // 实现将在 view_group 中关联，这里先声明

    bool is_dirty() const {
        return is_dirty_;
    }

    void clear_dirty() {
        is_dirty_ = false;
    }

    // 可见性与交互状态
    void set_visibility(Visibility v) {
        if (visibility_ != v) {
            visibility_ = v;
            invalidate();
        }
    }

    Visibility get_visibility() const {
        return visibility_;
    }

    bool is_visible() const {
        return visibility_ == Visibility::VISIBLE;
    }

    void set_enabled(bool enabled) {
        if (enabled_ != enabled) {
            enabled_ = enabled;
            invalidate();
        }
    }

    bool is_enabled() const {
        return enabled_;
    }

    // 坐标与尺寸
    int16_t get_x() const {
        return x_;
    }

    int16_t get_y() const {
        return y_;
    }

    void set_position(int16_t x, int16_t y) {
        if (x_ != x || y_ != y) {
            x_ = x;
            y_ = y;
            invalidate();
        }
    }

    uint16_t get_width() const {
        return width_;
    }

    uint16_t get_height() const {
        return height_;
    }

    void set_size(uint16_t w, uint16_t h) {
        if (width_ != w || height_ != h) {
            width_ = w;
            height_ = h;
            invalidate();
        }
    }

    Rect get_bounds() const {
        return {x_, y_, width_, height_};
    }

    // 碰撞检测：判断手势坐标是否落在该组件范围内
    bool contains(int16_t px, int16_t py) const {
        return (px >= x_ && px < x_ + width_ && py >= y_ && py < y_ + height_);
    }
};

} // namespace UI

#endif // AURORA_UI_VIEW_HPP
