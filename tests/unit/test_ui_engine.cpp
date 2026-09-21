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

// =============================================================================
// 事件分发架构扩展测试：过滤器、监听器、触控捕获、拦截与递归命中测试
// =============================================================================

class MockGestureFilter : public IGestureFilter {
public:
    GestureType intercepted_type = GestureType::NONE;
    bool should_intercept = true;

    bool on_filter_gesture(const GestureEvent& event) override {
        if (should_intercept && event.type == intercepted_type) {
            return true;
        }
        return false;
    }
};

class MockUiGestureListener : public IGestureListener {
public:
    int dispatch_count = 0;
    GestureType last_type = GestureType::NONE;
    bool last_handled = false;

    void on_gesture_dispatched(const GestureEvent& event, bool handled) override {
        dispatch_count++;
        last_type = event.type;
        last_handled = handled;
    }
};

TEST_F(UiEngineTest, GestureFilterAndListener) {
    ViewGroup* root = new ViewGroup(0, 0, 192, 490);
    Button* btn = new Button(50, 50, 100, 50, 0, 0);
    bool clicked = false;
    btn->set_on_click([](void* ctx) { *static_cast<bool*>(ctx) = true; }, &clicked);
    root->add_child(btn);

    UiManager::instance().set_root_view(root);

    MockGestureFilter filter;
    filter.intercepted_type = GestureType::TAP;
    UiManager::instance().set_gesture_filter(&filter);

    MockUiGestureListener listener;
    UiManager::instance().set_gesture_listener(&listener);

    // 1. Filter 拦截 TAP：按钮不应被点击，但全局分发返回 true
    GestureEvent evt_tap = {GestureType::TAP, 60, 60};
    EXPECT_TRUE(UiManager::instance().dispatch_gesture(evt_tap));
    EXPECT_FALSE(clicked);
    EXPECT_EQ(listener.dispatch_count, 1);
    EXPECT_TRUE(listener.last_handled);

    // 2. 取消 Filter 拦截：按钮正常响应点击
    filter.should_intercept = false;
    EXPECT_TRUE(UiManager::instance().dispatch_gesture(evt_tap));
    EXPECT_TRUE(clicked);
    EXPECT_EQ(listener.dispatch_count, 2);
    EXPECT_TRUE(listener.last_handled);

    UiManager::instance().set_gesture_filter(nullptr);
    UiManager::instance().set_gesture_listener(nullptr);
    UiManager::instance().set_root_view(nullptr);
    delete root;
}

TEST_F(UiEngineTest, TouchCaptureRoutingAndAutoRelease) {
    ViewGroup* root = new ViewGroup(0, 0, 192, 490);
    Button* btn = new Button(50, 50, 100, 50, 0, 0);
    root->add_child(btn);
    UiManager::instance().set_root_view(root);

    EXPECT_EQ(UiManager::instance().get_captured_target(), nullptr);

    // 将触控焦点直接锁定到 btn
    UiManager::instance().capture_touch(btn);
    EXPECT_EQ(UiManager::instance().get_captured_target(), btn);

    // 发送一个在 btn 坐标范围之外 (10, 10) 的 TOUCH_DOWN
    // 由于被捕获，事件将直接投递给 btn
    GestureEvent evt_down = {GestureType::TOUCH_DOWN, 60, 60};
    UiManager::instance().dispatch_gesture(evt_down);
    EXPECT_TRUE(btn->is_pressed());

    // 发送 DRAG_END：事件被消费，且捕获自动解除
    GestureEvent evt_end = {GestureType::DRAG_END, 60, 60};
    UiManager::instance().dispatch_gesture(evt_end);
    EXPECT_EQ(UiManager::instance().get_captured_target(), nullptr);

    UiManager::instance().set_root_view(nullptr);
    delete root;
}

TEST_F(UiEngineTest, RecursiveHitTestingFindViewAt) {
    ViewGroup* root = new ViewGroup(0, 0, 192, 490);
    ViewGroup* container = new ViewGroup(10, 10, 100, 100);
    Button* btn = new Button(20, 20, 50, 30, 0, 0);

    container->add_child(btn);
    root->add_child(container);
    UiManager::instance().set_root_view(root);

    // 命中深层嵌套子组件
    EXPECT_EQ(UiManager::instance().find_view_at(25, 25), btn);

    // 命中中间容器
    EXPECT_EQ(UiManager::instance().find_view_at(15, 15), container);

    // 命中根容器
    EXPECT_EQ(UiManager::instance().find_view_at(150, 150), root);

    // 超出屏幕边界
    EXPECT_EQ(UiManager::instance().find_view_at(300, 300), nullptr);

    UiManager::instance().set_root_view(nullptr);
    delete root;
}

class InterceptingContainer : public ViewGroup {
public:
    bool intercepted = false;
    InterceptingContainer(int16_t x, int16_t y, uint16_t w, uint16_t h) : ViewGroup(x, y, w, h) {}

    bool on_intercept_gesture(const GestureEvent& event) override {
        if (event.type == GestureType::SWIPE_LEFT) {
            intercepted = true;
            return true; // 拦截向子节点的传递
        }
        return false;
    }
};

TEST_F(UiEngineTest, ViewGroupGestureInterception) {
    InterceptingContainer* container = new InterceptingContainer(0, 0, 192, 490);
    SliderView* slider = new SliderView(10, 10, 100, 30);
    container->add_child(slider);
    UiManager::instance().set_root_view(container);

    int32_t init_val = slider->get_value();

    // 发送 SWIPE_LEFT：被容器 on_intercept_gesture 拦截
    GestureEvent swipe = {GestureType::SWIPE_LEFT, 50, 20};
    EXPECT_TRUE(UiManager::instance().dispatch_gesture(swipe));
    EXPECT_TRUE(container->intercepted);
    // 滑块未收到被拦截的事件，数值未变
    EXPECT_EQ(slider->get_value(), init_val);

    UiManager::instance().set_root_view(nullptr);
    delete container;
}

TEST_F(UiEngineTest, ExtendedViewListenersAndButtonHighlight) {
    ViewGroup* root = new ViewGroup(0, 0, 192, 490);
    Button* btn = new Button(20, 20, 80, 40, 0x0000, 0xF800);
    root->add_child(btn);
    UiManager::instance().set_root_view(root);

    bool long_clicked = false;
    bool double_clicked = false;
    bool touch_intercepted = false;

    btn->set_on_long_click_listener([](View*, void* ctx) { *static_cast<bool*>(ctx) = true; }, &long_clicked);
    btn->set_on_double_click_listener([](View*, void* ctx) { *static_cast<bool*>(ctx) = true; }, &double_clicked);

    // 1. 测试按钮按压视觉态
    EXPECT_FALSE(btn->is_pressed());
    GestureEvent evt_down = {GestureType::TOUCH_DOWN, 30, 30};
    UiManager::instance().dispatch_gesture(evt_down);
    EXPECT_TRUE(btn->is_pressed());

    GestureEvent evt_up = {GestureType::TOUCH_UP, 30, 30};
    UiManager::instance().dispatch_gesture(evt_up);
    EXPECT_FALSE(btn->is_pressed());

    // 2. 测试长按监听
    GestureEvent evt_long = {GestureType::LONG_PRESS, 30, 30};
    EXPECT_TRUE(UiManager::instance().dispatch_gesture(evt_long));
    EXPECT_TRUE(long_clicked);

    // 3. 测试双击监听
    GestureEvent evt_double = {GestureType::DOUBLE_TAP, 30, 30};
    EXPECT_TRUE(UiManager::instance().dispatch_gesture(evt_double));
    EXPECT_TRUE(double_clicked);

    // 4. 测试通用 on_touch_listener 优先级
    btn->set_on_touch_listener([](View*, const GestureEvent&, void* ctx) -> bool {
        *static_cast<bool*>(ctx) = true;
        return true; // 拦截消费
    }, &touch_intercepted);

    GestureEvent evt_any = {GestureType::TAP, 30, 30};
    EXPECT_TRUE(UiManager::instance().dispatch_gesture(evt_any));
    EXPECT_TRUE(touch_intercepted);

    UiManager::instance().set_root_view(nullptr);
    delete root;
}


