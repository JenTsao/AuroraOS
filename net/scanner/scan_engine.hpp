#ifndef AURORA_SCANNER_SCAN_ENGINE_HPP
#define AURORA_SCANNER_SCAN_ENGINE_HPP

#include <stdint.h>
#include <stddef.h>

#include "scan_handler.hpp"
#include "../../kernel/task_notify.hpp"
#include "../../kernel/task/task.hpp"
#include "../../kernel/mm/memory_pool.hpp"
#include "../../kernel/core/mutex.hpp"
#include "../../vfs/vfs.hpp"
#include "../../vfs/procfs.hpp"

// ============================================================
// Scan Engine -- 网络扫描总控引擎
//
// 架构设计：
//   ┌─────────────────────────────────────────┐
//   │  ScanEngine (Singleton)                 │
//   │  ┌─────────────────────────────────┐    │
//   │  │ Job Queue (TaskNotify IPC)      │    │
//   │  │  ├─ Worker 1 ──► PortScanner    │    │
//   │  │  ├─ Worker 2 ──► HostDiscovery  │    │
//   │  │  ├─ Worker 3 ──► ServiceDetector│    │
//   │  │  └─ Worker 4 ──► VulnProbe      │    │
//   │  └─────────────────────────────────┘    │
//   │  ┌─────────────────────────────────┐    │
//   │  │ Result Ring Buffer (64 slots)   │    │
//   │  │    ↓                            │    │
//   │  │ /proc/scan_results (ProcFS)    │    │
//   │  └─────────────────────────────────┘    │
//   │  ┌─────────────────────────────────┐    │
//   │  │ Lua Bindings (MiniProgramEngine)│    │
//   │  └─────────────────────────────────┘    │
//   └─────────────────────────────────────────┘
//
// TaskNotify 零开销 IPC:
//   - 主控任务通过 TaskNotify::give() 分配扫描目标给 Worker
//   - Worker 通过 TaskNotify::take() 阻塞等待任务
//   - 5 级优先级调度：Worker(Low=1) 不阻塞系统交互(Normal=2)
//
// 实现文件: scan_engine.cpp
// ============================================================

enum class ScanJobType : uint8_t {
    TcpPortScan    = 0,
    UdpPortScan    = 1,
    AckPortScan    = 2,
    ArpDiscovery   = 3,
    IcmpPing       = 4,
    ServiceDetect  = 5,
    VulnProbe      = 6,
};

struct ScanJobDesc {
    ScanJobType job_type;
    uint32_t    ip;
    uint16_t    port;
    uint16_t    job_id;
};

struct UnifiedScanResult {
    uint32_t     ip;
    uint16_t     port;
    uint8_t      host_state;
    uint8_t      port_state;
    uint8_t      scan_type;
    char         service_name[32];
    char         version[64];
    char         banner[256];
    char         cve_id[32];
    uint8_t      severity;
    uint32_t     latency_ms;
    uint32_t     timestamp;
};

struct WorkerContext {
    uint32_t         worker_id;
    uint32_t         controller_task_id;
    bool             running;
};

// ============================================================
// ProcFS 节点: /proc/scan_results
// ============================================================
class ScanResultNode : public ProcNode {
public:
    void set_engine(class ScanEngine* engine) { engine_ = engine; }
    int read(char* buf, int len, int offset, void* priv) override;

private:
    class ScanEngine* engine_ = nullptr;
};

// ============================================================
// Scan Engine 主类
// ============================================================
class ScanEngine {
public:
    static ScanEngine& instance() {
        static ScanEngine engine;
        return engine;
    }

    // ---- 初始化 (.cpp 实现) ----
    bool init(uint32_t controller_task_id, int worker_count = 4,
              struct netif* netif = nullptr);

    // ---- 扫描 API (.cpp 实现) ----

    int start_tcp_port_scan(uint32_t target_ip, const uint16_t* ports,
                            int port_count, uint32_t timeout_ms = 2000);
    int start_host_discovery(uint32_t network_prefix, uint32_t timeout_ms = 1500);
    int start_service_detection(uint32_t target_ip, const uint16_t* ports,
                                int port_count, uint32_t timeout_ms = 3000);
    int start_vuln_probe(uint32_t target_ip, const uint16_t* ports,
                         int port_count, const char* service_name = nullptr,
                         uint32_t timeout_ms = 3000);
    int start_full_scan(uint32_t network_prefix, const uint16_t* ports,
                        int port_count, uint32_t timeout_ms = 2000);

    // ---- 结果查询 (.cpp 实现) ----
    int get_result_count() const;
    bool get_result(int index, UnifiedScanResult& out) const;
    bool get_latest_result(UnifiedScanResult& out) const;
    void clear_results();

    // ---- 配置 (内联) ----
    void set_banner_timeout(uint32_t timeout_ms) {
        banner_timeout_ms_ = timeout_ms;
    }
    void set_vuln_timeout(uint32_t timeout_ms) {
        vuln_timeout_ms_ = timeout_ms;
    }
    uint32_t get_banner_timeout() const { return banner_timeout_ms_; }
    uint32_t get_vuln_timeout() const { return vuln_timeout_ms_; }

    void register_handler(ScanJobType type, IScanHandler* handler);

    // ---- 便捷方法 (.cpp 实现) ----
    int quick_scan(uint32_t ip, const uint16_t* ports, int port_count,
                   UnifiedScanResult* out_results, int max_results);

    // ---- Lua 绑定 (.cpp 实现) ----
    static void register_lua_bindings(void* lua_state);

private:
    ScanEngine() = default;

    static constexpr int max_workers_ = 8;
    static constexpr int max_results_ = 64;
    static constexpr int max_pending_jobs_ = 128;
    static constexpr int worker_stack_size_ = 1024;

    bool initialized_ = false;
    uint32_t controller_task_id_ = 0;
    int worker_count_ = 0;
    uint16_t next_job_id_ = 1;

    WorkerContext* workers_[max_workers_]{};
    MemoryPool<WorkerContext, max_workers_> worker_pool_;
    uint32_t worker_stacks_[max_workers_][worker_stack_size_ / 4]{};

    UnifiedScanResult result_buffer_[max_results_]{};
    mutable Mutex result_mutex_;
    int result_count_ = 0;

    ScanJobDesc pending_jobs_[max_pending_jobs_]{};
    mutable Mutex job_mutex_;
    int job_head_ = 0;
    int job_tail_ = 0;
    int job_count_ = 0;

    uint32_t banner_timeout_ms_ = 3000;
    uint32_t vuln_timeout_ms_ = 3000;
    
    IScanHandler* handlers_[8]{};

    ScanResultNode   scan_result_node_;

    // ---- 任务管理 (.cpp 实现) ----
    bool create_worker_task_(int index, WorkerContext* ctx);
    static void worker_entry_();

    // ---- 作业队列 (.cpp 实现) ----
    bool dispatch_job_(const ScanJobDesc& job, uint32_t timeout_ms);
    bool dequeue_job_(ScanJobDesc& out);

    // ---- 作业执行 (.cpp 实现) ----
    void execute_job_(const ScanJobDesc& job);

    // ---- 结果管理 (.cpp 实现) ----
    void append_result_(const UnifiedScanResult& result);
public:
    void find_service_for_target(uint32_t ip, uint16_t port,
                                   char* out_buf, int max_len);
private:
};

#endif // AURORA_SCANNER_SCAN_ENGINE_HPP
