#include "../../../hal/gpio_hal.hpp"
#include "../../../hal/spi_hal.hpp"
#include "../../../hal/i2c_hal.hpp"
#include "../../../hal/i2c_bus.hpp"
#include "../../../hal/secure_storage_hal.hpp"
#include "../../../kernel/core/device.hpp"
#include "../../../net/ble/hci/hci_uart_transport.hpp"
#include "board.h"
#include <string.h>

namespace auroraos {
namespace hal {

// ============================================================================
// Apollo3 Blue GPIO 寄存器映射 (base 0x40010000, 50 个 GPIO: 0~49)
//
// 分组：A=0~15, B=16~31, C=32~47, D=48~49（每组最多 16 脚）
// 写后置位/清零/翻转 (Write-Then Set/Clear/Toggle) 模型，避免读改写竞态。
// ============================================================================
#define AM_HAL_GPIO_BASE 0x40010000UL
#define AM_REG_GPIO_CFG_A 0x010 // GPIO 模式配置（每脚 4 bit，8 脚/寄存器）
#define AM_REG_GPIO_WTS_A 0x030 // 写后置位 (Set)
#define AM_REG_GPIO_WTC_A 0x040 // 写后清零 (Clear)
#define AM_REG_GPIO_WTT_A 0x050 // 写后翻转 (Toggle)
#define AM_REG_GPIO_RD_A 0x060  // 读输入电平

// ============================================================================
// Apollo3 Blue IOM0 寄存器映射 (base 0x50004000, SPI Master 模式)
// 偏移以 Apollo3 Blue datasheet「IOM Register Map」为准。
// ============================================================================
#define AM_HAL_IOM0_BASE 0x50004000UL
#define AM_REG_IOM_CFG 0x000          // 模块配置（功能/主从/相位/使能）
#define AM_REG_IOM_DMACFG 0x010       // DMA 配置
#define AM_REG_IOM_DMASTAT 0x014      // DMA 状态
#define AM_REG_IOM_DMATOTCOUNT 0x018  // DMA 传输总字节数
#define AM_REG_IOM_DMATARGADDR 0x024  // DMA 目标地址
#define AM_REG_IOM_DMATRIG 0x02C      // DMA 触发
#define AM_REG_IOM_CLKCFG 0x100       // 时钟配置
#define AM_REG_IOM_STATUS 0x104       // 状态
#define AM_REG_IOM_CMD 0x108          // 命令
#define AM_REG_IOM_MSPICFG 0x1A0      // MSPI 配置
#define AM_REG_IOM_FIFOPOP 0x1F8      // FIFO 读
#define AM_REG_IOM_FIFOPUSH 0x1FC     // FIFO 写

// ---- CFG 位域 ----
#define IOM_CFG_FUNCSEL_MSPI 0x0 // [2:0]=000 选择 SPI Master
#define IOM_CFG_MASTER (1u << 4) // [4] 主模式
#define IOM_CFG_FSPOL (1u << 5)  // [5] CPOL 时钟极性
#define IOM_CFG_FSPHA (1u << 6)  // [6] CPHA 时钟相位
#define IOM_CFG_ENABLE (1u << 31) // [31] 模块使能

// ---- CLKCFG 位域 ----
#define IOM_CLKCFG_HSEN (1u << 3) // [3] 高速分频使能
#define IOM_CLKCFG_HSDIV_SHIFT 8  // [10:8] 高速分频值 (96MHz/(n+1))

// ---- STATUS 位域 ----
#define IOM_STATUS_IDLE (1u << 1)   // [1] 空闲
#define IOM_STATUS_CMDCMP (1u << 0) // [0] 命令完成

// ---- DMA 位域 ----
#define IOM_DMACFG_DMADIR (1u << 0) // [0] 0=RAM→IOM(TX), 1=IOM→RAM(RX)
#define IOM_DMACFG_DMAEN (1u << 1)  // [1] DMA 使能
#define IOM_DMASTAT_DMAERR (1u << 0) // [0] DMA 错误
#define IOM_DMASTAT_DMACPL (1u << 2) // [2] DMA 完成

// ============================================================================
// Apollo3 GPIO HAL
// ============================================================================
class Apollo3GpioHal : public IGpioHal {
private:
    // 返回引脚所属组的配置寄存器地址（CFG/WTS/WTC/WTT/RD 基址由调用方传入）
    static volatile uint32_t* group_reg(uint32_t pin, uint32_t base_off) {
        return reinterpret_cast<volatile uint32_t*>(AM_HAL_GPIO_BASE + base_off + (pin / 16u) * 4u);
    }

public:
    void init_pin(uint32_t pin, GpioMode mode, GpioPull pull) override {
        if (pin >= 50u)
            return;

        // 每个 GPIO 用 4 bit 配置，8 脚共用一个 32 位 CFG 寄存器
        volatile uint32_t* cfg =
            reinterpret_cast<volatile uint32_t*>(AM_HAL_GPIO_BASE + AM_REG_GPIO_CFG_A + (pin / 8u) * 4u);
        const uint32_t shift = (pin % 8u) * 4u;

        uint32_t gpio_mode = (mode == GpioMode::Output) ? 1u : 0u; // 0=输入, 1=输出
        uint32_t pull_bits = 0u;
        if (mode != GpioMode::Output) {
            pull_bits = (pull == GpioPull::PullUp) ? (1u << 2) : (pull == GpioPull::PullDown) ? (2u << 2) : 0u;
        }

        *cfg = (*cfg & ~(0xFu << shift)) | ((gpio_mode | pull_bits) << shift);
    }

    void set_pin(uint32_t pin, bool high) override {
        if (pin >= 50u)
            return;
        const uint32_t bit = 1u << (pin % 16u);
        volatile uint32_t* reg = group_reg(pin, high ? AM_REG_GPIO_WTS_A : AM_REG_GPIO_WTC_A);
        *reg = bit;
    }

    bool read_pin(uint32_t pin) override {
        if (pin >= 50u)
            return false;
        const uint32_t bit = 1u << (pin % 16u);
        return (*group_reg(pin, AM_REG_GPIO_RD_A) & bit) != 0;
    }

    void toggle_pin(uint32_t pin) override {
        if (pin >= 50u)
            return;
        const uint32_t bit = 1u << (pin % 16u);
        *group_reg(pin, AM_REG_GPIO_WTT_A) = bit;
    }
};

// ============================================================================
// Apollo3 IOM SPI Master HAL
//
// 完整 DMA 路径：transmit_dma 配置 DMA 引擎后由 wait_transmit_complete
// 以 WFI 等待 DMASTAT.DMACPL，而非纯忙等轮询（见 README 路线图「完成 DMA
// 路径并移除忙等占位」）。
// ============================================================================
class Apollo3SpiHal : public ISpiHal {
private:
    bool configured_ = false;

    static volatile uint32_t* reg(uint32_t off) {
        return reinterpret_cast<volatile uint32_t*>(AM_HAL_IOM0_BASE + off);
    }

    // 等待 IOM 空闲（上一次传输完成）
    void wait_idle() {
        volatile uint32_t* status = reg(AM_REG_IOM_STATUS);
        uint32_t timeout = 1000000u;
        while ((*status & IOM_STATUS_IDLE) == 0 && --timeout) {
#if !defined(AURORA_HOST_TEST)
            __asm__ volatile("nop");
#endif
        }
    }

    // 首次使用时把 IOM0 配置为 SPI Master。
    // 时钟源 HFRC 96MHz，HSDIV=7 → 约 12MHz SCLK（需按屏的外设时序上限复核）。
    void ensure_configured() {
        if (configured_)
            return;
        configured_ = true;

        // 先关闭再配置，避免半配置状态误触发传输
        *reg(AM_REG_IOM_CFG) = IOM_CFG_FUNCSEL_MSPI | IOM_CFG_MASTER; // CPOL=0, CPHA=0 (Mode 0)
        *reg(AM_REG_IOM_CLKCFG) = IOM_CLKCFG_HSEN | (7u << IOM_CLKCFG_HSDIV_SHIFT);
        *reg(AM_REG_IOM_CFG) = IOM_CFG_FUNCSEL_MSPI | IOM_CFG_MASTER | IOM_CFG_ENABLE;
    }

public:
    void transmit_byte(uint8_t byte) override {
        ensure_configured();
        wait_idle();
        *reg(AM_REG_IOM_FIFOPUSH) = byte;
    }

    void transmit(const uint8_t* data, size_t len) override {
        if (!data)
            return;
        for (size_t i = 0; i < len; ++i) {
            transmit_byte(data[i]);
        }
    }

    void receive(uint8_t* data, size_t len) override {
        if (!data)
            return;
        ensure_configured();
        // 全双工：写入 dummy 字节并回读 FIFOPOP
        for (size_t i = 0; i < len; ++i) {
            wait_idle();
            *reg(AM_REG_IOM_FIFOPUSH) = 0xFF;
            wait_idle();
            data[i] = static_cast<uint8_t>(*reg(AM_REG_IOM_FIFOPOP));
        }
    }

    void transmit_dma(const uint8_t* data, size_t len) override {
        if (!data || len == 0)
            return;
        ensure_configured();
        wait_idle();

        // TX 方向：RAM → IOM (DMADIR=0)
        *reg(AM_REG_IOM_DMATARGADDR) = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(data));
        *reg(AM_REG_IOM_DMATOTCOUNT) = static_cast<uint32_t>(len);
        *reg(AM_REG_IOM_DMACFG) = IOM_DMACFG_DMAEN; // DMADIR=0
        // 触发 DMA 传输
        *reg(AM_REG_IOM_DMATRIG) = 1u;
    }

    void wait_transmit_complete() override {
        volatile uint32_t* dmastat = reg(AM_REG_IOM_DMASTAT);
        uint32_t timeout = 5000000u;
        while ((*dmastat & IOM_DMASTAT_DMACPL) == 0 && --timeout) {
#if !defined(AURORA_HOST_TEST)
            __asm__ volatile("wfi" : : : "memory");
#endif
        }
    }
};

IGpioHal* get_gpio_hal() {
    static Apollo3GpioHal gpio_hal;
    return &gpio_hal;
}

ISpiHal* get_spi_hal(int bus_id) {
    // 假设 bus_id 0 对应唯一的 SPI 实例 (IOM0)
    (void)bus_id;
    static Apollo3SpiHal spi_hal;
    return &spi_hal;
}

// ============================================================================
// Apollo3 IOM I2C Master HAL
//
// 专用于 I2C 传感器与触控总线 (IOM1 / SENSOR_I2C_PORT)，从机设备包括：
//   - GT316 电容触控 IC (0x14)
//   - GH3026 PPG 心率传感器 (0x28)
//   - BHI260AP 6 轴加速度计 (0x28)
//
// 并发安全：IOM 的 FIFO/CMD 是共享寄存器，同一总线上 GT316 (ui_render_task,
// Realtime) 与 BHI260AP (sensor_ble_daemon_task, High) 会并发发起事务。
// 每个公开操作用 I2cBusGuard 把「起始地址写入 + 数据推送 + CMD 触发 +
// 等待完成 + FIFO 读出」整段事务包在总线锁内，防止事务被撕裂。
//
// 锁不可重入：公开方法加锁后只调用 _write_impl/_read_impl 等私有实现，
// 私有实现绝不自行加锁。read_reg 原先内部调用 write+read，现已改为在
// 单次持锁下调用两个 _impl，避免自死锁 —— 同时这比原先的两次独立调用
// 更强：写寄存器地址与读回数据之间不再可能被其他任务插入。
// ============================================================================
#define AM_HAL_IOM1_BASE 0x50005000UL
#define IOM_CFG_FUNCSEL_I2C 0x1u // [2:0]=001 选择 I2C Master

// ============================================================================
// L2a I2C 埋点计数器（诊断用途）
//
// 为什么不用 Metrics::record()：metrics/metrics.cpp:25-29 的 record() 有
// `if (!g_is_active) return` 门控，而 g_is_active 仅由 start_measurement() 打开。
// 若埋点挂在 Metrics 上，未显式开启测量窗口时读到恒 0 —— 这正是「埋了但
// 读不到数」最常见的坑。改用裸 volatile 计数器：写侧永不丢弃，
// 由 HIL 侧决定何时读取/清零。
//
// 为什么是 static：wait_idle() 是 const 成员函数，成员计数器需 mutable，
// 但那样会变成「每 HAL 实例一份」，而 get_i2c_hal() 的 static 实例虽然唯一，
// 仍让语义与可见性变差。文件作用域 static 给出「按物理总线全局一份」的
// 直白语义，且 const 方法内可写。
//
// 原子性：累加位于 I2cBusLock 互斥区内——所有公开入口（write:369 /
// write_reg:376 / read:401 / read_reg:408）都先构造 I2cBusGuard，再进入
// write_impl/read_impl（:331/:348），wait_idle() 只被 _impl 调用（:335/:344/
// :352/:357）或在 guard 作用域内直接调用（:384/:397）。互斥区内不可能有
// 第二个任务进来，故 uint32_t 读-改-写不会丢计数。
//
// ⚠️ 该保证来自 I2cBusLock 的互斥，而非「单核」或「关中断」。别用后者当依据：
//   poll_input 的两个调用点分属不同优先级任务（ui_render_task=Realtime、
//   miband_kernel.hpp:95；sensor_ble_daemon_task=High、:102），SysTick 完全
//   可以在这条 volatile 的 load 与 store 之间抢占。
//   真正需要 irq_save 的是 I2cBusLock 自己的两个诊断计数器：
//   try_acquire()（i2c_bus.hpp:107-114）在 mutex_.lock() 返回 false 之后才
//   ++degraded_count_，而超时返回前调用过 Scheduler::schedule()（mutex.hpp:268），
//   中断早已恢复，此时两个 ++ 之间存在真实抢占窗口。
//
// ⚠️ 若日后出现第二个 Apollo3I2cHal 实例，而计数器仍为文件作用域 static，
//   两个实例的锁互不相关，上述互斥保证即失效——需改为 per-instance 成员
//   （配合 mutable）或显式 irq_save/restore 保护。
// ============================================================================
#if BOARD_HAS_DWT
static volatile uint32_t g_i2c_busy_cycles = 0;      // 累计忙等周期数
static volatile uint32_t g_i2c_wait_idle_calls = 0;  // wait_idle() 调用次数
static volatile uint32_t g_i2c_idle_timeouts = 0;    // timeout 耗尽次数（硬件异常）
#endif

class Apollo3I2cHal : public II2cHal {
private:
    uint32_t base_addr_;
    bool configured_;

    // 总线级串行化锁。作用域为本实例：get_i2c_hal() 对同一物理总线返回
    // 同一 static 实例，挂在实例上即可覆盖该总线的全部客户端。
    I2cBusLock bus_lock_;

    volatile uint32_t* reg(uint32_t off) const {
        return reinterpret_cast<volatile uint32_t*>(base_addr_ + off);
    }

    void wait_idle() const {
        // 评审记录：此处 nop 忙等与 SPI 路径 wait_transmit_complete() 的 wfi
        // 写法不一致。已核算改为 wfi 仅省 0.67% CPU，且 IOM 无时钟门控、
        // 无省电收益，故暂不改动，避免后人重复评估。
        //
        // 【L2a 埋点】只在 BOARD_HAS_DWT 时加计时代码，且不改动自旋行为本身
        // （循环条件、timeout 初值、nop 一律原样保留）。miband8 的 BOARD_HAS_DWT
        // 由 board.h 显式定义为 1；M0+（nucleo 定义为 0）与 host test 全部编译掉，
        // 零代码、零 .bss 占用。
#if BOARD_HAS_DWT
        const uint32_t t0 = Arch::get_cycle();
#endif
        volatile uint32_t* status = reg(AM_REG_IOM_STATUS);
        uint32_t timeout = 1000000u;
        while ((*status & IOM_STATUS_IDLE) == 0 && --timeout) {
#if !defined(AURORA_HOST_TEST)
            __asm__ volatile("nop");
#endif
        }
#if BOARD_HAS_DWT
        // 累加本轮忙等耗时（含 timeout 提前退出的异常情形）。
        // 单核 + 任务上下文，无并发写者；`+=` 非原子仅在真并发下丢计数，
        // 而 I2C 路径当前只由 watch_app / miband_kernel 两个任务串行调用。
        g_i2c_busy_cycles += (Arch::get_cycle() - t0);
        g_i2c_wait_idle_calls++;
        // timeout 耗尽 = IOM 未在 1000000 次轮询内回报 IDLE，属硬件异常
        // （总线被拉死/从机 NACK 卡死/时钟门控误关）。单独计数以便现场区分
        // 「正常等传输完成」与「异常空转」——后者会白烧 CPU 却不产出数据。
        if (timeout == 0) {
            g_i2c_idle_timeouts++;
        }
#endif
    }

    void ensure_configured() {
        if (configured_)
            return;
        configured_ = true;

        // 配置为 I2C Master 模式 (400kHz Fast Mode)
        *reg(AM_REG_IOM_CFG) = IOM_CFG_FUNCSEL_I2C | IOM_CFG_MASTER;
        *reg(AM_REG_IOM_CLKCFG) = IOM_CLKCFG_HSEN | (7u << IOM_CLKCFG_HSDIV_SHIFT);
        *reg(AM_REG_IOM_CFG) = IOM_CFG_FUNCSEL_I2C | IOM_CFG_MASTER | IOM_CFG_ENABLE;
    }

    // ---- 以下为私有事务实现：调用方必须已持有 bus_lock_ ----
    // 拆出 _impl 是为了支持 read_reg 在单次持锁下复用写/读路径。
    // 这些函数绝不自行加锁（锁非递归，重入即自死锁）。

    bool write_impl(uint8_t dev_addr, const uint8_t* data, size_t len) {
        if (!data || len == 0)
            return false;
        ensure_configured();
        wait_idle();

        for (size_t i = 0; i < len; ++i) {
            *reg(AM_REG_IOM_FIFOPUSH) = data[i];
        }

        uint32_t cmd = (0x1u << 16) | (static_cast<uint32_t>(dev_addr) << 8) | static_cast<uint32_t>(len & 0xFF);
        *reg(AM_REG_IOM_CMD) = cmd;

        wait_idle();
        return true;
    }

    bool read_impl(uint8_t dev_addr, uint8_t* data, size_t len) {
        if (!data || len == 0)
            return false;
        ensure_configured();
        wait_idle();

        uint32_t cmd = (0x2u << 16) | (static_cast<uint32_t>(dev_addr) << 8) | static_cast<uint32_t>(len & 0xFF);
        *reg(AM_REG_IOM_CMD) = cmd;

        wait_idle();

        for (size_t i = 0; i < len; ++i) {
            data[i] = static_cast<uint8_t>(*reg(AM_REG_IOM_FIFOPOP) & 0xFF);
        }
        return true;
    }

public:
    explicit Apollo3I2cHal(uint32_t base_addr = AM_HAL_IOM1_BASE)
        : base_addr_(base_addr), configured_(false) {}

    bool write(uint8_t dev_addr, const uint8_t* data, size_t len) override {
        I2cBusGuard guard(bus_lock_);
        if (!guard.acquired())
            return false;
        return write_impl(dev_addr, data, len);
    }

    bool write_reg(uint8_t dev_addr, uint8_t reg_addr, const uint8_t* data, size_t len) override {
        I2cBusGuard guard(bus_lock_);
        if (!guard.acquired())
            return false;

        // write_reg 的寄存器地址不走 write_impl：write_impl 在 len>1 时不会
        // 自行插入寄存器地址，语义是纯数据写入，与 write_reg 不同。
        ensure_configured();
        wait_idle();

        *reg(AM_REG_IOM_FIFOPUSH) = reg_addr;
        if (data && len > 0) {
            for (size_t i = 0; i < len; ++i) {
                *reg(AM_REG_IOM_FIFOPUSH) = data[i];
            }
        }

        uint32_t total_len = static_cast<uint32_t>(len + 1);
        uint32_t cmd = (0x1u << 16) | (static_cast<uint32_t>(dev_addr) << 8) | (total_len & 0xFF);
        *reg(AM_REG_IOM_CMD) = cmd;

        wait_idle();
        return true;
    }

    bool read(uint8_t dev_addr, uint8_t* data, size_t len) override {
        I2cBusGuard guard(bus_lock_);
        if (!guard.acquired())
            return false;
        return read_impl(dev_addr, data, len);
    }

    bool read_reg(uint8_t dev_addr, uint8_t reg_addr, uint8_t* data, size_t len) override {
        // 单次持锁覆盖「写寄存器地址 + 读回数据」整个组合事务。
        // 原实现调用公开的 write()/read()，加锁后必然自死锁（非递归锁）。
        I2cBusGuard guard(bus_lock_);
        if (!guard.acquired())
            return false;
        if (!write_impl(dev_addr, &reg_addr, 1))
            return false;
        return read_impl(dev_addr, data, len);
    }
};

II2cHal* get_i2c_hal(int bus_id) {
    if (bus_id == 0) {
        static Apollo3I2cHal i2c_hal_0(AM_HAL_IOM0_BASE);
        return &i2c_hal_0;
    }
    static Apollo3I2cHal i2c_hal_1(AM_HAL_IOM1_BASE);
    return &i2c_hal_1;
}

// ============================================================================
// L2a 埋点读取接口
//
// 供 HIL / 调试台经 UART 命令读取。刻意做成 extern "C" + POD 返回，
// 便于在 gdb 里直接 print，也避免把Metrics 体系拉进 HAL 层（保持分层）。
//
// 注意【单位换算】：返回值是 DWT 周期数（@96MHz），除以 96 得µs。
// 不要用 LatencyRecorder 的 get_avg_us() 路径——那会顺带把计数塞进
// 100 深度的 history 数组，HAL 层不该为此付400B .bss。
// ============================================================================
#if BOARD_HAS_DWT
extern "C" {

uint32_t aurora_i2c_busy_cycles() { return g_i2c_busy_cycles; }
uint32_t aurora_i2c_wait_idle_calls() { return g_i2c_wait_idle_calls; }
uint32_t aurora_i2c_idle_timeouts() { return g_i2c_idle_timeouts; }

// 单次 wait_idle 平均忙等周期数。分母为 0 时返回 0（避免除零）。
uint32_t aurora_i2c_busy_cycles_avg() {
    uint32_t calls = g_i2c_wait_idle_calls;
    if (calls == 0)
        return 0;
    return g_i2c_busy_cycles / calls;
}

// 清零，供「测一个固定窗口」用：读快照 → 跑 N 秒 → 再读 → 相减。
// 单独提供而非让读函数自动清零，是为了让调用方决定观察窗口边界。
void aurora_i2c_metrics_reset() {
    g_i2c_busy_cycles = 0;
    g_i2c_wait_idle_calls = 0;
    g_i2c_idle_timeouts = 0;
}

} // extern "C"
#endif // BOARD_HAS_DWT

// ========================================================
// Apollo3 Secure Storage — customer OTP 密钥读取
//
// 从 customer OTP 区域读取每设备唯一的 SoftBus 预共享密钥。
// 未烧录 (全 0xFF) 或魔术字不匹配时 fail-closed 返回 false，
// 绝不回退到默认/共享密钥。
// ========================================================
class Apollo3SecureStorageHal : public ISecureStorageHal {
public:
    bool is_provisioned() override {
        return read_record(/*out_key=*/nullptr, /*out_version=*/nullptr);
    }

    bool read_softbus_key(uint8_t key[32], uint32_t* version) override {
        return read_record(key, version);
    }

private:
    // 从 OTP 读取密钥记录并校验魔术字与烧录状态。
    // out_key / out_version 可为 nullptr（仅探测 provision 状态）。
    bool read_record(uint8_t* out_key, uint32_t* out_version) {
        const volatile uint8_t* otp =
            reinterpret_cast<const volatile uint8_t*>(OTP_CUSTOMER_BASE + OTP_SOFTBUS_KEY_OFFSET);

        // 读魔术字 (小端)
        uint32_t magic = 0;
        for (int i = 0; i < 4; ++i)
            magic |= static_cast<uint32_t>(otp[i]) << (8 * i);

        // 未烧录时 OTP 为全 0xFF；魔术字不匹配即视为未供应。
        if (magic != OTP_SOFTBUS_KEY_MAGIC)
            return false;

        // 读版本 (小端)
        uint32_t ver = 0;
        for (int i = 0; i < 4; ++i)
            ver |= static_cast<uint32_t>(otp[4 + i]) << (8 * i);

        // 读 32 字节密钥
        if (out_key) {
            for (int i = 0; i < 32; ++i)
                out_key[i] = otp[8 + i];
        }
        if (out_version)
            *out_version = ver;
        return true;
    }
};

// ============================================================================
// Apollo3 Blue UART1 寄存器映射 (base 0x4001D000, 专用于 BLE 控制器 HCI 通信)
// ============================================================================
#define AM_HAL_UART1_BASE 0x4001D000UL
#define AM_REG_UART_DR    0x000 // 数据寄存器 (读 RX / 写 TX)
#define AM_REG_UART_RSR   0x004 // 接收状态寄存器
#define AM_REG_UART_FR    0x018 // 标志寄存器 (bit 4 = RXFE 空, bit 5 = TXFF 满)
#define AM_REG_UART_ILPR  0x020
#define AM_REG_UART_IBRD  0x024 // 整数波特率分频
#define AM_REG_UART_FBRD  0x028 // 小数波特率分频
#define AM_REG_UART_LCRH  0x02C // 线控制 (bit 4 = FEN 启用 FIFO, [6:5] = 8 bit)
#define AM_REG_UART_CR    0x030 // 控制寄存器 (bit 0 = UARTEN, bit 8 = TXE, bit 9 = RXE)
#define AM_REG_UART_IER   0x038 // 中断使能 (bit 4 = RXIM, bit 6 = RTIM 接收超时)
#define AM_REG_UART_MIS   0x044 // 屏蔽中断状态
#define AM_REG_UART_ICR   0x048 // 中断清除寄存器

class Apollo3BleUartDevice : public CharDevice {
public:
    Apollo3BleUartDevice() : CharDevice("uart_ble") {}

    int open() override { return 0; }
    int close() override { return 0; }

    int read(char* buf, int len, int flags, void* user) override {
        (void)buf; (void)len; (void)flags; (void)user;
        return 0; // 异步中断驱动 (通过 UART1_IRQHandler -> feed_rx_byte 喂数)
    }

    int write(const char* buf, int len, int flags, void* user) override {
        (void)flags; (void)user;
        if (!buf || len <= 0) return 0;
        volatile uint32_t* fr = reinterpret_cast<volatile uint32_t*>(AM_HAL_UART1_BASE + AM_REG_UART_FR);
        volatile uint32_t* dr = reinterpret_cast<volatile uint32_t*>(AM_HAL_UART1_BASE + AM_REG_UART_DR);
        for (int i = 0; i < len; ++i) {
            // 等待 TX FIFO 未满 (bit 5 TXFF == 0)
            uint32_t timeout = 100000;
            while ((*fr & (1u << 5)) != 0 && --timeout) {
#if !defined(AURORA_HOST_TEST)
                __asm__ volatile("nop");
#endif
            }
            *dr = static_cast<uint8_t>(buf[i]);
        }
        return len;
    }
};

static Apollo3BleUartDevice s_ble_uart_dev;
static auroraos::ble::hci::HciUartTransport s_hci_uart_transport(&s_ble_uart_dev);

extern "C" void board_ble_uart_init() {
    volatile uint32_t* cr   = reinterpret_cast<volatile uint32_t*>(AM_HAL_UART1_BASE + AM_REG_UART_CR);
    volatile uint32_t* lcrh = reinterpret_cast<volatile uint32_t*>(AM_HAL_UART1_BASE + AM_REG_UART_LCRH);
    volatile uint32_t* ibrd = reinterpret_cast<volatile uint32_t*>(AM_HAL_UART1_BASE + AM_REG_UART_IBRD);
    volatile uint32_t* fbrd = reinterpret_cast<volatile uint32_t*>(AM_HAL_UART1_BASE + AM_REG_UART_FBRD);
    volatile uint32_t* ier  = reinterpret_cast<volatile uint32_t*>(AM_HAL_UART1_BASE + AM_REG_UART_IER);

    // 禁用 UART 进行配置
    *cr = 0;

    // 波特率配置 (115200 at 24MHz UART clock: IBRD = 13, FBRD = 1)
    *ibrd = 13;
    *fbrd = 1;

    // 8 位数据, 1 停止位, 启用 FIFO (FEN = bit 4, WLEN = 0x3 << 5)
    *lcrh = (0x3u << 5) | (1u << 4);

    // 使能 RX 中断 (RXIM = bit 4) 与 接收超时中断 (RTIM = bit 6)
    *ier = (1u << 4) | (1u << 6);

    // 使能 UART, TXE, RXE
    *cr = (1u << 0) | (1u << 8) | (1u << 9);

    // 注册全局 HCI 传输层并初始化
    auroraos::ble::hci::g_hci_transport = &s_hci_uart_transport;
    s_hci_uart_transport.init();
}

ISecureStorageHal* get_secure_storage_hal() {
    static Apollo3SecureStorageHal secure;
    return &secure;
}

} // namespace hal
} // namespace auroraos

extern "C" void board_ble_uart_feed_rx(uint8_t byte) {
    if (auroraos::ble::hci::g_hci_transport) {
        auroraos::ble::hci::g_hci_transport->feed_rx_byte(byte);
    }
}

extern "C" void board_ble_uart_feed_rx_bytes(const uint8_t* buf, size_t len) {
    if (auroraos::ble::hci::g_hci_transport) {
        auroraos::ble::hci::g_hci_transport->feed_rx_bytes(buf, len);
    }
}

// ============================================================================
// Apollo3 BLE UART 中断服务例程 (ISR)
//
// 负责在硬件接收到 BLE Controller 的 H4 字节流时直接读取并喂入 HciUartTransport
// ============================================================================
extern "C" void UART1_IRQHandler(void) {
    volatile uint32_t* mis = reinterpret_cast<volatile uint32_t*>(AM_HAL_UART1_BASE + AM_REG_UART_MIS);
    volatile uint32_t* icr = reinterpret_cast<volatile uint32_t*>(AM_HAL_UART1_BASE + AM_REG_UART_ICR);
    volatile uint32_t* fr  = reinterpret_cast<volatile uint32_t*>(AM_HAL_UART1_BASE + AM_REG_UART_FR);
    volatile uint32_t* dr  = reinterpret_cast<volatile uint32_t*>(AM_HAL_UART1_BASE + AM_REG_UART_DR);

    uint32_t status = *mis;
    *icr = status; // 清除中断状态标志

    // 只要 RX FIFO 非空 (RXFE bit 4 == 0)，循环读出并逐字节驱动 H4 状态机
    while ((*fr & (1u << 4)) == 0) {
        uint8_t byte = static_cast<uint8_t>(*dr & 0xFF);
        board_ble_uart_feed_rx(byte);
    }
}
