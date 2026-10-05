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
    // 世界坐标累积脏区：仅当本容器就是根节点（parent_ == nullptr）时被写入。
    Rect damage_{};

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
    // 世界坐标 damage 累积（仅根节点）
    //
    // 中间容器一律纯转发；只有 parent_ == nullptr 的根容器才把冒泡上来的
    // 世界矩形并入自身 damage_。这样帧循环只需向根取一次即可拿到整帧脏区，
    // 无需递归复位每个容器的状态（栈里的 Screen 不在 children_ 里，递归也够不到）。
    // ========================================================
    void invalidate_rect_world(const Rect& r) override {
        if (parent_ == nullptr) {
            damage_ = Rect::enclose(damage_, r);
            return;
        }
        parent_->invalidate_rect_world(r);
    }

    // 取出并清空本容器累积的 damage（不递归）。
    Rect take_damage() noexcept {
        const Rect d = damage_;
        damage_ = {};
        return d;
    }

    // ========================================================
    // 子控件坐标 → 本容器父坐标 的映射钩子
    //
    // world_bounds() 沿父链逐级调用它，因此**任何**带内部坐标变换的容器都必须
    // 覆写，否则子控件冒泡上来的世界脏矩形会停在「未变换」的位置上：分带循环
    // 按错误的行号选带，真正改变的那一屏行永不被重绘（画面卡住）。
    // 目前唯一的覆写者是 ScrollView（减去滚动偏移）。ScreenNavigator 的转场
    // set_offset 无需在此登记：转场每帧都把导航器自身（=整屏矩形）标脏，
    // 帧管线据此退化为全屏重绘，动画结束后再落回精确脏区。
    // 入参 x/y 为子控件在本容器局部坐标系的位置，返回时已折算到父坐标系。
    // ========================================================
    virtual void map_child_coords(int32_t& x, int32_t& y) const noexcept {
        x += x_;
        y += y_;
    }

    // ========================================================
    // 渲染分发：递归绘制所有脏子节点 (跳过不可见节点)
    // ========================================================
    void draw(UIRenderer& renderer) override {
        if (visibility_ != Visibility::VISIBLE) {
            return;
        }

        // 幂等无副作用：无条件遍历可见子节点并重绘，绝不再做 per-child is_dirty 门控，
        // 也不再 clear_dirty()。脏区裁剪交给 scissor（damage ∩ band），推送交给
        // FrameBuffer::DirtyRect——「画什么」与「推什么」分离后，draw() 可安全重复调用。
        for (int i = 0; i < child_count_; i++) {
            if (children_[i] && children_[i]->get_visibility() == Visibility::VISIBLE) {
                children_[i]->draw(renderer);
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
// 延迟定义 View::invalidate / View::world_bounds (解决循环依赖)
// ========================================================
inline void View::invalidate() {
    is_dirty_ = true;
    // 只置自身脏标记；脏区以「世界坐标矩形」冒泡，由根容器统一累积。
    // 不再调 parent_->invalidate()：既避免与 invalidate_rect_world 的冒泡重复，
    // 也避免把整条父链都标脏（那样帧循环就失去精确脏区）。
    invalidate_rect_world(world_bounds());
}

inline void View::invalidate_rect_world(const Rect& r) {
    // 叶子默认：纯转发给父容器（无存储）。ViewGroup 会覆写以在根节点累积。
    if (parent_) {
        parent_->invalidate_rect_world(r);
    }
}

inline Rect View::world_bounds() const noexcept {
    int32_t wx = x_;
    int32_t wy = y_;
    // 沿父链逐级做「子坐标 → 父坐标」映射（父链由 ScreenNavigator/Screen 的
    // set_parent 正确挂接）。必须走 map_child_coords 而不是直接加 p->x_/p->y_：
    // 容器可能自带坐标变换（ScrollView 的滚动偏移），漏掉它脏区就会错位。
    // 需要完整 ViewGroup 定义，故与 invalidate() 同置于此处。
    for (const ViewGroup* p = parent_; p != nullptr; p = p->parent_) {
        p->map_child_coords(wx, wy);
    }
    return {static_cast<int16_t>(wx), static_cast<int16_t>(wy), width_, height_};
}

} // namespace UI

#endif // AURORA_UI_VIEW_GROUP_HPP
