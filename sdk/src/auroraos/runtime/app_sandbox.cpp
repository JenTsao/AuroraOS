#include "app_sandbox.hpp"

namespace auroraos {
namespace runtime {

AppSandbox::AppSandbox(const AppManifest& manifest) 
    : manifest_(manifest), current_memory_usage_(0) {}

bool AppSandbox::has_capability(AppCapability cap) const {
    return (manifest_.required_caps & static_cast<uint32_t>(cap)) != 0;
}

bool AppSandbox::check_memory_quota(uint32_t allocation_size) const {
    if (manifest_.max_memory_bytes == 0) return true; // 0 means no limit (or system task)
    return (current_memory_usage_ + allocation_size) <= manifest_.max_memory_bytes;
}

void AppSandbox::allocate_memory(uint32_t size) {
    current_memory_usage_ += size;
}

void AppSandbox::free_memory(uint32_t size) {
    if (current_memory_usage_ >= size) {
        current_memory_usage_ -= size;
    } else {
        current_memory_usage_ = 0; // Prevent underflow
    }
}

} // namespace runtime
} // namespace auroraos
