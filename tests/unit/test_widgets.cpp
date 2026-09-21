// =============================================================================
// tests/unit/test_widgets.cpp
//
// AuroraOS UI 控件库全量单元测试
// 覆盖：SwitchView, ScrollView, ImageView, DialogView, TextView, ProgressBar, SliderView, Button
// =============================================================================
#include <gtest/gtest.h>

#include "../../ui/ui_manager.hpp"
#include "../../ui/widgets/button.hpp"
#include "../../ui/widgets/text_view.hpp"
#include "../../ui/widgets/progress_bar.hpp"
#include "../../ui/widgets/slider_view.hpp"
#include "../../ui/widgets/arc_progress.hpp"
#include "../../ui/widgets/switch_view.hpp"
#include "../../ui/widgets/scroll_view.hpp"
#include "../../ui/widgets/image_view.hpp"
#include "../../ui/widgets/dialog_view.hpp"

using namespace UI;

class WidgetsTest : public ::testing::Test {
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

// ========================================================
// 1. SwitchView 开关控件测试
// ========================================================
TEST_F(WidgetsTest, SwitchViewToggleAndCallbacks) {
    SwitchView sw(20, 20, 50, 28, false);
    EXPECT_FALSE(sw.is_checked());

    bool callback_triggered = false;

    sw.set_on_checked_changed_listener([](SwitchView*, bool checked, void* ctx) {
        (void)checked;
        auto* triggered = static_cast<bool*>(ctx);
        *triggered = true;
    }, &callback_triggered);

    // 1. 点击切换开
    GestureEvent tap = {GestureType::TAP, 30, 30};
    EXPECT_TRUE(sw.handle_gesture(tap));
    EXPECT_TRUE(sw.is_checked());
    EXPECT_TRUE(callback_triggered);

    // 2. 再次点击切换关
    callback_triggered = false;
    EXPECT_TRUE(sw.handle_gesture(tap));
    EXPECT_FALSE(sw.is_checked());
    EXPECT_TRUE(callback_triggered);

    // 3. 右滑开启，左滑关闭
    GestureEvent swipe_right = {GestureType::SWIPE_RIGHT, 30, 30};
    EXPECT_TRUE(sw.handle_gesture(swipe_right));
    EXPECT_TRUE(sw.is_checked());

    GestureEvent swipe_left = {GestureType::SWIPE_LEFT, 30, 30};
    EXPECT_TRUE(sw.handle_gesture(swipe_left));
    EXPECT_FALSE(sw.is_checked());
}

// ========================================================
// 2. ScrollView 可滚动视图测试
// ========================================================
TEST_F(WidgetsTest, ScrollViewScrollingAndBounds) {
    ScrollView* scroll = new ScrollView(0, 0, 100, 200);

    for (int i = 0; i < 5; ++i) {
        scroll->add_child(new TextView(10, static_cast<int16_t>(i * 60), "Item", 0xFFFF));
    }

    scroll->set_content_size(100, 400);
    EXPECT_EQ(scroll->get_content_height(), 400u);
    EXPECT_EQ(scroll->get_scroll_y(), 0);

    // 1. 拖拽滚动 (用户向上拖拽 50px，内容向下滚动)
    GestureEvent drag = {GestureType::DRAG_MOVE, 50, 100, 0, -50};
    EXPECT_TRUE(scroll->handle_gesture(drag));
    EXPECT_EQ(scroll->get_scroll_y(), 50);

    // 2. 边界钳制测试 (不能滚出顶部负值)
    GestureEvent drag_up = {GestureType::DRAG_MOVE, 50, 100, 0, 100};
    EXPECT_TRUE(scroll->handle_gesture(drag_up));
    EXPECT_EQ(scroll->get_scroll_y(), 0);

    // 3. 翻页滑动手势
    GestureEvent swipe_up = {GestureType::SWIPE_UP, 50, 100};
    EXPECT_TRUE(scroll->handle_gesture(swipe_up));
    EXPECT_EQ(scroll->get_scroll_y(), 100); // 滚动半屏高度 200 / 2 = 100

    delete scroll;
}

TEST_F(WidgetsTest, ScrollViewInterceptsDragFromChildren) {
    ScrollView* scroll = new ScrollView(0, 0, 100, 200);
    scroll->set_content_size(100, 400);

    // 容器内拖拽手势应被 on_intercept_gesture 拦截
    GestureEvent drag = {GestureType::DRAG_START, 50, 50, 0, -15};
    EXPECT_TRUE(scroll->on_intercept_gesture(drag));

    // 容器外的拖拽不拦截
    GestureEvent drag_outside = {GestureType::DRAG_START, 150, 50, 0, -15};
    EXPECT_FALSE(scroll->on_intercept_gesture(drag_outside));

    delete scroll;
}

// ========================================================
// 3. ImageView 图像与图标控件测试
// ========================================================
TEST_F(WidgetsTest, ImageViewProperties) {
    static const uint16_t mock_bitmap[4] = {0xFFFF, 0x0000, 0x07E0, 0xF800};
    ImageView img(10, 10, 20, 20, mock_bitmap, true, 0x0000);

    EXPECT_EQ(img.get_bitmap(), mock_bitmap);

    // 重新设置位图
    img.set_bitmap(nullptr, 0, 0);
    EXPECT_EQ(img.get_bitmap(), nullptr);
}

// ========================================================
// 4. DialogView 模态弹窗测试
// ========================================================
TEST_F(WidgetsTest, DialogViewModalInterceptionAndButtons) {
    DialogView* dialog = new DialogView(192, 490, "Alert", "Confirm action?", 160, 120);

    bool confirmed = false;
    bool cancelled = false;

    dialog->set_on_confirm_listener([](DialogView*, void* ctx) {
        *static_cast<bool*>(ctx) = true;
    }, &confirmed);

    dialog->set_on_cancel_listener([](DialogView*, void* ctx) {
        *static_cast<bool*>(ctx) = true;
    }, &cancelled);

    // 1. 点击卡片外遮罩区域：全屏拦截，不传透
    GestureEvent tap_outside = {GestureType::TAP, 5, 5};
    EXPECT_TRUE(dialog->handle_gesture(tap_outside));
    EXPECT_FALSE(confirmed);
    EXPECT_FALSE(cancelled);

    // 2. 开启点击外部自关闭
    dialog->set_dismiss_on_touch_outside(true);
    EXPECT_TRUE(dialog->handle_gesture(tap_outside));
    EXPECT_TRUE(cancelled);

    delete dialog;
}

// ========================================================
// 5. TextView 对齐与动态尺寸测试
// ========================================================
TEST_F(WidgetsTest, TextViewAlignmentAndDynamicText) {
    TextView tv(10, 10, 100, 20, "Hello", 0xFFFF, 0x0000, 2, TextAlign::CENTER);
    EXPECT_EQ(tv.get_text_align(), TextAlign::CENTER);
    EXPECT_STREQ(tv.get_text(), "Hello");

    tv.set_text_align(TextAlign::RIGHT);
    EXPECT_EQ(tv.get_text_align(), TextAlign::RIGHT);

    tv.set_text("World!");
    EXPECT_STREQ(tv.get_text(), "World!");
}

// ========================================================
// 6. Button 与 ProgressBar 圆角与属性测试
// ========================================================
TEST_F(WidgetsTest, ButtonAndProgressBarCornerRadius) {
    Button btn(10, 10, 80, 40, 0x0000, 0xF800, 10);
    EXPECT_EQ(btn.get_corner_radius(), 10u);

    btn.set_corner_radius(15);
    EXPECT_EQ(btn.get_corner_radius(), 15u);

    ProgressBar bar(10, 60, 100, 15, 0, 100, 30, 0x0000, 0x07E0, 5);
    EXPECT_EQ(bar.get_corner_radius(), 5u);

    bar.set_corner_radius(8);
    EXPECT_EQ(bar.get_corner_radius(), 8u);
}
