# February（二月）— Phase 1

System-level AI runtime for AuroraOS. Inspired by Iron Man’s JARVIS; **product name is February**.

## Phase 1 goals

- Always-on, event-driven service skeleton
- Unified `UserContext` + fixed-slot `SessionMemory`
- Extensible **IntentRule** table (keyword → Intent)
- Unified **CooldownGate** / **LevelLatch** for proactive intents
- Persona replies + ActionHooks
- Configurable **wake word** (default unset)
- Optional `FEBRUARY_LOG` / `FEBRUARY_LOGV`
- Zero heap, C++17, MCU-friendly
- Drop-in bridge for legacy `IntentEngine::process_sensors`

## Layout

```
ai/february/
  types.hpp event_bus.hpp context_manager.hpp
  cooldown.hpp memory.hpp intent_rules.hpp intent_engine.hpp
  persona.hpp action_executor.hpp wake_word.hpp log.hpp
  february_core.hpp compat_intent_engine.hpp
  APPLY.md README.md
tests/unit/test_february_core.cpp
```

## Host test

```bash
g++ -std=c++17 -Wall -Wextra -Werror -I. \
    -o /tmp/test_february tests/unit/test_february_core.cpp
/tmp/test_february
```

Expected: `ALL CHECKS PASSED`.

## Integration checklist

| Step | API |
|------|-----|
| Init | `FebruaryCore::instance().init()` |
| Hooks | `set_action_hooks(...)` |
| Wake word | `set_wake_word("...")` — empty = no gate |
| Sensors | `feed_steps` / `feed_battery` / `feed_heart_rate` |
| Text | `feed_text` / `inject_intent` |
| Loop | `tick(now_ms)` then `process_events()` |
| Legacy | `compat_intent_engine.hpp` (samples SensorManager) |

## Extending commands

Edit `intent_rules.hpp` or `IntentEngine::set_rules(my_table)`.

## Branch

All AI work on **`February`**.
