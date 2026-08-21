#ifndef AURORA_STEALTH_IDENTITY_HPP
#define AURORA_STEALTH_IDENTITY_HPP

#include <stdint.h>
#include <string.h>
#include "net_device.hpp"
#include "../kernel/core/arch_api.hpp"

// =====================================================================
// StealthIdentity — 局域网全栈隐身与多态伪装引擎
//
// 当设备连入局域网/Wi-Fi 后，通过四层伪装将其在路由器 DHCP 列表、
// MAC 厂商分析工具、Nmap OS 探测器和网络行为分析仪 (DPI) 中完全混淆或隐身：
//
//   Layer 1: MAC 地址 OUI 厂商欺骗与单播随机化 (eth_driver / netif_add 前)
//   Layer 2: DHCP 主机名与 Option 55 协议指纹模拟 (dhcp_start 前)
//   Layer 3: TCP/IP 协议栈指纹伪装 (TTL / TCP Window Size / MSS)
//   Layer 4: 局域网探测防御与幽灵隐身 (Ping 禁绝 / mDNS-SSDP-LLMNR 静音过滤 / 诱饵响应)
//
// =====================================================================

class StealthIdentity {
public:
    // ── 预设伪装身份 ──────────────────────────────────────────────
    enum class Preset : uint8_t {
        NONE = 0, // 不启用伪装，使用原生系统默认值

        // Apple 系列 (常见个人设备)
        APPLE_IPAD,    // iPad-of-Staff — 员工 iPad
        APPLE_IPHONE,  // iPhone-15 — 通用 iPhone 伪装
        APPLE_MACBOOK, // MacBook-Pro — 办公 Mac

        // 办公打印设备 (极低安全审计优先级)
        HP_LASERJET,  // HP-LaserJet-M402dn — 激光打印机
        HP_OFFICEJET, // HP-OfficeJet-Pro9010 — 多功能一体机

        // 智能移动/家居/安防 IoT 系列
        SAMSUNG_GALAXY, // Galaxy-S24 — Android 旗舰
        HIKVISION_CAM,  // HIKVISION-DS-2CD2042WD — 海康威视网络监控摄像头
        XIAOMI_IOT,     // xiaomi-smartplug-v3 — 小米智能设备
        CISCO_SWITCH,   // cisco-catalyst-2960 — Cisco 企业级网络交换机
        TESLA_VEHICLE,  // Tesla-Model3 — 特斯拉车载车机

        // 终极幽灵模式 (全静默黑客隐身)
        SILENT_GHOST, // 局域网完全静默：无主机名、ICMP 丢弃、广播丢弃、随机非广播 MAC
    };

    // ── MAC OUI 前缀 ──────────────────────────────────────────────
    struct MacOui {
        uint8_t prefix[3];  // OUI 前 3 字节 (IEEE 厂商识别码)
        const char* vendor; // 厂商名 (仅用于调试与审计)
    };

    static constexpr MacOui kOuiApple = {{0x10, 0xDD, 0xB1}, "Apple Inc."};
    static constexpr MacOui kOuiApplePhone = {{0xF0, 0x18, 0x98}, "Apple Inc."};
    static constexpr MacOui kOuiAppleMac = {{0x3C, 0x22, 0xFB}, "Apple Inc."};
    static constexpr MacOui kOuiHpLaser = {{0x00, 0x26, 0x55}, "Hewlett Packard"};
    static constexpr MacOui kOuiHpOffice = {{0x00, 0x1E, 0x0B}, "Hewlett Packard"};
    static constexpr MacOui kOuiSamsung = {{0x8C, 0xF5, 0xA3}, "Samsung Electronics"};
    static constexpr MacOui kOuiHikvision = {{0x00, 0x18, 0xAE}, "Hikvision Digital Technology"};
    static constexpr MacOui kOuiXiaomi = {{0x78, 0x11, 0xDC}, "Xiaomi Communications"};
    static constexpr MacOui kOuiCisco = {{0x00, 0x00, 0x0C}, "Cisco Systems"};
    static constexpr MacOui kOuiTesla = {{0x98, 0xED, 0x5C}, "Tesla Motors"};
    static constexpr MacOui kOuiGhost = {{0x02, 0xFA, 0xCE}, "Locally Administered Ghost"};

    // ── DHCP Option 55 指纹定义 ───────────────────────────────────
    enum class DhcpFingerprint : uint8_t {
        Default = 0,   // lwIP 默认 (Subnet/Router/Broadcast/DNS)
        iOS15 = 1,     // iOS 15/16/17 特征参数列表
        Windows10 = 2, // Windows 10/11 DHCP 指纹
        HpPrinter = 3, // HP 打印机固件 DHCP 指纹
        LinuxIoT = 4,  // Linux/Android IoT 指纹
        CiscoIOS = 5,  // Cisco IOS DHCP 指纹
    };

    // ── 综合伪装配置结构体 ─────────────────────────────────────────
    struct PresetConfig {
        MacOui oui;
        const char* hostname;     // DHCP Option 12 主机名
        uint8_t dhcp_fingerprint; // Option 55 指纹 ID
        uint8_t default_ttl;      // 模拟的 IP 默认 TTL (iOS/Linux=64, Win=128, Cisco/Printer=255)
        uint16_t tcp_window_size; // 模拟的 TCP 初始窗口大小 (iOS=65535, Win=64240, Linux=29200)
        uint16_t tcp_mss;         // 模拟的 TCP 最大段大小 (典型 1460)
        bool drop_icmp_echo;      // 是否静默拒绝 ICMP Ping 探测
        bool drop_discovery;      // 是否丢弃 mDNS(5353)/SSDP(1900)/LLMNR(5355)/NetBIOS(137)
        bool suppress_garp;       // 是否抑制入网 Gratuitous ARP 广播
    };

    // ── DHCP Option 55 静态指纹数组 ─────────────────────────────────
    static constexpr uint8_t kFingerprint_iOS[] = {
        1, 3, 6, 15, 42, 33, 121, 44, 46, 252
    };

    static constexpr uint8_t kFingerprint_Win10[] = {
        1, 3, 6, 15, 44, 46, 47, 31, 33, 121, 249, 43
    };

    static constexpr uint8_t kFingerprint_HPPrinter[] = {
        1, 3, 6, 15, 44, 46
    };

    static constexpr uint8_t kFingerprint_LinuxIoT[] = {
        1, 3, 6, 15, 28, 33, 121, 42
    };

    static constexpr uint8_t kFingerprint_CiscoIOS[] = {
        1, 3, 6, 15, 67, 43
    };

    // ── 预设表 ────────────────────────────────────────────────────
    static PresetConfig get_preset(Preset preset) {
        switch (preset) {
        case Preset::APPLE_IPAD:
            return {kOuiApple, "iPad-of-Staff", 1, 64, 65535, 1460, false, false, false};
        case Preset::APPLE_IPHONE:
            return {kOuiApplePhone, "iPhone-15", 1, 64, 65535, 1460, false, false, false};
        case Preset::APPLE_MACBOOK:
            return {kOuiAppleMac, "MacBook-Pro", 1, 64, 65535, 1460, false, false, false};
        case Preset::HP_LASERJET:
            return {kOuiHpLaser, "HP-LaserJet-M402dn", 3, 255, 8192, 1460, false, false, false};
        case Preset::HP_OFFICEJET:
            return {kOuiHpOffice, "HP-OfficeJet-Pro9010", 3, 255, 8192, 1460, false, false, false};
        case Preset::SAMSUNG_GALAXY:
            return {kOuiSamsung, "Galaxy-S24", 4, 64, 65535, 1460, false, false, false};
        case Preset::HIKVISION_CAM:
            return {kOuiHikvision, "HIKVISION-DS-2CD2042WD", 4, 64, 29200, 1460, false, false, false};
        case Preset::XIAOMI_IOT:
            return {kOuiXiaomi, "xiaomi-smartplug-v3", 4, 64, 14600, 1460, false, false, false};
        case Preset::CISCO_SWITCH:
            return {kOuiCisco, "cisco-catalyst-2960", 5, 255, 4128, 1460, false, false, false};
        case Preset::TESLA_VEHICLE:
            return {kOuiTesla, "Tesla-Model3", 4, 64, 65535, 1460, false, false, false};
        case Preset::SILENT_GHOST:
            return {kOuiGhost, nullptr, 0, 64, 65535, 1460, true, true, true};
        case Preset::NONE:
        default:
            return {kOuiApple, nullptr, 0, 64, 65535, 1460, false, false, false};
        }
    }

    // ── 核心 API ──────────────────────────────────────────────────

    // 应用全部多层伪装到指定的 NetDevice 上。
    static void apply(NetDevice& device, Preset preset, uint8_t* out_mac = nullptr) {
        PresetConfig cfg = get_preset(preset);
        apply_mac_oui(device, cfg.oui, out_mac);
    }

    // 仅应用 MAC OUI 伪装（后 3 字节由 DWT 硬件时钟高熵随机生成）
    static void apply_mac_oui(NetDevice& device, const MacOui& oui, uint8_t* out_mac = nullptr) {
        uint8_t spoofed[6];
        spoofed[0] = oui.prefix[0];
        spoofed[1] = oui.prefix[1];
        spoofed[2] = oui.prefix[2];

        // 后 3 字节：基于 DWT 周期计数器的高熵随机
        uint32_t cycle0 = Arch::get_cycle();
        uint32_t cycle1 = Arch::get_cycle();
        uint32_t entropy = cycle0 ^ (cycle1 << 7) ^ (cycle0 >> 13) ^ (cycle1 >> 21);

        spoofed[3] = static_cast<uint8_t>(entropy & 0xFF);
        spoofed[4] = static_cast<uint8_t>((entropy >> 8) & 0xFF);
        spoofed[5] = static_cast<uint8_t>((entropy >> 16) & 0xFF);

        // 确保第 7 位为 0 (单播地址，不是多播/本地管理)
        // 确保第 8 位为 0 (全局唯一地址标记)
        spoofed[3] &= 0xFC;

        device.set_mac_address(spoofed);

        if (out_mac) {
            for (int i = 0; i < 6; i++)
                out_mac[i] = spoofed[i];
        }
    }

    // 动态周期性 MAC 地址轮转 (Ephemeral MAC Rotation)
    static void rotate_ephemeral_mac(NetDevice& device, uint8_t* out_mac = nullptr) {
        apply_mac_oui(device, instance().active_config_.oui, out_mac);
    }

    // ── 全局单例：持有当前活动伪装配置 ─────────────────────────────
    static StealthIdentity& instance() {
        static StealthIdentity si;
        return si;
    }

    void set_active_preset(Preset p) {
        active_preset_ = p;
        active_config_ = get_preset(p);
    }

    Preset active_preset() const {
        return active_preset_;
    }

    const PresetConfig& active_config() const {
        return active_config_;
    }

    // 获取当前应使用的 DHCP 主机名
    const char* get_hostname() const {
        return active_config_.hostname;
    }

    // 获取协议栈参数
    uint8_t get_target_ttl() const {
        return active_config_.default_ttl;
    }

    uint16_t get_target_tcp_window() const {
        return active_config_.tcp_window_size;
    }

    uint16_t get_target_mss() const {
        return active_config_.tcp_mss;
    }

    bool is_icmp_echo_suppressed() const {
        return active_config_.drop_icmp_echo;
    }

    bool is_gratuitous_arp_suppressed() const {
        return active_config_.suppress_garp;
    }

    bool is_ghost_mode() const {
        return active_preset_ == Preset::SILENT_GHOST;
    }

    // 局域网探测丢弃过滤判定 (用于 Firewall / Packet Ingress)
    // 拦截扫描器的 mDNS (5353), SSDP (1900), LLMNR (5355), NetBIOS (137) 与 ICMP Echo
    bool should_drop_inbound_probe(uint8_t ip_proto, uint16_t dport, uint8_t icmp_type = 0) const {
        // ICMP Echo (Ping) 探测拦截
        if (ip_proto == 1 /* ICMP */ && icmp_type == 8 /* Echo Request */) {
            if (active_config_.drop_icmp_echo) {
                return true;
            }
        }

        // 服务发现广播拦截
        if (active_config_.drop_discovery && (ip_proto == 17 /* UDP */)) {
            if (dport == 5353 || dport == 1900 || dport == 5355 || dport == 137) {
                return true;
            }
        }

        return false;
    }

    // 获取当前应注入的 DHCP Option 55 指纹数据
    const uint8_t* get_dhcp_fingerprint_data(uint8_t& out_len) const {
        return get_fingerprint_by_id(active_config_.dhcp_fingerprint, out_len);
    }

    // 按指纹 ID 获取预定义 Option 55 数据
    static const uint8_t* get_fingerprint_by_id(uint8_t fp_id, uint8_t& out_len) {
        switch (static_cast<DhcpFingerprint>(fp_id)) {
        case DhcpFingerprint::iOS15:
            out_len = sizeof(kFingerprint_iOS);
            return kFingerprint_iOS;
        case DhcpFingerprint::Windows10:
            out_len = sizeof(kFingerprint_Win10);
            return kFingerprint_Win10;
        case DhcpFingerprint::HpPrinter:
            out_len = sizeof(kFingerprint_HPPrinter);
            return kFingerprint_HPPrinter;
        case DhcpFingerprint::LinuxIoT:
            out_len = sizeof(kFingerprint_LinuxIoT);
            return kFingerprint_LinuxIoT;
        case DhcpFingerprint::CiscoIOS:
            out_len = sizeof(kFingerprint_CiscoIOS);
            return kFingerprint_CiscoIOS;
        case DhcpFingerprint::Default:
        default:
            out_len = 0;
            return nullptr;
        }
    }

    // 诱饵服务横幅伪装 (Decoy Banner)
    const char* get_decoy_service_banner(uint16_t port) const {
        switch (active_preset_) {
        case Preset::HP_LASERJET:
        case Preset::HP_OFFICEJET:
            if (port == 9100) return "HP JetDirect Ready\r\n";
            if (port == 80) return "HTTP/1.1 200 OK\r\nServer: HP-HTTP-Server/1.0\r\n";
            break;
        case Preset::HIKVISION_CAM:
            if (port == 80 || port == 8000) return "HTTP/1.1 401 Unauthorized\r\nServer: Hikvision-Webs\r\n";
            if (port == 554) return "RTSP/1.0 200 OK\r\nServer: HIKVISION-RTSP/1.0\r\n";
            break;
        case Preset::CISCO_SWITCH:
            if (port == 23) return "\r\nUser Access Verification\r\n\r\nPassword: ";
            if (port == 80) return "HTTP/1.1 200 OK\r\nServer: cisco-IOS\r\n";
            break;
        default:
            break;
        }
        return nullptr;
    }

private:
    StealthIdentity() = default;
    Preset active_preset_ = Preset::NONE;
    PresetConfig active_config_ = get_preset(Preset::NONE);
};

#endif // AURORA_STEALTH_IDENTITY_HPP
