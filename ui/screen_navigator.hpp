#ifndef AURORA_UI_SCREEN_NAVIGATOR_HPP
#define AURORA_UI_SCREEN_NAVIGATOR_HPP

#include "screen.hpp"
#include "../../kernel/mm/memory.hpp"

namespace UI {

// ========================================================
// ScreenNavigator: 页面导航与转场引擎
//
// 扮演系统的 Root View，管理页面生命周期并驱动转场动画。
// 支持 Push (右向左滑入) 与 Pop (左向右滑出)。
// ========================================================
class ScreenNavigator : public ViewGroup {
public:
    static constexpr int kMaxStackSize = 8;
    static constexpr uint32_t kTransitionDurationMs = 250; // 动画时长 250ms

    enum class TransitionType {
        NONE,
        PUSH_LEFT, // 新页面从右侧滑入
        POP_RIGHT, // 当前页面向右侧滑出
        PUSH_UP,   // 新页面从底部向上滑入
        POP_DOWN   // 当前页面向下退回滑出
    };

    static ScreenNavigator& instance() {
        static ScreenNavigator nav;
        return nav;
    }

    int get_stack_size() const {
        return stack_size_;
    }

    TransitionType get_transition_state() const {
        return transition_state_;
    }

    // 缓动曲线：Cubic Ease-Out (256定点化计算)
    static uint32_t ease_out_cubic(uint32_t progress_256) {
        if (progress_256 >= 256) return 256;
        uint32_t inv = 256 - progress_256;
        uint32_t inv3 = (inv * inv * inv) / (256 * 256);
        return 256 - inv3;
    }

    // ========================================================
    // 导航 API
    // ========================================================

    // 压入新页面。若当前正在动画中则忽略。
    void push(Screen* screen, TransitionType trans = TransitionType::PUSH_LEFT) {
        if (!screen || stack_size_ >= kMaxStackSize)
            return;
        if (transition_state_ != TransitionType::NONE)
            return; // 防抖，动画中禁止操作

        Screen* current = active_screen();
        if (current) {
            current->on_hide();
        }

        stack_[stack_size_++] = screen;
        screen->on_create();

        if (current) {
            start_transition(trans);
        } else {
            // 第一屏，直接展示
            screen->on_show();
            invalidate();
        }
    }

    // 弹出当前页面。若当前正在动画或栈底则忽略。
    void pop(TransitionType trans = TransitionType::POP_RIGHT) {
        if (stack_size_ <= 1)
            return;
        if (transition_state_ != TransitionType::NONE)
            return;

        Screen* current = active_screen();
        if (current) {
            current->on_hide();
        }

        start_transition(trans);
    }

    // 替换栈顶页面（无动画）。
    void replace(Screen* screen) {
        if (!screen || transition_state_ != TransitionType::NONE)
            return;

        Screen* current = active_screen();
        if (current) {
            current->on_hide();
            current->on_destroy();
            delete current;
            stack_size_--;
        }

        stack_[stack_size_++] = screen;
        screen->on_create();
        screen->on_show();
        invalidate();
    }

    // ========================================================
    // 清空导航栈，释放所有页面（用于测试清理）
    // ========================================================
    void clear() {
        for (int i = 0; i < stack_size_; ++i) {
            if (stack_[i]) {
                stack_[i]->on_destroy();
                delete stack_[i];
                stack_[i] = nullptr;
            }
        }
        stack_size_ = 0;
        transition_state_ = TransitionType::NONE;
        transition_elapsed_ms_ = 0;
    }

    // ========================================================
    // 时钟驱动：更新动画状态
    // ========================================================
    void on_tick(uint32_t delta_ms) {
        if (transition_state_ == TransitionType::NONE)
            return;

        transition_elapsed_ms_ += delta_ms;
        if (transition_elapsed_ms_ >= kTransitionDurationMs) {
            finish_transition();
        } else {
            // 动画仍在进行，触发重绘
            invalidate();
        }
    }

    // ========================================================
    // 事件路由拦截
    // ========================================================
    bool handle_gesture(const GestureEvent& event) override {
        // 如果正在转场，丢弃所有用户输入
        if (transition_state_ != TransitionType::NONE)
            return true;

        // 全局手势拦截：右滑退出当前页面
        if (event.type == GestureType::SWIPE_RIGHT) {
            if (stack_size_ > 1) {
                pop(TransitionType::POP_RIGHT);
                return true;
            }
        }

        Screen* current = active_screen();
        if (current) {
            return current->handle_gesture(event);
        }
        return false;
    }

    // ========================================================
    // 渲染分发与转场控制
    // ========================================================
    void draw(UIRenderer& renderer) override {
        Screen* current = active_screen();
        if (!current)
            return;

        if (transition_state_ == TransitionType::NONE) {
            current->draw(renderer);
            return;
        }

        // 正在动画中，需要同时绘制 outgoing 和 incoming
        Screen* incoming = nullptr;
        Screen* outgoing = nullptr;

        if (transition_state_ == TransitionType::PUSH_LEFT || transition_state_ == TransitionType::PUSH_UP) {
            incoming = stack_[stack_size_ - 1];
            outgoing = stack_[stack_size_ - 2];
        } else if (transition_state_ == TransitionType::POP_RIGHT || transition_state_ == TransitionType::POP_DOWN) {
            outgoing = stack_[stack_size_ - 1];
            incoming = stack_[stack_size_ - 2];
        }

        // Cubic 缓动
        const uint32_t linear_progress = (transition_elapsed_ms_ * 256u) / kTransitionDurationMs;
        const uint32_t progress = ease_out_cubic(linear_progress);
        const int16_t slide_distance = static_cast<int16_t>((DISPLAY_WIDTH * progress) / 256u);
        const int16_t slide_vert = static_cast<int16_t>((DISPLAY_HEIGHT * progress) / 256u);

        if (transition_state_ == TransitionType::PUSH_LEFT) {
            // 老页面：往左移动出屏 (0 -> -WIDTH)
            renderer.set_offset(-slide_distance, 0);
            outgoing->draw(renderer);

            // 新页面：从右移入屏 (WIDTH -> 0)
            renderer.set_offset(DISPLAY_WIDTH - slide_distance, 0);
            incoming->draw(renderer);

        } else if (transition_state_ == TransitionType::POP_RIGHT) {
            // 老页面：往右移出屏 (0 -> WIDTH)
            renderer.set_offset(slide_distance, 0);
            outgoing->draw(renderer);

            // 新页面：从左侧移入 (-WIDTH/3 -> 0) 产生视差感
            const int16_t parallax_start = -DISPLAY_WIDTH / 3;
            const int16_t parallax_dist = (DISPLAY_WIDTH / 3 * progress) / 256u;
            renderer.set_offset(parallax_start + parallax_dist, 0);
            incoming->draw(renderer);

        } else if (transition_state_ == TransitionType::PUSH_UP) {
            renderer.set_offset(0, -slide_vert);
            outgoing->draw(renderer);
            renderer.set_offset(0, DISPLAY_HEIGHT - slide_vert);
            incoming->draw(renderer);

        } else if (transition_state_ == TransitionType::POP_DOWN) {
            renderer.set_offset(0, slide_vert);
            outgoing->draw(renderer);
            renderer.set_offset(0, -DISPLAY_HEIGHT / 3 + (DISPLAY_HEIGHT / 3 * progress) / 256u);
            incoming->draw(renderer);
        }

        // 恢复渲染器默认偏移
        renderer.set_offset(0, 0);
    }

    // 屏蔽 ViewGroup 默认机制，避免滥用
    // cppcheck-suppress duplInheritedMember
    void add_child(View*) = delete;

#ifdef AURORA_HOST_TEST
public:
#else
private:
#endif
    ScreenNavigator()
        : ViewGroup(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT), stack_size_(0), transition_state_(TransitionType::NONE),
          transition_elapsed_ms_(0) {
        for (int i = 0; i < kMaxStackSize; ++i)
            stack_[i] = nullptr;
    }

    // 析构时清理栈中所有残留页面
    ~ScreenNavigator() {
        for (int i = 0; i < stack_size_; ++i) {
            if (stack_[i]) {
                stack_[i]->on_destroy();
                delete stack_[i];
                stack_[i] = nullptr;
            }
        }
        stack_size_ = 0;
    }

    Screen* active_screen() const {
        if (stack_size_ == 0)
            return nullptr;
        return stack_[stack_size_ - 1];
    }

    void start_transition(TransitionType type) {
        transition_state_ = type;
        transition_elapsed_ms_ = 0;
        invalidate();
    }

    void finish_transition() {
        if (transition_state_ == TransitionType::POP_RIGHT) {
            // 真正的 pop 销毁发生在动画结束之后
            Screen* old_top = stack_[stack_size_ - 1];
            old_top->on_destroy();
            delete old_top;
            stack_[stack_size_ - 1] = nullptr;
            stack_size_--;

            Screen* new_top = active_screen();
            if (new_top)
                new_top->on_show();
        } else if (transition_state_ == TransitionType::PUSH_LEFT) {
            Screen* new_top = active_screen();
            if (new_top)
                new_top->on_show();
        }

        transition_state_ = TransitionType::NONE;
        invalidate();
    }

    Screen* stack_[kMaxStackSize];
    int stack_size_;

    TransitionType transition_state_;
    uint32_t transition_elapsed_ms_;
};

} // namespace UI

#endif // AURORA_UI_SCREEN_NAVIGATOR_HPP
