#ifndef AURORA_USB_CORE_HPP
#define AURORA_USB_CORE_HPP

#include <stdint.h>
#include <stddef.h>

namespace auroraos {
namespace usb {

enum class UsbTransferType : uint8_t {
    Control = 0,
    Isochronous = 1,
    Bulk = 2,
    Interrupt = 3
};

enum class UsbTransferDirection : uint8_t {
    Out = 0, // Host to Device
    In = 1   // Device to Host
};

// Standard USB Setup Packet (8 bytes)
#pragma pack(push, 1)
struct UsbSetupPacket {
    uint8_t  bmRequestType;
    uint8_t  bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
};
#pragma pack(pop)

struct UsbTransfer {
    uint8_t endpoint;
    UsbTransferType type;
    UsbTransferDirection direction;
    void* buffer;
    size_t length;
    size_t actual_length;
    bool completed;
    
    // Callback for asynchronous transfers (e.g., Isochronous Camera Frames)
    void (*callback)(UsbTransfer* transfer, void* user_data);
    void* user_data;
};

class UsbHostController {
public:
    virtual ~UsbHostController() = default;
    
    virtual bool init() = 0;
    virtual bool submit_transfer(UsbTransfer* transfer) = 0;
};

class UsbDeviceDriver {
public:
    virtual ~UsbDeviceDriver() = default;
    
    virtual bool probe(uint16_t vendor_id, uint16_t product_id) = 0;
    virtual void attach(UsbHostController* hcd) = 0;
    virtual void detach() = 0;
};

} // namespace usb
} // namespace auroraos

#endif // AURORA_USB_CORE_HPP
