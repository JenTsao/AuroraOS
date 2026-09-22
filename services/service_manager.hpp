#ifndef AURORAOS_SERVICE_MANAGER_HPP
#define AURORAOS_SERVICE_MANAGER_HPP

#include "service_registry.hpp"
#include <stdint.h>
#include <stddef.h>

namespace auroraos {
namespace services {

// 服务运行生命周期状态
enum class ServiceState : uint8_t {
    Stopped = 0,
    Starting,
    Running,
    Error
};

struct ServiceDescriptor {
    ServiceId id = ServiceId::Unknown;
    char name[16] = {0};
    ServiceState state = ServiceState::Stopped;
    int endpoint_cap = -1;
    uint32_t restart_count = 0;
};

// 集中式微服务生命周期与健康监控管理器
class ServiceManager {
public:
    static constexpr size_t MAX_SERVICES = 8;

    static ServiceManager& instance() {
        static ServiceManager manager;
        return manager;
    }

    void init_all_services();
    bool start_service(ServiceId id);
    bool stop_service(ServiceId id);
    bool restart_service(ServiceId id);

    ServiceState get_service_state(ServiceId id) const;
    const ServiceDescriptor* get_service_descriptor(ServiceId id) const;

    // 全系统服务健康诊断
    bool health_check() const;

    size_t get_active_service_count() const;
    void reset();

private:
    ServiceManager() {
        reset();
    }

    ServiceDescriptor services_[MAX_SERVICES];
    size_t service_count_ = 0;
};

} // namespace services
} // namespace auroraos

#endif // AURORAOS_SERVICE_MANAGER_HPP
