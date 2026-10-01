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

    // kind: 0 = PARTIAL（阻止深睡，可息屏），1 = SCREEN（屏幕常亮）
    // timeout_ms: 锁自动过期时间，防止 App 忘记释放导致永久耗电
    bool acquire_wake_lock(uint8_t kind = 0, uint32_t timeout_ms = 0xFFFFFFFFu);
    bool release_wake_lock();
    bool release_all_wake_locks();
    uint8_t get_battery_level();
    bool get_battery_info(BatteryInfo& out) const;
    PowerState get_power_state();
    bool set_profile(uint32_t profile);
    bool get_profile(uint32_t& out) const;
    int get_wake_lock_count();
    bool can_sleep();

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
