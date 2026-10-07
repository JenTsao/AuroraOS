#ifndef AURORA_WATCH_APP_HPP
#define AURORA_WATCH_APP_HPP

#include <stdint.h>
// 引入底层核心引擎与硬件驱动
#include "power_manager.hpp"
#include "st7789_driver.hpp"
#include "gt316_driver.hpp"
#include "button_driver.hpp"
#include "sensor_framework.hpp"
#include "gesture_recognizer.hpp"
#include "font_engine.hpp" // 位图字体引擎

#include "../../ui/ui_config.hpp"
#include "../../ui/ui_manager.hpp"
#include "../../ui/screen_navigator.hpp"
#include "../../ui/overlay_root_view.hpp"
#include "screens/watch_face_screen.hpp"
#include "notification_overlay_view.hpp"
#include "ble_stack.hpp"
#include "../../net/ble/nimble_bridge.hpp"

// WatchApp 同时充当 UI 条带落屏接收器（UI::IBandSink）：UiManager 只负责
// 「画哪些条带、裁剪到哪」，由本类把每条带 flush 给 ST7789，从而 UI 层不依赖
// 具体显示驱动（依赖倒置）。
class WatchApp : public UI::IBandSink {
private:
    uint32_t simulated_time_h_;
    uint32_t simulated_time_m_;
    uint32_t current_tick_ms_;

    // 手势识别引擎
    GestureRecognizer recognizer_;

    // 按键交互状态（handle_key_event 用）
    uint32_t key_down_tick_ = 0;   // 最近一次 down 事件的 timestamp（预留：按压时长统计）
    bool key_repeat_seen_ = false; // 本次按压是否已见过 repeat（true = 长按已达成，
                                   //  抬起时不再触发短按动作）

    // UI Framework 组件
    FrameBuffer<DISPLAY_WIDTH, AURORA_UI_BAND_H>* fb_;
    UI::UIRenderer* renderer_;
    aurora::watch::WatchFaceScreen* watch_face_screen_;

    // 系统浮层：overlay_root_ 是交给 UiManager 的根容器，notif_overlay_ 是
    // 盖在页面栈之上的通知浮层（同时是 NotificationCenter 的表现层端口）。
    // 均为「进程期单例」式分配：与 renderer_ / watch_face_screen_ 同策略，
    // 裸机无退出路径，不做回收。
    UI::OverlayRootView* overlay_root_;
    aurora::NotificationOverlayView* notif_overlay_;

    WatchApp()
        : simulated_time_h_(10), simulated_time_m_(9), current_tick_ms_(0), fb_(nullptr), renderer_(nullptr),
          watch_face_screen_(nullptr), overlay_root_(nullptr), notif_overlay_(nullptr) {}

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
    // UI::IBandSink —— 把 UiManager 画好的每条带推给 ST7789
    // ========================================================
    UI::UIRenderer& begin_band(int16_t /*logical_top*/, uint16_t /*rows*/) noexcept override {
        return *renderer_;
    }

    void end_band(int16_t logical_top, uint16_t /*rows*/) noexcept override {
        // y_base 必须是屏幕绝对行号：SpiLcdDriverBase::set_window 按整屏 height
        // 钳制后才叠加面板显示偏移，传条带内相对行号会把所有条带都写到屏幕顶部。
        fb_->flush(St7789Driver::instance(), static_cast<uint16_t>(logical_top));
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

        // 1.1.0 初始化物理侧键驱动（轮询模型，UI 线程独占）
        // 低有效 + 内部上拉（BUTTON_SIDE_ACTIVE_HIGH=0 → open() 配 PullUp）
        ButtonDriver::instance().configure(auroraos::hal::get_gpio_hal(), PIN_BUTTON_SIDE, BUTTON_SIDE_ACTIVE_HIGH);
        ButtonDriver::instance().open();
        // 注册到 VFS /dev/button0：DeviceRegistry::register_device 会同时
        // 挂载 VFS（miband 构建含 device.cpp + vfs_service.cpp，已核实可链）。
        // 必须用 instance()，严禁再造全局实例（两实例 = 两套状态机 + 事件分裂）。
        DeviceRegistry::instance().register_device(&ButtonDriver::instance());

        // 1.1.1 绑定 BHI260AP 6 轴加速度计的 I2C 总线
        // （须在 init_all() 之前，init() 内据此探测并使能 accel）
        SensorManager::instance().get_accel_sensor().configure(auroraos::hal::get_i2c_hal(SENSOR_I2C_PORT),
                                                               I2C_ADDR_BHI260AP);

        // 1.2 初始化传感器套件
        SensorManager::instance().init_all();

        // 2. 启动蓝牙协议栈并开始广播 (初始化 NimBLE 桥接并触发隐身/常规广播)
        auroraos::ble::NimbleBridge::instance().init();
        BleManager::instance().init();

        // 3. 指向全局静态条带化 framebuffer，避免在堆上分配 184KB
        extern FrameBuffer<DISPLAY_WIDTH, AURORA_UI_BAND_H> g_fb;
        fb_ = &g_fb;
        renderer_ = new UI::UIRenderer(*fb_);
        UI::UiManager::instance().set_renderer(renderer_);
        // 注册本类为条带落屏接收器：render() 内部逐带调用 end_band() 完成 flush
        UI::UiManager::instance().set_band_sink(this);

        // 4. 构建 Watch Face 页面 Widget Tree
        // 手环屏为 12MHz SPI + 条带化缓冲：全屏 192x490x2B = 188,160B
        // ≈ 125.44ms/帧，而滑动转场必然整屏变脏 → 实际帧率上限约 8fps，
        // 无法维持 30fps 目标。故显式声明应用策略为「瞬切」；
        // 库默认仍是 PUSH_LEFT / POP_RIGHT，向后兼容不变。
        UI::ScreenNavigator::instance().set_default_transition(UI::ScreenNavigator::TransitionType::NONE);
        watch_face_screen_ = new aurora::watch::WatchFaceScreen();
        UI::ScreenNavigator::instance().push(watch_face_screen_, UI::ScreenNavigator::TransitionType::NONE);

        // 4.1 搭系统浮层根容器。
        //     不能把通知浮层直接 add_child 到 ScreenNavigator：它覆写了
        //     ViewGroup::draw()，只画 stack_ 里的当前 Screen，从不遍历
        //     children_（手势分发同理），子节点既画不出也收不到事件；而
        //     ~ViewGroup 还会 delete 掉静态的 ScreenNavigator。
        //     改为薄根容器包一层：add 顺序即绘制顺序，后者盖在前者之上。
        notif_overlay_ = new aurora::NotificationOverlayView();
        overlay_root_ = new UI::OverlayRootView();
        overlay_root_->add_child(&UI::ScreenNavigator::instance());
        overlay_root_->add_child(notif_overlay_);
        UI::UiManager::instance().set_root_view(overlay_root_);

        // 4.2 注册通知中心的表现层端口。
        //     在此之前 NotificationCenter::overlay_ 恒为 nullptr：BLE
        //     (ble_stack.cpp) 与 NFC (nfc_service.cpp) 投进来的通知只会
        //     一路堆进优先级队列 —— post() 不派发、on_tick() 直接返回，
        //     队列满后静默丢弃。真机上既看不见也清不掉。
        aurora::NotificationCenter::instance().set_overlay(notif_overlay_);

        // 5. 强制系统进入亮屏活跃状态
        PowerManager::instance().transition_to(PowerState::ACTIVE);
    }

    // ========================================================
    // 触控输入轮询与手势识别引擎
    // ========================================================
    void poll_input(uint32_t delta_ms);
    void handle_key_event(const InputEvent& event);
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

        // 触控采样【刻意不在此处进行】—— 已统一收敛到 on_background_tick()
        // （watch_app.cpp:73）由 sensor_ble_daemon_task 单点驱动，40ms 周期。
        //
        // 为什么删掉原poll_input(0)：poll_touch 的状态机（gt316_driver.hpp）
        // 有一个「同一 RELEASED 只允许被消费一次」的不变量。此前本函数
        // （Realtime 优先级，~30fps）与后台守护任务（High 优先级，40ms）
        // 两个不同优先级的任务都会调用它，构成跨优先级竞态：其中一个任务
        // 可能在本毫秒内把 RELEASED 消费掉并立即产出新的 PRESSED，导致
        // 抬起事件在到达手势层之前就被吃掉 —— 用户表现为「手指还没离屏，
        // 抬手就没反应了」。
        //
        // 单一采样者（single sampler）是消除这类竞态的根本手段：状态机
        // 不再有第二个并发调用者，就不必依赖调用时序的运气。
        // 本函数保留的 3 处电源状态早退（IDLE/SLEEP/CRITICAL）也因此
        // 不再影响触摸采样 —— 息屏期间触摸仍由后台守护任务正常驱动。

        // UI 引擎内部完成「取 damage → 逐带裁剪绘制 → 分带 flush」：
        // 推屏由本类作为 IBandSink 在 end_band() 中执行，此处不再额外 flush。
        UI::UiManager::instance().render();
    }

    // ========================================================
    // 后台逻辑主心跳 (处理数据同步与蓝牙分发)
    // ========================================================
    void on_background_tick(uint32_t delta_ticks);
};

#endif // AURORA_WATCH_APP_HPP
