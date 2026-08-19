// =============================================================================
// drivers/input/gesture_recognizer.hpp
//
// 7 态手势识别状态机引擎 (GestureRecognizer)
// 将原始 GT316 / 触控芯片采样帧序列转化为高阶手势事件
//
// 支持手势：
//   1. TAP         - 单击 (选择/触发)
//   2. DOUBLE_TAP  - 双击 (快捷操作/放大)
//   3. LONG_PRESS  - 长按 (表盘编辑/关机/设置)
//   4. SWIPE_LEFT  - 左滑 (切换下一屏/应用列表)
//   5. SWIPE_RIGHT - 右滑 (返回上一页/退出)
//   6. SWIPE_UP    - 上滑 (查看通知面板)
//   7. SWIPE_DOWN  - 下滑 (控制中心快捷栏)
//
// 算法阈值（依据智能穿戴人机工程标定）：
//   - 长按判定时间: >= 800ms (位移 <= 10px)
//   - 双击时间窗口: <= 300ms
//   - 滑动距离阈值: >= 30px
//   - 点击防抖容差: <= 10px
// =============================================================================
#ifndef AURORA_GESTURE_RECOGNIZER_HPP
#define AURORA_GESTURE_RECOGNIZER_HPP

#include <stdint.h>
#include "input_event.hpp"

// ========================================================
// 支持的 7 种手势定义
// ========================================================
enum class GestureType : uint8_t {
    NONE,
    TAP,        // 单击：选择/确认
    DOUBLE_TAP, // 双击：返回/快捷操作
    LONG_PRESS, // 长按：进入设置/重置/编辑表盘
    SWIPE_UP,   // 上滑：查看通知
    SWIPE_DOWN, // 下滑：快捷面板
    SWIPE_LEFT, // 左滑：下一个应用/下一页
    SWIPE_RIGHT // 右滑：返回上一页/退出
};

// 带有坐标的手势事件 (坐标为手势动作的起始点或点击点)
struct GestureEvent {
    GestureType type;
    uint16_t x;
    uint16_t y;
};

// 原始触控事件包 (由汇顶 GT316 等驱动传入)
struct RawTouchEvent {
    uint16_t x;
    uint16_t y;
    TouchState state;
    uint32_t timestamp; // 系统 tick (ms)
};

class GestureRecognizer {
public:
    // 手势识别算法硬核阈值
    static constexpr uint32_t THRESHOLD_LONG_PRESS_MS = 800; // 长按时间阈值 >=800ms
    static constexpr uint32_t THRESHOLD_DOUBLE_TAP_MS = 300; // 双击时间窗口 <=300ms
    static constexpr uint16_t THRESHOLD_SWIPE_PX = 30;       // 滑动距离判定阈值 >=30px
    static constexpr uint16_t THRESHOLD_TAP_MAX_PX = 10;     // 点击防抖位移容差 <=10px

private:
    TouchState current_state_;
    uint16_t start_x_;
    uint16_t start_y_;
    uint16_t last_x_;
    uint16_t last_y_;
    uint32_t start_time_;

    // 长按实时触发标记 (防止松手时二次误触发 TAP)
    bool long_press_fired_;

    // 双击检测记忆
    uint32_t last_tap_time_;
    uint16_t last_tap_x_;
    uint16_t last_tap_y_;
    bool is_tracking_double_tap_;

    // 微型内联绝对值计算
    static inline int32_t abs_diff(uint16_t a, uint16_t b) {
        return (a > b) ? (a - b) : (b - a);
    }

public:
    GestureRecognizer()
        : current_state_(TouchState::IDLE),
          start_x_(0),
          start_y_(0),
          last_x_(0),
          last_y_(0),
          start_time_(0),
          long_press_fired_(false),
          last_tap_time_(0),
          last_tap_x_(0),
          last_tap_y_(0),
          is_tracking_double_tap_(false) {}

    // 重置所有内部状态
    void reset() {
        current_state_ = TouchState::IDLE;
        start_x_ = 0;
        start_y_ = 0;
        last_x_ = 0;
        last_y_ = 0;
        start_time_ = 0;
        long_press_fired_ = false;
        is_tracking_double_tap_ = false;
        last_tap_time_ = 0;
    }

    // ========================================================
    // 核心状态机引擎：解析连续的触控帧数据
    // ========================================================
    GestureEvent process_event(const RawTouchEvent& event) {
        GestureEvent result = {GestureType::NONE, event.x, event.y};

        switch (event.state) {
        case TouchState::IDLE:
            if (current_state_ == TouchState::RELEASED) {
                current_state_ = TouchState::IDLE;
            }
            break;

        case TouchState::PRESSED:
            current_state_ = TouchState::PRESSED;
            start_x_ = event.x;
            start_y_ = event.y;
            last_x_ = event.x;
            last_y_ = event.y;
            start_time_ = event.timestamp;
            long_press_fired_ = false;
            break;

        case TouchState::MOVING:
            if (current_state_ == TouchState::PRESSED || current_state_ == TouchState::MOVING) {
                current_state_ = TouchState::MOVING;
                last_x_ = event.x;
                last_y_ = event.y;

                uint16_t abs_dx = abs_diff(event.x, start_x_);
                uint16_t abs_dy = abs_diff(event.y, start_y_);
                uint32_t duration = event.timestamp - start_time_;

                // 实时长按判定：手指未大范围移动且按压时间超过阈值时立即触发
                if (!long_press_fired_ && duration >= THRESHOLD_LONG_PRESS_MS &&
                    abs_dx <= THRESHOLD_TAP_MAX_PX && abs_dy <= THRESHOLD_TAP_MAX_PX) {
                    result.type = GestureType::LONG_PRESS;
                    result.x = start_x_;
                    result.y = start_y_;
                    long_press_fired_ = true;
                    is_tracking_double_tap_ = false; // 打断双击判定
                }
            }
            break;

        case TouchState::RELEASED:
            if (current_state_ != TouchState::IDLE) {
                uint32_t duration = event.timestamp - start_time_;
                int32_t dx = static_cast<int32_t>(event.x) - static_cast<int32_t>(start_x_);
                int32_t dy = static_cast<int32_t>(event.y) - static_cast<int32_t>(start_y_);
                uint16_t abs_dx = abs_diff(event.x, start_x_);
                uint16_t abs_dy = abs_diff(event.y, start_y_);

                // 1. 如果此前已经触发过长按，松手时不再产生任何额外点击事件
                if (long_press_fired_) {
                    long_press_fired_ = false;
                    current_state_ = TouchState::IDLE;
                    break;
                }

                // 2. 滑动判定 (位移 > 30px)
                if (abs_dx >= THRESHOLD_SWIPE_PX || abs_dy >= THRESHOLD_SWIPE_PX) {
                    if (abs_dx >= abs_dy) {
                        result.type = (dx > 0) ? GestureType::SWIPE_RIGHT : GestureType::SWIPE_LEFT;
                    } else {
                        result.type = (dy > 0) ? GestureType::SWIPE_DOWN : GestureType::SWIPE_UP;
                    }
                    result.x = start_x_;
                    result.y = start_y_;
                    is_tracking_double_tap_ = false; // 打断双击
                }
                // 3. 点击系判定 (位移 <= 10px)
                else if (abs_dx <= THRESHOLD_TAP_MAX_PX && abs_dy <= THRESHOLD_TAP_MAX_PX) {
                    if (duration >= THRESHOLD_LONG_PRESS_MS) {
                        result.type = GestureType::LONG_PRESS;
                        result.x = start_x_;
                        result.y = start_y_;
                        is_tracking_double_tap_ = false;
                    } else {
                        // 短按判定 (单/双击)
                        uint16_t tap_dist_x = abs_diff(start_x_, last_tap_x_);
                        uint16_t tap_dist_y = abs_diff(start_y_, last_tap_y_);
                        uint32_t time_since_last_tap = event.timestamp - last_tap_time_;

                        if (is_tracking_double_tap_ &&
                            (time_since_last_tap <= THRESHOLD_DOUBLE_TAP_MS) &&
                            (tap_dist_x <= THRESHOLD_TAP_MAX_PX) &&
                            (tap_dist_y <= THRESHOLD_TAP_MAX_PX)) {
                            // 300ms 内同一区域两次点击 -> 双击！
                            result.type = GestureType::DOUBLE_TAP;
                            result.x = start_x_;
                            result.y = start_y_;
                            is_tracking_double_tap_ = false;
                        } else {
                            // 单击 -> 启动双击追踪窗口
                            result.type = GestureType::TAP;
                            result.x = start_x_;
                            result.y = start_y_;
                            last_tap_time_ = event.timestamp;
                            last_tap_x_ = start_x_;
                            last_tap_y_ = start_y_;
                            is_tracking_double_tap_ = true;
                        }
                    }
                }

                current_state_ = TouchState::IDLE;
            }
            break;
        }

        return result;
    }

    // 辅助转换接口：直接接收 TouchPoint 并处理
    GestureEvent feed_touch_point(const TouchPoint& point, uint32_t timestamp_ms) {
        if (!point.is_valid && point.state == TouchState::IDLE && current_state_ == TouchState::IDLE) {
            return {GestureType::NONE, 0, 0};
        }
        RawTouchEvent raw = {point.x, point.y, point.state, timestamp_ms};
        return process_event(raw);
    }

    TouchState get_current_state() const {
        return current_state_;
    }
};

#endif // AURORA_GESTURE_RECOGNIZER_HPP
