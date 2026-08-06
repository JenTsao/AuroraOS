#include "mmu_manager.hpp"
#include "../../../../kernel/page_allocator.hpp"

namespace auroraos {
namespace kernel {
namespace mmu {

AArch64MmuManager::AArch64MmuManager() {
    l0_table_ = reinterpret_cast<PageTableEntry*>(PageAllocator::instance().alloc_page());
}

AArch64MmuManager::~AArch64MmuManager() {
    // A complete implementation would recursively free all allocated page tables.
    // For simplicity in Phase 4 Stage 2, we omit the deep recursive free.
    PageAllocator::instance().free_page(l0_table_);
}

uintptr_t AArch64MmuManager::get_pgdir_base() const {
    return reinterpret_cast<uintptr_t>(l0_table_);
}

PageTableEntry* AArch64MmuManager::get_or_allocate_next_level(PageTableEntry* current_entry) {
    if (current_entry->valid) {
        return reinterpret_cast<PageTableEntry*>(static_cast<uintptr_t>(current_entry->output_addr) << 12);
    }
    
    void* new_page = PageAllocator::instance().alloc_page();
    if (!new_page) return nullptr;
    
    current_entry->valid = 1;
    current_entry->is_table = 1;
    current_entry->output_addr = (reinterpret_cast<uintptr_t>(new_page) >> 12);
    
    return reinterpret_cast<PageTableEntry*>(new_page);
}

bool AArch64MmuManager::map(uintptr_t vaddr, uintptr_t paddr, MapFlags flags) {
    if (!l0_table_) return false;

    uint32_t l0_idx = (vaddr >> 39) & 0x1FF;
    uint32_t l1_idx = (vaddr >> 30) & 0x1FF;
    uint32_t l2_idx = (vaddr >> 21) & 0x1FF;
    uint32_t l3_idx = (vaddr >> 12) & 0x1FF;

    PageTableEntry* l1_table = get_or_allocate_next_level(&l0_table_[l0_idx]);
    if (!l1_table) return false;

    PageTableEntry* l2_table = get_or_allocate_next_level(&l1_table[l1_idx]);
    if (!l2_table) return false;

    PageTableEntry* l3_table = get_or_allocate_next_level(&l2_table[l2_idx]);
    if (!l3_table) return false;

    PageTableEntry& pte = l3_table[l3_idx];
    pte.valid = 1;
    pte.is_table = 1; // 1 means Page Descriptor at L3
    pte.output_addr = (paddr >> 12);
    
    // Set permissions
    pte.ap = (flags & MapFlags::Write) ? 0 : 2; // Basic AP config (0=RW, 2=RO for privileged)
    if (flags & MapFlags::User) {
        pte.ap |= 1; // Unprivileged access
    }
    
    pte.uxn = (flags & MapFlags::Execute) ? 0 : 1;
    pte.pxn = (flags & MapFlags::Execute) ? 0 : 1;
    pte.af = 1; // Access flag

    return true;
}

bool AArch64MmuManager::unmap(uintptr_t vaddr) {
    if (!l0_table_) return false;

    uint32_t l0_idx = (vaddr >> 39) & 0x1FF;
    uint32_t l1_idx = (vaddr >> 30) & 0x1FF;
    uint32_t l2_idx = (vaddr >> 21) & 0x1FF;
    uint32_t l3_idx = (vaddr >> 12) & 0x1FF;

    if (!l0_table_[l0_idx].valid) return false;
    PageTableEntry* l1_table = reinterpret_cast<PageTableEntry*>(static_cast<uintptr_t>(l0_table_[l0_idx].output_addr) << 12);

    if (!l1_table[l1_idx].valid) return false;
    PageTableEntry* l2_table = reinterpret_cast<PageTableEntry*>(static_cast<uintptr_t>(l1_table[l1_idx].output_addr) << 12);

    if (!l2_table[l2_idx].valid) return false;
    PageTableEntry* l3_table = reinterpret_cast<PageTableEntry*>(static_cast<uintptr_t>(l2_table[l2_idx].output_addr) << 12);

    // Clear the leaf PTE.
    l3_table[l3_idx].valid = 0;

    // If the L3 table is now empty, free it and clear the parent L2 entry.
    bool l3_empty = true;
    for (int i = 0; i < 512; ++i) {
        if (l3_table[i].valid) { l3_empty = false; break; }
    }
    if (l3_empty) {
        PageAllocator::instance().free_page(l3_table);
        l2_table[l2_idx].valid = 0;

        // If the L2 table is now empty, free it and clear the parent L1 entry.
        bool l2_empty = true;
        for (int i = 0; i < 512; ++i) {
            if (l2_table[i].valid) { l2_empty = false; break; }
        }
        if (l2_empty) {
            PageAllocator::instance().free_page(l2_table);
            l1_table[l1_idx].valid = 0;

            // If the L1 table is now empty, free it and clear the parent L0 entry.
            bool l1_empty = true;
            for (int i = 0; i < 512; ++i) {
                if (l1_table[i].valid) { l1_empty = false; break; }
            }
            if (l1_empty) {
                PageAllocator::instance().free_page(l1_table);
                l0_table_[l0_idx].valid = 0;
            }
        }
    }

    return true;
}

} // namespace mmu
} // namespace kernel
} // namespace auroraos
