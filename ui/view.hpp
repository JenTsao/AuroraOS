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
// ListenerSlot: 「函数指针 + 上下文指针」的统一所有权载体
//
// 所有权契约（一个概念一个名字，无 owned/borrowed 后缀）：
//  - deleter == nullptr ⇒ 借用（borrowed）：ctx 由调用方管理生命周期，
//    View 析构时不得释放它（例如传 this 的场景）。
//  - deleter != nullptr ⇒ 拥有（owned）：View 析构、或该槽被覆盖时，
//    调用 deleter(ctx) 释放 ctx。
// release() 幂等：空槽调用、重复调用均安全。
// ========================================================
template <class Fn> struct ListenerSlot {
    Fn fn = nullptr;
    void* ctx = nullptr;
    void (*deleter)(void*) = nullptr; // 非空 ⇒ 本 View 拥有该 ctx；空 = 借用

    void release() noexcept {
        if (ctx && deleter)
            deleter(ctx);
        fn = nullptr;
        ctx = nullptr;
        deleter = nullptr;
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

    // 四类回调各用一个 ListenerSlot 统一管理「函数指针 + ctx 所有权」。
    ListenerSlot<void (*)(View*, void*)> click_;
    ListenerSlot<void (*)(View*, void*)> long_click_;
    ListenerSlot<void (*)(View*, void*)> double_click_;
    ListenerSlot<bool (*)(View*, const GestureEvent&, void*)> touch_;

public:
    View(int16_t x, int16_t y, uint16_t w, uint16_t h)
        : x_(x), y_(y), width_(w), height_(h), is_dirty_(true), enabled_(true), visibility_(Visibility::VISIBLE),
          parent_(nullptr) {}

    // 析构：释放全部 4 个「拥有」的 ctx（仅 deleter != nullptr 的槽）。
    //
    // 缺陷 A 修复点：旧版无条件 ::operator delete(on_click_ctx_)，会把
    // 「ctx == 正在析构的 Screen 自身」这类借用场景变成对活对象的二次释放
    // （pop → delete old_top → ~Screen → ~ViewGroup → delete children_[i]
    //  → ~View → ::operator delete(Screen 自身)）。改为按 deleter 判定后，
    // 借用 ctx 不再被触碰，且 4 个槽的 owned ctx 都能回收（旧版只回收 click，
    // long/double/touch 的 ctx 从未释放，属既有泄漏，一并修正）。
    virtual ~View() {
        click_.release();
        long_click_.release();
        double_click_.release();
        touch_.release();
    }

    // ========================================================
    // 核心生命周期方法
    // ========================================================

    // 渲染方法：必须由子类实现
    virtual void draw(UIRenderer& renderer) = 0;

    // 设置监听回调。deleter 非空表示本 View 接管 ctx 的所有权：
    // 覆盖旧值时先 release()（释放旧 owned ctx，顺带修掉覆盖泄漏），
    // 析构时再由对应槽释放。deleter 为空表示借用 ctx。
    void set_on_click_listener(void (*cb)(View*, void*), void* ctx, void (*deleter)(void*) = nullptr) {
        click_.release();
        click_.fn = cb;
        click_.ctx = ctx;
        click_.deleter = deleter;
    }

    void set_on_long_click_listener(void (*cb)(View*, void*), void* ctx, void (*deleter)(void*) = nullptr) {
        long_click_.release();
        long_click_.fn = cb;
        long_click_.ctx = ctx;
        long_click_.deleter = deleter;
    }

    void set_on_double_click_listener(void (*cb)(View*, void*), void* ctx, void (*deleter)(void*) = nullptr) {
        double_click_.release();
        double_click_.fn = cb;
        double_click_.ctx = ctx;
        double_click_.deleter = deleter;
    }

    void set_on_touch_listener(bool (*cb)(View*, const GestureEvent&, void*), void* ctx,
                               void (*deleter)(void*) = nullptr) {
        touch_.release();
        touch_.fn = cb;
        touch_.ctx = ctx;
        touch_.deleter = deleter;
    }

    // Get click context for cleanup (used by Lua bindings)
    void* get_on_click_ctx() const {
        return click_.ctx;
    }

    // Clear click listener and context (for cleanup). 会释放 owned ctx。
    void clear_on_click_listener() {
        click_.release();
    }

    // 事件处理：如果子类处理了事件，返回 true；否则返回 false 继续向上传递
    virtual bool handle_gesture(const GestureEvent& event) {
        if (!enabled_ || visibility_ != Visibility::VISIBLE) {
            return false;
        }

        // 1. 自定义触控回调优先
        if (touch_.fn && touch_.fn(this, event, touch_.ctx)) {
            return true;
        }

        // 2. 双击
        if (event.type == GestureType::DOUBLE_TAP && contains(event.x, event.y)) {
            if (double_click_.fn) {
                double_click_.fn(this, double_click_.ctx);
                return true;
            }
        }

        // 3. 长按
        if (event.type == GestureType::LONG_PRESS && contains(event.x, event.y)) {
            if (long_click_.fn) {
                long_click_.fn(this, long_click_.ctx);
                return true;
            }
        }

        // 4. 单击
        if (event.type == GestureType::TAP && contains(event.x, event.y)) {
            if (click_.fn) {
                click_.fn(this, click_.ctx);
                return true;
            }
        }
        return false;
    }

    // 坐标命中测试：自顶向下检索命中本控件或子控件
    virtual View* find_view_at(int16_t x, int16_t y) {
        if (!enabled_ || visibility_ != Visibility::VISIBLE || !contains(x, y)) {
            return nullptr;
        }
        return this;
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
