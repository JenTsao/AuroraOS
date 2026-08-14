# How to apply February (二月) Phase-1 onto AuroraOS

Framework name: **February**  
Wake word: **not fixed** — call `FebruaryCore::set_wake_word(...)` when decided.

## Copy into tree

```bash
cp -a ai/february /path/to/AuroraOS/ai/
```

Keep original `ai/intent_engine.hpp` until call sites migrate.
Use `ai/february/compat_intent_engine.hpp` as a bridge if needed.

## Host test

```bash
g++ -std=c++17 -Wall -Wextra -I. -o /tmp/test_february tests/unit/test_february_core.cpp
/tmp/test_february
```

## Init sketch

```cpp
#include "ai/february/february_core.hpp"
using aurora::february::FebruaryCore;

auto& f = FebruaryCore::instance();
f.init();
// f.set_wake_word("hey february");  // set when product decides
f.set_action_hooks(hooks);
```

Future development stays on branch **February**.
