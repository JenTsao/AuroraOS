#ifndef AURORAOS_RUNTIME_APP_SANDBOX_HPP
#define AURORAOS_RUNTIME_APP_SANDBOX_HPP

#include "app_manifest.hpp"

namespace auroraos {
namespace runtime {

class AppSandbox {
public:
    AppSandbox(const AppManifest& manifest);

    // 检查应用是否具备某种能力
    bool has_capability(AppCapability cap) const;

    // 检查当前内存使用是否超出配额
    bool check_memory_quota(uint32_t allocation_size) const;
    void allocate_memory(uint32_t size);
    void free_memory(uint32_t size);

    const AppManifest& get_manifest() const {
        return manifest_;
    }

private:
    AppManifest manifest_;

    // 动态资源追踪
    uint32_t current_memory_usage_;
};

} // namespace runtime
} // namespace auroraos

#endif // AURORAOS_RUNTIME_APP_SANDBOX_HPP
