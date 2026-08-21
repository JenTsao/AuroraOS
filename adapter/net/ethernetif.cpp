#include "lwip/opt.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/etharp.h"
#include "../../net/net_device.hpp"
#include "../../net/eth_driver.hpp"
#include "../../net/packet_capture.hpp"
#include "../../services/firewall/firewall_client.hpp"
#include "../../security/ids/ids_engine.hpp"
#include "task.hpp"
#include "syscall.hpp"

// 1. 发送适配：lwIP 叫网卡发包时会触发此函数
static err_t low_level_output(struct netif* netif, struct pbuf* p) {
    static uint8_t tx_buffer[1514];
    int len = 0;

    // pbuf 可能是链式结构，需要把多段内存合并成一个完整以太网帧
    for (struct pbuf* q = p; q != NULL; q = q->next) {
        for (int i = 0; i < q->len && (len + i) < 1514; i++) {
            tx_buffer[len + i] = static_cast<uint8_t*>(q->payload)[i];
        }
        len += q->len;
    }

    // 零拷贝嗅探拦截(PacketTap Hook) 捕获发出的包
    PacketCapture::instance().tap_tx_packet(tx_buffer, len);

    // 网络入侵检测(NIDS) 观察所有出站流量（被动检测，不丢包）
    aurora::ids::IdsEngine::instance().process_packet(tx_buffer, len);

    // 防火墙引擎过滤(Firewall Engine) 记录出站连接状态
    if (!auroraos::firewall::FirewallClient::instance().process_packet(tx_buffer, len, "en0")) {
        // 如果防火墙拒绝该包，直接丢弃
        return ERR_IF;
    }

    // 调用我们在上一节手写的网卡底层发送接口！
    NetDevice* device = static_cast<NetDevice*>(netif->state);
    if (device && device->send_frame(tx_buffer, len)) {
        return ERR_OK;
    }
    return ERR_IF;
}

// 2. 接收适配：我们的接收任务读取到网卡 FIFO 数据后，转换为 pbuf
static struct pbuf* low_level_input(struct netif* netif) {
    static uint8_t rx_buffer[1514];
    NetDevice* device = static_cast<NetDevice*>(netif->state);
    if (!device)
        return nullptr;

    // 从底层网卡读取一个以太网帧
    int bytes_read = device->receive_frame(rx_buffer, sizeof(rx_buffer));
    if (bytes_read <= 0)
        return nullptr;

    // 零拷贝嗅探拦截(PacketTap Hook)
    PacketCapture::instance().tap_rx_packet(rx_buffer, bytes_read);

    // 网络入侵检测(NIDS) 观察所有入站流量（被动检测，不丢包）
    aurora::ids::IdsEngine::instance().process_packet(rx_buffer, bytes_read);

    // 防火墙引擎过滤(Firewall Engine)
    if (!auroraos::firewall::FirewallClient::instance().process_packet(rx_buffer, bytes_read, "en0")) {
        // 如果防火墙拒绝该包，直接丢弃
        return nullptr;
    }

    // 向 lwIP 申请一个专属的协议缓冲区 pbuf
    struct pbuf* p = pbuf_alloc(PBUF_RAW, bytes_read, PBUF_POOL);
    if (p != nullptr) {
        // 将收到的字节流拷贝进 pbuf 链中
        pbuf_take(p, rx_buffer, bytes_read);
    }
    return p;
}

// 3. lwIP 网卡接收守护线程：不断轮询底口并推入协议栈
// 注意：我们的任务创建函数不支持带参数，这里使用全局变量传递 netif
extern struct netif g_netif;

void ethernetif_input_task(void) {
    struct netif* netif = &g_netif;
    while (true) {
        struct pbuf* p = low_level_input(netif);
        if (p != nullptr) {
            // 通过接口把帧推入 lwIP 的 TCPIP 主守护进程处理！
            if (netif->input(p, netif) != ERR_OK) {
                pbuf_free(p);
            }
        } else {
            Scheduler::instance().sleep_ms(5); // 无数据时让出 CPU
        }
    }
}

// 4. 网卡注册初始化函数：挂载到 lwIP 系统时被调用
err_t ethernetif_init(struct netif* netif) {
    auroraos::firewall::FirewallClient::instance().init();
    netif->name[0] = 'e';
    netif->name[1] = 'n';                 // 网卡名 "en0"
    netif->output = etharp_output;        // ARP 解析绑定
    netif->linkoutput = low_level_output; // 实际物理发送绑定
    netif->mtu = 1500;
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_LINK_UP | NETIF_FLAG_UP;

    // 从底层硬件驱动拉取 MAC 地址
    NetDevice* device = static_cast<NetDevice*>(netif->state);
    if (device) {
        const uint8_t* hw_mac = device->get_mac();
        netif->hwaddr_len = 6;
        for (int i = 0; i < 6; i++)
            netif->hwaddr[i] = hw_mac[i];
    }

    // 初始化嗅探模块并挂载 /dev/pcap0
    PacketCapture::instance().init();

    // 初始化 NIDS 并挂载 /proc/ids
    aurora::ids::IdsEngine::instance().init();

    sys_print("[ethernetif] lwIP Network Interface 'en0' bound to NetDevice successfully!\r\n");
    return ERR_OK;
}
