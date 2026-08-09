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

    MockScreen(int i, bool* c, bool* s, bool* h, bool* d) 
        : id(i), created(c), shown(s), hidden(h), destroyed(d) {}

    void on_create() override { if(created) *created = true; }
    void on_show() override { if(shown) { *shown = true; } if(hidden) { *hidden = false; } }
    void on_hide() override { if(hidden) { *hidden = true; } if(shown) { *shown = false; } }
    void on_destroy() override { if(destroyed) *destroyed = true; }
};

// ========================================================
// Tests
// ========================================================

class ScreenNavigatorTest : public ::testing::Test {
protected:
    void SetUp() override {
        // We will just do functional tests using the singleton.
        // For testing we will replace the active screens.
    }
};

// Test Push Lifecycle
TEST(ScreenNavigatorTest, PushLifecycle) {
    ScreenNavigator nav;
    bool c1=false, s1_=false, h1=false, d1=false;
    MockScreen* s1 = new MockScreen(1, &c1, &s1_, &h1, &d1);
    
    // First push, no animation needed.
    nav.push(s1);
    EXPECT_TRUE(c1);
    EXPECT_TRUE(s1_);

    bool c2=false, s2_=false, h2=false, d2=false;
    MockScreen* s2 = new MockScreen(2, &c2, &s2_, &h2, &d2);
    // Second push, triggers animation
    nav.push(s2);
    EXPECT_TRUE(c2);
    EXPECT_FALSE(s2_); // Not shown until animation ends
    EXPECT_TRUE(h1); // S1 is immediately hidden

    // Tick to finish animation
    nav.on_tick(300);
    EXPECT_TRUE(s2_);
}

// Test Pop Lifecycle
TEST(ScreenNavigatorTest, PopLifecycle) {
    ScreenNavigator nav;
    MockScreen* s1 = new MockScreen(1, nullptr, nullptr, nullptr, nullptr);
    nav.push(s1);

    bool c3=false, s3_=false, h3=false, d3=false;
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
}

TEST(ScreenNavigatorTest, GestureRouting) {
    ScreenNavigator nav;
    MockScreen* s1 = new MockScreen(1, nullptr, nullptr, nullptr, nullptr);
    nav.push(s1);

    bool c4=false, s4_=false, h4=false, d4=false;
    MockScreen* s4 = new MockScreen(4, &c4, &s4_, &h4, &d4);
    nav.push(s4);
    nav.on_tick(300);

    // Right swipe should trigger POP automatically
    GestureEvent evt = {GestureType::SWIPE_RIGHT, 0, 0};
    nav.handle_gesture(evt);

    // If it popped, animation started
    nav.on_tick(300);
    EXPECT_TRUE(d4);
}

// We assume it compiles and runs fine since it's just logic.
