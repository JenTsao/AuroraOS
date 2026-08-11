#ifndef KERNEL_OBJECT_HPP
#define KERNEL_OBJECT_HPP

#include <stdint.h>
#include "arch_api.hpp"

namespace auroraos {
namespace kernel {

enum class ObjectType : uint8_t {
    Null = 0,
    Task = 1,
    Endpoint = 2,
    MemoryRegion = 3
};

class KernelObject {
public:
    KernelObject(ObjectType type) : type_(type), ref_count_(1) {}
    virtual ~KernelObject() = default;

    void retain() {
        uint32_t primask = Arch::irq_save();
        ref_count_++;
        Arch::irq_restore(primask);
    }

    void release() {
        bool should_destroy = false;
        {
            uint32_t primask = Arch::irq_save();
            if (ref_count_ > 0) {
                ref_count_--;
                if (ref_count_ == 0) {
                    should_destroy = true;
                }
            }
            Arch::irq_restore(primask);
        }
        
        if (should_destroy) {
            destroy();
        }
    }

    uint32_t get_ref_count() const { 
        return ref_count_; 
    }
    
    ObjectType get_type() const { 
        return type_; 
    }

protected:
    virtual void destroy() {
        // Default destroy does nothing. Subclasses should override if they need
        // dynamic deallocation or cleanup.
    }

private:
    ObjectType type_;
    uint32_t ref_count_;
};

} // namespace kernel
} // namespace auroraos

#endif // KERNEL_OBJECT_HPP
