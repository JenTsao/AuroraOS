#ifndef AURORA_WIRELESS_WIFI_MONITOR_HPP
#define AURORA_WIRELESS_WIFI_MONITOR_HPP

#include <stdint.h>
#include <stddef.h>
#include "../../kernel/mutex.hpp"

// ============================================================
// WiFi Monitor Mode Driver Framework
//
// 参考 BLE HalBle 的 HAL 抽象 + NetDevice 的设备模式设计。
// 支持不同芯片组通过统一接口接入：
//   - RTL8812AU (USB 3.0, 802.11ac)
//   - RTL8187L  (USB 2.0, 802.11bg)
//
// 数据流：芯片固件 → USB Bulk Transfer → 802.11 帧 → 回调链
// MPU 沙盒隔离：WiFi 驱动任务独立 MPU 区域，通过 size_pow2 限制
// ============================================================

// Radiotap 头部（可变长，最小 8 字节）
// 参考: radiotap.org
struct __attribute__((packed)) RadiotapHeader {
    uint8_t  it_version;     // 0x00
    uint8_t  it_pad;
    uint16_t it_len;         // 总长度（含头部）
    uint32_t it_present;     // 位掩码标记后续字段

    // 常见字段偏移（当 it_present bit 置位时存在）：
    // bit 0: TSFT (8 bytes)
    // bit 1: Flags (1 byte) — bit 2=FCS, bit 4=Short GI
    // bit 2: Rate (1 byte)
    // bit 3: Channel (2 bytes freq + 2 bytes flags)
    // bit 4: Antenna Signal (1 byte, dBm)
    // bit 5: Antenna Noise (1 byte, dBm)
};

// 802.11 帧控制字段（Frame Control, 2 bytes）
struct __attribute__((packed)) FrameControl {
    uint8_t  protocol_version : 2;   // 00
    uint8_t  type             : 2;   // 00=Management, 01=Control, 10=Data
    uint8_t  subtype          : 4;
    uint8_t  to_ds            : 1;
    uint8_t  from_ds          : 1;
    uint8_t  more_frag        : 1;
    uint8_t  retry            : 1;
    uint8_t  power_mgmt       : 1;
    uint8_t  more_data        : 1;
    uint8_t  wep              : 1;   // Protected flag (802.11w)
    uint8_t  order            : 1;
};

// 802.11 帧类型
enum class FrameType : uint8_t {
    MgmtBeacon         = 0x80,  // Type=0, Subtype=8
    MgmtProbeReq       = 0x40,  // Type=0, Subtype=4
    MgmtProbeResp      = 0x50,  // Type=0, Subtype=5
    MgmtAuth           = 0xB0,  // Type=0, Subtype=11
    MgmtDeauth         = 0xC0,  // Type=0, Subtype=12
    MgmtAssocReq       = 0x00,  // Type=0, Subtype=0
    MgmtAssocResp      = 0x10,  // Type=0, Subtype=1
    MgmtDisassoc       = 0xA0,  // Type=0, Subtype=10
    MgmtAction         = 0xD0,  // Type=0, Subtype=13
    Data               = 0x08,  // Type=2
    DataNull           = 0x48,  // Type=2, Subtype=4
    DataQos            = 0x88,  // Type=2, Subtype=8
    DataQosNull        = 0xC8,  // Type=2, Subtype=12
    Unknown            = 0xFF
};

// 简化的 802.11 管理帧头部
struct __attribute__((packed)) WifiMacHeader {
    FrameControl frame_control;
    uint16_t     duration;
    uint8_t      addr1[6];  // RA (Receiver Address)
    uint8_t      addr2[6];  // TA (Transmitter Address)
    uint8_t      addr3[6];  // BSSID / SA / DA
    uint16_t     seq_ctrl;  // Fragment Number(4) + Sequence Number(12)
};

// 信道信息
struct WifiChannel {
    uint16_t frequency_mhz;  // 2412, 2437, 2462, 5180, ...
    uint8_t  channel_number; // 1-14 (2.4GHz), 36-165 (5GHz)
    uint8_t  band;           // 0=2.4GHz, 1=5GHz, 2=6GHz
};

// 捕获的完整 802.11 帧（含 radiotap 元数据）
struct CapturedFrame {
    uint32_t timestamp_sec;
    uint32_t timestamp_usec;
    int8_t   rssi_dbm;         // 信号强度
    uint8_t  noise_dbm;        // 噪声
    uint16_t channel_freq;     // 监听信道
    uint16_t frame_len;        // 802.11 帧长度
    uint8_t  data[2346];       // 最大 802.11 帧
};

// 数据速率
enum class PhyRate : uint8_t {
    Rate1M   = 2,
    Rate2M   = 4,
    Rate5_5M = 11,
    Rate11M  = 22,
    Rate6M   = 12,   // OFDM
    Rate9M   = 18,
    Rate12M  = 24,
    Rate18M  = 36,
    Rate24M  = 48,
    Rate36M  = 72,
    Rate48M  = 96,
    Rate54M  = 108,
    RateMCS0 = 0,    // HT/VHT (MCS index 0-9)
    Unknown  = 0xFF
};

// ============================================================
// WiFi Monitor HAL 抽象（参考 BLE HalBle 纯函数模式）
// ============================================================
namespace auroraos { namespace wireless { namespace WifiMonitorHal {

    // 初始化监控模式（芯片组相关）
    // 返回: true=成功进入监控模式
    bool init_monitor_mode(uint16_t vendor_id, uint16_t product_id);

    // 设置监听信道
    bool set_channel(uint16_t frequency_mhz);

    // 信道跳变（每隔 interval_ms 切换一次，遍历 channels 列表）
    bool start_channel_hopping(const uint16_t* channels, int count, uint32_t interval_ms);
    void stop_channel_hopping();

    // 从芯片接收一帧（阻塞或非阻塞，取决于实现）
    // 返回: >0 帧长度, 0 无数据, <0 错误
    int receive_frame(uint8_t* buffer, int max_len);

    // 注册帧回调（每收到一帧自动调用）
    using FrameCallback = void (*)(const CapturedFrame& frame, void* user_data);
    void register_frame_callback(FrameCallback cb, void* user_data);

    // 获取当前信道
    WifiChannel get_current_channel();

    // 获取芯片能力
    bool supports_5ghz();
    bool supports_monitor_mode();
    const char* get_chipset_name();

}}} // namespace auroraos::wireless::WifiMonitorHal

// ============================================================
// WiFi Monitor 设备驱动基类（参考 NetDevice 模式）
// ============================================================
class WifiMonitorDevice {
public:
    virtual ~WifiMonitorDevice() = default;

    // 设备生命周期
    virtual bool init() = 0;
    virtual bool deinit() = 0;

    // 监控模式控制
    virtual bool enter_monitor_mode() = 0;
    virtual bool leave_monitor_mode() = 0;
    virtual bool is_monitor_mode() const = 0;

    // 信道控制
    virtual bool set_channel(uint16_t frequency_mhz) = 0;
    virtual uint16_t get_channel() const = 0;

    // 帧捕获
    virtual int capture_frame(CapturedFrame& out_frame) = 0;

    // 注入发送（测试/渗透用途）
    virtual bool inject_frame(const uint8_t* frame, int len) = 0;

    // 能力查询
    virtual bool supports_injection() const = 0;
    virtual bool supports_5ghz() const = 0;
    virtual bool supports_6ghz() const = 0;
    virtual const char* chipset_name() const = 0;

    // 驱动状态
    enum class DriverState : uint8_t { Uninitialized, Ready, Monitor, Error };
    virtual DriverState get_state() const = 0;

protected:
    uint8_t mac_address_[6]{};
    WifiChannel current_channel_{2412, 1, 0};
};

// ============================================================
// RTL8812AU 芯片组驱动（USB 3.0, 802.11ac, 支持注入）
// ============================================================
class Rtl8812auMonitor : public WifiMonitorDevice {
public:
    static Rtl8812auMonitor& instance() {
        static Rtl8812auMonitor driver;
        return driver;
    }

    // WifiMonitorDevice 接口实现 — 声明
    bool init() override;
    bool deinit() override;
    bool enter_monitor_mode() override;
    bool leave_monitor_mode() override;
    bool is_monitor_mode() const override;
    bool set_channel(uint16_t frequency_mhz) override;
    uint16_t get_channel() const override;
    int capture_frame(CapturedFrame& out_frame) override;
    bool inject_frame(const uint8_t* frame, int len) override;
    bool supports_injection() const override { return true; }
    bool supports_5ghz() const override { return true; }
    bool supports_6ghz() const override { return false; }
    const char* chipset_name() const override { return "RTL8812AU"; }
    DriverState get_state() const override { return state_; }

    // RTL8812AU 特有方法
    bool upload_firmware(const uint8_t* fw_data, size_t fw_size);
    bool set_tx_power(uint8_t dbm);

private:
    Rtl8812auMonitor() = default;

    // USB 传输端点地址（芯片特有）
    static constexpr uint8_t EP_BULK_OUT = 0x02;
    static constexpr uint8_t EP_BULK_IN  = 0x83;

    // 芯片寄存器
    static constexpr uint16_t REG_CR            = 0x0002;  // 命令寄存器
    static constexpr uint16_t REG_RCR           = 0x0008;  // 接收配置寄存器
    static constexpr uint16_t REG_RX_PKT_LEN    = 0x0010;  // 接收包长度
    static constexpr uint16_t REG_CHANNEL       = 0x0018;  // 信道设置

    // 监控模式标志
    static constexpr uint32_t RCR_MONITOR_MODE  = 0x00000020;  // ACR 监控位
    static constexpr uint32_t RCR_RX_ALL        = 0x00000040;  // 接收所有帧

    DriverState state_ = DriverState::Uninitialized;
    bool monitor_mode_ = false;
    Mutex device_mutex_;
};

// ============================================================
// RTL8187L 芯片组驱动（USB 2.0, 802.11bg, 经典注入芯片）
// ============================================================
class Rtl8187lMonitor : public WifiMonitorDevice {
public:
    static Rtl8187lMonitor& instance() {
        static Rtl8187lMonitor driver;
        return driver;
    }

    bool init() override;
    bool deinit() override;
    bool enter_monitor_mode() override;
    bool leave_monitor_mode() override;
    bool is_monitor_mode() const override;
    bool set_channel(uint16_t frequency_mhz) override;
    uint16_t get_channel() const override;
    int capture_frame(CapturedFrame& out_frame) override;
    bool inject_frame(const uint8_t* frame, int len) override;
    bool supports_injection() const override { return true; }
    bool supports_5ghz() const override { return false; }
    bool supports_6ghz() const override { return false; }
    const char* chipset_name() const override { return "RTL8187L"; }
    DriverState get_state() const override { return state_; }

private:
    Rtl8187lMonitor() = default;

    static constexpr uint16_t REG_GPIO          = 0x0004;
    static constexpr uint16_t REG_TX_CONF       = 0x0010;
    static constexpr uint16_t REG_RX_CONF       = 0x0018;
    static constexpr uint16_t REG_CHANNEL_8187  = 0x0020;

    DriverState state_ = DriverState::Uninitialized;
    bool monitor_mode_ = false;
    Mutex device_mutex_;
};

// ============================================================
// 信道跳变管理器（供扫描引擎使用）
// ============================================================
class ChannelHopper {
public:
    // 配置跳变参数
    void configure(const uint16_t* channels, int count, uint32_t dwell_ms) {
        channel_count_ = (count > kMaxChannels) ? kMaxChannels : count;
        for (int i = 0; i < channel_count_; ++i) {
            channels_[i] = channels[i];
        }
        dwell_ms_ = dwell_ms;
        current_index_ = 0;
    }

    // 获取当前停留的信道
    uint16_t current_channel() const {
        if (channel_count_ == 0) return 2412;
        return channels_[current_index_];
    }

    // 跳变到下一个信道，返回新信道
    uint16_t hop() {
        if (channel_count_ == 0) return 2412;
        current_index_ = (current_index_ + 1) % channel_count_;
        return channels_[current_index_];
    }

    // 2.4GHz 全信道列表（1-14）
    static void make_24ghz_all(uint16_t* out_channels, int* out_count) {
        constexpr uint16_t freqs[] = {
            2412, 2417, 2422, 2427, 2432, // 1-5
            2437, 2442, 2447, 2452, 2457, // 6-10
            2462, 2467, 2472, 2484         // 11-14
        };
        *out_count = 14;
        for (int i = 0; i < 14; ++i) out_channels[i] = freqs[i];
    }

    // 非重叠 2.4GHz 信道（1, 6, 11）
    static void make_24ghz_non_overlap(uint16_t* out_channels, int* out_count) {
        out_channels[0] = 2412;
        out_channels[1] = 2437;
        out_channels[2] = 2462;
        *out_count = 3;
    }

    // 5GHz 非重叠信道子集（UNII-1/2/3 部分）
    static void make_5ghz_common(uint16_t* out_channels, int* out_count) {
        constexpr uint16_t freqs[] = {
            5180, 5200, 5220, 5240,      // UNII-1
            5260, 5280, 5300, 5320,      // UNII-2
            5500, 5520, 5540, 5560,      // UNII-2e
            5745, 5765, 5785, 5805, 5825 // UNII-3
        };
        *out_count = 17;
        for (int i = 0; i < 17; ++i) out_channels[i] = freqs[i];
    }

    int channel_count() const { return channel_count_; }
    uint32_t dwell_ms() const { return dwell_ms_; }

private:
    static constexpr int kMaxChannels = 64;
    uint16_t channels_[kMaxChannels]{};
    int      channel_count_ = 0;
    int      current_index_ = 0;
    uint32_t dwell_ms_ = 100;
};

// ============================================================
// 帧解析工具函数
// ============================================================
namespace auroraos { namespace wireless {

    // 从 802.11 帧数据解析帧控制字段
    inline FrameControl parse_frame_control(const uint8_t* frame, int len) {
        FrameControl fc{};
        if (len < 2) return fc;
        uint16_t raw = frame[0] | (static_cast<uint16_t>(frame[1]) << 8);
        fc.protocol_version = static_cast<uint8_t>(raw & 0x03);
        fc.type             = static_cast<uint8_t>((raw >> 2) & 0x03);
        fc.subtype          = static_cast<uint8_t>((raw >> 4) & 0x0F);
        fc.to_ds            = static_cast<uint8_t>((raw >> 8) & 0x01);
        fc.from_ds          = static_cast<uint8_t>((raw >> 9) & 0x01);
        fc.more_frag        = static_cast<uint8_t>((raw >> 10) & 0x01);
        fc.retry            = static_cast<uint8_t>((raw >> 11) & 0x01);
        fc.power_mgmt       = static_cast<uint8_t>((raw >> 12) & 0x01);
        fc.more_data        = static_cast<uint8_t>((raw >> 13) & 0x01);
        fc.wep              = static_cast<uint8_t>((raw >> 14) & 0x01);
        fc.order            = static_cast<uint8_t>((raw >> 15) & 0x01);
        return fc;
    }

    // 识别帧类型
    inline FrameType classify_frame(const FrameControl& fc) {
        uint8_t raw = (fc.type << 4) | fc.subtype;
        switch (raw) {
            case 0x80: return FrameType::MgmtBeacon;
            case 0x40: return FrameType::MgmtProbeReq;
            case 0x50: return FrameType::MgmtProbeResp;
            case 0xB0: return FrameType::MgmtAuth;
            case 0xC0: return FrameType::MgmtDeauth;
            case 0x00: return FrameType::MgmtAssocReq;
            case 0x10: return FrameType::MgmtAssocResp;
            case 0xA0: return FrameType::MgmtDisassoc;
            case 0xD0: return FrameType::MgmtAction;
            case 0x08: return FrameType::Data;
            case 0x48: return FrameType::DataNull;
            case 0x88: return FrameType::DataQos;
            default:   return FrameType::Unknown;
        }
    }

}} // namespace auroraos::wireless

#endif // AURORA_WIRELESS_WIFI_MONITOR_HPP
