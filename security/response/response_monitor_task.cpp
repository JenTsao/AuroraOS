// =============================================================================
// security/response/response_monitor_task.cpp
//
// 自动响应监控任务：低优先级后台任务，周期驱动响应引擎，
// 并轮询 NIDS / HIDS 的最新告警触发自动响应。
//
//   - 周期调用 ResponseEngine::tick()（到期解封/解除隔离）
//   - 轮询 IdsEngine / HidsEngine 的新告警 → ResponseEngine::handle_alert()
// =============================================================================

#include "response_engine.hpp"
#include "../ids/ids_engine.hpp"
#include "../hids/hids_engine.hpp"
#include "../../kernel/task/task.hpp"

namespace aurora {
namespace response {

namespace {

alignas(8) static uint32_t g_response_task_stack[256];

constexpr uint32_t kPollIntervalMs = 500;

ResponseSeverity to_response_sev(uint8_t rank) {
    return static_cast<ResponseSeverity>(rank);
}

} // namespace

// ---- 响应监控任务入口 ----
void response_monitor_task_entry() {
    ResponseEngine& resp = ResponseEngine::instance();
    ids::IdsEngine& nids = ids::IdsEngine::instance();
    hids::HidsEngine& hids = hids::HidsEngine::instance();

    resp.init();
    nids.init();
    hids.init();

    uint32_t nids_cursor = 0;
    uint32_t hids_cursor = 0;

    while (true) {
        // 1. 到期解封 / 解除隔离
        resp.tick();

        // 2. 轮询 NIDS 新告警 → 自动响应（按源 IP 封禁）
        const uint32_t nids_total = nids.get_alert_count();
        for (uint32_t i = nids_cursor; i < nids_total; ++i) {
            const ids::IdsAlert* a = nids.get_alert(static_cast<int>(i));
            if (a) {
                resp.handle_alert(to_response_sev(static_cast<uint8_t>(a->severity)),
                                  a->src_ip, ResponseEngine::kNoTask, a->description);
            }
        }
        nids_cursor = nids_total;

        // 3. 轮询 HIDS 新告警 → 自动响应（主机侧：取证快照）
        const uint32_t hids_total = hids.get_alert_count();
        for (uint32_t i = hids_cursor; i < hids_total; ++i) {
            const hids::HidsAlert* a = hids.get_alert(static_cast<int>(i));
            if (a) {
                resp.handle_alert(to_response_sev(static_cast<uint8_t>(a->severity)),
                                  0, ResponseEngine::kNoTask, a->description);
            }
        }
        hids_cursor = hids_total;

        Scheduler::instance().sleep_ms(kPollIntervalMs);
    }
}

// ---- 创建响应监控任务 ----
bool create_response_monitor_task() {
    ResponseEngine::instance().init();

    TaskControlBlock* tcb = Scheduler::instance().create_task(
        response_monitor_task_entry,
        g_response_task_stack,
        sizeof(g_response_task_stack),
        TaskPriority::Low, // 后台响应，不阻塞交互
        10,                // size_pow2: 2^10 = 1024 字节栈区域
        TaskPrivilege::Kernel
    );

    return tcb != nullptr;
}

} // namespace response
} // namespace aurora
