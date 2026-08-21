// =============================================================================
// security/response/forensic_snapshot.hpp
//
// 取证快照：内存 + 流量
//
//   - capture()：记录堆使用率、任务状态、指定内存区域副本、流量统计到环形缓冲
//   - set_traffic_stats()：由响应引擎/网络层喂入流量计数
//
// 复用 AuroraOS 特性：KernelHeap 堆统计 + Scheduler 任务表
// 设计原则（遵循 AGENTS.md）：固定数组、零堆分配、noexcept
// =============================================================================
#ifndef AURORA_RESPONSE_FORENSIC_SNAPSHOT_HPP
#define AURORA_RESPONSE_FORENSIC_SNAPSHOT_HPP

#include <stdint.h>
#include <stddef.h>
#include "../../kernel/mm/memory.hpp"
#include "../../kernel/task/task.hpp"

// 系统 tick 计数器
extern volatile uint32_t tick_count;

namespace aurora {
namespace response {

class ForensicRecorder {
public:
    static constexpr int kMaxSnapshots = 8;
    static constexpr int kRegionMax = 64;

    struct Snapshot {
        uint32_t timestamp_ms;
        size_t heap_free;
        size_t heap_total;
        uint32_t task_count;
        uint32_t terminated_tasks;
        uint32_t traffic_packets;
        uint32_t traffic_bytes;
        uintptr_t region_addr;
        uint32_t region_len;
        uint8_t region[kRegionMax];
    };

    // 抓取一次快照。region 为要保留的内存区域（可为 nullptr）。
    void capture(const void* region, int len) {
        Snapshot& s = snapshots_[snapshot_count_ % kMaxSnapshots];
        ++snapshot_count_;

        s.timestamp_ms = tick_count;
        s.heap_free = KernelHeap::instance().get_free_memory();
        s.heap_total = KernelHeap::instance().get_total_memory();
        s.task_count = static_cast<uint32_t>(Scheduler::instance().get_task_count());
        s.terminated_tasks = count_terminated_();
        s.traffic_packets = traffic_packets_;
        s.traffic_bytes = traffic_bytes_;
        s.region_addr = reinterpret_cast<uintptr_t>(region);
        s.region_len = 0;

        if (region && len > 0) {
            const int n = (len > kRegionMax) ? kRegionMax : len;
            const uint8_t* src = static_cast<const uint8_t*>(region);
            for (int i = 0; i < n; ++i)
                s.region[i] = src[i];
            s.region_len = static_cast<uint32_t>(n);
        }
    }

    // 由网络层/响应引擎喂入流量统计
    void set_traffic_stats(uint32_t packets, uint32_t bytes) noexcept {
        traffic_packets_ = packets;
        traffic_bytes_ = bytes;
    }

    uint32_t get_snapshot_count() const noexcept {
        return snapshot_count_;
    }

    const Snapshot* get_snapshot(int index) const {
        if (index < 0)
            return nullptr;
        return &snapshots_[static_cast<uint32_t>(index) % kMaxSnapshots];
    }

    void reset() noexcept {
        snapshot_count_ = 0;
        traffic_packets_ = 0;
        traffic_bytes_ = 0;
    }

private:
    Snapshot snapshots_[kMaxSnapshots]{};
    uint32_t snapshot_count_ = 0;
    uint32_t traffic_packets_ = 0;
    uint32_t traffic_bytes_ = 0;

    static uint32_t count_terminated_() {
        Scheduler& sched = Scheduler::instance();
        uint32_t n = 0;
        const int count = sched.get_task_count();
        for (int i = 0; i < count; ++i) {
            TaskControlBlock* tcb = sched.get_task(i);
            if (tcb && tcb->scheduler.state == TaskState::Terminated)
                ++n;
        }
        return n;
    }
};

} // namespace response
} // namespace aurora

#endif // AURORA_RESPONSE_FORENSIC_SNAPSHOT_HPP
