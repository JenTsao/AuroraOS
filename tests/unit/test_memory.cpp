// =============================================================================
// test_memory.cpp — Unit tests for KernelHeap (kernel/memory.hpp)
//
// Strategy: Allocate a plain byte array on the host stack/heap to serve as
// the "heap region", then call KernelHeap::init() with its bounds.  Because
// KernelHeap is a singleton we reset state between test cases via a helper
// fixture.
//
// C++ Core Guidelines applied:
//  ES.20: all test variables are initialised at declaration
//  Con.1: fixture members are const where not mutated
//  F.20: tests use EXPECT_/ASSERT_ return-value checks, no output params
// =============================================================================

#include <gtest/gtest.h>

// Pull in the header-only KernelHeap.  The stubs/ directory is injected
// before kernel/ so arch_api.hpp, mutex.hpp, syscall.hpp resolve to stubs.
#include "memory.hpp"

#include <cstdint>
#include <cstring>
#include <array>

// ---------------------------------------------------------------------------
// HeapFixture — provides a fresh 4 KB heap region for every test case.
// ---------------------------------------------------------------------------
class HeapTest : public ::testing::Test {
protected:
    static constexpr std::size_t kHeapSize = 4096;

    // Aligned storage so the heap allocator's 4-byte alignment assumption holds.
    alignas(4) std::array<uint8_t, kHeapSize> heap_storage_{};

    void SetUp() override {
        // Re-initialise the singleton heap before every test to ensure
        // full isolation (Con.1: treat each test as a fresh, immutable world).
        KernelHeap::instance().init(
            heap_storage_.data(),
            heap_storage_.data() + kHeapSize);
    }
};

// ---------------------------------------------------------------------------
// 1. Basic allocation returns non-null and reduces free memory
// ---------------------------------------------------------------------------
TEST_F(HeapTest, AllocateBasic) {
    const std::size_t free_before = KernelHeap::instance().get_free_memory();

    void* const ptr = KernelHeap::instance().allocate(64);

    ASSERT_NE(ptr, nullptr);
    EXPECT_LT(KernelHeap::instance().get_free_memory(), free_before);
}

// ---------------------------------------------------------------------------
// 2. Returned pointer is 4-byte aligned (ES.46: no narrowing; alignment check)
// ---------------------------------------------------------------------------
TEST_F(HeapTest, AllocateAlignment) {
    void* const ptr = KernelHeap::instance().allocate(1);

    ASSERT_NE(ptr, nullptr);
    const auto addr = reinterpret_cast<uintptr_t>(ptr);
    EXPECT_EQ(addr % 4u, 0u) << "Allocated pointer must be 4-byte aligned";
}

// ---------------------------------------------------------------------------
// 3. Free memory is restored after deallocate (basic coalesce path)
// ---------------------------------------------------------------------------
TEST_F(HeapTest, AllocateThenFree) {
    const std::size_t free_before = KernelHeap::instance().get_free_memory();

    void* const ptr = KernelHeap::instance().allocate(128);
    ASSERT_NE(ptr, nullptr);

    KernelHeap::instance().deallocate(ptr);

    // Because of lazy coalescing, we must explicitly defragment
    // to restore the block headers and fully recover the free space.
    KernelHeap::instance().defragment();

    // After freeing the only allocation the heap should report the same
    // free bytes as the initial state.
    EXPECT_EQ(KernelHeap::instance().get_free_memory(), free_before);
}

// ---------------------------------------------------------------------------
// 4. Adjacent free blocks are coalesced during next OOM allocate()
// ---------------------------------------------------------------------------
TEST_F(HeapTest, AllocateCoalesce) {
    void* const a = KernelHeap::instance().allocate(1000);
    void* const b = KernelHeap::instance().allocate(1000);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);

    const std::size_t free_after_two = KernelHeap::instance().get_free_memory();

    // Free both — they won't coalesce instantly, but total free memory increases
    KernelHeap::instance().deallocate(a);
    KernelHeap::instance().deallocate(b);

    EXPECT_GT(KernelHeap::instance().get_free_memory(), free_after_two);
    
    // This large allocation forces lazy coalescing in allocate()
    void* const c = KernelHeap::instance().allocate(2000);
    EXPECT_NE(c, nullptr);
}

// ---------------------------------------------------------------------------
// 5. Double-free must not crash or corrupt the heap
// ---------------------------------------------------------------------------
TEST_F(HeapTest, DoubleFreeGuard) {
    void* const ptr = KernelHeap::instance().allocate(32);
    ASSERT_NE(ptr, nullptr);

    KernelHeap::instance().deallocate(ptr);

    // Second free of the same pointer should be silently ignored.
    EXPECT_NO_FATAL_FAILURE(KernelHeap::instance().deallocate(ptr));
}

// ---------------------------------------------------------------------------
// 6. Freeing a wild/invalid pointer must not crash
// ---------------------------------------------------------------------------
TEST_F(HeapTest, InvalidPtrGuard) {
    uint8_t stack_buf[16]{};
    void* const wild_ptr = stack_buf;  // Points outside the heap region

    EXPECT_NO_FATAL_FAILURE(KernelHeap::instance().deallocate(wild_ptr));
}

// ---------------------------------------------------------------------------
// 7. Allocating more than the heap can hold returns nullptr (OOM)
// ---------------------------------------------------------------------------
TEST_F(HeapTest, OomReturnsNull) {
    // Exhaust the heap with a single oversized request.
    void* const ptr = KernelHeap::instance().allocate(kHeapSize * 2);

    EXPECT_EQ(ptr, nullptr) << "Allocating beyond heap capacity must return nullptr";
}

// ---------------------------------------------------------------------------
// 8. get_requested_size() returns the original request size
// ---------------------------------------------------------------------------
TEST_F(HeapTest, GetRequestedSize) {
    constexpr std::size_t kRequest = 37u;  // Odd size to exercise alignment

    void* const ptr = KernelHeap::instance().allocate(kRequest);
    ASSERT_NE(ptr, nullptr);

    EXPECT_EQ(KernelHeap::instance().get_requested_size(ptr), kRequest);

    KernelHeap::instance().deallocate(ptr);
}

// ---------------------------------------------------------------------------
// 9. get_requested_size() on nullptr returns 0
// ---------------------------------------------------------------------------
TEST_F(HeapTest, GetRequestedSizeNullptr) {
    EXPECT_EQ(KernelHeap::instance().get_requested_size(nullptr), 0u);
}

// ---------------------------------------------------------------------------
// 10. Total memory reports the full heap size
// ---------------------------------------------------------------------------
TEST_F(HeapTest, TotalMemoryMatchesHeapSize) {
    EXPECT_EQ(KernelHeap::instance().get_total_memory(), kHeapSize);
}
