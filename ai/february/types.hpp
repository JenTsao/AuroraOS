/**
 * @file types.hpp
 * @brief February core types — Intent, Context, Action, Event
 *
 * Designed for resource-constrained RTOS (Cortex-M / RV32) while remaining
 * extensible to higher-capability platforms and multi-device SoftBus.
 */
#ifndef AURORA_FEBRUARY_TYPES_HPP
#define AURORA_FEBRUARY_TYPES_HPP

#include <cstdint>

namespace aurora {
namespace february {

enum class ActivityState : uint8_t {
    Unknown = 0,
    Idle,
    Walking,
    Running,
    Sleeping,
    Working,
    Exercising,
    Driving,
    Count
};

enum class PowerMode : uint8_t {
    Active = 0,
    Dim,
    Idle,
    Sleep,
    Critical
};

enum class IntentType : uint16_t {
    None = 0,
    QueryStatus,
    SetDoNotDisturb,
    SetPowerMode,
    OpenApp,
    CloseApp,
    PromoteApp,
    DemoteApp,
    StartFitness,
    StopFitness,
    QueryHealth,
    RemindRest,
    RemindHydration,
    BatteryLow,
    Greeting,
    Help,
    UnknownCommand,
    Custom = 0x8000
};

struct Intent {
    IntentType type = IntentType::None;
    uint32_t   confidence_x1000 = 0;
    uint32_t   source_id        = 0;
    int32_t    param0           = 0;
    int32_t    param1           = 0;
    char       text[64]         = {};

    void clear() {
        type = IntentType::None;
        confidence_x1000 = 0;
        source_id = 0;
        param0 = param1 = 0;
        text[0] = '\0';
    }

    bool valid() const { return type != IntentType::None; }
};

struct UserContext {
    ActivityState activity      = ActivityState::Unknown;
    PowerMode     power         = PowerMode::Active;
    uint32_t      steps         = 0;
    uint32_t      steps_delta   = 0;
    uint16_t      heart_rate    = 0;
    uint8_t       battery_pct   = 100;
    bool          wrist_raised  = false;
    bool          ble_connected = false;
    bool          dnd           = false;
    uint32_t      idle_seconds  = 0;
    uint32_t      timestamp_ms  = 0;
    uint32_t      session_id    = 0;
};

enum class ActionType : uint16_t {
    None = 0,
    TransitionApp,
    SetDnd,
    SetPower,
    NotifyUser,
    Speak,
    Log,
    PublishEvent,
    Custom = 0x8000
};

struct Action {
    ActionType type = ActionType::None;
    int32_t    arg0 = 0;
    int32_t    arg1 = 0;
    char       message[96] = {};

    void clear() {
        type = ActionType::None;
        arg0 = arg1 = 0;
        message[0] = '\0';
    }
};

enum class EventType : uint16_t {
    None = 0,
    SensorUpdate,
    IntentDetected,
    ContextChanged,
    ActionExecuted,
    ProactiveTrigger,
    VoiceUtterance,
    RemoteIntent,
    SystemTick
};

struct Event {
    EventType type = EventType::None;
    uint32_t  timestamp_ms = 0;
    uint32_t  source_id    = 0;
    union {
        Intent       intent;
        UserContext  context;
        Action       action;
        uint32_t     raw[8];
    } payload{};

    Event() : type(EventType::None), timestamp_ms(0), source_id(0) {
        payload = {};
    }
};

constexpr unsigned kMaxEventSubscribers = 8;
constexpr unsigned kEventQueueDepth     = 16;

}  // namespace february
}  // namespace aurora

#endif  // AURORA_FEBRUARY_TYPES_HPP
