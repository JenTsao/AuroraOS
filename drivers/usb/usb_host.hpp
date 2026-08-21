#ifndef AURORA_USB_HOST_HPP
#define AURORA_USB_HOST_HPP

#include <stdint.h>
#include <stddef.h>
#include "usb_core.hpp"
#include "../../kernel/core/mutex.hpp"

// ============================================================
// USB Host Controller Driver Framework
//
// Modeled after OHCI/EHCI-lite for LM3S6965 (which has a USB
// Host controller on-chip). Designed to be portable to other
// Cortex-M / RISC-V SoCs with minimal changes.
//
// Layers:
//   UsbDevice  → per-device state (endpoints, descriptors)
//   UsbHost    → controller-level enumeration + transfer engine
//   UsbHal     → hardware-specific register I/O
// ============================================================

// ---- USB Standard Descriptors ----

struct __attribute__((packed)) UsbDeviceDescriptor {
    uint8_t bLength;
    uint8_t bDescriptorType; // 1 = Device
    uint16_t bcdUSB;
    uint8_t bDeviceClass;
    uint8_t bDeviceSubClass;
    uint8_t bDeviceProtocol;
    uint8_t bMaxPacketSize0;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t iManufacturer;
    uint8_t iProduct;
    uint8_t iSerialNumber;
    uint8_t bNumConfigurations;
};

struct __attribute__((packed)) UsbConfigDescriptor {
    uint8_t bLength;
    uint8_t bDescriptorType; // 2 = Configuration
    uint16_t wTotalLength;
    uint8_t bNumInterfaces;
    uint8_t bConfigurationValue;
    uint8_t iConfiguration;
    uint8_t bmAttributes;
    uint8_t bMaxPower; // in 2mA units
};

struct __attribute__((packed)) UsbInterfaceDescriptor {
    uint8_t bLength;
    uint8_t bDescriptorType; // 4 = Interface
    uint8_t bInterfaceNumber;
    uint8_t bAlternateSetting;
    uint8_t bNumEndpoints;
    uint8_t bInterfaceClass;
    uint8_t bInterfaceSubClass;
    uint8_t bInterfaceProtocol;
    uint8_t iInterface;
};

struct __attribute__((packed)) UsbEndpointDescriptor {
    uint8_t bLength;
    uint8_t bDescriptorType;  // 5 = Endpoint
    uint8_t bEndpointAddress; // bit 7=IN, bits 0-3=number
    uint8_t bmAttributes;     // bits 0-1: 0=Control, 1=Isoch, 2=Bulk, 3=Int
    uint16_t wMaxPacketSize;  // bits 0-10=size, bits 11-12=additional transactions
    uint8_t bInterval;        // polling interval (int/isoch)
};

// ---- USB Setup Packet ----

struct __attribute__((packed)) UsbSetupPacket {
    uint8_t bmRequestType;
    uint8_t bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
};

// Standard request codes
namespace UsbRequest {
constexpr uint8_t GetStatus = 0;
constexpr uint8_t ClearFeature = 1;
constexpr uint8_t SetFeature = 3;
constexpr uint8_t SetAddress = 5;
constexpr uint8_t GetDescriptor = 6;
constexpr uint8_t SetDescriptor = 7;
constexpr uint8_t GetConfiguration = 8;
constexpr uint8_t SetConfiguration = 9;
constexpr uint8_t GetInterface = 10;
constexpr uint8_t SetInterface = 11;
} // namespace UsbRequest

// Standard descriptor types
namespace UsbDescriptorType {
constexpr uint8_t Device = 1;
constexpr uint8_t Configuration = 2;
constexpr uint8_t String = 3;
constexpr uint8_t Interface = 4;
constexpr uint8_t Endpoint = 5;
} // namespace UsbDescriptorType

// Standard USB Device Class Codes
namespace UsbClass {
constexpr uint8_t UseInterface = 0x00;
constexpr uint8_t Audio = 0x01;
constexpr uint8_t CdcControl = 0x02;
constexpr uint8_t Hid = 0x03;
constexpr uint8_t Physical = 0x05;
constexpr uint8_t Image = 0x06;
constexpr uint8_t Printer = 0x07;
constexpr uint8_t MassStorage = 0x08;
constexpr uint8_t Hub = 0x09;
constexpr uint8_t CdcData = 0x0A;
constexpr uint8_t SmartCard = 0x0B;
constexpr uint8_t Video = 0x0E;
constexpr uint8_t WirelessController = 0xE0;
constexpr uint8_t Miscellaneous = 0xEF;
constexpr uint8_t ApplicationSpecific = 0xFE;
constexpr uint8_t VendorSpecific = 0xFF;
} // namespace UsbClass

// ---- Transfer Result ----

struct UsbTransferResult {
    bool success;
    int32_t bytes_transferred;
    int32_t error_code; // 0=ok, -EBUSY, -EIO, -ETIMEDOUT
};

// ---- Per-Endpoint State ----

struct UsbEndpoint {
    uint8_t address; // raw bEndpointAddress
    uint8_t number;  // endpoint number (0-15)
    bool is_in;      // true = IN (device→host), false = OUT
    uint8_t type;    // UsbEndpointType (0=Ctrl, 1=Isoch, 2=Bulk, 3=Int)
    uint16_t max_packet;
    uint8_t interval; // polling interval in frames (int/isoch)
    bool active;
    uint8_t toggle; // data toggle bit (Bulk/Int)
};

enum class UsbEndpointType : uint8_t {
    Control = 0,
    Isochronous = 1,
    Bulk = 2,
    Interrupt = 3,
};

// ---- Per-Device State ----

class UsbDevice {
public:
    static constexpr int kMaxEndpoints = 8;
    static constexpr int kMaxConfigSize = 256;

    uint8_t address; // assigned USB address (1-127)
    uint16_t vendor_id;
    uint16_t product_id;
    uint8_t device_class;
    uint8_t config_value;

    UsbEndpoint endpoints[kMaxEndpoints];
    int endpoint_count = 0;

    uint8_t config_raw[kMaxConfigSize];
    int config_len = 0;

    // Populated during enumeration
    uint8_t bulk_in_ep = 0;     // endpoint number for bulk IN
    uint8_t bulk_out_ep = 0;    // endpoint number for bulk OUT
    uint16_t bulk_max_pkt = 64; // max packet size for bulk
    bool enumerated = false;

    UsbDevice() : address(0), vendor_id(0), product_id(0), device_class(0), config_value(0), endpoints{},
                  config_raw{} {}

    UsbEndpoint* find_endpoint(uint8_t number, bool is_in) {
        for (int i = 0; i < endpoint_count; ++i) {
            if (endpoints[i].number == number && endpoints[i].is_in == is_in)
                return &endpoints[i];
        }
        return nullptr;
    }
};

// ---- USB Host Controller Driver ----

class UsbHost : public auroraos::usb::UsbHostController {
public:
    static UsbHost& instance() {
        static UsbHost host;
        return host;
    }

    // ---- UsbHostController Core API ----
    bool init() override;
    bool submit_transfer(auroraos::usb::UsbTransfer* transfer) override;

    // ---- Lifecycle ----
    void reset();

    // ---- Device Enumeration & Descriptors ----
    bool enumerate_device(UsbDevice& dev);
    bool reset_port(int port);

    // String descriptor decoding (UTF-16LE to ASCII/UTF-8)
    bool get_string_descriptor(UsbDevice& dev, uint8_t index, char* out_str, size_t max_len);

    // Endpoint stall / feature management
    bool clear_endpoint_stall(UsbDevice& dev, uint8_t ep_number, bool is_in);
    bool clear_feature(UsbDevice& dev, uint8_t recipient, uint16_t feature, uint16_t index);

    // Class name lookup
    static const char* get_class_name(uint8_t device_class);

    // ---- Control Transfers ----
    UsbTransferResult control_transfer(UsbDevice& dev, uint8_t dir, const UsbSetupPacket& setup, uint8_t* data,
                                       int len);

    // ---- Bulk Transfers (Zero-Copy & Multi-Packet Streaming) ----
    UsbTransferResult bulk_out(UsbDevice& dev, uint8_t ep_number, const uint8_t* data, int len);
    UsbTransferResult bulk_in(UsbDevice& dev, uint8_t ep_number, uint8_t* buffer, int max_len);

    // ---- Register Access Helpers (for WiFi chipset drivers & peripherals) ----
    bool read_register(UsbDevice& dev, uint16_t reg, uint32_t* value);
    bool write_register(UsbDevice& dev, uint16_t reg, uint32_t value);
    bool read_registers(UsbDevice& dev, uint16_t start_reg, uint8_t* buf, int len);
    bool write_registers(UsbDevice& dev, uint16_t start_reg, const uint8_t* buf, int len);

    // Typed Register Access Primitives
    bool read_reg8(UsbDevice& dev, uint16_t reg, uint8_t* val);
    bool write_reg8(UsbDevice& dev, uint16_t reg, uint8_t val);
    bool read_reg16(UsbDevice& dev, uint16_t reg, uint16_t* val);
    bool write_reg16(UsbDevice& dev, uint16_t reg, uint16_t val);
    bool read_reg32(UsbDevice& dev, uint16_t reg, uint32_t* val);
    bool write_reg32(UsbDevice& dev, uint16_t reg, uint32_t val);

    // ---- Status ----
    bool is_initialized() const {
        return initialized_;
    }

private:
    UsbHost() = default;
    bool initialized_ = false;
    uint8_t next_address_ = 1;

    mutable Mutex host_mutex_;

    // Platform-specific HAL
    bool hal_init_();
    void hal_reset_();
    bool hal_port_reset_(int port);
    bool hal_control_xfer_(UsbDevice& dev, uint8_t dir, const UsbSetupPacket& setup, uint8_t* data, int* len);
    bool hal_bulk_xfer_(UsbDevice& dev, uint8_t ep_number, bool is_in, uint8_t* data, int* len);
};

#endif // AURORA_USB_HOST_HPP

