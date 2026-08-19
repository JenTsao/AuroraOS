#ifndef ARCH_AARCH64_MMU_PTE_HPP
#define ARCH_AARCH64_MMU_PTE_HPP

#include <stdint.h>
#include <stddef.h>

namespace auroraos {
namespace kernel {
namespace mmu {

// MAIR (Memory Attribute Indirection Register) Indices
constexpr uint64_t MAIR_IDX_DEVICE_nGnRnE = 0; // Device memory: non-Gathering, non-Reordering, no Early write acknowledgement
constexpr uint64_t MAIR_IDX_DEVICE_nGnRE  = 1; // Device memory: non-Gathering, non-Reordering, early write acknowledgement
constexpr uint64_t MAIR_IDX_NORMAL_NC     = 2; // Normal memory: Non-cacheable
constexpr uint64_t MAIR_IDX_NORMAL_WB     = 3; // Normal memory: Write-Back, Read/Write Allocate

// MAIR Attribute Encoded Values (ARMv8-A architecture)
constexpr uint64_t MAIR_ATTR_DEVICE_nGnRnE = 0x00;
constexpr uint64_t MAIR_ATTR_DEVICE_nGnRE  = 0x04;
constexpr uint64_t MAIR_ATTR_NORMAL_NC     = 0x44;
constexpr uint64_t MAIR_ATTR_NORMAL_WB     = 0xFF;

constexpr uint64_t MAIR_EL1_VALUE =
    (MAIR_ATTR_DEVICE_nGnRnE << (MAIR_IDX_DEVICE_nGnRnE * 8)) |
    (MAIR_ATTR_DEVICE_nGnRE  << (MAIR_IDX_DEVICE_nGnRE  * 8)) |
    (MAIR_ATTR_NORMAL_NC     << (MAIR_IDX_NORMAL_NC     * 8)) |
    (MAIR_ATTR_NORMAL_WB     << (MAIR_IDX_NORMAL_WB     * 8));

// Shareability attributes
constexpr uint64_t SH_NON_SHAREABLE   = 0;
constexpr uint64_t SH_OUTER_SHAREABLE = 2;
constexpr uint64_t SH_INNER_SHAREABLE = 3;

// Access Permissions (AP[2:1])
// AP[2] = Read-only (1) / Read-write (0)
// AP[1] = Unprivileged accessible (1) / Privileged only (0)
constexpr uint64_t AP_EL1_RW_EL0_NONE = 0; // 0b00: Privileged Read/Write, User No Access
constexpr uint64_t AP_ALL_RW          = 1; // 0b01: Privileged Read/Write, User Read/Write
constexpr uint64_t AP_EL1_RO_EL0_NONE = 2; // 0b10: Privileged Read-Only, User No Access
constexpr uint64_t AP_ALL_RO          = 3; // 0b11: Privileged Read-Only, User Read-Only

// C++ Core Guidelines: Type safety, encapsulate bit operations
struct PageTableEntry {
    uint64_t valid : 1;        // Bit 0: Valid
    uint64_t is_table : 1;     // Bit 1: 1=Table (L0-L2) or Page (L3), 0=Block (L1/L2)
    uint64_t attr_indx : 3;    // Bits 2-4: Memory attributes index (MAIR index)
    uint64_t ns : 1;           // Bit 5: Non-secure
    uint64_t ap : 2;           // Bits 6-7: Data access permissions (AP[2:1])
    uint64_t sh : 2;           // Bits 8-9: Shareability (00=none, 10=outer, 11=inner)
    uint64_t af : 1;           // Bit 10: Access flag (1=accessed)
    uint64_t ng : 1;           // Bit 11: Not global (0=global, 1=process-specific)
    uint64_t output_addr : 36; // Bits 12-47: Physical address / Next table address [47:12]
    uint64_t res0 : 4;         // Bits 48-51: Reserved, must be 0
    uint64_t contiguous : 1;   // Bit 52: Contiguous hint
    uint64_t pxn : 1;          // Bit 53: Privileged execute-never
    uint64_t uxn : 1;          // Bit 54: Unprivileged execute-never (XN)
    uint64_t software : 4;     // Bits 55-58: Software defined
    uint64_t pbha : 4;         // Bits 59-62: Page-based hardware attributes
    uint64_t ignored : 1;      // Bit 63: Ignored

    uintptr_t get_address() const {
        return static_cast<uintptr_t>(output_addr) << 12;
    }

    void set_address(uintptr_t addr) {
        output_addr = (addr >> 12) & 0xFFFFFFFFFULL;
    }

    void clear() {
        *reinterpret_cast<uint64_t*>(this) = 0;
    }

    uint64_t raw() const {
        return *reinterpret_cast<const uint64_t*>(this);
    }
};

// Ensure exactly 64-bit size
static_assert(sizeof(PageTableEntry) == sizeof(uint64_t), "PTE must be exactly 64 bits");

} // namespace mmu
} // namespace kernel
} // namespace auroraos

#endif // ARCH_AARCH64_MMU_PTE_HPP
