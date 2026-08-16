#include "mmu_manager.hpp"
#include "../../../../kernel/mm/page_allocator.hpp"

namespace auroraos {
namespace kernel {
namespace mmu {

AArch64MmuManager::AArch64MmuManager() {
    l0_table_ = reinterpret_cast<PageTableEntry*>(PageAllocator::instance().alloc_page());
}

AArch64MmuManager::AArch64MmuManager(AArch64MmuManager&& other) noexcept
    : l0_table_(other.l0_table_) {
    other.l0_table_ = nullptr;
}

AArch64MmuManager& AArch64MmuManager::operator=(AArch64MmuManager&& other) noexcept {
    if (this != &other) {
        if (l0_table_) {
            free_table_recursive(l0_table_, 0);
        }
        l0_table_ = other.l0_table_;
        other.l0_table_ = nullptr;
    }
    return *this;
}

void AArch64MmuManager::free_table_recursive(PageTableEntry* table, int level) {
    if (!table)
        return;
    if (level < 3) {
        for (int i = 0; i < 512; ++i) {
            if (table[i].valid && table[i].is_table) {
                PageTableEntry* next_level =
                    reinterpret_cast<PageTableEntry*>(static_cast<uintptr_t>(table[i].output_addr) << 12);
                free_table_recursive(next_level, level + 1);
            }
        }
    }
    PageAllocator::instance().free_page(table);
}

AArch64MmuManager::~AArch64MmuManager() {
    if (l0_table_) {
        free_table_recursive(l0_table_, 0);
        l0_table_ = nullptr;
    }
}

uintptr_t AArch64MmuManager::get_pgdir_base() const {
    return reinterpret_cast<uintptr_t>(l0_table_);
}

PageTableEntry* AArch64MmuManager::get_or_allocate_next_level(PageTableEntry* current_entry) {
    if (current_entry->valid) {
        return reinterpret_cast<PageTableEntry*>(static_cast<uintptr_t>(current_entry->output_addr) << 12);
    }

    void* new_page = PageAllocator::instance().alloc_page();
    if (!new_page)
        return nullptr;

    current_entry->clear();
    current_entry->valid = 1;
    current_entry->is_table = 1;
    current_entry->output_addr = (reinterpret_cast<uintptr_t>(new_page) >> 12);

    return reinterpret_cast<PageTableEntry*>(new_page);
}

bool AArch64MmuManager::map(uintptr_t vaddr, uintptr_t paddr, MapFlags flags) {
    if ((vaddr & 0xFFF) != 0 || (paddr & 0xFFF) != 0) {
        return false; // Addresses must be 4KB page aligned
    }

    if (!l0_table_) {
        l0_table_ = reinterpret_cast<PageTableEntry*>(PageAllocator::instance().alloc_page());
        if (!l0_table_)
            return false;
    }

    uint32_t l0_idx = (vaddr >> 39) & 0x1FF;
    uint32_t l1_idx = (vaddr >> 30) & 0x1FF;
    uint32_t l2_idx = (vaddr >> 21) & 0x1FF;
    uint32_t l3_idx = (vaddr >> 12) & 0x1FF;

    PageTableEntry* l1_table = get_or_allocate_next_level(&l0_table_[l0_idx]);
    if (!l1_table)
        return false;

    PageTableEntry* l2_table = get_or_allocate_next_level(&l1_table[l1_idx]);
    if (!l2_table)
        return false;

    PageTableEntry* l3_table = get_or_allocate_next_level(&l2_table[l2_idx]);
    if (!l3_table)
        return false;

    PageTableEntry& pte = l3_table[l3_idx];
    pte.clear();
    pte.valid = 1;
    pte.is_table = 1; // 1 represents 4KB Page Descriptor at Level 3
    pte.output_addr = (paddr >> 12);
    pte.af = 1;       // Access flag

    // Memory attributes (MAIR index & shareability)
    if (flags & MapFlags::Device) {
        pte.attr_indx = MAIR_IDX_DEVICE_nGnRE;
        pte.sh = SH_OUTER_SHAREABLE;
    } else {
        pte.attr_indx = MAIR_IDX_NORMAL_WB;
        pte.sh = SH_INNER_SHAREABLE;
    }

    // Access permissions (AP[2:1])
    if (flags & MapFlags::User) {
        pte.ap = (flags & MapFlags::Write) ? AP_ALL_RW : AP_ALL_RO;
    } else {
        pte.ap = (flags & MapFlags::Write) ? AP_EL1_RW_EL0_NONE : AP_EL1_RO_EL0_NONE;
    }

    // Execution permissions (UXN / PXN)
    if (flags & MapFlags::Execute) {
        pte.uxn = (flags & MapFlags::User) ? 0 : 1;
        pte.pxn = (flags & MapFlags::User) ? 1 : 0;
    } else {
        pte.uxn = 1;
        pte.pxn = 1;
    }

    return true;
}

bool AArch64MmuManager::unmap(uintptr_t vaddr) {
    if ((vaddr & 0xFFF) != 0 || !l0_table_) {
        return false;
    }

    uint32_t l0_idx = (vaddr >> 39) & 0x1FF;
    uint32_t l1_idx = (vaddr >> 30) & 0x1FF;
    uint32_t l2_idx = (vaddr >> 21) & 0x1FF;
    uint32_t l3_idx = (vaddr >> 12) & 0x1FF;

    if (!l0_table_[l0_idx].valid)
        return false;
    PageTableEntry* l1_table =
        reinterpret_cast<PageTableEntry*>(static_cast<uintptr_t>(l0_table_[l0_idx].output_addr) << 12);

    if (!l1_table[l1_idx].valid)
        return false;
    PageTableEntry* l2_table =
        reinterpret_cast<PageTableEntry*>(static_cast<uintptr_t>(l1_table[l1_idx].output_addr) << 12);

    if (!l2_table[l2_idx].valid)
        return false;
    PageTableEntry* l3_table =
        reinterpret_cast<PageTableEntry*>(static_cast<uintptr_t>(l2_table[l2_idx].output_addr) << 12);

    if (!l3_table[l3_idx].valid)
        return false;

    // Clear the leaf PTE
    l3_table[l3_idx].clear();
    invalidate_tlb_va(vaddr);

    // If the L3 table is now empty, free it and clear the parent L2 entry.
    bool l3_empty = true;
    for (int i = 0; i < 512; ++i) {
        if (l3_table[i].valid) {
            l3_empty = false;
            break;
        }
    }
    if (l3_empty) {
        PageAllocator::instance().free_page(l3_table);
        l2_table[l2_idx].clear();

        // If the L2 table is now empty, free it and clear the parent L1 entry.
        bool l2_empty = true;
        for (int i = 0; i < 512; ++i) {
            if (l2_table[i].valid) {
                l2_empty = false;
                break;
            }
        }
        if (l2_empty) {
            PageAllocator::instance().free_page(l2_table);
            l1_table[l1_idx].clear();

            // If the L1 table is now empty, free it and clear the parent L0 entry.
            bool l1_empty = true;
            for (int i = 0; i < 512; ++i) {
                if (l1_table[i].valid) {
                    l1_empty = false;
                    break;
                }
            }
            if (l1_empty) {
                PageAllocator::instance().free_page(l1_table);
                l0_table_[l0_idx].clear();
            }
        }
    }

    return true;
}

bool AArch64MmuManager::translate(uintptr_t vaddr, uintptr_t* paddr_out, MapFlags* flags_out) const {
    if (!l0_table_)
        return false;

    uint32_t l0_idx = (vaddr >> 39) & 0x1FF;
    uint32_t l1_idx = (vaddr >> 30) & 0x1FF;
    uint32_t l2_idx = (vaddr >> 21) & 0x1FF;
    uint32_t l3_idx = (vaddr >> 12) & 0x1FF;

    if (!l0_table_[l0_idx].valid)
        return false;
    const PageTableEntry* l1_table =
        reinterpret_cast<const PageTableEntry*>(static_cast<uintptr_t>(l0_table_[l0_idx].output_addr) << 12);

    if (!l1_table[l1_idx].valid)
        return false;
    const PageTableEntry* l2_table =
        reinterpret_cast<const PageTableEntry*>(static_cast<uintptr_t>(l1_table[l1_idx].output_addr) << 12);

    if (!l2_table[l2_idx].valid)
        return false;
    const PageTableEntry* l3_table =
        reinterpret_cast<const PageTableEntry*>(static_cast<uintptr_t>(l2_table[l2_idx].output_addr) << 12);

    if (!l3_table[l3_idx].valid)
        return false;

    const PageTableEntry& pte = l3_table[l3_idx];
    if (paddr_out) {
        *paddr_out = (static_cast<uintptr_t>(pte.output_addr) << 12) | (vaddr & 0xFFF);
    }

    if (flags_out) {
        MapFlags f = MapFlags::Read;
        if (pte.ap == AP_ALL_RW || pte.ap == AP_EL1_RW_EL0_NONE) {
            f = f | MapFlags::Write;
        }
        if (pte.ap == AP_ALL_RW || pte.ap == AP_ALL_RO) {
            f = f | MapFlags::User;
        }
        if (pte.uxn == 0 || pte.pxn == 0) {
            f = f | MapFlags::Execute;
        }
        if (pte.attr_indx == MAIR_IDX_DEVICE_nGnRE || pte.attr_indx == MAIR_IDX_DEVICE_nGnRnE) {
            f = f | MapFlags::Device;
        }
        *flags_out = f;
    }

    return true;
}

bool AArch64MmuManager::protect(uintptr_t vaddr, MapFlags flags) {
    if ((vaddr & 0xFFF) != 0 || !l0_table_) {
        return false;
    }

    uint32_t l0_idx = (vaddr >> 39) & 0x1FF;
    uint32_t l1_idx = (vaddr >> 30) & 0x1FF;
    uint32_t l2_idx = (vaddr >> 21) & 0x1FF;
    uint32_t l3_idx = (vaddr >> 12) & 0x1FF;

    if (!l0_table_[l0_idx].valid)
        return false;
    PageTableEntry* l1_table =
        reinterpret_cast<PageTableEntry*>(static_cast<uintptr_t>(l0_table_[l0_idx].output_addr) << 12);

    if (!l1_table[l1_idx].valid)
        return false;
    PageTableEntry* l2_table =
        reinterpret_cast<PageTableEntry*>(static_cast<uintptr_t>(l1_table[l1_idx].output_addr) << 12);

    if (!l2_table[l2_idx].valid)
        return false;
    PageTableEntry* l3_table =
        reinterpret_cast<PageTableEntry*>(static_cast<uintptr_t>(l2_table[l2_idx].output_addr) << 12);

    if (!l3_table[l3_idx].valid)
        return false;

    PageTableEntry& pte = l3_table[l3_idx];

    // Update attributes
    if (flags & MapFlags::Device) {
        pte.attr_indx = MAIR_IDX_DEVICE_nGnRE;
        pte.sh = SH_OUTER_SHAREABLE;
    } else {
        pte.attr_indx = MAIR_IDX_NORMAL_WB;
        pte.sh = SH_INNER_SHAREABLE;
    }

    // Update access permissions
    if (flags & MapFlags::User) {
        pte.ap = (flags & MapFlags::Write) ? AP_ALL_RW : AP_ALL_RO;
    } else {
        pte.ap = (flags & MapFlags::Write) ? AP_EL1_RW_EL0_NONE : AP_EL1_RO_EL0_NONE;
    }

    // Update execution permissions
    if (flags & MapFlags::Execute) {
        pte.uxn = (flags & MapFlags::User) ? 0 : 1;
        pte.pxn = (flags & MapFlags::User) ? 1 : 0;
    } else {
        pte.uxn = 1;
        pte.pxn = 1;
    }

    invalidate_tlb_va(vaddr);
    return true;
}

// ── Hardware MMU Management (ARMv8-A EL1 Registers) ──────────────────────────

void AArch64MmuManager::init_mmu_hardware() {
#if defined(__aarch64__) || defined(ARCH_AARCH64)
    // 1. Program MAIR_EL1
    __asm__ volatile("msr mair_el1, %0" : : "r"(MAIR_EL1_VALUE) : "memory");

    // 2. Program TCR_EL1:
    // T0SZ = 16 (48-bit VA, 2^(64-16) = 256TB)
    // TG0 = 00 (4KB granule)
    // SH0 = 11 (Inner Shareable)
    // ORGN0 = 01 (Normal Outer WB-WA)
    // IRGN0 = 01 (Normal Inner WB-WA)
    // IPS = 010 (40-bit PA space, 1TB)
    uint64_t tcr = (16ULL << 0)   // T0SZ = 16 (48-bit VA)
                 | (0ULL << 14)   // TG0 = 4KB
                 | (3ULL << 12)   // SH0 = Inner Shareable
                 | (1ULL << 10)   // ORGN0 = Normal Outer WB-WA
                 | (1ULL << 8)    // IRGN0 = Normal Inner WB-WA
                 | (2ULL << 32);  // IPS = 40-bit Physical Address (1TB)
    __asm__ volatile("msr tcr_el1, %0\n\t"
                     "isb\n\t"
                     :
                     : "r"(tcr)
                     : "memory");
#endif
}

void AArch64MmuManager::set_ttbr0(uintptr_t pgdir_base) {
#if defined(__aarch64__) || defined(ARCH_AARCH64)
    __asm__ volatile("msr ttbr0_el1, %0\n\t"
                     "isb\n\t"
                     "tlbi vmalle1is\n\t"
                     "dsb sy\n\t"
                     "isb\n\t"
                     :
                     : "r"(pgdir_base)
                     : "memory");
#else
    (void)pgdir_base;
#endif
}

void AArch64MmuManager::enable_mmu() {
#if defined(__aarch64__) || defined(ARCH_AARCH64)
    uint64_t sctlr;
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    sctlr |= (1ULL << 0)   // M bit: MMU enable
           | (1ULL << 2)   // C bit: Data and unified cache enable
           | (1ULL << 12); // I bit: Instruction cache enable
    __asm__ volatile("msr sctlr_el1, %0\n\t"
                     "isb\n\t"
                     :
                     : "r"(sctlr)
                     : "memory");
#endif
}

void AArch64MmuManager::disable_mmu() {
#if defined(__aarch64__) || defined(ARCH_AARCH64)
    uint64_t sctlr;
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    sctlr &= ~(1ULL << 0); // M bit: MMU disable
    __asm__ volatile("msr sctlr_el1, %0\n\t"
                     "isb\n\t"
                     :
                     : "r"(sctlr)
                     : "memory");
#endif
}

void AArch64MmuManager::invalidate_tlb_all() {
#if defined(__aarch64__) || defined(ARCH_AARCH64)
    __asm__ volatile("tlbi vmalle1is\n\t"
                     "dsb sy\n\t"
                     "isb\n\t"
                     :
                     :
                     : "memory");
#endif
}

void AArch64MmuManager::invalidate_tlb_va(uintptr_t vaddr) {
#if defined(__aarch64__) || defined(ARCH_AARCH64)
    uint64_t page = vaddr >> 12;
    __asm__ volatile("tlbi vaae1is, %0\n\t"
                     "dsb sy\n\t"
                     "isb\n\t"
                     :
                     : "r"(page)
                     : "memory");
#else
    (void)vaddr;
#endif
}

} // namespace mmu
} // namespace kernel
} // namespace auroraos
