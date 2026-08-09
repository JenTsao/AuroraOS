#ifndef ARCH_AARCH64_GIC_HPP
#define ARCH_AARCH64_GIC_HPP

#include <stdint.h>

namespace auroraos {
namespace kernel {
namespace gic {

class GicV2 {
public:
    static void init(uintptr_t dist_base, uintptr_t cpu_base);
    static void enable_interrupt(uint32_t int_id);
    static uint32_t acknowledge_interrupt();
    static void end_of_interrupt(uint32_t int_id);
    
private:
    static uintptr_t dist_base_;
    static uintptr_t cpu_base_;
};

} // namespace gic
} // namespace kernel
} // namespace auroraos

#endif // ARCH_AARCH64_GIC_HPP
