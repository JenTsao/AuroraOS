#include "app_base.hpp"
#include "../syscall/syscall.hpp"

namespace auroraos {
namespace runtime {

bool AppBase::start() {
    if (state_ != AppState::Created && state_ != AppState::Stopped) {
        return false;
    }
    
    state_ = AppState::Starting;
    sys_print("[Runtime] Starting App: ");
    sys_print(manifest_.name);
    sys_print("\r\n");

    if (on_start()) {
        state_ = AppState::Running;
        return true;
    } else {
        fault("Failed to start");
        return false;
    }
}

void AppBase::pause() {
    if (state_ == AppState::Running) {
        state_ = AppState::Paused;
        on_pause();
    }
}

void AppBase::resume() {
    if (state_ == AppState::Paused) {
        state_ = AppState::Running;
        on_resume();
    }
}

void AppBase::stop() {
    if (state_ == AppState::Running || state_ == AppState::Paused) {
        on_stop();
        state_ = AppState::Stopped;
        sys_print("[Runtime] Stopped App: ");
        sys_print(manifest_.name);
        sys_print("\r\n");
    }
}

void AppBase::destroy() {
    if (state_ != AppState::Destroyed) {
        if (state_ == AppState::Running || state_ == AppState::Paused) {
            stop();
        }
        on_destroy();
        state_ = AppState::Destroyed;
    }
}

void AppBase::fault(const char* reason) {
    state_ = AppState::Error;
    on_error(reason);
    sys_print("[Runtime] App Fault (");
    sys_print(manifest_.name);
    sys_print("): ");
    sys_print(reason);
    sys_print("\r\n");
}

} // namespace runtime
} // namespace auroraos

