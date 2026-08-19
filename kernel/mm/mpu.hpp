#ifndef AURORA_MPU_HPP
#define AURORA_MPU_HPP

#include <stdint.h>
#include "../utils/hmac_sha256.hpp" // Crc32 namespace
#include "../core/arch_api.hpp"     // Arch::MpuRegion + Arch::mpu_*

extern "C" void uart_puts(const char* s); // TEMP DEBUG probe

// ─────────────────────────────────────────────────────────────────────────────
// KERNEL_ASSERT: Controlled halt on fatal invariant violation.
// In production this triggers IWDG starvation (watchdog-forced reset).
// In host tests the extern "C" weak stub just records the failure.
// ─────────────────────────────────────────────────────────────────────────────
#ifdef AURORA_HOST_TEST
#include <cstdio>
#define KERNEL_ASSERT(cond, msg)                                                                                       \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            ::fprintf(stderr, "[KERNEL_ASSERT] %s  (file %s line %d)\n", (msg), __FILE__, __LINE__);                   \
        }                                                                                                              \
    } while (0)
#else
extern "C" void uart_puts(const char* s);
#define KERNEL_ASSERT(cond, msg)                                                                                       \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            uart_puts("[KERNEL_ASSERT] " msg "\n");                                                                    \
            /* Stall — IWDG will force a reset */                                                                      \
            while (1) {}                                                                                               \
        }                                                                                                              \
    } while (0)
#endif

// ─────────────────────────────────────────────────────────────────────────────
// SandboxDescriptor — descriptor for a per-task MPU/PMP sandbox region.
// The crc32 field covers all other fields to detect in-memory tampering.
// Architecture-neutral: ARM MPU and RISC-V PMP both consume this descriptor
// through the Arch::mpu_configure_region() abstraction.
// ─────────────────────────────────────────────────────────────────────────────
struct SandboxDescriptor {
    uintptr_t stack_base;       // Must be aligned to (1 << size_pow2)
    uint8_t size_pow2;          // Region size = 2^size_pow2 bytes  [5..17]
    uint8_t subregion_disable;  // MPU Sub-Region Disable (SRD) bitmask (bit0..7, 1=disabled)
    uint16_t reserved{0};       // Alignment padding
    uint32_t version;           // Monotonically increasing per-task version
    uint32_t crc32;             // CRC32(stack_base | size_pow2 | srd | version)

    // Compute and store the CRC32 over the fields.
    // Call this whenever stack_base / size_pow2 / subregion_disable / version are updated.
    void seal() noexcept {
        const uint32_t words[4] = {
            static_cast<uint32_t>(stack_base & 0xFFFFFFFF),
            static_cast<uint32_t>((static_cast<uint64_t>(stack_base) >> 32) & 0xFFFFFFFF),
            static_cast<uint32_t>(size_pow2) | (static_cast<uint32_t>(subregion_disable) << 8),
            version
        };
        crc32 = Crc32::compute(reinterpret_cast<const uint8_t*>(words), sizeof(words));
    }

    [[nodiscard]] bool is_valid() const noexcept {
        const uint32_t words[4] = {
            static_cast<uint32_t>(stack_base & 0xFFFFFFFF),
            static_cast<uint32_t>((static_cast<uint64_t>(stack_base) >> 32) & 0xFFFFFFFF),
            static_cast<uint32_t>(size_pow2) | (static_cast<uint32_t>(subregion_disable) << 8),
            version
        };
        const uint32_t expected = Crc32::compute(reinterpret_cast<const uint8_t*>(words), sizeof(words));
        return crc32 == expected;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// MPU — 架构无关的内存保护管理器 Singleton
//
// 【设计决策】: 寄存器级别的操作已全部下沉到 Arch::mpu_* 接口
// (arch/arm/cortex-m/cm4/arch_impl.hpp 实现 PMSAv7 RBAR/RASR,
//  arch/riscv/rv32imac/arch_impl.hpp 实现 PMP pmpaddrN/pmpcfgN CSR)
//
// 这个类本身不含任何寄存器地址魔数，是完全可移植的。
// 缺失架构实现的 mpu_configure_region 会在链接期报未定义符号，
// 而非运行时悄悄写坏其他外设寄存器。
//
// 【PMSAv8 注意事项】:
// 当需要迁移到 M23/M33 时，只需新增 arch/arm/cortex-m/cm33/arch_impl.hpp
// 并实现 RBAR/RLAR 寄存器对，本文件不需要任何改动。
// ─────────────────────────────────────────────────────────────────────────────
class MPU {
public:
    // AP 常量 (与 arch_impl 解耦: 高层使用语义名称)
    // ARM PMSAv7: 直接作为 AP 字段. RISC-V PMP: bit2=X,bit1=W,bit0=R
    static constexpr uint32_t AP_NO_ACCESS = 0b000;
    static constexpr uint32_t AP_PRIV_RW = 0b001;
    static constexpr uint32_t AP_ALL_RW = 0b011;
    static constexpr uint32_t AP_PRIV_RO = 0b101;
    static constexpr uint32_t AP_ALL_RO = 0b110;

    static MPU& instance() {
        static MPU mpu;
        return mpu;
    }

    // 禁用 MPU/PMP 进行重新配置
    void disable() {
        Arch::mpu_disable();
    }

    // 启用 MPU/PMP（ARM: 开启 PRIVDEFENA 和 MemFault; RISC-V: 无操作,入口即生效）
    void enable() {
        Arch::mpu_enable();
    }

    // ─────────────────────────────────────────────────────────────────────
    // calculate_stack_srd_mask — 计算任务栈保护与子区域禁用掩码 (SRD)
    //
    // 对于 PMSAv7 / ARM MPU (size_pow2 >= 8, 如 4096 字节区域):
    //   一个 4096 区域切 8 个 512B 子区 (sub-region 0..7)。
    //   ARM Cortex-M 栈向下生长 (从 sub-region 7 向 sub-region 0 压栈)：
    //   - 默认将 sub-region 0 (最低地址 512B) 作为硬件栈哨兵 (Guard Sub-Region) 关掉 (bit0=1)；
    //   - 若任务栈实际需求小于 3584B，可禁用更多低地址子区，精准省 RAM 并捕获越界。
    // ─────────────────────────────────────────────────────────────────────
    static constexpr uint8_t calculate_stack_srd_mask(uint32_t requested_stack_bytes, uint8_t region_size_pow2,
                                                      bool enable_guard_subregion = true) noexcept {
        if (region_size_pow2 < 8u) {
            return 0; // 小于 256 字节不支持 SRD
        }
        const uint32_t region_size = 1u << region_size_pow2;
        const uint32_t subregion_size = region_size >> 3; // 1/8 区域大小

        uint32_t subregions_needed = (requested_stack_bytes + subregion_size - 1) / subregion_size;
        if (subregions_needed == 0) subregions_needed = 1;
        const uint32_t max_usable = enable_guard_subregion ? 7u : 8u;
        if (subregions_needed > max_usable) subregions_needed = max_usable;

        uint8_t mask = 0;
        const uint32_t disabled_count = 8u - subregions_needed;
        for (uint32_t i = 0; i < disabled_count; ++i) {
            mask |= static_cast<uint8_t>(1u << i);
        }
        return mask;
    }

    // 配置单个保护区域 (通用接口)
    // region_num: 0~7(ARM) / 0~15(RISC-V)
    // size_power_of_2 范围: [5, 31] (ARM) / [3, 31] (RISC-V NAPOT)
    void configure_region(uint8_t region_num, uintptr_t base_addr, uint8_t size_power_of_2, uint32_t ap,
                          bool execute_never = false, bool is_device = false, uint8_t subregion_disable = 0) {
        if (size_power_of_2 < 5u || size_power_of_2 > 31u) {
            KERNEL_ASSERT(false, "MPU: configure_region size_power_of_2 out of range");
            return;
        }

        const uintptr_t align_mask = (static_cast<uintptr_t>(1) << size_power_of_2) - 1u;
        if (base_addr & align_mask) {
            KERNEL_ASSERT(false, "MPU: configure_region base_addr misaligned");
            return;
        }

        Arch::mpu_configure_region(region_num,
                                   Arch::MpuRegion{base_addr, size_power_of_2, ap, execute_never, is_device, subregion_disable});
    }

    // ─────────────────────────────────────────────────────────────────────
    // update_user_sandbox_verified — Multi-layer validated sandbox update.
    //
    // Checks (in order):
    //   1. CRC32 integrity of the descriptor (catches in-memory tampering)
    //   2. size_pow2 range [5, 17]  (32 B … 128 KB)
    //   3. Base address alignment to region size
    //
    // On any failure the region is left unchanged and KERNEL_ASSERT fires.
    // Called from mpu_switch_sandbox() in interrupts.cpp (PendSV context).
    //
    // 【跨架构安全】: 全部寄存器写入操作委托给 Arch::mpu_configure_region()
    // 这保证了 RISC-V 目标不会触碰任何 0xE000ED9x ARM SCS 地址。
    // ─────────────────────────────────────────────────────────────────────
    void update_user_sandbox_verified(const SandboxDescriptor& desc) noexcept {
        uart_puts("[MPU-DBG] enter update_user_sandbox_verified\r\n");
        // 1. CRC32 integrity
        if (!desc.is_valid()) {
            uart_puts("[MPU-DBG] CRC check FAILED\r\n");
            KERNEL_ASSERT(false, "MPU: SandboxDescriptor CRC32 mismatch");
            return;
        }
        uart_puts("[MPU-DBG] CRC check OK\r\n");

        // 2. size_pow2 range (32 B = 2^5 … 128 KB = 2^17)
        if (!(desc.size_pow2 >= 5u && desc.size_pow2 <= 17u)) {
            uart_puts("[MPU-DBG] size_pow2 range check FAILED\r\n");
            KERNEL_ASSERT(false, "MPU: size_pow2 out of range [5..17]");
            return;
        }
        uart_puts("[MPU-DBG] size_pow2 range check OK\r\n");

        // 3. Alignment: base_addr must be a multiple of region size
        const uintptr_t align_mask = (static_cast<uintptr_t>(1) << desc.size_pow2) - 1u;
        if ((desc.stack_base & align_mask) != 0u) {
            uart_puts("[MPU-DBG] alignment check FAILED\r\n");
            KERNEL_ASSERT(false, "MPU: stack_base misaligned to region size");
            return;
        }
        uart_puts("[MPU-DBG] alignment check OK, calling mpu_configure_region\r\n");

        // All checks passed — program user sandbox region (7 on ARM/PMP) with Sub-Region Disable
        Arch::mpu_configure_region(7, Arch::MpuRegion{desc.stack_base, desc.size_pow2, AP_ALL_RW,
                                                      /*execute_never=*/true, /*is_device=*/false, desc.subregion_disable});
        uart_puts("[MPU-DBG] mpu_configure_region returned OK\r\n");
    }

    // Compatibility wrapper for callers that have already validated parameters.
    void update_user_sandbox(uintptr_t stack_base, uint8_t size_power_of_2, uint8_t subregion_disable = 0) noexcept {
        Arch::mpu_configure_region(
            7, Arch::MpuRegion{stack_base, size_power_of_2, AP_ALL_RW, /*execute_never=*/true, /*is_device=*/false, subregion_disable});
    }
};

#endif // AURORA_MPU_HPP
