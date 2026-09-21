#ifndef AURORA_UI_VIEW_GROUP_HPP
#define AURORA_UI_VIEW_GROUP_HPP

#include "view.hpp"
#include "../../kernel/mm/memory.hpp" // for dynamic allocation of children array if needed

namespace UI {

// ========================================================
// ViewGroup: 包含子视图的容器
// ========================================================
class ViewGroup : public View {
private:
    static constexpr int MAX_CHILDREN = 16;
    View* children_[MAX_CHILDREN];
    int child_count_;

public:
    ViewGroup(int16_t x, int16_t y, uint16_t w, uint16_t h) : View(x, y, w, h), child_count_(0) {
        for (int i = 0; i < MAX_CHILDREN; i++) {
            children_[i] = nullptr;
        }
    }

    ~ViewGroup() override {
        // 使用动态内存模型：析构容器时自动释放所有子组件
        for (int i = 0; i < child_count_; i++) {
            delete children_[i];
        }
    }

    void add_child(View* child) {
        if (child_count_ < MAX_CHILDREN && child != nullptr) {
            children_[child_count_++] = child;
            child->set_parent(this);
            invalidate();
        }
    }

    bool remove_child(View* child) {
        if (!child) return false;
        int found_idx = -1;
        for (int i = 0; i < child_count_; ++i) {
            if (children_[i] == child) {
                found_idx = i;
                break;
            }
        }
        if (found_idx >= 0) {
            child->set_parent(nullptr);
            for (int i = found_idx; i < child_count_ - 1; ++i) {
                children_[i] = children_[i + 1];
            }
            children_[--child_count_] = nullptr;
            invalidate();
            return true;
        }
        return false;
    }

    int get_child_count() const {
        return child_count_;
    }

    View* get_child(int index) const {
        if (index >= 0 && index < child_count_) {
            return children_[index];
        }
        return nullptr;
    }

    // ========================================================
    // 渲染分发：递归绘制所有脏子节点 (跳过不可见节点)
    // ========================================================
    void draw(UIRenderer& renderer) override {
        if (visibility_ != Visibility::VISIBLE) {
            return;
        }

        for (int i = 0; i < child_count_; i++) {
            if (children_[i] && children_[i]->get_visibility() == Visibility::VISIBLE) {
                if (children_[i]->is_dirty()) {
                    children_[i]->draw(renderer);
                    children_[i]->clear_dirty();
                }
            }
        }
    }

    // ========================================================
    // 容器级手势拦截钩子（类似于 Android onInterceptTouchEvent）
    // 允许父容器（如滚动容器、导航器）在子控件接收前优先拦截手势
    // ========================================================
    virtual bool on_intercept_gesture(const GestureEvent& event) {
        (void)event;
        return false;
    }

    // ========================================================
    // 事件分发：深度优先与命中测试 (跳过隐藏与禁用节点)
    // ========================================================
    bool handle_gesture(const GestureEvent& event) override {
        if (!enabled_ || visibility_ != Visibility::VISIBLE) {
            return false;
        }

        // 1. 优先检查容器自身拦截钩子
        if (on_intercept_gesture(event)) {
            return true;
        }

        // 2. 从最顶层 (Z-Order 最高，数组后添加的) 往下分发
        for (int i = child_count_ - 1; i >= 0; i--) {
            View* child = children_[i];
            if (!child || !child->is_enabled() || child->get_visibility() != Visibility::VISIBLE) {
                continue;
            }

            // 针对具有明确触控点的事件进行几何碰撞过滤
            const bool is_point_event = (event.type == GestureType::TAP ||
                                         event.type == GestureType::DOUBLE_TAP ||
                                         event.type == GestureType::TRIPLE_TAP ||
                                         event.type == GestureType::LONG_PRESS ||
                                         event.type == GestureType::TOUCH_DOWN ||
                                         event.type == GestureType::TOUCH_UP ||
                                         event.type == GestureType::DRAG_START ||
                                         event.type == GestureType::DRAG_MOVE);

            if (is_point_event) {
                if (child->contains(event.x, event.y)) {
                    if (child->handle_gesture(event)) {
                        return true; // 目标子节点消费了事件
                    }
                }
            } else {
                // 滑动等全局手势：优先投递给包含触控点的子节点，未包含则向后广播
                if (child->contains(event.x, event.y)) {
                    if (child->handle_gesture(event)) {
                        return true;
                    }
                } else if (child->handle_gesture(event)) {
                    return true;
                }
            }
        }

        // 3. 子节点均未消费，回退到自身 View 处理
        return View::handle_gesture(event);
    }

    // ========================================================
    // 递归命中测试：检索位于 (x, y) 处最顶层的叶子控件
    // ========================================================
    View* find_view_at(int16_t x, int16_t y) override {
        if (!enabled_ || visibility_ != Visibility::VISIBLE || !contains(x, y)) {
            return nullptr;
        }

        for (int i = child_count_ - 1; i >= 0; --i) {
            View* child = children_[i];
            if (child && child->is_enabled() && child->get_visibility() == Visibility::VISIBLE) {
                View* hit = child->find_view_at(x, y);
                if (hit) {
                    return hit;
                }
            }
        }

        return this;
    }
};

// ========================================================
// 延迟定义 View::invalidate (解决循环依赖)
// ========================================================
inline void View::invalidate() {
    is_dirty_ = true;
    // 向上冒泡，通知父节点自己内部脏了（父节点自己不需要重绘，但需要遍历它的子节点）
    // 为了简化，目前每次 draw 都会遍历，所以只要标记自己 dirty 即可
    if (parent_) {
        // 实际上可以优化为通知 root_view 有脏矩形
        parent_->invalidate();
    }
}

} // namespace UI

#endif // AURORA_UI_VIEW_GROUP_HPP
