#ifndef AURORA_UI_MANAGER_HPP
#define AURORA_UI_MANAGER_HPP

#include "view_group.hpp"

namespace UI {

class UiManager {
private:
    ViewGroup* root_view_;
    UIRenderer* renderer_;

    UiManager() : root_view_(nullptr), renderer_(nullptr) {}

public:
    static UiManager& instance() {
        static UiManager manager;
        return manager;
    }

    void set_renderer(UIRenderer* renderer) {
        renderer_ = renderer;
    }

    void set_root_view(ViewGroup* root) {
        root_view_ = root;
        if (root_view_) {
            root_view_->invalidate();
        }
    }
    
    ViewGroup* get_root_view() const {
        return root_view_;
    }

    // ========================================================
    // 由 FrameScheduler 驱动的 UI 渲染主入口
    // ========================================================
    void render() {
        if (!root_view_ || !renderer_) return;

        // 如果根视图脏了，重新渲染整个树
        if (root_view_->is_dirty()) {
            // 根视图清理全屏 (或者由 RootView 自己决定)
            // renderer_->fill_rect(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, {0, 0, 0});
            
            root_view_->draw(*renderer_);
            root_view_->clear_dirty();
        }
    }

    // ========================================================
    // 手势事件分发中枢
    // ========================================================
    void dispatch_gesture(const GestureEvent& event) {
        if (!root_view_) return;
        
        // 发送给树根，进行深度优先路由
        root_view_->handle_gesture(event);
    }
};

} // namespace UI

#endif // AURORA_UI_MANAGER_HPP
