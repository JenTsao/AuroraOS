#include "watch_app.hpp"
#include "power_manager.hpp"
#include "st7789_driver.hpp"
#include "gt316_driver.hpp"
#include "sensor_framework.hpp"
#include "gesture_recognizer.hpp"
#include "ble_stack.hpp"
#include "font_engine.hpp"

// ========================================================
// 条带化渲染的板级约束（本 TU 仅被 miband8 编译）
// ========================================================
// 490 = 2 · 5 · 7²，合法的条带高只有 1/2/5/7/10/14/35/49/70/98/245/490；
// 35 精确整除 → 14 条带、无残带。
//
// 把「板级 -D 压掉 ui_config.hpp 的 #ifndef」这类静默错误（例如板级仍写着
// 30，于是 490/30 余 10、残带照旧）变成编译期硬错误。
static_assert(DISPLAY_HEIGHT % AURORA_UI_BAND_H == 0,
              "AURORA_FB_CHUNK_HEIGHT must divide DISPLAY_HEIGHT (miband8: 490 = 14 * 35)");

// ========================================================
// 静态全局变量与微型显存池
// ========================================================
// 手环独立定义 g_fb，供 mini_program_engine.hpp 引用
// 采用统一的 AURORA_FB_CHUNK_HEIGHT (35) 条带化策略，节省约 170KB SRAM
FrameBuffer<DISPLAY_WIDTH, AURORA_UI_BAND_H> g_fb;
HeartRateSensor g_health_sensor;

// 深色系主题常量 (极致降低 AMOLED 功耗)
static constexpr uint16_t COLOR_BG_DARK = 0x0821;     // 深渊黑
static constexpr uint16_t COLOR_TEXT_ACCENT = 0x07E0; // 极光绿
static constexpr uint16_t COLOR_TEXT_MUTED = 0x8410;  // 碳灰

// ========================================================
// 1. 硬件输入轮询（物理按键 + 触控）与手势转换管道
// ========================================================
void WatchApp::poll_input(uint32_t delta_ms) {
    current_tick_ms_ += delta_ms;

    // 1a. 物理按键轮询（放在触摸之前）：与触摸共用同一时间基准与线程。
    // 顺序有讲究——息屏唤醒的那次按键在本帧就被消费掉，同一帧内硬件
    // 不会也不该再产生触摸事件；先处理按键保证唤醒帧不被触摸误判。
    ButtonDriver::instance().poll(current_tick_ms_);
    InputEvent key_ev;
    while (ButtonDriver::instance().pop_event(&key_ev)) {
        handle_key_event(key_ev);
    }

    // 1b. 触控路径（原逻辑不变）
    TouchPoint point;
    if (Gt316Driver::instance().poll_touch(&point, current_tick_ms_)) {
        GestureEvent event = recognizer_.feed_touch_point(point, current_tick_ms_);
        if (event.type != GestureType::NONE) {
            handle_gesture_event(event);
        }
    }
}

// ========================================================
// 1.1 按键事件策略路由（事实层 ButtonDriver → 策略层，映射单点）
//
// 事件模型（Linux 输入风格）：value 1=down / 0=up / 2=repeat；
// 短按 = up 且期间未见过 repeat；长按 = 第一个 repeat。
// 映射（与手势语义对齐）：
//   息屏态(IDLE/SLEEP) down → 唤醒（notify_user_activity），本次按键
//     不再派发 UI（业界惯例：亮屏那次不触发动作）
//   CRITICAL（低电自保）→ 全部忽略，不允许唤醒
//   亮屏态(ACTIVE/DIM) 短按 → SWIPE_RIGHT（返回上一页，对齐右滑语义）
//   亮屏态 长按 → 息屏（transition_to(IDLE)）
// ========================================================
void WatchApp::handle_key_event(const InputEvent& event) {
    if (event.type != InputEventType::EV_KEY)
        return;

    const PowerState state = PowerManager::instance().get_state();

    // 息屏/深睡态：down 仅用于唤醒，该次按键被消费、不派发 UI。
    // notify_user_activity() = reset_idle_timer(USER_ACTIVITY)：
    // 重置息屏倒计时并 transition_to(ACTIVE)（CRITICAL 由其内部守卫排除）。
    if (state == PowerState::IDLE || state == PowerState::SLEEP) {
        if (event.value == 1) {
            PowerManager::instance().notify_user_activity();
            // 本次按压已在「唤醒」这个动作上消费完毕，必须在此标记
            // key_repeat_seen_，吞掉随后到达的 up 事件。否则：唤醒把状态切回
            // ACTIVE 后，同一个 up 会落进下方亮屏态的 case 0，被
            // `!key_repeat_seen_` 判成「短按」而误派发 SWIPE_RIGHT——
            // 即「按一下唤醒屏幕，屏幕亮起的同时页面莫名后退」。
            // 且该行为取决于上一次按压是短按（标志残留 false）还是长按
            // （残留 true），同一操作结果不确定。改动此行前先想清楚这条链。
            key_repeat_seen_ = true;
            key_down_tick_ = 0;
        }
        return;
    }
    if (state == PowerState::CRITICAL) {
        return; // 低电自保态：不唤醒、不派发
    }

    // 亮屏态（ACTIVE / DIM）
    switch (event.value) {
    case 1: // down
        key_down_tick_ = event.timestamp;
        key_repeat_seen_ = false;
        // 按键即交互：重置熄屏倒计时（与手势路径的 transition_to(ACTIVE) 对齐）
        PowerManager::instance().notify_user_activity();
        break;
    case 2: // repeat：第一个即"长按达成" → 息屏
        key_repeat_seen_ = true;
        PowerManager::instance().transition_to(PowerState::IDLE);
        break;
    case 0: // up：未达长按 → 短按
        if (!key_repeat_seen_) {
            PowerManager::instance().notify_user_activity();
            // 短按 = 返回上一页：复用触摸右滑的既有语义（SWIPE_RIGHT），
            // 经 ScreenNavigator 统一拦截，无需新增 UI 接口
            UI::UiManager::instance().dispatch_gesture(GestureEvent(GestureType::SWIPE_RIGHT, 0, 0));
        }
        break;
    default:
        break;
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

    // 驱动通知浮层：累计 banner 停留时长、超时收起、超时后自动顶上队列
    // 里的下一条（banner 默认 3s）。
    //
    // 刻意放在后台守护任务（40ms 周期）而不是 on_frame_render()：这是纯
    // 状态机推进，与 VSync 无关；放帧路径里会让通知计时精度被帧率抖动
    // 绑架，且息屏时帧循环被跳过、banner 超时逻辑一并停摆。
    aurora::NotificationCenter::instance().on_tick(delta_ticks);

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
        if (current_bpm > 0) {
            BleManager::instance().update_heart_rate(static_cast<uint8_t>(current_bpm));
        }
        uint8_t battery = ChargingManager::instance().get_soc();
        BleManager::instance().update_battery_level(battery);
        auroraos::ble::NimbleBridge::instance().step(0);
    }
}

// 供全局或 Lua 脚本获取当前模拟时间
void aurora_get_time(uint32_t& h, uint32_t& m) {
    WatchApp::instance().get_time(h, m);
}
