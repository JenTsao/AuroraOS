#include "../../drivers/usb/usb_host.hpp"
#include <string.h>

bool UsbHost::init() {
    initialized_ = true;
    return true;
}

void UsbHost::reset() {
    initialized_ = false;
}

bool UsbHost::submit_transfer(auroraos::usb::UsbTransfer* transfer) {
    if (!transfer)
        return false;
    transfer->actual_length = transfer->length;
    transfer->completed = true;
    if (transfer->callback) {
        transfer->callback(transfer, transfer->user_data);
    }
    return true;
}

bool UsbHost::enumerate_device(UsbDevice& dev) {
    dev.enumerated = true;
    return true;
}

bool UsbHost::reset_port(int port) {
    (void)port;
    return true;
}

bool UsbHost::get_string_descriptor(UsbDevice& dev, uint8_t index, char* out_str, size_t max_len) {
    (void)dev;
    (void)index;
    if (out_str && max_len > 0) {
        out_str[0] = '\0';
    }
    return true;
}

bool UsbHost::clear_endpoint_stall(UsbDevice& dev, uint8_t ep_number, bool is_in) {
    (void)dev;
    (void)ep_number;
    (void)is_in;
    return true;
}

bool UsbHost::clear_feature(UsbDevice& dev, uint8_t recipient, uint16_t feature, uint16_t index) {
    (void)dev;
    (void)recipient;
    (void)feature;
    (void)index;
    return true;
}

const char* UsbHost::get_class_name(uint8_t device_class) {
    switch (device_class) {
    case UsbClass::UseInterface: return "Interface Defined";
    case UsbClass::Audio: return "Audio";
    case UsbClass::CdcControl: return "CDC Control";
    case UsbClass::Hid: return "Human Interface Device";
    case UsbClass::Physical: return "Physical";
    case UsbClass::Image: return "Imaging Device";
    case UsbClass::Printer: return "Printer";
    case UsbClass::MassStorage: return "Mass Storage";
    case UsbClass::Hub: return "USB Hub";
    case UsbClass::CdcData: return "CDC Data";
    case UsbClass::SmartCard: return "Smart Card";
    case UsbClass::Video: return "Video (UVC)";
    case UsbClass::WirelessController: return "Wireless Controller";
    case UsbClass::Miscellaneous: return "Miscellaneous";
    case UsbClass::ApplicationSpecific: return "Application Specific";
    case UsbClass::VendorSpecific: return "Vendor Specific";
    default: return "Unknown";
    }
}

UsbTransferResult UsbHost::control_transfer(UsbDevice& dev, uint8_t dir, const UsbSetupPacket& setup, uint8_t* data,
                                            int len) {
    (void)dev;
    (void)dir;
    (void)setup;
    (void)data;
    return {true, len, 0};
}

UsbTransferResult UsbHost::bulk_out(UsbDevice& dev, uint8_t ep_number, const uint8_t* data, int len) {
    (void)dev;
    (void)ep_number;
    (void)data;
    return {true, len, 0};
}

UsbTransferResult UsbHost::bulk_in(UsbDevice& dev, uint8_t ep_number, uint8_t* buffer, int max_len) {
    (void)dev;
    (void)ep_number;
    (void)buffer;
    return {true, max_len, 0};
}

bool UsbHost::read_register(UsbDevice& dev, uint16_t reg, uint32_t* value) {
    (void)dev;
    (void)reg;
    if (value)
        *value = 0;
    return true;
}

bool UsbHost::write_register(UsbDevice& dev, uint16_t reg, uint32_t value) {
    (void)dev;
    (void)reg;
    (void)value;
    return true;
}

bool UsbHost::read_registers(UsbDevice& dev, uint16_t start_reg, uint8_t* buf, int len) {
    (void)dev;
    (void)start_reg;
    if (buf && len > 0)
        memset(buf, 0, len);
    return true;
}

bool UsbHost::write_registers(UsbDevice& dev, uint16_t start_reg, const uint8_t* buf, int len) {
    (void)dev;
    (void)start_reg;
    (void)buf;
    (void)len;
    return true;
}

bool UsbHost::read_reg8(UsbDevice& dev, uint16_t reg, uint8_t* val) {
    if (!val) return false;
    *val = 0;
    return true;
}

bool UsbHost::write_reg8(UsbDevice& dev, uint16_t reg, uint8_t val) {
    (void)dev;
    (void)reg;
    (void)val;
    return true;
}

bool UsbHost::read_reg16(UsbDevice& dev, uint16_t reg, uint16_t* val) {
    if (!val) return false;
    *val = 0;
    return true;
}

bool UsbHost::write_reg16(UsbDevice& dev, uint16_t reg, uint16_t val) {
    (void)dev;
    (void)reg;
    (void)val;
    return true;
}

bool UsbHost::read_reg32(UsbDevice& dev, uint16_t reg, uint32_t* val) {
    if (!val) return false;
    *val = 0;
    return true;
}

bool UsbHost::write_reg32(UsbDevice& dev, uint16_t reg, uint32_t val) {
    (void)dev;
    (void)reg;
    (void)val;
    return true;
}

// Private HAL methods implementations
bool UsbHost::hal_init_() {
    return true;
}

void UsbHost::hal_reset_() {}

bool UsbHost::hal_port_reset_(int port) {
    (void)port;
    return true;
}

bool UsbHost::hal_control_xfer_(UsbDevice& dev, uint8_t dir, const UsbSetupPacket& setup, uint8_t* data, int* len) {
    (void)dev;
    (void)dir;
    (void)setup;
    (void)data;
    (void)len;
    return true;
}

bool UsbHost::hal_bulk_xfer_(UsbDevice& dev, uint8_t ep_number, bool is_in, uint8_t* data, int* len) {
    (void)dev;
    (void)ep_number;
    (void)is_in;
    (void)data;
    (void)len;
    return true;
}

