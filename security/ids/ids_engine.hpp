// =============================================================================
// security/ids/ids_engine.hpp
//
// NIDS 核心引擎：组合签名匹配 + 流量分析 + 异常检测 + 告警管理
//
//   - IdsEngine 单例：process_packet() 驱动完整检测管线，
//     tick() 周期推进窗口/基线，init() 挂载 /proc/ids 状态节点
//   - 与 SecurityMonitor 联动（经 AlertManager），串口告警通道
//   - 屏幕 / 网络推送通道：通过 /proc/ids 暴露状态，由 UI / 网络服务消费
//
// 设计原则（遵循 AGENTS.md）：组合优于继承、单一职责、固定数组、零堆分配
// =============================================================================
#ifndef AURORA_IDS_ENGINE_HPP
#define AURORA_IDS_ENGINE_HPP

#include <stdint.h>
#include "../../vfs/vfs.hpp"
#include "../../vfs/procfs.hpp"
#include "alert_manager.hpp"
#include "signature_db.hpp"
#include "traffic_analyzer.hpp"
#include "anomaly_detector.hpp"

namespace aurora {
namespace ids {

// ---------------------------------------------------------------------------
// ProcFS 节点: /proc/ids
// ---------------------------------------------------------------------------
class IdsNode : public ProcNode {
public:
    void set_engine(class IdsEngine* e) {
        engine_ = e;
    }

    int read(char* buf, int len, int offset, void* priv) override;

private:
    class IdsEngine* engine_ = nullptr;
};

// ---------------------------------------------------------------------------
// IdsEngine
// ---------------------------------------------------------------------------
class IdsEngine {
public:
    static IdsEngine& instance() {
        static IdsEngine engine;
        return engine;
    }

    void init() {
        if (initialized_)
            return;
        sigdb_.init();
        node_.set_engine(this);
        VfsManager::instance().mount("/proc/ids", &node_);
        initialized_ = true;
    }

    void process_packet(const uint8_t* pkt, int len) {
        if (!enabled_)
            return;
        ++packet_count_;
        detector_.process_packet(pkt, len);
    }

    void tick() {
        detector_.tick();
    }

    void enable(bool e) {
        enabled_ = e;
    }

    bool is_enabled() const {
        return enabled_;
    }

    // ---- 查询 ----
    uint32_t get_packet_count() const {
        return packet_count_;
    }

    uint32_t get_alert_count() const {
        return alerts_.get_alert_count();
    }

    const IdsAlert* get_alert(int index) const {
        return alerts_.get_alert(index);
    }

    void reset() {
        detector_.reset();
        alerts_.reset();
        packet_count_ = 0;
    }

    // ---- 配置透传 ----
    void set_port_scan_threshold(uint8_t n) {
        detector_.set_port_scan_threshold(n);
    }

    void set_host_scan_threshold(uint8_t n) {
        detector_.set_host_scan_threshold(n);
    }

    void set_syn_flood_threshold(uint32_t n) {
        detector_.set_syn_flood_threshold(n);
    }

    void set_dns_query_threshold(uint32_t n) {
        detector_.set_dns_query_threshold(n);
    }

private:
    IdsEngine() : detector_(traffic_, sigdb_, alerts_) {}

    TrafficAnalyzer traffic_;
    SignatureDb sigdb_;
    AlertManager alerts_;
    AnomalyDetector detector_;

    IdsNode node_;
    bool enabled_ = true;
    bool initialized_ = false;
    uint32_t packet_count_ = 0;

    friend class IdsNode;
};

// =============================================================================
// ProcFS 节点实现（header-only，与 wireless_ids.hpp 一致）
// =============================================================================
inline int IdsNode::read(char* buf, int len, int /*offset*/, void* /*priv*/) {
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

    app_s("AuroraOS NIDS Status\n");
    app_s("===================\n");
    app_s("Enabled: ");
    app_s(engine_->is_enabled() ? "yes" : "no");
    app_s("\n");
    app_s("Packets: ");
    app_u(engine_->get_packet_count());
    app_s("\n");
    app_s("Alerts: ");
    app_u(engine_->get_alert_count());
    app_s("\n");
    app_s("\n--- Recent Alerts ---\n");

    const uint32_t total = engine_->get_alert_count();
    const uint32_t start = (total > 20) ? (total - 20) : 0;
    for (uint32_t i = start; i < total && pos < len - 96; ++i) {
        const IdsAlert* a = engine_->get_alert(static_cast<int>(i));
        if (!a)
            continue;

        app_u(a->timestamp_ms);
        app_s(" ");
        app_s(AlertManager::category_name(a->category));
        app_s(" x");
        app_u(a->count);
        app_s(" [");
        app_u(static_cast<uint32_t>(a->severity));
        app_s("] ");
        app_s(a->description);
        app_s("\n");
    }

    buf[pos] = '\0';
    return pos;
}

} // namespace ids
} // namespace aurora

#endif // AURORA_IDS_ENGINE_HPP
