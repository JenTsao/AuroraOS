#ifndef VASP_HPP
#define VASP_HPP

#include <stdint.h>
#include <stddef.h>

namespace auroraos {
namespace kernel {

enum class MapFlags : uint32_t {
    Read     = 1 << 0,
    Write    = 1 << 1,
    Execute  = 1 << 2,
    User     = 1 << 3,
    Device   = 1 << 4
};

inline MapFlags operator|(MapFlags a, MapFlags b) {
    return static_cast<MapFlags>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}
inline bool operator&(MapFlags a, MapFlags b) {
    return (static_cast<uint32_t>(a) & static_cast<uint32_t>(b)) != 0;
}

// Virtual Address Space Abstract Interface
class VirtualAddressSpace {
public:
    virtual ~VirtualAddressSpace() = default;
    
    // Map virtual address to physical address with flags
    virtual bool map(uintptr_t vaddr, uintptr_t paddr, MapFlags flags) = 0;
    
    // Unmap virtual address
    virtual bool unmap(uintptr_t vaddr) = 0;
    
    // Get the physical base address of the top-level page table (e.g. for TTBR0_EL1)
    virtual uintptr_t get_pgdir_base() const = 0;
};

} // namespace kernel
} // namespace auroraos

#endif // VASP_HPP
