#ifndef AURORA_HCI_UART_TRANSPORT_HPP
#define AURORA_HCI_UART_TRANSPORT_HPP

#include "hci_transport.hpp"
#include "../../../kernel/core/device.hpp"

namespace auroraos {
namespace ble {
namespace hci {

class HciUartTransport : public HciTransport {
private:
    CharDevice* uart_dev_;

    // H4 协议解析状态机
    enum class RxState {
        WaitPacketType,
        WaitEventHeader,
        WaitAclHeader,
        WaitPayload
    };

    RxState rx_state_;
    uint8_t pkt_type_;
    uint8_t rx_buffer_[260]{}; // BLE 5.x ACL 数据包最大 255 字节载荷 + 4 字节头
    uint16_t rx_expected_len_;
    uint16_t rx_cursor_;
    uint32_t rx_packets_count_{0};
    uint32_t rx_bytes_count_{0};
    uint32_t rx_errors_count_{0};

public:
    explicit HciUartTransport(CharDevice* uart_dev);

    void init() override;

    int send_cmd(const uint8_t* cmd, size_t len) override;
    int send_acl(const uint8_t* data, size_t len) override;

    // 由 UART 接收中断(ISR) 或后台轮询线程调用，驱动 H4 状态机
    void feed_rx_byte(uint8_t byte) override;
    void feed_rx_bytes(const uint8_t* buf, size_t len) override;

    // 统计接口
    uint32_t get_rx_packets() const { return rx_packets_count_; }
    uint32_t get_rx_bytes() const { return rx_bytes_count_; }
    uint32_t get_rx_errors() const { return rx_errors_count_; }
};

} // namespace hci
} // namespace ble
} // namespace auroraos

#endif // AURORA_HCI_UART_TRANSPORT_HPP
