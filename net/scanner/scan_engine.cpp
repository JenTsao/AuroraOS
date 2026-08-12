// ============================================================
// scan_engine.cpp -- 网络扫描总控引擎实现
//
// 核心架构�?
//   ┌─────────────────────────────────────────�?
//   �? ScanEngine (Singleton)                 �?
//   �? ┌─────────────────────────────────�?   �?
//   �? �?Job Queue (TaskNotify IPC)      �?   �?
//   �? �? ├─ Worker 1 ──? PortScanner    �?   �?
//   �? �? ├─ Worker 2 ──? HostDiscovery  �?   �?
//   �? �? ├─ Worker 3 ──? ServiceDetector�?   �?
//   �? �? └─ Worker 4 ──? VulnProbe      �?   �?
//   �? └─────────────────────────────────�?   �?
//   �? ┌─────────────────────────────────�?   �?
//   �? �?Result Ring Buffer (64 slots)   �?   �?
//   �? �?   �?                           �?   �?
//   �? �?/proc/scan_results (ProcFS)    �?   �?
//   �? └─────────────────────────────────�?   �?
//   �? ┌─────────────────────────────────�?   �?
//   �? �?Lua Bindings (MiniProgramEngine)�?   �?
//   �? └─────────────────────────────────�?   �?
//   └─────────────────────────────────────────�?
//
// TaskNotify 零开销 IPC:
//   - 主控任务通过 TaskNotify::give() 分配扫描目标�?Worker
//   - Worker 通过 TaskNotify::take() 阻塞等待任务
//   - 5 级优先级调度：Worker(Low=1) 不阻塞系统交�?Normal=2)
// ============================================================

#include "scan_engine.hpp"
#include "handlers/scan_handlers.hpp"
#include "scan_lua_binding.hpp"
#include <stdint.h>
#include <stddef.h>

extern "C" {
#include "net_client.hpp"
#include "lwip/netif.h"
#include "lwip/inet.h"
}

// ---- 内核 API ----
#include "../../kernel/task_notify.hpp"
#include "../../kernel/task/task.hpp"
#include "../../kernel/memory_pool.hpp"
#include "../../kernel/core/mutex.hpp"
#include "../../vfs/vfs.hpp"

// ---- 系统全局符号 ----
extern volatile uint32_t tick_count;

// ---- ScanEngine 静态注�?----
// register_lua_bindings 的实现委托给 scan_lua_binding.cpp 的入口函�?
void ScanEngine::register_lua_bindings(void* lua_state) {
    if (lua_state) {
        register_scan_lua_bindings(static_cast<lua_State*>(lua_state));
    }
}

// ============================================================
// 工具函数
// ============================================================

static uint32_t get_tick_count_() {
    return tick_count;
}

static void copy_str_(char* dst, const char* src, int max_len) {
    int i = 0;
    while (src[i] && i < max_len - 1) {
        dst[i] = src[i];
        ++i;
    }
    dst[i] = '\0';
}

// ============================================================
// 初始�?
// ============================================================

bool ScanEngine::init(uint32_t controller_task_id, int worker_count, struct netif* netif) {
    if (initialized_) return true;

    controller_task_id_ = controller_task_id;
    worker_count_ = (worker_count < 1) ? 1
        : ((worker_count > max_workers_) ? max_workers_ : worker_count);

    static TcpPortScanHandler tcp_handler;
    register_handler(ScanJobType::TcpPortScan, &tcp_handler);
    static UdpPortScanHandler udp_handler;
    register_handler(ScanJobType::UdpPortScan, &udp_handler);
    static AckPortScanHandler ack_handler;
    register_handler(ScanJobType::AckPortScan, &ack_handler);
    static ArpDiscoveryHandler arp_handler(netif);
    register_handler(ScanJobType::ArpDiscovery, &arp_handler);
    static IcmpPingHandler icmp_handler(netif);
    register_handler(ScanJobType::IcmpPing, &icmp_handler);
    static ServiceDetectHandler svc_handler;
    register_handler(ScanJobType::ServiceDetect, &svc_handler);
    static VulnProbeHandler vuln_handler(this);
    register_handler(ScanJobType::VulnProbe, &vuln_handler);


    // 创建 Worker 任务
    for (int i = 0; i < worker_count_; ++i) {
        auto* ctx = worker_pool_.allocate();
        if (!ctx) return false;

        ctx->worker_id = static_cast<uint32_t>(i);
        ctx->controller_task_id = controller_task_id_;
        ctx->running = true;

        workers_[i] = ctx;

        bool created = create_worker_task_(i, ctx);
        if (!created) {
            worker_pool_.deallocate(ctx);
            workers_[i] = nullptr;
            return false;
        }
    }

    // 挂载 ProcFS 节点
    scan_result_node_.set_engine(this);
    VfsManager::instance().mount("/proc/scan_results", &scan_result_node_);

    initialized_ = true;
    return true;
}

// ============================================================
// Worker 任务管理
// ============================================================

bool ScanEngine::create_worker_task_(int index, WorkerContext* ctx) {
    uint32_t* stack = worker_stacks_[index];
    TaskControlBlock* tcb = Scheduler::instance().create_task(
        ScanEngine::worker_entry_,
        stack,
        worker_stack_size_,
        TaskPriority::Low,       // Low 优先级，不阻塞系统交�?
        0,                       // size_pow2 (auto)
        TaskPrivilege::Kernel
    );

    if (!tcb) return false;

    // 保存 Worker TID，供主控通过 TaskNotify 通信
    ctx->worker_id = tcb->scheduler.id;

    return true;
}

// Worker 入口函数 -- 通过 TaskNotify 阻塞等待作业
void ScanEngine::worker_entry_() {
    ScanEngine& engine = ScanEngine::instance();

    while (true) {
        // 阻塞等待主控分发任务（TaskNotify 零开销 IPC�?
        uint32_t notify_val = TaskNotify::take(true);

        ScanJobDesc job{};
        if (!engine.dequeue_job_(job)) {
            continue; // 无作业，继续等待
        }

        // 执行作业
        engine.execute_job_(job);
    }
}

// ============================================================
// 作业队列管理
// ============================================================

bool ScanEngine::dispatch_job_(const ScanJobDesc& job, uint32_t /*timeout_ms*/) {
    LockGuard lock(job_mutex_);

    if (job_count_ >= max_pending_jobs_) return false;

    pending_jobs_[job_tail_] = job;
    job_tail_ = (job_tail_ + 1) % max_pending_jobs_;
    ++job_count_;

    // 通知下一个空�?Worker（轮询通知�?
    for (int i = 0; i < worker_count_; ++i) {
        if (workers_[i] && workers_[i]->running) {
            TaskNotify::give(workers_[i]->worker_id, job.job_id, false);
            break;
        }
    }

    return true;
}

bool ScanEngine::dequeue_job_(ScanJobDesc& out) {
    LockGuard lock(job_mutex_);

    if (job_count_ == 0) return false;

    out = pending_jobs_[job_head_];
    job_head_ = (job_head_ + 1) % max_pending_jobs_;
    --job_count_;

    return true;
}

// ============================================================
// 作业执行 -- 核心分发逻辑
// ============================================================

void ScanEngine::register_handler(ScanJobType type, IScanHandler* handler) {
    handlers_[static_cast<uint8_t>(type)] = handler;
}

void ScanEngine::execute_job_(const ScanJobDesc& job) {
    UnifiedScanResult ur{};
    ur.ip = job.ip;
    ur.port = job.port;
    ur.scan_type = static_cast<uint8_t>(job.job_type);
    ur.timestamp = get_tick_count_();

    uint8_t type_idx = static_cast<uint8_t>(job.job_type);
    if (type_idx < 8 && handlers_[type_idx]) {
        handlers_[type_idx]->execute(job, ur);
    }

    append_result_(ur);
    TaskNotify::give(controller_task_id_, ur.timestamp, false);
}

// ============================================================
// 结果缓冲区管�?
// ============================================================

void ScanEngine::append_result_(const UnifiedScanResult& result) {
    LockGuard lock(result_mutex_);
    result_buffer_[result_count_ % max_results_] = result;
    ++result_count_;
}

void ScanEngine::find_service_for_target(uint32_t ip, uint16_t port,
                                            char* out_buf, int max_len) {
    LockGuard lock(result_mutex_);
    for (int i = result_count_ - 1; i >= 0; --i) {
        const UnifiedScanResult& r = result_buffer_[i % max_results_];
        if (r.ip == ip && r.port == port && r.service_name[0] != '\0') {
            copy_str_(out_buf, r.service_name, max_len);
            return;
        }
    }
}

// ============================================================
// 扫描 API -- 启动异步扫描任务
// ============================================================

int ScanEngine::start_tcp_port_scan(uint32_t target_ip,
                                      const uint16_t* ports, int port_count,
                                      uint32_t timeout_ms) {
    if (!initialized_) return 0;

    int scheduled = 0;
    for (int i = 0; i < port_count; ++i) {
        ScanJobDesc job{};
        job.job_type = ScanJobType::TcpPortScan;
        job.ip = target_ip;
        job.port = ports[i];
        job.job_id = next_job_id_++;

        if (dispatch_job_(job, timeout_ms)) {
            ++scheduled;
        }
    }
    return scheduled;
}

int ScanEngine::start_host_discovery(uint32_t network_prefix,
                                       uint32_t timeout_ms) {
    if (!initialized_) return 0;

    host_discovery_.set_timeout(timeout_ms);

    uint32_t base = network_prefix & 0xFFFFFF00;
    int scheduled = 0;

    for (uint16_t host = 1; host <= 254; ++host) {
        uint32_t target_ip = base | htonl(host);

        ScanJobDesc job{};
        job.job_type = ScanJobType::ArpDiscovery;
        job.ip = target_ip;
        job.port = 0;
        job.job_id = next_job_id_++;

        if (dispatch_job_(job, timeout_ms)) {
            ++scheduled;
        }
    }
    return scheduled;
}

int ScanEngine::start_service_detection(uint32_t target_ip,
                                          const uint16_t* ports, int port_count,
                                          uint32_t timeout_ms) {
    if (!initialized_) return 0;

    int scheduled = 0;
    for (int i = 0; i < port_count; ++i) {
        ScanJobDesc job{};
        job.job_type = ScanJobType::ServiceDetect;
        job.ip = target_ip;
        job.port = ports[i];
        job.job_id = next_job_id_++;

        if (dispatch_job_(job, timeout_ms)) {
            ++scheduled;
        }
    }
    return scheduled;
}

int ScanEngine::start_vuln_probe(uint32_t target_ip,
                                   const uint16_t* ports, int port_count,
                                   const char* service_name, uint32_t timeout_ms) {
    if (!initialized_) return 0;

    int scheduled = 0;
    for (int i = 0; i < port_count; ++i) {
        ScanJobDesc job{};
        job.job_type = ScanJobType::VulnProbe;
        job.ip = target_ip;
        job.port = ports[i];
        job.job_id = next_job_id_++;

        if (dispatch_job_(job, timeout_ms)) {
            ++scheduled;
        }
    }
    return scheduled;
}

int ScanEngine::start_full_scan(uint32_t network_prefix,
                                  const uint16_t* ports, int port_count,
                                  uint32_t timeout_ms) {
    if (!initialized_) return 0;

    // 阶段一：主机发现（异步�?
    int hosts = start_host_discovery(network_prefix, timeout_ms);

    // 阶段�?四：�?Lua 脚本或上层应用编�?
    // 已发现的存活主机可在结果中查询并级联调度

    return hosts;
}

// ============================================================
// 便捷方法 -- 同步快速扫�?
// ============================================================

int ScanEngine::quick_scan(uint32_t ip, const uint16_t* ports, int port_count,
                             UnifiedScanResult* out_results, int max_results) {
    int count = 0;

    PortScanner scanner;
    scanner.set_tcp_timeout(1500);

    for (int i = 0; i < port_count && count < max_results; ++i) {
        PortResult pr = scanner.tcp_connect_scan(ip, ports[i]);

        UnifiedScanResult& ur = out_results[count];
        ur.ip = ip;
        ur.port = ports[i];
        ur.port_state = static_cast<uint8_t>(pr.scheduler.state);
        ur.scan_type = static_cast<uint8_t>(ScanJobType::TcpPortScan);
        ur.latency_ms = pr.latency_ms;

        if (pr.scheduler.state == PortState::Open) {
            ServiceInfo si{};
            if (service_detector_.detect_service(ip, ports[i], si)) {
                copy_str_(ur.service_name, si.service, sizeof(ur.service_name));
                copy_str_(ur.version, si.version, sizeof(ur.version));
                copy_str_(ur.banner, si.banner, sizeof(ur.banner));
            }
        }

        ++count;
    }

    return count;
}

// ============================================================
// 结果查询
// ============================================================

int ScanEngine::get_result_count() const {
    return result_count_;
}

bool ScanEngine::get_result(int index, UnifiedScanResult& out) const {
    LockGuard lock(result_mutex_);
    if (index < 0 || index >= result_count_) return false;
    out = result_buffer_[index % max_results_];
    return true;
}

bool ScanEngine::get_latest_result(UnifiedScanResult& out) const {
    LockGuard lock(result_mutex_);
    if (result_count_ == 0) return false;
    int idx = (result_count_ - 1) % max_results_;
    out = result_buffer_[idx];
    return true;
}

void ScanEngine::clear_results() {
    LockGuard lock(result_mutex_);
    result_count_ = 0;
}

// ============================================================
// ProcFS 节点: /proc/scan_results
// ============================================================

int ScanResultNode::read(char* buf, int len, int offset, void* /*priv*/) {
    if (!engine_) return 0;

    int result_count = engine_->get_result_count();
    if (offset >= result_count) return 0;

    int pos = 0;
    auto append_str = [&](const char* s) {
        while (*s && pos < len - 1) buf[pos++] = *s++;
    };
    auto append_num = [&](uint32_t num) {
        char temp[16]; int i = 0;
        if (num == 0) { temp[i++] = '0'; }
        while (num > 0) { temp[i++] = (num % 10) + '0'; num /= 10; }
        while (i > 0 && pos < len - 1) buf[pos++] = temp[--i];
    };
    auto append_ip = [&](uint32_t ip) {
        append_num((ip >> 24) & 0xFF); append_str(".");
        append_num((ip >> 16) & 0xFF); append_str(".");
        append_num((ip >> 8) & 0xFF);  append_str(".");
        append_num(ip & 0xFF);
    };

    UnifiedScanResult result;
    int idx = offset;

    while (idx < result_count && pos < len - 200) {
        if (!engine_->get_result(idx, result)) break;

        append_ip(result.ip);
        append_str("\t");
        append_num(result.port);
        append_str("\t");

        if (result.port > 0) {
            append_str(PortScanner::port_state_to_string(
                static_cast<PortState>(result.port_state)));
        } else {
            append_str(HostDiscovery::host_state_to_string(
                static_cast<HostState>(result.host_state)));
        }
        append_str("\t");

        if (result.service_name[0]) {
            append_str(result.service_name);
            if (result.version[0]) {
                append_str(" ");
                append_str(result.version);
            }
        } else {
            append_str("-");
        }
        append_str("\t");

        if (result.cve_id[0]) {
            append_str(result.cve_id);
            append_str("(");
            append_str(VulnProbe::severity_to_string(
                static_cast<Severity>(result.severity)));
            append_str(")");
        } else {
            append_str("-");
        }
        append_str("\t");

        append_num(result.latency_ms);
        append_str("ms\n");

        ++idx;
    }

    buf[pos] = '\0';
    return pos;
}

