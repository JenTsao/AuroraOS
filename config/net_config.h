#ifndef NET_CONFIG_H
#define NET_CONFIG_H

#include <stdint.h>

// =====================================================================
// 网络协议层配置：将 IP / 掩码 / 网关等可变参数从业务逻辑中剥离
//
// 后续接入 DHCP 时只需两步，业务层 (tcpip_init_done) 无需改动：
//   1. 在 adapter/net/lwipopts.h 中开启 LWIP_DHCP
//   2. 将 default_ipv4_config.use_dhcp 置为 true
// =====================================================================

struct NetIpv4Config {
    uint8_t ip[4];        // 本机 IPv4 地址
    uint8_t netmask[4];   // 子网掩码
    uint8_t gateway[4];   // 默认网关
    bool    use_dhcp;     // true: 由 DHCP 动态获取（需启用 LWIP_DHCP）；false: 使用静态配置
};

#include "autoconf.h"

inline uint8_t parse_octet(const char*& str) {
    uint8_t val = 0;
    while (*str >= '0' && *str <= '9') {
        val = val * 10 + (*str - '0');
        str++;
    }
    if (*str == '.' || *str == ':') str++;
    return val;
}

inline uint8_t parse_hex_octet(const char*& str) {
    uint8_t val = 0;
    int count = 0;
    while (count < 2 && ((*str >= '0' && *str <= '9') || 
           (*str >= 'a' && *str <= 'f') || 
           (*str >= 'A' && *str <= 'F'))) {
        uint8_t digit = 0;
        if (*str >= '0' && *str <= '9') digit = *str - '0';
        else if (*str >= 'a' && *str <= 'f') digit = *str - 'a' + 10;
        else if (*str >= 'A' && *str <= 'F') digit = *str - 'A' + 10;
        val = val * 16 + digit;
        str++;
        count++;
    }
    if (*str == ':' || *str == '-') str++;
    return val;
}

inline NetIpv4Config get_default_ipv4_config() {
    NetIpv4Config cfg = {{0, 0, 0, 0}, {255, 255, 255, 0}, {192, 168, 1, 1}, false};
#ifdef CONFIG_NET_DEFAULT_IP
    const char* ip_str = CONFIG_NET_DEFAULT_IP;
    cfg.ip[0] = parse_octet(ip_str);
    cfg.ip[1] = parse_octet(ip_str);
    cfg.ip[2] = parse_octet(ip_str);
    cfg.ip[3] = parse_octet(ip_str);
#else
    cfg.ip[0] = 192; cfg.ip[1] = 168; cfg.ip[2] = 1; cfg.ip[3] = 100;
#endif
    return cfg;
}

#endif // NET_CONFIG_H
