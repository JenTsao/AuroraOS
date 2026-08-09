// ============================================================
// service_detector.cpp -- 横幅抓取 + 服务指纹识别引擎实现
//
// 包含：
//   1. 静态指纹库 fingerprints_（22 条规则）
//   2. grab_banner / probe_http / probe_service 等网络 I/O 方法
// ============================================================

#include "service_detector.hpp"
#include <stdint.h>

extern "C" {
#include "lwip/sockets.h"
#include "lwip/inet.h"
}

// ---- 系统全局符号 ----
extern void sys_yield();

// ============================================================
// 内置服务指纹库（22 条规则）
// ============================================================
const ServiceFingerprint ServiceDetector::fingerprints_[ServiceDetector::fingerprint_count_] = {
    // ---- 被动 Banner 服务 ----
    {"openssh",    22,   nullptr,  "ssh",             false},
    {"dropbear",   22,   nullptr,  "dropbear",        false},
    {"proftpd",    21,   nullptr,  "proftpd",         false},
    {"vsftpd",     21,   nullptr,  "vsftpd",          false},
    {"pure-ftpd",  21,   nullptr,  "pure-ftpd",       false},
    {"postfix",    25,   nullptr,  "postfix",         false},
    {"sendmail",   25,   nullptr,  "sendmail",        false},
    {"exim",       25,   nullptr,  "exim",            false},
    {"dovecot",   110,   nullptr,  "dovecot",         false},
    {"dovecot",   143,   nullptr,  "dovecot",         false},
    {"couri er",  110,   nullptr,  "couri er-imap",   false},
    {"mysql",     3306,  nullptr,  "mysql",           false},
    {"mariadb",   3306,  nullptr,  "mariadb",         false},
    {"postgresql",5432,  nullptr,  "postgresql",      false},
    {"redis",     6379,  nullptr,  "redis",           false},
    {"mongodb",  27017,  nullptr,  "mongodb",         false},

    // ---- 主动探针服务 ----
    {"nginx",      80,   "GET / HTTP/1.0\r\nHost: localhost\r\n\r\n",  "nginx",     false},
    {"apache",     80,   "GET / HTTP/1.0\r\nHost: localhost\r\n\r\n",  "apache",    false},
    {"iis",        80,   "GET / HTTP/1.0\r\nHost: localhost\r\n\r\n",  "microsoft", false},
    {"tomcat",    8080,  "GET / HTTP/1.0\r\nHost: localhost\r\n\r\n",  "tomcat",    false},
    {"jetty",     8080,  "GET / HTTP/1.0\r\nHost: localhost\r\n\r\n",  "jetty",     false},
    {"node.js",   8080,  "GET / HTTP/1.0\r\nHost: localhost\r\n\r\n",  "node",      false},
};

// ============================================================
// 工具函数
// ============================================================

void ServiceDetector::yield_cpu_() {
    sys_yield();
}

// ============================================================
// 横幅抓取
// ============================================================

bool ServiceDetector::grab_banner(uint32_t ip, uint16_t port,
                                   ServiceInfo& out_info) {
    out_info.ip = ip;
    out_info.port = port;
    out_info.service[0] = '\0';
    out_info.version[0] = '\0';
    out_info.banner[0] = '\0';
    out_info.banner_len = 0;
    out_info.identified = false;

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = ip;

    int sock = lwip_socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return false;

    int rcv_timeout = static_cast<int>(timeout_ms_);
    lwip_setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
                     &rcv_timeout, sizeof(rcv_timeout));

    int flags = lwip_fcntl(sock, F_GETFL, 0);
    lwip_fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    err_t conn_err = lwip_connect(sock,
                                   reinterpret_cast<struct sockaddr*>(&addr),
                                   sizeof(addr));

    if (conn_err != ERR_OK && conn_err != ERR_INPROGRESS &&
        conn_err != ERR_WOULDBLOCK) {
        lwip_close(sock);
        return false;
    }

    if (conn_err != ERR_OK) {
        struct timeval tv{};
        tv.tv_sec = timeout_ms_ / 1000;
        tv.tv_usec = static_cast<int>((timeout_ms_ % 1000) * 1000);

        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(sock, &wfds);

        int sel_ret = lwip_select(sock + 1, nullptr, &wfds, nullptr, &tv);
        if (sel_ret <= 0) {
            lwip_close(sock);
            return false;
        }

        int error = 0;
        socklen_t len = sizeof(error);
        lwip_getsockopt(sock, SOL_SOCKET, SO_ERROR, &error, &len);
        if (error != 0) {
            lwip_close(sock);
            return false;
        }
    }

    yield_cpu_();

    char recv_buf[512];
    int total_read = 0;

    // 阶段 1：被动等待 Banner（500ms）
    struct timeval tv{};
    tv.tv_sec = 0;
    tv.tv_usec = 500000;

    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(sock, &rfds);

    int sel = lwip_select(sock + 1, &rfds, nullptr, nullptr, &tv);
    if (sel > 0) {
        int n = lwip_recv(sock, recv_buf, sizeof(recv_buf) - 1, 0);
        if (n > 0) {
            n = (n < static_cast<int>(sizeof(out_info.banner) - 1))
                ? n : static_cast<int>(sizeof(out_info.banner) - 1);
            for (int i = 0; i < n; ++i) {
                out_info.banner[i] = recv_buf[i];
            }
            out_info.banner[n] = '\0';
            out_info.banner_len = static_cast<uint16_t>(n);
            total_read = n;
        }
    }

    // 阶段 2：若无被动响应，主动发送 HTTP GET 探针
    if (total_read == 0) {
        const char* http_probe = "GET / HTTP/1.0\r\nHost: localhost\r\n\r\n";
        int probe_len = 0;
        while (http_probe[probe_len]) ++probe_len;

        int sent = lwip_send(sock, http_probe, probe_len, 0);
        if (sent > 0) {
            tv.tv_sec = timeout_ms_ / 1000;
            tv.tv_usec = static_cast<int>((timeout_ms_ % 1000) * 1000);

            FD_ZERO(&rfds);
            FD_SET(sock, &rfds);
            sel = lwip_select(sock + 1, &rfds, nullptr, nullptr, &tv);

            if (sel > 0) {
                int n = lwip_recv(sock, recv_buf, sizeof(recv_buf) - 1, 0);
                if (n > 0) {
                    n = (n < static_cast<int>(sizeof(out_info.banner) - 1))
                        ? n : static_cast<int>(sizeof(out_info.banner) - 1);
                    for (int i = 0; i < n; ++i) {
                        out_info.banner[i] = recv_buf[i];
                    }
                    out_info.banner[n] = '\0';
                    out_info.banner_len = static_cast<uint16_t>(n);
                }
            }
        }
    }

    lwip_close(sock);
    return out_info.banner_len > 0;
}

// ============================================================
// HTTP 探测
// ============================================================

bool ServiceDetector::probe_http(uint32_t ip, uint16_t port,
                                  ServiceInfo& out_info) {
    out_info.ip = ip;
    out_info.port = port;

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = ip;

    int sock = lwip_socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return false;

    int rcv_timeout = static_cast<int>(timeout_ms_);
    lwip_setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
                     &rcv_timeout, sizeof(rcv_timeout));

    int flags = lwip_fcntl(sock, F_GETFL, 0);
    lwip_fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    err_t conn_err = lwip_connect(sock,
                                   reinterpret_cast<struct sockaddr*>(&addr),
                                   sizeof(addr));
    if (conn_err != ERR_OK && conn_err != ERR_INPROGRESS &&
        conn_err != ERR_WOULDBLOCK) {
        lwip_close(sock);
        return false;
    }

    if (conn_err != ERR_OK) {
        struct timeval tv{};
        tv.tv_sec = timeout_ms_ / 1000;
        tv.tv_usec = static_cast<int>((timeout_ms_ % 1000) * 1000);
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(sock, &wfds);
        if (lwip_select(sock + 1, nullptr, &wfds, nullptr, &tv) <= 0) {
            lwip_close(sock);
            return false;
        }
    }

    const char* http_head = "HEAD / HTTP/1.0\r\nHost: localhost\r\n\r\n";
    int probe_len = 0;
    while (http_head[probe_len]) ++probe_len;
    lwip_send(sock, http_head, probe_len, 0);

    char buf[512];
    struct timeval tv{};
    tv.tv_sec = timeout_ms_ / 1000;
    tv.tv_usec = static_cast<int>((timeout_ms_ % 1000) * 1000);
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(sock, &rfds);

    if (lwip_select(sock + 1, &rfds, nullptr, nullptr, &tv) > 0) {
        int n = lwip_recv(sock, buf, sizeof(buf) - 1, 0);
        if (n > 0) {
            buf[n] = '\0';
            extract_http_server_(buf, n, out_info);
        }
    }

    lwip_close(sock);
    return out_info.service[0] != '\0';
}

// ============================================================
// 通用主动探测
// ============================================================

bool ServiceDetector::probe_service(uint32_t ip, uint16_t port,
                                     ServiceInfo& out_info) {
    if (!grab_banner(ip, port, out_info)) {
        out_info.ip = ip;
        out_info.port = port;
    }

    for (int i = 0; i < fingerprint_count_; ++i) {
        const ServiceFingerprint& fp = fingerprints_[i];
        if (fp.probe_payload == nullptr) continue;

        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = ip;

        int sock = lwip_socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) continue;

        int rcv_timeout = static_cast<int>(timeout_ms_);
        lwip_setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
                         &rcv_timeout, sizeof(rcv_timeout));

        err_t conn_err = lwip_connect(sock,
                                       reinterpret_cast<struct sockaddr*>(&addr),
                                       sizeof(addr));
        if (conn_err != ERR_OK) {
            lwip_close(sock);
            continue;
        }

        int plen = 0;
        while (fp.probe_payload[plen]) ++plen;
        lwip_send(sock, fp.probe_payload, plen, 0);

        char buf[512];
        int n = lwip_recv(sock, buf, sizeof(buf) - 1, 0);
        lwip_close(sock);

        if (n > 0) {
            buf[n] = '\0';
            if (match_pattern_(buf, fp.match_pattern)) {
                copy_str_(out_info.service, fp.service_name,
                          sizeof(out_info.service));
                extract_version_(buf, n, out_info);
                out_info.identified = true;
                return true;
            }
        }
    }

    return identify_service(out_info);
}
