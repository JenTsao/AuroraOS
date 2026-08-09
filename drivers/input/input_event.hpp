#ifndef AURORA_INPUT_EVENT_HPP
#define AURORA_INPUT_EVENT_HPP

#include <stdint.h>

// 输入事件类型枚举
enum class InputEventType : uint8_t {
    EV_SYN = 0x00, // 同步事件：标记一组输入动作的结束
    EV_KEY = 0x01, // 按键事件：实体按键或屏幕点击状态
    EV_ABS = 0x03  // 绝对坐标事件：触摸屏 X/Y 坐标
};

// 触控动作状态
enum class TouchState : uint8_t {
    IDLE     = 0, // 未触摸/空闲
    PRESSED  = 1, // 手指按下
    MOVING   = 2, // 手指拖拽滑动
    RELEASED = 3  // 手指抬起
};

// 规范化的输入事件数据包 (在 VFS read/write 中流转的数据体)
struct InputEvent {
    uint32_t       timestamp; // 事件发生的时间戳 (系统 Tick)
    InputEventType type;      // 事件类型
    uint16_t       code;      // 具体的事件码 (如 ABS_X=0, ABS_Y=1)
    int32_t        value;     // 事件的具体数值 (坐标值或按键状态)
};

// 面向 UI 引擎的高阶触控点结构体
struct TouchPoint {
    uint16_t   x;
    uint16_t   y;
    TouchState state;
    bool       is_valid;
};

#endif
