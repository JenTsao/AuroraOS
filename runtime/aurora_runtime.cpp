#include "aurora_runtime.hpp"
#include <string.h>

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
    if (!app)
        return false;

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
        if (apps_[i] && (apps_[i]->get_state() == AppState::Created || apps_[i]->get_state() == AppState::Stopped)) {
            apps_[i]->start();
        }
    }
}

void AuroraRuntime::stop_all() {
    for (int i = 0; i < app_count_; i++) {
        if (apps_[i] && (apps_[i]->get_state() == AppState::Running || apps_[i]->get_state() == AppState::Paused)) {
            apps_[i]->stop();
        }
    }
}

AppBase* AuroraRuntime::get_app_by_name(const char* name) {
    if (!name)
        return nullptr;

    for (int i = 0; i < app_count_; i++) {
        if (apps_[i] && strcmp(apps_[i]->get_name(), name) == 0) {
            return apps_[i];
        }
    }
    return nullptr;
}

AppBase* AuroraRuntime::get_app_by_index(int index) {
    if (index < 0 || index >= app_count_) {
        return nullptr;
    }
    return apps_[index];
}

void AuroraRuntime::audit_apps() {
    for (int i = 0; i < app_count_; i++) {
        AppBase* app = apps_[i];
        if (!app)
            continue;

        AppSandbox& sb = app->get_sandbox();
        if (sb.get_status() == SandboxStatus::Violation) {
            if (app->get_state() == AppState::Running) {
                app->stop();
            }
        }
    }
}

void AuroraRuntime::reset() {
    for (int i = 0; i < MAX_APPS; i++) {
        apps_[i] = nullptr;
    }
    app_count_ = 0;
}

} // namespace runtime
} // namespace auroraos
