// =============================================================================
// security/hids/process_monitor.hpp
//
// HIDS 任务行为监控：栈溢出（金丝雀）+ 异常终止审计
//
//   - 扫描所有任务的栈水印哨兵（Scheduler::STACK_CANARY），检测栈溢出
//   - 审计 Terminated 状态的任务（异常终止）
//   - 变化检测：仅当计数较上次增加时才上报新发现
//
// 复用 AuroraOS 特性：Scheduler 任务表 + 栈金丝雀 + SecurityMonitor（经引擎上报）
// 设计原则（遵循 AGENTS.md）：零堆分配、noexcept
// =============================================================================
#ifndef AURORA_HIDS_PROCESS_MONITOR_HPP
#define AURORA_HIDS_PROCESS_MONITOR_HPP

#include <stdint.h>
#include "../../kernel/task/task.hpp"

namespace aurora {
namespace hids {

class ProcessMonitor {
public:
    // 扫描所有任务，返回本次新发现数
    int scan() {
        Scheduler& sched = Scheduler::instance();
        const int count = sched.get_task_count();

        uint32_t overflow = 0;
        uint32_t terminated = 0;
        uint32_t last_terminated = 0;

        for (int i = 0; i < count; ++i) {
            TaskControlBlock* tcb = sched.get_task(i);
            if (!tcb || tcb->scheduler.state == TaskState::Unallocated)
                continue;

            // 1. 栈溢出：金丝雀被破坏且任务仍存活
            if (tcb->task.stack_canary_ptr != nullptr &&
                *tcb->task.stack_canary_ptr != Scheduler::STACK_CANARY &&
                tcb->scheduler.state != TaskState::Terminated) {
                ++overflow;
                record_("stack overflow", static_cast<uint32_t>(i));
            }

            // 2. 异常终止审计
            if (tcb->scheduler.state == TaskState::Terminated) {
                ++terminated;
                last_terminated = static_cast<uint32_t>(i);
            }
        }

        int findings = 0;
        if (overflow > last_overflow_count_) {
            findings += static_cast<int>(overflow - last_overflow_count_);
        }
        if (terminated > last_terminated_count_) {
            findings += static_cast<int>(terminated - last_terminated_count_);
            record_("task terminated", last_terminated);
        }
        last_overflow_count_ = overflow;
        last_terminated_count_ = terminated;

        return findings;
    }

    const char* get_name() const noexcept {
        return "process_monitor";
    }

    uint32_t get_total_findings() const noexcept {
        return total_findings_;
    }

    uint32_t get_overflow_count() const noexcept {
        return last_overflow_count_;
    }

    uint32_t get_terminated_count() const noexcept {
        return last_terminated_count_;
    }

    const char* get_last_finding() const noexcept {
        return last_finding_;
    }

    void reset() noexcept {
        total_findings_ = 0;
        last_overflow_count_ = 0;
        last_terminated_count_ = 0;
        last_finding_[0] = '\0';
    }

private:
    uint32_t total_findings_ = 0;
    uint32_t last_overflow_count_ = 0;
    uint32_t last_terminated_count_ = 0;
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

#endif // AURORA_HIDS_PROCESS_MONITOR_HPP
