#ifndef FIREWALL_SERVICE_HPP
#define FIREWALL_SERVICE_HPP

#include "firewall_ipc.hpp"

namespace auroraos {
namespace firewall {

class FirewallServer {
public:
    static FirewallServer& instance() {
        static FirewallServer server;
        return server;
    }

    void init();
    void process_request(const FirewallRequest& req, FirewallReply& reply);
    [[noreturn]] void run();

private:
    FirewallServer();
    bool initialized_ = false;
};

} // namespace firewall
} // namespace auroraos

#endif // FIREWALL_SERVICE_HPP
