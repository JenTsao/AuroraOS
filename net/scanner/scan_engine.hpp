#ifndef AURORA_SCANNER_SCAN_ENGINE_HPP
#define AURORA_SCANNER_SCAN_ENGINE_HPP

#include <stdint.h>
#include <stddef.h>

#include "port_scanner.hpp"
#include "host_discovery.hpp"
#include "service_detector.hpp"
#include "vuln_probe.hpp"
#include "../../kernel/task_notify.hpp"
#include "../../kernel/task.hpp"
#include "../../kernel/memory_pool.hpp"
#include "../../kernel/mutex.hpp"
#include "../../vfs/vfs.hpp"

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
//   - Worker 通过 TaskNotify::take() 阻塞等待任务，完成后 give() 通知主控
//   - 5 级优先级调度：Worker(Low=1) 不阻塞系统交互(Normal=2)
// ============================================================

// 扫描作业类型
enum class ScanJobType : uint8_t {
    TcpPortScan    = 0,  // TCP 端口扫描
    UdpPortScan    = 1,  // UDP 端口扫描
    AckPortScan    = 2,  // ACK 端口扫描
    ArpDiscovery   = 3,  // ARP 主机发现
    IcmpPing       = 4,  // ICMP Ping
    ServiceDetect  = 5,  // 服务探测
    VulnProbe      = 6,  // 漏洞检测
};

// 扫描作业描述符（通过 TaskNotify 32位值编码）
//   [31:24] job_type
//   [23:16] reserved
//   [15:0]  job_id
struct ScanJobDesc {
    ScanJobType job_type;
    uint32_t    ip;        // 目标 IP（网络字节序）
    uint16_t    port;      // 目标端口（主机字节序）
    uint16_t    job_id;    // 作业唯一 ID
};

// 统一扫描结果
struct UnifiedScanResult {
    uint32_t     ip;                  // IP（网络字节序）
    uint16_t     port;                // 端口（主机字节序）
    uint8_t      host_state;          // 主机状态 (HostState)
    uint8_t      port_state;          // 端口状态 (PortState)
    uint8_t      scan_type;           // 扫描类型 (ScanJobType)
    char         service_name[32];    // 服务名
    char         version[64];         // 版本
    char         banner[256];         // Banner
    char         cve_id[32];          // CVE 编号
    uint8_t      severity;            // 漏洞严重程度 (Severity)
    uint32_t     latency_ms;          // 延迟
    uint32_t     timestamp;           // 时间戳
};

// Worker 任务上下文（每个 Worker 一份）
struct WorkerContext {
    uint32_t     worker_id;            // Worker 编号 (0..N-1)
    uint32_t     controller_task_id;   // 主控任务 TID
    PortScanner  port_scanner;
    HostDiscovery host_discovery;
    ServiceDetector service_detector;
    VulnProbe     vuln_probe;
    bool          running;
};

// ============================================================
// ProcFS 节点: /proc/scan_results
// ============================================================
class ScanResultNode : public ProcNode {
public:
    void set_engine(class ScanEngine* engine) { engine_ = engine; }

    int read(char* buf, int len, int offset, void* /*priv*/) override {
        if (!engine_) return 0;

        // 支持多次读取（通过 offset 分页）
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
            append_num((ip >> 8) & 0xFF); append_str(".");
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

            // 状态
            if (result.port > 0) {
                append_str(PortScanner::port_state_to_string(
                    static_cast<PortState>(result.port_state)));
            } else {
                append_str(HostDiscovery::host_state_to_string(
                    static_cast<HostState>(result.host_state)));
            }
            append_str("\t");

            // 服务
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

            // CVE
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

    // ---- 初始化 ----

    // 初始化扫描引擎
    //   controller_task_id: 主控任务 TID（用于 Worker 向主控发送通知）
    //   worker_count: Worker 任务数（1-8，默认 4）
    //   netif: 网络接口（供 HostDiscovery 使用）
    bool init(uint32_t controller_task_id, int worker_count = 4, struct netif* netif = nullptr) {
        if (initialized_) return true;

        controller_task_id_ = controller_task_id;
        worker_count_ = (worker_count < 1) ? 1 : ((worker_count > max_workers_) ? max_workers_ : worker_count);

        // 初始化 HostDiscovery（需要 netif 用于 ARP 扫描）
        if (netif) {
            host_discovery_.init(netif);
        }

        // 创建 Worker 任务
        for (int i = 0; i < worker_count_; ++i) {
            auto* ctx = worker_pool_.allocate();
            if (!ctx) return false; // Worker Pool 耗尽

            ctx->worker_id = static_cast<uint32_t>(i);
            ctx->controller_task_id = controller_task_id_;
            ctx->running = true;

            // 共享 HostDiscovery（已绑定 netif）
            // 注：PortScanner/ServiceDetector/VulnProbe 无状态，可共享

            workers_[i] = ctx;

            // 创建 Worker 任务（Low 优先级，不阻塞系统）
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

    // ---- 扫描 API ----

    // 启动 TCP 端口扫描
    //   target_ip: 目标 IP（网络字节序）
    //   ports: 端口列表（主机字节序）
    //   port_count: 端口数量
    //   timeout_ms: 单端口超时（毫秒）
    //   返回: 实际调度作业数
    int start_tcp_port_scan(uint32_t target_ip, const uint16_t* ports, int port_count,
                            uint32_t timeout_ms = 2000) {
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

    // 启动主机发现扫描
    //   network_prefix: 子网前缀（如 192.168.1.0/24 的网络字节序后3字节为前缀）
    //   返回: 调度扫描的主机数
    int start_host_discovery(uint32_t network_prefix, uint32_t timeout_ms = 1500) {
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

    // 启动服务探测
    //   target_ip: 目标 IP（网络字节序）
    //   ports: 待探测端口列表（主机字节序）
    //   port_count: 端口数量
    int start_service_detection(uint32_t target_ip, const uint16_t* ports, int port_count,
                                uint32_t timeout_ms = 3000) {
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

    // 启动漏洞检测
    //   target_ip: 目标 IP（网络字节序）
    //   ports: 待检测端口列表
    //   port_count: 端口数量
    //   service_name: 已识别的服务名（可为 nullptr）
    int start_vuln_probe(uint32_t target_ip, const uint16_t* ports, int port_count,
                         const char* service_name = nullptr, uint32_t timeout_ms = 3000) {
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

    // 一站式全面扫描：主机发现 → 端口扫描 → 服务探测 → 漏洞检测
    //   network_prefix: 子网前缀
    //   ports: 通用端口列表
    //   port_count: 端口数
    int start_full_scan(uint32_t network_prefix, const uint16_t* ports, int port_count,
                        uint32_t timeout_ms = 2000) {
        if (!initialized_) return 0;

        // 阶段一：主机发现
        int hosts = start_host_discovery(network_prefix, timeout_ms);

        // 阶段二-四：对每个存活主机执行端口/服务/漏洞扫描
        // 注：由于主机发现是异步的，这里需要等待结果后级联调度
        // 在实际使用中，Lua 脚本可编排此逻辑

        return hosts;
    }

    // ---- 结果查询 ----

    // 获取结果数量（供 ProcFS 使用）
    int get_result_count() const {
        return result_count_;
    }

    // 获取单个结果
    bool get_result(int index, UnifiedScanResult& out) const {
        LockGuard lock(result_mutex_);
        if (index < 0 || index >= result_count_) return false;
        out = result_buffer_[index % max_results_];
        return true;
    }

    // 获取最新结果
    bool get_latest_result(UnifiedScanResult& out) const {
        LockGuard lock(result_mutex_);
        if (result_count_ == 0) return false;
        int idx = (result_count_ - 1) % max_results_;
        out = result_buffer_[idx];
        return true;
    }

    // 清空结果
    void clear_results() {
        LockGuard lock(result_mutex_);
        result_count_ = 0;
    }

    // 设置 Banner 抓取超时
    void set_banner_timeout(uint32_t timeout_ms) {
        service_detector_.set_timeout(timeout_ms);
    }

    // 设置漏洞检测超时
    void set_vuln_timeout(uint32_t timeout_ms) {
        vuln_probe_.set_timeout(timeout_ms);
    }

    // ---- 便捷方法 ----

    // 对单个IP快速执行端口+服务检测（同步，阻塞当前任务）
    int quick_scan(uint32_t ip, const uint16_t* ports, int port_count,
                   UnifiedScanResult* out_results, int max_results) {
        int count = 0;

        PortScanner scanner;
        scanner.set_tcp_timeout(1500);

        for (int i = 0; i < port_count && count < max_results; ++i) {
            PortResult pr = scanner.tcp_connect_scan(ip, ports[i]);

            UnifiedScanResult& ur = out_results[count];
            ur.ip = ip;
            ur.port = ports[i];
            ur.port_state = static_cast<uint8_t>(pr.state);
            ur.scan_type = static_cast<uint8_t>(ScanJobType::TcpPortScan);
            ur.latency_ms = pr.latency_ms;

            if (pr.state == PortState::Open) {
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

    // ---- Lua 绑定 API（供 MiniProgramEngine 调用） ----

    // 注册到 Lua VM（在 MiniProgramEngine 的 aurora.scan 命名空间下）
    // 由外部调用 engine 来注册这些方法
    static void register_lua_bindings(void* lua_state);

private:
    ScanEngine() = default;

    static constexpr int max_workers_ = 8;
    static constexpr int max_results_ = 64;
    static constexpr int max_pending_jobs_ = 128;
    static constexpr int worker_stack_size_ = 1024; // 1KB per worker

    bool initialized_ = false;
    uint32_t controller_task_id_ = 0;
    int worker_count_ = 0;
    uint16_t next_job_id_ = 1;

    // Worker 管理
    WorkerContext* workers_[max_workers_]{};               // Worker 上下文指针
    MemoryPool<WorkerContext, max_workers_> worker_pool_;  // Worker 内存池
    uint32_t worker_stacks_[max_workers_][worker_stack_size_ / 4]{}; // Worker 栈

    // 结果缓冲区（环形）
    UnifiedScanResult result_buffer_[max_results_]{};
    mutable Mutex result_mutex_;
    int result_count_ = 0;

    // 待处理作业（环形队列）
    ScanJobDesc pending_jobs_[max_pending_jobs_]{};
    mutable Mutex job_mutex_;
    int job_head_ = 0;
    int job_tail_ = 0;
    int job_count_ = 0;

    // 扫描组件实例
    PortScanner      port_scanner_;
    HostDiscovery    host_discovery_;
    ServiceDetector  service_detector_;
    VulnProbe        vuln_probe_;

    // ProcFS 节点
    ScanResultNode scan_result_node_;

    // ---- 任务管理 ----

    // 创建 Worker 任务
    bool create_worker_task_(int index, WorkerContext* ctx) {
        uint32_t* stack = worker_stacks_[index];
        TaskControlBlock* tcb = Scheduler::instance().create_task(
            worker_entry_,
            stack,
            worker_stack_size_,
            TaskPriority::Low,       // Low 优先级，不阻塞系统
            0,                       // size_pow2 (auto)
            TaskPrivilege::Kernel
        );

        if (!tcb) return false;

        // 保存 worker TID，供主控通过 TaskNotify 通信
        ctx->worker_id = tcb->id;

        return true;
    }

    // Worker 入口函数（静态，需要从 TCB 反查上下文）
    static void worker_entry_() {
        // Worker 通过 TaskNotify 等待任务
        while (true) {
            uint32_t notify_val = TaskNotify::take(true); // 阻塞等待

            // 从 notify_val 解码：ip 高16位 + port 低16位
            // 实际通过全局 job 队列获取详细作业描述
            ScanEngine& engine = ScanEngine::instance();
            ScanJobDesc job{};

            if (!engine.dequeue_job_(job)) {
                continue; // 无作业，继续等待
            }

            // 执行作业
            engine.execute_job_(job);
        }
    }

    // ---- 作业队列 ----

    // 分发作业给空闲 Worker
    bool dispatch_job_(const ScanJobDesc& job, uint32_t timeout_ms) {
        LockGuard lock(job_mutex_);

        if (job_count_ >= max_pending_jobs_) return false;

        pending_jobs_[job_tail_] = job;
        job_tail_ = (job_tail_ + 1) % max_pending_jobs_;
        ++job_count_;

        // 通知下一个空闲 Worker（轮询通知所有 Worker）
        for (int i = 0; i < worker_count_; ++i) {
            if (workers_[i] && workers_[i]->running) {
                TaskNotify::give(workers_[i]->worker_id, job.job_id, false);
                break;
            }
        }

        return true;
    }

    // Worker 从队列取作业
    bool dequeue_job_(ScanJobDesc& out) {
        LockGuard lock(job_mutex_);

        if (job_count_ == 0) return false;

        out = pending_jobs_[job_head_];
        job_head_ = (job_head_ + 1) % max_pending_jobs_;
        --job_count_;

        return true;
    }

    // ---- 作业执行 ----

    // 执行单个作业
    void execute_job_(const ScanJobDesc& job) {
        UnifiedScanResult ur{};
        ur.ip = job.ip;
        ur.port = job.port;
        ur.scan_type = static_cast<uint8_t>(job.job_type);
        ur.timestamp = get_tick_count_();

        switch (job.job_type) {
            case ScanJobType::TcpPortScan: {
                PortResult pr = port_scanner_.tcp_connect_scan(job.ip, job.port);
                ur.port_state = static_cast<uint8_t>(pr.state);
                ur.latency_ms = pr.latency_ms;
                break;
            }

            case ScanJobType::UdpPortScan: {
                PortResult pr = port_scanner_.udp_scan(job.ip, job.port);
                ur.port_state = static_cast<uint8_t>(pr.state);
                ur.latency_ms = pr.latency_ms;
                break;
            }

            case ScanJobType::AckPortScan: {
                PortResult pr = port_scanner_.ack_scan(job.ip, job.port);
                ur.port_state = static_cast<uint8_t>(pr.state);
                ur.latency_ms = pr.latency_ms;
                break;
            }

            case ScanJobType::ArpDiscovery: {
                HostResult hr = host_discovery_.arp_scan(job.ip);
                ur.host_state = static_cast<uint8_t>(hr.state);
                ur.latency_ms = hr.latency_ms;
                break;
            }

            case ScanJobType::IcmpPing: {
                HostResult hr = host_discovery_.icmp_ping(job.ip);
                ur.host_state = static_cast<uint8_t>(hr.state);
                ur.latency_ms = hr.latency_ms;
                break;
            }

            case ScanJobType::ServiceDetect: {
                ServiceInfo si{};
                if (service_detector_.detect_service(job.ip, job.port, si)) {
                    copy_str_(ur.service_name, si.service, sizeof(ur.service_name));
                    copy_str_(ur.version, si.version, sizeof(ur.version));
                    copy_str_(ur.banner, si.banner, sizeof(ur.banner));
                }
                ur.port_state = static_cast<uint8_t>(PortState::Open);
                break;
            }

            case ScanJobType::VulnProbe: {
                // 在结果中查找已识别的服务名
                char svc_name[32] = {};
                find_service_for_target_(job.ip, job.port, svc_name, sizeof(svc_name));

                VulnResult vulns[4];
                int vcount = vuln_probe_.probe_vulnerabilities(
                    job.ip, job.port, svc_name, vulns, 4);

                if (vcount > 0 && vulns[0].vulnerable) {
                    copy_str_(ur.cve_id, vulns[0].cve_id, sizeof(ur.cve_id));
                    ur.severity = static_cast<uint8_t>(vulns[0].severity);
                }
                break;
            }
        }

        // 写入结果缓冲区
        append_result_(ur);

        // 通知主控任务有新的扫描结果
        TaskNotify::give(controller_task_id_, ur.timestamp, false);
    }

    // 在已有结果中查找服务名
    void find_service_for_target_(uint32_t ip, uint16_t port, char* out_buf, int max_len) {
        LockGuard lock(result_mutex_);
        for (int i = result_count_ - 1; i >= 0; --i) {
            const UnifiedScanResult& r = result_buffer_[i % max_results_];
            if (r.ip == ip && r.port == port && r.service_name[0] != '\0') {
                copy_str_(out_buf, r.service_name, max_len);
                return;
            }
        }
    }

    // 追加结果到环形缓冲区
    void append_result_(const UnifiedScanResult& result) {
        LockGuard lock(result_mutex_);
        result_buffer_[result_count_ % max_results_] = result;
        ++result_count_;
    }

    // 安全字符串复制
    static void copy_str_(char* dst, const char* src, int max_len) {
        int i = 0;
        while (src[i] && i < max_len - 1) {
            dst[i] = src[i];
            ++i;
        }
        dst[i] = '\0';
    }

    static uint32_t get_tick_count_() {
        extern volatile uint32_t tick_count;
        return tick_count;
    }
};

#endif // AURORA_SCANNER_SCAN_ENGINE_HPP
