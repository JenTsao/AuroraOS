#ifndef AURORAOS_RUNTIME_MANAGER_HPP
#define AURORAOS_RUNTIME_MANAGER_HPP

#include "app_base.hpp"
#include <stdint.h>

namespace auroraos {
namespace runtime {

constexpr int MAX_APPS = 16;

class AuroraRuntime {
public:
    static AuroraRuntime& instance() {
        static AuroraRuntime runtime;
        return runtime;
    }

    bool register_app(AppBase* app);
    bool unregister_app(AppBase* app);
    
    void start_all();
    void stop_all();
    
    AppBase* get_app_by_name(const char* name);

private:
    AuroraRuntime() = default;

    AppBase* apps_[MAX_APPS] = {nullptr};
    int app_count_ = 0;
};

} // namespace runtime
} // namespace auroraos

#endif // AURORAOS_RUNTIME_MANAGER_HPP
