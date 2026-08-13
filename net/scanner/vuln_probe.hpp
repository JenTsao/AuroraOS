#ifndef AURORA_SCANNER_VULN_PROBE_HPP
#define AURORA_SCANNER_VULN_PROBE_HPP

#include <stdint.h>
#include <stddef.h>

extern "C" {
#include "net_client.hpp"
#include "lwip/inet.h"
}

// ============================================================
// Vulnerability Probe -- CVE 特征匹配探针
//
// 工作原理：
//   1. 根据已识别服务名和版本匹配 CVE 数据库
//   2. 发送 CVE 特定探针载荷
//   3. 分析响应确认漏洞
//
// CVE 签名库定义在 vuln_probe.cpp，避免 ODR 问题。
// ============================================================

enum class Severity : uint8_t {
    Critical = 0,
    High = 1,
    Medium = 2,
    Low = 3,
    Info = 4
};

struct CveSignature {
    const char* cve_id;
    const char* affected_service;
    const char* affected_version;
    const char* probe_payload;
    uint16_t probe_len;
    const char* match_pattern;
    Severity severity;
    const char* description;
    uint16_t default_port;
};

struct VulnResult {
    char cve_id[32];
    Severity severity;
    uint32_t ip;
    uint16_t port;
    bool vulnerable;
    char evidence[256];
    char description[128];
};

class VulnProbe {
public:
    // ---- 配置 (内联) ----
    void set_timeout(uint32_t timeout_ms) {
        timeout_ms_ = timeout_ms;
    }

    // ---- 单个 CVE 检测(.cpp 实现) ----
    VulnResult probe_cve(uint32_t ip, uint16_t port, const CveSignature& sig);

    // ---- 批量检测(内联，委托 probe_cve) ----
    int probe_vulnerabilities(uint32_t ip, uint16_t port, const char* service_name, VulnResult* out_results,
                              int max_results) {
        int count = 0;
        for (int i = 0; i < cve_count_; ++i) {
            if (count >= max_results)
                break;
            const CveSignature& sig = cve_signatures_[i];
            if (service_name && service_name[0] != '\0') {
                if (!match_pattern_(service_name, sig.affected_service))
                    continue;
            }
            if (sig.default_port != 0 && sig.default_port != port)
                continue;
            out_results[count] = probe_cve(ip, port, sig);
            ++count;
            yield_cpu_();
        }
        return count;
    }

    int probe_by_version(uint32_t ip, uint16_t port, const char* service_name, const char* version,
                         VulnResult* out_results, int max_results) {
        int count = 0;
        for (int i = 0; i < cve_count_; ++i) {
            if (count >= max_results)
                break;
            const CveSignature& sig = cve_signatures_[i];
            if (!service_name || !match_pattern_(service_name, sig.affected_service))
                continue;
            if (version && version[0] != '\0' && sig.affected_version[0] != '\0') {
                if (!version_in_range_(version, sig.affected_version))
                    continue;
            }
            out_results[count] = probe_cve(ip, port, sig);
            ++count;
            yield_cpu_();
        }
        return count;
    }

    // ---- 常见漏洞快速检测(.cpp 实现) ----
    VulnResult probe_heartbleed(uint32_t ip, uint16_t port);
    VulnResult probe_default_credentials(uint32_t ip, uint16_t port, const char* service_name);

    // ---- 工具方法 (内联) ----
    static const char* severity_to_string(Severity s) {
        switch (s) {
        case Severity::Critical:
            return "critical";
        case Severity::High:
            return "high";
        case Severity::Medium:
            return "medium";
        case Severity::Low:
            return "low";
        case Severity::Info:
            return "info";
        default:
            return "unknown";
        }
    }

    static constexpr int get_cve_count() {
        return cve_count_;
    }

private:
    uint32_t timeout_ms_ = 3000;

    static constexpr int cve_count_ = 12;
    static const CveSignature cve_signatures_[cve_count_];

    // ---- 内联辅助函数 ----
    static bool match_pattern_(const char* haystack, const char* needle) {
        if (!haystack || !needle)
            return false;
        if (needle[0] == '\0')
            return false;
        int hlen = 0;
        while (haystack[hlen])
            ++hlen;
        int nlen = 0;
        while (needle[nlen])
            ++nlen;
        if (nlen > hlen)
            return false;
        for (int i = 0; i <= hlen - nlen; ++i) {
            bool match = true;
            for (int j = 0; j < nlen; ++j) {
                char hc = haystack[i + j];
                if (hc >= 'A' && hc <= 'Z')
                    hc += 32;
                char nc = needle[j];
                if (nc >= 'A' && nc <= 'Z')
                    nc += 32;
                if (hc != nc) {
                    match = false;
                    break;
                }
            }
            if (match)
                return true;
        }
        return false;
    }

    static bool version_in_range_(const char* version, const char* range) {
        if (!version || !range)
            return false;
        if (range[0] == '\0')
            return true;
        int vlen = 0;
        while (version[vlen])
            ++vlen;
        int rlen = 0;
        while (range[rlen])
            ++rlen;
        for (int i = 0; i < rlen && i < vlen; ++i) {
            if (version[i] != range[i])
                return false;
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

    static void yield_cpu_();
};

#endif // AURORA_SCANNER_VULN_PROBE_HPP
