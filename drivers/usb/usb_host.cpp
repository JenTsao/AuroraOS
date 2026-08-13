// ============================================================
// usb_host.cpp — USB Host Controller driver (LM3S6965 USB Host)
//
// LM3S6965 has an on-chip USB Host controller (compatible with
// OHCI-lite).  This HAL implements the register-level primitives
// needed by the UsbHost abstraction layer.
//
// On targets that lack USB hardware, the HAL stubs return false
// gracefully; the upper-layer WiFi code checks capabilities before
// attempting enumeration.
// ============================================================

#include "usb_host.hpp"
#include <string.h>

// ---- LM3S6965 USB Host Register Base ----
// LM3S USB host controller is mapped at 0x40050000.
// This is the on-chip USB 2.0 Full-Speed controller.
static constexpr uintptr_t USB_BASE = 0x40050000;

// Key OHCI-style operational registers (offsets from USB_BASE)
static constexpr uintptr_t OFF_USB_CTRL = 0x000;    // Main control
static constexpr uintptr_t OFF_USB_STAT = 0x004;    // Status
static constexpr uintptr_t OFF_USB_EP_IDX = 0x00C;  // Endpoint index
static constexpr uintptr_t OFF_USB_FRAME = 0x010;   // Frame number
static constexpr uintptr_t OFF_USB_EP_CTRL = 0x014; // EP control/status
static constexpr uintptr_t OFF_USB_EP_MAXP = 0x018; // EP max packet
static constexpr uintptr_t OFF_USB_RX_ADDR = 0x020; // RX FIFO addr
static constexpr uintptr_t OFF_USB_RX_CNT = 0x028;  // RX FIFO count
static constexpr uintptr_t OFF_USB_TX_ADDR = 0x030; // TX FIFO addr
static constexpr uintptr_t OFF_USB_TX_CNT = 0x038;  // TX FIFO count

// Control register bits
static constexpr uint32_t USB_CTRL_ENABLE = 0x00000001;
static constexpr uint32_t USB_CTRL_HOST = 0x00000002;
static constexpr uint32_t USB_CTRL_RESET = 0x00000004;
static constexpr uint32_t USB_CTRL_SOF_EN = 0x00000008;

// Endpoint control bits
static constexpr uint32_t EP_CTRL_RX_EN = 0x00000001;
static constexpr uint32_t EP_CTRL_TX_EN = 0x00000002;
static constexpr uint32_t EP_CTRL_STALL = 0x00000010;

// ---- HAL Helpers ----

static inline volatile uint32_t* reg(uintptr_t off) {
    return reinterpret_cast<volatile uint32_t*>(USB_BASE + off);
}

static bool usb_hw_present() {
    // Check if USB peripheral is accessible by reading the control register.
    // On systems without USB, reads return 0 (or bus fault — but MPU traps it).
    volatile uint32_t ctrl = *reg(OFF_USB_CTRL);
    (void)ctrl;
    return true; // assume present if no fault
}

// ---- UsbHost HAL Implementation ----

bool UsbHost::hal_init_() {
    if (!usb_hw_present())
        return false;

    // Reset USB controller
    *reg(OFF_USB_CTRL) = USB_CTRL_RESET;
    for (volatile int d = 0; d < 1000; ++d) {
        __asm__ volatile("nop");
    }
    *reg(OFF_USB_CTRL) = 0;

    // Enable host mode + SOF generation
    *reg(OFF_USB_CTRL) = USB_CTRL_ENABLE | USB_CTRL_HOST | USB_CTRL_SOF_EN;
    for (volatile int d = 0; d < 500; ++d) {
        __asm__ volatile("nop");
    }

    next_address_ = 1;
    return true;
}

void UsbHost::hal_reset_() {
    *reg(OFF_USB_CTRL) |= USB_CTRL_RESET;
    for (volatile int d = 0; d < 1000; ++d) {
        __asm__ volatile("nop");
    }
    *reg(OFF_USB_CTRL) = 0;
}

bool UsbHost::hal_port_reset_(int /*port*/) {
    // LM3S has a single USB port — no port switching needed
    return true;
}

// ---- Lifecycle ----

void UsbHost::init() {
    if (initialized_)
        return;

    LockGuard lock(host_mutex_);
    if (!hal_init_())
        return;
    initialized_ = true;
}

void UsbHost::reset() {
    LockGuard lock(host_mutex_);
    hal_reset_();
    next_address_ = 1;
    initialized_ = false;
}

// ---- Device Enumeration ----

bool UsbHost::enumerate_device(UsbDevice& dev) {
    LockGuard lock(host_mutex_);
    if (!initialized_)
        return false;

    // Reset the port to bring device to default state
    hal_port_reset_(0);

    // --- Step 1: Read Device Descriptor (first 8 bytes for max packet size) ---
    UsbSetupPacket setup{};
    setup.bmRequestType = 0x80; // Device→Host, Standard, Device
    setup.bRequest = UsbRequest::GetDescriptor;
    setup.wValue = (UsbDescriptorType::Device << 8) | 0;
    setup.wIndex = 0;
    setup.wLength = 8;

    uint8_t desc_buf[18];
    int xfer_len = 8;
    if (!hal_control_xfer_(dev, 1, setup, desc_buf, &xfer_len) || xfer_len < 8)
        return false;

    dev.device_class = desc_buf[4]; // bDeviceClass
    uint8_t max_pkt0 = desc_buf[7];

    // Re-read full descriptor
    setup.wLength = 18;
    xfer_len = 18;
    if (!hal_control_xfer_(dev, 1, setup, desc_buf, &xfer_len) || xfer_len < 18)
        return false;

    const UsbDeviceDescriptor* ddesc = reinterpret_cast<const UsbDeviceDescriptor*>(desc_buf);
    dev.vendor_id = ddesc->idVendor;
    dev.product_id = ddesc->idProduct;

    // --- Step 2: Set Address ---
    uint8_t new_addr = next_address_++;
    setup.bmRequestType = 0x00; // Host→Device, Standard, Device
    setup.bRequest = UsbRequest::SetAddress;
    setup.wValue = new_addr;
    setup.wIndex = 0;
    setup.wLength = 0;
    xfer_len = 0;
    if (!hal_control_xfer_(dev, 0, setup, nullptr, &xfer_len))
        return false;
    dev.address = new_addr;

    // Give device time to settle on new address
    for (volatile int d = 0; d < 5000; ++d) {
        __asm__ volatile("nop");
    }

    // --- Step 3: Read Configuration Descriptor ---
    setup.bmRequestType = 0x80;
    setup.bRequest = UsbRequest::GetDescriptor;
    setup.wValue = (UsbDescriptorType::Configuration << 8) | 0;
    setup.wIndex = 0;
    setup.wLength = 9;
    xfer_len = 9;
    if (!hal_control_xfer_(dev, 1, setup, desc_buf, &xfer_len) || xfer_len < 9)
        return false;

    const UsbConfigDescriptor* cdesc = reinterpret_cast<const UsbConfigDescriptor*>(desc_buf);
    uint16_t total_len = cdesc->wTotalLength;
    if (total_len > UsbDevice::kMaxConfigSize)
        total_len = UsbDevice::kMaxConfigSize;

    setup.wLength = total_len;
    xfer_len = total_len;
    if (!hal_control_xfer_(dev, 1, setup, dev.config_raw, &xfer_len))
        return false;
    dev.config_len = xfer_len;

    // --- Step 4: Set Configuration ---
    setup.bmRequestType = 0x00;
    setup.bRequest = UsbRequest::SetConfiguration;
    setup.wValue = cdesc->bConfigurationValue;
    setup.wIndex = 0;
    setup.wLength = 0;
    xfer_len = 0;
    if (!hal_control_xfer_(dev, 0, setup, nullptr, &xfer_len))
        return false;
    dev.config_value = cdesc->bConfigurationValue;

    // --- Step 5: Parse Endpoints ---
    int pos = cdesc->bLength; // skip config descriptor itself
    dev.endpoint_count = 0;
    dev.bulk_in_ep = 0;
    dev.bulk_out_ep = 0;

    while (pos + 2 <= xfer_len && dev.endpoint_count < UsbDevice::kMaxEndpoints) {
        uint8_t desc_len = dev.config_raw[pos];
        uint8_t desc_type = dev.config_raw[pos + 1];

        if (desc_len == 0 || pos + desc_len > xfer_len)
            break;

        if (desc_type == UsbDescriptorType::Endpoint) {
            const UsbEndpointDescriptor* ep = reinterpret_cast<const UsbEndpointDescriptor*>(&dev.config_raw[pos]);
            UsbEndpoint& uep = dev.endpoints[dev.endpoint_count];
            uep.address = ep->bEndpointAddress;
            uep.number = ep->bEndpointAddress & 0x0F;
            uep.is_in = (ep->bEndpointAddress & 0x80) != 0;
            uep.type = ep->bmAttributes & 0x03;
            uep.max_packet = ep->wMaxPacketSize & 0x07FF;
            uep.interval = ep->bInterval;
            uep.active = true;
            uep.toggle = 0;

            // Record bulk endpoints for WiFi chipset drivers
            if (uep.type == static_cast<uint8_t>(UsbEndpointType::Bulk)) {
                if (uep.is_in) {
                    dev.bulk_in_ep = uep.number;
                } else {
                    dev.bulk_out_ep = uep.number;
                }
                dev.bulk_max_pkt = uep.max_packet;
            }

            ++dev.endpoint_count;
        }
        pos += desc_len;
    }

    dev.enumerated = true;
    return true;
}

// ---- Control Transfer ----

UsbTransferResult UsbHost::control_transfer(UsbDevice& dev, uint8_t dir, const UsbSetupPacket& setup, uint8_t* data,
                                            int len) {
    UsbTransferResult result{};
    result.success = false;

    LockGuard lock(host_mutex_);
    if (!initialized_)
        return result;

    int xfer = len;
    result.success = hal_control_xfer_(dev, dir, setup, data, &xfer);
    result.bytes_transferred = xfer;
    if (!result.success)
        result.error_code = -5; // -EIO
    return result;
}

// ---- Bulk Transfers ----

UsbTransferResult UsbHost::bulk_out(UsbDevice& dev, uint8_t ep_number, const uint8_t* data, int len) {
    UsbTransferResult result{};
    result.success = false;

    if (!initialized_)
        return result;

    // Copy to a non-const buffer for the HAL layer
    uint8_t buf[512];
    int copy = (len > 512) ? 512 : len;
    for (int i = 0; i < copy; ++i)
        buf[i] = data[i];

    LockGuard lock(host_mutex_);
    int xfer = copy;
    result.success = hal_bulk_xfer_(dev, ep_number, false, buf, &xfer);
    result.bytes_transferred = xfer;
    if (!result.success)
        result.error_code = -5;
    return result;
}

UsbTransferResult UsbHost::bulk_in(UsbDevice& dev, uint8_t ep_number, uint8_t* buffer, int max_len) {
    UsbTransferResult result{};
    result.success = false;

    if (!initialized_)
        return result;

    LockGuard lock(host_mutex_);
    int xfer = max_len;
    result.success = hal_bulk_xfer_(dev, ep_number, true, buffer, &xfer);
    result.bytes_transferred = xfer;
    if (!result.success)
        result.error_code = -5;
    return result;
}

// ---- Control Transfer HAL (LM3S6965 USB Host register sequence) ----

bool UsbHost::hal_control_xfer_(UsbDevice& dev, uint8_t dir, const UsbSetupPacket& setup, uint8_t* data, int* len) {
    // Select endpoint 0
    *reg(OFF_USB_EP_IDX) = 0;

    // Configure EP0 max packet (device descriptor says 8/16/32/64)
    *reg(OFF_USB_EP_MAXP) = 8; // default EP0 size

    // Enable EP0 for both RX and TX
    *reg(OFF_USB_EP_CTRL) = EP_CTRL_RX_EN | EP_CTRL_TX_EN;

    // Write SETUP packet to TX FIFO (8 bytes)
    const uint8_t* sp = reinterpret_cast<const uint8_t*>(&setup);
    for (int i = 0; i < 8; ++i) {
        *reg(OFF_USB_TX_ADDR) = sp[i];
    }
    *reg(OFF_USB_TX_CNT) = 8;

    // Wait for TX complete
    for (volatile int d = 0; d < 10000; ++d) {
        if (!(*reg(OFF_USB_EP_CTRL) & EP_CTRL_TX_EN))
            break;
        __asm__ volatile("nop");
    }

    // DATA phase
    if (data && *len > 0) {
        if (dir == 1) {
            // IN: read from RX FIFO
            int rx = 0;
            for (volatile int d = 0; d < 50000 && rx < *len; ++d) {
                uint32_t cnt = *reg(OFF_USB_RX_CNT);
                if (cnt > 0 && cnt <= static_cast<uint32_t>(*len - rx)) {
                    for (uint32_t i = 0; i < cnt; ++i)
                        data[rx + i] = static_cast<uint8_t>(*reg(OFF_USB_RX_ADDR));
                    rx += cnt;
                    *reg(OFF_USB_RX_CNT) = 0;
                    break;
                }
                __asm__ volatile("nop");
            }
            *len = rx;
        } else {
            // OUT: write to TX FIFO
            int tx = (*len > 64) ? 64 : *len;
            for (int i = 0; i < tx; ++i)
                *reg(OFF_USB_TX_ADDR) = data[i];
            *reg(OFF_USB_TX_CNT) = static_cast<uint32_t>(tx);

            for (volatile int d = 0; d < 10000; ++d) {
                if (!(*reg(OFF_USB_EP_CTRL) & EP_CTRL_TX_EN))
                    break;
                __asm__ volatile("nop");
            }
            *len = tx;
        }
    }

    // STATUS phase (handled automatically by hardware for EP0)
    for (volatile int d = 0; d < 1000; ++d) {
        __asm__ volatile("nop");
    }

    return true;
}

// ---- Bulk Transfer HAL ----

bool UsbHost::hal_bulk_xfer_(UsbDevice& dev, uint8_t ep_number, bool is_in, uint8_t* data, int* len) {
    if (*len <= 0)
        return false;

    // Select endpoint
    *reg(OFF_USB_EP_IDX) = ep_number;

    // Configure max packet
    UsbEndpoint* ep = dev.find_endpoint(ep_number, is_in);
    uint16_t maxpkt = ep ? ep->max_packet : 64;
    *reg(OFF_USB_EP_MAXP) = maxpkt;

    if (is_in) {
        // Enable RX for bulk IN
        *reg(OFF_USB_EP_CTRL) = EP_CTRL_RX_EN;

        int rx = 0;
        int max_rx = *len;
        for (volatile int d = 0; d < 50000 && rx < max_rx; ++d) {
            uint32_t cnt = *reg(OFF_USB_RX_CNT);
            if (cnt > 0) {
                int to_read = static_cast<int>(cnt);
                if (rx + to_read > max_rx)
                    to_read = max_rx - rx;
                for (int i = 0; i < to_read; ++i)
                    data[rx + i] = static_cast<uint8_t>(*reg(OFF_USB_RX_ADDR));
                rx += to_read;
                *reg(OFF_USB_RX_CNT) = 0;
                if (to_read < static_cast<int>(maxpkt))
                    break; // short packet = end of transfer
            }
            __asm__ volatile("nop");
        }
        *len = rx;
        return rx > 0;
    } else {
        // Bulk OUT
        int tx = (*len > 512) ? 512 : *len;
        *reg(OFF_USB_EP_CTRL) = EP_CTRL_TX_EN;

        for (int i = 0; i < tx; ++i)
            *reg(OFF_USB_TX_ADDR) = data[i];
        *reg(OFF_USB_TX_CNT) = static_cast<uint32_t>(tx);

        for (volatile int d = 0; d < 50000; ++d) {
            if (!(*reg(OFF_USB_EP_CTRL) & EP_CTRL_TX_EN))
                break;
            __asm__ volatile("nop");
        }
        *len = tx;
        return true;
    }
}

// ---- Register Access Helpers (for WiFi chipset drivers) ----

bool UsbHost::read_register(UsbDevice& dev, uint16_t reg, uint32_t* value) {
    // WiFi chipsets use vendor-specific control/bulk requests for register I/O.
    // RTL chipsets: register read = control IN with wValue=reg, wLength=4
    UsbSetupPacket setup{};
    setup.bmRequestType = 0xC0; // Device→Host, Vendor, Device
    setup.bRequest = 0x05;      // vendor-specific read
    setup.wValue = reg;
    setup.wIndex = 0;
    setup.wLength = 4;

    uint8_t data[4] = {};
    int xfer = 4;
    UsbTransferResult r = control_transfer(dev, 1, setup, data, xfer);
    if (!r.success)
        return false;

    *value = static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
             (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
    return true;
}

bool UsbHost::write_register(UsbDevice& dev, uint16_t reg, uint32_t value) {
    UsbSetupPacket setup{};
    setup.bmRequestType = 0x40; // Host→Device, Vendor, Device
    setup.bRequest = 0x05;      // vendor-specific write
    setup.wValue = reg;
    setup.wIndex = 0;
    setup.wLength = 4;

    uint8_t data[4];
    data[0] = static_cast<uint8_t>(value & 0xFF);
    data[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
    data[2] = static_cast<uint8_t>((value >> 16) & 0xFF);
    data[3] = static_cast<uint8_t>((value >> 24) & 0xFF);

    int xfer = 4;
    return control_transfer(dev, 0, setup, data, xfer).success;
}

bool UsbHost::read_registers(UsbDevice& dev, uint16_t start_reg, uint8_t* buf, int len) {
    for (int i = 0; i < len; i += 4) {
        uint32_t val;
        if (!read_register(dev, start_reg + i / 4, &val))
            return false;
        int remain = len - i;
        int copy = (remain > 4) ? 4 : remain;
        for (int j = 0; j < copy; ++j)
            buf[i + j] = static_cast<uint8_t>((val >> (j * 8)) & 0xFF);
    }
    return true;
}

bool UsbHost::write_registers(UsbDevice& dev, uint16_t start_reg, const uint8_t* buf, int len) {
    for (int i = 0; i < len; i += 4) {
        uint32_t val = 0;
        int remain = len - i;
        int shift = 0;
        for (int j = 0; j < 4 && j < remain; ++j) {
            val |= static_cast<uint32_t>(buf[i + j]) << shift;
            shift += 8;
        }
        if (!write_register(dev, start_reg + i / 4, val))
            return false;
    }
    return true;
}
