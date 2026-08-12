#ifndef ETH_DRIVER_HPP
#define ETH_DRIVER_HPP

#include "net_device.hpp"
#include "board.h"
#include "../kernel/core/mutex.hpp"

// Hardware Register Definitions
#define ETH_MAC_RIS   (BOARD_ETH_MAC_BASE + 0x000)
#define ETH_MAC_IACK  (BOARD_ETH_MAC_BASE + 0x000)
#define ETH_MAC_RCTL  (BOARD_ETH_MAC_BASE + 0x008)
#define ETH_MAC_TCTL  (BOARD_ETH_MAC_BASE + 0x00C)
#define ETH_MAC_DATA  (BOARD_ETH_MAC_BASE + 0x018)
#define ETH_MAC_IA0   (BOARD_ETH_MAC_BASE + 0x01C)
#define ETH_MAC_IA1   (BOARD_ETH_MAC_BASE + 0x020)

#define SYSCTL_RCGC2_R    (BOARD_SYSCTL_BASE + 0x108)
#define SYSCTL_RCGC2_MAC  (1 << 28)
#define SYSCTL_RCGC2_PHY  (1 << 30)

#define MAC_RCTL_RXEN (1 << 0)
#define MAC_RCTL_AMUL (1 << 1)
#define MAC_RCTL_PRMS (1 << 2)

#define MAC_TCTL_TXEN  (1 << 0)
#define MAC_TCTL_PADEN (1 << 1)
#define MAC_TCTL_CRC   (1 << 2)

#define MAC_RIS_RXINT  (1 << 0)
#define MAC_IACK_RXINT (1 << 0)

class StellarisEth : public NetDevice {
private:
    // MAC / SYSCTL 基地址统一来自 BSP (boards/<board>/board.h)
    // 驱动不再硬编码任何物理地址，更换芯片只需替换 board.h
    volatile uint32_t* const mac_ris_;  // 中断状态
    volatile uint32_t* const mac_iack_; // 中断清除
    volatile uint32_t* const mac_rctl_; // 接收控制
    volatile uint32_t* const mac_tctl_; // 发送控制
    volatile uint32_t* const mac_data_; // FIFO 数据发送/接收口
    volatile uint32_t* const mac_ia0_;  // MAC 地址低4字节
    volatile uint32_t* const mac_ia1_;  // MAC 地址高2字节
    Mutex rx_mutex_;
    Mutex tx_mutex_;

public:
    StellarisEth();
    
    bool init() override;
    int receive_frame(uint8_t* buffer, int max_len) override;
    bool send_frame(const uint8_t* buffer, int len) override;

    void set_promiscuous_mode(bool enable);

    static StellarisEth& instance() {
        static StellarisEth eth;
        return eth;
    }
};

#endif
