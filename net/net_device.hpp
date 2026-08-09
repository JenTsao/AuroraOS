#ifndef NET_DEVICE_HPP
#define NET_DEVICE_HPP

#include <stdint.h>
#include <stddef.h>

// 标准以太网 MAC 地址长度
constexpr int MAC_ADDR_LEN = 6;

// 标准以太网帧头部 (14 字节)
struct EthernetHeader {
    uint8_t dest_mac[MAC_ADDR_LEN]; // 目的 MAC
    uint8_t src_mac[MAC_ADDR_LEN];  // 源 MAC
    uint16_t eth_type;              // 协议类型 (如 0x0800=IPv4, 0x0806=ARP)
} __attribute__((packed));

// 网卡硬件抽象基类
class NetDevice {
protected:
    uint8_t mac_address_[MAC_ADDR_LEN];
    bool link_up_ = false;

public:
    virtual ~NetDevice() = default;

    // 初始化网卡硬件
    virtual bool init() = 0;

    // 从网卡硬件 FIFO 接收一个以太网帧 (返回实际读取字节数，0 表示无数据)
    virtual int receive_frame(uint8_t* buffer, int max_len) = 0;

    // 向网卡硬件 FIFO 发送一个以太网帧
    virtual bool send_frame(const uint8_t* buffer, int len) = 0;

    // 获取本机 MAC 地址
    const uint8_t* get_mac() const { return mac_address_; }

    // 运行时覆写 MAC 地址（用于 OUI 伪装，需在 init() 前调用）
    void set_mac_address(const uint8_t mac[6]) {
        for (int i = 0; i < MAC_ADDR_LEN; i++) mac_address_[i] = mac[i];
    }

    bool is_link_up() const { return link_up_; }
};

#endif
