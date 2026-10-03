// =============================================================================
// tests/stress/test_heap_stress.cpp — KernelHeap (TLSF) 资源耗尽与长时间压力测试
//
// 与 unit/test_memory.cpp 的逐 API 单元测试不同，本文件聚焦 AGENTS.md #33 要求
// 的 "resource exhaustion / buffer limits / double free" 类别，在**大堆 + 高水位
// + 长时间 churn** 下验证 TLSF 分配器的全局不变量：
//
//   I1. 确定性 OOM：堆填满后 allocate 必返回 nullptr，且过程不破坏堆完整性。
//   I2. 精确恢复：全部释放后 get_free_memory() 精确回到初始值（非模糊阈值）。
//   I3. 完全合并：交错释放制造碎片后再全释放，应能分配近整堆的单块——
//       证明 TLSF 的 O(1) 即时物理合并生效（defragment() 本身是 no-op，
//       见 kernel/mm/memory.hpp:322，故本测试不依赖它）。
//   I4. 长时间 churn 稳定性：多轮可变大小分配/交错释放，free_memory 零漂移、
//       verify_integrity() 恒为 0（无泄漏、无元数据损坏）。
//   I5. 双重释放防御：受控 double-free 被 is_free guard 拦截，free_memory
//       不溢出 total，堆完整性保持。
//
// 单例隔离：KernelHeap 是全局单例。ctest 为每个 TEST 启动独立进程，且 fixture
// 在 SetUp() 中对专属 64KB 缓冲区重新 init()，因此测试间无状态污染。
// =============================================================================

#include <gtest/gtest.h>

#include "memory.hpp"

#include <cstdint>
#include <cstddef>
#include <vector>

// ---------------------------------------------------------------------------
// fixture：每个测试独享一段 64KB 堆区域，SetUp 时重新初始化 TLSF 状态。
// ---------------------------------------------------------------------------
class StressKernelHeap : public ::testing::Test {
protected:
    static constexpr size_t kHeapSize = 64u * 1024u;

    // 8 字节对齐，满足 BlockHeader 的对齐假设。
    alignas(8) uint8_t heap_[kHeapSize] = {0};

    void SetUp() override {
        KernelHeap::instance().init(heap_, heap_ + kHeapSize);
    }

    static KernelHeap& heap() {
        return KernelHeap::instance();
    }

    // 初始空闲量（init 后、任何分配前）：total 减去首个块头。
    size_t free_at_init() const {
        return heap().get_free_memory();
    }
};

// ---------------------------------------------------------------------------
// I1. 确定性 OOM：填满堆后必须返回 nullptr，且填充全程堆完整。
//     替代原空壳测试里恒真的 EXPECT_LT(count, 128)。
// ---------------------------------------------------------------------------
TEST_F(StressKernelHeap, OomFillReturnsNullAndStaysConsistent) {
    const size_t total = heap().get_total_memory();
    ASSERT_GT(total, 0u);

    // 用固定 16 字节请求填满堆，记录成功分配数。
    std::vector<void*> blocks;
    for (;;) {
        void* p = heap().allocate(16);
        if (!p)
            break;
        blocks.push_back(p);
        // 高水位下每一步都必须保持堆元数据完整。
        ASSERT_EQ(heap().verify_integrity(), 0u) << "填充第 " << blocks.size() << " 块时堆损坏";
    }

    // 必须真的分配了大量块（64KB / ~80B ≈ 800+），而非寥寥数个。
    EXPECT_GT(blocks.size(), 100u);

    // 真 OOM：剩余空间放不下任何有效块（最小块 = 对齐后 payload + 块头）。
    const size_t min_block = KernelHeap::BLOCK_HEADER_SIZE + 8u;
    EXPECT_LT(heap().get_free_memory(), min_block);

    // 各种大小的后续分配都必须失败——证明是真实耗尽，不是偶发失败。
    EXPECT_EQ(heap().allocate(1), nullptr);
    EXPECT_EQ(heap().allocate(8), nullptr);
    EXPECT_EQ(heap().allocate(16), nullptr);
    EXPECT_EQ(heap().verify_integrity(), 0u);

    // 清理。
    for (void* p : blocks)
        heap().deallocate(p);
    EXPECT_EQ(heap().verify_integrity(), 0u);
}

// ---------------------------------------------------------------------------
// I2. 精确恢复：全部释放后 free_memory 必须**精确**等于初始值。
//     替代原测试模糊的 EXPECT_GT(fm, 900)（容忍 12% 丢失）。
// ---------------------------------------------------------------------------
TEST_F(StressKernelHeap, FullReleaseRestoresExactFreeMemory) {
    const size_t free0 = free_at_init();

    std::vector<void*> blocks;
    for (;;) {
        void* p = heap().allocate(24);
        if (!p)
            break;
        blocks.push_back(p);
    }
    ASSERT_GT(blocks.size(), 100u);

    // 全部释放（每块恰好一次）。
    for (void* p : blocks)
        heap().deallocate(p);

    // TLSF 即时合并应把整堆还原为单个空闲块：free 精确回到 free0，零泄漏。
    EXPECT_EQ(heap().get_free_memory(), free0);
    EXPECT_EQ(heap().verify_integrity(), 0u);
}

// ---------------------------------------------------------------------------
// I3. 完全合并：交错释放制造碎片后再全释放，应能分配近整堆的单块。
//     这是对原测试无断言 defragment() 分支的真实替代——证明合并不需要
//     defragment()（它是 no-op），即时合并已把碎片还原为连续大块。
// ---------------------------------------------------------------------------
TEST_F(StressKernelHeap, CoalescingAfterFragmentationAllowsNearWholeHeapAlloc) {
    const size_t free0 = free_at_init();
    const size_t total = heap().get_total_memory();

    // 填满。
    std::vector<void*> blocks;
    for (;;) {
        void* p = heap().allocate(32);
        if (!p)
            break;
        blocks.push_back(p);
    }
    ASSERT_GT(blocks.size(), 100u);

    // 交错释放偶数下标，制造"空洞"碎片。
    for (size_t i = 0; i < blocks.size(); i += 2)
        heap().deallocate(blocks[i]);
    // 碎片状态下应无法分配一个近整堆的大块。
    EXPECT_EQ(heap().allocate(free0 - 8u), nullptr);

    // 释放剩余奇数下标（每块恰好一次）——触发链式合并。
    for (size_t i = 1; i < blocks.size(); i += 2)
        heap().deallocate(blocks[i]);

    EXPECT_EQ(heap().get_free_memory(), free0);
    EXPECT_EQ(heap().verify_integrity(), 0u);

    // 完全合并后，应能分配一个几乎占满整堆的单块。
    void* big = heap().allocate(free0 - 8u);
    EXPECT_NE(big, nullptr) << "碎片未完全合并：无法分配近整堆块";
    heap().deallocate(big);

    // 超过整堆容量的请求必须失败（边界）。
    EXPECT_EQ(heap().allocate(total), nullptr);
    EXPECT_EQ(heap().get_free_memory(), free0);
}

// ---------------------------------------------------------------------------
// I4. 长时间 churn 稳定性：多轮可变大小分配 + 交错释放，free_memory 零漂移、
//     堆完整性恒为 0。这是"stress"的核心——大量重复操作后无累积泄漏/损坏。
//     替代原测试里失败即自我豁免的 if(!big_ptr) 分支。
// ---------------------------------------------------------------------------
TEST_F(StressKernelHeap, LongRunChurnHasNoDriftAndNoCorruption) {
    constexpr int kRounds = 200;
    constexpr int kBlocksPerRound = 100;

    const size_t free0 = free_at_init();

    for (int round = 0; round < kRounds; ++round) {
        std::vector<void*> live;
        std::vector<char> freed;
        live.reserve(kBlocksPerRound);
        freed.reserve(kBlocksPerRound);

        // 可变大小分配（8..72 字节），模拟真实混合负载。
        for (int i = 0; i < kBlocksPerRound; ++i) {
            void* p = heap().allocate(8u + static_cast<size_t>(i % 9) * 8u);
            if (p) {
                live.push_back(p);
                freed.push_back(0);
            }
        }
        ASSERT_FALSE(live.empty()) << "round " << round << " 未分配到任何块";

        // 交错释放偶数下标制造碎片（每块恰好一次）。
        for (size_t i = 0; i < live.size(); i += 2) {
            heap().deallocate(live[i]);
            freed[i] = 1;
        }
        // 释放剩余（跳过已释放，避免双重释放污染本轮不变量）。
        for (size_t i = 0; i < live.size(); ++i) {
            if (!freed[i]) {
                heap().deallocate(live[i]);
                freed[i] = 1;
            }
        }

        // 每轮结束：free 精确回到初始值，堆完整。任何漂移都意味着泄漏或
        // 合并记账错误。
        ASSERT_EQ(heap().get_free_memory(), free0) << "round " << round << " free_memory 漂移";
        ASSERT_EQ(heap().verify_integrity(), 0u) << "round " << round << " 堆损坏";
    }

    // 长跑之后仍能分配近整堆块，证明无残留碎片。
    void* big = heap().allocate(free0 - 8u);
    EXPECT_NE(big, nullptr);
    heap().deallocate(big);
    EXPECT_EQ(heap().get_free_memory(), free0);
}

// ---------------------------------------------------------------------------
// I5. 双重释放防御：受控 double-free 必须被 is_free guard 拦截，
//     free_memory 不得溢出 total，堆完整性保持。
//     对应 AGENTS.md #33 Safety 类别的 "double free"。
// ---------------------------------------------------------------------------
TEST_F(StressKernelHeap, DoubleFreeIsGuardedWithoutCorruption) {
    const size_t free0 = free_at_init();
    const size_t total = heap().get_total_memory();

    void* a = heap().allocate(32);
    void* b = heap().allocate(32);
    void* c = heap().allocate(32);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    ASSERT_NE(c, nullptr);

    // 释放 a，再释放相邻的 b（触发 a+b 合并），然后对已被吸收的 a 二次释放。
    heap().deallocate(a);
    heap().deallocate(b);
    heap().deallocate(a); // 双重释放：a 现为合并块的头部，is_free=true → 应被拦截

    // guard 生效：堆未损坏，free_memory 未因重复累加而溢出 total。
    EXPECT_EQ(heap().verify_integrity(), 0u);
    EXPECT_LE(heap().get_free_memory(), total);

    // 释放 c 后，链式合并应把整堆还原，free 精确回到 free0。
    heap().deallocate(c);
    EXPECT_EQ(heap().get_free_memory(), free0);
    EXPECT_EQ(heap().verify_integrity(), 0u);
}
