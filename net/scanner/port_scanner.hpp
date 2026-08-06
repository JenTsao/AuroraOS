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
//   - 所有操作非阻塞，超时可配置，防止饿死协议栈
//   - 遵循 C++ Core Guidelines: RAII, enum class, constexpr
// ============================================================

enum class PortState : uint8_t {
    Open     = 0,   // 端口开放，服务响应
    Closed   = 1,   // 端口关闭，收到 RST
    Filtered = 2,   // 无响应或 ICMP Unreachable
    Error    = 3    // 扫描出错（无路由、内存不足等）
};

enum class ScanType : uint8_t {
    TcpConnect = 0,  // TCP 全连接扫描
    Udp        = 1,  // UDP 端口扫描
    Ack        = 2   // TCP ACK 扫描（防火墙规则探测）
};

// 单端口扫描结果
struct PortResult {
    uint32_t  ip;            // 目标 IP（网络字节序）
    uint16_t  port;          // 目标端口（网络字节序）
    PortState state;         // 端口状态
    ScanType  scan_type;     // 扫描类型
    uint32_t  latency_ms;    // 响应延迟（毫秒）
};

// 端口扫描器
class PortScanner {
public:
    // ---- 可配置参数 ----

    // 设置 TCP connect 超时（毫秒），默认 2000ms
    void set_tcp_timeout(uint32_t timeout_ms) {
        tcp_timeout_ms_ = timeout_ms;
        // 将毫秒转换为 lwIP 需要的秒+微秒形式
        // SO_SNDTIMEO 在 lwIP 中以毫秒为单位（取决于版本）
    }

    // 设置 UDP 等待超时（毫秒），默认 3000ms
    void set_udp_timeout(uint32_t timeout_ms) {
        udp_timeout_ms_ = timeout_ms;
    }

    // 设置最大重试次数，默认 2
    void set_max_retries(uint8_t retries) {
        max_retries_ = retries;
    }

    // ---- 扫描操作 ----

    // TCP Connect 扫描单个端口
    //   ip: 目标 IP（网络字节序）
    //   port: 目标端口（主机字节序）
    //   返回: PortResult（含端口状态和延迟）
    PortResult tcp_connect_scan(uint32_t ip, uint16_t port) {
        PortResult result{};
        result.ip = ip;
        result.port = htons(port);
        result.scan_type = ScanType::TcpConnect;
        result.state = PortState::Filtered; // 默认无响应

        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = ip;

        int sock = lwip_socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) {
            result.state = PortState::Error;
            return result;
        }

        // 设置为非阻塞
        int flags = lwip_fcntl(sock, F_GETFL, 0);
        lwip_fcntl(sock, F_SETFL, flags | O_NONBLOCK);

        // 记录开始时间（tick 计数）
        uint32_t start_tick = get_tick_count_();

        err_t conn_err = lwip_connect(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));

        if (conn_err == ERR_OK) {
            // 立即连接成功（非常少见，通常是本地环回）
            result.state = PortState::Open;
            result.latency_ms = get_tick_count_() - start_tick;
        } else if (conn_err == ERR_INPROGRESS || conn_err == ERR_WOULDBLOCK) {
            // 正在连接中，使用 select 等待完成
            result = wait_tcp_connect_(sock, ip, port, start_tick);
        } else {
            // 连接立即失败
            result.state = PortState::Closed;
            result.latency_ms = get_tick_count_() - start_tick;
        }

        lwip_close(sock);
        return result;
    }

    // TCP Connect 扫描端口范围
    //   ip: 目标 IP（网络字节序）
    //   port_start, port_end: 端口范围（主机字节序，含两端）
    //   out_results: 结果输出缓冲区
    //   max_results: 输出缓冲区容量
    //   返回: 实际扫描的端口数
    int tcp_connect_scan_range(uint32_t ip, uint16_t port_start, uint16_t port_end,
                               PortResult* out_results, int max_results) {
        int count = 0;
        for (uint16_t port = port_start; port <= port_end && count < max_results; ++port) {
            // 每扫描一个端口后短暂让出 CPU，防止饿死协议栈
            out_results[count] = tcp_connect_scan(ip, port);
            ++count;
            yield_cpu_();
        }
        return count;
    }

    // TCP Connect 扫描指定端口列表
    //   ip: 目标 IP（网络字节序）
    //   ports: 端口列表（主机字节序）
    //   port_count: 端口数量
    //   out_results: 结果输出缓冲区
    //   max_results: 输出缓冲区容量
    //   callback: 每端口完成回调（可为 nullptr），返回 true 则继续，false 中止
    //   返回: 实际扫描数
    int tcp_connect_scan_ports(uint32_t ip, const uint16_t* ports, int port_count,
                               PortResult* out_results, int max_results,
                               bool (*callback)(const PortResult&) = nullptr) {
        int count = 0;
        for (int i = 0; i < port_count && count < max_results; ++i) {
            out_results[count] = tcp_connect_scan(ip, ports[i]);
            if (callback && !callback(out_results[count])) {
                break;
            }
            ++count;
            yield_cpu_();
        }
        return count;
    }

    // UDP 扫描单个端口
    //   发送空 UDP 数据报，若收到 ICMP Port Unreachable 则端口关闭
    //   若无响应则可能开放或被过滤
    PortResult udp_scan(uint32_t ip, uint16_t port) {
        PortResult result{};
        result.ip = ip;
        result.port = htons(port);
        result.scan_type = ScanType::Udp;
        result.state = PortState::Open; // UDP 默认无响应 = 可能开放

        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = ip;

        int sock = lwip_socket(AF_INET, SOCK_DGRAM, 0);
        if (sock < 0) {
            result.state = PortState::Error;
            return result;
        }

        // 设置接收超时
        int timeout_ms = static_cast<int>(udp_timeout_ms_);
        lwip_setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout_ms, sizeof(timeout_ms));

        uint32_t start_tick = get_tick_count_();

        // 发送空 UDP 数据报
        uint8_t probe = 0;
        err_t send_err = lwip_sendto(sock, &probe, sizeof(probe), 0,
                                      reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));

        if (send_err < 0) {
            result.state = PortState::Error;
            lwip_close(sock);
            return result;
        }

        // 尝试接收 ICMP Port Unreachable 错误
        uint8_t recv_buf[64];
        struct sockaddr_in from{};
        socklen_t fromlen = sizeof(from);
        int recv_len = lwip_recvfrom(sock, recv_buf, sizeof(recv_buf), 0,
                                      reinterpret_cast<struct sockaddr*>(&from), &fromlen);

        result.latency_ms = get_tick_count_() - start_tick;

        if (recv_len < 0) {
            // 超时：ICMP 未返回 → 端口可能开放或被过滤
            result.state = PortState::Open;
        } else {
            // 收到 ICMP Port Unreachable → 端口关闭
            result.state = PortState::Closed;
        }

        lwip_close(sock);
        return result;
    }

    // ACK 扫描：发送 TCP ACK 包探测防火墙规则
    //   由于 lwIP RAW socket 未启用，ACK 扫描通过 TCP connect + RST 时序判定：
    //   - 若直接收到 RST → 端口未被过滤
    //   - 若无响应或收到 ICMP Unreachable → 端口被过滤
    //   注：这不是真正的 TCP ACK 扫描，而是基于 connect 行为的过滤推断
    PortResult ack_scan(uint32_t ip, uint16_t port) {
        PortResult result{};
        result.ip = ip;
        result.port = htons(port);
        result.scan_type = ScanType::Ack;
        result.state = PortState::Filtered;

        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = ip;

        int sock = lwip_socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) {
            result.state = PortState::Error;
            return result;
        }

        int flags = lwip_fcntl(sock, F_GETFL, 0);
        lwip_fcntl(sock, F_SETFL, flags | O_NONBLOCK);

        // 极短超时：ACK 扫描不关心连接是否成功，只关心防火墙行为
        uint32_t start_tick = get_tick_count_();

        lwip_connect(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));

        // 等待短暂时间，观察响应
        struct timeval tv{};
        tv.tv_sec = 0;
        tv.tv_usec = static_cast<int>(ack_timeout_ms_ * 1000); // ms → us

        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(sock, &wfds);
        fd_set efds;
        FD_ZERO(&efds);
        FD_SET(sock, &efds);

        int select_ret = lwip_select(sock + 1, nullptr, &wfds, &efds, &tv);
        result.latency_ms = get_tick_count_() - start_tick;

        if (select_ret > 0) {
            if (FD_ISSET(sock, &wfds)) {
                // Socket 可写 → 收到了 SYN-ACK（端口开放）或 RST
                int error = 0;
                socklen_t len = sizeof(error);
                lwip_getsockopt(sock, SOL_SOCKET, SO_ERROR, &error, &len);
                if (error == 0) {
                    // 连接成功 → 端口开放且未被过滤
                    result.state = PortState::Open;
                } else if (error == ECONNREFUSED) {
                    // RST → 端口关闭但未被过滤
                    result.state = PortState::Closed;
                } else {
                    result.state = PortState::Filtered;
                }
            }
            if (FD_ISSET(sock, &efds)) {
                // Socket 异常 → 被过滤
                result.state = PortState::Filtered;
            }
        } else {
            // 超时 → 被过滤
            result.state = PortState::Filtered;
        }

        lwip_close(sock);
        return result;
    }

    // ---- 便捷方法：综合端口扫描 ----
    // 同时执行 TCP Connect + UDP 扫描
    //   返回: 发现的总开放端口数
    int combined_scan(uint32_t ip, const uint16_t* tcp_ports, int tcp_count,
                      const uint16_t* udp_ports, int udp_count,
                      PortResult* out_results, int max_results) {
        int count = 0;

        // TCP 扫描
        for (int i = 0; i < tcp_count && count < max_results; ++i) {
            out_results[count++] = tcp_connect_scan(ip, tcp_ports[i]);
            yield_cpu_();
        }

        // UDP 扫描
        for (int i = 0; i < udp_count && count < max_results; ++i) {
            out_results[count++] = udp_scan(ip, udp_ports[i]);
            yield_cpu_();
        }

        return count;
    }

    // ---- 状态查询 ----

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

    // 等待 TCP connect 完成（使用 select）
    PortResult wait_tcp_connect_(int sock, uint32_t ip, uint16_t port, uint32_t start_tick) {
        PortResult result{};
        result.ip = ip;
        result.port = htons(port);
        result.scan_type = ScanType::TcpConnect;

        struct timeval tv{};
        tv.tv_sec = tcp_timeout_ms_ / 1000;
        tv.tv_usec = static_cast<int>((tcp_timeout_ms_ % 1000) * 1000);

        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(sock, &wfds);
        fd_set efds;
        FD_ZERO(&efds);
        FD_SET(sock, &efds);

        int select_ret = lwip_select(sock + 1, nullptr, &wfds, &efds, &tv);
        result.latency_ms = get_tick_count_() - start_tick;

        if (select_ret > 0) {
            if (FD_ISSET(sock, &wfds)) {
                // Socket 可写 → 检查是否有错误
                int error = 0;
                socklen_t len = sizeof(error);
                lwip_getsockopt(sock, SOL_SOCKET, SO_ERROR, &error, &len);

                if (error == 0) {
                    result.state = PortState::Open;
                } else if (error == ECONNREFUSED || error == ECONNABORTED) {
                    result.state = PortState::Closed;
                } else if (error == ETIMEDOUT || error == EHOSTUNREACH) {
                    result.state = PortState::Filtered;
                } else {
                    result.state = PortState::Error;
                }
            }
            if (FD_ISSET(sock, &efds)) {
                result.state = PortState::Filtered;
            }
        } else if (select_ret == 0) {
            // 超时
            result.state = PortState::Filtered;
        } else {
            // select 错误
            result.state = PortState::Error;
        }

        return result;
    }

    // 获取系统 tick（供延迟计算用）
    static uint32_t get_tick_count_() {
        extern volatile uint32_t tick_count;
        return tick_count;
    }

    // 短暂让出 CPU，防止扫描阻塞系统
    static void yield_cpu_() {
        extern void sys_yield();
        sys_yield();
    }
};

#endif // AURORA_SCANNER_PORT_SCANNER_HPP
