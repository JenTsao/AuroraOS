#include "watch_app.hpp"
#include "power_manager.hpp"
#include "st7789_driver.hpp"
#include "gt316_driver.hpp"
#include "sensor_framework.hpp"
#include "gesture_recognizer.hpp"
#include "ble_stack.hpp"
#include "font_engine.hpp"

// ========================================================
// 静态全局变量与微型显存池
// ========================================================
// 手环独立定义 g_fb，供 mini_program_engine.hpp 引用
// 采用统一的 AURORA_FB_CHUNK_HEIGHT (30) 条带化策略，节省约 170KB SRAM
FrameBuffer<DISPLAY_WIDTH, AURORA_FB_CHUNK_HEIGHT> g_fb;
HeartRateSensor g_health_sensor;

// 深色系主题常量 (极致降低 AMOLED 功耗)
static constexpr uint16_t COLOR_BG_DARK = 0x0821;     // 深渊黑
static constexpr uint16_t COLOR_TEXT_ACCENT = 0x07E0; // 极光绿
static constexpr uint16_t COLOR_TEXT_MUTED = 0x8410;  // 碳灰

// ========================================================
// 1. 硬件触控轮询与手势转换管道
// ========================================================
void WatchApp::poll_input(uint32_t delta_ms) {
    current_tick_ms_ += delta_ms;

    TouchPoint point;
    if (Gt316Driver::instance().poll_touch(&point, current_tick_ms_)) {
        GestureEvent event = recognizer_.feed_touch_point(point, current_tick_ms_);
        if (event.type != GestureType::NONE) {
            handle_gesture_event(event);
        }
    }
}

// ========================================================
// 2. 交互状态路由接管 (7 种手势响应)
// ========================================================
void WatchApp::handle_gesture_event(const GestureEvent& event) {
    if (event.type == GestureType::NONE)
        return;

    // 交互防抖：触发任何手势，系统立即重置熄屏倒计时，保持 Active 状态
    PowerManager::instance().transition_to(PowerState::ACTIVE);

    // 新的 ScreenNavigator 接管了 root_view，会统一拦截全局手势（如右滑退出）并向下分发
    UI::UiManager::instance().dispatch_gesture(event);
}

void WatchApp::handle_gesture(GestureType gesture) {
    GestureEvent event = {gesture, 0, 0};
    handle_gesture_event(event);
}

// ========================================================
// 3. 蓝牙与后台数据流同步接管
// ========================================================
void WatchApp::on_background_tick(uint32_t delta_ticks) {
    // 轮询触控硬件输入并驱动手势引擎
    poll_input(delta_ticks);

    // 驱动电源生命周期引擎
    PowerManager::instance().on_tick(delta_ticks);

    // 驱动 UI 过渡动画引擎
    UI::ScreenNavigator::instance().on_tick(delta_ticks);

    // 模拟时间流逝
    static uint32_t ms_accumulator = 0;
    ms_accumulator += delta_ticks;
    if (ms_accumulator >= 60000) { // 每 60 秒 (1分钟)
        ms_accumulator = 0;
        simulated_time_m_++;
        if (simulated_time_m_ >= 60) {
            simulated_time_m_ = 0;
            simulated_time_h_ = (simulated_time_h_ + 1) % 24;
        }

        if (watch_face_screen_) {
            watch_face_screen_->set_time(simulated_time_h_, simulated_time_m_);
        }
    }

    uint32_t current_bpm = 0;
    uint32_t current_steps = SensorManager::instance().get_accel_sensor().get_steps();
    SensorData data;
    if (SensorManager::instance().pop_data(&data) && data.type == SensorType::HEART_RATE) {
        current_bpm = data.payload.bpm;
    }

    // 这里其实不应该在 WatchApp 里轮询更新 UI，而是 WatchFaceScreen 自己通过 on_show 或者 on_tick 获取。
    // 为了兼容原有逻辑，我们将数据直接传给表盘
    if (watch_face_screen_) {
        watch_face_screen_->set_health_data(current_bpm, current_steps);
    }

    // 蓝牙 GATT Server 数据同步
    static uint32_t sync_throttle = 0;
    sync_throttle += delta_ticks;

    // 限制蓝牙同步频率为 1Hz，防止射频芯片过热并节省电量
    if (sync_throttle >= 1000) {
        sync_throttle = 0;
        if (current_bpm > 0) {}
    }
}

// 供全局或 Lua 脚本获取当前模拟时间
void aurora_get_time(uint32_t& h, uint32_t& m) {
    WatchApp::instance().get_time(h, m);
}
