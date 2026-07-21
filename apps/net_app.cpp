#include "net_app.hpp"
#include "posix.hpp" // 使用我们的标准 sleep 和 print 封装
#include "vfs.hpp"
#include "timer.hpp" // 引入软件定时器
#include "../net/distributed_bus.hpp" // 引入软总线
#include "../net/wifi_driver.hpp"     // 引入 WiFi 驱动

// 引入 lwIP 核心头文件
#include "lwip/netif.h"
#include "lwip/tcpip.h"
#include "lwip/dhcp.h"
#include "netif/ethernet.h"

// 声明你在 adapter/net/ethernetif.cpp 中写好的底层网卡初始化函数
extern err_t ethernetif_init(struct netif *netif);

// 全局网卡结构体
struct netif g_netif;

// ========================================================
// lwIP 核心协议栈初始化完成后的回调函数
// ========================================================
static void tcpip_init_done_cb(void* /*arg*/) {
    ip4_addr_t ipaddr, netmask, gw;
    
    // 初始化时 IP 全设为 0，因为我们要通过 DHCP 获取
    IP4_ADDR(&ipaddr, 0, 0, 0, 0);
    IP4_ADDR(&netmask, 0, 0, 0, 0);
    IP4_ADDR(&gw, 0, 0, 0, 0);

    // 1. 将以太网卡挂载到 lwIP 协议栈，并绑定底层驱动和输入入口
    netif_add(&g_netif, &ipaddr, &netmask, &gw, nullptr, ethernetif_init, tcpip_input);
    
    // 2. 设置为默认网卡并启动
    netif_set_default(&g_netif);
    netif_set_up(&g_netif);

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
                    while (*s && len < (int)sizeof(msg) - 1) msg[len++] = *s++; 
                };
                auto append_num = [&](uint8_t n) {
                    char tmp[4]; int i = 0;
                    if (n == 0) tmp[i++] = '0';
                    while (n > 0) { tmp[i++] = (n % 10) + '0'; n /= 10; }
                    while (i > 0 && len < (int)sizeof(msg) - 1) msg[len++] = tmp[--i];
                };

                append("\r\n\r\n🌐 [DHCP] Success! auroraOS got IP Address: ");
                append_num(ip4_addr1(netif_ip4_addr(&g_netif))); append(".");
                append_num(ip4_addr2(netif_ip4_addr(&g_netif))); append(".");
                append_num(ip4_addr3(netif_ip4_addr(&g_netif))); append(".");
                append_num(ip4_addr4(netif_ip4_addr(&g_netif))); append("\r\n\r\n");

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
                    Scheduler::instance().create_task(softbus_listener_entry, bus_stack, 1024 * sizeof(uint32_t), TaskPriority::High);

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
// 无线网络主轮询任务：由调度器在后台运行
// ========================================================
void NetApp::init_wifi_and_dhcp(const char* ssid, const char* password) {
    int console_fd = open("/dev/uart0", 0);
    write(console_fd, "[Network] Starting WiFi Connection Sequence...\r\n", 48);

    // 1. 初始化网络驱动
#ifdef CONFIG_ESP_WIFI
    static auroraos::net::EspWifiDriver wifi_driver;
    if (!wifi_driver.init()) {
        write(console_fd, "[Network] WiFi Hardware Init Failed!\r\n", 38);
        close(console_fd);
        return;
    }

    if (!wifi_driver.connect(ssid, password)) {
        write(console_fd, "[Network] WiFi Connection Failed!\r\n", 35);
        close(console_fd);
        return;
    }

    // 2. 将驱动实例绑定给 lwIP 的 netif state
    g_netif.state = &wifi_driver;
#else
    // QEMU/有线以太网：跳过 WiFi 初始化，直接使用 Stellaris ETH
    (void)ssid;
    (void)password;
    write(console_fd, "[Network] Using Ethernet (Stellaris) for QEMU...\r\n", 52);
#endif

    write(console_fd, "[Network] WiFi Link UP. Starting lwIP TCP/IP Stack...\r\n", 55);
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
                    while (*s && len < (int)sizeof(msg) - 1) msg[len++] = *s++; 
                };
                auto append_num = [&](uint8_t n) {
                    char tmp[4]; int i = 0;
                    if (n == 0) tmp[i++] = '0';
                    while (n > 0) { tmp[i++] = (n % 10) + '0'; n /= 10; }
                    while (i > 0 && len < (int)sizeof(msg) - 1) msg[len++] = tmp[--i];
                };

                append("\r\n\r\n🌐 [WiFi DHCP] Success! auroraOS got IP Address: ");
                append_num(ip4_addr1(netif_ip4_addr(&g_netif))); append(".");
                append_num(ip4_addr2(netif_ip4_addr(&g_netif))); append(".");
                append_num(ip4_addr3(netif_ip4_addr(&g_netif))); append(".");
                append_num(ip4_addr4(netif_ip4_addr(&g_netif))); append("\r\n\r\n");

                msg[len] = '\0';
                write(console_fd, msg, len);
                
                if (!network_services_started) {
                    DistributedSoftBus::instance().init();
                    uint32_t* bus_stack = new uint32_t[1024];
                    Scheduler::instance().create_task(softbus_listener_entry, bus_stack, 1024 * sizeof(uint32_t), TaskPriority::High);
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
