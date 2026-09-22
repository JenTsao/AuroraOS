#ifndef AURORA_MEMORY_PROTECTION_HPP
#define AURORA_MEMORY_PROTECTION_HPP

#include <stdint.h>
#include <stddef.h>
#include "mpu.hpp"
#include "mmu.hpp"
#include "../core/syscall_validator.hpp"

namespace auroraos::kernel {

enum class ProtectionModel : uint8_t {
    None = 0,
    Mpu  = 1,   // Region-based protection (ARM PMSAv7/PMSAv8, RISC-V PMP)
    Mmu  = 2    // Page-table based virtual memory (AArch64, RV32/64 SV39)
};

enum class MemoryPermission : uint8_t {
    None        = 0,
    Read        = 1,
    Write       = 2,
    ReadWrite   = 3,
    Execute     = 4,
    ReadExecute = 5,
    All         = 7
};

class IMemoryProtection {
public:
    virtual ~IMemoryProtection() = default;

    virtual ProtectionModel get_model() const noexcept = 0;
    virtual bool is_active() const noexcept = 0;
    virtual bool protect_task_stack(TaskControlBlock* task) noexcept = 0;
    virtual bool configure_sandbox(TaskControlBlock* task, uintptr_t base, size_t size, MemoryPermission perm) noexcept = 0;
    virtual bool validate_user_buffer(const TaskControlBlock* task, const void* ptr, size_t size, bool write_access) const noexcept = 0;
};

class MemoryProtectionManager : public IMemoryProtection {
public:
    static MemoryProtectionManager& instance() {
        static MemoryProtectionManager mgr;
        return mgr;
    }

    ProtectionModel get_model() const noexcept override {
#if defined(ARCH_AARCH64)
        return ProtectionModel::Mmu;
#elif defined(ARCH_ARM) || defined(ARCH_RISCV32)
        return ProtectionModel::Mpu;
#else
        return ProtectionModel::Mpu;
#endif
    }

    bool is_active() const noexcept override {
        if (get_model() == ProtectionModel::Mmu) {
            return Mmu::instance().is_mmu_enabled();
        } else {
            return true;
        }
    }

    bool protect_task_stack(TaskControlBlock* task) noexcept override {
        if (!task) return false;
        if (task->memory.size_pow2 >= 8) {
            uint8_t srd = MPU::calculate_stack_srd_mask(
                1u << task->memory.size_pow2,
                task->memory.size_pow2,
                /*enable_guard_subregion=*/true
            );
            task->memory.subregion_disable = srd;
            task->memory.mpu_sandbox.subregion_disable = srd;
            task->memory.mpu_sandbox.seal();
        }
        return true;
    }

    bool configure_sandbox(TaskControlBlock* task, uintptr_t base, size_t size, MemoryPermission perm) noexcept override {
        (void)perm;
        if (!task || size == 0) return false;
        task->memory.stack_base = base;
        task->memory.mpu_sandbox.stack_base = base;
        task->memory.mpu_sandbox.version++;
        task->memory.mpu_sandbox.seal();
        return true;
    }

    bool validate_user_buffer(const TaskControlBlock* task, const void* ptr, size_t size, bool write_access) const noexcept override {
        if (!task || !ptr || size == 0) return false;
        return SyscallValidator::validate_user_ptr(ptr, size, task, write_access);
    }

private:
    constexpr MemoryProtectionManager() = default;
};

} // namespace auroraos::kernel

#endif // AURORA_MEMORY_PROTECTION_HPP
