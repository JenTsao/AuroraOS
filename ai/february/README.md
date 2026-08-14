# February (二月) — Phase 2.2

System-level **cross-device** AI runtime for AuroraOS.  
Product name: **February**. Branch: **`February`**.

## Phase 2.2 highlights

| Item | Description |
|------|-------------|
| `crit.hpp` | Optional IRQ/mutex critical-section hooks for EventBus / SoftBus |
| `Kconfig` | Board fragment: SERVICE / PLANNER / SOFTBUS / PEER_TABLE / sizes |
| `planner.hpp` | Static `PlanRule` table + `set_rules()` swap |
| `peer_table.hpp` | Fixed peer last-seen / TX-RX / session_open |
| `board_bind.hpp` | 10-line board bring-up helper |
| Service | Remote yields to local intent when `FEBRUARY_REMOTE_YIELD_TO_LOCAL=1` |

## SoftBus (real adapter)

February never links OpenHarmony headers directly. Platform code binds a
**transport** once; Intent frames travel as binary packets.

```
publish_intent -> softbus_pack -> SoftBusTransportOps::send_bytes
RX bytes -> softbus_unpack -> SoftBusStub inbox -> FebruaryService::run_once
```

### Bind real SoftBus (device)

```cpp
#include "ai/february/board_bind.hpp"
#include "ai/february/softbus_oh_adapter.hpp"

OhSoftBusFns fns;
fns.create_session_server = CreateSessionServer;
fns.open_session          = OpenSession;
fns.send_bytes            = SendBytes;
OhSoftBusAdapter::instance().bind(fns);

CapabilityHooks caps; /* bind TTS / notify / ... */

BoardBindArgs a;
a.crit_enter = my_irq_enter;
a.crit_exit  = my_irq_exit;
a.caps = &caps;
a.transport = &OhSoftBusAdapter::instance().ops();
a.wake_word = "hey february";
board_bind_start(a);

SoftBus::instance().register_peer(2, peerNetworkId, now_ms);
```

### Host tests

```bash
g++ -std=c++17 -Wall -Wextra -Werror -I. -o /tmp/t1 tests/unit/test_february_core.cpp
g++ -std=c++17 -Wall -Wextra -Werror -I. -o /tmp/t2 tests/unit/test_february_phase2.cpp
g++ -std=c++17 -Wall -Wextra -Werror -I. -o /tmp/t3 tests/unit/test_february_softbus.cpp
g++ -std=c++17 -Wall -Wextra -Werror -I. -o /tmp/t4 tests/unit/test_february_phase22.cpp
/tmp/t1 && /tmp/t2 && /tmp/t3 && /tmp/t4
```

## Feature flags

| Macro | Default |
|-------|--------|
| `FEBRUARY_ENABLE_SERVICE` | 1 |
| `FEBRUARY_ENABLE_PLANNER` | 1 |
| `FEBRUARY_ENABLE_SOFTBUS` | 1 |
| `FEBRUARY_ENABLE_PEER_TABLE` | 1 |
| `FEBRUARY_REMOTE_YIELD_TO_LOCAL` | 1 |
| `FEBRUARY_PLANNER_MAX_STEPS` | 4 |
| `FEBRUARY_SOFTBUS_QUEUE_DEPTH` | 8 |
| `FEBRUARY_SOFTBUS_MAX_SESSIONS` | 4 |
| `FEBRUARY_PEER_TABLE_SIZE` | 4 |

Tiny MCU: `-DFEBRUARY_ENABLE_PLANNER=0 -DFEBRUARY_ENABLE_SOFTBUS=0 -DFEBRUARY_ENABLE_PEER_TABLE=0`

## Changelog

### Phase 2.2 — board-ready
- FebruaryCrit, Kconfig, PlanRule table, PeerTable
- Remote yield-to-local, SoftBus::close_peer, board_bind
- ActionHooks::on_set_power

### Phase 2.1 — SoftBus adapter
- Codec, transport ops, OH adapter, SoftBus facade

### Phase 2.0
- FebruaryService, CapabilityHooks, Planner, SoftBusStub
