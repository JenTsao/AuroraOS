# Apply February onto AuroraOS

Branch: **February**  
Framework: **February**（二月）  
Scope: **OS-level cross-device AI service** (not watch-only)

Wake word: **unset by default** — `FebruaryCore::set_wake_word(...)`

## Copy

```bash
cp -a ai/february /path/to/AuroraOS/ai/
cp tests/unit/test_february_*.cpp /path/to/AuroraOS/tests/unit/
```

Keep `ai/intent_engine.hpp` until call sites migrate.  
Drop-in legacy: `ai/february/compat_intent_engine.hpp`.

## Verify

```bash
g++ -std=c++17 -Wall -Wextra -Werror -I. \
    -o /tmp/test_february tests/unit/test_february_core.cpp
/tmp/test_february

g++ -std=c++17 -Wall -Wextra -Werror -I. \
    -o /tmp/test_february_p2 tests/unit/test_february_phase2.cpp
/tmp/test_february_p2
```

## Init (Phase 2 service)

```cpp
#include "ai/february/service.hpp"
using aurora::february::FebruaryService;
using aurora::february::CapabilityHooks;

auto& svc = FebruaryService::instance();
svc.start();

CapabilityHooks caps;
// bind node-specific drivers
svc.set_capability_hooks(caps);

// loop: feed_* on FebruaryCore; svc.run_once(now_ms);
```

## Init (Phase 1 core only)

```cpp
#include "ai/february/february_core.hpp"
FebruaryCore::instance().init();
FebruaryCore::instance().set_action_hooks(hooks);
```

## Compile-time strip (small MCU)

```
-DFEBRUARY_ENABLE_PLANNER=0 -DFEBRUARY_ENABLE_SOFTBUS=0
```
