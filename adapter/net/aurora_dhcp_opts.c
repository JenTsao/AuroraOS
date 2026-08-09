/**
 * aurora_dhcp_opts.c — DHCP Option 55 自定义参数请求列表
 *
 * 当 AURORA_DHCP_OPTION55_CUSTOM 宏定义时，lwIP 的 dhcp.c 将使用此文件
 * 提供的外部数组替代内置的 dhcp_discover_request_options。
 *
 * 编译期根据 Kconfig CONFIG_STEALTH_DHCP_FINGERPRINT 选择对应指纹。
 */

#include "lwip/opt.h"
#include "lwip/prot/dhcp.h"

/* ── 编译期指纹选择 ────────────────────────────────────────── */
/* 0=默认(lwIP), 1=iOS, 2=Windows10, 3=HP打印机 */
#ifndef CONFIG_STEALTH_DHCP_FINGERPRINT
#define CONFIG_STEALTH_DHCP_FINGERPRINT 1
#endif

#if CONFIG_STEALTH_DHCP_FINGERPRINT == 0 || CONFIG_STEALTH_DHCP_FINGERPRINT == 4
/* 回退到 lwIP 默认 Option 55（Subnet/Router/Broadcast/DNS） */
const u8_t dhcp_discover_request_options[] = {
    DHCP_OPTION_SUBNET_MASK,
    DHCP_OPTION_ROUTER,
    DHCP_OPTION_BROADCAST,
    DHCP_OPTION_DNS_SERVER,
};
const u8_t aurora_dhcp_option55_len = sizeof(dhcp_discover_request_options);

#elif CONFIG_STEALTH_DHCP_FINGERPRINT == 1
/* iOS 15.x DHCP 特征 — 请求 10 个参数 */
const u8_t dhcp_discover_request_options[] = {
    DHCP_OPTION_SUBNET_MASK,    /*  1 */
    DHCP_OPTION_ROUTER,         /*  3 */
    DHCP_OPTION_DNS_SERVER,     /*  6 */
    15,                         /* Domain Name */
    42,                         /* NTP Server */
    33,                         /* Static Route */
    121,                        /* Classless Static Route */
    44,                         /* NetBIOS Name Server */
    46,                         /* NetBIOS Node Type */
    252,                        /* WPAD */
};
const u8_t aurora_dhcp_option55_len = sizeof(dhcp_discover_request_options);

#elif CONFIG_STEALTH_DHCP_FINGERPRINT == 2
/* Windows 10/11 DHCP 特征 — 请求 12 个参数 */
const u8_t dhcp_discover_request_options[] = {
    DHCP_OPTION_SUBNET_MASK,    /*  1 */
    DHCP_OPTION_ROUTER,         /*  3 */
    DHCP_OPTION_DNS_SERVER,     /*  6 */
    15,                         /* Domain Name */
    44,                         /* NetBIOS Name Server */
    46,                         /* NetBIOS Node Type */
    47,                         /* NetBIOS Scope */
    31,                         /* Router Discovery */
    33,                         /* Static Route */
    121,                        /* Classless Static Route */
    249,                        /* Classless Static Route (MS) */
    43,                         /* Vendor Specific Info */
};
const u8_t aurora_dhcp_option55_len = sizeof(dhcp_discover_request_options);

#elif CONFIG_STEALTH_DHCP_FINGERPRINT == 3
/* HP 打印机固件特征 — 极简 6 个参数 */
const u8_t dhcp_discover_request_options[] = {
    DHCP_OPTION_SUBNET_MASK,    /*  1 */
    DHCP_OPTION_ROUTER,         /*  3 */
    DHCP_OPTION_DNS_SERVER,     /*  6 */
    15,                         /* Domain Name */
    44,                         /* NetBIOS Name Server */
    46,                         /* NetBIOS Node Type */
};
const u8_t aurora_dhcp_option55_len = sizeof(dhcp_discover_request_options);

#else
/* 回退：不覆盖，使用 lwIP 默认 */
/* (此分支不应在 AURORA_DHCP_OPTION55_CUSTOM 启用时到达) */
#error "Invalid CONFIG_STEALTH_DHCP_FINGERPRINT value"
#endif
