#ifndef AURORA_MEMORY_ATTRIBUTES_HPP
#define AURORA_MEMORY_ATTRIBUTES_HPP

#include <stddef.h>
#include <stdint.h>

// =============================================================================
// 高速 RAM 与紧耦合内存 (DTCM / CCMRAM / FastRAM) 属性定义
//
// 针对 ARM Cortex-M4F (如 Ambiq Apollo3 384KB TCM, STM32 CCMRAM) 与 Cortex-M7 DTCM：
//   - 将 TCB 任务控制块、调度器就绪队列、VNode / 文件描述符等最高频热点对象映射至
//     物理零等待周期的 .fastram / .dtcm 段；
//   - 保证 8 字节严格硬件对齐（支持 LDRD/STRD 双字原子传输与 ARMv7E-M 栈对齐要求）；
//   - 在不支持多 RAM 银行的平台 (QEMU/Host 测试) 自动退化为标准 8 字节对齐，无缝向下兼容。
// =============================================================================

#if (defined(__GNUC__) || defined(__clang__)) && !defined(AURORA_HOST_TEST)
#define AURORA_FASTRAM __attribute__((section(".fastram"), aligned(8)))
#define AURORA_DTCM    __attribute__((section(".dtcm"), aligned(8)))
#define AURORA_CCMRAM  __attribute__((section(".ccmram"), aligned(8)))
#else
#define AURORA_FASTRAM alignas(8)
#define AURORA_DTCM    alignas(8)
#define AURORA_CCMRAM  alignas(8)
#endif

// L1 Cache Line 对齐 (32 字节)，用于防止多核/DMA 伪共享 (False Sharing)
#define AURORA_CACHE_ALIGNED alignas(32)

#endif // AURORA_MEMORY_ATTRIBUTES_HPP
