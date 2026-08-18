#ifndef AURORA_WATCH_APP_HPP
#define AURORA_WATCH_APP_HPP

#include <stdint.h>
// 引入底层核心引擎与硬件驱动
#include "power_manager.hpp"
#include "st7789_driver.hpp"
#include "gt316_driver.hpp"
#include "sensor_framework.hpp"
#include "gesture_recognizer.hpp"
#include "font_engine.hpp" // 位图字体引擎

#include "../../ui/ui_config.hpp"
#include "../../ui/ui_manager.hpp"
#include "../../ui/screen_navigator.hpp"
#include "screens/watch_face_screen.hpp"

class WatchApp {
private:
    uint32_t simulated_time_h_;
    uint32_t simulated_time_m_;
    uint32_t current_tick_ms_;

    // 手势识别引擎
    GestureRecognizer recognizer_;

    // UI Framework 组件
    FrameBuffer<DISPLAY_WIDTH, AURORA_FB_CHUNK_HEIGHT>* fb_;
    UI::UIRenderer* renderer_;
    aurora::watch::WatchFaceScreen* watch_face_screen_;

    WatchApp()
        : simulated_time_h_(10),
          simulated_time_m_(9),
          current_tick_ms_(0),
          fb_(nullptr),
          renderer_(nullptr),
          watch_face_screen_(nullptr) {}

public:
    static WatchApp& instance() {
        static WatchApp app;
        return app;
    }

    void get_time(uint32_t& h, uint32_t& m) const {
        h = simulated_time_h_;
        m = simulated_time_m_;
    }

    // ========================================================
    // 系统启动时的全量初始化
    // ========================================================
    void init() {
        // 1. 唤醒外设与显示
        St7789Driver::instance().configure(auroraos::hal::get_spi_hal(DISPLAY_SPI_PORT),
                                           auroraos::hal::get_gpio_hal(),
                                           PIN_DISP_DC,
                                           PIN_DISP_RST);
        // 面板有效区相对 GRAM 原点的列/行偏移（192x490 非标屏需量产标定）
        St7789Driver::instance().set_display_offset(DISPLAY_X_OFFSET, DISPLAY_Y_OFFSET);
        St7789Driver::instance().open();

        // 1.1 初始化汇顶 GT316 真实电容触控驱动
        Gt316Driver::instance().configure(auroraos::hal::get_i2c_hal(SENSOR_I2C_PORT),
                                          auroraos::hal::get_gpio_hal(),
                                          PIN_TOUCH_INT);
        Gt316Driver::instance().open();

        // 1.2 初始化传感器套件
        SensorManager::instance().init_all();

        // 2. 启动蓝牙协议栈并开始广播

        // 3. 指向全局静态条带化 framebuffer，避免在堆上分配 184KB
        extern FrameBuffer<DISPLAY_WIDTH, AURORA_FB_CHUNK_HEIGHT> g_fb;
        fb_ = &g_fb;
        renderer_ = new UI::UIRenderer(*fb_);
        UI::UiManager::instance().set_renderer(renderer_);

        // 4. 构建 Watch Face 页面 Widget Tree
        watch_face_screen_ = new aurora::watch::WatchFaceScreen();
        UI::ScreenNavigator::instance().push(watch_face_screen_);
        UI::UiManager::instance().set_root_view(&UI::ScreenNavigator::instance());

        // 5. 强制系统进入亮屏活跃状态
        PowerManager::instance().transition_to(PowerState::ACTIVE);
    }

    // ========================================================
    // 触控输入轮询与手势识别引擎
    // ========================================================
    void poll_input(uint32_t delta_ms);
    void handle_gesture(GestureType gesture);
    void handle_gesture_event(const GestureEvent& event);

    GestureRecognizer& get_recognizer() {
        return recognizer_;
    }

    // ========================================================
    // UI 渲染主心跳 (由 FrameSchedulerV2 在允许的帧窗口内调用)
    // ========================================================
    void on_frame_render() {
        // 如果电源管理器处于息屏状态，直接跳过渲染以极限省电
        if (PowerManager::instance().get_state() == PowerState::IDLE ||
            PowerManager::instance().get_state() == PowerState::SLEEP ||
            PowerManager::instance().get_state() == PowerState::CRITICAL) {
            return;
        }

        // 触控即时采样
        poll_input(0);

        // 调用 UI Framework 引擎驱动自动重绘
        UI::UiManager::instance().render();

        // 将当前条带 (FrameBuffer<192, AURORA_FB_CHUNK_HEIGHT>) 的脏区域刷入 ST7789
        fb_->flush(St7789Driver::instance());
    }

    // ========================================================
    // 后台逻辑主心跳 (处理数据同步与蓝牙分发)
    // ========================================================
    void on_background_tick(uint32_t delta_ticks);
};

#endif // AURORA_WATCH_APP_HPP
