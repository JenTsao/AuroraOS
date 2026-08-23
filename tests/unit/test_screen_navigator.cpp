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

    MockScreen(int i, bool* c, bool* s, bool* h, bool* d) : id(i), created(c), shown(s), hidden(h), destroyed(d) {}

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
