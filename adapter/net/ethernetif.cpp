#include "lwip/opt.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/etharp.h"
#include "../../net/net_device.hpp"
#include "../../net/eth_driver.hpp"
#include "../../net/packet_capture.hpp"
#include "../../services/firewall/firewall_client.hpp"
#include "task.hpp"
#include "syscall.hpp"

// 1. 鍙戦€侀€傞厤锛歭wIP 鍙綉鍗″彂鍖呮椂浼氳Е鍙戞鍑芥暟
static err_t low_level_output(struct netif *netif, struct pbuf *p) {
    static uint8_t tx_buffer[1514];
    int len = 0;

    // pbuf 鍙兘鏄摼寮忕粨鏋勶紝闇€瑕佹妸澶氭鍐呭瓨鍚堝苟鎴愪竴涓畬鏁翠互澶綉甯?
    for (struct pbuf *q = p; q != NULL; q = q->next) {
        for (int i = 0; i < q->len && (len + i) < 1514; i++) {
            tx_buffer[len + i] = static_cast<uint8_t*>(q->payload)[i];
        }
        len += q->len;
    }

    // 闆舵嫹璐濆梾鎺㈡嫤鎴?(PacketTap Hook) 鈥?鎹曡幏鍙戝嚭鐨勫寘
    PacketCapture::instance().tap_tx_packet(tx_buffer, len);

    // 闃茬伀澧欏紩鎿庤繃婊?(Firewall Engine) 鈥?璁板綍鍑虹珯杩炴帴鐘舵€?
    if (!auroraos::firewall::FirewallClient::instance().process_packet(tx_buffer, len, "en0")) {
        // 濡傛灉闃茬伀澧欐嫆缁濊鍖咃紝鐩存帴涓㈠純
        return ERR_IF;
    }

    // 璋冪敤鎴戜滑鍦ㄤ笂涓€鑺傛墜鍐欑殑缃戝崱搴曞眰鍙戦€佹帴鍙ｏ紒
    NetDevice* device = static_cast<NetDevice*>(netif->state);
    if (device && device->send_frame(tx_buffer, len)) {
        return ERR_OK;
    }
    return ERR_IF;
}

// 2. 鎺ユ敹閫傞厤锛氭垜浠殑鎺ユ敹浠诲姟璇诲彇鍒扮綉鍗?FIFO 鏁版嵁鍚庯紝杞崲鎴?pbuf
static struct pbuf* low_level_input(struct netif *netif) {
    static uint8_t rx_buffer[1514];
    NetDevice* device = static_cast<NetDevice*>(netif->state);
    if (!device) return nullptr;

    // 浠庡簳灞傜綉鍗¤鍙栦竴涓互澶綉甯?
    int bytes_read = device->receive_frame(rx_buffer, sizeof(rx_buffer));
    if (bytes_read <= 0) return nullptr;

    // 闆舵嫹璐濆梾鎺㈡嫤鎴?(PacketTap Hook)
    PacketCapture::instance().tap_rx_packet(rx_buffer, bytes_read);

    // 闃茬伀澧欏紩鎿庤繃婊?(Firewall Engine)
    if (!auroraos::firewall::FirewallClient::instance().process_packet(rx_buffer, bytes_read, "en0")) {
        // 濡傛灉闃茬伀澧欐嫆缁濊鍖咃紝鐩存帴涓㈠純
        return nullptr;
    }

    // 鍚?lwIP 鐢宠涓€涓笓灞炵殑鍗忚缂撳啿鍖?pbuf
    struct pbuf *p = pbuf_alloc(PBUF_RAW, bytes_read, PBUF_POOL);
    if (p != nullptr) {
        // 灏嗘敹鍒扮殑瀛楄妭娴佹嫹璐濊繘 pbuf 閾句腑
        pbuf_take(p, rx_buffer, bytes_read);
    }
    return p;
}

// 3. lwIP 缃戝崱鎺ユ敹瀹堟姢绾跨▼锛氫笉鏂疆璇㈠簳鍙ｅ苟鎺ㄥ叆鍗忚鏍?
// 娉ㄦ剰锛氭垜浠殑浠诲姟鍒涘缓鍑芥暟涓嶆敮鎸佸甫鍙傛暟锛岃繖閲屼娇鐢ㄥ叏灞€鍙橀噺浼犻€?netif
extern struct netif g_netif;

void ethernetif_input_task(void) {
    struct netif *netif = &g_netif;
    while (true) {
        struct pbuf *p = low_level_input(netif);
        if (p != nullptr) {
            // 閫氳繃鎺ュ彛鎶婂抚鎺ㄥ叆 lwIP 鐨?TCPIP 涓诲畧鎶よ繘绋嬪鐞嗭紒
            if (netif->input(p, netif) != ERR_OK) {
                pbuf_free(p);
            }
        } else {
            Scheduler::instance().sleep_ms(5); // 鏃犳暟鎹椂璁╁嚭 CPU
        }
    }
}

// 4. 缃戝崱娉ㄥ唽鍒濆鍖栧嚱鏁帮細鎸傝浇鍒?lwIP 绯荤粺鏃惰璋冪敤
err_t ethernetif_init(struct netif *netif) {
    auroraos::firewall::FirewallClient::instance().init();
    netif->name[0] = 'e'; netif->name[1] = 'n'; // 缃戝崱鍚? "en0"
    netif->output = etharp_output;              // ARP 瑙ｆ瀽缁戝畾
    netif->linkoutput = low_level_output;       // 瀹為檯鐗╃悊鍙戦€佺粦瀹?
    netif->mtu = 1500;
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_LINK_UP | NETIF_FLAG_UP;

    // 浠庡簳灞傜‖浠堕┍鍔ㄦ媺鍙?MAC 鍦板潃
    NetDevice* device = static_cast<NetDevice*>(netif->state);
    if (device) {
        const uint8_t* hw_mac = device->get_mac();
        netif->hwaddr_len = 6;
        for (int i = 0; i < 6; i++) netif->hwaddr[i] = hw_mac[i];
    }

    // 鍒濆鍖栧梾鎺㈡ā鍧楀苟鎸傝浇 /dev/pcap0
    PacketCapture::instance().init();

    sys_print("[ethernetif] lwIP Network Interface 'en0' bound to NetDevice successfully!\r\n");
    return ERR_OK;
}


