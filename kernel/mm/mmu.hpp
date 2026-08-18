// =============================================================================
// kernel/mm/mmu.hpp
//
// 统一虚拟内存与 MMU 多任务地址空间隔离子系统
// 核心职责：
//   1. 统管物理页分配 (PageAllocator) 与多级页表虚拟地址空间 (VirtualAddressSpace)
//   2. 进程 / 任务独立地址空间生命周期管理 (Task Address Space Isolation)
//   3. 提供架构无关的标准内存布局、页表切换、权限控制与虚实映射接口
// =============================================================================
#ifndef AURORA_MMU_HPP
#define AURORA_MMU_HPP

#include <stdint.h>
#include <stddef.h>
#include "vasp.hpp"
#include "page_allocator.hpp"
#include "../core/arch_api.hpp"

struct TaskControlBlock;

namespace auroraos {
namespace kernel {

// =============================================================================
// 标准虚拟内存布局常量 (4KB 粒度分页)
// =============================================================================
constexpr size_t MMU_PAGE_SIZE     = 4096;
constexpr size_t MMU_PAGE_SHIFT    = 12;
constexpr uintptr_t MMU_PAGE_MASK  = 0xFFFULL;

constexpr uintptr_t USER_SPACE_BASE   = 0x00400000ULL; // 用户程序代码段默认基址 (4MB)
constexpr uintptr_t USER_STACK_TOP    = 0x7FFFF000ULL; // 用户栈顶地址 (2GB-4KB)
constexpr uintptr_t USER_SPACE_TOP    = 0x80000000ULL; // 用户空间上限 (2GB)
constexpr uintptr_t KERNEL_SPACE_BASE = 0x80000000ULL; // 内核虚拟空间基址
constexpr uintptr_t KERNEL_RAM_BASE   = 0x40000000ULL; // 内核 RAM 物理基址

// =============================================================================
// Mmu: 架构无关的统一虚拟内存与地址空间隔离管理器
// =============================================================================
class Mmu {
public:
    static Mmu& instance();

    // 初始化 MMU 子系统（可指定物理页内存池基址和大小）
    bool init(void* page_pool = nullptr, size_t pool_size = 0);

    // 查询当前硬件平台是否支持 MMU 以及是否处于激活状态
    bool is_mmu_supported() const;
    bool is_mmu_enabled() const;

    // 激活 / 关闭硬件 MMU
    void enable_mmu();
    void disable_mmu();

    // 创建全新的虚拟地址空间（自动建立内核常驻段映射，保证中断/异常不发生缺页）
    VirtualAddressSpace* create_vas();

    // 释放指定的虚拟地址空间及其全部多级页表
    void destroy_vas(VirtualAddressSpace* vas);

    // 获取内核全局地址空间
    VirtualAddressSpace* get_kernel_vas();

    // =========================================================================
    // 多任务地址空间隔离生命周期接口
    // =========================================================================

    // 为任务分配独立的虚拟地址空间，并绑定到 task->memory.vasp / pgdir_base
    bool create_task_vas(TaskControlBlock* task);

    // 销毁任务绑定的虚拟地址空间并安全复位
    void destroy_task_vas(TaskControlBlock* task);

    // 切换当前 CPU 正在使用的虚拟地址空间 (TTBR0 / satp)
    void switch_vas(TaskControlBlock* task);

    // 为任务映射用户空间内存段 (自动按 4KB 对齐)
    bool map_user_memory(TaskControlBlock* task, uintptr_t vaddr, uintptr_t paddr, size_t size, MapFlags flags);

    // 撤销任务指定范围的用户内存映射
    bool unmap_user_memory(TaskControlBlock* task, uintptr_t vaddr, size_t size);

    // 动态调整任务内存页访问权限
    bool protect_user_memory(TaskControlBlock* task, uintptr_t vaddr, size_t size, MapFlags flags);

    // 虚实地址转换与属性查询
    bool translate(TaskControlBlock* task, uintptr_t vaddr, uintptr_t* paddr_out, MapFlags* flags_out = nullptr) const;

    // =========================================================================
    // 物理页帧分配器代理接口
    // =========================================================================
    void* alloc_page() {
        return PageAllocator::instance().alloc_page();
    }

    void free_page(void* page) {
        PageAllocator::instance().free_page(page);
    }

    size_t get_free_pages() const {
        return PageAllocator::instance().get_free_pages();
    }

    size_t get_total_pages() const {
        return PageAllocator::instance().get_total_pages();
    }

private:
    Mmu();
    ~Mmu() = default;

    Mmu(const Mmu&) = delete;
    Mmu& operator=(const Mmu&) = delete;

    VirtualAddressSpace* kernel_vas_{nullptr};
    bool mmu_enabled_{false};
};

} // namespace kernel
} // namespace auroraos

#endif // AURORA_MMU_HPP
