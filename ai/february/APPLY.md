# Apply February onto AuroraOS

Branch: **February**  
Framework: **February** (二月)  
Scope: **OS-level cross-device AI service** (Phase 2.2 board-ready)

Wake word: **unset by default** — `FebruaryCore::set_wake_word(...)`

## Copy

```bash
cp -a ai/february /path/to/AuroraOS/ai/
cp tests/unit/test_february_*.cpp /path/to/AuroraOS/tests/unit/
```

Optional Kconfig:

```
source "ai/february/Kconfig"
```

Keep `ai/intent_engine.hpp` until call sites migrate.  
Drop-in legacy: `ai/february/compat_intent_engine.hpp`.

## Verify

```bash
g++ -std=c++17 -Wall -Wextra -Werror -I. \
    -o /tmp/test_february tests/unit/test_february_core.cpp
g++ -std=c++17 -Wall -Wextra -Werror -I. \
    -o /tmp/test_february_p2 tests/unit/test_february_phase2.cpp
g++ -std=c++17 -Wall -Wextra -Werror -I. \
    -o /tmp/test_softbus tests/unit/test_february_softbus.cpp
g++ -std=c++17 -Wall -Wextra -Werror -I. \
    -o /tmp/test_p22 tests/unit/test_february_phase22.cpp
/tmp/test_february && /tmp/test_february_p2 && /tmp/test_softbus && /tmp/test_p22
```

## Board init (Phase 2.2 — recommended)

```cpp
#include "ai/february/board_bind.hpp"
#include "ai/february/softbus_oh_adapter.hpp"

using namespace aurora::february;

FebruaryCrit::set(irq_enter, irq_exit);

CapabilityHooks caps;
caps.on_speak = my_tts;
caps.on_notify = my_notify;
caps.on_set_dnd = my_dnd;
caps.on_set_power = my_power;
caps.on_transition_app = my_app;

OhSoftBusFns fns; /* fill from real SoftBus symbols if present */
OhSoftBusAdapter::instance().bind(fns);

BoardBindArgs a;
a.crit_enter = irq_enter;
a.crit_exit  = irq_exit;
a.caps = &caps;
a.transport = &OhSoftBusAdapter::instance().ops();
a.wake_word = "hey february";
board_bind_start(a);

SoftBus::instance().register_peer(2, peerNetworkId, now_ms);

// Task loop:
//   FebruaryCore::instance().feed_steps(steps, now_ms);
//   FebruaryService::instance().run_once(now_ms);
```

## Init (Phase 1 core only)

```cpp
#include "ai/february/february_core.hpp"
FebruaryCore::instance().init();
FebruaryCore::instance().set_action_hooks(hooks);
```

## Compile-time strip (small MCU)

```
-DFEBRUARY_ENABLE_PLANNER=0 -DFEBRUARY_ENABLE_SOFTBUS=0 -DFEBRUARY_ENABLE_PEER_TABLE=0
```

## Custom plan table

```cpp
static const PlanRule kMyPlans[] = {
    {IntentType::BatteryLow, false, 0, 1,
     {{ActionType::NotifyUser, 0, 0, "Low battery"}}},
    {IntentType::None, false, 0, 0, {}},
};
Planner::instance().set_rules(kMyPlans);
```
