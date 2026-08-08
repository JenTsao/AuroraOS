// ============================================================
// rtl8187l_monitor.cpp — RTL8187L USB WiFi Monitor Driver
//
// RTL8187L: USB 2.0, 802.11b/g, classic injection-capable chipset.
// Registers are documented in the Realtek RTL8187L datasheet and
// the Linux rtl8187 driver (drivers/net/wireless/realtek/rtl818x/).
//
// Key features:
//   - Enter/leave monitor mode via RX configuration register
//   - Channel control via analog RF register write
//   - Frame capture through USB bulk IN endpoint
//   - Frame injection through USB bulk OUT endpoint
//   - TX power control via analog register
// ============================================================

#include "../wireless/wifi_monitor.hpp"
#include "../../drivers/usb/usb_host.hpp"
#include <string.h>

// ---- External tick_count (SysTick-driven, 1ms resolution) ----
extern volatile uint32_t tick_count;

// ---- RTL8187L MAC Register Offsets ----
namespace Rtl8187lReg {
    constexpr uint16_t MAC_ADDR         = 0x0000;  // 6-byte MAC address
    constexpr uint16_t REG_GPIO         = 0x0004;  // General-purpose I/O
    constexpr uint16_t REG_TX_CONF      = 0x0010;  // Transmit configuration
    constexpr uint16_t REG_RX_CONF      = 0x0018;  // Receive configuration
    constexpr uint16_t REG_CHANNEL      = 0x0020;  // Channel/frequency control
    constexpr uint16_t REG_ANAPARM      = 0x0084;  // Analog parameter
    constexpr uint16_t REG_ANAPARM2     = 0x0088;  // Analog parameter 2
    constexpr uint16_t REG_ANAPARM3     = 0x008C;  // Analog parameter 3
    constexpr uint16_t REG_BRSR         = 0x0048;  // Basic rate set
    constexpr uint16_t REG_TX_GAIN      = 0x0024;  // TX gain control
    constexpr uint16_t REG_RF_CTRL      = 0x0030;  // RF control register
    constexpr uint16_t REG_WPA_CONFIG   = 0x0050;  // WPA/WEP config
    constexpr uint16_t REG_ACPI_WAKEUP  = 0x0054;  // ACPI wakeup
    constexpr uint16_t REG_EEPROM_CMD   = 0x0058;  // EEPROM command
    constexpr uint16_t REG_RF_PARA      = 0x0060;  // RF parameters
}

// RX configuration bits
namespace RtlRxConf {
    // Monitor mode: accept all frames regardless of destination
    constexpr uint32_t RX_MONITOR       = 0x00000001;  // Accept all (promiscuous+monitor)
    constexpr uint32_t RX_CTRL_FRAME    = 0x00000002;  // Accept control frames
    constexpr uint32_t RX_BAD_FCS       = 0x00000004;  // Accept frames with bad FCS
    constexpr uint32_t RX_ALL_MULTICAST = 0x00000008;  // Accept all multicast
    constexpr uint32_t RX_BROADCAST     = 0x00000010;  // Accept broadcast
    constexpr uint32_t RX_DATA_FRAME    = 0x00000020;  // Accept data frames
    // Normal mode: filtered by MAC address
    constexpr uint32_t RX_NORMAL        = 0x00000000;
}

// TX configuration bits
namespace RtlTxConf {
    constexpr uint32_t TX_LOOPBACK_CONT  = 0x00000001;  // Continuous loopback
    constexpr uint32_t TX_NO_CCK         = 0x00000002;  // Disable CCK rates
    constexpr uint32_t TX_NO_OFDM        = 0x00000004;  // Disable OFDM rates
    constexpr uint32_t TX_MAC_LOOPBACK   = 0x00000008;  // MAC layer loopback
    constexpr uint32_t TX_DISABLE_PREAM  = 0x00000010;  // Disable preamble
}

// ---- Channel ↔ Frequency Mapping (2.4GHz, 802.11b/g) ----

static uint8_t freq_to_channel_8187(uint16_t freq_mhz) {
    if (freq_mhz >= 2412 && freq_mhz <= 2472) {
        if (freq_mhz == 2484) return 14;
        uint16_t diff = freq_mhz - 2407;
        return static_cast<uint8_t>((diff + 2) / 5);
    }
    return 1;  // default: channel 1 (2412 MHz)
}

// ---- RTL8187L USB R/W Helpers ----

static bool rtl_reg_read(UsbDevice& dev, uint16_t reg, uint32_t* val) {
    return UsbHost::instance().read_register(dev, reg, val);
}

static bool rtl_reg_write(UsbDevice& dev, uint16_t reg, uint32_t val) {
    return UsbHost::instance().write_register(dev, reg, val);
}

static bool rtl_reg_write_mask(UsbDevice& dev, uint16_t reg,
                                 uint32_t mask, uint32_t set) {
    uint32_t val;
    if (!rtl_reg_read(dev, reg, &val)) return false;
    val = (val & ~mask) | (set & mask);
    return rtl_reg_write(dev, reg, val);
}

// ---- RTL8187L Monitor Driver Implementation ----

bool Rtl8187lMonitor::init() {
    LockGuard lock(device_mutex_);

    if (state_ == DriverState::Ready || state_ == DriverState::Monitor)
        return true;

    // 1. Init USB host
    UsbHost::instance().init();
    if (!UsbHost::instance().is_initialized()) {
        state_ = DriverState::Error;
        return false;
    }

    // 2. Enumerate device
    UsbDevice dev;
    if (!UsbHost::instance().enumerate_device(dev)) {
        state_ = DriverState::Error;
        return false;
    }

    // Verify it's an RTL8187L
    // RTL8187L: VID=0x0BDA PID=0x8187 (Realtek)
    // We accept VID==0x0BDA and PID in {0x8187, 0x8189} (RTL8187L/B variants)
    if (dev.vendor_id != 0x0BDA ||
        (dev.product_id != 0x8187 && dev.product_id != 0x8189)) {
        state_ = DriverState::Error;
        return false;
    }

    // 3. Reset the chip
    // Write 0 to reset, then set normal operation
    rtl_reg_write(dev, Rtl8187lReg::REG_TX_CONF, 0);
    rtl_reg_write(dev, Rtl8187lReg::REG_RX_CONF, 0);

    for (volatile int d = 0; d < 5000; ++d) { __asm__ volatile("nop"); }

    // 4. Enable TX/RX engines
    rtl_reg_write(dev, Rtl8187lReg::REG_TX_CONF, 0);
    rtl_reg_write(dev, Rtl8187lReg::REG_RX_CONF, RtlRxConf::RX_NORMAL);

    // 5. Set default channel (1 = 2412MHz)
    rtl_reg_write(dev, Rtl8187lReg::REG_CHANNEL, 1);

    // 6. Read MAC address
    uint32_t mac_lo, mac_hi;
    if (rtl_reg_read(dev, Rtl8187lReg::MAC_ADDR, &mac_lo) &&
        rtl_reg_read(dev, Rtl8187lReg::MAC_ADDR + 4, &mac_hi)) {
        mac_address_[0] = static_cast<uint8_t>(mac_lo & 0xFF);
        mac_address_[1] = static_cast<uint8_t>((mac_lo >> 8) & 0xFF);
        mac_address_[2] = static_cast<uint8_t>((mac_lo >> 16) & 0xFF);
        mac_address_[3] = static_cast<uint8_t>((mac_lo >> 24) & 0xFF);
        mac_address_[4] = static_cast<uint8_t>(mac_hi & 0xFF);
        mac_address_[5] = static_cast<uint8_t>((mac_hi >> 8) & 0xFF);
    }

    state_ = DriverState::Ready;
    return true;
}

bool Rtl8187lMonitor::deinit() {
    LockGuard lock(device_mutex_);
    leave_monitor_mode();
    state_ = DriverState::Uninitialized;
    return true;
}

bool Rtl8187lMonitor::enter_monitor_mode() {
    LockGuard lock(device_mutex_);
    if (state_ == DriverState::Monitor) return true;
    if (state_ != DriverState::Ready) return false;

    UsbDevice dev;
    dev.vendor_id  = 0x0BDA;
    dev.product_id = 0x8187;
    if (!UsbHost::instance().enumerate_device(dev)) return false;

    // Configure RX for monitor mode:
    //   Accept all frames (RX_MONITOR | RX_CTRL_FRAME | RX_BAD_FCS)
    uint32_t rx_conf = RtlRxConf::RX_MONITOR | RtlRxConf::RX_CTRL_FRAME
                     | RtlRxConf::RX_BAD_FCS | RtlRxConf::RX_ALL_MULTICAST
                     | RtlRxConf::RX_BROADCAST | RtlRxConf::RX_DATA_FRAME;
    if (!rtl_reg_write(dev, Rtl8187lReg::REG_RX_CONF, rx_conf)) return false;

    // Disable MAC filtering
    rtl_reg_write(dev, Rtl8187lReg::REG_WPA_CONFIG, 0);

    monitor_mode_ = true;
    state_ = DriverState::Monitor;
    return true;
}

bool Rtl8187lMonitor::leave_monitor_mode() {
    LockGuard lock(device_mutex_);
    if (!monitor_mode_) return true;

    UsbDevice dev;
    dev.vendor_id  = 0x0BDA;
    dev.product_id = 0x8187;

    if (UsbHost::instance().enumerate_device(dev)) {
        rtl_reg_write(dev, Rtl8187lReg::REG_RX_CONF, RtlRxConf::RX_NORMAL);
    }

    monitor_mode_ = false;
    state_ = DriverState::Ready;
    return true;
}

bool Rtl8187lMonitor::is_monitor_mode() const {
    return monitor_mode_;
}

bool Rtl8187lMonitor::set_channel(uint16_t frequency_mhz) {
    LockGuard lock(device_mutex_);
    if (state_ == DriverState::Uninitialized || state_ == DriverState::Error)
        return false;

    UsbDevice dev;
    dev.vendor_id  = 0x0BDA;
    dev.product_id = 0x8187;
    if (!UsbHost::instance().enumerate_device(dev)) return false;

    uint8_t ch = freq_to_channel_8187(frequency_mhz);

    // 1. Write channel number to MAC register
    if (!rtl_reg_write(dev, Rtl8187lReg::REG_CHANNEL, ch)) return false;

    // 2. Configure analog RF parameters for the channel
    //    (Simplified — full table lookup would be needed for production)
    uint32_t analog_val = (ch <= 11) ? 0x00000020 : 0x00000028;
    rtl_reg_write(dev, Rtl8187lReg::REG_ANAPARM, analog_val);

    current_channel_.frequency_mhz  = frequency_mhz;
    current_channel_.channel_number = ch;
    current_channel_.band = 0;  // 2.4GHz only for RTL8187L

    // Allow hardware to settle
    for (volatile int d = 0; d < 5000; ++d) { __asm__ volatile("nop"); }

    return true;
}

uint16_t Rtl8187lMonitor::get_channel() const {
    return current_channel_.frequency_mhz;
}

int Rtl8187lMonitor::capture_frame(CapturedFrame& out_frame) {
    LockGuard lock(device_mutex_);
    if (state_ != DriverState::Monitor) return -1;

    UsbDevice dev;
    dev.vendor_id  = 0x0BDA;
    dev.product_id = 0x8187;
    dev.address = 1; // single device, always address 1 after enumeration

    // USB bulk IN from EP_BULK_IN (0x83 = EP3 IN)
    uint8_t buf[2048];
    UsbTransferResult r = UsbHost::instance().bulk_in(dev, 3, buf, sizeof(buf));

    if (!r.success || r.bytes_transferred <= 0) return 0;

    int rx_len = r.bytes_transferred;

    // RTL8187L frames include a 4-byte header: [rx_ok(1) | rssi(1) | reserved(2)]
    if (rx_len < 5) return 0;

    uint8_t  rx_ok = buf[0];
    int8_t   rssi  = static_cast<int8_t>(buf[1]);
    int      frame_len = rx_len - 4;

    if (!rx_ok || frame_len <= 0 || frame_len > 2346) return 0;

    // Fill CapturedFrame
    out_frame.timestamp_sec  = tick_count / 1000;
    out_frame.timestamp_usec = (tick_count % 1000) * 1000;
    out_frame.rssi_dbm       = rssi;
    out_frame.noise_dbm      = static_cast<uint8_t>(-95);  // typical noise floor
    out_frame.channel_freq   = current_channel_.frequency_mhz;
    out_frame.frame_len      = static_cast<uint16_t>(frame_len);

    for (int i = 0; i < frame_len; ++i)
        out_frame.data[i] = buf[4 + i];

    return frame_len;
}

bool Rtl8187lMonitor::inject_frame(const uint8_t* frame, int len) {
    LockGuard lock(device_mutex_);
    if (state_ != DriverState::Monitor || len <= 0 || len > 2346) return false;

    UsbDevice dev;
    dev.vendor_id  = 0x0BDA;
    dev.product_id = 0x8187;
    dev.address = 1;

    // RTL8187L injection: wrap frame in USB bulk OUT
    // with a 1-byte TX descriptor header
    uint8_t tx_buf[2350];
    tx_buf[0] = 0x00;  // TX descriptor: flags (normal TX)
    for (int i = 0; i < len; ++i) tx_buf[1 + i] = frame[i];

    UsbTransferResult r = UsbHost::instance().bulk_out(dev, 2, tx_buf, len + 1);
    return r.success;
}
