#ifndef AURORAOS_RUNTIME_APP_STATE_HPP
#define AURORAOS_RUNTIME_APP_STATE_HPP

#include <stdint.h>

namespace auroraos {
namespace runtime {

enum class AppState : uint8_t {
    Created = 0,
    Starting = 1,
    Running = 2,
    Paused = 3,
    Stopped = 4,
    Destroyed = 5,
    Error = 6
};

inline const char* app_state_to_string(AppState state) {
    switch (state) {
        case AppState::Created: return "Created";
        case AppState::Starting: return "Starting";
        case AppState::Running: return "Running";
        case AppState::Paused: return "Paused";
        case AppState::Stopped: return "Stopped";
        case AppState::Destroyed: return "Destroyed";
        case AppState::Error: return "Error";
        default: return "Unknown";
    }
}

} // namespace runtime
} // namespace auroraos

#endif // AURORAOS_RUNTIME_APP_STATE_HPP
