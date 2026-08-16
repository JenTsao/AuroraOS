#ifndef ARCH_AARCH64_MMU_MANAGER_HPP
#define ARCH_AARCH64_MMU_MANAGER_HPP

#include "../../../../kernel/mm/vasp.hpp"
#include "mmu_pte.hpp"

namespace auroraos {
namespace kernel {
namespace mmu {

class AArch64MmuManager : public VirtualAddressSpace {
public:
    AArch64MmuManager();
    ~AArch64MmuManager() override;

    // Non-copyable to prevent double-free of page tables
    AArch64MmuManager(const AArch64MmuManager&) = delete;
    AArch64MmuManager& operator=(const AArch64MmuManager&) = delete;

    // Move operations
    AArch64MmuManager(AArch64MmuManager&& other) noexcept;
    AArch64MmuManager& operator=(AArch64MmuManager&& other) noexcept;

    // VirtualAddressSpace Interface Implementation
    bool map(uintptr_t vaddr, uintptr_t paddr, MapFlags flags) override;
    bool unmap(uintptr_t vaddr) override;
    bool translate(uintptr_t vaddr, uintptr_t* paddr_out, MapFlags* flags_out = nullptr) const override;
    bool protect(uintptr_t vaddr, MapFlags flags) override;
    uintptr_t get_pgdir_base() const override;

    // Check if the root translation table is allocated and valid
    bool is_valid() const { return l0_table_ != nullptr; }

    // Hardware MMU Management (ARMv8-A system registers)
    static void init_mmu_hardware();
    static void enable_mmu();
    static void disable_mmu();
    static void set_ttbr0(uintptr_t pgdir_base);
    static void invalidate_tlb_all();
    static void invalidate_tlb_va(uintptr_t vaddr);

private:
    PageTableEntry* l0_table_{nullptr};

    PageTableEntry* get_or_allocate_next_level(PageTableEntry* current_entry);
    static void free_table_recursive(PageTableEntry* table, int level);
};

} // namespace mmu
} // namespace kernel
} // namespace auroraos

#endif // ARCH_AARCH64_MMU_MANAGER_HPP
