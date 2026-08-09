#include "gic.hpp"

namespace auroraos {
namespace kernel {
namespace gic {

uintptr_t GicV2::dist_base_ = 0;
uintptr_t GicV2::cpu_base_ = 0;

// Registers offset
constexpr uint32_t GICD_CTLR = 0x000;
constexpr uint32_t GICD_ISENABLER = 0x100;
constexpr uint32_t GICC_CTLR = 0x0000;
constexpr uint32_t GICC_PMR = 0x0004;
constexpr uint32_t GICC_IAR = 0x000C;
constexpr uint32_t GICC_EOIR = 0x0010;

void GicV2::init(uintptr_t dist_base, uintptr_t cpu_base) {
    dist_base_ = dist_base;
    cpu_base_ = cpu_base;

    // Disable distributor
    *reinterpret_cast<volatile uint32_t*>(dist_base_ + GICD_CTLR) = 0;
    
    // Enable distributor
    *reinterpret_cast<volatile uint32_t*>(dist_base_ + GICD_CTLR) = 1;

    // Enable CPU interface and set priority mask to lowest
    *reinterpret_cast<volatile uint32_t*>(cpu_base_ + GICC_PMR) = 0xFF;
    *reinterpret_cast<volatile uint32_t*>(cpu_base_ + GICC_CTLR) = 1;
}

void GicV2::enable_interrupt(uint32_t int_id) {
    uint32_t reg_offset = (int_id / 32) * 4;
    uint32_t bit = int_id % 32;
    *reinterpret_cast<volatile uint32_t*>(dist_base_ + GICD_ISENABLER + reg_offset) = (1 << bit);
}

uint32_t GicV2::acknowledge_interrupt() {
    return *reinterpret_cast<volatile uint32_t*>(cpu_base_ + GICC_IAR);
}

void GicV2::end_of_interrupt(uint32_t int_id) {
    *reinterpret_cast<volatile uint32_t*>(cpu_base_ + GICC_EOIR) = int_id;
}

} // namespace gic
} // namespace kernel
} // namespace auroraos
