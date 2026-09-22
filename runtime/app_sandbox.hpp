#ifndef AURORAOS_RUNTIME_APP_SANDBOX_HPP
#define AURORAOS_RUNTIME_APP_SANDBOX_HPP

#include "app_manifest.hpp"
#include <stdint.h>
#include <stddef.h>

namespace auroraos {
namespace runtime {

// 沙盒运行生命周期状态
enum class SandboxStatus : uint8_t {
    Active,      // 正常受控运行中
    Suspended,   // 被挂起/暂停
    Terminated,  // 已安全终止
    Violation    // 发生安全违规/资源超限
};

class AppSandbox {
public:
    explicit AppSandbox(const AppManifest& manifest);

    // 能力校验与动态授权/撤回
    bool has_capability(AppCapability cap) const;
    bool grant_capability(AppCapability cap);
    bool revoke_capability(AppCapability cap);

    // 内存配额审计与追踪
    bool check_memory_quota(uint32_t allocation_size) const;
    bool allocate_memory(uint32_t size);
    void free_memory(uint32_t size);
    uint32_t get_memory_usage() const { return current_memory_usage_; }
    uint32_t get_memory_limit() const { return manifest_.max_memory_bytes; }

    // CPU 配额审计
    bool check_cpu_quota(uint32_t current_cpu_percent) const;
    uint32_t get_cpu_limit() const { return manifest_.max_cpu_percent; }

    // 沙盒安全违规监控
    SandboxStatus get_status() const { return status_; }
    void set_status(SandboxStatus status) { status_ = status; }
    void record_violation(const char* reason);
    const char* get_violation_reason() const { return violation_reason_; }
    uint32_t get_violation_count() const { return violation_count_; }
    void reset_violation();

    // 关联的内核任务 ID
    void set_task_id(uint32_t tid) { task_id_ = tid; }
    uint32_t get_task_id() const { return task_id_; }

    const AppManifest& get_manifest() const {
        return manifest_;
    }

private:
    AppManifest manifest_;
    uint32_t current_memory_usage_ = 0;
    uint32_t task_id_ = 0;
    SandboxStatus status_ = SandboxStatus::Active;
    uint32_t violation_count_ = 0;
    char violation_reason_[64] = {0};
};

} // namespace runtime
} // namespace auroraos

#endif // AURORAOS_RUNTIME_APP_SANDBOX_HPP
