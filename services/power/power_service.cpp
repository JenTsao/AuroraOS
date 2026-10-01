#include "power_service.hpp"
#include "syscall.hpp"

namespace auroraos {
namespace power_service {

int g_power_service_ep = 7; // Default placeholder capability ID

PowerServer::PowerServer() : initialized_(false) {}

void PowerServer::init() {
    if (!initialized_) {
        PowerManager::instance().reset();
        initialized_ = true;
    }
}

void PowerServer::process_request(const PowerRequest& req, PowerReply& reply, uint32_t caller_id) {
    reply.status = -1;

    switch (req.opcode) {
    case PowerOpcode::AcquireWakeLock: {
        const auto kind = static_cast<WakeLockKind>(req.lock.kind);
        // 越界的锁类型一律按最弱的 PARTIAL 处理，避免 App 传入非法值获得屏幕常亮权限
        const WakeLockKind safe_kind = (kind == WakeLockKind::SCREEN) ? WakeLockKind::SCREEN : WakeLockKind::PARTIAL;
        reply.status = PowerManager::instance().acquire_wake_lock(caller_id, safe_kind, req.lock.timeout_ms) ? 0 : -1;
        break;
    }
    case PowerOpcode::ReleaseWakeLock:
        reply.status = PowerManager::instance().release_wake_lock(caller_id) ? 0 : -1;
        break;
    case PowerOpcode::ReleaseAllWakeLocks:
        PowerManager::instance().release_all_wake_locks(caller_id);
        reply.status = 0;
        break;
    case PowerOpcode::GetBatteryLevel:
        reply.data.battery_percent = PowerManager::instance().get_battery_level();
        reply.status = 0;
        break;
    case PowerOpcode::GetBatteryInfo: {
        const BatteryInfo info = PowerManager::instance().get_battery_info();
        reply.data.battery.level = info.level;
        reply.data.battery.health = info.health;
        reply.data.battery.charge_state = info.charge_state;
        reply.data.battery.plugged = info.plugged;
        reply.data.battery.voltage_mv = info.voltage_mv;
        reply.data.battery.temperature_c = info.temperature_c;
        reply.status = 0;
        break;
    }
    case PowerOpcode::GetPowerState:
        reply.data.current_state = PowerManager::instance().get_current_state();
        reply.status = 0;
        break;
    case PowerOpcode::SetProfile:
        if (req.profile <= static_cast<uint32_t>(PowerProfile::ULTRA_SAVER)) {
            PowerManager::instance().set_profile(static_cast<PowerProfile>(req.profile));
            reply.status = 0;
        }
        break;
    case PowerOpcode::GetProfile:
        reply.data.profile = static_cast<uint32_t>(PowerManager::instance().get_profile());
        reply.status = 0;
        break;
    case PowerOpcode::GetWakeLockCount:
        reply.data.count = PowerManager::instance().get_wake_lock_count();
        reply.status = 0;
        break;
    case PowerOpcode::CanSleep:
        reply.data.flag = PowerManager::instance().can_sleep() ? 1 : 0;
        reply.status = 0;
        break;
    default:
        reply.status = -1; // 未知 opcode：拒绝而不是静默成功
        break;
    }
}

[[noreturn]] void PowerServer::run() {
    init();
    uint32_t ep_cap = static_cast<uint32_t>(g_power_service_ep);

    while (true) {
        struct {
            uint32_t msg_type;
            PowerRequest req;
        } ipc_msg;

        uint32_t caller_cap = 0;
        // sys_ipc_receive 为阻塞语义（void 返回），失败场景不存在
        sys_ipc_receive(ep_cap, &ipc_msg, sizeof(ipc_msg), &caller_cap);

        PowerReply reply;
        reply.status = -1;

        if (ipc_msg.msg_type == 1) {
            process_request(ipc_msg.req, reply, caller_cap);
        }

        sys_ipc_reply(caller_cap, &reply, sizeof(reply));
    }
}

} // namespace power_service
} // namespace auroraos

extern "C" void power_service_entry() {
    auroraos::power_service::PowerServer::instance().run();
}
