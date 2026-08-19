/**
 * Three-tier SessionMemory host test
 * g++ -std=c++17 -Wall -Wextra -Werror -I. -o /tmp/tmem tests/unit/test_memory_tiers.cpp && /tmp/tmem
 */
#include "ai/february/memory.hpp"
#include "ai/february/working_memory.hpp"
#include "ai/february/episodic_memory.hpp"
#include "ai/february/world_model.hpp"
#include "ai/february/peer_table.hpp"
#include <cstdio>
#include <cassert>

using namespace aurora::february;

int main() {
    std::printf("=== Memory tiers test ===\n");

    SessionMemory::instance().clear();
    WorkingMemory::instance().clear();

    uint32_t t0 = 1000;
    Intent in;
    in.type = IntentType::Greeting;
    in.confidence_x1000 = 900;
    SessionMemory::instance().note_intent(in, t0);
    assert(SessionMemory::instance().last_intent().type == IntentType::Greeting);
    assert(WorkingMemory::instance().count() >= 1);

    WorkingMemory::instance().push(WmKind::Sensor, 0, 1, 0, 255, t0);
    WorkingMemory::instance().push(WmKind::Speak, 0, 0, 0, 255, t0 + 10);
    assert(WorkingMemory::instance().count() >= 3);

    WorkingMemory::instance().decay(t0 + FEBRUARY_WORKING_MEMORY_WINDOW_MS + 1);
    assert(WorkingMemory::instance().count() == 0);
    std::printf("working decay OK\n");

#if FEBRUARY_ENABLE_EPISODIC_MEMORY
    EpisodicMemory& epi = EpisodicMemory::instance();
    epi.clear();
    epi.set_kv(EpisodicMemory::make_ram_kv());
    epi.seed_defaults();
    assert(epi.count() >= 1);

    HabitRule hit{};
    assert(epi.match(23, 0, HabitTrigger::WristRaise, &hit));
    assert(hit.action == HabitAction::CheckTimeOnly);
    assert(!epi.match(10, 0, HabitTrigger::WristRaise, &hit));

    assert(epi.flush());
    const char* keys[] = { "night_wrist" };
    epi.clear();
    assert(epi.count() == 0);
    assert(epi.load_from_kv(keys, 1));
    assert(epi.count() >= 1);
    std::printf("episodic habit OK\n");
#endif

#if FEBRUARY_ENABLE_WORLD_MODEL
    DeviceGraph& g = DeviceGraph::instance();
    g.clear();
    g.touch(1, "room-a-light", 5000);
    g.set_caps(1, static_cast<uint16_t>(DeviceCap::Light), 5000);
    g.set_room(1, 2, 5000);
    g.set_battery(1, 80, 5000);
    g.set_trust(1, 200, 5000);

    g.touch(2, "room-b-light", 5000);
    g.set_caps(2, static_cast<uint16_t>(DeviceCap::Light) |
                  static_cast<uint16_t>(DeviceCap::Speaker), 5000);
    g.set_room(2, 3, 5000);
    g.set_battery(2, 40, 5000);
    g.set_trust(2, 100, 5000);

    uint32_t target = g.route(DeviceCap::Light, 2);
    assert(target == 1);
    assert(g.route(DeviceCap::Speaker, 0) == 2);

    PeerTable::instance().clear();
    PeerTable::instance().touch(9, "net9", 6000);
    PeerTable::instance().note_tx(9, 6000, true);
    assert(DeviceGraph::instance().find(9) != nullptr);
    std::printf("world model route OK\n");
#endif

    SessionMemory::instance().clear();
    std::printf("ALL memory tier tests passed\n");
    return 0;
}
