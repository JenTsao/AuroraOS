#ifndef AURORA_HCI_TRANSPORT_HPP
#define AURORA_HCI_TRANSPORT_HPP

#include <stdint.h>
#include <stddef.h>

namespace auroraos {
namespace ble {
namespace hci {

// ========================================================
// HCI Transport Layer (Host <-> Controller Interface)
// 负责与底层硬件 BLE Controller 交互 (通过 UART, SPI, 内存 IPC)
// ========================================================
class HciTransport {
public:
    virtual ~HciTransport() = default;

    virtual void init() = 0;
    
    // 发送 HCI Command (Host -> Controller)
    virtual int send_cmd(const uint8_t* cmd, size_t len) = 0;
    
    // 发送 ACL Data (Host -> Controller)
    virtual int send_acl(const uint8_t* data, size_t len) = 0;
    
    // 硬件 ISR 或轮询线程调用的入口：收到底层硬件数据后上报给 Host 栈
    // 底层驱动在解析出完整的 HCI Event / ACL 数据包后调用
    void on_hardware_rx(uint8_t pkt_type, const uint8_t* data, size_t len);
};

// 全局 HCI 传输层实例配置
extern HciTransport* g_hci_transport;

} // namespace hci
} // namespace ble
} // namespace auroraos

#endif // AURORA_HCI_TRANSPORT_HPP
