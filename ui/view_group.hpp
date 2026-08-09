#ifndef AURORA_UI_VIEW_GROUP_HPP
#define AURORA_UI_VIEW_GROUP_HPP

#include "view.hpp"
#include "../../kernel/memory.hpp" // for dynamic allocation of children array if needed

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
    ViewGroup(int16_t x, int16_t y, uint16_t w, uint16_t h) 
        : View(x, y, w, h), child_count_(0) {
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
        }
    }

    // ========================================================
    // 渲染分发：递归绘制所有脏子节点
    // ========================================================
    void draw(UIRenderer& renderer) override {
        // 如果本容器脏了，理论上可以选择擦除背景
        // ... (可选：填充背景色)

        for (int i = 0; i < child_count_; i++) {
            if (children_[i]->is_dirty()) {
                children_[i]->draw(renderer);
                children_[i]->clear_dirty();
            }
        }
    }

    // ========================================================
    // 事件分发：深度优先命中测试
    // ========================================================
    bool handle_gesture(const GestureEvent& event) override {
        // 从最顶层（数组最后添加的）开始往下传递事件
        for (int i = child_count_ - 1; i >= 0; i--) {
            View* child = children_[i];
            
            // 只有点击类事件需要坐标碰撞检测；滑动事件可以全量广播给能处理的组件
            if (event.type == GestureType::TAP || 
                event.type == GestureType::DOUBLE_TAP || 
                event.type == GestureType::LONG_PRESS) {
                
                if (child->contains(event.x, event.y)) {
                    if (child->handle_gesture(event)) {
                        return true; // 子节点消费了该事件
                    }
                }
            } else {
                // 滑动事件等全局广播
                if (child->handle_gesture(event)) {
                    return true;
                }
            }
        }
        return View::handle_gesture(event);
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
