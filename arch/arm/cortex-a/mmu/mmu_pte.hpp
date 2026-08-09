#ifndef ARCH_AARCH64_MMU_PTE_HPP
#define ARCH_AARCH64_MMU_PTE_HPP

#include <stdint.h>

namespace auroraos {
namespace kernel {
namespace mmu {

// C++ Core Guidelines: Type safety, encapsulate bit operations
struct PageTableEntry {
    uint64_t valid       : 1;  // Bit 0: Valid
    uint64_t is_table    : 1;  // Bit 1: 1=Table, 0=Block/Page
    uint64_t attr_indx   : 3;  // Bits 2-4: Memory attributes index
    uint64_t ns          : 1;  // Bit 5: Non-secure
    uint64_t ap          : 2;  // Bits 6-7: Data access permissions
    uint64_t sh          : 2;  // Bits 8-9: Shareability
    uint64_t af          : 1;  // Bit 10: Access flag
    uint64_t ng          : 1;  // Bit 11: Not global
    uint64_t output_addr : 36; // Bits 12-47: Physical address (4KB aligned)
    uint64_t res0        : 4;  // Bits 48-51: Reserved, must be 0
    uint64_t contiguous  : 1;  // Bit 52: Contiguous hint
    uint64_t pxn         : 1;  // Bit 53: Privileged execute-never
    uint64_t uxn         : 1;  // Bit 54: Execute-never
    uint64_t software    : 4;  // Bits 55-58: Software defined
    uint64_t pbha        : 4;  // Bits 59-62: Page-based hardware attributes
    uint64_t ignored     : 1;  // Bit 63: Ignored
};

// Ensure exactly 64-bit size
static_assert(sizeof(PageTableEntry) == sizeof(uint64_t), "PTE must be exactly 64 bits");

} // namespace mmu
} // namespace kernel
} // namespace auroraos

#endif // ARCH_AARCH64_MMU_PTE_HPP
