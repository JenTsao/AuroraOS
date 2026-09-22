#include "service_manager.hpp"
#include "vfs/vfs_service.hpp"
#include "net/net_service.hpp"
#include "firewall/firewall_service.hpp"
#include "sensor/sensor_service.hpp"
#include "power/power_service.hpp"
#include "display/display_service.hpp"
#include <string.h>

namespace auroraos {
namespace services {

void ServiceManager::reset() {
    for (size_t i = 0; i < MAX_SERVICES; ++i) {
        services_[i].id = ServiceId::Unknown;
        services_[i].name[0] = '\0';
        services_[i].state = ServiceState::Stopped;
        services_[i].endpoint_cap = -1;
        services_[i].restart_count = 0;
    }
    service_count_ = 0;
}

void ServiceManager::init_all_services() {
    reset();

    // 1. 初始化底层服务端实例
    vfs::VfsServer::instance().init();
    net::NetServer::instance().init();
    firewall::FirewallServer::instance().init();
    sensor_service::SensorServer::instance().init();
    power_service::PowerServer::instance().init();
    display::DisplayServer::instance().init();

    // 2. 注册服务描述符并发布端点至 ServiceRegistry
    auto& reg = ServiceRegistry::instance();

    struct InitialService {
        ServiceId id;
        const char* name;
        int default_ep;
    };

    const InitialService init_list[] = {
        {ServiceId::Vfs, "VfsService", 5},
        {ServiceId::Net, "NetService", 2},
        {ServiceId::Firewall, "FirewallService", 4},
        {ServiceId::Sensor, "SensorService", 6},
        {ServiceId::Power, "PowerService", 7},
        {ServiceId::UI, "DisplayService", 8},
    };

    service_count_ = sizeof(init_list) / sizeof(init_list[0]);
    for (size_t i = 0; i < service_count_; ++i) {
        services_[i].id = init_list[i].id;
        strncpy(services_[i].name, init_list[i].name, sizeof(services_[i].name) - 1);
        services_[i].name[sizeof(services_[i].name) - 1] = '\0';
        services_[i].endpoint_cap = init_list[i].default_ep;
        services_[i].state = ServiceState::Running;
        services_[i].restart_count = 0;

        reg.register_service(init_list[i].id, init_list[i].default_ep, init_list[i].name);
    }
}

bool ServiceManager::start_service(ServiceId id) {
    for (size_t i = 0; i < service_count_; ++i) {
        if (services_[i].id == id) {
            services_[i].state = ServiceState::Running;
            ServiceRegistry::instance().register_service(id, services_[i].endpoint_cap, services_[i].name);
            return true;
        }
    }
    return false;
}

bool ServiceManager::stop_service(ServiceId id) {
    for (size_t i = 0; i < service_count_; ++i) {
        if (services_[i].id == id) {
            services_[i].state = ServiceState::Stopped;
            ServiceRegistry::instance().unregister_service(id);
            return true;
        }
    }
    return false;
}

bool ServiceManager::restart_service(ServiceId id) {
    for (size_t i = 0; i < service_count_; ++i) {
        if (services_[i].id == id) {
            services_[i].restart_count++;
            services_[i].state = ServiceState::Running;
            ServiceRegistry::instance().register_service(id, services_[i].endpoint_cap, services_[i].name);
            return true;
        }
    }
    return false;
}

ServiceState ServiceManager::get_service_state(ServiceId id) const {
    for (size_t i = 0; i < service_count_; ++i) {
        if (services_[i].id == id) {
            return services_[i].state;
        }
    }
    return ServiceState::Stopped;
}

const ServiceDescriptor* ServiceManager::get_service_descriptor(ServiceId id) const {
    for (size_t i = 0; i < service_count_; ++i) {
        if (services_[i].id == id) {
            return &services_[i];
        }
    }
    return nullptr;
}

bool ServiceManager::health_check() const {
    if (service_count_ == 0) {
        return false;
    }
    for (size_t i = 0; i < service_count_; ++i) {
        if (services_[i].state != ServiceState::Running) {
            return false;
        }
        if (!ServiceRegistry::instance().is_service_available(services_[i].id)) {
            return false;
        }
    }
    return true;
}

size_t ServiceManager::get_active_service_count() const {
    size_t count = 0;
    for (size_t i = 0; i < service_count_; ++i) {
        if (services_[i].state == ServiceState::Running) {
            count++;
        }
    }
    return count;
}

} // namespace services
} // namespace auroraos
