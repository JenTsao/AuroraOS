#ifndef AURORA_GESTURE_RECOGNIZER_HPP
#define AURORA_GESTURE_RECOGNIZER_HPP

#include <stdint.h>
#include "input_event.hpp"

// ========================================================
// 支持的 7 种手势定义
// ========================================================
enum class GestureType : uint8_t {
    NONE,
    TAP,            // 单击：选择/确认
    DOUBLE_TAP,     // 双击：返回/快捷操作
    LONG_PRESS,     // 长按：进入设置/重置
    SWIPE_UP,       // 上滑：查看通知
    SWIPE_DOWN,     // 下滑：快捷面板
    SWIPE_LEFT,     // 左滑：下一个应用
    SWIPE_RIGHT     // 右滑：返回上一页
};

// 带有坐标的手势事件
struct GestureEvent {
    GestureType type;
    uint16_t x;
    uint16_t y;
};

// 原始触控事件包 (由汇顶 GT316 驱动传入)
struct RawTouchEvent {
    uint16_t   x;
    uint16_t   y;
    TouchState state;
    uint32_t   timestamp; // 系统 tick (ms)
};

class GestureRecognizer {
private:
    TouchState current_state_;
    uint16_t   start_x_;
    uint16_t   start_y_;
    uint32_t   start_time_;
    
    // 双击检测记忆
    uint32_t   last_tap_time_;
    bool       is_tracking_double_tap_;

    // ========================================================
    // 手势识别算法硬核阈值
    // ========================================================
    static constexpr uint32_t THRESHOLD_LONG_PRESS_MS = 800; // 长按时间阈值 >800ms
    static constexpr uint32_t THRESHOLD_DOUBLE_TAP_MS = 300; // 双击时间窗口 <300ms
    static constexpr uint16_t THRESHOLD_SWIPE_PX      = 30;  // 滑动距离判定阈值 >30px
    static constexpr uint16_t THRESHOLD_TAP_MAX_PX    = 10;  // 点击防抖位移容差 <10px

    // 微型内联绝对值计算
    inline int32_t abs_diff(uint16_t a, uint16_t b) {
        return (a > b) ? (a - b) : (b - a);
    }

public:
    GestureRecognizer() : 
        current_state_(TouchState::IDLE), 
        start_x_(0), start_y_(0), start_time_(0), 
        last_tap_time_(0), is_tracking_double_tap_(false) {}

    // ========================================================
    // 核心状态机引擎：解析连续的触控帧数据
    // ========================================================
    GestureEvent process_event(const RawTouchEvent& event) {
        GestureEvent result = {GestureType::NONE, event.x, event.y};

        switch (event.state) {
            case TouchState::IDLE:
                break;
            case TouchState::PRESSED:
                if (current_state_ == TouchState::IDLE || current_state_ == TouchState::RELEASED) {
                    current_state_ = TouchState::PRESSED;
                    start_x_    = event.x;
                    start_y_    = event.y;
                    start_time_ = event.timestamp;
                }
                break;

            case TouchState::MOVING:
                if (current_state_ == TouchState::PRESSED || current_state_ == TouchState::MOVING) {
                    current_state_ = TouchState::MOVING;
                    
                    // 可选：在此处增加实时滑动的阻尼计算 or 拖拽跟随逻辑
                }
                break;

            case TouchState::RELEASED:
                if (current_state_ != TouchState::RELEASED) {
                    uint32_t duration = event.timestamp - start_time_;
                    int32_t dx = event.x - start_x_;
                    int32_t dy = event.y - start_y_;
                    uint16_t abs_dx = abs_diff(event.x, start_x_);
                    uint16_t abs_dy = abs_diff(event.y, start_y_);

                    // 1. 判断是否为滑动 (距离 > 30px)
                    if (abs_dx > THRESHOLD_SWIPE_PX || abs_dy > THRESHOLD_SWIPE_PX) {
                        if (abs_dx > abs_dy) {
                            result.type = (dx > 0) ? GestureType::SWIPE_RIGHT : GestureType::SWIPE_LEFT;
                        } else {
                            result.type = (dy > 0) ? GestureType::SWIPE_DOWN : GestureType::SWIPE_UP;
                        }
                        is_tracking_double_tap_ = false; // 打断双击判定
                    } 
                    // 2. 距离极小，判定为点击系动作 (位移 < 10px)
                    else if (abs_dx <= THRESHOLD_TAP_MAX_PX && abs_dy <= THRESHOLD_TAP_MAX_PX) {
                        
                        // 长按判定 (时间 > 800ms)
                        if (duration > THRESHOLD_LONG_PRESS_MS) {
                            result.type = GestureType::LONG_PRESS;
                            is_tracking_double_tap_ = false;
                        } 
                        // 短按判定 (包含单双击分支)
                        else {
                            if (is_tracking_double_tap_ && (event.timestamp - last_tap_time_ <= THRESHOLD_DOUBLE_TAP_MS)) {
                                // 两次 Tap 间隔在 300ms 内，触发双击！
                                result.type = GestureType::DOUBLE_TAP;
                                is_tracking_double_tap_ = false;
                            } else {
                                // 触发首次单击，开始追踪双击窗口
                                result.type = GestureType::TAP;
                                last_tap_time_ = event.timestamp;
                                is_tracking_double_tap_ = true;
                            }
                        }
                    }

                    // 状态重置归零
                    current_state_ = TouchState::IDLE;
                }
                break;
                
                break;
        }

        // 滑动或者长按等动作可能需要起始坐标而不是松手时的游离坐标，这里以最终松手时的坐标为准（简化）
        // 但对于长按和点击，x,y 取的是 event.x, event.y
        return result;
    }
    
    TouchState get_current_state() const { return current_state_; }
};

#endif // AURORA_GESTURE_RECOGNIZER_HPP
