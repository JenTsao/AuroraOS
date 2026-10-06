// =============================================================================
// tests/unit/test_button_driver.cpp
//
// 物理按键轮询驱动（ButtonDriver）全链路单元测试
// 测试策略：FakeGpioHal 注入电平 + 固定时间戳手动推进，确定性测试。
// 同一场景同时覆盖"注入电平"与"fake HAL 电平"两条输入路径。
// =============================================================================
#include <gtest/gtest.h>
#include <vector>

#include "../../hal/gpio_hal.hpp"
#include "../../drivers/input/button_driver.hpp"
#include "../../drivers/input/input_event.hpp"

// =============================================================================
// 模拟 GPIO HAL：可设定电平、记录 init_pin 参数
// =============================================================================
class FakeGpioHal : public auroraos::hal::IGpioHal {
public:
    bool level = true; // 空闲高（配合低有效按键 + 内部上拉）
    int init_pin_calls = 0;
    uint32_t last_pin = 0;
    auroraos::hal::GpioMode last_mode = auroraos::hal::GpioMode::Analog;
    auroraos::hal::GpioPull last_pull = auroraos::hal::GpioPull::None;

    void init_pin(uint32_t pin, auroraos::hal::GpioMode mode, auroraos::hal::GpioPull pull) override {
        init_pin_calls++;
        last_pin = pin;
        last_mode = mode;
        last_pull = pull;
    }

    void set_pin(uint32_t pin, bool high) override {
        (void)pin;
        (void)high;
    }

    bool read_pin(uint32_t pin) override {
        (void)pin;
        return level;
    }

    void toggle_pin(uint32_t pin) override {
        (void)pin;
        level = !level;
    }
};

// 收集驱动当前队列中的全部事件
static std::vector<InputEvent> drain(ButtonDriver& d) {
    std::vector<InputEvent> events;
    InputEvent ev;
    while (d.pop_event(&ev)) {
        events.push_back(ev);
    }
    return events;
}

// =============================================================================
// 1. 干净按下：消抖确认后产出 down 事件，字段正确
// =============================================================================
TEST(ButtonDriverTest, FreshPressEmitsKeyDown) {
    ButtonDriver d;
    d.inject_press();
    d.poll(1000);
    d.poll(1031); // 跨过 30ms 消抖窗口

    auto events = drain(d);
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].type, InputEventType::EV_KEY);
    EXPECT_EQ(events[0].code, static_cast<uint16_t>(KeyCode::KEY_POWER));
    EXPECT_EQ(events[0].value, 1);
    EXPECT_EQ(events[0].timestamp, 1031u);
}

// =============================================================================
// 2. 消抖窗口边界：t+29 无事件，t+30 有（>= 判定）
// =============================================================================
TEST(ButtonDriverTest, DebounceWindowNotElapsed) {
    ButtonDriver d;
    d.inject_press();

    d.poll(1000);
    EXPECT_FALSE(d.pop_event(nullptr));

    d.poll(1029);
    EXPECT_FALSE(d.pop_event(nullptr));

    d.poll(1030);
    auto events = drain(d);
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].value, 1);
}

// =============================================================================
// 3. 消抖期内电平反复翻转：全程无事件
// =============================================================================
TEST(ButtonDriverTest, DebounceRejectsBounce) {
    ButtonDriver d;
    d.inject_press();
    d.poll(1000);

    d.inject_release();
    d.poll(1010);
    d.inject_press();
    d.poll(1020);
    d.inject_release();
    d.poll(1025);

    EXPECT_EQ(drain(d).size(), 0u);
}

// =============================================================================
// 4. 进入消抖后回弹回原稳态并稳定：零事件（防误发回归）
//    （否则会在 debounce 到期时误发一次无效 up/down）
// =============================================================================
TEST(ButtonDriverTest, BounceBackToOriginalEmitsNothing) {
    ButtonDriver d;
    d.inject_press();
    d.poll(1000);

    d.inject_release(); // 回弹回空闲
    d.poll(1010);
    d.poll(1060); // 远超消抖窗口，但稳态未翻转

    EXPECT_EQ(drain(d).size(), 0u);
}

// =============================================================================
// 5. 释放：消抖确认后产出 up 事件
// =============================================================================
TEST(ButtonDriverTest, ReleaseEmitsKeyUp) {
    ButtonDriver d;
    d.inject_press();
    d.poll(1000);
    d.poll(1031);
    EXPECT_EQ(drain(d).size(), 1u); // 先消费 down，隔离本次验证对象

    d.inject_release();
    d.poll(1031);
    d.poll(1062);

    auto events = drain(d);
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].value, 0);
    EXPECT_EQ(events[0].timestamp, 1062u);
}

// =============================================================================
// 6. 完整短按：down, up 依序出队（FIFO）
// =============================================================================
TEST(ButtonDriverTest, ShortPressSequenceOrder) {
    ButtonDriver d;
    d.inject_press();
    d.poll(1000);
    d.poll(1031);
    d.inject_release();
    d.poll(1031);
    d.poll(1062);

    auto events = drain(d);
    ASSERT_EQ(events.size(), 2u);
    EXPECT_EQ(events[0].value, 1);
    EXPECT_EQ(events[1].value, 0);
    EXPECT_EQ(events[1].timestamp, 1062u);
}

// =============================================================================
// 7. 长按（repeat 禁用）：t+800 恰好一个 value=2，之后不再发
// =============================================================================
TEST(ButtonDriverTest, LongPressEmitsSingleRepeat) {
    ButtonDriver d;
    d.inject_press();
    d.poll(1000);
    d.poll(1031);
    drain(d);

    d.poll(1831); // press_start=1031, +800
    auto events = drain(d);
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].value, 2);

    d.poll(2031); // repeat 禁用，继续按住无新事件
    EXPECT_EQ(drain(d).size(), 0u);
}

// =============================================================================
// 8. 长按后抬起：序列 = down, repeat, up，无额外事件（long_press_fired 抑制）
// =============================================================================
TEST(ButtonDriverTest, LongPressThenRelease) {
    ButtonDriver d;
    d.inject_press();
    d.poll(1000);
    d.poll(1031);
    d.poll(1831); // 长按达成
    drain(d);

    d.inject_release();
    d.poll(1831);
    d.poll(1862);

    auto events = drain(d);
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].value, 0);
}

// =============================================================================
// 9. 启用长按重复：t+800 首个 repeat，之后每 500ms 补发
// =============================================================================
TEST(ButtonDriverTest, RepeatEventsWhenEnabled) {
    ButtonDriver d;
    d.set_repeat_interval_ms(500);
    d.inject_press();
    d.poll(1000);
    d.poll(1031);
    drain(d);

    d.poll(1831); // 首个 repeat（长按达成）
    d.poll(2331); // +500
    d.poll(2832); // +501

    auto events = drain(d);
    ASSERT_EQ(events.size(), 3u);
    EXPECT_EQ(events[0].value, 2);
    EXPECT_EQ(events[0].timestamp, 1831u);
    EXPECT_EQ(events[1].value, 2);
    EXPECT_EQ(events[1].timestamp, 2331u);
    EXPECT_EQ(events[2].value, 2);
    EXPECT_EQ(events[2].timestamp, 2832u);
}

// =============================================================================
// 10. 快速连按：两次完整 press/release 依序 down,up,down,up
// =============================================================================
TEST(ButtonDriverTest, QuickDoublePress) {
    ButtonDriver d;
    for (int i = 0; i < 2; ++i) {
        uint32_t t = 1000 + static_cast<uint32_t>(i) * 200;
        d.inject_press();
        d.poll(t);
        d.poll(t + 31);
        d.inject_release();
        d.poll(t + 31);
        d.poll(t + 62);
    }

    auto events = drain(d);
    ASSERT_EQ(events.size(), 4u);
    EXPECT_EQ(events[0].value, 1);
    EXPECT_EQ(events[1].value, 0);
    EXPECT_EQ(events[2].value, 1);
    EXPECT_EQ(events[3].value, 0);
}

// =============================================================================
// 11. 队列满：丢最旧、计数可观测、队列后续仍可用
// =============================================================================
TEST(ButtonDriverTest, QueueOverflowDropsOldest) {
    ButtonDriver d;
    uint32_t t = 1000;
    // 5 次完整短按 = 10 个事件 > 容量 8
    for (int i = 0; i < 5; ++i) {
        d.inject_press();
        d.poll(t);
        d.poll(t + 31);
        d.inject_release();
        d.poll(t + 31);
        d.poll(t + 62);
        t += 100;
    }

    EXPECT_EQ(d.dropped_events(), 2u);

    auto events = drain(d);
    ASSERT_EQ(events.size(), 8u);
    // 最旧的 2 个（第一次按压的 down,up）被挤掉，队头是第二次按压的 down
    EXPECT_EQ(events[0].value, 1);
    EXPECT_EQ(events[0].timestamp, 1100u + 31u);
    EXPECT_EQ(events.back().value, 0);

    // 队列清空后仍可正常接收新事件
    d.inject_press();
    d.poll(t);
    d.poll(t + 31);
    auto after = drain(d);
    ASSERT_EQ(after.size(), 1u);
    EXPECT_EQ(after[0].value, 1);
}

// =============================================================================
// 12. 空闲电平：任意长时间轮询无事件
// =============================================================================
TEST(ButtonDriverTest, IdleLevelNoEvents) {
    FakeGpioHal gpio; // level=true，低有效按键 => 空闲
    ButtonDriver d;
    d.configure(&gpio, 6, /*active_high=*/false);
    d.open();

    for (uint32_t t = 0; t < 10000; t += 10) {
        d.poll(t);
    }
    EXPECT_EQ(drain(d).size(), 0u);
}

// =============================================================================
// 13. 未 configure：poll 任意次数无事件、无崩溃（gpio_==nullptr 时
//     sample_level 维持稳态、永不翻转，故永不产事件）
// =============================================================================
TEST(ButtonDriverTest, UnconfiguredSafety) {
    ButtonDriver d;
    for (uint32_t t = 0; t < 5000; t += 10) {
        d.poll(t);
    }
    EXPECT_EQ(drain(d).size(), 0u);
}

// =============================================================================
// 14. open() 引脚初始化：低有效=Input+PullUp（且只调一次）；高有效=PullDown
// =============================================================================
TEST(ButtonDriverTest, OpenConfiguresPull) {
    {
        FakeGpioHal gpio;
        ButtonDriver d;
        d.configure(&gpio, 6, /*active_high=*/false);
        EXPECT_EQ(d.open(), 0);
        EXPECT_EQ(gpio.init_pin_calls, 1);
        EXPECT_EQ(gpio.last_pin, 6u);
        EXPECT_EQ(gpio.last_mode, auroraos::hal::GpioMode::Input);
        EXPECT_EQ(gpio.last_pull, auroraos::hal::GpioPull::PullUp);
    }
    {
        FakeGpioHal gpio;
        ButtonDriver d;
        d.configure(&gpio, 7, /*active_high=*/true);
        EXPECT_EQ(d.open(), 0);
        EXPECT_EQ(gpio.init_pin_calls, 1);
        EXPECT_EQ(gpio.last_pull, auroraos::hal::GpioPull::PullDown);
    }
}

// =============================================================================
// 15. 同毫秒多次 poll：状态机幂等，无重复事件
// =============================================================================
TEST(ButtonDriverTest, SameMsRepeatedPolls) {
    ButtonDriver d;
    d.inject_press();
    for (int i = 0; i < 5; ++i) {
        d.poll(1000);
    }
    for (int i = 0; i < 5; ++i) {
        d.poll(1031);
    }
    auto events = drain(d);
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].value, 1);
}

// =============================================================================
// 16. tick 回绕：now_ms 跨越 2^32 边界，消抖与长按判定仍正确
// =============================================================================
TEST(ButtonDriverTest, TimeWraparound) {
    ButtonDriver d;
    const uint32_t t0 = 0xFFFFFFF0u; // 距回绕仅 16ms
    d.inject_press();
    d.poll(t0);
    d.poll(t0 + 31); // 回绕到 0x0000000F，无符号减法正确判定 31ms

    auto events = drain(d);
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].value, 1);

    d.poll(t0 + 31 + 800); // 长按判定同样回绕安全
    events = drain(d);
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].value, 2);
}

// =============================================================================
// 17. 注入路径与 fake HAL 电平路径等价：同场景产出一致的事件序列
// =============================================================================
TEST(ButtonDriverTest, InjectEqualsFakeHal) {
    // 路径 A：inject_press/inject_release
    ButtonDriver a;
    a.inject_press();
    a.poll(1000);
    a.poll(1031);
    a.inject_release();
    a.poll(1031);
    a.poll(1062);
    auto events_a = drain(a);

    // 路径 B：FakeGpioHal 电平（低有效：按下 = level=false）
    FakeGpioHal gpio;
    gpio.level = true;
    ButtonDriver b;
    b.configure(&gpio, 6, /*active_high=*/false);
    b.open();
    gpio.level = false; // 按下
    b.poll(1000);
    b.poll(1031);
    gpio.level = true; // 释放
    b.poll(1031);
    b.poll(1062);
    auto events_b = drain(b);

    ASSERT_EQ(events_a.size(), events_b.size());
    ASSERT_EQ(events_a.size(), 2u);
    for (size_t i = 0; i < events_a.size(); ++i) {
        EXPECT_EQ(events_a[i].timestamp, events_b[i].timestamp);
        EXPECT_EQ(events_a[i].type, events_b[i].type);
        EXPECT_EQ(events_a[i].code, events_b[i].code);
        EXPECT_EQ(events_a[i].value, events_b[i].value);
    }
}
