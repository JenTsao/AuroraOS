#ifndef POWER_SERVICE_HPP
#define POWER_SERVICE_HPP

#include "power_ipc.hpp"
#include "power_manager.hpp"

namespace auroraos {
namespace power_service {

class PowerServer {
public:
    static PowerServer& instance() {
        static PowerServer server;
        return server;
    }

    void init();
    void process_request(const PowerRequest& req, PowerReply& reply, uint32_t caller_id = 0);
    [[noreturn]] void run();

private:
    PowerServer();
    bool initialized_ = false;
};

extern int g_power_service_ep;

} // namespace power_service
} // namespace auroraos

#endif // POWER_SERVICE_HPP
