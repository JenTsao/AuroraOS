// =============================================================================
// drivers/input/gt316_driver.hpp
//
// 汇顶 (Goodix) GT316 电容触控芯片驱动
// 目标硬件：小米手环 8 (Xiaomi Smart Band 8) 1.62" AMOLED 触控面板
//
//   芯片型号    : Goodix GT316 (单点/多点电容触控 IC)
//   通信接口    : I2C (从机地址 0x14 / I2C_ADDR_GT316)
//   中断引脚    : PIN_TOUCH_INT (Pin 15, 低电平/下降沿触发)
//   屏幕分辨率  : 192 x 490 (DISPLAY_WIDTH x DISPLAY_HEIGHT)
//
// 寄存器协议架构：
//   - 0x8040: 命令/模式寄存器 (GT316_REG_COMMAND)
//   - 0x8047: 配置版本寄存器 (GT316_REG_CONFIG_VERSION)
//   - 0x8140: 芯片产品 ID (4 字节, "316\0" 或 "911\0")
//   - 0x814E: 缓冲状态与触控点数 (bit7=Ready, bit3..0=PointCount)
//   - 0x814F: 触控点 1 详情 (TrackID, X_L, X_H, Y_L, Y_H, Size)
//
// 握手机制：
//   读取完坐标数据后，主机向 0x814E 写入 0x00 清除状态标志，
//   释放硬件 INT 引脚，准备下一次触控中断。
//
// 设计原则（遵循 AGENTS.md 内核/驱动规范）：
//   - 零动态内存分配：静态结构体与对象内缓冲
//   - 通过 auroraos::hal::II2cHal / IGpioHal 抽象接口解耦硬件
//   - 支持真实 I2C 硬件通信、手动坐标注入与仿真模式回退
//   - 规范化输出 TouchPoint (VFS /dev/touch0) 与 RawTouchEvent
// =============================================================================
#ifndef AURORA_GT316_DRIVER_HPP
#define AURORA_GT316_DRIVER_HPP

#include <stdint.h>
#include <stddef.h>
#include "../../kernel/core/device.hpp"
#include "../../hal/i2c_hal.hpp"
#include "../../hal/gpio_hal.hpp"
#include "input_event.hpp"

// =============================================================================
// GT316 寄存器宏定义 (Goodix 16-bit 寄存器地址)
// =============================================================================
#define GT316_REG_COMMAND         0x8040 // 控制命令 (0=正常读写, 1=软复位, 2=休眠)
#define GT316_REG_CONFIG_VERSION  0x8047 // 配置版本号
#define GT316_REG_PRODUCT_ID      0x8140 // 产品 ID 字符串起始地址 (4 字节)
#define GT316_REG_FIRMWARE_VER    0x8144 // 固件版本号
#define GT316_REG_BUFFER_STATUS   0x814E // 缓冲状态与点数 (bit7: Ready, bit3..0: Count)
#define GT316_REG_POINT1_BASE     0x814F // 触控点 1 起始地址

// 8 位紧凑模式备用寄存器 (用于简化 HAL 或测试桩)
#define GT316_REG_STATUS_8BIT     0x02   // 触控状态
#define GT316_REG_POINT1_8BIT     0x03   // 触控点 1 坐标

// 默认从机 I2C 地址
#ifndef I2C_ADDR_GT316
#define I2C_ADDR_GT316 0x14
#endif

// 触控点内部寄存器偏移 (相对 POINT1_BASE)
#define GT316_OFFSET_TRACK_ID     0
#define GT316_OFFSET_X_LOW        1
#define GT316_OFFSET_X_HIGH       2
#define GT316_OFFSET_Y_LOW        3
#define GT316_OFFSET_Y_HIGH       4
#define GT316_OFFSET_POINT_SIZE   5
#define GT316_POINT_RECORD_LEN    8      // 每个触控点数据占用 8 字节

class Gt316Driver : public CharDevice {
public:
    static constexpr uint8_t kDefaultI2cAddr = I2C_ADDR_GT316;
    static constexpr uint16_t kDefaultWidth = 192;
    static constexpr uint16_t kDefaultHeight = 490;

private:
    auroraos::hal::II2cHal* i2c_;
    auroraos::hal::IGpioHal* gpio_;
    uint8_t i2c_addr_;
    int int_pin_;
    int rst_pin_;

    uint16_t max_x_;
    uint16_t max_y_;

    // 坐标映射与校准
    bool swap_xy_;
    bool invert_x_;
    bool invert_y_;

    // 触控内部状态机追踪
    TouchState current_state_;
    uint16_t last_x_;
    uint16_t last_y_;
    uint32_t last_event_time_ms_;

    // 仿真/回退模式状态
    bool sim_mode_enabled_;
    bool sim_active_;
    int sim_step_;
    uint16_t sim_x_;
    uint16_t sim_y_;
    int sim_dx_;
    int sim_dy_;

    // 外部注入缓冲 (用于单元测试与事件注入)
    bool has_injected_event_;
    TouchPoint injected_point_;

    // I2C 16 位大端寄存器写入辅助函数
    bool write_reg16(uint16_t reg, const uint8_t* data, size_t len) {
        if (!i2c_)
            return false;

        // 构建 [reg_hi, reg_lo, data...] 连续包
        uint8_t buf[16];
        if (len + 2 > sizeof(buf))
            return false;

        buf[0] = static_cast<uint8_t>((reg >> 8) & 0xFF);
        buf[1] = static_cast<uint8_t>(reg & 0xFF);
        for (size_t i = 0; i < len; ++i) {
            buf[2 + i] = data[i];
        }

        return i2c_->write(i2c_addr_, buf, len + 2);
    }

    // I2C 16 位大端寄存器读取辅助函数
    bool read_reg16(uint16_t reg, uint8_t* data, size_t len) {
        if (!i2c_ || !data || len == 0)
            return false;

        uint8_t addr_buf[2];
        addr_buf[0] = static_cast<uint8_t>((reg >> 8) & 0xFF);
        addr_buf[1] = static_cast<uint8_t>(reg & 0xFF);

        // 先发送 2 字节寄存器地址
        if (!i2c_->write(i2c_addr_, addr_buf, 2))
            return false;

        // 再读取目标长度的数据
        return i2c_->read(i2c_addr_, data, len);
    }

    // 清除 GT316 缓冲状态标志 (向 0x814E 写入 0x00)
    bool clear_buffer_status() {
        uint8_t zero = 0x00;
        return write_reg16(GT316_REG_BUFFER_STATUS, &zero, 1);
    }

public:
    explicit Gt316Driver(const char* name = "touch0",
                         uint16_t max_x = kDefaultWidth,
                         uint16_t max_y = kDefaultHeight)
        : CharDevice(name),
          i2c_(nullptr),
          gpio_(nullptr),
          i2c_addr_(kDefaultI2cAddr),
          int_pin_(-1),
          rst_pin_(-1),
          max_x_(max_x),
          max_y_(max_y),
          swap_xy_(false),
          invert_x_(false),
          invert_y_(false),
          current_state_(TouchState::IDLE),
          last_x_(0),
          last_y_(0),
          last_event_time_ms_(0),
          sim_mode_enabled_(false),
          sim_active_(false),
          sim_step_(0),
          sim_x_(100),
          sim_y_(64),
          sim_dx_(-4),
          sim_dy_(0),
          has_injected_event_(false),
          injected_point_{0, 0, TouchState::IDLE, false} {}

    // 单例访问
    static Gt316Driver& instance() {
        static Gt316Driver s_instance;
        return s_instance;
    }

    // 配置硬件总线与引脚
    void configure(auroraos::hal::II2cHal* i2c,
                   auroraos::hal::IGpioHal* gpio = nullptr,
                   int int_pin = -1,
                   int rst_pin = -1,
                   uint8_t i2c_addr = kDefaultI2cAddr) {
        i2c_ = i2c;
        gpio_ = gpio;
        int_pin_ = int_pin;
        rst_pin_ = rst_pin;
        i2c_addr_ = i2c_addr;
    }

    // 设置分辨率与方向校准
    void set_resolution(uint16_t max_x, uint16_t max_y) {
        max_x_ = max_x;
        max_y_ = max_y;
    }

    void set_coordinate_transform(bool swap_xy, bool invert_x, bool invert_y) {
        swap_xy_ = swap_xy;
        invert_x_ = invert_x;
        invert_y_ = invert_y;
    }

    // 是否启用 QEMU / 无硬件时的仿真模式
    void enable_simulation_mode(bool enable) {
        sim_mode_enabled_ = enable;
    }

    bool is_simulation_mode() const {
        return sim_mode_enabled_;
    }

    // 手动注入触控点（用于单元测试与调试）
    void inject_touch(uint16_t x, uint16_t y, TouchState state) {
        injected_point_.x = x;
        injected_point_.y = y;
        injected_point_.state = state;
        injected_point_.is_valid = (state != TouchState::IDLE);
        has_injected_event_ = true;
    }

    // ========================================================
    // 设备生命周期管理
    // ========================================================
    int open() override {
        // 1. 硬件复位脉冲时序 (若配置了 RST 引脚)
        if (gpio_ && rst_pin_ >= 0) {
            gpio_->init_pin(rst_pin_, auroraos::hal::GpioMode::Output, auroraos::hal::GpioPull::None);
            gpio_->set_pin(rst_pin_, false); // 拉低复位
            for (volatile int i = 0; i < 5000; ++i) {} // 保持 >1ms
            gpio_->set_pin(rst_pin_, true);  // 拉高退出复位
            for (volatile int i = 0; i < 50000; ++i) {} // 等待芯片启动就绪 >10ms
        }

        // 2. 中断引脚配置 (输入 + 上拉)
        if (gpio_ && int_pin_ >= 0) {
            gpio_->init_pin(int_pin_, auroraos::hal::GpioMode::Input, auroraos::hal::GpioPull::PullUp);
        }

        // 3. 检查硬件 I2C 连接
        if (i2c_) {
            uint8_t prod_id[4] = {0};
            if (read_reg16(GT316_REG_PRODUCT_ID, prod_id, 4)) {
                // 成功读到芯片 ID
                clear_buffer_status();
                sim_mode_enabled_ = false;
            } else {
                // 硬件无响应时回退到仿真模式
                sim_mode_enabled_ = true;
            }
        } else {
            // 没有配置 I2C 时默认走仿真/注入模式
            sim_mode_enabled_ = true;
        }

        current_state_ = TouchState::IDLE;
        last_x_ = 0;
        last_y_ = 0;
        return 0;
    }

    int close() override {
        if (i2c_) {
            // 可向 0x8040 写入 0x02 使 GT316 进入休眠省电
            uint8_t sleep_cmd = 0x02;
            write_reg16(GT316_REG_COMMAND, &sleep_cmd, 1);
        }
        current_state_ = TouchState::IDLE;
        return 0;
    }

    // ========================================================
    // 核心硬件轮询：读取单次触控状态与坐标
    // ========================================================
    bool poll_touch(TouchPoint* out_point, uint32_t current_ms = 0) {
        if (!out_point)
            return false;

        last_event_time_ms_ = current_ms;

        // 1. 优先消费手动注入的事件
        if (has_injected_event_) {
            *out_point = injected_point_;
            current_state_ = injected_point_.state;
            last_x_ = injected_point_.x;
            last_y_ = injected_point_.y;
            has_injected_event_ = false;
            return true;
        }

        // 2. 真实硬件 I2C 读取路径
        if (i2c_ && !sim_mode_enabled_) {
            uint8_t status = 0;
            // 读取缓冲状态
            if (read_reg16(GT316_REG_BUFFER_STATUS, &status, 1)) {
                bool data_ready = (status & 0x80) != 0;
                uint8_t touch_count = status & 0x0F;

                if (data_ready && touch_count > 0) {
                    // 读取触控点 1 数据 (8 字节)
                    uint8_t raw_data[GT316_POINT_RECORD_LEN] = {0};
                    if (read_reg16(GT316_REG_POINT1_BASE, raw_data, GT316_POINT_RECORD_LEN)) {
                        uint16_t raw_x = static_cast<uint16_t>(raw_data[GT316_OFFSET_X_LOW]) |
                                         (static_cast<uint16_t>(raw_data[GT316_OFFSET_X_HIGH]) << 8);
                        uint16_t raw_y = static_cast<uint16_t>(raw_data[GT316_OFFSET_Y_LOW]) |
                                         (static_cast<uint16_t>(raw_data[GT316_OFFSET_Y_HIGH]) << 8);

                        // 坐标旋转与映射
                        uint16_t mapped_x = swap_xy_ ? raw_y : raw_x;
                        uint16_t mapped_y = swap_xy_ ? raw_x : raw_y;

                        if (invert_x_ && max_x_ > 0) {
                            mapped_x = (mapped_x < max_x_) ? (max_x_ - 1 - mapped_x) : 0;
                        }
                        if (invert_y_ && max_y_ > 0) {
                            mapped_y = (mapped_y < max_y_) ? (max_y_ - 1 - mapped_y) : 0;
                        }

                        // 边界钳位
                        if (max_x_ > 0 && mapped_x >= max_x_) mapped_x = max_x_ - 1;
                        if (max_y_ > 0 && mapped_y >= max_y_) mapped_y = max_y_ - 1;

                        out_point->x = mapped_x;
                        out_point->y = mapped_y;
                        last_x_ = mapped_x;
                        last_y_ = mapped_y;

                        // 状态机演进
                        if (current_state_ == TouchState::IDLE || current_state_ == TouchState::RELEASED) {
                            current_state_ = TouchState::PRESSED;
                        } else {
                            current_state_ = TouchState::MOVING;
                        }

                        out_point->state = current_state_;
                        out_point->is_valid = true;

                        // 清除握手状态
                        clear_buffer_status();
                        return true;
                    }
                } else if (data_ready && touch_count == 0) {
                    // 硬件上报 0 个触摸点 -> 手指抬起
                    clear_buffer_status();
                    if (current_state_ == TouchState::PRESSED || current_state_ == TouchState::MOVING) {
                        current_state_ = TouchState::RELEASED;
                        out_point->x = last_x_;
                        out_point->y = last_y_;
                        out_point->state = TouchState::RELEASED;
                        out_point->is_valid = true;
                        return true;
                    }
                }
            }

            // 无新事件发生且处于松手状态后，回退至 IDLE
            if (current_state_ == TouchState::RELEASED) {
                current_state_ = TouchState::IDLE;
            }

            out_point->x = last_x_;
            out_point->y = last_y_;
            out_point->state = TouchState::IDLE;
            out_point->is_valid = false;
            return false;
        }

        // 3. 仿真模式路径 (供 QEMU 与无硬件环境)
        return poll_simulation(out_point);
    }

    // ========================================================
    // VFS 接口：供 read(touch_fd, buf, len) 读取
    // ========================================================
    int read(char* buf, int len, int offset, void* priv) override {
        (void)offset;
        (void)priv;
        if (!buf || len < static_cast<int>(sizeof(TouchPoint)))
            return 0;

        TouchPoint* point = reinterpret_cast<TouchPoint*>(buf);
        if (poll_touch(point, last_event_time_ms_ + 33)) {
            return sizeof(TouchPoint);
        }

        point->is_valid = false;
        point->state = TouchState::IDLE;
        return sizeof(TouchPoint);
    }

private:
    bool poll_simulation(TouchPoint* point) {
        static uint32_t sim_frame = 0;
        sim_frame++;

        if (!sim_active_ && (sim_frame % 150 == 0)) {
            sim_active_ = true;
            sim_step_ = 0;
            static int gesture_cycle = 0;
            gesture_cycle = (gesture_cycle + 1) % 4;

            if (gesture_cycle == 0 || gesture_cycle == 1) {
                // 左滑模拟：从 x=100 减小到 20
                sim_x_ = (max_x_ > 100) ? 100 : (max_x_ * 3 / 4);
                sim_y_ = max_y_ / 2;
                sim_dx_ = -4;
                sim_dy_ = 0;
            } else {
                // 右滑模拟：从 x=20 增大到 100
                sim_x_ = (max_x_ > 100) ? 20 : (max_x_ / 4);
                sim_y_ = max_y_ / 2;
                sim_dx_ = 4;
                sim_dy_ = 0;
            }
        }

        if (sim_active_) {
            point->x = sim_x_;
            point->y = sim_y_;

            if (sim_step_ == 0) {
                point->state = TouchState::PRESSED;
                current_state_ = TouchState::PRESSED;
            } else if (sim_step_ < 20) {
                point->state = TouchState::MOVING;
                current_state_ = TouchState::MOVING;
                sim_x_ += sim_dx_;
                sim_y_ += sim_dy_;
            } else {
                point->state = TouchState::RELEASED;
                current_state_ = TouchState::RELEASED;
                sim_active_ = false;
            }
            point->is_valid = true;
            sim_step_++;
            return true;
        }

        if (current_state_ == TouchState::RELEASED) {
            current_state_ = TouchState::IDLE;
        }

        point->x = last_x_;
        point->y = last_y_;
        point->state = TouchState::IDLE;
        point->is_valid = false;
        return false;
    }
};

#endif // AURORA_GT316_DRIVER_HPP
