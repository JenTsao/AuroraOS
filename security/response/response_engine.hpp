// =============================================================================
// security/response/response_engine.hpp
//
// 自动响应策略引擎：组合自动封禁 + 隔离 + 取证快照
//
//   - handle_alert()：根据告警严重度与来源执行策略
//       High/Critical + src_ip → 自动封禁 + 取证快照
//       High/Critical + task_id → 隔离任务
//       Medium 及以下 → 取证快照
//   - 维护响应动作环形缓冲 + /proc/response 状态节点 + SecurityMonitor 上报
//
// 设计原则（遵循 AGENTS.md）：组合优于继承、固定数组、零堆分配
// =============================================================================
#ifndef AURORA_RESPONSE_ENGINE_HPP
#define AURORA_RESPONSE_ENGINE_HPP

#include <stdint.h>
#include "../../kernel/core/mutex.hpp"
#include "../../kernel/core/security_monitor.hpp"
#include "../../vfs/vfs.hpp"
#include "../../vfs/procfs.hpp"
#include "auto_block.hpp"
#include "quarantine.hpp"
#include "forensic_snapshot.hpp"

namespace aurora {
namespace response {

// ---------------------------------------------------------------------------
// 响应严重度
// ---------------------------------------------------------------------------
enum class ResponseSeverity : uint8_t {
    Info = 0,
    Low = 1,
    Medium = 2,
    High = 3,
    Critical = 4,
};

inline uint8_t resp_sev_rank(ResponseSeverity s) {
    return static_cast<uint8_t>(s);
}

// ---------------------------------------------------------------------------
// 响应动作类型
// ---------------------------------------------------------------------------
enum class ResponseActionType : uint8_t {
    Ignore = 0,
    BlockSource = 1,
    QuarantineTask = 2,
    Snapshot = 3,
};

// ---------------------------------------------------------------------------
// 响应动作记录
// ---------------------------------------------------------------------------
struct ResponseAction {
    uint32_t timestamp_ms;
    ResponseSeverity severity;
    ResponseActionType type;
    uint32_t src_ip;
    uint32_t task_id;
    char description[64];
};

// ---------------------------------------------------------------------------
// ProcFS 节点: /proc/response
// ---------------------------------------------------------------------------
class ResponseNode : public ProcNode {
public:
    void set_engine(class ResponseEngine* e) {
        engine_ = e;
    }

    int read(char* buf, int len, int offset, void* priv) override;

private:
    class ResponseEngine* engine_ = nullptr;
};

// ---------------------------------------------------------------------------
// ResponseEngine
// ---------------------------------------------------------------------------
class ResponseEngine {
public:
    static constexpr int kMaxActions = 32;
    static constexpr uint32_t kNoTask = 0xFFFFFFFFu;

    static ResponseEngine& instance() {
        static ResponseEngine engine;
        return engine;
    }

    void init() {
        if (initialized_)
            return;
        node_.set_engine(this);
        VfsManager::instance().mount("/proc/response", &node_);
        initialized_ = true;
    }

    // 处理一条告警并执行响应策略
    void handle_alert(ResponseSeverity sev, uint32_t src_ip, uint32_t task_id, const char* desc) {
        ResponseAction a{};
        a.timestamp_ms = tick_count;
        a.severity = sev;
        a.src_ip = src_ip;
        a.task_id = task_id;
        copy_desc_(a.description, desc);

        const bool severe = (resp_sev_rank(sev) >= resp_sev_rank(ResponseSeverity::High));

        if (severe && src_ip != 0) {
            auto_block_.block_ip(src_ip);
            forensic_.capture(nullptr, 0);
            a.type = ResponseActionType::BlockSource;
        } else if (severe && task_id != kNoTask) {
            quarantine_.quarantine_task(task_id);
            forensic_.capture(nullptr, 0);
            a.type = ResponseActionType::QuarantineTask;
        } else if (resp_sev_rank(sev) >= resp_sev_rank(ResponseSeverity::Medium)) {
            forensic_.capture(nullptr, 0);
            a.type = ResponseActionType::Snapshot;
        } else {
            a.type = ResponseActionType::Ignore;
        }

        log_action_(a);

        if (severe) {
            SecurityMonitor::instance().report_firewall_anomaly(desc);
        }
    }

    // 周期执行：到期解封/解除隔离
    void tick() {
        auto_block_.tick();
        quarantine_.tick();
    }

    // 访问器
    AutoBlockManager& auto_block() {
        return auto_block_;
    }

    QuarantineManager& quarantine() {
        return quarantine_;
    }

    ForensicRecorder& forensic() {
        return forensic_;
    }

    uint32_t get_action_count() const {
        return action_count_;
    }

    const ResponseAction* get_action(int index) const {
        if (index < 0)
            return nullptr;
        return &actions_[static_cast<uint32_t>(index) % kMaxActions];
    }

    void reset() {
        auto_block_.reset();
        quarantine_.reset();
        forensic_.reset();
        action_count_ = 0;
        for (int i = 0; i < kMaxActions; ++i)
            actions_[i] = ResponseAction{};
    }

private:
    ResponseEngine() = default;

    AutoBlockManager auto_block_;
    QuarantineManager quarantine_;
    ForensicRecorder forensic_;

    ResponseAction actions_[kMaxActions]{};
    uint32_t action_count_ = 0;
    Mutex action_mutex_;

    ResponseNode node_;
    bool initialized_ = false;

    void log_action_(const ResponseAction& a) {
        LockGuard lock(action_mutex_);
        actions_[action_count_ % kMaxActions] = a;
        ++action_count_;
    }

    static void copy_desc_(char* dst, const char* src) {
        int i = 0;
        while (src && src[i] && i < 63) {
            dst[i] = src[i];
            ++i;
        }
        dst[i] = '\0';
    }

    friend class ResponseNode;
};

// =============================================================================
// ProcFS 节点实现（header-only，与 wireless_ids.hpp 一致）
// =============================================================================
inline int ResponseNode::read(char* buf, int len, int /*offset*/, void* /*priv*/) {
    if (!engine_)
        return 0;

    int pos = 0;
    auto app_s = [&](const char* s) {
        while (*s && pos < len - 1)
            buf[pos++] = *s++;
    };
    auto app_u = [&](uint32_t v) {
        char tmp[16];
        int i = 0;
        if (v == 0) {
            tmp[i++] = '0';
        }
        while (v > 0) {
            tmp[i++] = static_cast<char>('0' + (v % 10u));
            v /= 10u;
        }
        while (i > 0 && pos < len - 1)
            buf[pos++] = tmp[--i];
    };

    app_s("AuroraOS Response Status\n");
    app_s("========================\n");
    app_s("Blocked IPs: ");
    app_u(static_cast<uint32_t>(engine_->auto_block_.get_block_count()));
    app_s("\n");
    app_s("Quarantined tasks: ");
    app_u(static_cast<uint32_t>(engine_->quarantine_.get_task_quarantine_count()));
    app_s("\n");
    app_s("Quarantined devices: ");
    app_u(static_cast<uint32_t>(engine_->quarantine_.get_device_quarantine_count()));
    app_s("\n");
    app_s("Snapshots: ");
    app_u(engine_->forensic_.get_snapshot_count());
    app_s("\n\nActions: ");
    app_u(engine_->get_action_count());
    app_s("\n--- Recent Actions ---\n");

    const uint32_t total = engine_->get_action_count();
    const uint32_t start = (total > 16) ? (total - 16) : 0;
    for (uint32_t i = start; i < total && pos < len - 96; ++i) {
        const ResponseAction* a = engine_->get_action(static_cast<int>(i));
        if (!a)
            continue;

        app_u(a->timestamp_ms);
        app_s(" [");
        app_u(static_cast<uint32_t>(a->severity));
        app_s("] ");
        switch (a->type) {
        case ResponseActionType::BlockSource:
            app_s("block");
            break;
        case ResponseActionType::QuarantineTask:
            app_s("quarantine");
            break;
        case ResponseActionType::Snapshot:
            app_s("snapshot");
            break;
        default:
            app_s("ignore");
            break;
        }
        app_s(" ");
        app_s(a->description);
        app_s("\n");
    }

    buf[pos] = '\0';
    return pos;
}

} // namespace response
} // namespace aurora

#endif // AURORA_RESPONSE_ENGINE_HPP
