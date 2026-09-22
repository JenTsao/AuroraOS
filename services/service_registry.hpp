#ifndef AURORAOS_SERVICE_REGISTRY_HPP
#define AURORAOS_SERVICE_REGISTRY_HPP

#include <stdint.h>
#include <stddef.h>
#include <string.h>

namespace auroraos {
namespace services {

// 系统微服务标准标识符
enum class ServiceId : uint8_t {
    Unknown = 0,
    Vfs = 1,       // 虚拟文件系统服务
    Net = 2,       // 网络套接字服务 (lwIP)
    Firewall = 3,  // 防火墙与流量控制服务
    Sensor = 4,    // 传感器聚合服务
    Power = 5,     // 电源管理与 WakeLock 服务
    UI = 6,        // 显示与窗口渲染服务
    Security = 7,  // 安全审计与入侵检测服务
    MaxServices = 8
};

struct ServiceEntry {
    ServiceId id = ServiceId::Unknown;
    int endpoint_cap = -1;
    char name[16] = {0};
    bool active = false;
};

// 集中式零堆分配微服务注册中心
class ServiceRegistry {
public:
    static constexpr size_t MAX_REGISTRY_ENTRIES = 16;

    static ServiceRegistry& instance() {
        static ServiceRegistry registry;
        return registry;
    }

    void reset() {
        for (size_t i = 0; i < MAX_REGISTRY_ENTRIES; ++i) {
            entries_[i].id = ServiceId::Unknown;
            entries_[i].endpoint_cap = -1;
            entries_[i].name[0] = '\0';
            entries_[i].active = false;
        }
        count_ = 0;
    }

    bool register_service(ServiceId id, int endpoint_cap, const char* name = nullptr) {
        if (id == ServiceId::Unknown || endpoint_cap < 0) {
            return false;
        }

        // 如果已存在则就地更新
        for (size_t i = 0; i < count_; ++i) {
            if (entries_[i].id == id) {
                entries_[i].endpoint_cap = endpoint_cap;
                entries_[i].active = true;
                if (name) {
                    strncpy(entries_[i].name, name, sizeof(entries_[i].name) - 1);
                    entries_[i].name[sizeof(entries_[i].name) - 1] = '\0';
                }
                return true;
            }
        }

        if (count_ >= MAX_REGISTRY_ENTRIES) {
            return false;
        }

        entries_[count_].id = id;
        entries_[count_].endpoint_cap = endpoint_cap;
        entries_[count_].active = true;
        if (name) {
            strncpy(entries_[count_].name, name, sizeof(entries_[count_].name) - 1);
            entries_[count_].name[sizeof(entries_[count_].name) - 1] = '\0';
        } else {
            entries_[count_].name[0] = '\0';
        }
        count_++;
        return true;
    }

    bool unregister_service(ServiceId id) {
        if (id == ServiceId::Unknown) {
            return false;
        }

        for (size_t i = 0; i < count_; ++i) {
            if (entries_[i].id == id) {
                for (size_t j = i; j < count_ - 1; ++j) {
                    entries_[j] = entries_[j + 1];
                }
                entries_[count_ - 1].id = ServiceId::Unknown;
                entries_[count_ - 1].endpoint_cap = -1;
                entries_[count_ - 1].name[0] = '\0';
                entries_[count_ - 1].active = false;
                count_--;
                return true;
            }
        }
        return false;
    }

    int get_service_endpoint(ServiceId id) const {
        for (size_t i = 0; i < count_; ++i) {
            if (entries_[i].id == id && entries_[i].active) {
                return entries_[i].endpoint_cap;
            }
        }
        return -1;
    }

    const char* get_service_name(ServiceId id) const {
        for (size_t i = 0; i < count_; ++i) {
            if (entries_[i].id == id && entries_[i].active) {
                return entries_[i].name;
            }
        }
        return "";
    }

    bool is_service_available(ServiceId id) const {
        return get_service_endpoint(id) >= 0;
    }

    size_t get_registered_count() const {
        return count_;
    }

private:
    ServiceRegistry() {
        reset();
    }

    ServiceEntry entries_[MAX_REGISTRY_ENTRIES];
    size_t count_ = 0;
};

} // namespace services
} // namespace auroraos

#endif // AURORAOS_SERVICE_REGISTRY_HPP
