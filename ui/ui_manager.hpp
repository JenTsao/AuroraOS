#ifndef AURORA_UI_MANAGER_HPP
#define AURORA_UI_MANAGER_HPP

#include "view_group.hpp"

namespace UI {

// ========================================================
// 全局手势预分发过滤器（可用于锁屏、全局快捷键、系统截屏拦截）
// ========================================================
class IGestureFilter {
public:
    virtual ~IGestureFilter() = default;
    // 返回 true 表示拦截并消费事件，不再向下分发给 UI 视图树
    virtual bool on_filter_gesture(const GestureEvent& event) = 0;
};

// ========================================================
// 全局手势分发结果监听器（可用于系统唤醒、触控音效/振动、性能统计）
// ========================================================
class IGestureListener {
public:
    virtual ~IGestureListener() = default;
    virtual void on_gesture_dispatched(const GestureEvent& event, bool handled) = 0;
};

class UiManager {
private:
    ViewGroup* root_view_;
    UIRenderer* renderer_;
    IGestureFilter* filter_;
    IGestureListener* listener_;
    View* captured_target_;

    UiManager()
        : root_view_(nullptr),
          renderer_(nullptr),
          filter_(nullptr),
          listener_(nullptr),
          captured_target_(nullptr) {}

public:
    static UiManager& instance() {
        static UiManager manager;
        return manager;
    }

    void set_renderer(UIRenderer* renderer) noexcept {
        renderer_ = renderer;
    }

    void set_root_view(ViewGroup* root) {
        root_view_ = root;
        captured_target_ = nullptr;
        if (root_view_) {
            root_view_->invalidate();
        }
    }

    ViewGroup* get_root_view() const noexcept {
        return root_view_;
    }

    void set_gesture_filter(IGestureFilter* filter) noexcept {
        filter_ = filter;
    }

    IGestureFilter* get_gesture_filter() const noexcept {
        return filter_;
    }

    void set_gesture_listener(IGestureListener* listener) noexcept {
        listener_ = listener;
    }

    IGestureListener* get_gesture_listener() const noexcept {
        return listener_;
    }

    // ========================================================
    // 触控焦点锁定与捕获机制 (Touch Target Tracking)
    // ========================================================
    void capture_touch(View* view) noexcept {
        captured_target_ = view;
    }

    void release_touch() noexcept {
        captured_target_ = nullptr;
    }

    View* get_captured_target() const noexcept {
        return captured_target_;
    }

    // 全局命中测试：检索位于屏幕 (x, y) 处最顶层可见的叶子视图
    View* find_view_at(int16_t x, int16_t y) const {
        if (!root_view_)
            return nullptr;
        return root_view_->find_view_at(x, y);
    }

    // ========================================================
    // 由 FrameScheduler 驱动的 UI 渲染主入口
    // ========================================================
    void render() {
        if (!root_view_ || !renderer_)
            return;

        // 如果根视图脏了，重新渲染整个树
        if (root_view_->is_dirty()) {
            root_view_->draw(*renderer_);
            root_view_->clear_dirty();
        }
    }

    // ========================================================
    // 手势事件分发中枢：贯穿 Filter -> Focus Target -> UI Tree -> Listener
    // ========================================================
    bool dispatch_gesture(const GestureEvent& event) {
        // 1. 全局拦截器优先判定 (如锁屏/系统防误触)
        if (filter_ && filter_->on_filter_gesture(event)) {
            if (listener_) {
                listener_->on_gesture_dispatched(event, true);
            }
            return true;
        }

        bool handled = false;

        // 2. 焦点目标捕获路由：若已有控件捕获了当前手势流（如拖拽中的滑块），优先直接投递
        if (captured_target_) {
            if (captured_target_->is_enabled() && captured_target_->get_visibility() == Visibility::VISIBLE) {
                handled = captured_target_->handle_gesture(event);
            } else {
                captured_target_ = nullptr;
            }

            // 拖拽释放或取消时自动释放捕获
            if (event.type == GestureType::DRAG_END ||
                event.type == GestureType::TOUCH_UP ||
                event.type == GestureType::CANCEL) {
                captured_target_ = nullptr;
            }
        }

        // 3. 未被捕获目标消费，则由根视图自顶向下深度优先路由
        if (!handled && root_view_) {
            handled = root_view_->handle_gesture(event);
        }

        // 4. 通知全局手势观察者
        if (listener_) {
            listener_->on_gesture_dispatched(event, handled);
        }

        return handled;
    }
};

} // namespace UI

#endif // AURORA_UI_MANAGER_HPP
