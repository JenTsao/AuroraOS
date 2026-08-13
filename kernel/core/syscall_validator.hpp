#ifndef SYSCALL_VALIDATOR_HPP
#define SYSCALL_VALIDATOR_HPP

#include <stdint.h>
#include <stddef.h>

namespace auroraos {
namespace kernel {

class SyscallValidator {
public:
    // Validate that [ptr, ptr+len] lies entirely within either:
    // (a) the calling task's stack region, or
    // (b) the read-only Flash segment (for string literals).
    // Returns true if the range is safe to dereference in kernel context.
    [[nodiscard]] static bool validate_user_ptr(const void* ptr, size_t len, uintptr_t task_stack_base,
                                                size_t task_stack_size) noexcept;
};

} // namespace kernel
} // namespace auroraos

#endif // SYSCALL_VALIDATOR_HPP
