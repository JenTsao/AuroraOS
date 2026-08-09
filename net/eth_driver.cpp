#include "eth_driver.hpp"
#include "mutex.hpp"
#include "syscall.hpp"
#include "autoconf.h"
#include "config/net_config.h"
#include "../metrics/metrics.hpp"
#include <string.h>

extern Mutex uart_mutex;

StellarisEth::StellarisEth()
    : mac_ris_(reinterpret_cast<uint32_t*>(ETH_MAC_RIS)),
      mac_iack_(reinterpret_cast<uint32_t*>(ETH_MAC_IACK)),
      mac_rctl_(reinterpret_cast<uint32_t*>(ETH_MAC_RCTL)),
      mac_tctl_(reinterpret_cast<uint32_t*>(ETH_MAC_TCTL)),
      mac_data_(reinterpret_cast<uint32_t*>(ETH_MAC_DATA)),
      mac_ia0_(reinterpret_cast<uint32_t*>(ETH_MAC_IA0)),
      mac_ia1_(reinterpret_cast<uint32_t*>(ETH_MAC_IA1)) {

    // 使用 Kconfig 配置的默认 MAC 地址
#ifdef CONFIG_NET_DEFAULT_MAC
    const char* mac_str = CONFIG_NET_DEFAULT_MAC;
    for (int i = 0; i < 6; i++) {
        auto hex_val = [](char c) -> uint8_t {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return 0;
        };
        mac_address_[i] = (hex_val(mac_str[0]) << 4) | hex_val(mac_str[1]);
        if (mac_str[2] != '\0') mac_str += 3;
    }
#else
    // 如果没有配置则回退到 BSP (board.h) 提供的值
    mac_address_[0] = BOARD_DEFAULT_MAC0;
    mac_address_[1] = BOARD_DEFAULT_MAC1;
    mac_address_[2] = BOARD_DEFAULT_MAC2;
    mac_address_[3] = BOARD_DEFAULT_MAC3;
    mac_address_[4] = BOARD_DEFAULT_MAC4;
    mac_address_[5] = BOARD_DEFAULT_MAC5;
#endif
}

bool StellarisEth::init() {
    sys_print("[NetDriver] Activating Stellaris Ethernet Controller Clocks...\r\n");

    // 1. 开启系统控制寄存器中的以太网 MAC 和 PHY 时钟门控
    volatile uint32_t* sysctl_rcgc2 = reinterpret_cast<uint32_t*>(SYSCTL_RCGC2_R);
    *sysctl_rcgc2 |= SYSCTL_RCGC2_MAC | SYSCTL_RCGC2_PHY; 
    
    // 简单循环等待时钟稳定
    for (volatile int i = 0; i < 10000; i++);

    // 2. 写入本机 MAC 地址到硬件过滤寄存器
    *mac_ia0_ = (mac_address_[3] << 24) | (mac_address_[2] << 16) | (mac_address_[1] << 8) | mac_address_[0];
    *mac_ia1_ = (mac_address_[5] << 8) | mac_address_[4];

    // 3. 配置接收控制寄存器 (RCTL):
    // RXEN (开启接收), AMUL (接收多播)
    *mac_rctl_ = MAC_RCTL_RXEN | MAC_RCTL_AMUL;

    // 4. 配置发送控制寄存器 (TCTL):
    // TXEN (开启发包), PADEN (自动补齐到60字节), CRC (自动追加硬件 CRC 校验和)
    *mac_tctl_ = MAC_TCTL_TXEN | MAC_TCTL_PADEN | MAC_TCTL_CRC;

    link_up_ = true;
    sys_print("[NetDriver] L2 Ethernet MAC Initialized [UP]\r\n");
    return true;
}

// 从网卡硬件 FIFO 读取以太网帧
int StellarisEth::receive_frame(uint8_t* buffer, int max_len) {
    LockGuard rx_lock(rx_mutex_);
    // 检查缓冲区合法性
    if (max_len <= 0) return 0;

    // 检查是否有数据帧到达 (读取 MAC_RIS 的 Bit 0: RXINT)
    if ((*mac_ris_ & MAC_RIS_RXINT) == 0) {
        return 0; // FIFO 为空
    }

    // Stellaris 接收 FIFO 的第一个字是：帧长度 (减去 CRC 的实际数据大小)
    uint32_t frame_len = *mac_data_;
    if (frame_len > static_cast<uint32_t>(max_len) || frame_len == 0) {
        Metrics::inc_net_drop();
        // [安全加固] 数据帧异常，必须排空 FIFO 里的这包数据，否则硬件 MAC 会死锁
        int words_to_discard = (frame_len + 3) / 4;
        for (int i = 0; i < words_to_discard; i++) {
            volatile uint32_t discard = *mac_data_;
            (void)discard;
        }
        *mac_iack_ = MAC_IACK_RXINT;
        return 0;
    }

    // 按 4 字节 (Word) 从 FIFO 读取并填入缓冲区
    int words_to_read = frame_len / 4;
    for (int i = 0; i < words_to_read; i++) {
        uint32_t word = *mac_data_;
        memcpy(buffer + i * 4, &word, 4);
    }
    
    int remaining_bytes = frame_len % 4;
    if (remaining_bytes > 0) {
        uint32_t last_word = *mac_data_;
        memcpy(buffer + words_to_read * 4, &last_word, remaining_bytes);
    }

    // 清除硬件接收中断标志位
    *mac_iack_ = MAC_IACK_RXINT;
    return static_cast<int>(frame_len);
}

// 将以太网帧写入网卡硬件 FIFO 并触发发包
bool StellarisEth::send_frame(const uint8_t* buffer, int len) {
    LockGuard tx_lock(tx_mutex_);
    if (!link_up_ || len <= 0 || len > 1514) return false;

    // Stellaris 发送 FIFO 要求先写入两字节的长度，再写入数据
    // 为兼容 32 位对齐写，我们先将长度和前两字节拼凑在一起写入
    uint32_t first_word = (len & 0xFFFF);
    if (len >= 1) first_word |= ((static_cast<uint32_t>(buffer[0]) << 16) & 0xFF0000);
    if (len >= 2) first_word |= ((static_cast<uint32_t>(buffer[1]) << 24) & 0xFF000000);
    *mac_data_ = first_word;

    // 把余下的数据按字写入 FIFO
    const uint8_t* remaining_data = buffer + 2;
    int remaining_len = len - 2;
    if (remaining_len > 0) {
        int words_to_write = (remaining_len + 3) / 4;
        for (int i = 0; i < words_to_write; i++) {
            uint32_t word = 0;
            int copy_len = (remaining_len - i * 4 >= 4) ? 4 : (remaining_len - i * 4);
            memcpy(&word, remaining_data + i * 4, copy_len);
            *mac_data_ = word;
        }
    }

    // 触发网卡发送引擎开始把 FIFO 数据送上物理网线！
    // 改写脉冲寄存器，避免 read-modify-write 带来的竞争问题
    *mac_tctl_ = MAC_TCTL_TXEN | MAC_TCTL_PADEN | MAC_TCTL_CRC;
    return true;
}

void StellarisEth::set_promiscuous_mode(bool enable) {
    if (enable) {
        *mac_rctl_ |= MAC_RCTL_PRMS;
    } else {
        *mac_rctl_ &= ~MAC_RCTL_PRMS;
    }
}
