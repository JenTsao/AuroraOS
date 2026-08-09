#include <gtest/gtest.h>
#include "../../kernel/page_allocator.hpp"

using namespace auroraos::kernel;

TEST(PageAllocatorTest, BasicAllocation) {
    // 64KB buffer for testing (16 pages of 4KB)
    alignas(4096) static uint8_t memory_pool[65536];
    
    PageAllocator::instance().init(memory_pool, sizeof(memory_pool));
    
    EXPECT_EQ(PageAllocator::instance().get_total_pages(), 16);
    EXPECT_EQ(PageAllocator::instance().get_free_pages(), 16);
    
    void* p1 = PageAllocator::instance().alloc_page();
    EXPECT_NE(p1, nullptr);
    EXPECT_EQ(PageAllocator::instance().get_free_pages(), 15);
    
    void* p2 = PageAllocator::instance().alloc_page();
    EXPECT_NE(p2, nullptr);
    EXPECT_NE(p1, p2);
    EXPECT_EQ(PageAllocator::instance().get_free_pages(), 14);
    
    PageAllocator::instance().free_page(p1);
    EXPECT_EQ(PageAllocator::instance().get_free_pages(), 15);
    
    PageAllocator::instance().free_page(p2);
    EXPECT_EQ(PageAllocator::instance().get_free_pages(), 16);
}
