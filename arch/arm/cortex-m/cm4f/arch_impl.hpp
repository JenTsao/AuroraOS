#ifndef AURORA_ARCH_CM4F_IMPL_HPP
#define AURORA_ARCH_CM4F_IMPL_HPP

#include <stdint.h>
#include "board.h" // 包含 SYSTEM_CORE_CLOCK

// ========================================================
// Cortex-M4F 核心控制寄存器定义
// ========================================================
#define SCB_CPACR       (*((volatile uint32_t *)0xE000ED88)) // 协处理器访问控制寄存器
#define SCB_ICSR        (*((volatile uint32_t *)0xE000ED04)) // 中断控制和状态寄存器
#define SHPR3_PRI_14    (*((volatile uint8_t  *)0xE000ED22)) // PendSV 优先级寄存器
#define SHPR3_PRI_15    (*((volatile uint8_t  *)0xE000ED23)) // SysTick 优先级寄存器

namespace Arch {

    // =====================================================================
    // Cortex-M4F 核心寄存器定义 (SysTick)
    // =====================================================================
    inline volatile uint32_t* const SYST_CSR = reinterpret_cast<volatile uint32_t*>(0xE000E010);
    inline volatile uint32_t* const SYST_RVR = reinterpret_cast<volatile uint32_t*>(0xE000E014);
    inline volatile uint32_t* const SYST_CVR = reinterpret_cast<volatile uint32_t*>(0xE000E018);

    // =====================================================================
    // Ambiq Apollo3 STIMER 寄存器定义 (运行在 32.768kHz 晶振下)
    // 专门用于在 Deep Sleep 期间提供极低功耗的时间基准
    // =====================================================================
    inline volatile uint32_t* const STIMER_STMRCTRL = reinterpret_cast<volatile uint32_t*>(0x40008080);
    inline volatile uint32_t* const STIMER_STMRCNT  = reinterpret_cast<volatile uint32_t*>(0x40008084);
    inline volatile uint32_t* const STIMER_SCMPR0   = reinterpret_cast<volatile uint32_t*>(0x40008090);

    // =====================================================================
    // Cortex-M DWT 寄存器 (Cycle Counter)
    // =====================================================================
    inline volatile uint32_t* const DEMCR    = reinterpret_cast<volatile uint32_t*>(0xE000EDFC);
    inline volatile uint32_t* const DWT_CTRL = reinterpret_cast<volatile uint32_t*>(0xE0001000);
    inline volatile uint32_t* const DWT_CYCCNT= reinterpret_cast<volatile uint32_t*>(0xE0001004);

    // 记录进入休眠时的低频时钟绝对计数值
    inline uint32_t sleep_start_cycle = 0;

    // ========================================================
    // 开启 M4F 硬件浮点运算单元 (FPU)
    // ========================================================
    inline void enable_fpu() {
        // 设置 CP10 和 CP11 协处理器的访问权限为全权访问 (Full Access)
        // 这将允许执行所有的硬件浮点指令
        SCB_CPACR |= (0xF << 20);
        __asm__ volatile ("dsb\n\t" "isb\n\t" : : : "memory");
    }

    // ========================================================
    // 初始化系统架构级中断与调度环境
    // ========================================================
    inline void init() {
        enable_fpu();

        // 将 PendSV 和 SysTick 的中断优先级设置为最低 (0xFF)
        // 确保系统的上下文切换只在没有其他 high-priority 硬件中断抢占时才发生
        SHPR3_PRI_14 = 0xFF;
        SHPR3_PRI_15 = 0xFF;

        // 启用 DWT 循环计数器，用于 get_cycle()
        *DEMCR |= 0x01000000;      // TRCENA = 1
        *DWT_CYCCNT = 0;           // 清零计数器
        *DWT_CTRL |= 1;            // CYCCNTENA = 1
    }

    inline uint32_t get_cycle() {
        return *DWT_CYCCNT;
    }

    inline uint32_t get_cycles_per_us() {
        return SYSTEM_CORE_CLOCK / 1000000;
    }

    // ========================================================
    // 底层内联汇编控制
    // ========================================================
    inline void disable_interrupts() {
        __asm__ volatile ("cpsid i" : : : "memory");
    }

    inline void enable_interrupts() {
        __asm__ volatile ("cpsie i" : : : "memory");
    }

    inline uint32_t irq_save() {
        uint32_t flags;
        __asm__ volatile (
            "mrs %0, primask \n\t"
            "cpsid i         \n\t"
            : "=r" (flags)
            :
            : "memory"
        );
        return flags;
    }

    inline void irq_restore(uint32_t flags) {
        __asm__ volatile (
            "msr primask, %0 \n\t"
            :
            : "r" (flags)
            : "memory"
        );
    }

    inline void wait_for_interrupt() {
        // 睡眠三步曲：
        // 1. dsb (Data Synchronization Barrier): 确保所有写内存操作在睡眠前彻底完成
        // 2. wfi (Wait For Interrupt): 核心下电，进入休眠
        // 3. isb (Instruction Synchronization Barrier): 唤醒后清空流水线，防止预取指令引发错乱
        __asm__ volatile ("dsb\n\t" "wfi\n\t" "isb\n\t" : : : "memory");
    }

    // ---------------------------------------------------------------------
    // SysTick 启停控制 (Tickless 的核心操作)
    // ---------------------------------------------------------------------
    inline void disable_systick() {
        // 清除 Bit 0 (ENABLE) 和 Bit 1 (TICKINT)，彻底掐断高频心跳中断
        *SYST_CSR &= ~0x00000003;
        // 清空当前计数器，防止恢复时产生多余的触发
        *SYST_CVR = 0; 
    }

    inline void enable_systick() {
        // 恢复 Bit 0 (ENABLE) 和 Bit 1 (TICKINT)，使用处理器时钟 (Bit 2)
        *SYST_CSR |= 0x00000007;
    }

    // ---------------------------------------------------------------------
    // 底层唤醒定时器配置 (连接真实物理世界的时间扭曲)
    // ---------------------------------------------------------------------
    inline void start_wakeup_timer(uint32_t ticks_to_sleep) {
        // 我们系统的 OS Tick = 1ms (1000Hz)
        // Apollo3 STIMER 运行在超低功耗的 32768Hz 时钟域
        // 换算公式：cycles = (ticks * 32768) / 1000
        uint32_t sleep_cycles = (ticks_to_sleep * 32768) / 1000;

        // 拍下 CPU 准备闭上眼睛的那一刻的绝对时间戳
        sleep_start_cycle = *STIMER_STMRCNT;

        // 将唤醒阈值写入比较寄存器 0
        *STIMER_SCMPR0 = sleep_start_cycle + sleep_cycles;

        // 假设这里还包含开启 STIMER 比较器 0 对应的 NVIC 中断使能位
        // (取决于具体的 Apollo3 HAL 宏配置)
    }

    inline uint32_t stop_wakeup_timer() {
        // CPU 刚刚睁开眼睛，立刻抓取当前的绝对时间戳
        uint32_t wake_cycle = *STIMER_STMRCNT;

        // 假设这里关闭了对应的唤醒中断使能
        // ...

        // 计算物理晶振真实流逝的周期数 (处理 32 位计数器的硬件翻转)
        uint32_t elapsed_cycles;
        if (wake_cycle >= sleep_start_cycle) {
            elapsed_cycles = wake_cycle - sleep_start_cycle;
        } else {
            elapsed_cycles = (0xFFFFFFFF - sleep_start_cycle) + wake_cycle + 1;
        }

        // 将物理周期逆向换算回 OS Ticks (ms) 准备交给内核去快进补偿
        // ticks = (cycles * 1000) / 32768
        uint32_t actual_ticks = (elapsed_cycles * 1000) / 32768;

        return actual_ticks;
    }

    // ========================================================
    // 触发软中断，请求上下文切换
    // ========================================================
    inline void trigger_context_switch() {
        SCB_ICSR |= (1 << 28); // 触发 PendSV 中断
        __asm__ volatile ("isb\n\t" "dsb\n\t" : : : "memory");
    }

    // ========================================================
    // M4F 任务栈初始化构造器
    // ========================================================
    // 为新创建的线程伪造一个中断返回时的堆栈快照。
    // 对于 M4F 架构，除了 R0-R12, LR, PC, xPSR 外，如果硬件使用了 FPU 延迟压栈，
    // EXC_RETURN 的值必须是 0xFFFFFFFD (不带FPU压栈) 或 0xFFFFFFED (带FPU压栈)。
    // 这里初始创建任务时，我们按照标准线程模式(Thread Mode + PSP + 无初始FPU快照)配置。
    inline uint32_t* init_task_stack(uint32_t* stack_top, void (*entry_point)(void)) {
        uint32_t* stk = stack_top;

        // 模拟硬件自动压栈的上下文 (Exception Frame)
        stk--; *stk = 0x01000000;                      // xPSR: 设置 Thumb 位 (T-bit)
        stk--; *stk = reinterpret_cast<uint32_t>(entry_point); // PC: 任务入口地址
        stk--; *stk = 0xFFFFFFFD;                      // LR (R14): 设为 EXC_RETURN，表明返回后使用 PSP，并且初始不包含扩展的 FPU 栈帧
        stk--; *stk = 0x12121212;                      // R12
        stk--; *stk = 0x03030303;                      // R3
        stk--; *stk = 0x02020202;                      // R2
        stk--; *stk = 0x01010101;                      // R1
        stk--; *stk = 0x00000000;                      // R0: 传参可以放这里

        // 模拟软件手动压栈的上下文 (被调用者保存寄存器)
        stk--; *stk = 0x11111111;                      // R11
        stk--; *stk = 0x10101010;                      // R10
        stk--; *stk = 0x09090909;                      // R9
        stk--; *stk = 0x08080808;                      // R8
        stk--; *stk = 0x07070707;                      // R7
        stk--; *stk = 0x06060606;                      // R6
        stk--; *stk = 0x05050505;                      // R5
        stk--; *stk = 0x04040404;                      // R4

        // 最终返回当前的栈顶指针，存入 TCB 中
        return stk;
    }

    inline uint32_t* init_thread_stack(void (*task_entry)(void), uint32_t* stack_space, uint32_t stack_size) {
        uint32_t* top = stack_space + (stack_size / sizeof(uint32_t));
        return init_task_stack(top, task_entry);
    }

    // ========================================================
    // SysTick 初始化
    // ========================================================
    inline void systick_init(uint32_t hz) {
        *SYST_CSR = 0;                                        // 1. 禁用 SysTick
        *SYST_RVR = (SYSTEM_CORE_CLOCK / hz) - 1;             // 2. 设定重载周期
        *SYST_CVR = 0;                                        // 3. 清零当前值
        *SYST_CSR = (1UL << 2) | (1UL << 1) | (1UL << 0);     // 4. 启动 (CLKSOURCE | TICKINT | ENABLE)
    }

    // ========================================================
    // 引导第一个任务
    // ========================================================
    [[noreturn]] inline void start_first_task(uint32_t* stack_ptr, void (*entry_point)(), uint32_t privilege = 0) {
        __asm__ volatile (
            "ldm  %0!, {r4-r11}  \n\t"  // 弹出 R4-R11（init_thread_stack 预留的）
            "msr  psp, %0        \n\t"  // 将更新后的指针写入 PSP
            "mov  r0, #2         \n\t"  // CONTROL = 0b10: Thread mode, use PSP
            "orr  r0, r0, %2     \n\t"  // 合并特权级 (0: Kernel->2, 1: User->3)
            "msr  control, r0   \n\t"
            "isb                 \n\t"  // 指令同步屏障
            "cpsie i             \n\t"  // 全局开中断
            "bx   %1             \n\t"  // 跳入任务入口（直接 bx，不保存 LR）
            : : "r"(stack_ptr),
                "r"(reinterpret_cast<uint32_t>(entry_point)),
                "r"(privilege)
            : "r0", "r4", "r5", "r6", "r7", "r8", "r9", "r10", "r11", "memory"
        );
        __builtin_unreachable();
    }

    // =====================================================================
    // ARM PMSAv7 (Cortex-M3/M4/M7/M4F) 内存保护单元实现
    // 寄存器模型: RNR 选区 / RBAR 地址 / RASR 属性
    // =====================================================================
    static constexpr uintptr_t MPU_CTRL = 0xE000ED94U;
    static constexpr uintptr_t MPU_RNR  = 0xE000ED98U;
    static constexpr uintptr_t MPU_RBAR = 0xE000ED9CU;
    static constexpr uintptr_t MPU_RASR = 0xE000EDA0U;

    inline void mpu_configure_region(uint8_t idx, const MpuRegion& r) noexcept {
        auto* rnr  = reinterpret_cast<volatile uint32_t*>(MPU_RNR);
        auto* rbar = reinterpret_cast<volatile uint32_t*>(MPU_RBAR);
        auto* rasr = reinterpret_cast<volatile uint32_t*>(MPU_RASR);

        *rnr = idx;
        *rbar = r.base & ~0x1Fu;     // 清除低5位配置位

        uint32_t rasr_val = (1u << 0);                             // ENABLE
        rasr_val |= ((static_cast<uint32_t>(r.size_pow2 - 1u) & 0x1Fu) << 1); // SIZE
        rasr_val |= (r.ap & 0x7u) << 24;                          // AP
        if (r.is_device) {
            rasr_val |= (1u << 16);                                // B=1, C=0 : Device
        } else {
            rasr_val |= (1u << 17) | (1u << 16);                  // B=1, C=1 : Normal WB
        }
        if (r.execute_never) {
            rasr_val |= (1u << 28);                                // XN
        }
        *rasr = rasr_val;
        __asm__ volatile ("dsb\n\t" "isb\n\t" : : : "memory");
    }

    inline void mpu_enable() noexcept {
        auto* ctrl  = reinterpret_cast<volatile uint32_t*>(MPU_CTRL);
        auto* shcsr = reinterpret_cast<volatile uint32_t*>(0xE000ED24U);
        *ctrl  = (1u << 2) | (1u << 0);  // PRIVDEFENA | ENABLE
        *shcsr |= (1u << 16);             // MemFaultEna
        __asm__ volatile ("dsb\n\t" "isb\n\t" : : : "memory");
    }

    inline void mpu_disable() noexcept {
        auto* ctrl = reinterpret_cast<volatile uint32_t*>(MPU_CTRL);
        __asm__ volatile ("dmb\n\t" : : : "memory");
        *ctrl = 0;
        __asm__ volatile ("dsb\n\t" "isb\n\t" : : : "memory");
    }

} // namespace Arch

#endif // AURORA_ARCH_CM4F_IMPL_HPP
