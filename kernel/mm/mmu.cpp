// =============================================================================
// kernel/mm/mmu.cpp
//
// 统一虚拟内存与 MMU 多任务地址空间隔离子系统实现
// =============================================================================
#include "mmu.hpp"
#include "../task/task.hpp"
#include "../../arch/arm/cortex-a/mmu/mmu_manager.hpp"

namespace auroraos {
namespace kernel {

Mmu& Mmu::instance() {
    static Mmu mmu;
    return mmu;
}

Mmu::Mmu() : kernel_vas_(nullptr), mmu_enabled_(false) {}

bool Mmu::init(void* page_pool, size_t pool_size) {
    if (page_pool && pool_size > 0) {
        PageAllocator::instance().init(page_pool, pool_size);
    }

    // 创建内核常驻虚拟地址空间
    if (!kernel_vas_) {
        kernel_vas_ = create_vas();
    }

#if defined(ARCH_AARCH64)
    mmu::AArch64MmuManager::init_mmu_hardware();
    if (kernel_vas_) {
        mmu::AArch64MmuManager::set_ttbr0(kernel_vas_->get_pgdir_base());
    }
    enable_mmu();
#endif

    return kernel_vas_ != nullptr;
}

bool Mmu::is_mmu_supported() const {
#if defined(ARCH_AARCH64) || defined(AURORA_HOST_TEST)
    return true;
#else
    return false;
#endif
}

bool Mmu::is_mmu_enabled() const {
    return mmu_enabled_;
}

void Mmu::enable_mmu() {
#if defined(ARCH_AARCH64)
    mmu::AArch64MmuManager::enable_mmu();
#endif
    mmu_enabled_ = true;
}

void Mmu::disable_mmu() {
#if defined(ARCH_AARCH64)
    mmu::AArch64MmuManager::disable_mmu();
#endif
    mmu_enabled_ = false;
}

VirtualAddressSpace* Mmu::create_vas() {
    auto* vas = new mmu::AArch64MmuManager();
    if (vas) {
        vas->map_kernel_regions();
    }
    return vas;
}

void Mmu::destroy_vas(VirtualAddressSpace* vas) {
    if (vas) {
        delete vas;
    }
}

VirtualAddressSpace* Mmu::get_kernel_vas() {
    return kernel_vas_;
}

bool Mmu::create_task_vas(TaskControlBlock* task) {
    if (!task)
        return false;

    // 清理可能已存在的旧地址空间
    destroy_task_vas(task);

    VirtualAddressSpace* vas = create_vas();
    if (!vas)
        return false;

    task->memory.vasp = vas;
    task->memory.pgdir_base = vas->get_pgdir_base();
    return true;
}

void Mmu::destroy_task_vas(TaskControlBlock* task) {
    if (!task)
        return;

    if (task->memory.vasp) {
        destroy_vas(task->memory.vasp);
        task->memory.vasp = nullptr;
        task->memory.pgdir_base = 0;
    }
}

void Mmu::switch_vas(TaskControlBlock* task) {
    if (!task)
        return;

    uintptr_t pgdir = task->memory.pgdir_base;
    if (pgdir == 0 && kernel_vas_) {
        pgdir = kernel_vas_->get_pgdir_base();
    }

    if (pgdir != 0) {
        Arch::switch_address_space(pgdir);
    }
}

bool Mmu::map_user_memory(TaskControlBlock* task, uintptr_t vaddr, uintptr_t paddr, size_t size, MapFlags flags) {
    if (!task || !task->memory.vasp || size == 0)
        return false;

    // 强制确保 User 权限位
    flags |= MapFlags::User;

    return task->memory.vasp->map_range(vaddr, paddr, size, flags);
}

bool Mmu::unmap_user_memory(TaskControlBlock* task, uintptr_t vaddr, size_t size) {
    if (!task || !task->memory.vasp || size == 0)
        return false;

    return task->memory.vasp->unmap_range(vaddr, size);
}

bool Mmu::protect_user_memory(TaskControlBlock* task, uintptr_t vaddr, size_t size, MapFlags flags) {
    if (!task || !task->memory.vasp || size == 0)
        return false;

    flags |= MapFlags::User;
    return task->memory.vasp->protect_range(vaddr, size, flags);
}

bool Mmu::translate(TaskControlBlock* task, uintptr_t vaddr, uintptr_t* paddr_out, MapFlags* flags_out) const {
    if (!task || !task->memory.vasp)
        return false;

    return task->memory.vasp->translate(vaddr, paddr_out, flags_out);
}

} // namespace kernel
} // namespace auroraos
