// test_screen_navigator.cpp — Screen Navigation Stack Unit Tests
#include <gtest/gtest.h>

#ifndef AURORA_HOST_TEST
#define AURORA_HOST_TEST
#endif

#include "../../ui/screen_navigator.hpp"

using namespace UI;

// ========================================================
// Mock Screen & Renderer
// ========================================================

class MockScreen : public Screen {
public:
    int id;
    bool* created;
    bool* shown;
    bool* hidden;
    bool* destroyed;
    bool allow_swipe_back = true;

    MockScreen(int i, bool* c, bool* s, bool* h, bool* d, bool allow_sb = true)
        : id(i), created(c), shown(s), hidden(h), destroyed(d), allow_swipe_back(allow_sb) {}

    void on_create() override {
        if (created)
            *created = true;
    }

    void on_show() override {
        if (shown) {
            *shown = true;
        }
        if (hidden) {
            *hidden = false;
        }
    }

    void on_hide() override {
        if (hidden) {
            *hidden = true;
        }
        if (shown) {
            *shown = false;
        }
    }

    void on_destroy() override {
        if (destroyed)
            *destroyed = true;
    }

    bool enable_swipe_back() const override {
        return allow_swipe_back;
    }
};

// ========================================================
// Tests
// ========================================================

class ScreenNavigatorTest : public ::testing::Test {
protected:
    void SetUp() override {
        ScreenNavigator::instance().clear();
    }

    void TearDown() override {
        ScreenNavigator::instance().clear();
    }
};

// Test Push Lifecycle
TEST_F(ScreenNavigatorTest, PushLifecycle) {
    ScreenNavigator nav;
    bool c1 = false, s1_ = false, h1 = false, d1 = false;
    MockScreen* s1 = new MockScreen(1, &c1, &s1_, &h1, &d1);

    // First push, no animation needed.
    nav.push(s1);
    EXPECT_TRUE(c1);
    EXPECT_TRUE(s1_);

    bool c2 = false, s2_ = false, h2 = false, d2 = false;
    MockScreen* s2 = new MockScreen(2, &c2, &s2_, &h2, &d2);
    // Second push, triggers animation
    nav.push(s2);
    EXPECT_TRUE(c2);
    EXPECT_FALSE(s2_); // Not shown until animation ends
    EXPECT_TRUE(h1);   // S1 is immediately hidden

    // Tick to finish animation
    nav.on_tick(300);
    EXPECT_TRUE(s2_);

    // Explicit clear cleans up all screens
    nav.clear();
    EXPECT_TRUE(d1);
    EXPECT_TRUE(d2);
}

// Test Pop Lifecycle
TEST_F(ScreenNavigatorTest, PopLifecycle) {
    ScreenNavigator nav;
    bool c1 = false, s1_ = false, h1 = false, d1 = false;
    MockScreen* s1 = new MockScreen(1, &c1, &s1_, &h1, &d1);
    nav.push(s1);

    bool c3 = false, s3_ = false, h3 = false, d3 = false;
    MockScreen* s3 = new MockScreen(3, &c3, &s3_, &h3, &d3);
    nav.push(s3);
    nav.on_tick(300); // finish push

    EXPECT_FALSE(d3);

    nav.pop();
    // Pop triggers animation, destruction doesn't happen yet
    EXPECT_TRUE(h3);
    EXPECT_FALSE(d3);

    // Tick to finish pop animation
    nav.on_tick(300);
    EXPECT_TRUE(d3);

    // Explicit clear cleans up remaining stack
    nav.clear();
    EXPECT_TRUE(d1);
}

TEST_F(ScreenNavigatorTest, GestureRouting) {
    ScreenNavigator nav;
    bool c1 = false, s1_ = false, h1 = false, d1 = false;
    MockScreen* s1 = new MockScreen(1, &c1, &s1_, &h1, &d1);
    nav.push(s1);

    bool c4 = false, s4_ = false, h4 = false, d4 = false;
    MockScreen* s4 = new MockScreen(4, &c4, &s4_, &h4, &d4);
    nav.push(s4);
    nav.on_tick(300);

    // Right swipe should trigger POP automatically
    GestureEvent evt = {GestureType::SWIPE_RIGHT, 0, 0};
    nav.handle_gesture(evt);

    // If it popped, animation started
    nav.on_tick(300);
    EXPECT_TRUE(d4);

    // Explicit clear cleans up remaining stack
    nav.clear();
    EXPECT_TRUE(d1);
}

TEST_F(ScreenNavigatorTest, ClearLifecycle) {
    ScreenNavigator nav;
    bool c1 = false, s1_ = false, h1 = false, d1 = false;
    bool c2 = false, s2_ = false, h2 = false, d2 = false;
    MockScreen* s1 = new MockScreen(1, &c1, &s1_, &h1, &d1);
    MockScreen* s2 = new MockScreen(2, &c2, &s2_, &h2, &d2);

    nav.push(s1);
    nav.push(s2);
    nav.on_tick(300);

    EXPECT_EQ(nav.active_screen(), s2);
    EXPECT_FALSE(d1);
    EXPECT_FALSE(d2);

    nav.clear();
    EXPECT_EQ(nav.active_screen(), nullptr);
    EXPECT_TRUE(d1);
    EXPECT_TRUE(d2);
}

// ========================================================
// 垂直转场生命周期回归测试
// （修复前：POP_DOWN 泄漏页面、PUSH_UP 永不 on_show）
// ========================================================

// PUSH_UP：动画结束后新页面必须收到 on_show，且转场状态归位
TEST_F(ScreenNavigatorTest, PushUpLifecycle) {
    ScreenNavigator nav;
    bool c1 = false, s1_ = false, h1 = false, d1 = false;
    bool c2 = false, s2_ = false, h2 = false, d2 = false;
    MockScreen* s1 = new MockScreen(1, &c1, &s1_, &h1, &d1);
    MockScreen* s2 = new MockScreen(2, &c2, &s2_, &h2, &d2);

    nav.push(s1);

    nav.push(s2, ScreenNavigator::TransitionType::PUSH_UP);
    EXPECT_FALSE(s2_); // 动画期间未展示

    nav.on_tick(300); // 完成动画
    EXPECT_TRUE(s2_);  // 回归：on_show 必须触发
    EXPECT_EQ(nav.active_screen(), s2);
    EXPECT_EQ(nav.get_stack_size(), 2);
    EXPECT_EQ(nav.get_transition_state(), ScreenNavigator::TransitionType::NONE);

    nav.clear();
}

// POP_DOWN：动画结束后旧页面必须销毁、栈深回退、新栈顶 on_show
TEST_F(ScreenNavigatorTest, PopDownLifecycle) {
    ScreenNavigator nav;
    bool c1 = false, s1_ = false, h1 = false, d1 = false;
    bool c3 = false, s3_ = false, h3 = false, d3 = false;
    MockScreen* s1 = new MockScreen(1, &c1, &s1_, &h1, &d1);
    MockScreen* s3 = new MockScreen(3, &c3, &s3_, &h3, &d3);

    nav.push(s1);
    nav.push(s3, ScreenNavigator::TransitionType::PUSH_UP);
    nav.on_tick(300);

    nav.pop(ScreenNavigator::TransitionType::POP_DOWN);
    EXPECT_TRUE(h3);
    EXPECT_FALSE(d3); // 动画期间未销毁

    nav.on_tick(300);
    // 回归：此前 POP_DOWN 结束后 d3 永远为 false（泄漏）
    EXPECT_TRUE(d3);
    EXPECT_EQ(nav.active_screen(), s1);
    EXPECT_EQ(nav.get_stack_size(), 1);
    EXPECT_TRUE(s1_); // 底层页面重新可见

    nav.clear();
    EXPECT_TRUE(d1);
}

// 下滑手势应触发纵向 pop（与右滑返回对称）
TEST_F(ScreenNavigatorTest, SwipeDownPopsVertically) {
    ScreenNavigator nav;
    bool c1 = false, s1_ = false, h1 = false, d1 = false;
    bool c4 = false, s4_ = false, h4 = false, d4 = false;
    MockScreen* s1 = new MockScreen(1, &c1, &s1_, &h1, &d1);
    MockScreen* s4 = new MockScreen(4, &c4, &s4_, &h4, &d4);

    nav.push(s1);
    nav.push(s4);
    nav.on_tick(300);

    GestureEvent down = {GestureType::SWIPE_DOWN, 0, 0};
    EXPECT_TRUE(nav.handle_gesture(down)); // 被导航拦截并启动 pop

    EXPECT_EQ(nav.get_transition_state(), ScreenNavigator::TransitionType::POP_DOWN);

    nav.on_tick(300);
    EXPECT_TRUE(d4);
    EXPECT_EQ(nav.active_screen(), s1);
    EXPECT_EQ(nav.get_stack_size(), 1);

    nav.clear();
}

// ========================================================
// 扩展测试：Instant / 无动画 Push 与 Pop
// ========================================================
TEST_F(ScreenNavigatorTest, InstantPushAndPop) {
    ScreenNavigator nav;
    bool c1 = false, s1_ = false, h1 = false, d1 = false;
    bool c2 = false, s2_ = false, h2 = false, d2 = false;
    MockScreen* s1 = new MockScreen(1, &c1, &s1_, &h1, &d1);
    MockScreen* s2 = new MockScreen(2, &c2, &s2_, &h2, &d2);

    nav.push(s1);
    EXPECT_TRUE(s1_);

    // 立即压栈（NONE）
    EXPECT_TRUE(nav.push(s2, ScreenNavigator::TransitionType::NONE));
    EXPECT_TRUE(h1);
    EXPECT_TRUE(c2);
    EXPECT_TRUE(s2_);
    EXPECT_EQ(nav.get_transition_state(), ScreenNavigator::TransitionType::NONE);
    EXPECT_EQ(nav.get_stack_size(), 2);

    // 立即出栈（NONE）
    EXPECT_TRUE(nav.pop(ScreenNavigator::TransitionType::NONE));
    EXPECT_TRUE(h2);
    EXPECT_TRUE(d2);
    EXPECT_TRUE(s1_);
    EXPECT_EQ(nav.get_stack_size(), 1);
    EXPECT_EQ(nav.active_screen(), s1);

    nav.clear();
    EXPECT_TRUE(d1);
}

// ========================================================
// 扩展测试：Replace 页面替换生命周期
// ========================================================
TEST_F(ScreenNavigatorTest, ReplaceLifecycle) {
    ScreenNavigator nav;
    bool c1 = false, s1_ = false, h1 = false, d1 = false;
    bool c2 = false, s2_ = false, h2 = false, d2 = false;
    MockScreen* s1 = new MockScreen(1, &c1, &s1_, &h1, &d1);
    MockScreen* s2 = new MockScreen(2, &c2, &s2_, &h2, &d2);

    nav.push(s1);
    EXPECT_TRUE(s1_);
    EXPECT_EQ(nav.get_stack_size(), 1);

    EXPECT_TRUE(nav.replace(s2));
    EXPECT_TRUE(h1);
    EXPECT_TRUE(d1);
    EXPECT_TRUE(c2);
    EXPECT_TRUE(s2_);
    EXPECT_EQ(nav.get_stack_size(), 1);
    EXPECT_EQ(nav.active_screen(), s2);

    nav.clear();
    EXPECT_TRUE(d2);
}

// ========================================================
// 扩展测试：PopToRoot 页面回退（带动画与立即回退）
// ========================================================
TEST_F(ScreenNavigatorTest, PopToRootAnimated) {
    ScreenNavigator nav;
    bool c1 = false, s1_ = false, h1 = false, d1 = false;
    bool c2 = false, s2_ = false, h2 = false, d2 = false;
    bool c3 = false, s3_ = false, h3 = false, d3 = false;
    bool c4 = false, s4_ = false, h4 = false, d4 = false;
    MockScreen* s1 = new MockScreen(1, &c1, &s1_, &h1, &d1);
    MockScreen* s2 = new MockScreen(2, &c2, &s2_, &h2, &d2);
    MockScreen* s3 = new MockScreen(3, &c3, &s3_, &h3, &d3);
    MockScreen* s4 = new MockScreen(4, &c4, &s4_, &h4, &d4);

    nav.push(s1);
    nav.push(s2);
    nav.on_tick(300);
    nav.push(s3);
    nav.on_tick(300);
    nav.push(s4);
    nav.on_tick(300);
    EXPECT_EQ(nav.get_stack_size(), 4);

    // 执行回退至根页面
    EXPECT_TRUE(nav.pop_to_root(ScreenNavigator::TransitionType::POP_RIGHT));
    // 中间页面 s2 与 s3 应当立即被销毁
    EXPECT_TRUE(d2);
    EXPECT_TRUE(d3);
    // 顶层页面 s4 正在执行转场，未销毁
    EXPECT_FALSE(d4);
    EXPECT_TRUE(h4);
    EXPECT_EQ(nav.get_transition_state(), ScreenNavigator::TransitionType::POP_RIGHT);

    // 完成动画
    nav.on_tick(300);
    EXPECT_TRUE(d4);
    EXPECT_TRUE(s1_);
    EXPECT_EQ(nav.get_stack_size(), 1);
    EXPECT_EQ(nav.active_screen(), s1);

    nav.clear();
    EXPECT_TRUE(d1);
}

TEST_F(ScreenNavigatorTest, PopToRootInstant) {
    ScreenNavigator nav;
    bool c1 = false, s1_ = false, h1 = false, d1 = false;
    bool c2 = false, s2_ = false, h2 = false, d2 = false;
    bool c3 = false, s3_ = false, h3 = false, d3 = false;
    MockScreen* s1 = new MockScreen(1, &c1, &s1_, &h1, &d1);
    MockScreen* s2 = new MockScreen(2, &c2, &s2_, &h2, &d2);
    MockScreen* s3 = new MockScreen(3, &c3, &s3_, &h3, &d3);

    nav.push(s1);
    nav.push(s2);
    nav.on_tick(300);
    nav.push(s3);
    nav.on_tick(300);

    EXPECT_TRUE(nav.pop_to_root(ScreenNavigator::TransitionType::NONE));
    EXPECT_TRUE(d2);
    EXPECT_TRUE(d3);
    EXPECT_TRUE(s1_);
    EXPECT_EQ(nav.get_stack_size(), 1);
    EXPECT_EQ(nav.active_screen(), s1);

    nav.clear();
    EXPECT_TRUE(d1);
}

// ========================================================
// 扩展测试：PopTo 指定页面
// ========================================================
TEST_F(ScreenNavigatorTest, PopToSpecificScreen) {
    ScreenNavigator nav;
    bool c1 = false, s1_ = false, h1 = false, d1 = false;
    bool c2 = false, s2_ = false, h2 = false, d2 = false;
    bool c3 = false, s3_ = false, h3 = false, d3 = false;
    bool c4 = false, s4_ = false, h4 = false, d4 = false;
    MockScreen* s1 = new MockScreen(1, &c1, &s1_, &h1, &d1);
    MockScreen* s2 = new MockScreen(2, &c2, &s2_, &h2, &d2);
    MockScreen* s3 = new MockScreen(3, &c3, &s3_, &h3, &d3);
    MockScreen* s4 = new MockScreen(4, &c4, &s4_, &h4, &d4);

    nav.push(s1);
    nav.push(s2);
    nav.on_tick(300);
    nav.push(s3);
    nav.on_tick(300);
    nav.push(s4);
    nav.on_tick(300);

    // 回退到 s2
    EXPECT_TRUE(nav.pop_to(s2, ScreenNavigator::TransitionType::POP_RIGHT));
    EXPECT_TRUE(d3);  // 夹在中间的 s3 立即销毁
    EXPECT_FALSE(d4); // s4 动画中
    EXPECT_FALSE(d2); // s2 保留

    nav.on_tick(300);
    EXPECT_TRUE(d4);
    EXPECT_TRUE(s2_);
    EXPECT_EQ(nav.get_stack_size(), 2);
    EXPECT_EQ(nav.active_screen(), s2);

    nav.clear();
    EXPECT_TRUE(d1);
    EXPECT_TRUE(d2);
}

// ========================================================
// 扩展测试：反向方向转场动画 (PUSH_RIGHT / POP_LEFT / PUSH_DOWN / POP_UP)
// ========================================================
TEST_F(ScreenNavigatorTest, ReverseTransitions) {
    ScreenNavigator nav;
    bool c1 = false, s1_ = false, h1 = false, d1 = false;
    bool c2 = false, s2_ = false, h2 = false, d2 = false;
    bool c3 = false, s3_ = false, h3 = false, d3 = false;
    MockScreen* s1 = new MockScreen(1, &c1, &s1_, &h1, &d1);
    MockScreen* s2 = new MockScreen(2, &c2, &s2_, &h2, &d2);
    MockScreen* s3 = new MockScreen(3, &c3, &s3_, &h3, &d3);

    nav.push(s1);

    // 1. PUSH_RIGHT
    EXPECT_TRUE(nav.push(s2, ScreenNavigator::TransitionType::PUSH_RIGHT));
    EXPECT_EQ(nav.get_transition_state(), ScreenNavigator::TransitionType::PUSH_RIGHT);
    nav.on_tick(300);
    EXPECT_TRUE(s2_);

    // 2. POP_LEFT
    EXPECT_TRUE(nav.pop(ScreenNavigator::TransitionType::POP_LEFT));
    EXPECT_EQ(nav.get_transition_state(), ScreenNavigator::TransitionType::POP_LEFT);
    nav.on_tick(300);
    EXPECT_TRUE(d2);
    EXPECT_TRUE(s1_);

    // 3. PUSH_DOWN
    EXPECT_TRUE(nav.push(s3, ScreenNavigator::TransitionType::PUSH_DOWN));
    EXPECT_EQ(nav.get_transition_state(), ScreenNavigator::TransitionType::PUSH_DOWN);
    nav.on_tick(300);
    EXPECT_TRUE(s3_);

    // 4. POP_UP
    EXPECT_TRUE(nav.pop(ScreenNavigator::TransitionType::POP_UP));
    EXPECT_EQ(nav.get_transition_state(), ScreenNavigator::TransitionType::POP_UP);
    nav.on_tick(300);
    EXPECT_TRUE(d3);
    EXPECT_TRUE(s1_);

    nav.clear();
    EXPECT_TRUE(d1);
}

// ========================================================
// 扩展测试：栈状态查询辅助方法
// ========================================================
TEST_F(ScreenNavigatorTest, StackQueries) {
    ScreenNavigator nav;
    bool c1 = false, s1_ = false, h1 = false, d1 = false;
    bool c2 = false, s2_ = false, h2 = false, d2 = false;
    MockScreen* s1 = new MockScreen(1, &c1, &s1_, &h1, &d1);
    MockScreen* s2 = new MockScreen(2, &c2, &s2_, &h2, &d2);

    EXPECT_FALSE(nav.can_pop());
    EXPECT_TRUE(nav.can_push());
    EXPECT_EQ(nav.get_root_screen(), nullptr);

    nav.push(s1);
    EXPECT_EQ(nav.get_root_screen(), s1);
    EXPECT_TRUE(nav.contains(s1));
    EXPECT_FALSE(nav.contains(s2));
    EXPECT_EQ(nav.index_of(s1), 0);
    EXPECT_EQ(nav.index_of(s2), -1);
    EXPECT_FALSE(nav.can_pop());

    nav.push(s2);
    nav.on_tick(300);
    EXPECT_TRUE(nav.can_pop());
    EXPECT_EQ(nav.get_screen(0), s1);
    EXPECT_EQ(nav.get_screen(1), s2);
    EXPECT_EQ(nav.get_screen(2), nullptr);

    nav.clear();
}

// ========================================================
// 扩展测试：定点缓动曲线验证
// ========================================================
TEST_F(ScreenNavigatorTest, EasingCurvesCalculations) {
    // 1. ease_in_cubic
    EXPECT_EQ(ScreenNavigator::ease_in_cubic(0), 0u);
    EXPECT_EQ(ScreenNavigator::ease_in_cubic(256), 256u);
    EXPECT_LT(ScreenNavigator::ease_in_cubic(128), 128u); // 加速型在前半程落后于线性

    // 2. ease_in_out_cubic
    EXPECT_EQ(ScreenNavigator::ease_in_out_cubic(0), 0u);
    EXPECT_EQ(ScreenNavigator::ease_in_out_cubic(128), 128u); // 中点严格对称
    EXPECT_EQ(ScreenNavigator::ease_in_out_cubic(256), 256u);

    // 3. ease_linear
    EXPECT_EQ(ScreenNavigator::ease_linear(0), 0u);
    EXPECT_EQ(ScreenNavigator::ease_linear(100), 100u);
    EXPECT_EQ(ScreenNavigator::ease_linear(256), 256u);

    // 4. calculate_ease 与配置
    ScreenNavigator nav;
    nav.set_easing_curve(ScreenNavigator::EasingCurve::LINEAR);
    EXPECT_EQ(nav.calculate_ease(100), 100u);
    nav.set_easing_curve(ScreenNavigator::EasingCurve::CUBIC_IN);
    EXPECT_EQ(nav.calculate_ease(256), 256u);
}

// ========================================================
// 扩展测试：导航事件监听器 (NavigationListener)
// ========================================================
class MockNavigationListener : public ScreenNavigator::NavigationListener {
public:
    int started_count = 0;
    int finished_count = 0;
    ScreenNavigator::TransitionType last_started_type = ScreenNavigator::TransitionType::NONE;
    ScreenNavigator::TransitionType last_finished_type = ScreenNavigator::TransitionType::NONE;
    Screen* last_outgoing = nullptr;
    Screen* last_incoming = nullptr;
    Screen* last_active = nullptr;

    void on_transition_started(ScreenNavigator::TransitionType type, Screen* outgoing, Screen* incoming) override {
        started_count++;
        last_started_type = type;
        last_outgoing = outgoing;
        last_incoming = incoming;
    }

    void on_transition_finished(ScreenNavigator::TransitionType type, Screen* active) override {
        finished_count++;
        last_finished_type = type;
        last_active = active;
    }
};

TEST_F(ScreenNavigatorTest, NavigationListenerNotifications) {
    ScreenNavigator nav;
    MockNavigationListener listener;
    nav.set_navigation_listener(&listener);

    bool c1 = false, s1_ = false, h1 = false, d1 = false;
    bool c2 = false, s2_ = false, h2 = false, d2 = false;
    MockScreen* s1 = new MockScreen(1, &c1, &s1_, &h1, &d1);
    MockScreen* s2 = new MockScreen(2, &c2, &s2_, &h2, &d2);

    // 首屏直接就绪
    nav.push(s1);
    EXPECT_EQ(listener.finished_count, 1);
    EXPECT_EQ(listener.last_active, s1);

    // 第二屏转场开始
    nav.push(s2, ScreenNavigator::TransitionType::PUSH_LEFT);
    EXPECT_EQ(listener.started_count, 1);
    EXPECT_EQ(listener.last_started_type, ScreenNavigator::TransitionType::PUSH_LEFT);
    EXPECT_EQ(listener.last_outgoing, s1);
    EXPECT_EQ(listener.last_incoming, s2);

    // 转场结束
    nav.on_tick(300);
    EXPECT_EQ(listener.finished_count, 2);
    EXPECT_EQ(listener.last_finished_type, ScreenNavigator::TransitionType::PUSH_LEFT);
    EXPECT_EQ(listener.last_active, s2);

    nav.clear();
}

// ========================================================
// 扩展测试：页面级与全局手势返回控制
// ========================================================
TEST_F(ScreenNavigatorTest, SwipeBackControls) {
    ScreenNavigator nav;
    bool c1 = false, s1_ = false, h1 = false, d1 = false;
    bool c2 = false, s2_ = false, h2 = false, d2 = false;
    // s2 页面显式禁用手势返回 (例如关键表单或游戏页面)
    MockScreen* s1 = new MockScreen(1, &c1, &s1_, &h1, &d1, true);
    MockScreen* s2 = new MockScreen(2, &c2, &s2_, &h2, &d2, false);

    nav.push(s1);
    nav.push(s2);
    nav.on_tick(300);

    GestureEvent right = {GestureType::SWIPE_RIGHT, 0, 0};
    // 由于 s2 禁用了手势返回，导航器不拦截，转由 s2 处理，页面不触发 pop
    EXPECT_FALSE(nav.handle_gesture(right));
    EXPECT_EQ(nav.get_transition_state(), ScreenNavigator::TransitionType::NONE);
    EXPECT_EQ(nav.get_stack_size(), 2);

    // 允许全局开关测试
    nav.clear();
    bool c3 = false, s3_ = false, h3 = false, d3 = false;
    bool c4 = false, s4_ = false, h4 = false, d4 = false;
    MockScreen* s3 = new MockScreen(3, &c3, &s3_, &h3, &d3, true);
    MockScreen* s4 = new MockScreen(4, &c4, &s4_, &h4, &d4, true);

    nav.push(s3);
    nav.push(s4);
    nav.on_tick(300);

    // 关闭全局手势返回
    nav.set_swipe_back_enabled(false);
    EXPECT_FALSE(nav.is_swipe_back_enabled());
    EXPECT_FALSE(nav.handle_gesture(right));
    EXPECT_EQ(nav.get_transition_state(), ScreenNavigator::TransitionType::NONE);

    // 重新开启全局手势返回
    nav.set_swipe_back_enabled(true);
    EXPECT_TRUE(nav.handle_gesture(right));
    EXPECT_EQ(nav.get_transition_state(), ScreenNavigator::TransitionType::POP_RIGHT);

    nav.on_tick(300);
    EXPECT_TRUE(d4);
    EXPECT_EQ(nav.get_stack_size(), 1);

    nav.clear();
}

// ========================================================
// 扩展测试：可配置动画时长
// ========================================================
TEST_F(ScreenNavigatorTest, ConfigurableTransitionDuration) {
    ScreenNavigator nav;
    nav.set_transition_duration(100); // 设置为 100ms
    EXPECT_EQ(nav.get_transition_duration(), 100u);

    bool c1 = false, s1_ = false, h1 = false, d1 = false;
    bool c2 = false, s2_ = false, h2 = false, d2 = false;
    MockScreen* s1 = new MockScreen(1, &c1, &s1_, &h1, &d1);
    MockScreen* s2 = new MockScreen(2, &c2, &s2_, &h2, &d2);

    nav.push(s1);
    nav.push(s2);

    // 推进 50ms (未完成)
    nav.on_tick(50);
    EXPECT_NE(nav.get_transition_state(), ScreenNavigator::TransitionType::NONE);

    // 再推进 60ms (累计 110ms >= 100ms，完成转场)
    nav.on_tick(60);
    EXPECT_EQ(nav.get_transition_state(), ScreenNavigator::TransitionType::NONE);
    EXPECT_TRUE(s2_);

    nav.clear();
}

