#ifndef AURORA_USB_CORE_HPP
#define AURORA_USB_CORE_HPP

#include <stdint.h>

// USB 2.0 standard: SETUP packet is exactly 8 bytes (bmRequestType + bRequest +
// wValue + wIndex + wLength), verified by unit test sizeof() assertion.

namespace auroraos {
namespace usb {

struct UsbSetupPacket {
    uint8_t  bmRequestType;
    uint8_t  bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
};

// Ensure the struct packs to exactly 8 bytes on all platforms.
static_assert(sizeof(UsbSetupPacket) == 8, "UsbSetupPacket must be exactly 8 bytes");

} // namespace usb
} // namespace auroraos

#endif // AURORA_USB_CORE_HPP
