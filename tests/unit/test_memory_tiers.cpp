/**
 * Three-tier SessionMemory host test (GoogleTest).
 *
 * Originally a standalone program with its own main() and hand-written
 * asserts. It is built as its own test executable because the February
 * singletons (SessionMemory/WorkingMemory/EpisodicMemory/DeviceGraph/
 * PeerTable) are process-global.
 */
#include <gtest/gtest.h>

#include "ai/february/memory.hpp"
#include "ai/february/working_memory.hpp"
#include "ai/february/episodic_memory.hpp"
#include "ai/february/world_model.hpp"
#include "ai/february/peer_table.hpp"

using namespace aurora::february;

class FebruaryMemoryTiersTest : public ::testing::Test {
protected:
    void SetUp() override {
        SessionMemory::instance().clear();
        WorkingMemory::instance().clear();
#if FEBRUARY_ENABLE_EPISODIC_MEMORY
        EpisodicMemory::instance().clear();
#endif
#if FEBRUARY_ENABLE_WORLD_MODEL
        DeviceGraph::instance().clear();
#endif
        PeerTable::instance().clear();
    }
};

TEST_F(FebruaryMemoryTiersTest, SessionAndWorkingMemoryRecording) {
    uint32_t t0 = 1000;
    Intent in;
    in.type = IntentType::Greeting;
    in.confidence_x1000 = 900;
    SessionMemory::instance().note_intent(in, t0);
    EXPECT_EQ(SessionMemory::instance().last_intent().type, IntentType::Greeting);
    EXPECT_GE(WorkingMemory::instance().count(), 1u);

    WorkingMemory::instance().push(WmKind::Sensor, 0, 1, 0, 255, t0);
    WorkingMemory::instance().push(WmKind::Speak, 0, 0, 0, 255, t0 + 10);
    EXPECT_GE(WorkingMemory::instance().count(), 3u);

    WorkingMemory::instance().decay(t0 + FEBRUARY_WORKING_MEMORY_WINDOW_MS + 1);
    EXPECT_EQ(WorkingMemory::instance().count(), 0u);
}

TEST_F(FebruaryMemoryTiersTest, EpisodicHabitMemory) {
#if FEBRUARY_ENABLE_EPISODIC_MEMORY
    EpisodicMemory& epi = EpisodicMemory::instance();
    epi.clear();
    epi.set_kv(EpisodicMemory::make_ram_kv());
    epi.seed_defaults();
    EXPECT_GE(epi.count(), 1u);

    HabitRule hit{};
    ASSERT_TRUE(epi.match(23, 0, HabitTrigger::WristRaise, &hit));
    EXPECT_EQ(hit.action, HabitAction::CheckTimeOnly);
    EXPECT_FALSE(epi.match(10, 0, HabitTrigger::WristRaise, &hit));

    ASSERT_TRUE(epi.flush());
    const char* keys[] = { "night_wrist" };
    epi.clear();
    EXPECT_EQ(epi.count(), 0u);
    ASSERT_TRUE(epi.load_from_kv(keys, 1));
    EXPECT_GE(epi.count(), 1u);
#else
    GTEST_SKIP() << "episodic memory disabled";
#endif
}

TEST_F(FebruaryMemoryTiersTest, WorldModelDeviceRouting) {
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
                     static_cast<uint16_t>(DeviceCap::Speaker),
               5000);
    g.set_room(2, 3, 5000);
    g.set_battery(2, 40, 5000);
    g.set_trust(2, 100, 5000);

    uint32_t target = g.route(DeviceCap::Light, 2);
    EXPECT_EQ(target, 1u);
    EXPECT_EQ(g.route(DeviceCap::Speaker, 0), 2u);

    PeerTable::instance().clear();
    PeerTable::instance().touch(9, "net9", 6000);
    PeerTable::instance().note_tx(9, 6000, true);
    EXPECT_NE(DeviceGraph::instance().find(9), nullptr);
#else
    GTEST_SKIP() << "world model disabled";
#endif
}
