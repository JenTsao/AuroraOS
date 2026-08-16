#include "net_app.hpp"
#include "posix.hpp" // 使用我们的标准 sleep 和 print 封装
#include "vfs.hpp"
#include "timer.hpp"                   // 引入软件定时器
#include "../net/distributed_bus.hpp"  // 引入软总线
#include "../net/stealth_identity.hpp" // 局域网隐身伪装引擎
#if !defined(ARCH_RISCV32) && !defined(ARCH_AARCH64) && !defined(BOARD_MIBAND8)
#include "../net/eth_driver.hpp" // StellarisEth (LM3S6965 only)
#define HAS_STELLARIS_ETH 1
#endif

// 引入 lwIP 核心头文件
#include "lwip/netif.h"
#include "lwip/tcpip.h"
#include "lwip/dhcp.h"
#include "lwip/etharp.h"
#include "netif/ethernet.h"

#if defined(HAS_STELLARIS_ETH)
// 声明在 adapter/net/ethernetif.cpp 中实现的硬件以太网卡初始化函数
extern err_t ethernetif_init(struct netif* netif);
#else
// 虚拟/无物理网卡环境下的通用 netif 初始化回调
err_t ethernetif_init(struct netif* netif) {
    netif->name[0] = 'e';
    netif->name[1] = 'n';
    netif->output = etharp_output;
    netif->linkoutput = [](struct netif*, struct pbuf*) -> err_t { return ERR_OK; };
    netif->mtu = 1500;
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_LINK_UP;
    netif->hwaddr_len = 6;
    netif->hwaddr[0] = 0x02;
    netif->hwaddr[1] = 0x00;
    netif->hwaddr[2] = 0x00;
    netif->hwaddr[3] = 0x00;
    netif->hwaddr[4] = 0x00;
    netif->hwaddr[5] = 0x01;
    return ERR_OK;
}
#endif

// 全局网卡结构体
struct netif g_netif;

// ========================================================
// 伪装身份预设 — 由 Kconfig choice 编译期选择
// ========================================================
static StealthIdentity::Preset stealth_preset_from_config() {
#if defined(CONFIG_STEALTH_APPLE_IPAD)
    return StealthIdentity::Preset::APPLE_IPAD;
#elif defined(CONFIG_STEALTH_APPLE_IPHONE)
    return StealthIdentity::Preset::APPLE_IPHONE;
#elif defined(CONFIG_STEALTH_APPLE_MACBOOK)
    return StealthIdentity::Preset::APPLE_MACBOOK;
#elif defined(CONFIG_STEALTH_HP_OFFICEJET)
    return StealthIdentity::Preset::HP_OFFICEJET;
#elif defined(CONFIG_STEALTH_SAMSUNG_GALAXY)
    return StealthIdentity::Preset::SAMSUNG_GALAXY;
#elif defined(CONFIG_STEALTH_HP_LASERJET)
    return StealthIdentity::Preset::HP_LASERJET;
#else
    return StealthIdentity::Preset::NONE;
#endif
}

// ========================================================
// lwIP 核心协议栈初始化完成后的回调函数
// ========================================================
static void tcpip_init_done_cb(void* /*arg*/) {
    ip4_addr_t ipaddr, netmask, gw;

    // 初始化时 IP 全设为 0，因为我们要通过 DHCP 获取
    IP4_ADDR(&ipaddr, 0, 0, 0, 0);
    IP4_ADDR(&netmask, 0, 0, 0, 0);
    IP4_ADDR(&gw, 0, 0, 0, 0);

    // ── Layer 1: MAC OUI 厂商欺骗 ──────────────────────────────
    // 在 netif_add 之前，获取网卡单例并施加 MAC 伪装。
    // 后 3 字节由 DWT 硬件时钟随机生成，确保单播+全局唯一。
    StealthIdentity& stealth = StealthIdentity::instance();
    stealth.set_active_preset(stealth_preset_from_config());
#if defined(HAS_STELLARIS_ETH)
    StellarisEth& eth = StellarisEth::instance();
    uint8_t spoofed_mac[6];
    stealth.apply(eth, stealth.active_preset(), spoofed_mac);

    // 初始化网卡硬件 (将伪装 MAC 写入 MAC 地址过滤寄存器)
    eth.init();

    // 1. 将以太网卡挂载到 lwIP 协议栈，并绑定底层驱动和输入入口
    //    将 StellarisEth 实例作为 state 传入，供 ethernetif_init 读取 MAC
    netif_add(&g_netif, &ipaddr, &netmask, &gw, &eth, ethernetif_init, tcpip_input);
#else
    // 无物理以太网卡平台（RISC-V / AArch64 QEMU Virt 等）
    netif_add(&g_netif, &ipaddr, &netmask, &gw, nullptr, ethernetif_init, tcpip_input);
#endif

    // 2. 设置为默认网卡并启动
    netif_set_default(&g_netif);
    netif_set_up(&g_netif);

    // ── Layer 2: DHCP 主机名伪装 ───────────────────────────────
    // 在 dhcp_start 之前设置主机名，lwIP 将自动作为 DHCP Option 12 发送
    const char* spoofed_host = stealth.get_hostname();
    if (spoofed_host) {
        g_netif.hostname = spoofed_host;
    }
    // Layer 3 (Option 55 指纹) 由编译期 AURORA_DHCP_OPTION55_CUSTOM
    // 在 adapter/net/aurora_dhcp_opts.c 中自动注入，无需运行时干预

    // 3. 启动 DHCP 客户端，开始在局域网内广播请求！
    dhcp_start(&g_netif);
}

// ========================================================
// 软总线监听任务的入口包装
void softbus_listener_entry(void) {
    DistributedSoftBus::instance().listener_task();
}

// 软件定时器回调：自动发送心跳广播
void beacon_timer_callback(void* /*arg*/) {
    DistributedSoftBus::instance().broadcast_beacon();
}

// ========================================================
// 网络主轮询任务：由调度器在后台运行
// ========================================================
void NetApp::run_dhcp_client() {
    int console_fd = open("/dev/uart0", 0);
    write(console_fd, "[Network] Starting lwIP TCP/IP Stack...\r\n", 41);

    // 启动 lwIP 内部的 tcpip_thread 核心守护线程，并注册完成回调
    tcpip_init(tcpip_init_done_cb, nullptr);

    bool ip_assigned = false;
    static bool network_services_started = false;

    // 轮询等待 DHCP 服务器分配 IP
    while (true) {
        if (dhcp_supplied_address(&g_netif)) {
            if (!ip_assigned) {
                // 当成功拿到 IP 时，进行格式化打印
                char msg[128];
                int len = 0;
                auto append = [&](const char* s) {
                    while (*s && len < (int)sizeof(msg) - 1)
                        msg[len++] = *s++;
                };
                auto append_num = [&](uint8_t n) {
                    char tmp[4];
                    int i = 0;
                    if (n == 0)
                        tmp[i++] = '0';
                    while (n > 0) {
                        tmp[i++] = (n % 10) + '0';
                        n /= 10;
                    }
                    while (i > 0 && len < (int)sizeof(msg) - 1)
                        msg[len++] = tmp[--i];
                };

                append("\r\n\r\n🌐 [DHCP] Success! auroraOS got IP Address: ");
                append_num(ip4_addr1(netif_ip4_addr(&g_netif)));
                append(".");
                append_num(ip4_addr2(netif_ip4_addr(&g_netif)));
                append(".");
                append_num(ip4_addr3(netif_ip4_addr(&g_netif)));
                append(".");
                append_num(ip4_addr4(netif_ip4_addr(&g_netif)));
                append("\r\n\r\n");

                msg[len] = '\0';
                write(console_fd, msg, len);

                if (!network_services_started) {
                    // ========================================================
                    // 核心：在拿到网络身份后，正式激活 HarmonyOS 级软总线！
                    // 且整个生命周期只初始化一次，防止掉线重连导致的 Socket 泄漏
                    // ========================================================
                    DistributedSoftBus::instance().init();

                    // 1. 创建独立的软总线监听线程 (高优先级)
                    uint32_t* bus_stack = new uint32_t[1024];
                    Scheduler::instance().create_task(softbus_listener_entry, bus_stack, 1024 * sizeof(uint32_t),
                                                      TaskPriority::High);

                    // 2. 利用定时器，每 3000ms 异步非阻塞发送一次心跳广播
                    TimerManager::instance().start_timer(3000, TimerType::Periodic, beacon_timer_callback);

                    network_services_started = true;
                }

                ip_assigned = true;
            }
        } else {
            // 如果还没拿到，或者租期到期掉线，重置状态
            ip_assigned = false;
        }

        // 释放 CPU，每秒检查一次
        sleep(1000);
    }
}

// ========================================================
// 网络主轮询任务：由调度器在后台运行
// ========================================================
void NetApp::start_network() {
    int console_fd = open("/dev/uart0", 0);
    write(console_fd, "[Network] Starting lwIP TCP/IP Stack...\r\n", 41);
    close(console_fd);

    // 3. 启动 lwIP 内部守护进程并注册完成回调（共用 tcpip_init_done_cb 处理后续 DHCP 和网卡添加）
    tcpip_init(tcpip_init_done_cb, nullptr);

    bool ip_assigned = false;
    static bool network_services_started = false;
    console_fd = open("/dev/uart0", 0);

    // 轮询等待 DHCP 服务器分配 IP (与有线逻辑相同)
    while (true) {
        if (dhcp_supplied_address(&g_netif)) {
            if (!ip_assigned) {
                char msg[128];
                int len = 0;
                auto append = [&](const char* s) {
                    while (*s && len < (int)sizeof(msg) - 1)
                        msg[len++] = *s++;
                };
                auto append_num = [&](uint8_t n) {
                    char tmp[4];
                    int i = 0;
                    if (n == 0)
                        tmp[i++] = '0';
                    while (n > 0) {
                        tmp[i++] = (n % 10) + '0';
                        n /= 10;
                    }
                    while (i > 0 && len < (int)sizeof(msg) - 1)
                        msg[len++] = tmp[--i];
                };

                append("\r\n\r\n🌐 [WiFi DHCP] Success! auroraOS got IP Address: ");
                append_num(ip4_addr1(netif_ip4_addr(&g_netif)));
                append(".");
                append_num(ip4_addr2(netif_ip4_addr(&g_netif)));
                append(".");
                append_num(ip4_addr3(netif_ip4_addr(&g_netif)));
                append(".");
                append_num(ip4_addr4(netif_ip4_addr(&g_netif)));
                append("\r\n\r\n");

                msg[len] = '\0';
                write(console_fd, msg, len);

                if (!network_services_started) {
                    DistributedSoftBus::instance().init();
                    uint32_t* bus_stack = new uint32_t[1024];
                    Scheduler::instance().create_task(softbus_listener_entry, bus_stack, 1024 * sizeof(uint32_t),
                                                      TaskPriority::High);
                    TimerManager::instance().start_timer(3000, TimerType::Periodic, beacon_timer_callback);
                    network_services_started = true;
                }
                ip_assigned = true;
            }
        } else {
            ip_assigned = false;
        }
        sleep(1000);
    }
}
