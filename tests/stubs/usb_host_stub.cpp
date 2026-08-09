#include "../../drivers/usb/usb_host.hpp"
#include <string.h>

void UsbHost::init() { initialized_ = true; }
void UsbHost::reset() { initialized_ = false; }
bool UsbHost::enumerate_device(UsbDevice& dev) { return true; }
bool UsbHost::reset_port(int port) { return true; }

UsbTransferResult UsbHost::control_transfer(UsbDevice& dev, uint8_t dir,
                                             const UsbSetupPacket& setup,
                                             uint8_t* data, int len) {
    return {true, len, 0};
}

UsbTransferResult UsbHost::bulk_out(UsbDevice& dev, uint8_t ep_number,
                                     const uint8_t* data, int len) {
    return {true, len, 0};
}

UsbTransferResult UsbHost::bulk_in(UsbDevice& dev, uint8_t ep_number,
                                    uint8_t* buffer, int max_len) {
    return {true, max_len, 0};
}

bool UsbHost::read_register(UsbDevice& dev, uint16_t reg, uint32_t* value) {
    if (value) *value = 0;
    return true;
}

bool UsbHost::write_register(UsbDevice& dev, uint16_t reg, uint32_t value) {
    return true;
}

bool UsbHost::read_registers(UsbDevice& dev, uint16_t start_reg, uint8_t* buf, int len) {
    if (buf && len > 0) memset(buf, 0, len);
    return true;
}

bool UsbHost::write_registers(UsbDevice& dev, uint16_t start_reg, const uint8_t* buf, int len) {
    return true;
}

// Private HAL methods implementations
bool UsbHost::hal_init_() { return true; }
void UsbHost::hal_reset_() {}
bool UsbHost::hal_port_reset_(int port) { return true; }
bool UsbHost::hal_control_xfer_(UsbDevice& dev, uint8_t dir,
                                 const UsbSetupPacket& setup,
                                 uint8_t* data, int* len) {
    return true;
}
bool UsbHost::hal_bulk_xfer_(UsbDevice& dev, uint8_t ep_number, bool is_in,
                              uint8_t* data, int* len) {
    return true;
}
