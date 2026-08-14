# February（二月）— Phase 2 SoftBus

System-level **cross-device** AI runtime for AuroraOS.  
Product name: **February**. Branch: **`February`**.

## SoftBus (real adapter)

February never links OpenHarmony headers directly. Platform code binds a
**transport** once; Intent frames travel as binary packets over that transport.

```
publish_intent → softbus_pack → SoftBusTransportOps::send_bytes
RX bytes → softbus_unpack → SoftBusStub inbox → FebruaryService::run_once
```

| File | Role |
|------|------|
| `softbus_codec.hpp` | LE binary Intent frame (magic `0xFB02`) |
| `softbus_transport.hpp` | `SoftBusTransportOps` + `SoftBusRxSink` |
| `softbus.hpp` | Facade: sessions, pack/send, RX → inbox |
| `softbus_oh_adapter.hpp` | OH-style `CreateSessionServer` / `SendBytes` fnptrs |
| `softbus_stub.hpp` | Local RX ring (drop-oldest) |

### Bind real SoftBus (device)

```cpp
#include "ai/february/softbus_oh_adapter.hpp"
#include "ai/february/service.hpp"

// Point at your SoftBus symbols (names vary by OH version)
OhSoftBusFns fns;
fns.create_session_server = CreateSessionServer;
fns.remove_session_server = RemoveSessionServer;
fns.open_session          = OpenSession;
fns.close_session         = CloseSession;
fns.send_bytes            = SendBytes;
OhSoftBusAdapter::instance().bind(fns);

auto& svc = FebruaryService::instance();
svc.bind_softbus_transport(OhSoftBusAdapter::instance().ops());
svc.start();

SoftBus::instance().register_peer(2, peerNetworkId);
// later:
Intent in; in.type = IntentType::Help; in.confidence_x1000 = 900;
svc.publish_remote(2, in, now_ms);
```

If your stack owns the session listener, forward RX:

```cpp
void OnBytesReceived(int sessionId, const void* data, unsigned len) {
    OhSoftBusAdapter::forward_bytes(sessionId, data, len);
}
```

Or pass `&OhSoftBusAdapter::bridge_listener()` into `CreateSessionServer`.

### Host / no SoftBus

Leave transport unbound (or bind mocks). `publish_remote(0, …)` and the
inbox still work for local tests.

### Frame layout (v1)

```
magic u16=0xFB02 | ver u8=1 | flags u8
type u16 | conf u32 | source u32 | param0 i32 | param1 i32
text_len u8 | text[text_len]
timestamp_ms u32 | peer_id u32
```

Max ~96 bytes. Zero-heap pack/unpack.

## Phase goals

| Phase | Scope |
|-------|--------|
| **1 / 1.5** | Intent/context/persona, rules, cooldown, memory, legacy bridge |
| **2.0** | Service, CapabilityHooks, planner, SoftBus stub |
| **2.1 (here)** | Codec + transport ops + OH adapter + SoftBus facade |
| **2.x** | Board Kconfig, discovery helpers, richer plans |
| **3+** | Cloud / Agent |

## Layout

```
ai/february/
  types.hpp event_bus.hpp context_manager.hpp cooldown.hpp memory.hpp
  intent_rules.hpp intent_engine.hpp persona.hpp action_executor.hpp
  wake_word.hpp log.hpp february_core.hpp compat_intent_engine.hpp
  string_util.hpp config.hpp platform_hooks.hpp planner.hpp
  softbus_stub.hpp softbus_codec.hpp softbus_transport.hpp
  softbus.hpp softbus_oh_adapter.hpp service.hpp
tests/unit/
  test_february_core.cpp
  test_february_phase2.cpp
  test_february_softbus.cpp
```

## Host tests

```bash
g++ -std=c++17 -Wall -Wextra -Werror -I. \
    -o /tmp/test_february tests/unit/test_february_core.cpp
g++ -std=c++17 -Wall -Wextra -Werror -I. \
    -o /tmp/test_february_p2 tests/unit/test_february_phase2.cpp
g++ -std=c++17 -Wall -Wextra -Werror -I. \
    -o /tmp/test_softbus tests/unit/test_february_softbus.cpp
/tmp/test_february && /tmp/test_february_p2 && /tmp/test_softbus
```

## Feature flags

| Macro | Default |
|-------|---------|
| `FEBRUARY_ENABLE_SOFTBUS` | 1 |
| `FEBRUARY_SOFTBUS_QUEUE_DEPTH` | 8 |
| `FEBRUARY_SOFTBUS_MAX_SESSIONS` | 4 |
| `FEBRUARY_SOFTBUS_PKG` | `"aurora.february"` |
| `FEBRUARY_SOFTBUS_SESSION` | `"february.intent"` |

## Changelog

### Phase 2.1 — SoftBus adapter
- Binary Intent codec (pack/unpack)
- `SoftBusTransportOps` platform binding surface
- `SoftBus` facade: peer map, ensure_session, publish_intent, on_bytes_received
- `OhSoftBusAdapter` for OH-like CreateSessionServer / SendBytes
- Service uses SoftBus facade; transport bind API

### Phase 2.0
- FebruaryService, CapabilityHooks, Planner, SoftBusStub, config/string_util
