#ifndef AURORA_APPS_WATCH_FACE_SCREEN_HPP
#define AURORA_APPS_WATCH_FACE_SCREEN_HPP

#include "../../../ui/screen.hpp"
#include "../../../ui/widgets/text_view.hpp"
#include "../../../ui/widgets/arc_progress.hpp"
#include "power_manager.hpp"
#include "sensor_framework.hpp"
#include "../../../ui/screen_navigator.hpp"
#include "heart_rate_screen.hpp"

namespace aurora {
namespace watch {

// ========================================================
// WatchFaceScreen: 默认表盘页面
// ========================================================
class WatchFaceScreen : public UI::Screen {
public:
    WatchFaceScreen() 
        : time_text_(nullptr), hr_text_(nullptr), steps_text_(nullptr) {}

    void on_create() override {
        // 顶部状态栏: 蓝牙和电量
        UI::TextView* ble_text = new UI::TextView(150, 10, "BLE", 0x07E0, 0, 1);
        add_child(ble_text);
        
        // 时间显示 (居中)
        time_text_ = new UI::TextView(20, 100, "10:09", 0xFFFF, 0, 4);
        add_child(time_text_);

        // 运动健康数据 (底部)
        hr_text_ = new UI::TextView(80, 250, "HR: 0", 0xF800, 0, 2);
        add_child(hr_text_);

        steps_text_ = new UI::TextView(80, 300, "STP: 0", 0x07E0, 0, 2);
        add_child(steps_text_);
        
        // 进度条圆弧
        UI::ArcProgress* battery_arc = new UI::ArcProgress(96, 400, 40, 85, 0x07E0);
        add_child(battery_arc);
    }

    void on_show() override {
        // 每次显示时强制刷新一次数据
        update_data();
    }

    bool handle_gesture(const UI::GestureEvent& event) override {
        if (event.type == GestureType::SWIPE_LEFT) {
            // 左滑进入心率检测页面
            UI::ScreenNavigator::instance().push(new HeartRateScreen());
            return true;
        } else if (event.type == GestureType::SWIPE_DOWN) {
            // TODO: push(new QuickPanelScreen());
            return true;
        }
        
        // 将事件传递给 ViewGroup 的子节点
        return UI::ViewGroup::handle_gesture(event);
    }

    // 暴露更新接口供 WatchApp::on_background_tick 调用
    void set_time(uint32_t h, uint32_t m) {
        if (!time_text_) return;
        static char time_str[6];
        time_str[0] = (h / 10) + '0';
        time_str[1] = (h % 10) + '0';
        time_str[2] = ':';
        time_str[3] = (m / 10) + '0';
        time_str[4] = (m % 10) + '0';
        time_str[5] = '\0';
        time_text_->set_text(time_str);
    }

    void set_health_data(uint32_t bpm, uint32_t steps) {
        if (hr_text_) {
            static char hr_buf[16];
            hr_buf[0] = 'H'; hr_buf[1] = 'R'; hr_buf[2] = ':'; hr_buf[3] = ' ';
            hr_buf[4] = (bpm / 100) + '0';
            hr_buf[5] = ((bpm / 10) % 10) + '0';
            hr_buf[6] = (bpm % 10) + '0';
            hr_buf[7] = '\0';
            hr_text_->set_text(hr_buf);
        }

        if (steps_text_) {
            static char stp_buf[16];
            stp_buf[0] = 'S'; stp_buf[1] = 'T'; stp_buf[2] = 'P'; stp_buf[3] = ':'; stp_buf[4] = ' ';
            stp_buf[5] = (steps / 1000) + '0';
            stp_buf[6] = ((steps / 100) % 10) + '0';
            stp_buf[7] = ((steps / 10) % 10) + '0';
            stp_buf[8] = (steps % 10) + '0';
            stp_buf[9] = '\0';
            steps_text_->set_text(stp_buf);
        }
    }

private:
    void update_data() {
        uint32_t current_bpm = 0;
        uint32_t current_steps = SensorManager::instance().get_accel_sensor().get_steps();
        SensorData data;
        if (SensorManager::instance().pop_data(&data) && data.type == SensorType::HEART_RATE) {
            current_bpm = data.payload.bpm;
        }
        set_health_data(current_bpm, current_steps);
    }

    UI::TextView* time_text_;
    UI::TextView* hr_text_;
    UI::TextView* steps_text_;
};

} // namespace watch
} // namespace aurora

#endif // AURORA_APPS_WATCH_FACE_SCREEN_HPP
