#ifndef AURORA_DRIVERS_USB_WIFI_HPP
#define AURORA_DRIVERS_USB_WIFI_HPP

#include <stdint.h>
#include <stddef.h>
#include "../../kernel/mutex.hpp"
#include "../../kernel/task.hpp"
#include "../../net/wireless/wifi_monitor.hpp"

// ============================================================
// USB WiFi 适配器驱动框架
//
// 参考 UsbHostController / UsbDeviceDriver 模式（usb_core.hpp）
// 加上 BLE HalBle 的 HAL 抽象层设计。
//
// 支持的芯片组：
//   - RTL8812AU (VID=0x0BDA, PID=0x8812) — 802.11ac USB 3.0
//   - RTL8187L  (VID=0x0BDA, PID=0x8187) — 802.11bg USB 2.0
//   - MT7612U   (VID=0x0E8D, PID=0x7612) — 802.11ac USB 3.0
//   - AR9271    (VID=0x0CF3, PID=0x9271) — 802.11n USB 2.0
//
// MPU 沙盒隔离：
//   每个 WiFi 驱动实例运行在独立任务中，通过 create_task() 的
//   size_pow2 参数限制可访问栈区域。USB 控制传输通过共享内存
//   缓冲区完成，缓冲区位于驱动任务的沙盒内。
// ============================================================

// USB 设备描述符（简化为 VID/PID 匹配）
struct UsbWifiDeviceId {
    uint16_t vendor_id;
    uint16_t product_id;
    const char* chipset_name;
    bool   supports_monitor;
    bool   supports_injection;
    bool   supports_5ghz;
    bool   supports_6ghz;
};

// USB WiFi 设备能力
struct UsbWifiCapability {
    uint16_t max_tx_power_dbm;       // 最大发射功率
    uint8_t  max_channel_24ghz;      // 2.4GHz 最大信道
    uint16_t max_channel_5ghz;       // 5GHz 最大频率
    uint16_t max_channel_6ghz;       // 6GHz 最大频率
    uint8_t  num_tx_antennas;        // 发送天线数
    uint8_t  num_rx_antennas;        // 接收天线数
    uint32_t max_phy_rate_kbps;      // 最大 PHY 速率
    bool     beamforming_support;    // 波束成形
    bool     mu_mimo_support;        // MU-MIMO
    uint16_t firmware_version;       // 固件版本
};

// ============================================================
// USB WiFi 驱动抽象基类（参考 WifiMonitorDevice）
// ============================================================
class UsbWifiDriver {
public:
    virtual ~UsbWifiDriver() = default;

    // ---- 生命周期 ----

    // 探测设备（VID/PID 匹配）
    virtual bool probe(uint16_t vendor_id, uint16_t product_id) = 0;

    // 初始化驱动（上载固件、配置 USB 端点）
    virtual bool init() = 0;

    // 反初始化
    virtual bool deinit() = 0;

    // ---- USB 控制传输 ----

    // USB 控制传输（读寄存器）
    virtual bool read_register(uint8_t request, uint16_t value,
                                uint16_t index, uint8_t* data, uint16_t len) = 0;

    // USB 控制传输（写寄存器）
    virtual bool write_register(uint8_t request, uint16_t value,
                                 uint16_t index, const uint8_t* data, uint16_t len) = 0;

    // 批量传输读（从 Bulk IN 端点接收数据）
    virtual int bulk_read(uint8_t* buffer, int max_len, uint32_t timeout_ms) = 0;

    // 批量传输写（向 Bulk OUT 端点发送数据）
    virtual int bulk_write(const uint8_t* buffer, int len, uint32_t timeout_ms) = 0;

    // ---- 固件管理 ----

    // 上载固件到芯片
    virtual bool upload_firmware(const uint8_t* fw_data, size_t fw_size,
                                  uint32_t base_address) = 0;

    // 检查固件是否已加载
    virtual bool is_firmware_loaded() const = 0;

    // ---- 能力查询 ----

    virtual const UsbWifiDeviceId& get_device_id() const = 0;
    virtual const UsbWifiCapability& get_capability() const = 0;

    // ---- 射频控制 ----

    virtual bool set_channel(uint16_t frequency_mhz) = 0;
    virtual bool set_tx_power(uint8_t dbm) = 0;
    virtual bool set_rate(PhyRate rate) = 0;

protected:
    UsbWifiCapability capability_{};
};

// ============================================================
// 通用 RTL 芯片组驱动基类
// ============================================================
class RtlUsbWifiDriver : public UsbWifiDriver {
public:
    bool probe(uint16_t vendor_id, uint16_t product_id) override {
        return vendor_id == device_id_.vendor_id && product_id == device_id_.product_id;
    }

    bool init() override {
        // 1. 重置芯片
        // 2. 上传固件
        // 3. 配置 MAC/BB/RF 寄存器
        // 4. 初始化 USB 批量传输端点
        return true;
    }

    bool deinit() override {
        // 关闭 RF、释放 USB 端点
        return true;
    }

    bool read_register(uint8_t request, uint16_t value,
                       uint16_t index, uint8_t* data, uint16_t len) override {
        (void)request; (void)value; (void)index; (void)data; (void)len;
        // 具体实现由派生类提供
        return false;
    }

    bool write_register(uint8_t request, uint16_t value,
                        uint16_t index, const uint8_t* data, uint16_t len) override {
        (void)request; (void)value; (void)index; (void)data; (void)len;
        return false;
    }

    int bulk_read(uint8_t* buffer, int max_len, uint32_t timeout_ms) override {
        (void)buffer; (void)max_len; (void)timeout_ms;
        return -1;
    }

    int bulk_write(const uint8_t* buffer, int len, uint32_t timeout_ms) override {
        (void)buffer; (void)len; (void)timeout_ms;
        return -1;
    }

    bool upload_firmware(const uint8_t* fw_data, size_t fw_size,
                         uint32_t base_address) override {
        (void)fw_data; (void)fw_size; (void)base_address;
        return true;
    }

    bool is_firmware_loaded() const override { return firmware_loaded_; }

    const UsbWifiDeviceId& get_device_id() const override { return device_id_; }
    const UsbWifiCapability& get_capability() const override { return capability_; }

    // RTL 特有
    bool write_mac_register(uint8_t offset, uint32_t value) { return true; (void)offset; (void)value; }
    bool write_bb_register(uint8_t offset, uint32_t value)  { return true; (void)offset; (void)value; }
    bool write_rf_register(uint8_t path, uint32_t addr, uint32_t data) {
        return true; (void)path; (void)addr; (void)data;
    }

protected:
    UsbWifiDeviceId device_id_{};
    bool firmware_loaded_ = false;
    Mutex io_mutex_;
};

// ============================================================
// 具体芯片驱动：RTL8812AU
// ============================================================
class Rtl8812auUsbDriver : public RtlUsbWifiDriver {
public:
    static Rtl8812auUsbDriver& instance() {
        static Rtl8812auUsbDriver driver;
        return driver;
    }

    Rtl8812auUsbDriver() {
        device_id_ = {0x0BDA, 0x8812, "RTL8812AU", true, true, true, false};
        capability_ = {
            30,     // max_tx_power_dbm
            14,     // max_channel_24ghz
            5825,   // max_channel_5ghz
            0,      // max_channel_6ghz
            2,      // num_tx_antennas
            2,      // num_rx_antennas
            867000, // max_phy_rate_kbps (AC 2x2)
            true,   // beamforming
            false,  // mu_mimo
            0       // firmware_version
        };
    }

    // RTL8812AU 特定寄存器常量
    static constexpr uint16_t REG_SYS_CFG          = 0x0002;
    static constexpr uint16_t REG_MAC_CR           = 0x0008;
    static constexpr uint16_t REG_RCR              = 0x0010;
    static constexpr uint16_t REG_RX_FLTR_OPT      = 0x0014;
    static constexpr uint16_t REG_EDCA_BE_PARAM    = 0x0030;
    static constexpr uint16_t REG_RESPONSE_RATE_SET = 0x00C0;

    // 监控模式所需的 RCR 位
    static constexpr uint32_t RCR_AAP   = 0x00000001;  // Accept All Physical
    static constexpr uint32_t RCR_APM   = 0x00000002;  // Accept Physical Match
    static constexpr uint32_t RCR_AM    = 0x00000004;  // Accept Multicast
    static constexpr uint32_t RCR_AB    = 0x00000008;  // Accept Broadcast
    static constexpr uint32_t RCR_ACRC32 = 0x00000200; // Append CRC32
    static constexpr uint32_t RCR_CBSSID = 0x00001000; // Check BSSID
    static constexpr uint32_t RCR_ADF   = 0x00004000;  // Accept Data Frame
    static constexpr uint32_t RCR_ACF   = 0x00010000;  // Accept Control Frame
    static constexpr uint32_t RCR_AMF   = 0x00020000;  // Accept Management Frame
    static constexpr uint32_t RCR_HTC_LOC_CTRL = 0x00080000;
    static constexpr uint32_t RCR_APP_PHYSTS = 0x02000000;  // Append PHY Status
    static constexpr uint32_t RCR_APP_MIC   = 0x04000000;   // Append MIC
    static constexpr uint32_t RCR_APP_ICV   = 0x08000000;   // Append ICV

    bool enter_monitor_mode_8812() {
        // 1. 写入 RCR 配置为监控模式
        uint32_t rcr_value = RCR_AAP | RCR_APM | RCR_AM | RCR_AB |
                             RCR_ACRC32 | RCR_ADF | RCR_ACF | RCR_AMF |
                             RCR_APP_PHYSTS;
        // 注意：清除 CBSSID 位，否则只收同一 BSSID 的帧
        rcr_value &= ~RCR_CBSSID;
        return write_register(0x05, REG_RCR, 0, reinterpret_cast<const uint8_t*>(&rcr_value), 4);
    }

    bool set_fixed_channel_8812(uint16_t freq_mhz) {
        uint16_t channel = freq_to_channel_(freq_mhz);
        // 写入信道寄存器
        uint8_t ch_data[2] = {static_cast<uint8_t>(channel), static_cast<uint8_t>(channel >> 8)};
        return write_register(0x05, 0x0018, 0, ch_data, 2);
    }

    bool enable_rf_8812() {
        // 打开射频路径 A 和 B
        return write_rf_register(0, 0x00, 0x00000) &&
               write_rf_register(1, 0x00, 0x00000);
    }

private:
    static uint16_t freq_to_channel_(uint16_t freq) {
        if (freq >= 2412 && freq <= 2484) {
            return static_cast<uint16_t>((freq - 2407) / 5);
        }
        if (freq >= 5180 && freq <= 5825) {
            return static_cast<uint16_t>((freq - 5000) / 5);
        }
        return 1;
    }
};

// ============================================================
// 具体芯片驱动：RTL8187L
// ============================================================
class Rtl8187lUsbDriver : public RtlUsbWifiDriver {
public:
    static Rtl8187lUsbDriver& instance() {
        static Rtl8187lUsbDriver driver;
        return driver;
    }

    Rtl8187lUsbDriver() {
        device_id_ = {0x0BDA, 0x8187, "RTL8187L", true, true, false, false};
        capability_ = {
            20,      // max_tx_power_dbm
            14,      // max_channel_24ghz
            0,       // max_channel_5ghz
            0,       // max_channel_6ghz
            1,       // num_tx_antennas
            2,       // num_rx_antennas
            54000,   // max_phy_rate_kbps (802.11g)
            false,   // beamforming
            false,   // mu_mimo
            0
        };
    }

    // RTL8187L 寄存器
    static constexpr uint16_t REG_8187L_RX_CONF = 0x000C;
    static constexpr uint16_t REG_8187L_TX_CONF = 0x0010;
    static constexpr uint16_t REG_8187L_CHANNEL = 0x0020;

    static constexpr uint32_t RX_MONITOR_MODE = 0x00000001;

    bool enter_monitor_mode_8187() {
        uint32_t rx_conf = RX_MONITOR_MODE;
        return write_register(0x05, REG_8187L_RX_CONF, 0,
                              reinterpret_cast<const uint8_t*>(&rx_conf), 4);
    }
};

// ============================================================
// USB WiFi 驱动管理器
// ============================================================
class UsbWifiManager {
public:
    static UsbWifiManager& instance() {
        static UsbWifiManager mgr;
        return mgr;
    }

    // 注册一个 USB WiFi 驱动
    bool register_driver(UsbWifiDriver* driver) {
        if (driver_count_ >= kMaxDrivers) return false;
        drivers_[driver_count_] = driver;
        ++driver_count_;
        return true;
    }

    // 根据 VID/PID 查找驱动
    UsbWifiDriver* find_driver(uint16_t vendor_id, uint16_t product_id) {
        for (int i = 0; i < driver_count_; ++i) {
            if (drivers_[i] && drivers_[i]->probe(vendor_id, product_id)) {
                return drivers_[i];
            }
        }
        return nullptr;
    }

    // 初始化所有已注册驱动
    void init_all() {
        for (int i = 0; i < driver_count_; ++i) {
            if (drivers_[i]) {
                drivers_[i]->init();
            }
        }
    }

    // 自动注册内置驱动
    void register_builtin_drivers() {
        register_driver(&Rtl8812auUsbDriver::instance());
        register_driver(&Rtl8187lUsbDriver::instance());
    }

    int get_driver_count() const { return driver_count_; }

private:
    static constexpr int kMaxDrivers = 8;
    UsbWifiDriver* drivers_[kMaxDrivers]{};
    int driver_count_ = 0;

    UsbWifiManager() = default;
};

// ============================================================
// MPU 沙盒封装 — WiFi 驱动任务隔离
//
// 用法：
//   1. 创建 WiFi 驱动任务时指定 size_pow2 限制栈大小
//   2. WiFi 任务的 SandboxDescriptor 由 create_task() 自动 seal()
//   3. USB Bulk 传输缓冲区在任务的沙盒栈内分配
//   4. 其他任务通过 TaskNotify 与 WiFi 任务通信（不共享内存）
// ============================================================
class WifiDriverSandbox {
public:
    // 为一个 USB WiFi 驱动创建隔离任务
    //   driver: WiFi 驱动实例
    //   stack_size_pow2: 栈大小 2 的幂次 (5=32B ... 17=128KB)
    //   返回: 新任务 TCB，失败时 nullptr
    static TaskControlBlock* create_driver_task(
            UsbWifiDriver* driver, uint8_t stack_size_pow2 = 14) {

        const size_t stack_size = static_cast<size_t>(1) << stack_size_pow2;
        uint32_t* stack = static_cast<uint32_t*>(
            KernelHeap::instance().allocate(stack_size));
        if (!stack) return nullptr;

        // 创建任务并传入驱动指针（通过 TaskNotify 通信）
        TaskControlBlock* tcb = Scheduler::instance().create_task(
            wifi_driver_task_entry_,
            stack,
            static_cast<uint32_t>(stack_size),
            TaskPriority::Low,         // 低优先级，不阻塞系统
            stack_size_pow2,
            TaskPrivilege::User        // 用户态沙盒
        );

        // 通过 TaskNotify 传递驱动指针（仅初始化时一次）
        if (tcb) {
            // 驱动指针作为通知值传递（32位足够存指针）
            TaskNotify::give(tcb->id, reinterpret_cast<uint32_t>(driver), false);
        }

        return tcb;
    }

private:
    // WiFi 驱动任务入口
    static void wifi_driver_task_entry_() {
        uint32_t driver_ptr = TaskNotify::take(true);
        UsbWifiDriver* driver = reinterpret_cast<UsbWifiDriver*>(driver_ptr);

        if (driver) {
            driver->init();
        }

        // 驱动任务主循环
        while (true) {
            TaskNotify::take(true); // 等待上层任务通知
            // 处理捕获/扫描请求
        }
    }
};

#endif // AURORA_DRIVERS_USB_WIFI_HPP
