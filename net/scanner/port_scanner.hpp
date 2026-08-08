#ifndef AURORA_SCANNER_PORT_SCANNER_HPP
#define AURORA_SCANNER_PORT_SCANNER_HPP

#include <stdint.h>
#include <stddef.h>

extern "C" {
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/inet.h"
}

// ============================================================
// Port Scanner -- TCP Connect / UDP / ACK 扫描引擎
//
// 设计约束：
//   - lwIP 原生 socket API（无需 LWIP_RAW）
//   - TCP Connect Scan: socket()→connect()→close()
//   - UDP Scan: sendto() + ICMP Unreachable 判定
//   - ACK Scan: TCP connect + TTL/RST 组合判定
//   - 所有操作非阻塞，超时可配置
//   - .cpp 实现避免嵌入式内联代码膨胀
// ============================================================

enum class PortState : uint8_t {
    Open     = 0,
    Closed   = 1,
    Filtered = 2,
    Error    = 3
};

enum class ScanType : uint8_t {
    TcpConnect = 0,
    Udp        = 1,
    Ack        = 2
};

struct PortResult {
    uint32_t  ip;
    uint16_t  port;
    PortState state;
    ScanType  scan_type;
    uint32_t  latency_ms;
};

class PortScanner {
public:
    // ---- 可配置参数 (内联) ----

    void set_tcp_timeout(uint32_t timeout_ms) { tcp_timeout_ms_ = timeout_ms; }
    void set_udp_timeout(uint32_t timeout_ms) { udp_timeout_ms_ = timeout_ms; }
    void set_max_retries(uint8_t retries)     { max_retries_ = retries; }

    // ---- 扫描操作 (.cpp 实现) ----

    PortResult tcp_connect_scan(uint32_t ip, uint16_t port);
    int tcp_connect_scan_range(uint32_t ip, uint16_t port_start, uint16_t port_end,
                               PortResult* out_results, int max_results);
    int tcp_connect_scan_ports(uint32_t ip, const uint16_t* ports, int port_count,
                               PortResult* out_results, int max_results,
                               bool (*callback)(const PortResult&) = nullptr);
    PortResult udp_scan(uint32_t ip, uint16_t port);
    PortResult ack_scan(uint32_t ip, uint16_t port);
    int combined_scan(uint32_t ip, const uint16_t* tcp_ports, int tcp_count,
                      const uint16_t* udp_ports, int udp_count,
                      PortResult* out_results, int max_results);

    // ---- 状态查询 (内联) ----

    static const char* port_state_to_string(PortState state) {
        switch (state) {
            case PortState::Open:     return "open";
            case PortState::Closed:   return "closed";
            case PortState::Filtered: return "filtered";
            case PortState::Error:    return "error";
            default:                  return "unknown";
        }
    }

    static const char* scan_type_to_string(ScanType stype) {
        switch (stype) {
            case ScanType::TcpConnect: return "tcp_connect";
            case ScanType::Udp:        return "udp";
            case ScanType::Ack:        return "ack";
            default:                   return "unknown";
        }
    }

private:
    uint32_t tcp_timeout_ms_ = 2000;
    uint32_t udp_timeout_ms_ = 3000;
    uint32_t ack_timeout_ms_ = 800;
    uint8_t  max_retries_ = 2;

    PortResult wait_tcp_connect_(int sock, uint32_t ip, uint16_t port,
                                  uint32_t start_tick);
    static uint32_t get_tick_count_();
    static void yield_cpu_();
};

#endif // AURORA_SCANNER_PORT_SCANNER_HPP
