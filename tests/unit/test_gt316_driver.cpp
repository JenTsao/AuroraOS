// =============================================================================
// tests/unit/test_gt316_driver.cpp
//
// 汇顶 GT316 真实电容触控驱动与 7 态手势识别全链路测试
// =============================================================================
#include <gtest/gtest.h>
#include <vector>
#include <cstring>

#include "../../hal/i2c_hal.hpp"
#include "../../hal/gpio_hal.hpp"
#include "../../drivers/input/gt316_driver.hpp"
#include "../../drivers/input/gesture_recognizer.hpp"
#include "../../ui/ui_manager.hpp"
#include "../../ui/screen_navigator.hpp"
#include "../../ui/widgets/button.hpp"

// =============================================================================
// 模拟 I2C HAL 设备，实现 GT316 真实寄存器交互模型
// =============================================================================
class MockGt316I2cHal : public auroraos::hal::II2cHal {
public:
    uint8_t memory[0x10000]; // 64KB 寄存器空间
    bool write_called;
    bool read_called;
    uint16_t last_reg_written;
    uint8_t last_val_written;

    MockGt316I2cHal() : write_called(false), read_called(false), last_reg_written(0), last_val_written(0) {
        std::memset(memory, 0, sizeof(memory));

        // 默认初始化 GT316 识别标识
        memory[GT316_REG_PRODUCT_ID] = '3';
        memory[GT316_REG_PRODUCT_ID + 1] = '1';
        memory[GT316_REG_PRODUCT_ID + 2] = '6';
        memory[GT316_REG_PRODUCT_ID + 3] = '\0';
    }

    void simulate_touch(uint16_t x, uint16_t y, uint8_t point_count = 1) {
        memory[GT316_REG_BUFFER_STATUS] = 0x80 | (point_count & 0x0F); // Ready + Count
        memory[GT316_REG_POINT1_BASE + GT316_OFFSET_TRACK_ID] = 0;
        memory[GT316_REG_POINT1_BASE + GT316_OFFSET_X_LOW] = static_cast<uint8_t>(x & 0xFF);
        memory[GT316_REG_POINT1_BASE + GT316_OFFSET_X_HIGH] = static_cast<uint8_t>((x >> 8) & 0xFF);
        memory[GT316_REG_POINT1_BASE + GT316_OFFSET_Y_LOW] = static_cast<uint8_t>(y & 0xFF);
        memory[GT316_REG_POINT1_BASE + GT316_OFFSET_Y_HIGH] = static_cast<uint8_t>((y >> 8) & 0xFF);
        memory[GT316_REG_POINT1_BASE + GT316_OFFSET_POINT_SIZE] = 0x20;
    }

    void simulate_release() {
        memory[GT316_REG_BUFFER_STATUS] = 0x80 | 0x00; // Ready + 0 Points
    }

    bool write(uint8_t dev_addr, const uint8_t* data, size_t len) override {
        if (dev_addr != I2C_ADDR_GT316 || !data || len < 2)
            return false;
        write_called = true;

        uint16_t reg = (static_cast<uint16_t>(data[0]) << 8) | data[1];
        last_reg_written = reg;

        if (len > 2) {
            last_val_written = data[2];
            for (size_t i = 0; i < len - 2; ++i) {
                if (reg + i < sizeof(memory)) {
                    memory[reg + i] = data[2 + i];
                }
            }
        }
        return true;
    }

    bool write_reg(uint8_t dev_addr, uint8_t reg_addr, const uint8_t* data, size_t len) override {
        (void)dev_addr; (void)reg_addr; (void)data; (void)len;
        return false;
    }

    bool read(uint8_t dev_addr, uint8_t* data, size_t len) override {
        if (dev_addr != I2C_ADDR_GT316 || !data)
            return false;
        read_called = true;
        for (size_t i = 0; i < len; ++i) {
            if (last_reg_written + i < sizeof(memory)) {
                data[i] = memory[last_reg_written + i];
            } else {
                data[i] = 0;
            }
        }
        return true;
    }

    bool read_reg(uint8_t dev_addr, uint8_t reg_addr, uint8_t* data, size_t len) override {
        (void)dev_addr; (void)reg_addr; (void)data; (void)len;
        return false;
    }
};

// =============================================================================
// 模拟 GPIO HAL
// =============================================================================
class MockGpioHal : public auroraos::hal::IGpioHal {
public:
    bool rst_state = true;
    int rst_init_count = 0;

    void init_pin(uint32_t pin, auroraos::hal::GpioMode mode, auroraos::hal::GpioPull pull) override {
        (void)pin; (void)mode; (void)pull;
        rst_init_count++;
    }
    void set_pin(uint32_t pin, bool high) override {
        (void)pin;
        rst_state = high;
    }
    bool read_pin(uint32_t pin) override {
        (void)pin;
        return true;
    }
    void toggle_pin(uint32_t pin) override {
        (void)pin;
        rst_state = !rst_state;
    }
};

// =============================================================================
// 1. GT316 驱动基础初始化与硬件通信测试
// =============================================================================
TEST(Gt316DriverTest, InitAndOpenSuccess) {
    MockGt316I2cHal mock_i2c;
    MockGpioHal mock_gpio;

    Gt316Driver driver("touch0", 192, 490);
    driver.configure(&mock_i2c, &mock_gpio, 15, 13, I2C_ADDR_GT316);

    int ret = driver.open();
    EXPECT_EQ(ret, 0);
    EXPECT_FALSE(driver.is_simulation_mode());
    EXPECT_TRUE(mock_i2c.write_called);
    EXPECT_TRUE(mock_i2c.read_called);

    // 握手写入：开机应向 0x814E 清除缓冲状态
    EXPECT_EQ(mock_i2c.memory[GT316_REG_BUFFER_STATUS], 0x00);
}

// =============================================================================
// 2. GT316 真实触控采样与状态机转换测试
// =============================================================================
TEST(Gt316DriverTest, TouchPressMoveReleaseSequence) {
    MockGt316I2cHal mock_i2c;
    Gt316Driver driver("touch0", 192, 490);
    driver.configure(&mock_i2c, nullptr, -1, -1, I2C_ADDR_GT316);
    driver.open();

    TouchPoint p{};

    // 1. 空闲状态：无触控数据
    bool has_touch = driver.poll_touch(&p, 100);
    EXPECT_FALSE(has_touch);
    EXPECT_EQ(p.state, TouchState::IDLE);

    // 2. 模拟手指初次按下 (x=50, y=100)
    mock_i2c.simulate_touch(50, 100, 1);
    has_touch = driver.poll_touch(&p, 133);
    EXPECT_TRUE(has_touch);
    EXPECT_EQ(p.x, 50);
    EXPECT_EQ(p.y, 100);
    EXPECT_EQ(p.state, TouchState::PRESSED);
    EXPECT_TRUE(p.is_valid);
    EXPECT_EQ(mock_i2c.memory[GT316_REG_BUFFER_STATUS], 0x00); // 握手清除验证

    // 3. 模拟手指在屏幕上拖拽滑动 (x=55, y=120)
    mock_i2c.simulate_touch(55, 120, 1);
    has_touch = driver.poll_touch(&p, 166);
    EXPECT_TRUE(has_touch);
    EXPECT_EQ(p.x, 55);
    EXPECT_EQ(p.y, 120);
    EXPECT_EQ(p.state, TouchState::MOVING);

    // 4. 模拟手指离开屏幕 (上报 0 个点)
    mock_i2c.simulate_release();
    has_touch = driver.poll_touch(&p, 200);
    EXPECT_TRUE(has_touch);
    EXPECT_EQ(p.state, TouchState::RELEASED);
    EXPECT_EQ(p.x, 55); // 保持松手前的坐标

    // 5. 再次采样，回到 IDLE
    has_touch = driver.poll_touch(&p, 233);
    EXPECT_FALSE(has_touch);
    EXPECT_EQ(p.state, TouchState::IDLE);
}

// =============================================================================
// 3. 坐标边界钳位与坐标系翻转测试
// =============================================================================
TEST(Gt316DriverTest, CoordinateTransformAndClamping) {
    MockGt316I2cHal mock_i2c;
    Gt316Driver driver("touch0", 192, 490);
    driver.configure(&mock_i2c, nullptr, -1, -1, I2C_ADDR_GT316);
    driver.set_coordinate_transform(false, true, false); // 水平镜像翻转
    driver.open();

    TouchPoint p{};
    // 模拟超出边界的超大坐标
    mock_i2c.simulate_touch(10, 600, 1);
    driver.poll_touch(&p, 100);

    // invert_x = true -> mapped_x = 192 - 1 - 10 = 181
    EXPECT_EQ(p.x, 181);
    // y 超过 490 -> 钳位在 489
    EXPECT_EQ(p.y, 489);
}

// =============================================================================
// 4. GT316 + GestureRecognizer 完整 7 态手势识别链路测试
// =============================================================================
TEST(GestureRecognizerTest, SwipeLeftAndRightRecognition) {
    GestureRecognizer recognizer;

    // --- 测试左滑 (x 从 150 减少到 30，位移 -120px) ---
    RawTouchEvent press{150, 100, TouchState::PRESSED, 100};
    GestureEvent ge = recognizer.process_event(press);
    EXPECT_EQ(ge.type, GestureType::NONE);

    RawTouchEvent move{90, 100, TouchState::MOVING, 150};
    ge = recognizer.process_event(move);
    EXPECT_EQ(ge.type, GestureType::NONE);

    RawTouchEvent release{30, 100, TouchState::RELEASED, 200};
    ge = recognizer.process_event(release);
    EXPECT_EQ(ge.type, GestureType::SWIPE_LEFT);
    EXPECT_EQ(ge.x, 150); // 起始 X
    EXPECT_EQ(ge.y, 100); // 起始 Y

    recognizer.reset();

    // --- 测试右滑 (x 从 30 增加到 150，位移 +120px) ---
    press = {30, 100, TouchState::PRESSED, 300};
    recognizer.process_event(press);

    release = {150, 100, TouchState::RELEASED, 400};
    ge = recognizer.process_event(release);
    EXPECT_EQ(ge.type, GestureType::SWIPE_RIGHT);
}

TEST(GestureRecognizerTest, SwipeUpDownRecognition) {
    GestureRecognizer recognizer;

    // --- 测试上滑 (y 从 200 减少到 50，位移 -150px) ---
    recognizer.process_event({100, 200, TouchState::PRESSED, 100});
    GestureEvent ge = recognizer.process_event({100, 50, TouchState::RELEASED, 200});
    EXPECT_EQ(ge.type, GestureType::SWIPE_UP);

    recognizer.reset();

    // --- 测试下滑 (y 从 50 增加到 200，位移 +150px) ---
    recognizer.process_event({100, 50, TouchState::PRESSED, 300});
    ge = recognizer.process_event({100, 200, TouchState::RELEASED, 400});
    EXPECT_EQ(ge.type, GestureType::SWIPE_DOWN);
}

TEST(GestureRecognizerTest, TapAndDoubleTapRecognition) {
    GestureRecognizer recognizer;

    // --- 第一次点击 (t=100 -> t=150) ---
    recognizer.process_event({50, 50, TouchState::PRESSED, 100});
    GestureEvent ge1 = recognizer.process_event({52, 51, TouchState::RELEASED, 150});
    EXPECT_EQ(ge1.type, GestureType::TAP);
    EXPECT_EQ(ge1.x, 50);
    EXPECT_EQ(ge1.y, 50);

    // --- 在 300ms 窗口内发生第二次点击 (t=250 -> t=300) ---
    recognizer.process_event({51, 50, TouchState::PRESSED, 250});
    GestureEvent ge2 = recognizer.process_event({50, 50, TouchState::RELEASED, 300});
    EXPECT_EQ(ge2.type, GestureType::DOUBLE_TAP);
    EXPECT_EQ(ge2.x, 51);
}

TEST(GestureRecognizerTest, RealtimeLongPressRecognition) {
    GestureRecognizer recognizer;

    // 手指按下并原地长按保持 >800ms
    recognizer.process_event({80, 80, TouchState::PRESSED, 100});

    // 持续按住，时间来到 t=950ms (持续 850ms)
    GestureEvent ge = recognizer.process_event({82, 81, TouchState::MOVING, 950});
    EXPECT_EQ(ge.type, GestureType::LONG_PRESS);
    EXPECT_EQ(ge.x, 80);
    EXPECT_EQ(ge.y, 80);

    // 松手时不会误触发 TAP
    GestureEvent ge_rel = recognizer.process_event({82, 81, TouchState::RELEASED, 1050});
    EXPECT_EQ(ge_rel.type, GestureType::NONE);
}

// =============================================================================
// 5. 端到端链路：GT316 驱动 -> 手势识别 -> UI 按钮响应与页面路由
// =============================================================================
static bool s_button_was_clicked = false;
static void on_test_btn_clicked(void* ctx) {
    (void)ctx;
    s_button_was_clicked = true;
}

TEST(Gt316E2ETest, Gt316ToUiButtonClickAndPageNav) {
    MockGt316I2cHal mock_i2c;
    Gt316Driver driver("touch0", 192, 490);
    driver.configure(&mock_i2c, nullptr, -1, -1, I2C_ADDR_GT316);
    driver.open();

    GestureRecognizer recognizer;

    // 构建一个包含 Button 的 UI 树
    UI::ViewGroup* root = new UI::ViewGroup(0, 0, 192, 490);
    UI::Button* btn = new UI::Button(40, 40, 80, 40, 0x0000, 0x07E0);
    s_button_was_clicked = false;
    btn->set_on_click(on_test_btn_clicked, nullptr);
    root->add_child(btn);

    UI::UiManager::instance().set_root_view(root);

    // 1. 模拟 GT316 产生按钮区域内点击 (x=60, y=50)
    mock_i2c.simulate_touch(60, 50, 1);
    TouchPoint p1{};
    driver.poll_touch(&p1, 100);
    GestureEvent ge1 = recognizer.feed_touch_point(p1, 100);
    EXPECT_EQ(ge1.type, GestureType::NONE);

    mock_i2c.simulate_release();
    TouchPoint p2{};
    driver.poll_touch(&p2, 180);
    GestureEvent ge2 = recognizer.feed_touch_point(p2, 180);
    EXPECT_EQ(ge2.type, GestureType::TAP);
    EXPECT_EQ(ge2.x, 60);
    EXPECT_EQ(ge2.y, 50);

    // 分发至 UI 树
    UI::UiManager::instance().dispatch_gesture(ge2);
    EXPECT_TRUE(s_button_was_clicked);

    // 2. 模拟 GT316 产生左滑动作 (从 x=150 滑至 x=20)
    recognizer.reset();
    mock_i2c.simulate_touch(150, 200, 1);
    TouchPoint p3{};
    driver.poll_touch(&p3, 300);
    recognizer.feed_touch_point(p3, 300);

    mock_i2c.simulate_touch(20, 200, 1);
    TouchPoint p4{};
    driver.poll_touch(&p4, 350);
    recognizer.feed_touch_point(p4, 350);

    mock_i2c.simulate_release();
    TouchPoint p5{};
    driver.poll_touch(&p5, 400);
    GestureEvent ge_swipe = recognizer.feed_touch_point(p5, 400);
    EXPECT_EQ(ge_swipe.type, GestureType::SWIPE_LEFT);

    UI::UiManager::instance().set_root_view(nullptr);
    delete root;
}
