#include "lwip/opt.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/etharp.h"
#include "../../net/net_device.hpp"
#include "../../net/link_layer.hpp"
#include "../../net/protocol_registry.hpp"
#include "../../net/packet_capture.hpp"
#include "../../services/firewall/firewall_client.hpp"
#include "../../security/ids/ids_engine.hpp"
#include "task.hpp"
#include "syscall.hpp"

namespace {

// NIDS 链路层被动嗅探钩子（通过 LinkLayer 注册，不再硬编码进收发路径）
void ids_link_tap(const uint8_t* frame, int len, auroraos::net::PacketDirection dir, auroraos::net::LinkType link_type,
                  const char* ifname, void* user_ctx) noexcept {
    (void)dir;
    (void)ifname;
    (void)user_ctx;
    if (link_type == auroraos::net::LinkType::Ethernet && frame && len > 0) {
        aurora::ids::IdsEngine::instance().process_packet(frame, len);
    }
}

// 防火墙链路层过滤钩子（通过 LinkLayer 注册，不再硬编码进收发路径）
bool firewall_link_filter(const uint8_t* frame, int len, auroraos::net::PacketDirection dir,
                          auroraos::net::LinkType link_type, const char* ifname, void* user_ctx) noexcept {
    (void)dir;
    (void)user_ctx;
    if (link_type != auroraos::net::LinkType::Ethernet) {
        return true;
    }
    return auroraos::firewall::FirewallClient::instance().process_packet(frame, len, ifname ? ifname : "en0");
}

// lwIP 作为可插拔 L2 协议处理器注册到 LinkLayer
class LwipLinkProtocolHandler final : public auroraos::net::ILinkProtocolHandler {
public:
    const char* protocol_name() const noexcept override {
        return "lwip_eth";
    }

    bool accepts(auroraos::net::LinkType link_type, uint16_t channel_or_ethtype) const noexcept override {
        (void)channel_or_ethtype;
        return link_type == auroraos::net::LinkType::Ethernet;
    }

    auroraos::net::ProtocolVerdict on_frame(const uint8_t* frame, int len, auroraos::net::LinkType link_type,
                                            uint16_t channel_or_ethtype, const char* ifname) noexcept override {
        (void)frame;
        (void)len;
        (void)link_type;
        (void)channel_or_ethtype;
        (void)ifname;
        return auroraos::net::ProtocolVerdict::Pass;
    }
};

// lwIP TCP/IP 栈作为可插拔 L4 兜底协议处理器注册到 ProtocolRegistry
class LwipL4ProtocolHandler final : public auroraos::net::IProtocolHandler {
public:
    const char* name() const noexcept override {
        return "lwip_tcpip";
    }

    auroraos::net::ProtocolVerdict on_packet(const auroraos::net::L4PacketContext& ctx, const uint8_t* payload,
                                             size_t payload_len) noexcept override {
        (void)ctx;
        (void)payload;
        (void)payload_len;
        return auroraos::net::ProtocolVerdict::Pass;
    }
};

LwipLinkProtocolHandler g_lwip_link_handler;
LwipL4ProtocolHandler g_lwip_l4_handler;

} // namespace

// 1. 发送适配：lwIP 叫网卡发包时会触发此函数，统一经由 LinkLayer 管线（Tap -> Filter -> NetDevice）
static err_t low_level_output(struct netif* netif, struct pbuf* p) {
    static uint8_t tx_buffer[1514];
    int len = 0;

    // pbuf 可能是链式结构，需要把多段内存合并成一个完整以太网帧
    for (struct pbuf* q = p; q != NULL; q = q->next) {
        for (int i = 0; i < q->len && (len + i) < 1514; i++) {
            tx_buffer[len + i] = static_cast<uint8_t*>(q->payload)[i];
        }
        len += q->len;
        if (len > 1514) {
            len = 1514; // 累计长度与实拷贝字节数保持一致（拷贝已按 1514 截断）
        }
    }

    NetDevice* device = static_cast<NetDevice*>(netif->state);
    if (auroraos::net::LinkLayer::instance().transmit_frame("en0", tx_buffer, len, device)) {
        return ERR_OK;
    }
    return ERR_IF;
}

// 2. 接收适配：统一经由 LinkLayer 管线拉取帧、执行 Tap / Filter / L4 ProtocolRegistry 分发后转交 pbuf
static struct pbuf* low_level_input(struct netif* netif) {
    static uint8_t rx_buffer[1514];
    NetDevice* device = static_cast<NetDevice*>(netif->state);
    if (!device)
        return nullptr;

    int bytes_read = auroraos::net::LinkLayer::instance().receive_frame("en0", rx_buffer, sizeof(rx_buffer), device);
    if (bytes_read <= 0)
        return nullptr;

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
    auto& link = auroraos::net::LinkLayer::instance();
    auto& proto_reg = auroraos::net::ProtocolRegistry::instance();

    auroraos::firewall::FirewallClient::instance().init();
    netif->name[0] = 'e';
    netif->name[1] = 'n';                 // 网卡名 "en0"
    netif->output = etharp_output;        // ARP 解析绑定
    netif->linkoutput = low_level_output; // 实际物理发送绑定
    netif->mtu = 1500;
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_LINK_UP | NETIF_FLAG_UP;

    // 从底层硬件驱动拉取 MAC 地址并注册到 LinkLayer
    NetDevice* device = static_cast<NetDevice*>(netif->state);
    if (device) {
        link.register_device("en0", device, auroraos::net::LinkType::Ethernet);
        netif->mtu = device->get_mtu();
        const uint8_t* hw_mac = device->get_mac();
        netif->hwaddr_len = 6;
        for (int i = 0; i < 6; i++)
            netif->hwaddr[i] = hw_mac[i];
    }

    // 初始化嗅探模块并挂载 /dev/pcap0（内部自动向 LinkLayer 注册 ILinkTap）
    PacketCapture::instance().init();

    // 初始化 NIDS 并挂载 /proc/ids，同时注册为 LinkLayer Tap
    aurora::ids::IdsEngine::instance().init();
    // 注册失败（表满）不得静默：否则 NIDS/防火墙/lwIP 兜底挂载悄然缺失
    if (!link.register_tap_fn("nids", ids_link_tap)) {
        sys_print("[ethernetif] WARN: NIDS tap registration failed\r\n");
    }

    // 注册防火墙为 LinkLayer 帧过滤器
    if (!link.register_filter_fn("firewall", firewall_link_filter)) {
        sys_print("[ethernetif] WARN: firewall filter registration failed\r\n");
    }

    // 注册 lwIP 为 Ethernet L2 与 TCP/UDP/ICMP L4 协议处理器
    if (!link.register_link_protocol(&g_lwip_link_handler)) {
        sys_print("[ethernetif] WARN: lwIP link protocol registration failed\r\n");
    }
    if (!proto_reg.register_handler(auroraos::net::PROTO_TCP, auroraos::net::ProtocolRegistry::PORT_ANY,
                                    &g_lwip_l4_handler, auroraos::net::ProtocolRegistry::PRIORITY_FALLBACK) ||
        !proto_reg.register_handler(auroraos::net::PROTO_UDP, auroraos::net::ProtocolRegistry::PORT_ANY,
                                    &g_lwip_l4_handler, auroraos::net::ProtocolRegistry::PRIORITY_FALLBACK) ||
        !proto_reg.register_handler(auroraos::net::PROTO_ICMP, auroraos::net::ProtocolRegistry::PORT_ANY,
                                    &g_lwip_l4_handler, auroraos::net::ProtocolRegistry::PRIORITY_FALLBACK)) {
        sys_print("[ethernetif] WARN: lwIP L4 handler registration failed\r\n");
    }

    sys_print("[ethernetif] lwIP Network Interface 'en0' bound to LinkLayer & ProtocolRegistry!\r\n");
    return ERR_OK;
}
