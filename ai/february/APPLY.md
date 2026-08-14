# Apply February Phase 1 onto AuroraOS

Branch: **February**  
Wake word: unset by default — `set_wake_word(...)`

```bash
cp -a ai/february /path/to/AuroraOS/ai/
cp tests/unit/test_february_core.cpp /path/to/AuroraOS/tests/unit/
g++ -std=c++17 -Wall -Wextra -Werror -I. -o /tmp/test_february tests/unit/test_february_core.cpp
/tmp/test_february
```

```cpp
#include "ai/february/february_core.hpp"
auto& f = aurora::february::FebruaryCore::instance();
f.init();
f.set_action_hooks(hooks);
// f.set_wake_word("hey february");
```
