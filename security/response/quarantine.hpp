// =============================================================================
// security/response/quarantine.hpp
//
// 隔离受感染任务/设备
//
//   - quarantine_task()：挂起受感染任务（Scheduler::set_task_state → Suspended）
//   - quarantine_device()：按接口封禁设备流量（动态防火墙规则 match_interface + DROP）
//   - tick()：超时自动解除隔离
//
// 复用 AuroraOS 特性：Scheduler 任务状态机 + FirewallEngine 动态规则
// 设计原则（遵循 AGENTS.md）：固定数组、零堆分配、noexcept
// =============================================================================
#ifndef AURORA_RESPONSE_QUARANTINE_HPP
#define AURORA_RESPONSE_QUARANTINE_HPP

#include <stdint.h>
#include <string.h>
#include "../../kernel/task/task.hpp"
#include "../../net/firewall/firewall_engine.hpp"

// 系统 tick 计数器
extern volatile uint32_t tick_count;

namespace aurora {
namespace response {

class QuarantineManager {
public:
    static constexpr int kMaxQuarantine = 8;
    static constexpr uint32_t kDefaultQuarantineMs = 60000;
    static constexpr uint32_t kPermanent = 0xFFFFFFFFu;
    static constexpr uint32_t kNoTask = 0xFFFFFFFFu;

    // 隔离任务（挂起）。返回是否成功。
    bool quarantine_task(uint32_t task_id, uint32_t duration_ms = kDefaultQuarantineMs) {
        if (task_id == kNoTask)
            return false;
        TaskControlBlock* tcb = Scheduler::instance().get_task_by_id(task_id);
        if (!tcb)
            return false;

        // 已隔离 → 刷新到期时间
        for (int i = 0; i < kMaxQuarantine; ++i) {
            if (tasks_[i].active && tasks_[i].task_id == task_id) {
                tasks_[i].expire_tick = (duration_ms == 0) ? kPermanent : now_ms_() + duration_ms;
                return true;
            }
        }

        for (int i = 0; i < kMaxQuarantine; ++i) {
            if (!tasks_[i].active) {
                tasks_[i].active = true;
                tasks_[i].task_id = task_id;
                tasks_[i].expire_tick = (duration_ms == 0) ? kPermanent : now_ms_() + duration_ms;
                Scheduler::instance().set_task_state(task_id, TaskState::Suspended);
                record_(task_id, "task quarantined");
                return true;
            }
        }
        return false;
    }

    // 解除任务隔离（恢复就绪）
    bool release_task(uint32_t task_id) {
        for (int i = 0; i < kMaxQuarantine; ++i) {
            if (tasks_[i].active && tasks_[i].task_id == task_id) {
                tasks_[i].active = false;
                Scheduler::instance().set_task_state(task_id, TaskState::Ready);
                record_(task_id, "task released");
                return true;
            }
        }
        return false;
    }

    bool is_quarantined(uint32_t task_id) const {
        for (int i = 0; i < kMaxQuarantine; ++i)
            if (tasks_[i].active && tasks_[i].task_id == task_id)
                return true;
        return false;
    }

    // 隔离设备（按接口封禁流量）
    bool quarantine_device(const char* ifname, uint32_t duration_ms = kDefaultQuarantineMs) {
        if (!ifname)
            return false;

        for (int i = 0; i < kMaxQuarantine; ++i) {
            if (devices_[i].active && strncmp(devices_[i].ifname, ifname, sizeof(devices_[i].ifname)) == 0) {
                devices_[i].expire_tick = (duration_ms == 0) ? kPermanent : now_ms_() + duration_ms;
                return true;
            }
        }

        for (int i = 0; i < kMaxQuarantine; ++i) {
            if (!devices_[i].active) {
                devices_[i].active = true;
                copy_ifname_(devices_[i].ifname, ifname);
                devices_[i].expire_tick = (duration_ms == 0) ? kPermanent : now_ms_() + duration_ms;

                FwRule rule{};
                rule.match_interface = true;
                copy_ifname_(rule.interface, ifname);
                rule.action = FwAction::DROP;
                FirewallEngine::instance().get_rule_table().add_rule(rule);

                record_dev_(ifname, "device quarantined");
                return true;
            }
        }
        return false;
    }

    bool release_device(const char* ifname) {
        for (int i = 0; i < kMaxQuarantine; ++i) {
            if (devices_[i].active && strncmp(devices_[i].ifname, ifname, sizeof(devices_[i].ifname)) == 0) {
                devices_[i].active = false;
                const int r = find_device_rule_(ifname);
                if (r >= 0)
                    FirewallEngine::instance().get_rule_table().delete_rule(r);
                record_dev_(ifname, "device released");
                return true;
            }
        }
        return false;
    }

    void tick() {
        const uint32_t now = now_ms_();
        for (int i = 0; i < kMaxQuarantine; ++i) {
            if (tasks_[i].active && tasks_[i].expire_tick != kPermanent && now >= tasks_[i].expire_tick) {
                release_task(tasks_[i].task_id);
            }
            if (devices_[i].active && devices_[i].expire_tick != kPermanent && now >= devices_[i].expire_tick) {
                release_device(devices_[i].ifname);
            }
        }
    }

    int get_task_quarantine_count() const {
        int n = 0;
        for (int i = 0; i < kMaxQuarantine; ++i)
            if (tasks_[i].active)
                ++n;
        return n;
    }

    int get_device_quarantine_count() const {
        int n = 0;
        for (int i = 0; i < kMaxQuarantine; ++i)
            if (devices_[i].active)
                ++n;
        return n;
    }

    const char* get_last_action() const noexcept {
        return last_action_;
    }

    void reset() {
        for (int i = 0; i < kMaxQuarantine; ++i) {
            if (tasks_[i].active) {
                Scheduler::instance().set_task_state(tasks_[i].task_id, TaskState::Ready);
                tasks_[i].active = false;
            }
            if (devices_[i].active) {
                const int r = find_device_rule_(devices_[i].ifname);
                if (r >= 0)
                    FirewallEngine::instance().get_rule_table().delete_rule(r);
                devices_[i].active = false;
            }
        }
        last_action_[0] = '\0';
    }

private:
    struct TaskQuarantine {
        bool active = false;
        uint32_t task_id = 0;
        uint32_t expire_tick = 0;
    };
    struct DeviceQuarantine {
        bool active = false;
        char ifname[8] = {0};
        uint32_t expire_tick = 0;
    };

    TaskQuarantine tasks_[kMaxQuarantine]{};
    DeviceQuarantine devices_[kMaxQuarantine]{};
    char last_action_[64]{};

    static uint32_t now_ms_() noexcept {
        return tick_count;
    }

    static int find_device_rule_(const char* ifname) {
        const FwRule* rules = FirewallEngine::instance().get_rule_table().get_rules();
        for (int i = 0; i < RuleTable::MAX_RULES; ++i) {
            const FwRule& r = rules[i];
            if (r.enabled && r.match_interface && r.action == FwAction::DROP &&
                strncmp(r.interface, ifname, sizeof(r.interface)) == 0) {
                return i;
            }
        }
        return -1;
    }

    static void copy_ifname_(char* dst, const char* src) {
        int i = 0;
        while (src[i] && i < 7) {
            dst[i] = src[i];
            ++i;
        }
        dst[i] = '\0';
    }

    void record_(uint32_t id, const char* what) {
        char* p = last_action_;
        const char* const end = last_action_ + sizeof(last_action_) - 1;
        while (*what && p < end)
            *p++ = *what++;
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

    void record_dev_(const char* ifname, const char* what) {
        char* p = last_action_;
        const char* const end = last_action_ + sizeof(last_action_) - 1;
        while (*what && p < end)
            *p++ = *what++;
        if (p < end)
            *p++ = ' ';
        while (*ifname && p < end)
            *p++ = *ifname++;
        *p = '\0';
    }
};

} // namespace response
} // namespace aurora

#endif // AURORA_RESPONSE_QUARANTINE_HPP
