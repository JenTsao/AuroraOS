#include "syscall_validator.hpp"

// Flash region boundaries exposed by the linker script.
// Defined as weak to allow host test stubs to override them.
extern "C" __attribute__((weak)) uint32_t _flash_start;
extern "C" __attribute__((weak)) uint32_t _flash_end;

namespace auroraos {
namespace kernel {

bool SyscallValidator::validate_user_ptr(
    const void* ptr, size_t len,
    uintptr_t task_stack_base, size_t task_stack_size) noexcept
{
    if (!ptr) return false;

    const uintptr_t p = reinterpret_cast<uintptr_t>(ptr);
    const uintptr_t end = p + len;

    // Integer wrap-around check
    if (end < p) return false;

    // (a) Within the task's own stack
    if (task_stack_size > 0) {
        const bool in_stack = (p >= task_stack_base) && 
                              (end <= task_stack_base + task_stack_size);
        if (in_stack) return true;
    }

    // (b) Within read-only Flash (for string literals passed as SYS_PRINT arg)
    const uintptr_t flash_s = reinterpret_cast<uintptr_t>(&_flash_start);
    const uintptr_t flash_e = reinterpret_cast<uintptr_t>(&_flash_end);

    if (flash_s != flash_e) { // linker symbols valid
        const bool in_flash = (p >= flash_s) && (end <= flash_e);
        if (in_flash) return true;
    }

    return false;
}

} // namespace kernel
} // namespace auroraos
