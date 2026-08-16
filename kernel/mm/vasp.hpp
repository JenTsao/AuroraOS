#ifndef VASP_HPP
#define VASP_HPP

#include <stdint.h>
#include <stddef.h>

namespace auroraos {
namespace kernel {

enum class MapFlags : uint32_t {
    Read = 1 << 0,
    Write = 1 << 1,
    Execute = 1 << 2,
    User = 1 << 3,
    Device = 1 << 4
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
    static constexpr size_t PAGE_SIZE = 4096;

    virtual ~VirtualAddressSpace() = default;

    // Map a single page (vaddr to paddr) with flags
    virtual bool map(uintptr_t vaddr, uintptr_t paddr, MapFlags flags) = 0;

    // Map a continuous range of pages
    virtual bool map_range(uintptr_t vaddr, uintptr_t paddr, size_t size, MapFlags flags) {
        if ((vaddr % PAGE_SIZE) != 0 || (paddr % PAGE_SIZE) != 0 || size == 0) {
            return false;
        }
        for (size_t offset = 0; offset < size; offset += PAGE_SIZE) {
            if (!map(vaddr + offset, paddr + offset, flags)) {
                return false;
            }
        }
        return true;
    }

    // Unmap a single page
    virtual bool unmap(uintptr_t vaddr) = 0;

    // Unmap a continuous range of pages
    virtual bool unmap_range(uintptr_t vaddr, size_t size) {
        if ((vaddr % PAGE_SIZE) != 0 || size == 0) {
            return false;
        }
        bool all_ok = true;
        for (size_t offset = 0; offset < size; offset += PAGE_SIZE) {
            if (!unmap(vaddr + offset)) {
                all_ok = false;
            }
        }
        return all_ok;
    }

    // Translate virtual address to physical address and query flags
    virtual bool translate(uintptr_t vaddr, uintptr_t* paddr_out, MapFlags* flags_out = nullptr) const = 0;

    // Change memory protection flags for an existing page mapping
    virtual bool protect(uintptr_t vaddr, MapFlags flags) = 0;

    // Change memory protection flags for a range of pages
    virtual bool protect_range(uintptr_t vaddr, size_t size, MapFlags flags) {
        if ((vaddr % PAGE_SIZE) != 0 || size == 0) {
            return false;
        }
        for (size_t offset = 0; offset < size; offset += PAGE_SIZE) {
            if (!protect(vaddr + offset, flags)) {
                return false;
            }
        }
        return true;
    }

    // Check whether a virtual address is currently mapped
    virtual bool is_mapped(uintptr_t vaddr) const {
        uintptr_t paddr = 0;
        return translate(vaddr, &paddr, nullptr);
    }

    // Get the physical base address of the top-level page table (e.g. for TTBR0_EL1)
    virtual uintptr_t get_pgdir_base() const = 0;
};

} // namespace kernel
} // namespace auroraos

#endif // VASP_HPP
