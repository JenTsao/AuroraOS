// =============================================================================
// security/hids/privilege_auditor.hpp
//
// HIDS 权限提升检测：审计任务能力空间（CSpace）中的权限异常
//
//   - 扫描所有用户态任务：持有 grant 权限的能力视为权限提升风险
//     （用户态不应持有可派生/授予内核对象的能力）
//   - report_escalation_attempt()：记录被 CSpace 拒绝的派生/授予提升尝试
//
// 复用 AuroraOS 特性：CSpace 能力空间（cap_derive/mint/grant 的防提升语义）
// 设计原则（遵循 AGENTS.md）：零堆分配、noexcept
// =============================================================================
#ifndef AURORA_HIDS_PRIVILEGE_AUDITOR_HPP
#define AURORA_HIDS_PRIVILEGE_AUDITOR_HPP

#include <stdint.h>
#include "../../kernel/task/task.hpp"
#include "../../kernel/core/cspace.hpp"

namespace aurora {
namespace hids {

class PrivilegeAuditor {
public:
    // 审计所有用户态任务的能力空间，返回本次新发现数
    int scan() {
        Scheduler& sched = Scheduler::instance();
        const int count = sched.get_task_count();

        uint32_t violations = 0;
        uint32_t last_task = 0;

        for (int i = 0; i < count; ++i) {
            TaskControlBlock* tcb = sched.get_task(i);
            if (!tcb || tcb->scheduler.state == TaskState::Unallocated)
                continue;
            if (tcb->task.privilege != static_cast<uint32_t>(TaskPrivilege::User))
                continue;

            for (int slot = 0; slot < auroraos::kernel::MAX_CSPACE_SLOTS; ++slot) {
                const auroraos::kernel::Capability& cap = tcb->security.cspace[slot];
                if (cap.type == auroraos::kernel::CapType::Null)
                    continue;
                // 用户态任务持有 grant 权限 = 可派生/授予能力，权限提升风险
                if (cap.rights.grant) {
                    ++violations;
                    last_task = static_cast<uint32_t>(i);
                    break;
                }
            }
        }

        int findings = 0;
        if (violations > last_violation_count_) {
            findings += static_cast<int>(violations - last_violation_count_);
            record_("user task holds grant capability", last_task);
        }
        last_violation_count_ = violations;

        return findings;
    }

    // 记录一次被拒绝的权限提升尝试（cap_derive/mint/grant 返回 false 时由上层调用）
    void report_escalation_attempt(uint32_t task_id) {
        ++attempt_count_;
        record_("privilege escalation attempt", task_id);
    }

    const char* get_name() const noexcept {
        return "privilege_auditor";
    }

    uint32_t get_total_findings() const noexcept {
        return total_findings_;
    }

    uint32_t get_attempt_count() const noexcept {
        return attempt_count_;
    }

    const char* get_last_finding() const noexcept {
        return last_finding_;
    }

    void reset() noexcept {
        total_findings_ = 0;
        attempt_count_ = 0;
        last_violation_count_ = 0;
        last_finding_[0] = '\0';
    }

private:
    uint32_t total_findings_ = 0;
    uint32_t attempt_count_ = 0;
    uint32_t last_violation_count_ = 0;
    char last_finding_[64]{};

    void record_(const char* what, uint32_t id) {
        ++total_findings_;
        char* p = last_finding_;
        const char* const end = last_finding_ + sizeof(last_finding_) - 1;
        while (*what && p < end)
            *p++ = *what++;
        if (p < end)
            *p++ = ':';
        if (p < end)
            *p++ = ' ';
        char digits[10];
        int n = 0;
        uint32_t v = id;
        do {
            digits[n++] = static_cast<char>('0' + (v % 10u));
            v /= 10u;
        } while (v && n < 10);
        while (n > 0 && p < end)
            *p++ = digits[--n];
        *p = '\0';
    }
};

} // namespace hids
} // namespace aurora

#endif // AURORA_HIDS_PRIVILEGE_AUDITOR_HPP
