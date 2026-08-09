#ifndef AURORA_APPS_HEART_RATE_SCREEN_HPP
#define AURORA_APPS_HEART_RATE_SCREEN_HPP

#include "../../../ui/screen.hpp"
#include "../../../ui/widgets/text_view.hpp"

namespace aurora {
namespace watch {

// ========================================================
// HeartRateScreen: 实时心率检测页
// ========================================================
class HeartRateScreen : public UI::Screen {
public:
    HeartRateScreen() {}

    void on_create() override {
        // 渲染心率专用测量动画与历史折线图（此处为简化 Placeholder）
        UI::TextView* title = new UI::TextView(30, 50, "Heart Rate", 0xF800, 0, 2);
        add_child(title);

        UI::TextView* measuring = new UI::TextView(20, 200, "Measuring...", 0xFFFF, 0, 2);
        add_child(measuring);
    }

    void on_show() override {
        // 开启 PPG 传感器高频采样模式
    }

    void on_hide() override {
        // 恢复 PPG 传感器低频/休眠模式
    }
};

} // namespace watch
} // namespace aurora

#endif // AURORA_APPS_HEART_RATE_SCREEN_HPP
