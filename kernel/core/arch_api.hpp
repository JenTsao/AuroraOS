#ifndef ARCH_API_HPP
#define ARCH_API_HPP

#include <stdint.h>

// =====================================================================
// HAL 架构无关接口契约
// 调度器与内核逻辑层只依赖此处的声明，不感知具体异常返回码、寄存器魔数
// 或内联汇编。各架构在 arch_impl.hpp 中提供 inline 实现，构建系统按
// -DBOARD 自动将其加入 include 路径。
// =====================================================================
namespace Arch {
// ── 临界区 / 低功耗 ────────────────────────────────────────────
void disable_interrupts();
void enable_interrupts();
uint32_t irq_save();
void irq_restore(uint32_t flags);
void wait_for_interrupt();

// ── Tickless Idle 时钟管理 ────────────────────────────────────
void disable_systick();
void enable_systick();
void start_wakeup_timer(uint32_t ticks);
uint32_t stop_wakeup_timer();

// ── 性能分析与度量 ────────────────────────────────────────────
uint32_t get_cycle();
uint32_t get_cycles_per_us();

// ── 系统节拍定时器 ────────────────────────────────────────────
// 配置 SysTick 产生周期性中断（系统心跳），hz = 每秒中断次数
// 必须在全局开中断 (cpsie i) 之前调用：先禁用再配置，避免配置中途触发中断
void systick_init(uint32_t hz);

// ── 上下文切换 ────────────────────────────────────────────────
// 触发软中断请求调度器做硬件级上下文切换 (Cortex-M 上即 pending PendSV)
void trigger_context_switch();

// 伪造硬件异常压栈的初始线程栈帧，返回新的栈顶指针
uint32_t* init_thread_stack(void (*task_entry)(void), uint32_t* stack_space, uint32_t stack_size);

// 从特权 main 上下文引导进入第一个任务：切换 PSP/CONTROL、开中断、跳入入口
// 与 init_thread_stack() 配套，是调度器唯一「无中生有」建立任务上下文的入口
[[noreturn]] void start_first_task(uint32_t* stack_ptr, void (*entry_point)(), uint32_t privilege);

// ── 特权级管理 ───────────────────────────────────────────────
// 动态修改当前处理器特权级（0 = Privileged, 1 = Unprivileged）
void set_privilege(uint32_t privilege);

// ── 内存与地址空间 ───────────────────────────────────────────
// 切换当前进程/任务的虚拟地址空间页表基址（MMU TTBR0/satp，在 MPU 架构上为空操作）
void switch_address_space(uintptr_t pgdir_base);

// ── 内存保护单元 (MPU/PMP) ──────────────────────────────────────────
struct MpuRegion {
    uintptr_t base;
    uint8_t size_pow2;
    uint32_t ap;
    bool execute_never;
    bool is_device;
};

void mpu_configure_region(uint8_t idx, const MpuRegion& region) noexcept;
void mpu_enable() noexcept;
void mpu_disable() noexcept;

// ── 硬件位图加速与前导零计算 ───────────────────────────────────────
// 计算前导零 (Count Leading Zeros) — 硬件指令 (Cortex-M3/M4/M4F/AArch64: CLZ; x86: BSR/LZCNT)
inline uint32_t clz(uint32_t val) noexcept {
#if defined(__GNUC__) || defined(__clang__)
    return val ? static_cast<uint32_t>(__builtin_clz(val)) : 32u;
#elif defined(_MSC_VER)
    unsigned long index;
    return _BitScanReverse(&index, val) ? (31u - index) : 32u;
#else
    if (val == 0) return 32u;
    uint32_t n = 0;
    if ((val & 0xFFFF0000u) == 0) { n += 16; val <<= 16; }
    if ((val & 0xFF000000u) == 0) { n += 8;  val <<= 8;  }
    if ((val & 0xF0000000u) == 0) { n += 4;  val <<= 4;  }
    if ((val & 0xC0000000u) == 0) { n += 2;  val <<= 2;  }
    if ((val & 0x80000000u) == 0) { n += 1; }
    return n;
#endif
}

// 查找位图中最高有效位 (MSB / 最高优先级 0..31)，若位图为 0 则返回 -1
inline int32_t find_highest_bit(uint32_t bitmask) noexcept {
    if (bitmask == 0) return -1;
#if defined(__GNUC__) || defined(__clang__)
    return 31 - __builtin_clz(bitmask);
#elif defined(_MSC_VER)
    unsigned long index;
    return _BitScanReverse(&index, bitmask) ? static_cast<int32_t>(index) : -1;
#else
    return 31 - static_cast<int32_t>(clz(bitmask));
#endif
}
} // namespace Arch

// 拉入当前架构的内联实现 (arch/arm/cortex-m/cm4/arch_impl.hpp 等)
#include "arch_impl.hpp"

#endif // ARCH_API_HPP
