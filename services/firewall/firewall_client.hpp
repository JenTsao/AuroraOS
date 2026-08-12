#ifndef AURORAOS_FIREWALL_CLIENT_HPP
#define AURORAOS_FIREWALL_CLIENT_HPP

#include <stdint.h>

namespace auroraos {
namespace firewall {

class FirewallClient {
public:
    static FirewallClient& instance() {
        static FirewallClient client;
        return client;
    }

    void init();
    void set_endpoint(int ep_cap);

    bool process_packet(const uint8_t* packet, int len, const char* interface);

private:
    FirewallClient() = default;
    int service_ep_cap_ = -1;
};

} // namespace firewall
} // namespace auroraos

#endif // AURORAOS_FIREWALL_CLIENT_HPP
