#include "app_sandbox.hpp"
#include <string.h>

namespace auroraos {
namespace runtime {

AppSandbox::AppSandbox(const AppManifest& manifest)
    : manifest_(manifest), current_memory_usage_(0), task_id_(0), status_(SandboxStatus::Active), violation_count_(0) {
    violation_reason_[0] = '\0';
}

bool AppSandbox::has_capability(AppCapability cap) const {
    return (manifest_.required_caps & static_cast<uint32_t>(cap)) != 0;
}

bool AppSandbox::grant_capability(AppCapability cap) {
    manifest_.required_caps |= static_cast<uint32_t>(cap);
    return true;
}

bool AppSandbox::revoke_capability(AppCapability cap) {
    manifest_.required_caps &= ~static_cast<uint32_t>(cap);
    return true;
}

bool AppSandbox::check_memory_quota(uint32_t allocation_size) const {
    if (manifest_.max_memory_bytes == 0) {
        return true; // 0 means no limit (or privileged system task)
    }
    // Overflow check
    if (current_memory_usage_ + allocation_size < current_memory_usage_) {
        return false;
    }
    return (current_memory_usage_ + allocation_size) <= manifest_.max_memory_bytes;
}

bool AppSandbox::allocate_memory(uint32_t size) {
    if (!check_memory_quota(size)) {
        record_violation("Memory quota exceeded");
        return false;
    }
    current_memory_usage_ += size;
    return true;
}

void AppSandbox::free_memory(uint32_t size) {
    if (current_memory_usage_ >= size) {
        current_memory_usage_ -= size;
    } else {
        current_memory_usage_ = 0; // Prevent underflow
    }
}

bool AppSandbox::check_cpu_quota(uint32_t current_cpu_percent) const {
    if (manifest_.max_cpu_percent == 0) {
        return true; // Unrestricted
    }
    if (current_cpu_percent > manifest_.max_cpu_percent) {
        return false;
    }
    return true;
}

void AppSandbox::record_violation(const char* reason) {
    status_ = SandboxStatus::Violation;
    violation_count_++;
    if (reason) {
        size_t len = 0;
        while (reason[len] && len < sizeof(violation_reason_) - 1) {
            violation_reason_[len] = reason[len];
            len++;
        }
        violation_reason_[len] = '\0';
    }
}

void AppSandbox::reset_violation() {
    status_ = SandboxStatus::Active;
    violation_reason_[0] = '\0';
}

} // namespace runtime
} // namespace auroraos
