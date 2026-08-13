#include <gtest/gtest.h>
#include "memory_pool.hpp"

// Define a test object
struct Packet {
    uint32_t id;
    uint32_t data[15];
};

class MemoryPoolTest : public ::testing::Test {
protected:
    void SetUp() override {}

    void TearDown() override {}
};

// Test basic allocation and deallocation
TEST_F(MemoryPoolTest, BasicAllocDealloc) {
    MemoryPool<Packet, 4> pool;
    EXPECT_EQ(pool.get_capacity(), 4);

    Packet* p1 = pool.allocate();
    ASSERT_NE(p1, nullptr);
    p1->id = 1;

    Packet* p2 = pool.allocate();
    ASSERT_NE(p2, nullptr);
    p2->id = 2;

    // We can deallocate and reuse
    pool.deallocate(p1);

    Packet* p3 = pool.allocate();
    ASSERT_NE(p3, nullptr);

    // Since p1 was deallocated, p3 should likely reuse its slot
    EXPECT_EQ(p1, p3);
}

// Test memory exhaustion
TEST_F(MemoryPoolTest, Exhaustion) {
    MemoryPool<Packet, 2> pool;

    Packet* p1 = pool.allocate();
    Packet* p2 = pool.allocate();

    ASSERT_NE(p1, nullptr);
    ASSERT_NE(p2, nullptr);

    // Pool is exhausted, should return nullptr
    Packet* p3 = pool.allocate();
    EXPECT_EQ(p3, nullptr);

    // After freeing one, we can allocate again
    pool.deallocate(p1);
    Packet* p4 = pool.allocate();
    EXPECT_NE(p4, nullptr);
}

// Test out of bounds deallocation protection
TEST_F(MemoryPoolTest, OutOfBoundsFree) {
    MemoryPool<Packet, 2> pool;
    Packet* p1 = pool.allocate();
    (void)p1;

    // Create a fake packet on stack
    Packet fake_packet;

    // Try to free the fake packet (should be ignored safely)
    EXPECT_NO_FATAL_FAILURE(pool.deallocate(&fake_packet));

    // Ensure the pool isn't corrupted (can still allocate the remaining 1 slot)
    Packet* p2 = pool.allocate();
    EXPECT_NE(p2, nullptr);
}

// Test double free protection
TEST_F(MemoryPoolTest, DoubleFree) {
    MemoryPool<Packet, 2> pool;
    Packet* p1 = pool.allocate();

    // Free once
    pool.deallocate(p1);

    // Free again (should be ignored safely)
    EXPECT_NO_FATAL_FAILURE(pool.deallocate(p1));

    // If it was double-freed, allocate would return p1 twice.
    Packet* p2 = pool.allocate();
    Packet* p3 = pool.allocate();

    EXPECT_NE(p2, p3);
    EXPECT_NE(p2, nullptr);
    EXPECT_NE(p3, nullptr);

    // Should be exhausted now
    Packet* p4 = pool.allocate();
    EXPECT_EQ(p4, nullptr);
}
