#ifndef AURORAOS_POWER_CLIENT_HPP
#define AURORAOS_POWER_CLIENT_HPP

#include "power_ipc.hpp"
#include <stdint.h>

namespace auroraos {
namespace power_service {

class PowerClient {
public:
    static PowerClient& instance() {
        static PowerClient client;
        return client;
    }

    void set_endpoint(int ep_cap);
    int get_endpoint() const;

    bool acquire_wake_lock();
    bool release_wake_lock();
    uint8_t get_battery_level();
    PowerState get_power_state();

    // 直接分发器（用于单元测试和无 IPC 调度环境）
    using DirectHandler = void (*)(const PowerRequest& req, PowerReply& reply);
    static void set_direct_handler(DirectHandler handler);

private:
    PowerClient();
    int endpoint_cap_ = -1;
    static DirectHandler direct_handler_;
};

} // namespace power_service
} // namespace auroraos

#endif // AURORAOS_POWER_CLIENT_HPP
