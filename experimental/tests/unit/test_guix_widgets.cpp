#include <gtest/gtest.h>
#include "experimental/guix/guix.hpp"
#include "experimental/drivers/gpu/soft_gpu_device.hpp"

using namespace auroraos::guix;
using namespace auroraos::gpu;

class GuixWidgetTest : public ::testing::Test {
protected:
    void SetUp() override {
        gpu_ = new SoftGpuDevice();
        screen_ = new Surface(320, 240);
        compositor_ = new Compositor(screen_, gpu_);
    }

    void TearDown() override {
        delete compositor_;
        delete screen_;
        delete gpu_;
    }

    SoftGpuDevice* gpu_ = nullptr;
    Surface* screen_ = nullptr;
    Compositor* compositor_ = nullptr;
};

// 1. 测试基础控件树层级与几何计算
TEST_F(GuixWidgetTest, WidgetHierarchyAndGeometry) {
    Panel root_panel;
    root_panel.set_bounds(10, 10, 200, 150);

    Button child_btn("OK");
    child_btn.set_bounds(5, 5, 60, 25);

    root_panel.add_child(&child_btn);

    EXPECT_EQ(child_btn.get_parent(), &root_panel);
    EXPECT_EQ(root_panel.get_first_child(), &child_btn);

    // 相对坐标
    EXPECT_EQ(child_btn.get_x(), 5);
    EXPECT_EQ(child_btn.get_y(), 5);

    // 绝对窗口坐标 (10+5=15, 10+5=15)
    Rect wb = child_btn.get_window_bounds();
    EXPECT_EQ(wb.x, 15);
    EXPECT_EQ(wb.y, 15);
    EXPECT_EQ(wb.w, 60);
    EXPECT_EQ(wb.h, 25);

    // 命中测试
    EXPECT_EQ(root_panel.hit_test(16, 16), &child_btn);
    EXPECT_EQ(root_panel.hit_test(100, 100), &root_panel);
    EXPECT_EQ(root_panel.hit_test(5, 5), nullptr); // 超出 root_panel 边界

    // 移除子节点
    root_panel.remove_child(&child_btn);
    EXPECT_EQ(child_btn.get_parent(), nullptr);
    EXPECT_EQ(root_panel.get_first_child(), nullptr);
}

// 2. 测试 Button 交互与点击回调
TEST_F(GuixWidgetTest, ButtonClickInteraction) {
    Window win(100, 100, gpu_, compositor_);
    Button btn("ClickMe");
    btn.set_bounds(10, 10, 80, 30);
    btn.set_colors(Color::DarkGray, Color::White, Color::Red, Color::White);

    static int click_count = 0;
    click_count = 0;

    btn.set_click_handler([](Button* b, void* data) {
        (void)b;
        int* counter = static_cast<int*>(data);
        (*counter)++;
    }, &click_count);

    win.set_root_widget(&btn);

    // 初始状态
    EXPECT_FALSE(btn.is_pressed());
    EXPECT_EQ(click_count, 0);

    // 模拟 PointerDown (点击进入按钮范围)
    InputEvent down_ev;
    down_ev.type = InputEventType::PointerDown;
    down_ev.x = win.get_x() + 20; // 屏幕坐标
    down_ev.y = win.get_y() + 20;
    EXPECT_TRUE(win.handle_event(down_ev));
    EXPECT_TRUE(btn.is_pressed());

    // 模拟 PointerUp (在按钮内抬起)
    InputEvent up_ev;
    up_ev.type = InputEventType::PointerUp;
    up_ev.x = win.get_x() + 20;
    up_ev.y = win.get_y() + 20;
    EXPECT_TRUE(win.handle_event(up_ev));
    EXPECT_FALSE(btn.is_pressed());
    EXPECT_EQ(click_count, 1);

    // 模拟 PointerDown 之后 PointerMove 移出按钮区域
    EXPECT_TRUE(win.handle_event(down_ev));
    EXPECT_TRUE(btn.is_pressed());

    InputEvent move_out_ev;
    move_out_ev.type = InputEventType::PointerMove;
    move_out_ev.x = win.get_x() + 95; // 移出按钮 (btn.w=80)
    move_out_ev.y = win.get_y() + 20;
    win.handle_event(move_out_ev);
    EXPECT_FALSE(btn.is_pressed());
}

// 3. 测试 Label 文本属性与对齐
TEST_F(GuixWidgetTest, LabelPropertiesAndAlignment) {
    Label lbl("AuroraOS");
    lbl.set_bounds(0, 0, 100, 20);
    EXPECT_STREQ(lbl.get_text(), "AuroraOS");

    lbl.set_text_color(Color::Yellow);
    EXPECT_EQ(lbl.get_text_color(), Color::Yellow);

    lbl.set_alignment(Alignment::Center);
    EXPECT_EQ(lbl.get_alignment(), Alignment::Center);

    lbl.set_scale(2);
    EXPECT_EQ(lbl.get_scale(), 2);

    lbl.set_text("Updated");
    EXPECT_STREQ(lbl.get_text(), "Updated");
}

// 4. 测试 ProgressBar 范围与值更新
TEST_F(GuixWidgetTest, ProgressBarValuesAndClamping) {
    ProgressBar bar(0, 100, 25);
    bar.set_bounds(0, 0, 100, 10);

    EXPECT_EQ(bar.get_value(), 25);
    EXPECT_EQ(bar.get_min(), 0);
    EXPECT_EQ(bar.get_max(), 100);

    // 边界钳位
    bar.set_value(150);
    EXPECT_EQ(bar.get_value(), 100);

    bar.set_value(-20);
    EXPECT_EQ(bar.get_value(), 0);

    bar.set_range(50, 200);
    EXPECT_EQ(bar.get_min(), 50);
    EXPECT_EQ(bar.get_max(), 200);
    EXPECT_EQ(bar.get_value(), 50); // 之前是0，被自动钳位到新下限 50
}

// 5. 测试 Slider 拖拽交互与数值变化回调
TEST_F(GuixWidgetTest, SliderDragAndCallback) {
    Window win(120, 60, gpu_, compositor_);
    Slider slider(0, 100, 10);
    slider.set_bounds(10, 10, 100, 20);

    static int last_val = -1;
    last_val = -1;

    slider.set_value_changed_handler([](Slider* s, int32_t val, void* data) {
        (void)s;
        int* output = static_cast<int*>(data);
        *output = val;
    }, &last_val);

    win.set_root_widget(&slider);

    EXPECT_EQ(slider.get_value(), 10);

    // 拖拽到 50% 位置 (win.x=0, slider.x=10, slider.w=100 -> rel_x=50 -> screen_x = 60)
    InputEvent down_ev;
    down_ev.type = InputEventType::PointerDown;
    down_ev.x = 60;
    down_ev.y = 20;
    EXPECT_TRUE(win.handle_event(down_ev));
    EXPECT_TRUE(slider.is_dragging());
    EXPECT_EQ(slider.get_value(), 50);
    EXPECT_EQ(last_val, 50);

    // 拖拽到最右侧 (screen_x = 110 -> 100%)
    InputEvent move_ev;
    move_ev.type = InputEventType::PointerMove;
    move_ev.x = 110;
    move_ev.y = 20;
    EXPECT_TRUE(win.handle_event(move_ev));
    EXPECT_EQ(slider.get_value(), 100);
    EXPECT_EQ(last_val, 100);

    // 释放拖拽
    InputEvent up_ev;
    up_ev.type = InputEventType::PointerUp;
    up_ev.x = 110;
    up_ev.y = 20;
    EXPECT_TRUE(win.handle_event(up_ev));
    EXPECT_FALSE(slider.is_dragging());
}

// 6. 测试 Panel 容器与复合控件树绘制
TEST_F(GuixWidgetTest, PanelAndCompositeRendering) {
    Window win(150, 100, gpu_, compositor_);
    Panel root_panel(Color::Black, false, Color::White);
    root_panel.set_bounds(0, 0, 150, 100);
    root_panel.set_draw_border(true);

    Label title("Control Panel");
    title.set_bounds(10, 5, 130, 15);
    title.set_alignment(Alignment::Center);

    ProgressBar progress(0, 100, 60);
    progress.set_bounds(10, 30, 130, 15);

    Button btn("Submit");
    btn.set_bounds(35, 55, 80, 25);

    root_panel.add_child(&title);
    root_panel.add_child(&progress);
    root_panel.add_child(&btn);

    win.set_root_widget(&root_panel);

    // 执行树形渲染
    win.paint_widgets();

    // 验证 backing store 表面已填充像素 (非全零)
    Surface* surface = win.get_surface();
    ASSERT_NE(surface, nullptr);
    const uint16_t* buf = static_cast<const uint16_t*>(surface->get_buffer());
    ASSERT_NE(buf, nullptr);

    bool has_non_zero = false;
    for (uint32_t i = 0; i < 150 * 100; ++i) {
        if (buf[i] != 0) {
            has_non_zero = true;
            break;
        }
    }
    EXPECT_TRUE(has_non_zero);
}
