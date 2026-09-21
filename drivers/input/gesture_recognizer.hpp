// =============================================================================
// drivers/input/gesture_recognizer.hpp
//
// 7 态及扩展手势识别状态机引擎 (GestureRecognizer)
// 将原始 GT316 / 触控芯片采样帧序列转化为高阶手势事件
//
// 支持手势：
//   1. TAP         - 单击 (选择/触发)
//   2. DOUBLE_TAP  - 双击 (快捷操作/放大)
//   3. TRIPLE_TAP  - 三击 (辅助功能/快速复位)
//   4. LONG_PRESS  - 长按 (表盘编辑/关机/设置)
//   5. SWIPE_LEFT  - 左滑 (切换下一屏/应用列表)
//   6. SWIPE_RIGHT - 右滑 (返回上一页/退出)
//   7. SWIPE_UP    - 上滑 (查看通知面板)
//   8. SWIPE_DOWN  - 下滑 (控制中心快捷栏)
//   9. DRAG_START  - 拖拽开始 (位移首次越过防抖阈值)
//  10. DRAG_MOVE   - 连续拖拽移动 (带有瞬时增量 delta_x/delta_y)
//  11. DRAG_END    - 拖拽结束 (松手)
//  12. TOUCH_DOWN  - 原始按下瞬态 (用于高亮与捕获)
//  13. TOUCH_UP    - 原始抬起瞬态
//  14. CANCEL      - 手势取消/被打断
// =============================================================================
#ifndef AURORA_GESTURE_RECOGNIZER_HPP
#define AURORA_GESTURE_RECOGNIZER_HPP

#include <stdint.h>
#include "input_event.hpp"

// ========================================================
// 支持的手势类型定义
// ========================================================
enum class GestureType : uint8_t {
    NONE = 0,
    TAP,         // 单击：选择/确认
    DOUBLE_TAP,  // 双击：快捷操作/放大
    TRIPLE_TAP,  // 三击：快捷功能
    LONG_PRESS,  // 长按：进入设置/重置/编辑表盘
    SWIPE_UP,    // 上滑：查看通知
    SWIPE_DOWN,  // 下滑：快捷面板
    SWIPE_LEFT,  // 左滑：下一个应用/下一页
    SWIPE_RIGHT, // 右滑：返回上一页/退出
    DRAG_START,  // 拖拽开始
    DRAG_MOVE,   // 拖拽移动
    DRAG_END,    // 拖拽释放
    TOUCH_DOWN,  // 触控按下瞬态
    TOUCH_UP,    // 触控抬起瞬态
    CANCEL       // 手势打断/取消
};

// 带有坐标、位移增量、持续时间与边缘标志的高阶手势事件
struct GestureEvent {
    GestureType type;
    uint16_t x;
    uint16_t y;
    int16_t delta_x;
    int16_t delta_y;
    uint32_t duration_ms;
    bool is_edge;

    // 默认构造与向下兼容的 3 参数/全参数构造函数
    GestureEvent(GestureType t = GestureType::NONE, uint16_t px = 0, uint16_t py = 0,
                 int16_t dx = 0, int16_t dy = 0, uint32_t dur = 0, bool edge = false)
        : type(t), x(px), y(py), delta_x(dx), delta_y(dy), duration_ms(dur), is_edge(edge) {}
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
    // 手势识别算法硬核默认阈值（依据智能穿戴人机工程标定）
    static constexpr uint32_t THRESHOLD_LONG_PRESS_MS = 800; // 长按时间阈值 >=800ms
    static constexpr uint32_t THRESHOLD_DOUBLE_TAP_MS = 300; // 多击时间窗口 <=300ms
    static constexpr uint16_t THRESHOLD_SWIPE_PX = 30;       // 滑动距离判定阈值 >=30px
    static constexpr uint16_t THRESHOLD_TAP_MAX_PX = 10;     // 点击防抖位移容差 <=10px
    static constexpr uint16_t THRESHOLD_EDGE_ZONE_PX = 20;   // 边缘滑动判定区域 <=20px

private:
    TouchState current_state_;
    uint16_t start_x_;
    uint16_t start_y_;
    uint16_t last_x_;
    uint16_t last_y_;
    uint32_t start_time_;

    // 长按实时触发标记 (防止松手时二次误触发 TAP)
    bool long_press_fired_;

    // 连续拖拽状态跟踪
    bool is_dragging_;

    // 多击检测记忆
    uint32_t last_tap_time_;
    uint16_t last_tap_x_;
    uint16_t last_tap_y_;
    bool is_tracking_double_tap_;
    uint8_t tap_count_;

    // 动态可调阈值
    uint32_t threshold_long_press_ms_;
    uint32_t threshold_double_tap_ms_;
    uint16_t threshold_swipe_px_;
    uint16_t threshold_tap_max_px_;
    uint16_t threshold_edge_zone_px_;

    // 屏幕物理尺寸（用于边缘手势判定）
    uint16_t display_width_;
    uint16_t display_height_;

    // 模式开关
    bool drag_events_enabled_;
    bool raw_touch_events_enabled_;

    // 微型内联绝对值计算
    static inline int32_t abs_diff(uint16_t a, uint16_t b) noexcept {
        return (a > b) ? (a - b) : (b - a);
    }

    bool check_is_edge(uint16_t x, uint16_t y) const noexcept {
        return (x <= threshold_edge_zone_px_ ||
                x >= (display_width_ - threshold_edge_zone_px_) ||
                y <= threshold_edge_zone_px_ ||
                y >= (display_height_ - threshold_edge_zone_px_));
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
          is_dragging_(false),
          last_tap_time_(0),
          last_tap_x_(0),
          last_tap_y_(0),
          is_tracking_double_tap_(false),
          tap_count_(0),
          threshold_long_press_ms_(THRESHOLD_LONG_PRESS_MS),
          threshold_double_tap_ms_(THRESHOLD_DOUBLE_TAP_MS),
          threshold_swipe_px_(THRESHOLD_SWIPE_PX),
          threshold_tap_max_px_(THRESHOLD_TAP_MAX_PX),
          threshold_edge_zone_px_(THRESHOLD_EDGE_ZONE_PX),
          display_width_(192),
          display_height_(490),
          drag_events_enabled_(false),
          raw_touch_events_enabled_(false) {}

    // 重置所有内部状态机
    void reset() noexcept {
        current_state_ = TouchState::IDLE;
        start_x_ = 0;
        start_y_ = 0;
        last_x_ = 0;
        last_y_ = 0;
        start_time_ = 0;
        long_press_fired_ = false;
        is_dragging_ = false;
        is_tracking_double_tap_ = false;
        last_tap_time_ = 0;
        tap_count_ = 0;
    }

    // ========================================================
    // 阈值与参数配置 API
    // ========================================================
    void set_long_press_threshold_ms(uint32_t ms) noexcept {
        threshold_long_press_ms_ = ms;
    }

    uint32_t get_long_press_threshold_ms() const noexcept {
        return threshold_long_press_ms_;
    }

    void set_double_tap_threshold_ms(uint32_t ms) noexcept {
        threshold_double_tap_ms_ = ms;
    }

    uint32_t get_double_tap_threshold_ms() const noexcept {
        return threshold_double_tap_ms_;
    }

    void set_swipe_threshold_px(uint16_t px) noexcept {
        threshold_swipe_px_ = px;
    }

    uint16_t get_swipe_threshold_px() const noexcept {
        return threshold_swipe_px_;
    }

    void set_tap_tolerance_px(uint16_t px) noexcept {
        threshold_tap_max_px_ = px;
    }

    uint16_t get_tap_tolerance_px() const noexcept {
        return threshold_tap_max_px_;
    }

    void set_edge_zone_px(uint16_t px) noexcept {
        threshold_edge_zone_px_ = px;
    }

    uint16_t get_edge_zone_px() const noexcept {
        return threshold_edge_zone_px_;
    }

    void set_display_dimensions(uint16_t w, uint16_t h) noexcept {
        display_width_ = w;
        display_height_ = h;
    }

    void set_drag_events_enabled(bool enabled) noexcept {
        drag_events_enabled_ = enabled;
    }

    bool is_drag_events_enabled() const noexcept {
        return drag_events_enabled_;
    }

    void set_raw_touch_events_enabled(bool enabled) noexcept {
        raw_touch_events_enabled_ = enabled;
    }

    bool is_raw_touch_events_enabled() const noexcept {
        return raw_touch_events_enabled_;
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
            is_dragging_ = false;

            if (raw_touch_events_enabled_) {
                result.type = GestureType::TOUCH_DOWN;
                result.x = event.x;
                result.y = event.y;
                result.is_edge = check_is_edge(event.x, event.y);
            }
            break;

        case TouchState::MOVING:
            if (current_state_ == TouchState::PRESSED || current_state_ == TouchState::MOVING) {
                current_state_ = TouchState::MOVING;
                const int32_t dx = static_cast<int32_t>(event.x) - static_cast<int32_t>(last_x_);
                const int32_t dy = static_cast<int32_t>(event.y) - static_cast<int32_t>(last_y_);
                const uint16_t abs_total_dx = abs_diff(event.x, start_x_);
                const uint16_t abs_total_dy = abs_diff(event.y, start_y_);
                const uint32_t duration = event.timestamp - start_time_;
                last_x_ = event.x;
                last_y_ = event.y;

                // 1. 实时长按判定：手指未发生大范围位移且按压保持超过阈值时立即触发
                if (!long_press_fired_ && duration >= threshold_long_press_ms_ &&
                    abs_total_dx <= threshold_tap_max_px_ && abs_total_dy <= threshold_tap_max_px_) {
                    result.type = GestureType::LONG_PRESS;
                    result.x = start_x_;
                    result.y = start_y_;
                    result.duration_ms = duration;
                    result.is_edge = check_is_edge(start_x_, start_y_);
                    long_press_fired_ = true;
                    is_tracking_double_tap_ = false;
                    tap_count_ = 0;
                    return result;
                }

                // 2. 连续拖拽流判定 (若开启 drag_events_enabled_)
                if (drag_events_enabled_ && !long_press_fired_) {
                    if (!is_dragging_) {
                        if (abs_total_dx > threshold_tap_max_px_ || abs_total_dy > threshold_tap_max_px_) {
                            is_dragging_ = true;
                            is_tracking_double_tap_ = false;
                            tap_count_ = 0;
                            result.type = GestureType::DRAG_START;
                            result.x = event.x;
                            result.y = event.y;
                            result.delta_x = static_cast<int16_t>(dx);
                            result.delta_y = static_cast<int16_t>(dy);
                            result.duration_ms = duration;
                            result.is_edge = check_is_edge(start_x_, start_y_);
                            return result;
                        }
                    } else {
                        result.type = GestureType::DRAG_MOVE;
                        result.x = event.x;
                        result.y = event.y;
                        result.delta_x = static_cast<int16_t>(dx);
                        result.delta_y = static_cast<int16_t>(dy);
                        result.duration_ms = duration;
                        result.is_edge = check_is_edge(start_x_, start_y_);
                        return result;
                    }
                }
            }
            break;

        case TouchState::RELEASED:
            if (current_state_ != TouchState::IDLE) {
                const uint32_t duration = event.timestamp - start_time_;
                const int32_t dx = static_cast<int32_t>(event.x) - static_cast<int32_t>(start_x_);
                const int32_t dy = static_cast<int32_t>(event.y) - static_cast<int32_t>(start_y_);
                const uint16_t abs_dx = abs_diff(event.x, start_x_);
                const uint16_t abs_dy = abs_diff(event.y, start_y_);
                const bool from_edge = check_is_edge(start_x_, start_y_);

                // 1. 如果此前已经触发过长按，松手时不再产生额外点击事件
                if (long_press_fired_) {
                    long_press_fired_ = false;
                    current_state_ = TouchState::IDLE;
                    if (raw_touch_events_enabled_) {
                        result.type = GestureType::TOUCH_UP;
                        result.x = event.x;
                        result.y = event.y;
                        result.duration_ms = duration;
                    }
                    break;
                }

                // 2. 如果正在拖拽，松手时触发 DRAG_END
                if (is_dragging_) {
                    is_dragging_ = false;
                    current_state_ = TouchState::IDLE;
                    result.type = GestureType::DRAG_END;
                    result.x = event.x;
                    result.y = event.y;
                    result.delta_x = static_cast<int16_t>(dx);
                    result.delta_y = static_cast<int16_t>(dy);
                    result.duration_ms = duration;
                    result.is_edge = from_edge;
                    break;
                }

                // 3. 滑动判定 (位移 >= threshold_swipe_px_)
                if (abs_dx >= threshold_swipe_px_ || abs_dy >= threshold_swipe_px_) {
                    if (abs_dx >= abs_dy) {
                        result.type = (dx > 0) ? GestureType::SWIPE_RIGHT : GestureType::SWIPE_LEFT;
                    } else {
                        result.type = (dy > 0) ? GestureType::SWIPE_DOWN : GestureType::SWIPE_UP;
                    }
                    result.x = start_x_;
                    result.y = start_y_;
                    result.delta_x = static_cast<int16_t>(dx);
                    result.delta_y = static_cast<int16_t>(dy);
                    result.duration_ms = duration;
                    result.is_edge = from_edge;
                    is_tracking_double_tap_ = false;
                    tap_count_ = 0;
                }
                // 4. 点击系判定 (位移 <= threshold_tap_max_px_)
                else if (abs_dx <= threshold_tap_max_px_ && abs_dy <= threshold_tap_max_px_) {
                    if (duration >= threshold_long_press_ms_) {
                        result.type = GestureType::LONG_PRESS;
                        result.x = start_x_;
                        result.y = start_y_;
                        result.duration_ms = duration;
                        result.is_edge = from_edge;
                        is_tracking_double_tap_ = false;
                        tap_count_ = 0;
                    } else {
                        // 短按判定 (单击 / 双击 / 三击)
                        const uint16_t tap_dist_x = abs_diff(start_x_, last_tap_x_);
                        const uint16_t tap_dist_y = abs_diff(start_y_, last_tap_y_);
                        const uint32_t time_since_last_tap = event.timestamp - last_tap_time_;

                        if (is_tracking_double_tap_ &&
                            (time_since_last_tap <= threshold_double_tap_ms_) &&
                            (tap_dist_x <= threshold_tap_max_px_) &&
                            (tap_dist_y <= threshold_tap_max_px_)) {
                            if (tap_count_ == 1) {
                                result.type = GestureType::DOUBLE_TAP;
                                tap_count_ = 2;
                            } else if (tap_count_ == 2) {
                                result.type = GestureType::TRIPLE_TAP;
                                tap_count_ = 0;
                                is_tracking_double_tap_ = false;
                            }
                        } else {
                            result.type = GestureType::TAP;
                            tap_count_ = 1;
                            is_tracking_double_tap_ = true;
                        }
                        result.x = start_x_;
                        result.y = start_y_;
                        result.duration_ms = duration;
                        result.is_edge = from_edge;
                        last_tap_time_ = event.timestamp;
                        last_tap_x_ = start_x_;
                        last_tap_y_ = start_y_;
                    }
                } else if (raw_touch_events_enabled_) {
                    result.type = GestureType::TOUCH_UP;
                    result.x = event.x;
                    result.y = event.y;
                    result.duration_ms = duration;
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

    TouchState get_current_state() const noexcept {
        return current_state_;
    }
};

#endif // AURORA_GESTURE_RECOGNIZER_HPP
