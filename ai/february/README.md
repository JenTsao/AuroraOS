# February（二月）— Phase 1.5

System-level AI runtime for AuroraOS. Inspired by Iron Man’s JARVIS; **product name is February**.

**Git branch for all AI work: `February`.**

## Phase 1 goals

- Always-on, event-driven skeleton (no heap, C++17, MCU-friendly)
- Unified `UserContext` + fixed-slot `SessionMemory`
- Extensible **IntentRule** table (`intent_rules.hpp`)
- Unified **CooldownGate** / **LevelLatch** (`cooldown.hpp`)
- Persona + ActionHooks
- Configurable wake word (default **unset**)
- Optional `FEBRUARY_LOG` (`-DFEBRUARY_LOG_LEVEL=1|2`)
- Drop-in bridge for legacy `IntentEngine::process_sensors`

## Layout

```
ai/february/
  types.hpp  event_bus.hpp  context_manager.hpp
  cooldown.hpp  memory.hpp  intent_rules.hpp  intent_engine.hpp
  persona.hpp  action_executor.hpp  wake_word.hpp  log.hpp
  february_core.hpp  compat_intent_engine.hpp
  APPLY.md  README.md
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
| Legacy | include **either** `ai/intent_engine.hpp` **or** `ai/february/compat_intent_engine.hpp` |

## Legacy bridge (compat) — important

`compat_intent_engine.hpp`:

1. Samples **live** steps from `SensorManager` (not `ctx.last_steps`).
2. Owns **AppControlBlock** FOREGROUND / BACKGROUND transitions (same as original).
3. Feeds February for Context / Intent / Persona **without** a second `TransitionApp`
   (`set_manage_app_transitions(false)` around the feed).
4. Defines `AURORA_INTENT_ENGINE_HPP` so including both old and compat headers
   cannot define `::IntentEngine` twice (if old was included first → `#error`).

Pure February path (no compat): leave `manage_app_transitions` at default `true`
so `StartFitness` still drives hooks.

## Extending commands

Edit `intent_rules.hpp` or call `IntentEngine::instance().set_rules(table)`.

## Wake word

```cpp
FebruaryCore::instance().set_wake_word("hey february");  // or u8"二月"
```

## Debug log

```cpp
// compile with -DFEBRUARY_LOG_LEVEL=1
FebruaryCore::instance().set_log_sink(my_uart_log, nullptr);
```

## Changelog (bugfixes after Phase 1.5 review)

- **Double app switch**: compat no longer lets February emit `TransitionApp` while
  legacy `transition_to` already ran.
- **Header clash**: compat claims `AURORA_INTENT_ENGINE_HPP` / errors if old header
  already included.
- **EventBus::publish**: docs match behavior (always accept; drop oldest if full).
- **CooldownGate**: safer across `now_ms` wrap / backwards time.
- **idle_seconds**: saturated on time wrap (earlier fix).
- **Sensor sample**: compat uses `SensorManager`, not stale `ctx.last_steps` (Codex).

## Phase 2 (not here)

Service task wiring, real KWS/TTS, SoftBus/API, light planner.
