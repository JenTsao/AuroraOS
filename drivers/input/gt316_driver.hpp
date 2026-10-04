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

// =============================================================================
// L2a 埋点：poll_touch 的 touch_count 分类计数
//
// 为什么必须分类而不是只记总数：poll_touch 的 I2C 开销随「本轮硬件上报的
// 触摸点数」分档（下方 poll_touch 实测代码路径）：
//   空闲（data_ready=0，或 data_ready 且 count=0）：只读 0x814E → 2 事务
//   活跃（data_ready 且 count>0）：读 0x814E + 读 POINT1 8B + clear → 5 事务
// 两者相差 2.5 倍。若只统计「平均事务数」，55Hz 轮询下空闲与活跃会被
// 混成一个均值，得到一个既不等于空闲也不等于活跃的无意义数字——
// 这正是本轮分析第一轮 CPU 估算偏高 4 倍的根因（用 5 事务乘全部轮询频率）。
//
// 为什么用 inline static（C++17 inline 变量，基线 5b0d107 已钉住 -std=c++17）：
// 本头被 2 个 TU 包含（apps/watch/watch_app.cpp、tests/unit/test_gt316_driver.cpp）。
// 普通 static 会各 TU 一份副本，导致测试与实机各读各的数；inline static 保证
// 全程序唯一实例（实测符号为 V/weak，链接时合并）。
//
// 用 volatile 而非原子操作：写侧是 load/store 32 位对齐单指令，单核下不会
// 撕裂到「半个数」的程度；跨调用不保证快照一致，但埋点只需统计量级，
// 读取侧应自行容忍 ±1 的误差。
//
// 【排查指引】i2c_fail_polls / data_fail_polls 若非 0，**优先怀疑 I2C 总线锁
// 竞争超时，而非硬件故障**。I2cBusLock（hal/i2c_bus.hpp:83）获取超时是
// 20 tick = 20ms；持锁路径 wait_idle() 在 IOM 异常时可能长时间自旋占满这
// 20ms，随后 read_reg16 返回 false → 计入本计数器。区分方法：同期读
// aurora_i2c_idle_timeouts()，非 0 才是真正的 IOM/硬件层异常；为 0 而本计数
// 非 0，则几乎必然是锁竞争导致降级。硬件排查应放在排除竞争之后。
// =============================================================================
struct Gt316PollStats {
    volatile uint32_t idle_polls;      // data_ready=0（纯空闲轮询，2 事务）
    volatile uint32_t active_polls;    // data_ready=1 且 count>0（5 事务）
    // ⚠️ release_polls 含【两种语义】，HIL 侧读数偏高时不要直接查硬件：
    //   1) 真抬手（gt316_driver.hpp poll_touch 的 RELEASED 产出分支，3 事务）
    //   2) 同毫秒抖动抑制（release_pending_ 分支，2 事务：读状态 + clear，
    //      并未真正抬手，也未交付任何事件）
    // 拆成两个计数器需在抖动路径上再占 4B .bss；当前判据是「偏差 1~2 次/次
    // 抬手」属正常，若偏差与抬手次数同量级则说明硬件在抖，应查 GT316 侧。
    //
    // 【关于语义 2 的可观测性—— 修正一个此前的错误结论】
    // 曾有判断认为「抑制路径丢弃的点无法观测，clear_buffer_status() 之后
    // Ready bit 已读不到，软件计数器补不回来，必须在抑制时额外读一次 I2C
    // 才能区分」。该结论对【读事务】而言不成立：
    //   poll_touch :375 的 `uint8_t touch_count = status & 0x0F;` 已经把点数
    //   算好，且抑制分支 :395 就在同一 if 作用域内，变量可直接用。
    //   判 touch_count > 1 是【零 I2C 事务】的寄存器比较 —— status 本来就
    //   是为了取 data_ready 读回来的，多点信息在同一次传输里已经到手，
    //   只是在 clear 之前没人看它。
    // 因此下面 suppressed_multitouch 的成本是 0 事务 + 4B .bss，
    // 而非「一次额外读」。真正丢失的只是「被丢弃点数」这一个维度，
    // 且该维度要区分也只能靠计数，无法补救已丢的坐标。
    volatile uint32_t release_polls;
    volatile uint32_t i2c_fail_polls;  // 读 0x814E 失败
    volatile uint32_t data_fail_polls; // 读 POINT1 失败
    volatile uint32_t injected_polls;  // 手动注入（仿真/测试，0 事务）
    // 抑制路径上硬件【同时报告了多点】（touch_count > 1）的次数。
    // 语义：在 RELEASED 待消费期间被抑制的那一轮，硬件缓冲区里不止 1 个点。
    //   0（预期）：抑制的都是抬起时的单点残留帧，闩锁判据成立，
    //              丢弃是正确行为。
    //   > 0（异常）：抑制路径丢弃了多点数据 —— 正常情况下抬起后的残留帧
    //              应只有 1 个触点记录。
    //
    // 【不要把这个计数读成「吞掉了真实的第二根手指」】
    // release_pending_ 只在 data_ready && touch_count == 0 的分支置位，
    // 因此闩锁置位轮硬件报 0 点、抑制轮报 >0 点。该组合在物理上更可能
    // 是「抬起残留帧里含多个触点记录」，而非「一根手指抬起的同时另一根
    // 按下」——后者不该发生在同一毫秒内。要区分二者需要读 POINT1 的
    // TrackID（+1 次 I2C 事务），当前不做。
    //
    // 【触发 > 0 时应查什么】采样节奏是否被破坏。闩锁判据
    // current_ms == release_ms_ 依赖「同毫秒 = 双采样者已交错」这一前提。
    // 可论证的排查起点：采样统一由 sensor_ble_daemon_task（High，40ms）
    // 驱动，帧渲染路径不发起 I2C 事务；因此若 > 0 显著非 0，应查是否
    // 存在以固定/停滞时间戳调用 poll_touch 的第二条路径。
    //
    // 成本：4B .bss + 一次 volatile 自增，零额外 I2C 事务（见上方说明）。
    volatile uint32_t suppressed_multitouch;
};

inline Gt316PollStats g_gt316_poll_stats = {};

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

    // RELEASED 的「已产出、待消费」标志。
    //
    // 为什么需要它（这是本状态机加固的核心）：RELEASED 是一个【必须恰好
    // 产出一次】的事件，但「产出」与「消费」发生在【不同的 poll_touch
    // 调用】里。若把「RELEASED -> IDLE」的回退写成无条件的（见下方
    // idle 分支），那么任何一个【第二个采样者】都能在 RELEASED 产出后的
    // 任意一次轮询里把它提前消费掉；若此时硬件又报了一个点，同一次
    // poll_touch 内就会「消费掉上一个 RELEASED」又「立即产出新的
    // PRESSED」——抬起事件就此丢失。
    //
    // 本标志把「RELEASED 已产出」变成显式状态：只有标志被清除后，
    // current_state_ 才允许离开 RELEASED，状态机因此自洽。
    //
    // release_ms_ 记录 RELEASED 的产出时刻（ms），用于判定「同毫秒的
    // 第二个采样者」。这不是随意的设计：apps/watch/watch_app.cpp 的
    // poll_input(delta_ms) 用 `current_tick_ms_ += delta_ms` 推进时间戳，
    // 而帧渲染路径历史上传的是 poll_input(0) —— 时间戳【不前进】，
    // 于是 ui_render_task 与 sensor_ble_daemon_task 在同一毫秒内看到
    // 完全相同的 current_tick_ms_。同毫秒 = 双采样者已经交错，这是
    // 唯一需要抑制 PRESSED 的窗口；跨毫秒的新触点是合法新手势。
    bool release_pending_;
    uint32_t release_ms_;

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
    //
    // 【必须走 II2cHal::read_reg16，不能用 write()+read() 拼】
    // 修复前本函数调 i2c_->write() + i2c_->read() 两个公开入口，
    // 各加一次锁 —— 两次锁之间是可被同总线其他客户端插入的窗口
    // （触摸与加速度计共用 SENSOR_I2C_PORT，见 watch_app.hpp:83/:90）。
    // 窗口内插入一次加速度计事务后，下面这次 read 读到的已不是刚才
    // 写入的 0x814E 对应的数据：status 字节撕裂 → data_ready /
    // touch_count 判错 → 漏掉一次抬手，或读出半帧坐标。
    //
    // read_reg16 由 HAL 在单次 I2cBusGuard 内完成整个组合事务，调用方
    // 拿不到、也不需要知道内部加锁细节。GT316 的寄存器地址是 16 位，
    // 8 位的 II2cHal::read_reg 无法表达（会把 0x814E 截断成 0x4E）。
    bool read_reg16(uint16_t reg, uint8_t* data, size_t len) {
        if (!i2c_ || !data || len == 0)
            return false;
        return i2c_->read_reg16(i2c_addr_, reg, data, len);
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
          release_pending_(false),
          release_ms_(0),
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
        release_pending_ = false;
        release_ms_ = 0;
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
            g_gt316_poll_stats.injected_polls++;
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
                    // =========================================================
                    // RELEASED 消费门控（状态机自洽性的核心，勿删）
                    //
                    // 场景：上一次调用已产出 RELEASED（手指抬起），但【还没人
                    // 消费它】。若本轮硬件又报了点（手指抖动/新接触），这里若
                    // 直接走下面的状态机演进，会把 current_state_ 从 RELEASED
                    // 一步推到 PRESSED —— 抬起语义就在同一次 poll_touch 内被
                    // 吃掉，上层永远看不到 RELEASED。
                    //
                    // release_pending_ 为真表示「RELEASED 已产出、待消费」。
                    // 此时本次调用【只负责消费】RELEASED，不得产出新 PRESSED：
                    //   - 同毫秒（current_ms == release_ms_）：第二个采样者已经
                    //     在本毫秒内交错进来（帧渲染路径历史上传 poll_input(0)
                    //     使时间戳不前进），必须抑制 PRESSED。
                    //   - 跨毫秒：RELEASED 已被后续调用正常消费过，本轮是合法的
                    //     新手势，放行（保持既有行为，不给正常交互加延迟）。
                    // =========================================================
                    if (release_pending_ && current_ms == release_ms_) {
                        // 同毫秒的第二个采样者：本次调用【只消费】RELEASED，
                        // 不产出任何事件。RELEASED 已在产生它的那次调用里
                        // 交付给上层（poll_input 返回 true ->喂手势层），
                        // 此处若再产出一次就违反「恰好产出一次」。
                        //
                        // 硬件在本毫秒又报的点，是抬起过程中同一根手指的
                        // 残留帧/抖动（不同物理接触不可能发生在同一毫秒内），
                        // 因此连同握手一起清掉，不予交付。
                        release_pending_ = false;
                        current_state_ = TouchState::IDLE;
                        out_point->x = last_x_;
                        out_point->y = last_y_;
                        out_point->state = TouchState::IDLE;
                        out_point->is_valid = false;
                        clear_buffer_status();
                        // release_polls 语义 2/2：同毫秒抖动抑制。
                        // 本轮未抬手、未交付事件（见结构体注释），故本计数
                        // 高于「抬手次数」是正常的，不代表硬件异常。
                        g_gt316_poll_stats.release_polls++;
                        // L2a 埋点：抑制时硬件是否同时报了多点。
                        // touch_count 来自 :375 对 status 的解析，与本分支
                        // 同作用域，无需额外 I2C 事务 —— 修正了此前「必须额外
                        // 读一次才能观测」的错误判断。
                        // 判读见结构体注释：> 0 只说明抑制轮硬件报的是多点，
                        // 【不要】直接读成「吞掉了真实的第二根手指」——
                        //闩锁置位轮报0 点、抑制轮报多点，该组合更可能是抬起
                        // 残留帧里含多个触点记录。
                        if (touch_count > 1) {
                            g_gt316_poll_stats.suppressed_multitouch++;
                        }
                        return false;
                    }
                    release_pending_ = false;

                    // L2a 埋点：活跃轮询 = 5 事务路径
                    g_gt316_poll_stats.active_polls++;
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
                    // L2a 埋点：状态寄存器说有数据、但坐标读失败。
                    // 与 i2c_fail_polls 区分：后者是 0x814E 读失败（总线层），
                    // 此处是 POINT1 读失败（寄存器层/长度/并发撕裂）。
                    // 注意此时未 clear_buffer_status()，GT316 可能持续置 Ready
                    // 位 → 会反复进入本分支，故该计数同时是「握手卡死」观察点。
                    // 排查顺序见文件头【排查指引】：先看 aurora_i2c_idle_timeouts
                    // 排除硬件，再怀疑总线锁竞争。
                    g_gt316_poll_stats.data_fail_polls++;
                } else if (data_ready && touch_count == 0) {
                    // 硬件上报 0 个触摸点 -> 手指抬起
                    // L2a 埋点：抬手指令轮询（3 事务：读状态 + 读点 + clear）
                    // release_polls 语义 1/2：真抬手，本分支确实产出了 RELEASED。
                    g_gt316_poll_stats.release_polls++;
                    clear_buffer_status();
                    if (current_state_ == TouchState::PRESSED || current_state_ == TouchState::MOVING) {
                        current_state_ = TouchState::RELEASED;
                        // 标记「RELEASED 已产出、待消费」。这是加固的核心：
                        // 在被消费之前，current_state_ 不得被直接推进到 PRESSED。
                        release_pending_ = true;
                        release_ms_ = current_ms;
                        out_point->x = last_x_;
                        out_point->y = last_y_;
                        out_point->state = TouchState::RELEASED;
                        out_point->is_valid = true;
                        return true;
                    }
                }
            } else {
                // L2a 埋点：0x814E 读取失败（NACK/总线异常/锁竞争降级）。
                g_gt316_poll_stats.i2c_fail_polls++;
            }

            // L2a 埋点：走到这里 = 本轮无有效事件（data_ready=0）。
            // 这是占绝大多数的空闲路径，只花 2 个 I2C 事务（读 0x814E）。
            g_gt316_poll_stats.idle_polls++;

            // 无新事件发生且处于松手状态后，回退至 IDLE
            //
            // 消费 RELEASED：这是「RELEASED -> IDLE」的唯一合法出口。
            // 必须同时清掉 release_pending_，否则标志与状态会失配。
            if (current_state_ == TouchState::RELEASED) {
                current_state_ = TouchState::IDLE;
                release_pending_ = false;
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
