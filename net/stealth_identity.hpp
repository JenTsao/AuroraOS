#ifndef AURORA_STEALTH_IDENTITY_HPP
#define AURORA_STEALTH_IDENTITY_HPP

#include <stdint.h>
#include <string.h>
#include "net_device.hpp"
#include "../kernel/core/arch_api.hpp"

// =====================================================================
// StealthIdentity — 局域网隐身伪装引擎
//
// 当设备连入局域网/Wi-Fi 后，通过以下三层伪装将其在路由器 DHCP 列表、
// MAC 厂商分析工具和网络行为分析仪中完全隐形：
//
//   Layer 1: MAC 地址 OUI 厂商欺骗  (eth_driver / netif_add 前)
//   Layer 2: DHCP 主机名伪装          (dhcp_start 前)
//   Layer 3: DHCP Option 55 指纹模拟  (lwIP dhcp_option 编译期覆盖)
//
// 用法:
//   StealthIdentity::setup(StealthIdentity::Preset::APPLE_IPAD);
//   然后正常启动 netif_add / dhcp_start
// =====================================================================

class StealthIdentity {
public:
    // ── 预设伪装身份 ──────────────────────────────────────────────
    enum class Preset : uint8_t {
        NONE = 0, // 不启用伪装，使用 Kconfig/board 默认值

        // Apple 系列
        APPLE_IPAD,    // iPad-of-Staff — 看起来像员工的 iPad
        APPLE_IPHONE,  // iPhone — 通用 iPhone 伪装
        APPLE_MACBOOK, // MacBook-Pro — 办公 Mac

        // 办公设备系列
        HP_LASERJET,  // HP-LaserJet-M402dn — 无害打印机
        HP_OFFICEJET, // HP-OfficeJet-Pro-9010 — 一体机

        // 其他伪装
        SAMSUNG_GALAXY, // Galaxy-S24 — Android 伪装
    };

    // ── MAC OUI 前缀 ──────────────────────────────────────────────
    struct MacOui {
        uint8_t prefix[3];  // OUI 前 3 字节 (厂商识别码)
        const char* vendor; // 厂商名 (仅用于调试)
    };

    static constexpr MacOui kOuiApple = {{0x10, 0xDD, 0xB1}, "Apple Inc."};
    static constexpr MacOui kOuiHp = {{0x00, 0x26, 0x55}, "Hewlett Packard"};
    static constexpr MacOui kOuiSamsung = {{0x8C, 0xF5, 0xA3}, "Samsung Electronics"};

    // ── DHCP 主机名 ───────────────────────────────────────────────
    struct PresetConfig {
        MacOui oui;
        const char* hostname;     // DHCP Option 12 主机名
        uint8_t dhcp_fingerprint; // Option 55 指纹 ID (0=默认, 1=iOS, 2=Win10, 3=Printer)
    };

    // ── DHCP Option 55 指纹定义 ───────────────────────────────────
    //
    // 不同的操作系统在 DHCP 请求时携带的参数请求列表 (Option 55)
    // 的**顺序和条目**各不相同，这是深度包检测 (DPI) 判定设备类型
    // 的关键指纹。以下数组按标准参数顺序提供，会直接注入 lwIP。
    //
    enum class DhcpFingerprint : uint8_t {
        Default = 0,   // lwIP 默认 (Subnet/Router/Broadcast/DNS)
        iOS15 = 1,     // iOS 15.x 特征参数列表
        Windows10 = 2, // Windows 10/11 DHCP 指纹
        HpPrinter = 3, // HP 打印机固件 DHCP 指纹
    };

    // iOS 15.x DHCP 参数请求列表 (Option 55)
    // 特征：Subnet, Router, DNS, DomainName, NTP, StaticRoute, ClasslessStaticRoute,
    //       NetBIOS_NameServer, NetBIOS_NodeType, ProxyAutoDiscovery
    static constexpr uint8_t kFingerprint_iOS[] = {
        1,   // Subnet Mask
        3,   // Router
        6,   // DNS Server
        15,  // Domain Name
        42,  // NTP Server
        33,  // Static Route
        121, // Classless Static Route
        44,  // NetBIOS Name Server
        46,  // NetBIOS Node Type
        252, // WPAD (Proxy Auto-Discovery)
    };

    // Windows 10/11 DHCP 参数请求列表
    // 特征：Subnet, Router, DNS, DomainName, NetBIOS_NameServer,
    //       NetBIOS_NodeType, NetBIOS_Scope, RouterDiscovery, StaticRoute
    static constexpr uint8_t kFingerprint_Win10[] = {
        1,   // Subnet Mask
        3,   // Router
        6,   // DNS Server
        15,  // Domain Name
        44,  // NetBIOS Name Server
        46,  // NetBIOS Node Type
        47,  // NetBIOS Scope
        31,  // Router Discovery
        33,  // Static Route
        121, // Classless Static Route
        249, // Classless Static Route (Microsoft)
        43,  // Vendor Specific Info
    };

    // HP 打印机固件 DHCP 参数请求列表
    // 特征：极简，仅请求网络基本配置 + 打印机特定选项
    static constexpr uint8_t kFingerprint_HPPrinter[] = {
        1,  // Subnet Mask
        3,  // Router
        6,  // DNS Server
        15, // Domain Name
        44, // NetBIOS Name Server
        46, // NetBIOS Node Type
    };

    // ── 预设表 ────────────────────────────────────────────────────
    static PresetConfig get_preset(Preset preset) {
        switch (preset) {
        case Preset::APPLE_IPAD:
            return {kOuiApple, "iPad-of-Staff", 1};
        case Preset::APPLE_IPHONE:
            return {kOuiApple, "iPhone-15", 1};
        case Preset::APPLE_MACBOOK:
            return {kOuiApple, "MacBook-Pro", 1};
        case Preset::HP_LASERJET:
            return {kOuiHp, "HP-LaserJet-M402dn", 3};
        case Preset::HP_OFFICEJET:
            return {kOuiHp, "HP-OfficeJet-Pro9010", 3};
        case Preset::SAMSUNG_GALAXY:
            return {kOuiSamsung, "Galaxy-S24", 0};
        case Preset::NONE:
        default:
            return {kOuiApple, nullptr, 0}; // hostname=null 表示使用默认
        }
    }

    // ── 核心 API ──────────────────────────────────────────────────

    // 应用全部三层伪装到指定的 NetDevice 上。
    // 必须在 netif_add 之前、device->init() 之前调用。
    //
    // device:  目标网卡 (StellarisEth::instance())
    // preset:  伪装身份预设
    // out_mac: 若 non-null，将生成的伪装 MAC (6 字节) 复制到此
    static void apply(NetDevice& device, Preset preset, uint8_t* out_mac = nullptr) {
        PresetConfig cfg = get_preset(preset);
        apply_mac_oui(device, cfg.oui, out_mac);
        // hostname 和 dhcp_fingerprint 在 net_app.cpp 的 DHCP 初始化阶段读取
    }

    // 仅应用 MAC OUI 伪装（后 3 字节由 DWT 硬件时钟随机生成）
    static void apply_mac_oui(NetDevice& device, const MacOui& oui, uint8_t* out_mac = nullptr) {
        uint8_t spoofed[6];
        spoofed[0] = oui.prefix[0];
        spoofed[1] = oui.prefix[1];
        spoofed[2] = oui.prefix[2];

        // 后 3 字节：基于 DWT 周期计数器的高熵随机
        // 多次采样 + 位旋转确保 MAC 地址足够分散
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
        case DhcpFingerprint::Default:
        default:
            out_len = 0;
            return nullptr; // nullptr = 使用 lwIP 默认
        }
    }

private:
    StealthIdentity() = default;
    Preset active_preset_ = Preset::NONE;
    PresetConfig active_config_ = get_preset(Preset::NONE);
};

#endif // AURORA_STEALTH_IDENTITY_HPP
