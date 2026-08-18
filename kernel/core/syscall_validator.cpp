// =============================================================================
// kernel/core/syscall_validator.cpp
//
// 用户态指针与内存区间合法性校验器实现
// =============================================================================
#include "syscall_validator.hpp"
#include "../task/task.hpp"
#include "../mm/memory.hpp"
#include "../mm/mmu.hpp"

#if defined(AURORA_HOST_TEST)
extern "C" {
    extern uintptr_t _flash_start;
    extern uintptr_t _flash_end;
    extern uintptr_t _sdata;
    extern uintptr_t _edata;
    extern uintptr_t _sbss;
    extern uintptr_t _ebss;
    extern uintptr_t _heap_start;
    extern uintptr_t _heap_end;
}
static inline uintptr_t sym_val(const uintptr_t& sym) {
    return sym;
}
#else
extern "C" {
    __attribute__((weak)) extern uint32_t _flash_start;
    __attribute__((weak)) extern uint32_t _flash_end;
    __attribute__((weak)) extern uint32_t _sdata;
    __attribute__((weak)) extern uint32_t _edata;
    __attribute__((weak)) extern uint32_t _sbss;
    __attribute__((weak)) extern uint32_t _ebss;
    __attribute__((weak)) extern uint32_t _heap_start;
    __attribute__((weak)) extern uint32_t _heap_end;
}
static inline uintptr_t sym_val(const uint32_t& sym) {
    return reinterpret_cast<uintptr_t>(&sym);
}
#endif

namespace auroraos {
namespace kernel {

bool SyscallValidator::validate_user_ptr(const void* ptr, size_t len,
                                         uintptr_t task_stack_base,
                                         size_t task_stack_size,
                                         bool need_write) noexcept {
    if (!ptr)
        return false;

    const uintptr_t p = reinterpret_cast<uintptr_t>(ptr);
    const uintptr_t end = p + len;

    // 整数溢出与回绕检查
    if (end < p)
        return false;

    // 0 长度指针非空即合法
    if (len == 0)
        return true;

    // (a) 检查是否在任务私有栈空间内 (栈空间为可读写内存)
    if (task_stack_size > 0) {
        if (p >= task_stack_base && end <= task_stack_base + task_stack_size) {
            return true;
        }
    }

    // (b) 检查是否在内核/用户通用堆空间内 (Heap, 可读写)
    if (KernelHeap::instance().contains(ptr, len)) {
        return true;
    }

    const uintptr_t heap_s = sym_val(_heap_start);
    const uintptr_t heap_e = sym_val(_heap_end);
    if (heap_s != heap_e && p >= heap_s && end <= heap_e) {
        return true;
    }

    // (c) 检查是否在全局数据段与 BSS 段内 (Data / BSS, 可读写)
    const uintptr_t data_s = sym_val(_sdata);
    const uintptr_t data_e = sym_val(_edata);
    if (data_s != data_e && p >= data_s && end <= data_e) {
        return true;
    }

    const uintptr_t bss_s = sym_val(_sbss);
    const uintptr_t bss_e = sym_val(_ebss);
    if (bss_s != bss_e && p >= bss_s && end <= bss_e) {
        return true;
    }

    // (d) 检查只读 Flash 区域 (仅限非写入操作, 如 SYS_PRINT 打印静态字符串常量)
    if (!need_write) {
        const uintptr_t flash_s = sym_val(_flash_start);
        const uintptr_t flash_e = sym_val(_flash_end);
        if (flash_s != flash_e && p >= flash_s && end <= flash_e) {
            return true;
        }

        // Cortex-M 平台 Flash 地址空间典型范围 (< 0x20000000)
#if !defined(ARCH_AARCH64) && !defined(AURORA_HOST_TEST)
        if (p < 0x20000000u && end <= 0x20000000u) {
            return true;
        }
#endif
    }

    return false;
}

bool SyscallValidator::validate_user_ptr(const void* ptr, size_t len,
                                         const TaskControlBlock* task,
                                         bool need_write) noexcept {
    if (!ptr)
        return false;

    const uintptr_t p = reinterpret_cast<uintptr_t>(ptr);
    const uintptr_t end = p + len;

    if (end < p)
        return false;

    if (len == 0)
        return true;

    if (task != nullptr) {
        // (1) 检查任务栈底水印与私有栈基址
        uintptr_t stack_base = task->memory.stack_base;
        size_t stack_size = (task->memory.size_pow2 > 0) ? (static_cast<size_t>(1) << task->memory.size_pow2) : 0;

        if (stack_size > 0 && p >= stack_base && end <= stack_base + stack_size) {
            return true;
        }

        if (task->task.stack_canary_ptr != nullptr) {
            uintptr_t canary_addr = reinterpret_cast<uintptr_t>(task->task.stack_canary_ptr);
            if (stack_size > 0 && p >= canary_addr && end <= canary_addr + stack_size) {
                return true;
            }
        }

        // (2) 检查任务 MPU / PMP 沙盒保护区域
        if (task->memory.mpu_sandbox.is_valid()) {
            uintptr_t sb_base = task->memory.mpu_sandbox.stack_base;
            size_t sb_size = (task->memory.mpu_sandbox.size_pow2 > 0) ? (static_cast<size_t>(1) << task->memory.mpu_sandbox.size_pow2) : 0;
            if (sb_size > 0 && p >= sb_base && end <= sb_base + sb_size) {
                return true;
            }
        }

        // (3) 检查 MMU 虚拟地址空间已映射的用户页 (MMU User Pages)
        if (task->memory.vasp != nullptr) {
            uintptr_t cur_va = p & ~(MMU_PAGE_SIZE - 1);
            bool all_mapped_valid = true;
            while (cur_va < end) {
                uintptr_t pa = 0;
                MapFlags flags = MapFlags::None;
                if (!task->memory.vasp->translate(cur_va, &pa, &flags)) {
                    all_mapped_valid = false;
                    break;
                }
                // 必须具有 User 访问权限
                if (!test_flags(flags, MapFlags::User)) {
                    all_mapped_valid = false;
                    break;
                }
                // 若需要写权限，必须具备 Write 标志
                if (need_write && !test_flags(flags, MapFlags::Write)) {
                    all_mapped_valid = false;
                    break;
                }
                cur_va += MMU_PAGE_SIZE;
            }
            if (all_mapped_valid) {
                return true;
            }
        }
    }

    // (4) 通用区域检查 (堆、数据段、BSS、只读 Flash)
    uintptr_t stack_base = task ? task->memory.stack_base : 0;
    size_t stack_size = (task && task->memory.size_pow2 > 0) ? (static_cast<size_t>(1) << task->memory.size_pow2) : 0;

    return validate_user_ptr(ptr, len, stack_base, stack_size, need_write);
}

} // namespace kernel
} // namespace auroraos
