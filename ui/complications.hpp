#ifndef AURORA_COMPLICATIONS_HPP
#define AURORA_COMPLICATIONS_HPP

#include <stdint.h>
#include "../drivers/display/framebuffer.hpp"
#include "../drivers/sensor/sensor_framework.hpp"

// 小组件数据提供者函数指针
using DataProvider = void (*)(char* out_str, int max_len);

// 小组件槽位结构
struct ComplicationSlot {
    uint16_t x, y;
    uint16_t width, height;
    ColorRGB565 text_color;
    ColorRGB565 bg_color;
    DataProvider provider;
    char last_drawn_text[16];
};

extern HeartRateSensor g_health_sensor;

// 预定义的数据源回调：心率
void hr_data_provider(char* out_str, int max_len) {
    SensorDriver* hr_sensor = &g_health_sensor;
    if (hr_sensor) {
        // hr_sensor->sample_fetch();
        SensorData val;
        hr_sensor->read(&val);

        // 简易整数转字符串
        int i = 0;
        int n = val.payload.bpm;
        out_str[i++] = 'H';
        out_str[i++] = 'R';
        out_str[i++] = ':';
        if (n == 0)
            out_str[i++] = '0';
        char tmp[8];
        int ti = 0;
        while (n > 0) {
            tmp[ti++] = (n % 10) + '0';
            n /= 10;
        }
        while (ti > 0)
            out_str[i++] = tmp[--ti];
        out_str[i] = '\0';
    }
}

// 预定义的数据源回调：计步
void step_data_provider(char* out_str, int max_len) {
    int i = 0;
    int n = SensorManager::instance().get_accel_sensor().get_steps();
    char tmp[8];
    int ti = 0;
    if (n == 0)
        tmp[ti++] = '0';
    while (n > 0) {
        tmp[ti++] = (n % 10) + '0';
        n /= 10;
    }
    while (ti > 0 && i < max_len - 5)
        out_str[i++] = tmp[--ti];
    out_str[i++] = 's';
    out_str[i++] = 't';
    out_str[i] = '\0';
}

// 表盘管理器
class WatchFaceEngine {
private:
    static constexpr int MAX_COMPLICATIONS = 4;
    ComplicationSlot slots_[MAX_COMPLICATIONS];
    int slot_count_ = 0;

    bool str_equals(const char* s1, const char* s2) {
        while (*s1 && *s2) {
            if (*s1++ != *s2++)
                return false;
        }
        return *s1 == *s2;
    }

    void str_copy(char* dest, const char* src) {
        while (*src)
            *dest++ = *src++;
        *dest = '\0';
    }

public:
    void add_complication(uint16_t x, uint16_t y, uint16_t w, uint16_t h, ColorRGB565 tc, ColorRGB565 bc,
                          DataProvider provider) {
        if (slot_count_ < MAX_COMPLICATIONS) {
            slots_[slot_count_] = {x, y, w, h, tc, bc, provider, {0}};
            slot_count_++;
        }
    }

    // 在帧渲染期调用：检查数据变化并利用脏区域刷新
    template <uint16_t W, uint16_t H> void render(FrameBuffer<W, H>& fb) {
        for (int i = 0; i < slot_count_; i++) {
            char current_text[16] = {0};
            slots_[i].provider(current_text, sizeof(current_text));

            // 数据驱动 UI：只有传感器数据真正发生变化时，才触发该区域的重绘！
            if (!str_equals(current_text, slots_[i].last_drawn_text)) {
                // 1. 擦除旧区域
                fb.fill_rect(slots_[i].x, slots_[i].y, slots_[i].width, slots_[i].height, slots_[i].bg_color);

                // 2. 在这里本应调用字模引擎绘制 current_text，仿真中用像素点示意
                fb.set_pixel(slots_[i].x + 2, slots_[i].y + 5, slots_[i].text_color);

                // 3. 记录最新状态
                str_copy(slots_[i].last_drawn_text, current_text);
            }
        }
    }
};

#endif
