#include "aurora_runtime.hpp"
#include <cstring>

namespace auroraos {
namespace runtime {

bool AuroraRuntime::register_app(AppBase* app) {
    if (!app || app_count_ >= MAX_APPS) {
        return false;
    }
    
    // Check if already registered
    for (int i = 0; i < app_count_; i++) {
        if (apps_[i] == app) {
            return true;
        }
    }
    
    apps_[app_count_++] = app;
    return true;
}

bool AuroraRuntime::unregister_app(AppBase* app) {
    if (!app) return false;
    
    for (int i = 0; i < app_count_; i++) {
        if (apps_[i] == app) {
            // Shift remaining elements
            for (int j = i; j < app_count_ - 1; j++) {
                apps_[j] = apps_[j + 1];
            }
            apps_[app_count_ - 1] = nullptr;
            app_count_--;
            return true;
        }
    }
    return false;
}

void AuroraRuntime::start_all() {
    for (int i = 0; i < app_count_; i++) {
        if (apps_[i]->get_state() == AppState::Created || apps_[i]->get_state() == AppState::Stopped) {
            apps_[i]->start();
        }
    }
}

void AuroraRuntime::stop_all() {
    for (int i = 0; i < app_count_; i++) {
        if (apps_[i]->get_state() == AppState::Running || apps_[i]->get_state() == AppState::Paused) {
            apps_[i]->stop();
        }
    }
}

AppBase* AuroraRuntime::get_app_by_name(const char* name) {
    if (!name) return nullptr;
    
    for (int i = 0; i < app_count_; i++) {
        if (std::strcmp(apps_[i]->get_name(), name) == 0) {
            return apps_[i];
        }
    }
    return nullptr;
}

} // namespace runtime
} // namespace auroraos
