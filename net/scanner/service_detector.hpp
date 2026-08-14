#ifndef AURORA_SCANNER_SERVICE_DETECTOR_HPP
#define AURORA_SCANNER_SERVICE_DETECTOR_HPP

#include <stdint.h>
#include <stddef.h>

extern "C" {
#include "net_client.hpp"
#include "lwip/inet.h"
}

// ============================================================
// Service Detector -- 横幅抓取 + 服务指纹识别
//
// 工作流程：
//   1. TCP Connect 到目标端口
//   2. 发送协议特定探针
//   3. 接收响应 Banner
//   4. 与内置指纹库匹配
//
// 指纹库定义在 service_detector.cpp，避免 ODR 问题。
// ============================================================

struct ServiceInfo {
    uint32_t ip;
    uint16_t port;
    char service[32];
    char version[64];
    char banner[256];
    uint16_t banner_len;
    bool identified;
};

struct ServiceFingerprint {
    const char* service_name;
    uint16_t default_port;
    const char* probe_payload;
    const char* match_pattern;
    bool is_ssl;
};

class ServiceDetector {
public:
    // ---- 配置 (内联) ----
    void set_timeout(uint32_t timeout_ms) {
        timeout_ms_ = timeout_ms;
    }

    // ---- 横幅抓取 (.cpp 实现) ----
    bool grab_banner(uint32_t ip, uint16_t port, ServiceInfo& out_info);

    // ---- 服务指纹识别 (内联) ----
    bool identify_service(ServiceInfo& info) {
        if (info.banner_len == 0) {
            return identify_by_port_(info);
        }
        if (identify_by_banner_(info)) {
            info.identified = true;
            return true;
        }
        if (identify_by_port_(info)) {
            info.identified = true;
            return true;
        }
        return false;
    }

    // ---- 一站式检测(内联) ----
    bool detect_service(uint32_t ip, uint16_t port, ServiceInfo& out_info) {
        if (!grab_banner(ip, port, out_info)) {
            out_info.ip = ip;
            out_info.port = port;
            return identify_by_port_(out_info);
        }
        return identify_service(out_info);
    }

    // ---- 主动探测 (.cpp 实现) ----
    bool probe_http(uint32_t ip, uint16_t port, ServiceInfo& out_info);
    bool probe_service(uint32_t ip, uint16_t port, ServiceInfo& out_info);

private:
    uint32_t timeout_ms_ = 3000;

    static constexpr int fingerprint_count_ = 22;
    static const ServiceFingerprint fingerprints_[fingerprint_count_];

    // ---- 内联辅助函数 ----

    bool identify_by_port_(ServiceInfo& info) {
        for (int i = 0; i < fingerprint_count_; ++i) {
            if (fingerprints_[i].default_port == info.port) {
                copy_str_(info.service, fingerprints_[i].service_name, sizeof(info.service));
                info.identified = true;
                return true;
            }
        }
        switch (info.port) {
        case 21:
            copy_str_(info.service, "ftp", sizeof(info.service));
            break;
        case 22:
            copy_str_(info.service, "ssh", sizeof(info.service));
            break;
        case 23:
            copy_str_(info.service, "telnet", sizeof(info.service));
            break;
        case 25:
            copy_str_(info.service, "smtp", sizeof(info.service));
            break;
        case 53:
            copy_str_(info.service, "dns", sizeof(info.service));
            break;
        case 80:
            copy_str_(info.service, "http", sizeof(info.service));
            break;
        case 110:
            copy_str_(info.service, "pop3", sizeof(info.service));
            break;
        case 143:
            copy_str_(info.service, "imap", sizeof(info.service));
            break;
        case 443:
            copy_str_(info.service, "https", sizeof(info.service));
            break;
        case 445:
            copy_str_(info.service, "smb", sizeof(info.service));
            break;
        case 993:
            copy_str_(info.service, "imaps", sizeof(info.service));
            break;
        case 995:
            copy_str_(info.service, "pop3s", sizeof(info.service));
            break;
        case 1433:
            copy_str_(info.service, "mssql", sizeof(info.service));
            break;
        case 1521:
            copy_str_(info.service, "oracle", sizeof(info.service));
            break;
        case 3306:
            copy_str_(info.service, "mysql", sizeof(info.service));
            break;
        case 3389:
            copy_str_(info.service, "rdp", sizeof(info.service));
            break;
        case 5432:
            copy_str_(info.service, "postgresql", sizeof(info.service));
            break;
        case 5900:
            copy_str_(info.service, "vnc", sizeof(info.service));
            break;
        case 6379:
            copy_str_(info.service, "redis", sizeof(info.service));
            break;
        case 8080:
            copy_str_(info.service, "http-proxy", sizeof(info.service));
            break;
        case 8443:
            copy_str_(info.service, "https-alt", sizeof(info.service));
            break;
        case 27017:
            copy_str_(info.service, "mongodb", sizeof(info.service));
            break;
        default:
            return false;
        }
        info.identified = true;
        return true;
    }

    bool identify_by_banner_(ServiceInfo& info) {
        for (int i = 0; i < fingerprint_count_; ++i) {
            if (match_pattern_(info.banner, fingerprints_[i].match_pattern)) {
                copy_str_(info.service, fingerprints_[i].service_name, sizeof(info.service));
                extract_version_(info.banner, static_cast<int>(info.banner_len), info);
                return true;
            }
        }
        return false;
    }

    static bool match_pattern_(const char* banner, const char* pattern) {
        if (!banner || !pattern)
            return false;
        if (pattern[0] == '\0')
            return false;
        int blen = 0;
        while (banner[blen])
            ++blen;
        int plen = 0;
        while (pattern[plen])
            ++plen;
        if (plen > blen)
            return false;
        for (int i = 0; i <= blen - plen; ++i) {
            bool match = true;
            for (int j = 0; j < plen; ++j) {
                char bc = banner[i + j];
                if (bc >= 'A' && bc <= 'Z')
                    bc += 32;
                char pc = pattern[j];
                if (pc >= 'A' && pc <= 'Z')
                    pc += 32;
                if (bc != pc) {
                    match = false;
                    break;
                }
            }
            if (match)
                return true;
        }
        return false;
    }

    static void extract_http_server_(const char* buf, int len, ServiceInfo& info) {
        for (int i = 0; i < len - 8; ++i) {
            if ((buf[i] == 'S' || buf[i] == 's') && (buf[i + 1] == 'E' || buf[i + 1] == 'e') &&
                (buf[i + 2] == 'R' || buf[i + 2] == 'r') && (buf[i + 3] == 'V' || buf[i + 3] == 'v') &&
                (buf[i + 4] == 'E' || buf[i + 4] == 'e') && (buf[i + 5] == 'R' || buf[i + 5] == 'r') &&
                buf[i + 6] == ':') {
                i += 7;
                while (i < len && (buf[i] == ' ' || buf[i] == '\t'))
                    ++i;
                int start = i;
                while (i < len && buf[i] != '\r' && buf[i] != '\n')
                    ++i;
                int end = i;
                int copy_len = (end - start < static_cast<int>(sizeof(info.version) - 1))
                                   ? (end - start)
                                   : static_cast<int>(sizeof(info.version) - 1);
                for (int j = 0; j < copy_len; ++j)
                    info.version[j] = buf[start + j];
                info.version[copy_len] = '\0';
                int sp = 0;
                while (sp < copy_len && info.version[sp] != ' ' && info.version[sp] != '/')
                    ++sp;
                int name_len =
                    (sp < static_cast<int>(sizeof(info.service) - 1)) ? sp : static_cast<int>(sizeof(info.service) - 1);
                for (int j = 0; j < name_len; ++j)
                    info.service[j] = info.version[j];
                info.service[name_len] = '\0';
                info.identified = true;
                return;
            }
        }
    }

    static void extract_version_(const char* buf, int len, ServiceInfo& info) {
        for (int i = 0; i < len - 2; ++i) {
            if (buf[i] >= '0' && buf[i] <= '9') {
                int start = i;
                while (start > 0 && buf[start - 1] != ' ' && buf[start - 1] != '\n' && buf[start - 1] != '\r')
                    --start;
                int end = i;
                while (end < len && buf[end] != ' ' && buf[end] != '\r' && buf[end] != '\n')
                    ++end;
                int copy_len = (end - start < static_cast<int>(sizeof(info.version) - 1))
                                   ? (end - start)
                                   : static_cast<int>(sizeof(info.version) - 1);
                for (int j = 0; j < copy_len; ++j)
                    info.version[j] = buf[start + j];
                info.version[copy_len] = '\0';
                return;
            }
        }
    }

    static void copy_str_(char* dst, const char* src, int max_len) {
        int i = 0;
        while (i < max_len - 1 && src[i]) {
            dst[i] = src[i];
            ++i;
        }
        dst[i] = '\0';
    }

    static void yield_cpu_();
};

#endif // AURORA_SCANNER_SERVICE_DETECTOR_HPP
