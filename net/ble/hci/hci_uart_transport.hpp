#ifndef AURORA_HCI_UART_TRANSPORT_HPP
#define AURORA_HCI_UART_TRANSPORT_HPP

#include "hci_transport.hpp"
#include "../../../kernel/device.hpp"

namespace auroraos {
namespace ble {
namespace hci {

class HciUartTransport : public HciTransport {
private:
    CharacterDevice* uart_dev_;

    // H4 协议解析状态机
    enum class RxState {
        WaitPacketType,
        WaitEventHeader,
        WaitAclHeader,
        WaitPayload
    };

    RxState rx_state_;
    uint8_t pkt_type_;
    uint8_t rx_buffer_[256]{}; // BLE 5.0 最大扩展到 255
    uint16_t rx_expected_len_;
    uint16_t rx_cursor_;

public:
    explicit HciUartTransport(CharacterDevice* uart_dev);

    void init() override;

    int send_cmd(const uint8_t* cmd, size_t len) override;
    int send_acl(const uint8_t* data, size_t len) override;

    // 由 UART 接收中断(ISR) 或后台轮询线程调用，驱动 H4 状态机
    void feed_rx_byte(uint8_t byte);
};

} // namespace hci
} // namespace ble
} // namespace auroraos

#endif // AURORA_HCI_UART_TRANSPORT_HPP
