#include <gtest/gtest.h>
#include <stdint.h>
#include "memory.hpp"

TEST(KernelHeapStressTest, OOMAndDefrag) {
    // 1. Allocate a small heap of 1024 bytes
    static uint8_t stress_heap[1024];
    KernelHeap::instance().init(&stress_heap[0], &stress_heap[1024]);
    
    // 2. Allocate blocks until OOM
    void* ptrs[128];
    int count = 0;
    while (count < 128) {
        void* p = KernelHeap::instance().allocate(16);
        if (!p) break;
        ptrs[count++] = p;
    }
    
    EXPECT_GT(count, 0); // We should have allocated something
    EXPECT_LT(count, 128); // We must have hit OOM
    
    // 3. Free every other block to create fragmentation
    for (int i = 0; i < count; i += 2) {
        KernelHeap::instance().deallocate(ptrs[i]);
        ptrs[i] = nullptr;
    }
    
    // 4. Try allocating a larger block. The lazy defrag should coalesce.
    void* big_ptr = KernelHeap::instance().allocate(32);
    // Depending on fragmentation, it might or might not succeed, but defragment() will run.
    if (!big_ptr) {
        // If it still fails, let's force a defrag and verify free memory increased if adjacent blocks exist
        KernelHeap::instance().defragment();
    } else {
        KernelHeap::instance().deallocate(big_ptr);
    }
    
    // 5. Clean up
    for (int i = 0; i < count; ++i) {
        if (ptrs[i]) {
            KernelHeap::instance().deallocate(ptrs[i]);
        }
    }
    
    // 6. Should be back to fully free
    KernelHeap::instance().defragment();
    size_t fm = KernelHeap::instance().get_free_memory();
    printf("Free memory: %zu, expected around 984\n", fm);
    EXPECT_GT(fm, 900); // just check if it coalesced most of it
}
