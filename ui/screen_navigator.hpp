#ifndef AURORA_UI_SCREEN_NAVIGATOR_HPP
#define AURORA_UI_SCREEN_NAVIGATOR_HPP

#include "screen.hpp"
#include "../../kernel/mm/memory.hpp"

namespace UI {

// ========================================================
// ScreenNavigator: 页面导航与转场引擎
//
// 扮演系统的 Root View，管理页面生命周期并驱动转场动画。
// 支持完整的页面栈操作 (Push/Pop/Replace/PopToRoot/PopTo)
// 以及 8 向平滑定点化转场动画、视差滚动、事件防抖与导航监听。
// ========================================================
class ScreenNavigator : public ViewGroup {
public:
    static constexpr int kMaxStackSize = 8;
    static constexpr uint32_t kDefaultTransitionDurationMs = 250; // 默认动画时长 250ms

    // 转场动画类型
    enum class TransitionType {
        NONE,        // 无动画（立即完成）
        PUSH_LEFT,   // 新页面从右侧滑入 (横向前进)
        POP_RIGHT,   // 当前页面向右侧滑出 (横向后退)
        PUSH_RIGHT,  // 新页面从左侧滑入 (横向反向前进)
        POP_LEFT,    // 当前页面向左侧滑出 (横向反向后退)
        PUSH_UP,     // 新页面从底部向上滑入 (纵向前进，多用于模态/工具流)
        POP_DOWN,    // 当前页面向下退回滑出 (纵向后退，关闭模态)
        PUSH_DOWN,   // 新页面从顶部向下滑入 (纵向反向前进)
        POP_UP       // 当前页面向上退回滑出 (纵向反向后退)
    };

    // 缓动曲线类型 (256 定点数算法，零浮点开销)
    enum class EasingCurve {
        CUBIC_OUT,    // 减速退出 (标准转场体验)
        CUBIC_IN,     // 加速进入
        CUBIC_IN_OUT, // S型加速后减速
        LINEAR        // 匀速
    };

    // 导航生命周期与转场事件监听器 (非拥有引用)
    class NavigationListener {
    public:
        virtual ~NavigationListener() = default;
        virtual void on_transition_started(TransitionType type, Screen* outgoing, Screen* incoming) {
            (void)type; (void)outgoing; (void)incoming;
        }
        virtual void on_transition_finished(TransitionType type, Screen* active) {
            (void)type; (void)active;
        }
    };

    static ScreenNavigator& instance() {
        static ScreenNavigator nav;
        return nav;
    }

    int get_stack_size() const noexcept {
        return stack_size_;
    }

    TransitionType get_transition_state() const noexcept {
        return transition_state_;
    }

    bool can_push() const noexcept {
        return (stack_size_ < kMaxStackSize && transition_state_ == TransitionType::NONE);
    }

    bool can_pop() const noexcept {
        return (stack_size_ > 1 && transition_state_ == TransitionType::NONE);
    }

    Screen* active_screen() const noexcept {
        if (stack_size_ == 0)
            return nullptr;
        return stack_[stack_size_ - 1];
    }

    Screen* get_root_screen() const noexcept {
        return get_screen(0);
    }

    Screen* get_screen(int index) const noexcept {
        if (index >= 0 && index < stack_size_)
            return stack_[index];
        return nullptr;
    }

    bool contains(Screen* screen) const noexcept {
        return index_of(screen) != -1;
    }

    int index_of(Screen* screen) const noexcept {
        for (int i = 0; i < stack_size_; ++i) {
            if (stack_[i] == screen)
                return i;
        }
        return -1;
    }

    // ========================================================
    // 定点缓动曲线计算 (输入 0~256，输出 0~256)
    // ========================================================

    // 缓动曲线：Cubic Ease-Out (256定点化计算)
    static uint32_t ease_out_cubic(uint32_t progress_256) noexcept {
        if (progress_256 >= 256) return 256;
        const uint32_t inv = 256 - progress_256;
        const uint32_t inv3 = (inv * inv * inv) / (256 * 256);
        return 256 - inv3;
    }

    static uint32_t ease_in_cubic(uint32_t progress_256) noexcept {
        if (progress_256 >= 256) return 256;
        return (progress_256 * progress_256 * progress_256) / (256 * 256);
    }

    static uint32_t ease_in_out_cubic(uint32_t progress_256) noexcept {
        if (progress_256 >= 256) return 256;
        if (progress_256 < 128) {
            return (4 * progress_256 * progress_256 * progress_256) / (256 * 256);
        } else {
            const uint32_t inv = 256 - progress_256;
            const uint32_t inv3 = (4 * inv * inv * inv) / (256 * 256);
            return 256 - inv3;
        }
    }

    static uint32_t ease_linear(uint32_t progress_256) noexcept {
        return (progress_256 >= 256) ? 256 : progress_256;
    }

    uint32_t calculate_ease(uint32_t progress_256) const noexcept {
        switch (easing_curve_) {
            case EasingCurve::CUBIC_OUT:
                return ease_out_cubic(progress_256);
            case EasingCurve::CUBIC_IN:
                return ease_in_cubic(progress_256);
            case EasingCurve::CUBIC_IN_OUT:
                return ease_in_out_cubic(progress_256);
            case EasingCurve::LINEAR:
                return ease_linear(progress_256);
        }
        // 兜底返回仅满足控制流分析；枚举全覆盖时不可达。
        // 不放在 switch 的 default 分支，以免抑制新增枚举值时的 -Wswitch 警告。
        return ease_out_cubic(progress_256);
    }

    // ========================================================
    // 导航栈 API
    // ========================================================

    // 压入新页面（使用默认转场，见 resolve_push_transition）。
    bool push(Screen* screen) {
        return push(screen, resolve_push_transition(TransitionType::PUSH_LEFT));
    }

    // 压入新页面。若当前正在动画或栈满则返回 false 忽略。
    bool push(Screen* screen, TransitionType trans) {
        if (!screen || stack_size_ >= kMaxStackSize)
            return false;
        if (transition_state_ != TransitionType::NONE)
            return false; // 交互防抖，动画中禁止重入

        Screen* current = active_screen();

        if (!current || trans == TransitionType::NONE) {
            // 首屏或立即跳转模式：无过渡动画
            if (current) {
                current->on_hide();
            }
            // 挂入导航器父链：View::invalidate() 依赖 parent_ 逐级冒泡，
            // 缺此一步则控件级无效化在 Screen 处断链，根视图永不置脏，
            // UiManager::render() 会持续空转（首帧之后画面不再更新）。
            screen->set_parent(this);
            stack_[stack_size_++] = screen;
            screen->on_create();
            screen->on_show();
            transition_state_ = TransitionType::NONE;
            transition_elapsed_ms_ = 0;
            if (listener_) {
                listener_->on_transition_finished(TransitionType::NONE, screen);
            }
            invalidate();
            return true;
        }

        current->on_hide();
        // 同上：先挂父链再入栈，否则滑动转场路径同样断链。
        screen->set_parent(this);
        stack_[stack_size_++] = screen;
        screen->on_create();
        start_transition(trans);
        return true;
    }

    // 弹出当前页面（使用默认转场，见 resolve_pop_transition）。
    bool pop() {
        return pop(resolve_pop_transition(TransitionType::POP_RIGHT));
    }

    // 弹出当前页面。若当前正在动画或仅剩 1 屏则返回 false 忽略。
    bool pop(TransitionType trans) {
        if (stack_size_ <= 1)
            return false;
        if (transition_state_ != TransitionType::NONE)
            return false;

        Screen* current = active_screen();

        if (trans == TransitionType::NONE) {
            // 立即弹出模式：同步销毁并呈现下层页面
            if (current) {
                current->on_hide();
                current->on_destroy();
                // 先断父链再析构：避免 Screen 带着指向导航器的 parent_ 被销毁，
                // 使其子控件析构期的 invalidate() 回写导航器状态。
                current->set_parent(nullptr);
                delete current;
                stack_[--stack_size_] = nullptr;
            }
            Screen* next = active_screen();
            if (next) {
                next->on_show();
            }
            transition_state_ = TransitionType::NONE;
            transition_elapsed_ms_ = 0;
            if (listener_) {
                listener_->on_transition_finished(TransitionType::NONE, next);
            }
            invalidate();
            return true;
        }

        if (current) {
            current->on_hide();
        }

        start_transition(trans);
        return true;
    }

    // 弹出回退至栈底根页面 (使用默认转场)
    bool pop_to_root() {
        return pop_to_root(resolve_pop_transition(TransitionType::POP_RIGHT));
    }

    // 弹出回退至栈底根页面 (如按手环电源键/表冠/长按返回表盘)
    bool pop_to_root(TransitionType trans) {
        if (stack_size_ <= 1 || transition_state_ != TransitionType::NONE)
            return false;

        if (trans == TransitionType::NONE) {
            // 立即回退至根节点
            for (int i = stack_size_ - 1; i >= 1; --i) {
                if (stack_[i]) {
                    stack_[i]->on_hide();
                    stack_[i]->on_destroy();
                    stack_[i]->set_parent(nullptr); // 先断父链再析构
                    delete stack_[i];
                    stack_[i] = nullptr;
                }
            }
            stack_size_ = 1;
            Screen* root = stack_[0];
            if (root) {
                root->on_show();
            }
            transition_state_ = TransitionType::NONE;
            transition_elapsed_ms_ = 0;
            if (listener_) {
                listener_->on_transition_finished(TransitionType::NONE, root);
            }
            invalidate();
            return true;
        }

        // 带转场回退：中间页面立即静默释放，仅保留栈底和当前顶层页面执行平滑回退
        Screen* top = stack_[stack_size_ - 1];
        for (int i = 1; i < stack_size_ - 1; ++i) {
            if (stack_[i]) {
                stack_[i]->on_destroy();
                stack_[i]->set_parent(nullptr); // 先断父链再析构
                delete stack_[i];
                stack_[i] = nullptr;
            }
        }
        // top 是同一对象，仍留在栈中参与回退转场：保持其 parent_ 不变
        stack_[1] = top;
        stack_size_ = 2;

        top->on_hide();
        start_transition(trans);
        return true;
    }

    // 弹出回退至指定的目标页面（使用默认转场）
    bool pop_to(Screen* target) {
        return pop_to(target, resolve_pop_transition(TransitionType::POP_RIGHT));
    }

    // 弹出回退至指定的目标页面
    bool pop_to(Screen* target, TransitionType trans) {
        if (!target || stack_size_ <= 1 || transition_state_ != TransitionType::NONE)
            return false;

        const int target_idx = index_of(target);
        if (target_idx < 0 || target_idx >= stack_size_ - 1)
            return false; // 目标不在栈中，或者本身就是栈顶

        if (trans == TransitionType::NONE) {
            for (int i = stack_size_ - 1; i > target_idx; --i) {
                if (stack_[i]) {
                    stack_[i]->on_hide();
                    stack_[i]->on_destroy();
                    stack_[i]->set_parent(nullptr); // 先断父链再析构
                    delete stack_[i];
                    stack_[i] = nullptr;
                }
            }
            stack_size_ = target_idx + 1;
            target->on_show();
            transition_state_ = TransitionType::NONE;
            transition_elapsed_ms_ = 0;
            if (listener_) {
                listener_->on_transition_finished(TransitionType::NONE, target);
            }
            invalidate();
            return true;
        }

        Screen* top = stack_[stack_size_ - 1];
        for (int i = target_idx + 1; i < stack_size_ - 1; ++i) {
            if (stack_[i]) {
                stack_[i]->on_destroy();
                stack_[i]->set_parent(nullptr); // 先断父链再析构
                delete stack_[i];
                stack_[i] = nullptr;
            }
        }
        // top 与 target 均保留在栈中：保持 parent_ 不变
        stack_[target_idx + 1] = top;
        stack_size_ = target_idx + 2;

        top->on_hide();
        start_transition(trans);
        return true;
    }

    // 弹出回退至指定索引的页面（使用默认转场）
    bool pop_to_index(int index) {
        return pop_to_index(index, resolve_pop_transition(TransitionType::POP_RIGHT));
    }

    // 弹出回退至指定索引的页面
    bool pop_to_index(int index, TransitionType trans) {
        if (index < 0 || index >= stack_size_ - 1)
            return false;
        return pop_to(stack_[index], trans);
    }

    // 替换栈顶页面（同步无动画）
    bool replace(Screen* screen) {
        if (!screen || transition_state_ != TransitionType::NONE)
            return false;

        Screen* current = active_screen();
        if (current) {
            current->on_hide();
            current->on_destroy();
            current->set_parent(nullptr); // 先断父链再析构
            delete current;
            stack_size_--;
        }

        screen->set_parent(this); // 新页面挂入父链
        stack_[stack_size_++] = screen;
        screen->on_create();
        screen->on_show();
        if (listener_) {
            listener_->on_transition_finished(TransitionType::NONE, screen);
        }
        invalidate();
        return true;
    }

    // ========================================================
    // 清空导航栈，释放所有页面（用于测试清理或全局重置）
    // ========================================================
    void clear() {
        for (int i = 0; i < stack_size_; ++i) {
            if (stack_[i]) {
                stack_[i]->on_destroy();
                stack_[i]->set_parent(nullptr); // 先断父链再析构（含 ~ScreenNavigator 路径）
                delete stack_[i];
                stack_[i] = nullptr;
            }
        }
        stack_size_ = 0;
        transition_state_ = TransitionType::NONE;
        transition_elapsed_ms_ = 0;
    }

    // ========================================================
    // 动画与手势控制配置
    // ========================================================
    void set_transition_duration(uint32_t duration_ms) noexcept {
        transition_duration_ms_ = (duration_ms == 0) ? 1 : duration_ms;
    }

    uint32_t get_transition_duration() const noexcept {
        return transition_duration_ms_;
    }

    void set_easing_curve(EasingCurve curve) noexcept {
        easing_curve_ = curve;
    }

    EasingCurve get_easing_curve() const noexcept {
        return easing_curve_;
    }

    void set_navigation_listener(NavigationListener* listener) noexcept {
        listener_ = listener;
    }

    NavigationListener* get_navigation_listener() const noexcept {
        return listener_;
    }

    void set_swipe_back_enabled(bool enabled) noexcept {
        swipe_back_enabled_ = enabled;
    }

    bool is_swipe_back_enabled() const noexcept {
        return swipe_back_enabled_;
    }

    // ========================================================
    // 默认转场配置
    //
    // 库默认值保持 PUSH_LEFT / POP_RIGHT / POP_DOWN，既有调用点与测试不受影响。
    // 把本项配置为 NONE 时，push / pop / pop_to* / 滑动返回 全部退化为「瞬切」，
    // 用于 SPI 带宽受限、滑动转场必然整屏脏的屏（如 12MHz SPI 下全屏 188KB
    // 约 125ms/帧，实际上限仅 ~8fps）。
    //
    // 注意「族别」约束：draw() 依据 PUSH_* / POP_* 判定哪一页是 incoming，
    // finish_transition() 依据族别判定动画结束后是否销毁栈顶并回退栈深。
    // 因此配置值只在**同族**时生效：
    //   - push 家族（push / replace）只接受 PUSH_*，异族值回退 PUSH_LEFT
    //   - pop  家族（pop / pop_to* / 滑动返回）只接受 POP_*，异族值回退各自固有值
    // 这样可以杜绝「把 PUSH_* 传给 pop 导致栈顶永不销毁」这类静默错误。
    // ========================================================
    void set_default_transition(TransitionType t) noexcept {
        default_transition_ = t;
    }

    TransitionType get_default_transition() const noexcept {
        return default_transition_;
    }

    // ========================================================
    // 时钟驱动：更新动画状态
    // ========================================================
    void on_tick(uint32_t delta_ms) {
        if (transition_state_ == TransitionType::NONE)
            return;

        transition_elapsed_ms_ += delta_ms;
        if (transition_elapsed_ms_ >= transition_duration_ms_) {
            finish_transition();
        } else {
            // 动画仍在进行，标记脏并触发重绘
            invalidate();
        }
    }

    // ========================================================
    // 事件路由拦截与手势处理
    // ========================================================
    bool handle_gesture(const GestureEvent& event) override {
        // 如果正在转场中，丢弃所有用户输入防抖
        if (transition_state_ != TransitionType::NONE)
            return true;

        // 全局手势拦截：右滑退出当前页面（水平返回），下滑退出当前页面
        // 两者均仅在栈深 > 1、且页面未显式禁用手势返回时生效。
        if (swipe_back_enabled_ && stack_size_ > 1) {
            Screen* current = active_screen();
            const bool allow_back = (!current || current->enable_swipe_back());

            if (allow_back) {
                if (event.type == GestureType::SWIPE_RIGHT) {
                    // 横向返回：库固有语义为 POP_RIGHT；default_transition_ 配成 NONE 时瞬切
                    pop(resolve_pop_transition(TransitionType::POP_RIGHT));
                    return true;
                }
                if (event.type == GestureType::SWIPE_DOWN) {
                    // 纵向返回：库固有语义为 POP_DOWN；default_transition_ 配成 NONE 时瞬切
                    pop(resolve_pop_transition(TransitionType::POP_DOWN));
                    return true;
                }
            }
        }

        Screen* current = active_screen();
        if (current) {
            return current->handle_gesture(event);
        }
        return false;
    }

    // ========================================================
    // 命中测试：必须转发到当前页
    //
    // 页面只存于 stack_、从不进 ViewGroup::children_（add_child 已被 delete），
    // 基类的递归够不到任何一页 —— 不覆写则 find_view_at 恒返回导航器自身，
    // 而 WatchApp::init 恰恰把导航器设为 UiManager 的根。
    // 语义与 ViewGroup 一致：命中最顶层可见叶控件；落点只在页内空白处则返回该页。
    // 转场期间输入已被 handle_gesture 防抖丢弃，此处按栈顶页解析即可。
    // ========================================================
    View* find_view_at(int16_t x, int16_t y) override {
        // View::contains 必须限定调用：本类的 contains(Screen*) 遮蔽了基类的
        // contains(int16_t, int16_t)（名字遮蔽，非重载解析）。
        if (!enabled_ || visibility_ != Visibility::VISIBLE || !View::contains(x, y)) {
            return nullptr;
        }
        Screen* current = active_screen();
        if (current && current->is_enabled() && current->get_visibility() == Visibility::VISIBLE) {
            View* hit = current->find_view_at(x, y);
            if (hit) {
                return hit;
            }
        }
        return this;
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

        // 防御检查：至少需 2 个页面才能渲染双屏转场
        if (stack_size_ < 2) {
            current->draw(renderer);
            return;
        }

        Screen* incoming = nullptr;
        Screen* outgoing = nullptr;

        const bool is_push = (transition_state_ == TransitionType::PUSH_LEFT ||
                              transition_state_ == TransitionType::PUSH_RIGHT ||
                              transition_state_ == TransitionType::PUSH_UP ||
                              transition_state_ == TransitionType::PUSH_DOWN);

        if (is_push) {
            incoming = stack_[stack_size_ - 1];
            outgoing = stack_[stack_size_ - 2];
        } else {
            outgoing = stack_[stack_size_ - 1];
            incoming = stack_[stack_size_ - 2];
        }

        if (!incoming || !outgoing) {
            current->draw(renderer);
            return;
        }

        // 定点缓动计算
        const uint32_t linear_progress = (transition_elapsed_ms_ * 256u) / transition_duration_ms_;
        const uint32_t progress = calculate_ease(linear_progress);
        const int16_t slide_distance = static_cast<int16_t>((DISPLAY_WIDTH * progress) / 256u);
        const int16_t slide_vert = static_cast<int16_t>((DISPLAY_HEIGHT * progress) / 256u);

        switch (transition_state_) {
            case TransitionType::PUSH_LEFT: {
                // 老页面：向左推出屏 (0 -> -WIDTH)
                renderer.set_offset(-slide_distance, 0);
                outgoing->draw(renderer);
                // 新页面：从右侧推入屏 (WIDTH -> 0)
                renderer.set_offset(DISPLAY_WIDTH - slide_distance, 0);
                incoming->draw(renderer);
                break;
            }
            case TransitionType::POP_RIGHT: {
                // 老页面：向右推出屏 (0 -> WIDTH)
                renderer.set_offset(slide_distance, 0);
                outgoing->draw(renderer);
                // 新页面：从左侧视差推入 (-WIDTH/3 -> 0)
                const int16_t parallax_start = -DISPLAY_WIDTH / 3;
                const int16_t parallax_dist = static_cast<int16_t>((DISPLAY_WIDTH / 3 * progress) / 256u);
                renderer.set_offset(parallax_start + parallax_dist, 0);
                incoming->draw(renderer);
                break;
            }
            case TransitionType::PUSH_RIGHT: {
                // 反向水平推进：老页面向右退出，新页面从左侧进入
                renderer.set_offset(slide_distance, 0);
                outgoing->draw(renderer);
                renderer.set_offset(-DISPLAY_WIDTH + slide_distance, 0);
                incoming->draw(renderer);
                break;
            }
            case TransitionType::POP_LEFT: {
                // 反向水平弹出：老页面向左退出，新页面从右侧视差进入
                renderer.set_offset(-slide_distance, 0);
                outgoing->draw(renderer);
                const int16_t parallax_start = DISPLAY_WIDTH / 3;
                const int16_t parallax_dist = static_cast<int16_t>((DISPLAY_WIDTH / 3 * progress) / 256u);
                renderer.set_offset(parallax_start - parallax_dist, 0);
                incoming->draw(renderer);
                break;
            }
            case TransitionType::PUSH_UP: {
                // 纵向向上推进：老页面向上移出，新页面从底部移入
                renderer.set_offset(0, -slide_vert);
                outgoing->draw(renderer);
                renderer.set_offset(0, DISPLAY_HEIGHT - slide_vert);
                incoming->draw(renderer);
                break;
            }
            case TransitionType::POP_DOWN: {
                // 纵向向下弹出：老页面向下移出，新页面从顶部视差移入
                renderer.set_offset(0, slide_vert);
                outgoing->draw(renderer);
                const int16_t parallax_start = -DISPLAY_HEIGHT / 3;
                const int16_t parallax_dist = static_cast<int16_t>((DISPLAY_HEIGHT / 3 * progress) / 256u);
                renderer.set_offset(0, parallax_start + parallax_dist);
                incoming->draw(renderer);
                break;
            }
            case TransitionType::PUSH_DOWN: {
                // 纵向向下推进：老页面向下移出，新页面从顶部移入
                renderer.set_offset(0, slide_vert);
                outgoing->draw(renderer);
                renderer.set_offset(0, -DISPLAY_HEIGHT + slide_vert);
                incoming->draw(renderer);
                break;
            }
            case TransitionType::POP_UP: {
                // 纵向向上弹出：老页面向上移出，新页面从底部视差移入
                renderer.set_offset(0, -slide_vert);
                outgoing->draw(renderer);
                const int16_t parallax_start = DISPLAY_HEIGHT / 3;
                const int16_t parallax_dist = static_cast<int16_t>((DISPLAY_HEIGHT / 3 * progress) / 256u);
                renderer.set_offset(0, parallax_start - parallax_dist);
                incoming->draw(renderer);
                break;
            }
            default: {
                current->draw(renderer);
                break;
            }
        }

        // 恢复渲染器默认视口偏移
        renderer.set_offset(0, 0);
    }

    // 屏蔽 ViewGroup 默认无序子视图挂载，统一由导航栈管理
    // cppcheck-suppress duplInheritedMember
    void add_child(View*) = delete;

#ifdef AURORA_HOST_TEST
public:
#else
private:
#endif
    // ---- 默认转场的族别安全解析 ----
    static constexpr bool is_push_family(TransitionType t) noexcept {
        return t == TransitionType::PUSH_LEFT || t == TransitionType::PUSH_RIGHT || t == TransitionType::PUSH_UP ||
               t == TransitionType::PUSH_DOWN;
    }

    static constexpr bool is_pop_family(TransitionType t) noexcept {
        return t == TransitionType::POP_RIGHT || t == TransitionType::POP_LEFT || t == TransitionType::POP_DOWN ||
               t == TransitionType::POP_UP;
    }

    // push 家族操作：NONE→瞬切；同族配置→采用；异族配置→回退固有默认
    TransitionType resolve_push_transition(TransitionType natural) const noexcept {
        if (default_transition_ == TransitionType::NONE)
            return TransitionType::NONE;
        if (is_push_family(default_transition_))
            return default_transition_;
        return natural;
    }

    // pop 家族操作：NONE→瞬切；同族配置→采用；异族配置→回退固有默认
    TransitionType resolve_pop_transition(TransitionType natural) const noexcept {
        if (default_transition_ == TransitionType::NONE)
            return TransitionType::NONE;
        if (is_pop_family(default_transition_))
            return default_transition_;
        return natural;
    }

    ScreenNavigator()
        : ViewGroup(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT), stack_size_(0), transition_state_(TransitionType::NONE),
          transition_elapsed_ms_(0), transition_duration_ms_(kDefaultTransitionDurationMs),
          easing_curve_(EasingCurve::CUBIC_OUT), listener_(nullptr), swipe_back_enabled_(true),
          default_transition_(TransitionType::PUSH_LEFT) {
        for (int i = 0; i < kMaxStackSize; ++i)
            stack_[i] = nullptr;
    }

    // 析构时清理栈中所有残留页面
    ~ScreenNavigator() {
        clear();
    }

    void start_transition(TransitionType type) {
        transition_state_ = type;
        transition_elapsed_ms_ = 0;

        if (listener_ && stack_size_ >= 2) {
            const bool is_push = (type == TransitionType::PUSH_LEFT ||
                                  type == TransitionType::PUSH_RIGHT ||
                                  type == TransitionType::PUSH_UP ||
                                  type == TransitionType::PUSH_DOWN);
            Screen* outgoing = is_push ? stack_[stack_size_ - 2] : stack_[stack_size_ - 1];
            Screen* incoming = is_push ? stack_[stack_size_ - 1] : stack_[stack_size_ - 2];
            listener_->on_transition_started(type, outgoing, incoming);
        }

        invalidate();
    }

    void finish_transition() {
        const bool popping = (transition_state_ == TransitionType::POP_RIGHT ||
                              transition_state_ == TransitionType::POP_DOWN ||
                              transition_state_ == TransitionType::POP_LEFT ||
                              transition_state_ == TransitionType::POP_UP);
        const bool pushing = (transition_state_ == TransitionType::PUSH_LEFT ||
                              transition_state_ == TransitionType::PUSH_UP ||
                              transition_state_ == TransitionType::PUSH_RIGHT ||
                              transition_state_ == TransitionType::PUSH_DOWN);
        const TransitionType finished_type = transition_state_;

        if (popping && stack_size_ >= 2) {
            // 真正的 pop 销毁发生在动画结束之后
            Screen* old_top = stack_[stack_size_ - 1];
            if (old_top) {
                old_top->on_destroy();
                old_top->set_parent(nullptr); // 先断父链再析构
                delete old_top;
                stack_[stack_size_ - 1] = nullptr;
            }
            stack_size_--;
        }

        Screen* new_top = active_screen();
        if ((popping || pushing) && new_top) {
            new_top->on_show();
        }

        transition_state_ = TransitionType::NONE;
        transition_elapsed_ms_ = 0;

        if (listener_) {
            listener_->on_transition_finished(finished_type, new_top);
        }

        invalidate();
    }

    Screen* stack_[kMaxStackSize];
    int stack_size_;

    TransitionType transition_state_;
    uint32_t transition_elapsed_ms_;
    uint32_t transition_duration_ms_;
    EasingCurve easing_curve_;
    NavigationListener* listener_;
    bool swipe_back_enabled_;
    // 默认转场（库默认 PUSH_LEFT，保持向后兼容）。配成 NONE 时全链路瞬切。
    TransitionType default_transition_;
};

} // namespace UI

#endif // AURORA_UI_SCREEN_NAVIGATOR_HPP
