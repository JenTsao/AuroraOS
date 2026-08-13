#include "hci_uart_transport.hpp"
#include <string.h>

namespace auroraos {
namespace ble {
namespace hci {

HciUartTransport::HciUartTransport(CharacterDevice* uart_dev)
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
    uint8_t h4_type = 0x01;

    // 发送 H4 Header (1 byte)
    uart_dev_->write(&h4_type, 1);

    // 发送 HCI Command Payload
    uart_dev_->write(cmd, len);

    return 0;
}

// --------------------------------------------------------
// Send HCI ACL Data (Packet Type 0x02)
// --------------------------------------------------------
int HciUartTransport::send_acl(const uint8_t* data, size_t len) {
    if (!uart_dev_)
        return -1;
    uint8_t h4_type = 0x02;

    // 发送 H4 Header (1 byte)
    uart_dev_->write(&h4_type, 1);

    // 发送 HCI ACL Payload
    uart_dev_->write(data, len);

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

// --------------------------------------------------------
// Dispatch to NimBLE Host
// --------------------------------------------------------
void HciTransport::on_hardware_rx(uint8_t pkt_type, const uint8_t* data, size_t len) {
    // 此处将提取出的完整 HCI 数据包推入 NimBLE Host 的事件队列中
    // 具体实现位于 NimBLE 的 HCI 绑定代码中，一般是通过 ble_hci_trans_ll_evt_tx 转发
    extern "C" int ble_hci_trans_ll_evt_tx(uint8_t* hci_ev);
    extern "C" int ble_hci_trans_ll_acl_tx(void* om);

    // 对于 NimBLE 而言，HCI 缓冲必须从 os_mempool 中分配，
    // 因此在对接层，我们通常会在这里先分配 ble_hci_trans_buf_alloc，拷贝后再传入。
    // 这部分留由 NimBLE HCI 桥接模块处理。
}

// 声明全局传输层指针
HciTransport* g_hci_transport = nullptr;

} // namespace hci
} // namespace ble
} // namespace auroraos
