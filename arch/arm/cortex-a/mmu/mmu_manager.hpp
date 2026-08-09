#ifndef ARCH_AARCH64_MMU_MANAGER_HPP
#define ARCH_AARCH64_MMU_MANAGER_HPP

#include "../../../../kernel/vasp.hpp"
#include "mmu_pte.hpp"

namespace auroraos {
namespace kernel {
namespace mmu {

class AArch64MmuManager : public VirtualAddressSpace {
public:
    AArch64MmuManager();
    ~AArch64MmuManager() override;

    bool map(uintptr_t vaddr, uintptr_t paddr, MapFlags flags) override;
    bool unmap(uintptr_t vaddr) override;
    uintptr_t get_pgdir_base() const override;

private:
    PageTableEntry* l0_table_;
    
    PageTableEntry* get_or_allocate_next_level(PageTableEntry* current_entry);
};

} // namespace mmu
} // namespace kernel
} // namespace auroraos

#endif // ARCH_AARCH64_MMU_MANAGER_HPP
