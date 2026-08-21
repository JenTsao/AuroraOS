#include <gtest/gtest.h>
#include "../../ui/ui_manager.hpp"
#include "../../ui/widgets/button.hpp"

using namespace UI;

// 测试用的模拟 UIRenderer，避免真正进行硬件渲染
// Renderer2D 是一个头文件实现，依赖 FrameBuffer
// 在测试中我们可以使用一个较小的 FrameBuffer
class UiEngineTest : public ::testing::Test {
protected:
    void SetUp() override {
        UiManager::instance().set_root_view(nullptr);
    }

    void TearDown() override {
        ViewGroup* root = UiManager::instance().get_root_view();
        UiManager::instance().set_root_view(nullptr);
        if (root) {
            delete root;
        }
    }
};

static bool button_clicked = false;

static void on_test_button_click(void* ctx) {
    button_clicked = true;
}

TEST_F(UiEngineTest, GestureRoutingTest) {
    ViewGroup* root = new ViewGroup(0, 0, 192, 490);
    Button* btn = new Button(50, 50, 100, 50, 0, 0);
    btn->set_on_click(on_test_button_click, nullptr);
    root->add_child(btn);

    UiManager::instance().set_root_view(root);

    button_clicked = false;

    // 模拟坐标外点击 (不应该触发)
    GestureEvent evt_miss = {GestureType::TAP, 10, 10};
    UiManager::instance().dispatch_gesture(evt_miss);
    EXPECT_FALSE(button_clicked);

    // 模拟坐标内点击 (应该触发)
    GestureEvent evt_hit = {GestureType::TAP, 60, 60};
    UiManager::instance().dispatch_gesture(evt_hit);
    EXPECT_TRUE(button_clicked);

    // 模拟双击 (不触发，因为按钮只拦截 TAP)
    button_clicked = false;
    GestureEvent evt_double = {GestureType::DOUBLE_TAP, 60, 60};
    UiManager::instance().dispatch_gesture(evt_double);
    EXPECT_FALSE(button_clicked);

    // 先解绑再释放，防止悬空指针（ViewGroup 析构会自动递归释放包含的 btn）
    UiManager::instance().set_root_view(nullptr);
    delete root;
}

#include "../../ui/widgets/slider_view.hpp"
#include "../../ui/widgets/progress_bar.hpp"
#include "../../ui/screen_navigator.hpp"

TEST_F(UiEngineTest, ViewVisibilityAndEnabledState) {
    ViewGroup* root = new ViewGroup(0, 0, 192, 490);
    Button* btn = new Button(50, 50, 100, 50, 0, 0);
    btn->set_on_click(on_test_button_click, nullptr);
    root->add_child(btn);

    UiManager::instance().set_root_view(root);

    // 1. Visible & Enabled -> should click
    button_clicked = false;
    GestureEvent evt_hit = {GestureType::TAP, 60, 60};
    UiManager::instance().dispatch_gesture(evt_hit);
    EXPECT_TRUE(button_clicked);

    // 2. Disabled -> should not click
    btn->set_enabled(false);
    EXPECT_FALSE(btn->is_enabled());
    button_clicked = false;
    UiManager::instance().dispatch_gesture(evt_hit);
    EXPECT_FALSE(button_clicked);

    // 3. Re-enabled but GONE -> should not click
    btn->set_enabled(true);
    btn->set_visibility(Visibility::GONE);
    EXPECT_FALSE(btn->is_visible());
    button_clicked = false;
    UiManager::instance().dispatch_gesture(evt_hit);
    EXPECT_FALSE(button_clicked);

    // 4. INVISIBLE -> should not click
    btn->set_visibility(Visibility::INVISIBLE);
    button_clicked = false;
    UiManager::instance().dispatch_gesture(evt_hit);
    EXPECT_FALSE(button_clicked);

    UiManager::instance().set_root_view(nullptr);
    delete root;
}

TEST_F(UiEngineTest, ViewGroupChildManagement) {
    ViewGroup* root = new ViewGroup(0, 0, 192, 490);
    Button* b1 = new Button(0, 0, 50, 50, 0, 0);
    Button* b2 = new Button(0, 60, 50, 50, 0, 0);

    EXPECT_EQ(root->get_child_count(), 0);
    root->add_child(b1);
    root->add_child(b2);
    EXPECT_EQ(root->get_child_count(), 2);
    EXPECT_EQ(root->get_child(0), b1);
    EXPECT_EQ(root->get_child(1), b2);
    EXPECT_EQ(b1->get_parent(), root);

    // Remove b1
    EXPECT_TRUE(root->remove_child(b1));
    EXPECT_EQ(root->get_child_count(), 1);
    EXPECT_EQ(root->get_child(0), b2);
    EXPECT_EQ(b1->get_parent(), nullptr);

    delete b1; // Manually delete removed child
    UiManager::instance().set_root_view(nullptr);
    delete root;
}

static int32_t g_slider_val = 0;
static void on_slider_changed(SliderView*, int32_t val, void*) {
    g_slider_val = val;
}

TEST_F(UiEngineTest, SliderViewInteraction) {
    SliderView slider(10, 10, 100, 30, 0, 100, 20);
    slider.set_on_value_changed_listener(on_slider_changed, nullptr);

    EXPECT_EQ(slider.get_value(), 20);
    slider.set_value(75);
    EXPECT_EQ(slider.get_value(), 75);
    EXPECT_EQ(g_slider_val, 75);

    // Clamping checks
    slider.set_value(150);
    EXPECT_EQ(slider.get_value(), 100);
    slider.set_value(-50);
    EXPECT_EQ(slider.get_value(), 0);

    // Touch tap at middle (x=60 -> rel_x=50 -> 50% value)
    GestureEvent evt_tap = {GestureType::TAP, 60, 25};
    EXPECT_TRUE(slider.handle_gesture(evt_tap));
    EXPECT_EQ(slider.get_value(), 50);
}

TEST_F(UiEngineTest, ProgressBarPercentage) {
    ProgressBar bar(10, 10, 100, 10, 0, 200, 50);
    EXPECT_EQ(bar.get_progress(), 50);
    EXPECT_EQ(bar.get_percentage(), 25); // 50 / 200 = 25%

    bar.set_progress(150);
    EXPECT_EQ(bar.get_percentage(), 75); // 150 / 200 = 75%
}

TEST_F(UiEngineTest, ScreenNavigatorCubicEase) {
    // Test cubic easing curve: ease_out_cubic(progress)
    EXPECT_EQ(ScreenNavigator::ease_out_cubic(0), 0u);
    EXPECT_EQ(ScreenNavigator::ease_out_cubic(256), 256u);

    // At progress 128 (50% time), ease out cubic is ahead of linear: ~224 (87.5%)
    EXPECT_GT(ScreenNavigator::ease_out_cubic(128), 128u);
    EXPECT_EQ(ScreenNavigator::ease_out_cubic(128), 224u);
}

