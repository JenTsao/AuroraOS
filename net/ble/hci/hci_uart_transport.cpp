#include "hci_uart_transport.hpp"
#include "hci_event_dispatch.hpp"
#include <string.h>

namespace auroraos {
namespace ble {
namespace hci {

HciUartTransport::HciUartTransport(CharDevice* uart_dev)
    : uart_dev_(uart_dev), rx_state_(RxState::WaitPacketType), pkt_type_(0), rx_expected_len_(0), rx_cursor_(0) {}

void HciUartTransport::init() {
    // UART should already be initialized by the board bring-up code.
    // Ensure we are in a clean state.
    rx_state_ = RxState::WaitPacketType;
    rx_cursor_ = 0;
}

// --------------------------------------------------------
// Send HCI Command (Packet Type 0x01)
// --------------------------------------------------------
int HciUartTransport::send_cmd(const uint8_t* cmd, size_t len) {
    if (!uart_dev_)
        return -1;
    char h4_type = 0x01;

    // 发送 H4 Header (1 byte)
    uart_dev_->write(&h4_type, 1, 0, nullptr);

    // 发送 HCI Command Payload
    uart_dev_->write(reinterpret_cast<const char*>(cmd), static_cast<int>(len), 0, nullptr);

    return 0;
}

// --------------------------------------------------------
// Send HCI ACL Data (Packet Type 0x02)
// --------------------------------------------------------
int HciUartTransport::send_acl(const uint8_t* data, size_t len) {
    if (!uart_dev_)
        return -1;
    char h4_type = 0x02;

    // 发送 H4 Header (1 byte)
    uart_dev_->write(&h4_type, 1, 0, nullptr);

    // 发送 HCI ACL Payload
    uart_dev_->write(reinterpret_cast<const char*>(data), static_cast<int>(len), 0, nullptr);

    return 0;
}

// --------------------------------------------------------
// RX State Machine (H4 Protocol Parsing)
// --------------------------------------------------------
void HciUartTransport::feed_rx_byte(uint8_t byte) {
    switch (rx_state_) {
    case RxState::WaitPacketType:
        if (byte == 0x04) { // HCI Event
            pkt_type_ = byte;
            rx_cursor_ = 0;
            rx_state_ = RxState::WaitEventHeader;
        } else if (byte == 0x02) { // HCI ACL
            pkt_type_ = byte;
            rx_cursor_ = 0;
            rx_state_ = RxState::WaitAclHeader;
        }
        // Ignore unknown packet types
        break;

    case RxState::WaitEventHeader:
        rx_buffer_[rx_cursor_++] = byte;
        if (rx_cursor_ == 2) {
            // Event Header is 2 bytes: [Event Code] [Parameter Total Length]
            rx_expected_len_ = rx_buffer_[1];
            if (rx_expected_len_ == 0) {
                on_hardware_rx(pkt_type_, rx_buffer_, 2);
                rx_state_ = RxState::WaitPacketType;
            } else {
                rx_state_ = RxState::WaitPayload;
            }
        }
        break;

    case RxState::WaitAclHeader:
        rx_buffer_[rx_cursor_++] = byte;
        if (rx_cursor_ == 4) {
            // ACL Header is 4 bytes: [Handle(12)+PB(2)+BC(2)] [Data Total Length (2 bytes, LE)]
            rx_expected_len_ = rx_buffer_[2] | (rx_buffer_[3] << 8);
            if (rx_expected_len_ == 0) {
                on_hardware_rx(pkt_type_, rx_buffer_, 4);
                rx_state_ = RxState::WaitPacketType;
            } else {
                rx_state_ = RxState::WaitPayload;
            }
        }
        break;

    case RxState::WaitPayload:
        rx_buffer_[rx_cursor_++] = byte;
        uint16_t header_len = (pkt_type_ == 0x04) ? 2 : 4;

        if (rx_cursor_ == header_len + rx_expected_len_ || rx_cursor_ >= sizeof(rx_buffer_)) {
            on_hardware_rx(pkt_type_, rx_buffer_, rx_cursor_);
            rx_state_ = RxState::WaitPacketType;
        }
        break;
    }
}

// 弱符号声明：当启用 NimBLE Host 时由 3rdparty/nimble_port 覆盖实现
extern "C" __attribute__((weak)) void aurora_nimble_rx_event(const uint8_t* data, size_t len) {
    (void)data;
    (void)len;
}

extern "C" __attribute__((weak)) void aurora_nimble_rx_acl(const uint8_t* data, size_t len) {
    (void)data;
    (void)len;
}

// --------------------------------------------------------
// Dispatch received HCI packet
// --------------------------------------------------------
void HciTransport::on_hardware_rx(uint8_t pkt_type, const uint8_t* data, size_t len) {
    if (!data || len == 0)
        return;

    // HCI Event (0x04) → 分发到安全模块与连接状态机，并桥接到 NimBLE Host 栈
    if (pkt_type == 0x04) {
        dispatch_hci_event(data, len);
        aurora_nimble_rx_event(data, len);
        return;
    }

    // HCI ACL Data (0x02) → 桥接到 NimBLE Host 栈
    if (pkt_type == 0x02) {
        aurora_nimble_rx_acl(data, len);
        return;
    }
}

// 声明全局传输层指针
HciTransport* g_hci_transport = nullptr;

} // namespace hci
} // namespace ble
} // namespace auroraos
