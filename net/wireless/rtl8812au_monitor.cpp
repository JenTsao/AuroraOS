// ============================================================
// rtl8812au_monitor.cpp — RTL8812AU USB WiFi Monitor Driver
//
// RTL8812AU: USB 3.0, 802.11a/b/g/n/ac, dual-band, MU-MIMO.
// Register map based on Realtek RTL8812AU datasheet and the
// Linux rtl8812au driver (aicsm/rtl8812au).
//
// Key features:
//   - Firmware upload via USB bulk OUT
//   - Monitor mode via RCR (Receive Configuration Register)
//   - Dual-band channel control (2.4GHz + 5GHz)
//   - Frame capture via USB bulk IN (EP 0x83)
//   - Frame injection via USB bulk OUT (EP 0x02)
//   - TX power control via BB register
// ============================================================

#include "../wireless/wifi_monitor.hpp"
#include "../../drivers/usb/usb_host.hpp"
#include <string.h>

extern volatile uint32_t tick_count;

// ---- RTL8812AU Register Offsets ----
namespace Rtl8812auReg {
constexpr uint16_t REG_SYS_ISO_CTRL = 0x0000;  // System isolation control
constexpr uint16_t REG_SYS_FUNC_EN = 0x0002;   // System function enable
constexpr uint16_t REG_CR = 0x0002;            // Command Register (alias)
constexpr uint16_t REG_RCR = 0x0008;           // Receive Configuration Register
constexpr uint16_t REG_RX_PKT_LEN = 0x0010;    // Max RX packet length
constexpr uint16_t REG_CHANNEL = 0x0018;       // Channel setting (MAC)
constexpr uint16_t REG_TXPAUSE = 0x0022;       // TX pause
constexpr uint16_t REG_RXFLTMAP0 = 0x0028;     // RX filter map 0
constexpr uint16_t REG_RXFLTMAP1 = 0x002A;     // RX filter map 1
constexpr uint16_t REG_RXFLTMAP2 = 0x002C;     // RX filter map 2
constexpr uint16_t REG_MAR = 0x0020;           // Multicast address register
constexpr uint16_t REG_BCN_CTRL = 0x0050;      // Beacon control
constexpr uint16_t REG_TBTT = 0x0056;          // TBTT timer
constexpr uint16_t REG_GPIO_MUXCFG = 0x0040;   // GPIO mux config
constexpr uint16_t REG_AFE_MISC = 0x0080;      // AFE misc
constexpr uint16_t REG_AFE_XTAL_CTRL = 0x0084; // AFE XTAL control
constexpr uint16_t REG_LDO_SWR_CTRL = 0x0088;  // LDO/SWR control
constexpr uint16_t REG_LEDCFG0 = 0x008C;       // LED config 0
constexpr uint16_t REG_RF_CTRL = 0x00C0;       // RF control
constexpr uint16_t REG_BB_CTRL = 0x00C4;       // Baseband control
constexpr uint16_t REG_FW_START_ADDR = 0x1000; // Firmware start address
constexpr uint16_t REG_MCU_FW_DL = 0x1080;     // MCU firmware download control
constexpr uint16_t REG_HIMR = 0x00B0;          // Host interrupt mask
constexpr uint16_t REG_HISR = 0x00B4;          // Host interrupt status
constexpr uint16_t REG_EFUSE_CTRL = 0x0030;    // eFuse control
constexpr uint16_t REG_EFUSE_TEST = 0x0034;    // eFuse test
constexpr uint16_t REG_TXPKT_EMPTY = 0x0060;   // TX packet empty
constexpr uint16_t REG_RXDMA_CTRL = 0x0280;    // RX DMA control
constexpr uint16_t REG_BCNQ_DESA = 0x0300;     // Beacon queue descriptor address
constexpr uint16_t REG_RXQ_CTRL = 0x0308;      // RX queue control
} // namespace Rtl8812auReg

// RCR (Receive Configuration Register) bits
namespace RtlRcr {
constexpr uint32_t AAP = 0x00000001;    // Accept all physical packets
constexpr uint32_t APM = 0x00000002;    // Accept physical match
constexpr uint32_t AM = 0x00000004;     // Accept multicast
constexpr uint32_t AB = 0x00000008;     // Accept broadcast
constexpr uint32_t AICV = 0x00000010;   // Accept ICV error
constexpr uint32_t ACF = 0x00000020;    // Accept CRC32 error (monitor mode)
constexpr uint32_t AMF = 0x00000040;    // Accept management frames
constexpr uint32_t ADF = 0x00000080;    // Accept data frames
constexpr uint32_t ACFG = 0x00000100;   // Accept configuration frames
constexpr uint32_t CBSSID = 0x00000200; // Check BSSID match
// Monitor mode: AAP | ACF | AMF | ADF
constexpr uint32_t MONITOR = AAP | ACF | AMF | ADF;
// Normal mode: APM | AM | AB
constexpr uint32_t NORMAL = APM | AM | AB;
} // namespace RtlRcr

// CR (Command Register) bits
namespace RtlCr {
constexpr uint32_t CR_TE = 0x00000001;     // Transmit enable
constexpr uint32_t CR_RE = 0x00000002;     // Receive enable
constexpr uint32_t CR_SCHED = 0x00000004;  // Schedule enable
constexpr uint32_t CR_MACRX = 0x00000008;  // MAC RX enable
constexpr uint32_t CR_MACTX = 0x00000010;  // MAC TX enable
constexpr uint32_t CR_HCI_RX = 0x00000020; // HCI RX enable
constexpr uint32_t CR_HCI_TX = 0x00000040; // HCI TX enable
} // namespace RtlCr

// ---- RTL8812AU USB Register Helpers ----

static bool rtl_read32(UsbDevice& dev, uint16_t reg, uint32_t* val) {
    return UsbHost::instance().read_register(dev, reg, val);
}

static bool rtl_write32(UsbDevice& dev, uint16_t reg, uint32_t val) {
    return UsbHost::instance().write_register(dev, reg, val);
}

static bool rtl_write8(UsbDevice& dev, uint16_t reg, uint8_t val) {
    uint32_t v32 = val;
    return UsbHost::instance().write_register(dev, reg, v32);
}

static bool rtl_read8(UsbDevice& dev, uint16_t reg, uint8_t* val) {
    uint32_t v32;
    if (!UsbHost::instance().read_register(dev, reg, &v32))
        return false;
    *val = static_cast<uint8_t>(v32 & 0xFF);
    return true;
}

// ---- Channel ↔ Frequency ----

static uint8_t freq_to_channel_8812(uint16_t freq_mhz) {
    if (freq_mhz >= 2412 && freq_mhz <= 2472) {
        if (freq_mhz == 2484)
            return 14;
        return static_cast<uint8_t>((freq_mhz - 2407 + 2) / 5);
    }
    if (freq_mhz >= 5180 && freq_mhz <= 5825) {
        return static_cast<uint8_t>((freq_mhz - 5000 + 2) / 5);
    }
    return 1;
}

// ---- RTL8812AU Monitor Driver Implementation ----

bool Rtl8812auMonitor::init() {
    LockGuard lock(device_mutex_);
    if (state_ == DriverState::Ready || state_ == DriverState::Monitor)
        return true;

    UsbHost::instance().init();
    if (!UsbHost::instance().is_initialized()) {
        state_ = DriverState::Error;
        return false;
    }

    UsbDevice dev;
    if (!UsbHost::instance().enumerate_device(dev)) {
        state_ = DriverState::Error;
        return false;
    }

    // RTL8812AU: VID=0x0BDA, PID=0x8812 (Realtek)
    // Also accept 0x881A, 0x881B (variants)
    if (dev.vendor_id != 0x0BDA || (dev.product_id != 0x8812 && dev.product_id != 0x881A && dev.product_id != 0x881B)) {
        state_ = DriverState::Error;
        return false;
    }

    // ---- Chip Reset Sequence ----

    // 1. Write system function enable: disable then enable
    rtl_write8(dev, Rtl8812auReg::REG_SYS_FUNC_EN, 0x00);
    for (volatile int d = 0; d < 10000; ++d) {
        __asm__ volatile("nop");
    }
    rtl_write8(dev, Rtl8812auReg::REG_SYS_FUNC_EN, 0xFF);

    // 2. Enable TX/RX
    uint32_t cr = RtlCr::CR_TE | RtlCr::CR_RE | RtlCr::CR_HCI_RX | RtlCr::CR_HCI_TX;
    rtl_write32(dev, Rtl8812auReg::REG_CR, cr);

    // 3. Set max RX packet length (default 2346)
    rtl_write32(dev, Rtl8812auReg::REG_RX_PKT_LEN, 2346);

    // 4. Set normal receive filter (will be changed by enter_monitor_mode)
    rtl_write32(dev, Rtl8812auReg::REG_RCR, RtlRcr::NORMAL);

    // 5. Set default channel (1 = 2412 MHz)
    rtl_write8(dev, Rtl8812auReg::REG_CHANNEL, 1);

    // 6. Read MAC from eFuse
    uint32_t mac_lo, mac_hi;
    if (rtl_read32(dev, 0x0160, &mac_lo) && rtl_read32(dev, 0x0164, &mac_hi)) {
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

bool Rtl8812auMonitor::deinit() {
    LockGuard lock(device_mutex_);
    leave_monitor_mode();

    UsbDevice dev;
    dev.vendor_id = 0x0BDA;
    dev.product_id = 0x8812;

    // Disable TX/RX engines
    if (UsbHost::instance().enumerate_device(dev)) {
        rtl_write32(dev, Rtl8812auReg::REG_CR, 0);
    }

    state_ = DriverState::Uninitialized;
    return true;
}

bool Rtl8812auMonitor::enter_monitor_mode() {
    LockGuard lock(device_mutex_);
    if (state_ == DriverState::Monitor)
        return true;
    if (state_ != DriverState::Ready)
        return false;

    UsbDevice dev;
    dev.vendor_id = 0x0BDA;
    dev.product_id = 0x8812;
    if (!UsbHost::instance().enumerate_device(dev))
        return false;

    // 1. Stop RX/TX
    rtl_write32(dev, Rtl8812auReg::REG_CR, 0);

    // 2. Set RCR to monitor mode (accept ALL frames, including CRC errors)
    rtl_write32(dev, Rtl8812auReg::REG_RCR, RtlRcr::MONITOR);

    // 3. Re-enable RX (without TX — we only capture)
    rtl_write32(dev, Rtl8812auReg::REG_CR, RtlCr::CR_RE | RtlCr::CR_HCI_RX);

    // 4. Disable BSSID filtering
    rtl_write32(dev, Rtl8812auReg::REG_RXFLTMAP0, 0xFFFF);
    rtl_write32(dev, Rtl8812auReg::REG_RXFLTMAP1, 0xFFFF);
    rtl_write32(dev, Rtl8812auReg::REG_RXFLTMAP2, 0xFFFF);

    // 5. Enable RX DMA
    rtl_write32(dev, Rtl8812auReg::REG_RXDMA_CTRL, 0x00000001);

    monitor_mode_ = true;
    state_ = DriverState::Monitor;
    return true;
}

bool Rtl8812auMonitor::leave_monitor_mode() {
    LockGuard lock(device_mutex_);
    if (!monitor_mode_)
        return true;

    UsbDevice dev;
    dev.vendor_id = 0x0BDA;
    dev.product_id = 0x8812;

    if (UsbHost::instance().enumerate_device(dev)) {
        rtl_write32(dev, Rtl8812auReg::REG_CR, 0);
        rtl_write32(dev, Rtl8812auReg::REG_RCR, RtlRcr::NORMAL);
        rtl_write32(dev, Rtl8812auReg::REG_CR, RtlCr::CR_TE | RtlCr::CR_RE | RtlCr::CR_HCI_RX | RtlCr::CR_HCI_TX);
    }

    monitor_mode_ = false;
    state_ = DriverState::Ready;
    return true;
}

bool Rtl8812auMonitor::is_monitor_mode() const {
    return monitor_mode_;
}

bool Rtl8812auMonitor::set_channel(uint16_t frequency_mhz) {
    LockGuard lock(device_mutex_);
    if (state_ == DriverState::Uninitialized || state_ == DriverState::Error)
        return false;

    UsbDevice dev;
    dev.vendor_id = 0x0BDA;
    dev.product_id = 0x8812;
    if (!UsbHost::instance().enumerate_device(dev))
        return false;

    uint8_t ch = freq_to_channel_8812(frequency_mhz);
    bool is_5ghz = (frequency_mhz >= 5000);

    // 1. Stop RX
    rtl_write32(dev, Rtl8812auReg::REG_CR, 0);

    // 2. Write channel number
    rtl_write8(dev, Rtl8812auReg::REG_CHANNEL, ch);

    // 3. Configure RF for band
    if (is_5ghz) {
        // Switch to 5GHz RF path
        rtl_write8(dev, Rtl8812auReg::REG_RF_CTRL, 0x01);
        rtl_write8(dev, Rtl8812auReg::REG_AFE_XTAL_CTRL, 0x80);
    } else {
        // Switch to 2.4GHz RF path
        rtl_write8(dev, Rtl8812auReg::REG_RF_CTRL, 0x00);
        rtl_write8(dev, Rtl8812auReg::REG_AFE_XTAL_CTRL, 0x00);
    }

    // 4. Re-enable RX
    uint32_t cr = monitor_mode_ ? (RtlCr::CR_RE | RtlCr::CR_HCI_RX)
                                : (RtlCr::CR_TE | RtlCr::CR_RE | RtlCr::CR_HCI_RX | RtlCr::CR_HCI_TX);
    rtl_write32(dev, Rtl8812auReg::REG_CR, cr);

    current_channel_.frequency_mhz = frequency_mhz;
    current_channel_.channel_number = ch;
    current_channel_.band = is_5ghz ? 1 : 0;

    for (volatile int d = 0; d < 10000; ++d) {
        __asm__ volatile("nop");
    }
    return true;
}

uint16_t Rtl8812auMonitor::get_channel() const {
    return current_channel_.frequency_mhz;
}

int Rtl8812auMonitor::capture_frame(CapturedFrame& out_frame) {
    LockGuard lock(device_mutex_);
    if (state_ != DriverState::Monitor)
        return -1;

    UsbDevice dev;
    dev.vendor_id = 0x0BDA;
    dev.product_id = 0x8812;
    dev.address = 1;

    // USB bulk IN from EP_BULK_IN (0x83)
    uint8_t buf[32768]; // USB 3.0 buffer
    UsbTransferResult r = UsbHost::instance().bulk_in(dev, 3, buf, sizeof(buf));
    if (!r.success || r.bytes_transferred <= 0)
        return 0;

    int rx_len = r.bytes_transferred;

    // RTL8812AU: RX descriptor is 24 bytes, then the 802.11 frame
    // RX desc offset 0:  dword0 (frame length in bits 0-13)
    // RX desc offset 8:  RSSI (byte)
    // RX desc offset 12: channel (word)
    static constexpr int RX_DESC_SIZE = 24;
    if (rx_len < RX_DESC_SIZE + 14)
        return 0; // min: desc + 802.11 header

    uint32_t dword0 = buf[0] | (static_cast<uint32_t>(buf[1]) << 8) | (static_cast<uint32_t>(buf[2]) << 16) |
                      (static_cast<uint32_t>(buf[3]) << 24);
    uint16_t frame_len = static_cast<uint16_t>(dword0 & 0x3FFF);

    if (frame_len == 0 || frame_len > 2346 || RX_DESC_SIZE + frame_len > static_cast<uint16_t>(rx_len)) {
        return 0;
    }

    int8_t rssi = static_cast<int8_t>(buf[8]);
    uint16_t freq = buf[12] | (static_cast<uint16_t>(buf[13]) << 8);

    out_frame.timestamp_sec = tick_count / 1000;
    out_frame.timestamp_usec = (tick_count % 1000) * 1000;
    out_frame.rssi_dbm = rssi;
    out_frame.noise_dbm = 0; // not reported by RTL8812AU
    out_frame.channel_freq = freq;
    out_frame.frame_len = frame_len;

    for (int i = 0; i < frame_len; ++i)
        out_frame.data[i] = buf[RX_DESC_SIZE + i];

    return static_cast<int>(frame_len);
}

bool Rtl8812auMonitor::inject_frame(const uint8_t* frame, int len) {
    LockGuard lock(device_mutex_);
    if (state_ != DriverState::Monitor || len <= 0 || len > 2346)
        return false;

    UsbDevice dev;
    dev.vendor_id = 0x0BDA;
    dev.product_id = 0x8812;
    dev.address = 1;

    // RTL8812AU TX descriptor: 8 bytes (simplified)
    uint8_t tx_buf[2360];
    tx_buf[0] = static_cast<uint8_t>(len & 0xFF);        // pkt_len low
    tx_buf[1] = static_cast<uint8_t>((len >> 8) & 0xFF); // pkt_len high
    tx_buf[2] = 0x00;                                    // offset
    tx_buf[3] = 0x00;                                    // TX desc flags 0
    tx_buf[4] = 0x00;                                    // TX desc flags 1
    tx_buf[5] = 0x00;                                    // TX desc flags 2
    tx_buf[6] = 0x00;                                    // reserved
    tx_buf[7] = 0x00;                                    // reserved

    for (int i = 0; i < len; ++i)
        tx_buf[8 + i] = frame[i];

    UsbTransferResult r = UsbHost::instance().bulk_out(dev, 2, tx_buf, len + 8);
    return r.success;
}

// ---- Firmware Upload ----

bool Rtl8812auMonitor::upload_firmware(const uint8_t* fw_data, size_t fw_size) {
    LockGuard lock(device_mutex_);
    if (!fw_data || fw_size == 0)
        return false;
    if (state_ == DriverState::Uninitialized || state_ == DriverState::Error)
        return false;

    UsbDevice dev;
    dev.vendor_id = 0x0BDA;
    dev.product_id = 0x8812;
    if (!UsbHost::instance().enumerate_device(dev))
        return false;

    // 1. Stop RX/TX and CPU
    rtl_write32(dev, Rtl8812auReg::REG_CR, 0);

    // 2. Put MCU into reset (write 0 to FW DL control)
    rtl_write32(dev, Rtl8812auReg::REG_MCU_FW_DL, 0x00000000);
    for (volatile int d = 0; d < 5000; ++d) {
        __asm__ volatile("nop");
    }

    // 3. Set firmware start address
    rtl_write32(dev, Rtl8812auReg::REG_FW_START_ADDR, 0x00000000);

    // 4. Upload firmware in chunks via USB bulk OUT (EP 0x02)
    constexpr size_t CHUNK = 512;
    for (size_t offset = 0; offset < fw_size; offset += CHUNK) {
        size_t chunk = (offset + CHUNK <= fw_size) ? CHUNK : fw_size - offset;
        UsbTransferResult r = UsbHost::instance().bulk_out(dev, 2, fw_data + offset, static_cast<int>(chunk));
        if (!r.success)
            return false;
    }

    // 5. Release MCU from reset
    rtl_write32(dev, Rtl8812auReg::REG_MCU_FW_DL, 0x00000001);
    for (volatile int d = 0; d < 10000; ++d) {
        __asm__ volatile("nop");
    }

    // 6. Verify MCU is running (check HIMR for interrupt)
    uint32_t himr;
    if (!rtl_read32(dev, Rtl8812auReg::REG_HIMR, &himr))
        return false;
    // Bit 0 = MCU ready interrupt
    if (!(himr & 0x00000001))
        return false;

    return true;
}

bool Rtl8812auMonitor::set_tx_power(uint8_t dbm) {
    LockGuard lock(device_mutex_);
    if (state_ == DriverState::Uninitialized || state_ == DriverState::Error)
        return false;

    UsbDevice dev;
    dev.vendor_id = 0x0BDA;
    dev.product_id = 0x8812;
    if (!UsbHost::instance().enumerate_device(dev))
        return false;

    // TX power is set through BB register
    // BB_REG_TX_GAIN_CTRL = 0xC50, value = (dbm << 1)
    uint8_t gain = dbm * 2;
    return rtl_write8(dev, 0x0C50, gain);
}
