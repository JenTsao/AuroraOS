/*
 * Cortex-M7 早期硬件初始化 — Reset_Handler 在 kernel_main 之前调用
 *
 * 职责:
 *   1. 使能 FPv5-SP FPU: CPACR CP10/CP11 全权访问 + FPCCR 自动保存/惰性压栈。
 *      硬浮点 ABI (-mfloat-abi=hard) 下任何浮点指令执行前必须完成。
 *   2. I-Cache / D-Cache 冷启动使能 (先全失效后开启)。
 *      Cortex-M7 cache line 固定 32 bytes (8 words)。
 *
 * 寄存器绝对地址依据 CMSIS core_cm7.h SCB_Type 布局 (SCB 基址 0xE000ED00):
 *   CPACR 0xE000ED88 | CSSELR 0xE000ED84 | CCSIDR 0xE000ED80
 *   FPCCR 0xE000EF34 | ICIALLU 0xE000EF50 | DCISW 0xE000EF60
 *
 * 【修正】原实现在此处读-改-写 0xE000EF9C 当作 "L1 Cache 控制寄存器"，
 * 但 Cortex-M7 的 System Control Space 里根本没有这个寄存器——CACR 是
 * Cortex-A 的东西。QEMU 会打印
 *     NVIC: Bad read offset 0xf9c / Bad write offset 0xf9c
 * 然后忽略该访问；真机上写保留地址属保留行为，架构未定义为忽略。
 * 更关键的是「cache 使能」这个概念在 M7 上不成立：CMSIS SCB_EnableICache()
 * / SCB_EnableDCache() 只做「全失效 + DSB/ISB」，不写任何使能位——M7 的
 * I/D cache 复位即开，只能通过维护寄存器与 CCR.INTC 控制。故该写操作已删除。
 *
 * 注: QEMU TCG 不建模 cache — ICIALLU/DCISW 写入被接受但无副作用,
 *     因此本序列在 QEMU 上无害; 真机上为 ARM 官方推荐使能流程 (参照 CMSIS
 *     cachel1_armv7.h)。热复位 (调试器附着的 warm reset) 场景如需绝对安全,
 *     应在进入本函数前确保 D-Cache 未持有脏数据。
 */
#include <stdint.h>

namespace {

inline volatile uint32_t* reg(uintptr_t addr) {
    return reinterpret_cast<volatile uint32_t*>(addr);
}

constexpr uintptr_t SCB_CPACR = 0xE000ED88U;   // 协处理器访问控制
constexpr uintptr_t SCB_CSSELR = 0xE000ED84U;  // cache 大小选择
constexpr uintptr_t SCB_CCSIDR = 0xE000ED80U;  // cache 大小标识 (随 CSSELR 变)
constexpr uintptr_t SCB_FPCCR = 0xE000EF34U;   // 浮点上下文控制
constexpr uintptr_t SCB_ICIALLU = 0xE000EF50U; // I-Cache 全失效 (写触发)
constexpr uintptr_t SCB_DCISW = 0xE000EF60U;   // D-Cache 按 set/way 失效 (写触发)

// CSSELR.Value: 0x1 = L1 data cache, 0x2 = L1 instruction cache。
// 0x0 是「未选择任何 cache」——写 0 会让 CCSIDR 读回全 0，于是下面的
// set/way 循环退化成只失效 1 组 1 路，等于没清干净。
constexpr uint32_t CSSELR_L1_DCACHE = 0x1;

constexpr uint32_t FPCCR_ASPEN = (1UL << 31); // 异常自动保留 FP 上下文
constexpr uint32_t FPCCR_LSPEN = (1UL << 30); // 惰性压栈

constexpr uint32_t CCSIDR_NUMSETS_POS = 13; // NUMSETS [27:13] (值 = 组数-1)
constexpr uint32_t CCSIDR_ASSOC_POS = 3;    // ASSOCIATIVITY [12:3] (值 = 路数-1)
constexpr uint32_t DCISW_SET_POS = 5;       // Set bits[7:5]
constexpr uint32_t DCISW_SET_MASK = (7UL << 5);
constexpr uint32_t DCISW_WAY_POS = 30; //             Way bits[31:30]
constexpr uint32_t DCISW_WAY_MASK = (3UL << 30);

} // namespace

extern "C" void arch_early_init(void) {
    // ── 1. FPU: CP10/CP11 全权访问 (CPACR bits [23:20] = 0b11 | 0b11)
    *reg(SCB_CPACR) |= (0xFUL << 20);
    *reg(SCB_FPCCR) |= FPCCR_ASPEN | FPCCR_LSPEN;
    __asm__ volatile("dsb\n\tisb" : : : "memory");

    // ── 2. I-Cache: 全失效
    *reg(SCB_ICIALLU) = 0;
    __asm__ volatile("dsb\n\tisb" : : : "memory");

    // ── 3. D-Cache: 选定 L1 data cache (CSSELR=0x1), 按 set/way 全失效
    *reg(SCB_CSSELR) = CSSELR_L1_DCACHE;
    __asm__ volatile("dsb" : : : "memory");
    const uint32_t ccsidr = *reg(SCB_CCSIDR);
    const uint32_t sets = ((ccsidr >> CCSIDR_NUMSETS_POS) & 0x7FFFUL) + 1;
    const uint32_t ways = ((ccsidr >> CCSIDR_ASSOC_POS) & 0x3FFUL) + 1;
    // SET/WAY 字段必须按位宽截断：DCISW 的 SET 只有 3 位 (bit7:5)、
    // WAY 只有 2 位 (bit31:30)，不截断时 set 索引会溢出进保留域，
    // 实际清掉的 set 远少于声明的数量。
    for (uint32_t s = sets; s > 0; --s) {
        for (uint32_t w = ways; w > 0; --w) {
            *reg(SCB_DCISW) =
                (((s - 1) << DCISW_SET_POS) & DCISW_SET_MASK) | (((w - 1) << DCISW_WAY_POS) & DCISW_WAY_MASK);
        }
    }
    __asm__ volatile("dsb\n\tisb" : : : "memory");

    // ── 4. 无「使能 cache」步骤
    // Cortex-M7 的 I/D cache 复位即已使能，架构上没有独立的 enable 寄存器：
    // CMSIS 的 SCB_EnableICache()/SCB_EnableDCache() 同样只做「全失效 + 屏障」。
    // 原实现在此处写 0xE000EF9C 并不存在（见文件头说明），已移除。
}
