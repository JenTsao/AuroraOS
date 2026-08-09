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
        // ...
    }
    
    void TearDown() override {
        // ...
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

    // 内存泄漏清理
    delete root; 
    UiManager::instance().set_root_view(nullptr);
}
