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

/** Confidence in Q8: 0 = 0.0, 255 ≈ 1.0. */
using ConfidenceQ8 = uint8_t;

enum class TimeOfDay : uint8_t {
    DeepNight = 0,
    Morning,
    Afternoon,
    Evening,
    LateNight
};

enum class DayClass : uint8_t {
    Workday = 0,
    Weekend,
    Holiday
};

struct TimeContext {
    TimeOfDay tod       = TimeOfDay::Morning;
    DayClass  day       = DayClass::Workday;
    uint8_t   hour      = 0;
    uint8_t   minute    = 0;
    uint8_t   weekday   = 0;
    uint16_t  minute_of_day = 0;
    uint32_t  unix_day  = 0;
};

enum class SensorKind : uint8_t {
    Accelerometer = 0,
    Activity,
    HeartRate,
    Time,
    Posture,
    BleRssi,
    WifiScan,
    RfInterference,
    StepCount,
    Battery,
    Count
};

struct SensorSample {
    SensorKind    kind          = SensorKind::Count;
    ConfidenceQ8  confidence_q8 = 0;
    uint32_t      timestamp_ms  = 0;
    union {
        struct { int16_t x_mg; int16_t y_mg; int16_t z_mg; } accel;
        struct { uint8_t activity; uint8_t intensity_q8; } activity;
        struct { uint16_t bpm; } heart;
        struct { uint8_t hour; uint8_t minute; uint8_t weekday; } time;
        struct { int8_t pitch_deg; int8_t roll_deg; uint8_t raised; } posture;
        struct { int16_t rssi_dbm; uint8_t connected; } ble;
        struct { uint8_t ap_count; int16_t strongest_rssi_dbm; } wifi;
        struct { uint8_t level_q8; int16_t noise_dbm_q8; } rf;
        struct { uint32_t steps; } steps;
        struct { uint8_t pct; } battery;
        uint32_t raw[3];
    } data{};
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
    TimeContext   time_ctx{};
    ConfidenceQ8  sensor_conf[static_cast<unsigned>(SensorKind::Count)]{};
    int16_t       accel_x_mg    = 0;
    int16_t       accel_y_mg    = 0;
    int16_t       accel_z_mg    = 0;
    int16_t       ble_rssi_dbm  = 0;
    int16_t       wifi_rssi_dbm = 0;
    uint8_t       wifi_ap_count = 0;
    uint8_t       rf_interference_q8 = 0;
    int8_t        posture_pitch = 0;
    int8_t        posture_roll  = 0;
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
    SensorFused,
    IntentDetected,
    ContextChanged,
    ActionExecuted,
    ProactiveTrigger,
    VoiceUtterance,
    RemoteIntent,
    SystemTick,
    TimeContextChanged
};

enum class SubPriority : uint8_t {
    Low = 0,
    Normal = 64,
    High = 128,
    Critical = 192
};

struct Event {
    EventType type = EventType::None;
    uint32_t  timestamp_ms = 0;
    uint32_t  source_id    = 0;
    union {
        Intent       intent;
        UserContext  context;
        Action       action;
        SensorSample sensor;
        TimeContext  time_ctx;
        uint32_t     raw[8];
    } payload{};

    Event() : type(EventType::None), timestamp_ms(0), source_id(0) {
        payload = {};
    }
};

constexpr unsigned kMaxEventSubscribers = 12;
constexpr unsigned kEventQueueDepth     = 16;
static_assert((kEventQueueDepth & (kEventQueueDepth - 1)) == 0, "queue depth pow2");

}  // namespace february
}  // namespace aurora

#endif  // AURORA_FEBRUARY_TYPES_HPP
