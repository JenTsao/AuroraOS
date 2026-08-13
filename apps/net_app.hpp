#ifndef AURORA_NET_APP_HPP
#define AURORA_NET_APP_HPP

class NetApp {
public:
    // 网络主线程入口 (以太网)
    static void run_dhcp_client();

    // 网络主线程入口 (WiFi)
    static void start_network();
};

#endif
