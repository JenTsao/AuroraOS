#ifndef AURORA_SCANNER_SERVICE_DETECTOR_HPP
#define AURORA_SCANNER_SERVICE_DETECTOR_HPP

#include <stdint.h>
#include <stddef.h>

extern "C" {
#include "lwip/sockets.h"
#include "lwip/inet.h"
}

// ============================================================
// Service Detector -- 横幅抓取 + 服务指纹识别
//
// 工作流程：
//   1. TCP Connect 到目标端口
//   2. 发送协议特定探针（HTTP GET、SSL ClientHello 等）
//   3. 接收响应 Banner
//   4. 与内置指纹库匹配，识别服务名和版本
//
// 内置指纹库覆盖常见服务：HTTP, SSH, FTP, SMTP, POP3, IMAP,
//   MySQL, PostgreSQL, Redis, MongoDB, SMB, RDP, VNC, Telnet
// ============================================================

// 服务信息
struct ServiceInfo {
    uint32_t  ip;            // 目标 IP（网络字节序）
    uint16_t  port;          // 端口（主机字节序）
    char      service[32];   // 服务名（如 "nginx", "openssh"）
    char      version[64];   // 版本字符串
    char      banner[256];   // 完整横幅文本
    uint16_t  banner_len;    // 横幅实际长度
    bool      identified;    // 是否成功识别
};

// 指纹匹配规则
struct ServiceFingerprint {
    const char* service_name;     // 服务标识名
    uint16_t    default_port;     // 默认端口
    const char* probe_payload;    // 探针负载（nullptr = 被动等待 banner）
    const char* match_pattern;    // 响应匹配模式（子串匹配）
    bool        is_ssl;           // 是否需要 TLS 握手
};

class ServiceDetector {
public:
    // 设置读取超时（毫秒）
    void set_timeout(uint32_t timeout_ms) {
        timeout_ms_ = timeout_ms;
    }

    // ---- 横幅抓取 ----

    // 连接到目标端口并抓取横幅
    //   ip: 目标 IP（网络字节序）
    //   port: 目标端口（主机字节序）
    //   out_info: 输出服务信息
    //   返回: true 表示抓取成功（至少收到了数据）
    bool grab_banner(uint32_t ip, uint16_t port, ServiceInfo& out_info) {
        // 初始化输出
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

        // 设置接收超时
        int rcv_timeout = static_cast<int>(timeout_ms_);
        lwip_setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &rcv_timeout, sizeof(rcv_timeout));

        // 非阻塞连接
        int flags = lwip_fcntl(sock, F_GETFL, 0);
        lwip_fcntl(sock, F_SETFL, flags | O_NONBLOCK);

        err_t conn_err = lwip_connect(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));

        if (conn_err != ERR_OK && conn_err != ERR_INPROGRESS && conn_err != ERR_WOULDBLOCK) {
            lwip_close(sock);
            return false;
        }

        // 等待连接完成
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

            // 检查连接是否成功
            int error = 0;
            socklen_t len = sizeof(error);
            lwip_getsockopt(sock, SOL_SOCKET, SO_ERROR, &error, &len);
            if (error != 0) {
                lwip_close(sock);
                return false;
            }
        }

        // 发送探针（等待一些服务主动发送 banner）
        // 有些服务（如 SSH, SMTP）连接后主动发送 banner
        // 有些服务（如 HTTP）需要先发送请求
        yield_cpu_();

        // 尝试接收 banner
        char recv_buf[512];
        int total_read = 0;

        // 先尝试被动接收（等待服务主动发送）
        struct timeval tv{};
        tv.tv_sec = 0;
        tv.tv_usec = 500000; // 500ms 初始等待

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(sock, &rfds);

        int sel = lwip_select(sock + 1, &rfds, nullptr, nullptr, &tv);
        if (sel > 0) {
            int n = lwip_recv(sock, recv_buf, sizeof(recv_buf) - 1, 0);
            if (n > 0) {
                n = (n < static_cast<int>(sizeof(out_info.banner) - 1)) ? n
                         : static_cast<int>(sizeof(out_info.banner) - 1);
                for (int i = 0; i < n; ++i) {
                    out_info.banner[i] = recv_buf[i];
                }
                out_info.banner[n] = '\0';
                out_info.banner_len = static_cast<uint16_t>(n);
                total_read = n;
            }
        }

        // 如果没收到被动 banner，发送 HTTP 探针（最常见的服务）
        if (total_read == 0) {
            const char* http_probe = "GET / HTTP/1.0\r\nHost: localhost\r\n\r\n";
            int probe_len = 0;
            while (http_probe[probe_len]) ++probe_len;

            int sent = lwip_send(sock, http_probe, probe_len, 0);
            if (sent > 0) {
                // 等待响应
                tv.tv_sec = timeout_ms_ / 1000;
                tv.tv_usec = static_cast<int>((timeout_ms_ % 1000) * 1000);

                FD_ZERO(&rfds);
                FD_SET(sock, &rfds);
                sel = lwip_select(sock + 1, &rfds, nullptr, nullptr, &tv);

                if (sel > 0) {
                    int n = lwip_recv(sock, recv_buf, sizeof(recv_buf) - 1, 0);
                    if (n > 0) {
                        n = (n < static_cast<int>(sizeof(out_info.banner) - 1)) ? n
                             : static_cast<int>(sizeof(out_info.banner) - 1);
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

    // ---- 服务指纹识别 ----

    // 识别指纹库中的服务
    //   根据端口和 banner 内容匹配
    bool identify_service(ServiceInfo& info) {
        if (info.banner_len == 0) {
            // 无 banner，仅按端口猜测
            return identify_by_port_(info);
        }

        // 优先按 banner 内容匹配
        if (identify_by_banner_(info)) {
            info.identified = true;
            return true;
        }

        // 退化为按端口猜测
        if (identify_by_port_(info)) {
            info.identified = true;
            return true;
        }

        return false;
    }

    // 一站式：连接+抓取+识别
    bool detect_service(uint32_t ip, uint16_t port, ServiceInfo& out_info) {
        if (!grab_banner(ip, port, out_info)) {
            // 即使抓取失败，也尝试按端口识别
            out_info.ip = ip;
            out_info.port = port;
            return identify_by_port_(out_info);
        }

        return identify_service(out_info);
    }

    // ---- 针对特定服务的主动探测 ----

    // HTTP 探测：发送 HEAD 请求并解析 Server 头
    bool probe_http(uint32_t ip, uint16_t port, ServiceInfo& out_info) {
        out_info.ip = ip;
        out_info.port = port;

        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = ip;

        int sock = lwip_socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) return false;

        int rcv_timeout = static_cast<int>(timeout_ms_);
        lwip_setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &rcv_timeout, sizeof(rcv_timeout));

        int flags = lwip_fcntl(sock, F_GETFL, 0);
        lwip_fcntl(sock, F_SETFL, flags | O_NONBLOCK);

        err_t conn_err = lwip_connect(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
        if (conn_err != ERR_OK && conn_err != ERR_INPROGRESS && conn_err != ERR_WOULDBLOCK) {
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
                // 提取 Server 头
                extract_http_server_(buf, n, out_info);
            }
        }

        lwip_close(sock);
        return out_info.service[0] != '\0';
    }

    // 通用主动探测：遍历指纹库发送探针
    bool probe_service(uint32_t ip, uint16_t port, ServiceInfo& out_info) {
        // 先尝试抓取被动 banner
        if (!grab_banner(ip, port, out_info)) {
            out_info.ip = ip;
            out_info.port = port;
        }

        // 对每个匹配端口的指纹发送主动探针
        for (int i = 0; i < fingerprint_count_; ++i) {
            const ServiceFingerprint& fp = fingerprints_[i];

            if (fp.probe_payload == nullptr) continue; // 无主动探针

            struct sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(port);
            addr.sin_addr.s_addr = ip;

            int sock = lwip_socket(AF_INET, SOCK_STREAM, 0);
            if (sock < 0) continue;

            int rcv_timeout = static_cast<int>(timeout_ms_);
            lwip_setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &rcv_timeout, sizeof(rcv_timeout));

            err_t conn_err = lwip_connect(sock,
                                           reinterpret_cast<struct sockaddr*>(&addr),
                                           sizeof(addr));
            if (conn_err != ERR_OK) {
                lwip_close(sock);
                continue;
            }

            // 发送探针
            int plen = 0;
            while (fp.probe_payload[plen]) ++plen;
            lwip_send(sock, fp.probe_payload, plen, 0);

            char buf[512];
            int n = lwip_recv(sock, buf, sizeof(buf) - 1, 0);
            lwip_close(sock);

            if (n > 0) {
                buf[n] = '\0';
                if (match_pattern_(buf, fp.match_pattern)) {
                    copy_str_(out_info.service, fp.service_name, sizeof(out_info.service));
                    extract_version_(buf, n, out_info);
                    out_info.identified = true;
                    return true;
                }
            }
        }

        // 无主动探针匹配，尝试被动分析
        return identify_service(out_info);
    }

private:
    uint32_t timeout_ms_ = 3000;

    // 内置指纹库
    static constexpr int fingerprint_count_ = 22;
    static const ServiceFingerprint fingerprints_[fingerprint_count_];

    // 按端口识别服务
    bool identify_by_port_(ServiceInfo& info) {
        for (int i = 0; i < fingerprint_count_; ++i) {
            if (fingerprints_[i].default_port == info.port) {
                copy_str_(info.service, fingerprints_[i].service_name, sizeof(info.service));
                info.identified = true;
                return true;
            }
        }

        // 知名端口映射
        switch (info.port) {
            case 21:   copy_str_(info.service, "ftp", sizeof(info.service)); break;
            case 22:   copy_str_(info.service, "ssh", sizeof(info.service)); break;
            case 23:   copy_str_(info.service, "telnet", sizeof(info.service)); break;
            case 25:   copy_str_(info.service, "smtp", sizeof(info.service)); break;
            case 53:   copy_str_(info.service, "dns", sizeof(info.service)); break;
            case 80:   copy_str_(info.service, "http", sizeof(info.service)); break;
            case 110:  copy_str_(info.service, "pop3", sizeof(info.service)); break;
            case 143:  copy_str_(info.service, "imap", sizeof(info.service)); break;
            case 443:  copy_str_(info.service, "https", sizeof(info.service)); break;
            case 445:  copy_str_(info.service, "smb", sizeof(info.service)); break;
            case 993:  copy_str_(info.service, "imaps", sizeof(info.service)); break;
            case 995:  copy_str_(info.service, "pop3s", sizeof(info.service)); break;
            case 1433: copy_str_(info.service, "mssql", sizeof(info.service)); break;
            case 1521: copy_str_(info.service, "oracle", sizeof(info.service)); break;
            case 3306: copy_str_(info.service, "mysql", sizeof(info.service)); break;
            case 3389: copy_str_(info.service, "rdp", sizeof(info.service)); break;
            case 5432: copy_str_(info.service, "postgresql", sizeof(info.service)); break;
            case 5900: copy_str_(info.service, "vnc", sizeof(info.service)); break;
            case 6379: copy_str_(info.service, "redis", sizeof(info.service)); break;
            case 8080: copy_str_(info.service, "http-proxy", sizeof(info.service)); break;
            case 8443: copy_str_(info.service, "https-alt", sizeof(info.service)); break;
            case 27017: copy_str_(info.service, "mongodb", sizeof(info.service)); break;
            default:   return false;
        }
        info.identified = true;
        return true;
    }

    // 按 banner 内容匹配
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

    // 子串匹配
    static bool match_pattern_(const char* banner, const char* pattern) {
        if (!banner || !pattern) return false;
        if (pattern[0] == '\0') return false;

        int blen = 0;
        while (banner[blen]) ++blen;
        int plen = 0;
        while (pattern[plen]) ++plen;

        if (plen > blen) return false;

        // 简单子串搜索（大小写不敏感）
        for (int i = 0; i <= blen - plen; ++i) {
            bool match = true;
            for (int j = 0; j < plen; ++j) {
                char bc = banner[i + j];
                char pc = pattern[j];
                // 大小写不敏感比较
                if (bc >= 'A' && bc <= 'Z') bc = static_cast<char>(bc + 32);
                if (pc >= 'A' && pc <= 'Z') pc = static_cast<char>(pc + 32);
                if (bc != pc) {
                    match = false;
                    break;
                }
            }
            if (match) return true;
        }
        return false;
    }

    // 从 HTTP 响应中提取 Server 头
    static void extract_http_server_(const char* buf, int len, ServiceInfo& info) {
        // 查找 "Server: "（大小写不敏感）
        for (int i = 0; i < len - 8; ++i) {
            if ((buf[i] == 'S' || buf[i] == 's') &&
                (buf[i+1] == 'E' || buf[i+1] == 'e') &&
                (buf[i+2] == 'R' || buf[i+2] == 'r') &&
                (buf[i+3] == 'V' || buf[i+3] == 'v') &&
                (buf[i+4] == 'E' || buf[i+4] == 'e') &&
                (buf[i+5] == 'R' || buf[i+5] == 'r') &&
                buf[i+6] == ':') {
                i += 7;
                while (i < len && (buf[i] == ' ' || buf[i] == '\t')) ++i;
                int start = i;
                while (i < len && buf[i] != '\r' && buf[i] != '\n') ++i;
                int end = i;
                int copy_len = (end - start < static_cast<int>(sizeof(info.version) - 1))
                               ? (end - start) : static_cast<int>(sizeof(info.version) - 1);
                for (int j = 0; j < copy_len; ++j) {
                    info.version[j] = buf[start + j];
                }
                info.version[copy_len] = '\0';

                // 提取服务名（Server 头的第一个 token）
                int sp = 0;
                while (sp < copy_len && info.version[sp] != ' ' && info.version[sp] != '/') ++sp;
                int name_len = (sp < static_cast<int>(sizeof(info.service) - 1))
                               ? sp : static_cast<int>(sizeof(info.service) - 1);
                for (int j = 0; j < name_len; ++j) {
                    info.service[j] = info.version[j];
                }
                info.service[name_len] = '\0';
                info.identified = true;
                return;
            }
        }
    }

    // 从 banner 提取版本号
    static void extract_version_(const char* buf, int len, ServiceInfo& info) {
        // 简单启发式：查找数字版本模式如 "1.2.3"、"v1.2" 等
        for (int i = 0; i < len - 2; ++i) {
            if (buf[i] >= '0' && buf[i] <= '9') {
                // 向前查找起始位置
                int start = i;
                while (start > 0 && buf[start-1] != ' ' && buf[start-1] != '\n' && buf[start-1] != '\r') {
                    --start;
                }
                // 向后查找结束位置
                int end = i;
                while (end < len && buf[end] != ' ' && buf[end] != '\r' && buf[end] != '\n') {
                    ++end;
                }
                int copy_len = (end - start < static_cast<int>(sizeof(info.version) - 1))
                               ? (end - start) : static_cast<int>(sizeof(info.version) - 1);
                for (int j = 0; j < copy_len; ++j) {
                    info.version[j] = buf[start + j];
                }
                info.version[copy_len] = '\0';
                return;
            }
        }
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

    static void yield_cpu_() {
        extern void sys_yield();
        sys_yield();
    }
};

// 内置服务指纹库定义
const ServiceFingerprint ServiceDetector::fingerprints_[ServiceDetector::fingerprint_count_] = {
    // 被动 Banner 服务（连接后主动发送欢迎信息）
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

    // 主动探针服务（需要先发送请求）
    {"nginx",      80,   "GET / HTTP/1.0\r\nHost: localhost\r\n\r\n",           "nginx",     false},
    {"apache",     80,   "GET / HTTP/1.0\r\nHost: localhost\r\n\r\n",           "apache",    false},
    {"iis",        80,   "GET / HTTP/1.0\r\nHost: localhost\r\n\r\n",           "microsoft", false},
    {"tomcat",    8080,  "GET / HTTP/1.0\r\nHost: localhost\r\n\r\n",           "tomcat",    false},
    {"jetty",     8080,  "GET / HTTP/1.0\r\nHost: localhost\r\n\r\n",           "jetty",     false},
    {"node.js",   8080,  "GET / HTTP/1.0\r\nHost: localhost\r\n\r\n",           "node",      false},
};

#endif // AURORA_SCANNER_SERVICE_DETECTOR_HPP
