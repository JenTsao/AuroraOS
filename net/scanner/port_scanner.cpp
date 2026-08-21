// ============================================================
// port_scanner.cpp -- TCP Connect / UDP / ACK 端口扫描引擎实现
//
// 将 header-only 实现中的重量级方法提取为 .cpp，减少内联代码膨胀。
// ============================================================

#include "port_scanner.hpp"
#include <stdint.h>

extern "C" {
#include "net_client.hpp"
#include "lwip/inet.h"
}

// ---- 系统全局符号 ----
extern volatile uint32_t tick_count;
extern void sys_yield();

// ============================================================
// 工具函数
// ============================================================

uint32_t PortScanner::get_tick_count_() {
    return tick_count;
}

void PortScanner::yield_cpu_() {
    sys_yield();
}

// ============================================================
// TCP Connect 扫描 -- 单端口
// ============================================================

PortResult PortScanner::tcp_connect_scan(uint32_t ip, uint16_t port) {
    PortResult result{};
    result.ip = ip;
    result.port = port;
    result.scan_type = ScanType::TcpConnect;
    result.state = PortState::Filtered;

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = ip;

    int sock = auroraos::net::net_socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        result.state = PortState::Error;
        return result;
    }

    int flags = auroraos::net::net_fcntl(sock, F_GETFL, 0);
    auroraos::net::net_fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    uint32_t start_tick = get_tick_count_();
    err_t conn_err = auroraos::net::net_connect(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));

    if (conn_err == ERR_OK) {
        result.state = PortState::Open;
        result.latency_ms = get_tick_count_() - start_tick;
    } else if (conn_err == ERR_INPROGRESS || conn_err == ERR_WOULDBLOCK) {
        result = wait_tcp_connect_(sock, ip, port, start_tick);
    } else {
        result.state = PortState::Closed;
        result.latency_ms = get_tick_count_() - start_tick;
    }

    auroraos::net::net_close(sock);
    return result;
}

// ============================================================
// TCP Connect 扫描 -- 端口范围
// ============================================================

int PortScanner::tcp_connect_scan_range(uint32_t ip, uint16_t port_start, uint16_t port_end, PortResult* out_results,
                                        int max_results) {
    int count = 0;
    for (uint16_t port = port_start; port <= port_end && count < max_results; ++port) {
        out_results[count] = tcp_connect_scan(ip, port);
        ++count;
        yield_cpu_();
    }
    return count;
}

// ============================================================
// TCP Connect 扫描 -- 指定端口列表
// ============================================================

int PortScanner::tcp_connect_scan_ports(uint32_t ip, const uint16_t* ports, int port_count, PortResult* out_results,
                                        int max_results, bool (*callback)(const PortResult&)) {
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

// ============================================================
// UDP 扫描
// ============================================================

PortResult PortScanner::udp_scan(uint32_t ip, uint16_t port) {
    PortResult result{};
    result.ip = ip;
    result.port = port;
    result.scan_type = ScanType::Udp;
    result.state = PortState::Filtered;

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = ip;

    int sock = auroraos::net::net_socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        result.state = PortState::Error;
        return result;
    }

    int timeout_ms_val = static_cast<int>(udp_timeout_ms_);
    auroraos::net::net_setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout_ms_val, sizeof(timeout_ms_val));

    uint32_t start_tick = get_tick_count_();

    uint8_t probe = 0;
    err_t send_err = auroraos::net::net_sendto(sock, &probe, sizeof(probe), 0,
                                               reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
    if (send_err < 0) {
        result.state = PortState::Error;
        auroraos::net::net_close(sock);
        return result;
    }

    uint8_t recv_buf[64];
    struct sockaddr_in from{};
    socklen_t fromlen = sizeof(from);
    int recv_len = auroraos::net::net_recvfrom(sock, recv_buf, sizeof(recv_buf), 0,
                                               reinterpret_cast<struct sockaddr*>(&from), &fromlen);
    result.latency_ms = get_tick_count_() - start_tick;

    if (recv_len >= 0) {
        result.state = PortState::Open;
    } else {
        result.state = PortState::Filtered;
    }

    auroraos::net::net_close(sock);
    return result;
}

// ============================================================
// ACK 扫描
// ============================================================

PortResult PortScanner::ack_scan(uint32_t ip, uint16_t port) {
    PortResult result{};
    result.ip = ip;
    result.port = port;
    result.scan_type = ScanType::Ack;
    result.state = PortState::Filtered;

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = ip;

    int sock = auroraos::net::net_socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        result.state = PortState::Error;
        return result;
    }

    int flags = auroraos::net::net_fcntl(sock, F_GETFL, 0);
    auroraos::net::net_fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    uint32_t start_tick = get_tick_count_();
    auroraos::net::net_connect(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));

    struct timeval tv{};
    tv.tv_sec = 0;
    tv.tv_usec = static_cast<int>(ack_timeout_ms_ * 1000);

    fd_set wfds;
    FD_ZERO(&wfds);
    FD_SET(sock, &wfds);
    fd_set efds;
    FD_ZERO(&efds);
    FD_SET(sock, &efds);

    int select_ret = auroraos::net::net_select(sock + 1, nullptr, &wfds, &efds, &tv);
    result.latency_ms = get_tick_count_() - start_tick;

    if (select_ret > 0) {
        if (FD_ISSET(sock, &wfds)) {
            int error = 0;
            socklen_t len = sizeof(error);
            auroraos::net::net_getsockopt(sock, SOL_SOCKET, SO_ERROR, &error, &len);
            if (error == 0) {
                result.state = PortState::Open;
            } else if (error == ECONNREFUSED) {
                result.state = PortState::Closed;
            } else {
                result.state = PortState::Filtered;
            }
        }
        if (FD_ISSET(sock, &efds)) {
            result.state = PortState::Filtered;
        }
    } else {
        result.state = PortState::Filtered;
    }

    auroraos::net::net_close(sock);
    return result;
}

// ============================================================
// 综合扫描
// ============================================================

int PortScanner::combined_scan(uint32_t ip, const uint16_t* tcp_ports, int tcp_count, const uint16_t* udp_ports,
                               int udp_count, PortResult* out_results, int max_results) {
    int count = 0;

    for (int i = 0; i < tcp_count && count < max_results; ++i) {
        out_results[count++] = tcp_connect_scan(ip, tcp_ports[i]);
        yield_cpu_();
    }

    for (int i = 0; i < udp_count && count < max_results; ++i) {
        out_results[count++] = udp_scan(ip, udp_ports[i]);
        yield_cpu_();
    }

    return count;
}

// ============================================================
// 等待 TCP connect 完成
// ============================================================

PortResult PortScanner::wait_tcp_connect_(int sock, uint32_t ip, uint16_t port, uint32_t start_tick) {
    PortResult result{};
    result.ip = ip;
    result.port = port;
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

    int select_ret = auroraos::net::net_select(sock + 1, nullptr, &wfds, &efds, &tv);
    result.latency_ms = get_tick_count_() - start_tick;

    if (select_ret > 0) {
        if (FD_ISSET(sock, &wfds)) {
            int error = 0;
            socklen_t len = sizeof(error);
            auroraos::net::net_getsockopt(sock, SOL_SOCKET, SO_ERROR, &error, &len);

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
        result.state = PortState::Filtered;
    } else {
        result.state = PortState::Error;
    }

    return result;
}
