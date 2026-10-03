#ifndef AURORA_HAL_I2C_BUS_HPP
#define AURORA_HAL_I2C_BUS_HPP

#include <stdint.h>
#include "../kernel/core/mutex.hpp"

// ============================================================================
// hal/i2c_bus.hpp — I2C 总线级串行化原语
//
// 存在理由：Apollo3 IOM 的 FIFO/CMD 是共享寄存器外设，同一物理总线上有多个
// 客户端（GT316 触控 0x14、BHI260AP 加速度计 0x28 等）。两个不同优先级的任务
// 并发发起事务时，会交错写 FIFO/CMD 寄存器，把事务撕裂成偶发 NACK 或错数据。
// 这是数据正确性缺陷，不是性能问题。
//
// 层次说明（AGENTS.md §5）：本文件让 HAL 反向依赖 kernel 的 Mutex。
// 同类先例：drivers/usb/usb_host.hpp:7、drivers/storage/flash_device.hpp:6、
// drivers/rf/spectrum_monitor.hpp:25 均直接包含 kernel/core/mutex.hpp，
// 用内核 Mutex 保护驱动层共享状态是该仓库既定的做法，此处保持一致。
//
// 硬性约束：无异常、无 RTTI、无动态分配。锁的成员全部为常量初始化，
// 不引入 .bss 之外的启动期动态初始化，可安全用于 freestanding 目标与
// -fno-threadsafe-statics 构建。
// ============================================================================

namespace auroraos {
namespace hal {

class I2cBusLock;

// ---------------------------------------------------------------------------
// I2cBusGuard — RAII 作用域锁
//
// 构造时尝试获取总线锁，析构时释放。任何 return 分支（正常返回、参数校验
// 失败、事务中途失败）离开作用域时都会自动释放，不会漏解锁。
//
// 必须以具名对象持有（见 AGENTS.md §15 禁止立即销毁的临时守卫）：
//     I2cBusGuard guard(bus_lock_);
//     if (!guard.acquired()) return false;
//     ... 完整事务 ...
// ---------------------------------------------------------------------------
class I2cBusGuard {
public:
    explicit I2cBusGuard(I2cBusLock& bus) noexcept;
    ~I2cBusGuard() noexcept;

    // 是否真正持有锁。false 表示走了有界降级路径（见 I2cBusLock 注释），
    // 此时调用方必须放弃本次事务并返回错误。
    bool acquired() const noexcept { return acquired_; }

    // 禁止拷贝与移动：守卫与作用域严格一一对应。
    I2cBusGuard(const I2cBusGuard&) = delete;
    I2cBusGuard& operator=(const I2cBusGuard&) = delete;

private:
    I2cBusLock& bus_;
    bool acquired_;
};

// ---------------------------------------------------------------------------
// I2cBusLock — 单个 I2C HAL 实例的总线互斥锁
//
// 作用域是「实例」而非全局单例：get_i2c_hal() 对同一物理总线返回同一
// static 实例，把锁挂到实例上即可覆盖该总线的全部客户端；挂成全局单例
// 则会让不同板型上互不相干的 I2C 实例互相阻塞。
//
// 关于「有界降级」：本类不使用忙等自旋。kernel 的 Mutex::lock(timeout) 在
// 竞争时是通过 Scheduler::schedule() 让出 CPU 的阻塞等待（不是自旋），
// 并以 TimerManager tick 计时（1 tick = 1 ms，Scheduler::TICK_RATE_HZ=1000）。
// 因此「有界」由超时参数保证：超时后 lock() 必定返回 false，环路终止。
//
// 之所以不在 Mutex 之上再套一层忙等自旋：单核上持锁者若优先级更低，等待方
// 自旋会占住本可用于让持锁者运行的唯一 CPU，把优先级反转变成真死锁 ——
// 正是有界降级要避免的后果。阻塞式等待 + PIP 才是正解。
// ---------------------------------------------------------------------------
class I2cBusLock {
public:
    // 默认获取超时（tick，1 tick = 1 ms）。
    //
    // 一次 400kHz I2C 事务（~10 字节）约 250µs；PIP 会在竞争时提权持锁者，
    // 正常竞争应在 1ms 内解决。20 tick 留了约 20 倍裕量用于「持锁者被抢占」
    // 等瞬态，同时远低于 sensor_ble_daemon 的 40ms 采样周期与看门狗阈值，
    // 确保该超时只在真正的丢失唤醒/持锁者卡死时才触发，不误报。
    static constexpr uint32_t kDefaultAcquireTimeoutTicks = 20;

    explicit I2cBusLock(uint32_t acquire_timeout_ticks = kDefaultAcquireTimeoutTicks) noexcept
        : timeout_ticks_(acquire_timeout_ticks) {}

    // 天花板保持默认 TaskPriority::Idle（即不额外提权）。I2C 由 Realtime 与
    // High 任务共同访问，置高天花板会让任何持锁者都被拉到 Realtime，反而
    // 破坏调度语义。这里只依赖 PIP 按实际等待者优先级提权。

    // 诊断计数：因超时走降级路径的次数。用于现场定位「总线上有持锁者卡死」。
    uint32_t degraded_count() const noexcept { return degraded_count_; }

    // 成功获取锁的次数。用于确认加锁确实生效（正常路径下应与事务次数相等）。
    uint32_t acquire_count() const noexcept { return acquire_count_; }

    // 复位诊断计数。仅供测试与故障恢复使用。
    void reset_diagnostics() noexcept {
        degraded_count_ = 0;
        acquire_count_ = 0;
    }

private:
    friend class I2cBusGuard;

    bool try_acquire() noexcept {
        if (!mutex_.lock(timeout_ticks_)) {
            ++degraded_count_;
            return false;
        }
        ++acquire_count_;
        return true;
    }

    void release() noexcept {
        mutex_.unlock();
    }

    // 优先级继承 + 死锁闭环检测的内核 Mutex。
    Mutex mutex_;

    // 有界等待预算（tick）。超时保证 lock() 一定返回，杜绝无限等待。
    uint32_t timeout_ticks_;

    // 诊断计数。freestanding 下不使用动态日志设施，固定计数器即可覆盖
    // 「是否发生过降级」这一现场定位需求（AGENTS.md §22 要求硬件失败不得
    // 静默忽略，此处通过可读计数器满足）。
    volatile uint32_t degraded_count_ = 0;
    volatile uint32_t acquire_count_ = 0;
};

// ---------------------------------------------------------------------------
// I2cBusGuard 实现
// ---------------------------------------------------------------------------
inline I2cBusGuard::I2cBusGuard(I2cBusLock& bus) noexcept
    : bus_(bus), acquired_(bus.try_acquire()) {}

inline I2cBusGuard::~I2cBusGuard() noexcept {
    if (acquired_) {
        bus_.release();
    }
}

} // namespace hal
} // namespace auroraos

#endif // AURORA_HAL_I2C_BUS_HPP
