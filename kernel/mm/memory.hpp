#ifndef MEMORY_HPP
#define MEMORY_HPP

#include <stdint.h>
#include <stddef.h>
#include "task.hpp"
#include "arch_api.hpp"
#include "../metrics/metrics.hpp"

// =============================================================================
// TLSF (Two-Level Segregated Fit) 实时内存分配器
//
// 嵌入式实时系统金标准分配器：
//   - O(1) 时间复杂度：分配与释放均在常数时间完成（无任何线性扫描循环）
//   - 即时物理双向合并：释放时基于 boundary tag 立即完成 O(1) 前后合并，杜绝碎片累积
//   - 硬件位图加速：基于 CLZ/CTZ 指令在单周期内完成两级 FL/SL 位图空闲链表检索
//   - 8 字节严格对齐，支持安全魔数与 OOB 防护
// =============================================================================

class KernelHeap {
private:
    struct alignas(8) BlockHeader {
        uint32_t magic;          // 魔数校验 0x544C5346 "TLSF"
        size_t size;             // 包含 BlockHeader 在内的总字节数 (8字节对齐)
        size_t requested_size;   // 用户原始请求大小
        bool is_free;            // 是否空闲
        BlockHeader* prev_phys;  // 物理相邻前驱块 (用于 O(1) 前向物理合并)
        BlockHeader* next_phys;  // 物理相邻后继块 (用于 O(1) 后向物理合并)
        BlockHeader* next_free;  // 空闲双向链表后继
        BlockHeader* prev_free;  // 空闲双向链表前驱
    };

    static constexpr uint32_t TLSF_MAGIC = 0x544C5346; // "TLSF"

    // TLSF 分级常数 (适配 32 位嵌入式与 64 位宿主机)
    static constexpr int SL_INDEX_COUNT_LOG2 = 4;
    static constexpr int SL_INDEX_COUNT = 1 << SL_INDEX_COUNT_LOG2; // 16
    static constexpr int FL_INDEX_SHIFT = 5;                        // 最小 First-Level 阶 2^5 = 32B
#if defined(CONFIG_BOARD_NUCLEO_L031K6)
    static constexpr int FL_INDEX_MAX = 13;                         // 最大 First-Level 阶 2^13 = 8KB (M0+ 8KB SRAM)
#else
    static constexpr int FL_INDEX_MAX = 24;                         // 最大 First-Level 阶 2^24 = 16MB (QEMU/MiBand/RV32/Host)
#endif
    static constexpr int FL_INDEX_COUNT = (FL_INDEX_MAX - FL_INDEX_SHIFT + 1);
    static constexpr size_t SMALL_BLOCK_SIZE = 1u << FL_INDEX_SHIFT;

    // TLSF 位图与二维空闲链表数组
    uint32_t fl_bitmap = 0;
    uint32_t sl_bitmap[FL_INDEX_COUNT]{};
    BlockHeader* blocks[FL_INDEX_COUNT][SL_INDEX_COUNT]{};

    BlockHeader* head_block = nullptr;
    size_t total_free_memory = 0;
    size_t total_size = 0;

    static void mapping_insert(size_t size, int& fl, int& sl) noexcept {
        if (size < SMALL_BLOCK_SIZE) {
            fl = 0;
            sl = static_cast<int>(size / (SMALL_BLOCK_SIZE / SL_INDEX_COUNT));
            if (sl >= SL_INDEX_COUNT) sl = SL_INDEX_COUNT - 1;
        } else {
            const int msb = Arch::find_highest_bit(static_cast<uint32_t>(size));
            fl = msb - FL_INDEX_SHIFT;
            if (fl < 0) fl = 0;
            if (fl >= FL_INDEX_COUNT) fl = FL_INDEX_COUNT - 1;
            sl = static_cast<int>((size ^ (1u << msb)) >> (msb - SL_INDEX_COUNT_LOG2));
            if (sl >= SL_INDEX_COUNT) sl = SL_INDEX_COUNT - 1;
        }
    }

    static void mapping_search(size_t size, int& fl, int& sl) noexcept {
        if (size >= SMALL_BLOCK_SIZE) {
            const int msb = Arch::find_highest_bit(static_cast<uint32_t>(size));
            const size_t round = (1u << (msb - SL_INDEX_COUNT_LOG2)) - 1;
            size += round;
        }
        mapping_insert(size, fl, sl);
    }

    BlockHeader* search_suitable_block(int& fl, int& sl) noexcept {
        if (fl < 0) fl = 0;
        if (fl >= FL_INDEX_COUNT) return nullptr;

        // 1. 先在当前 fl 的 sl_bitmap 中查找 >= sl 的子分类
        uint32_t sl_map = sl_bitmap[fl] & (~0u << sl);
        if (sl_map != 0) {
            sl = Arch::find_lowest_bit(sl_map);
            return blocks[fl][sl];
        }

        // 2. 当前 fl 无满足块，在更高 fl 中查找
        if (fl + 1 >= FL_INDEX_COUNT) return nullptr;
        uint32_t fl_map = fl_bitmap & (~0u << (fl + 1));
        if (fl_map == 0) return nullptr; // OOM

        fl = Arch::find_lowest_bit(fl_map);
        sl = Arch::find_lowest_bit(sl_bitmap[fl]);
        return blocks[fl][sl];
    }

    void insert_free_block(BlockHeader* block) noexcept {
        int fl = 0, sl = 0;
        mapping_insert(block->size, fl, sl);

        block->is_free = true;
        block->prev_free = nullptr;
        block->next_free = blocks[fl][sl];

        if (blocks[fl][sl]) {
            blocks[fl][sl]->prev_free = block;
        }
        blocks[fl][sl] = block;

        fl_bitmap |= (1u << fl);
        sl_bitmap[fl] |= (1u << sl);
    }

    void remove_free_block(BlockHeader* block) noexcept {
        int fl = 0, sl = 0;
        mapping_insert(block->size, fl, sl);

        if (block->prev_free) {
            block->prev_free->next_free = block->next_free;
        } else {
            blocks[fl][sl] = block->next_free;
        }

        if (block->next_free) {
            block->next_free->prev_free = block->prev_free;
        }

        if (blocks[fl][sl] == nullptr) {
            sl_bitmap[fl] &= ~(1u << sl);
            if (sl_bitmap[fl] == 0) {
                fl_bitmap &= ~(1u << fl);
            }
        }

        block->is_free = false;
        block->prev_free = nullptr;
        block->next_free = nullptr;
    }

public:
    KernelHeap() = default;
    KernelHeap(const KernelHeap&) = delete;
    KernelHeap& operator=(const KernelHeap&) = delete;

    static KernelHeap& instance() {
        static KernelHeap heap;
        return heap;
    }

    // 初始化堆，传入链接脚本暴露的起止地址
    void init(void* start_addr, void* end_addr) {
        uintptr_t start = reinterpret_cast<uintptr_t>(start_addr);
        uintptr_t end = reinterpret_cast<uintptr_t>(end_addr);

        if (end <= start || end - start < sizeof(BlockHeader) + 8) {
            Arch::disable_interrupts();
            while (true) {} // PANIC: heap region too small
        }

        // 8 字节对齐
        start = (start + 7) & ~7;
        end = end & ~7;

        // 清空 TLSF 状态表
        fl_bitmap = 0;
        for (int i = 0; i < FL_INDEX_COUNT; ++i) {
            sl_bitmap[i] = 0;
            for (int j = 0; j < SL_INDEX_COUNT; ++j) {
                blocks[i][j] = nullptr;
            }
        }

        head_block = reinterpret_cast<BlockHeader*>(start);
        head_block->magic = TLSF_MAGIC;
        head_block->size = end - start;
        head_block->requested_size = 0;
        head_block->is_free = true;
        head_block->prev_phys = nullptr;
        head_block->next_phys = nullptr;
        head_block->next_free = nullptr;
        head_block->prev_free = nullptr;

        total_size = end - start;
        total_free_memory = head_block->size - sizeof(BlockHeader);

        insert_free_block(head_block);
    }

    size_t get_total_memory() const {
        return total_size;
    }

    size_t get_free_memory() const {
        return total_free_memory;
    }

    uintptr_t get_heap_start() const {
        return reinterpret_cast<uintptr_t>(head_block);
    }

    uintptr_t get_heap_end() const {
        return reinterpret_cast<uintptr_t>(head_block) + total_size;
    }

    bool contains(const void* ptr, size_t len) const {
        if (!ptr || !head_block || total_size == 0)
            return false;
        uintptr_t p = reinterpret_cast<uintptr_t>(ptr);
        uintptr_t end = p + len;
        if (end < p)
            return false;
        uintptr_t hs = get_heap_start();
        uintptr_t he = get_heap_end();
        return (p >= hs) && (end <= he);
    }

    // 分配内存
    void* allocate(size_t size) {
        uint32_t t0 = Arch::get_cycle();
        void* p = allocate_impl(size);
        uint32_t dt = Arch::get_cycle() - t0;
        if (size <= 64)
            Metrics::record(METRIC_HEAP_64B, dt);
        return p;
    }

private:
    void* allocate_impl(size_t size) {
        IrqGuard lock; // CP.20: RAII 线程安全保护

        if (size == 0 || size > SIZE_MAX - 7 - sizeof(BlockHeader)) {
            return nullptr;
        }

        const size_t size_orig = size;
        // 8 字节对齐
        size = (size + 7) & ~7;
        const size_t required_space = size + sizeof(BlockHeader);

        int fl = 0, sl = 0;
        mapping_search(required_space, fl, sl);

        BlockHeader* block = search_suitable_block(fl, sl);
        if (!block) {
            return nullptr; // OOM: 内存不足
        }

        remove_free_block(block);

        // 如果剩余空间能够容纳完整 BlockHeader + 8 字节负载，执行 O(1) 物理块分裂
        if (block->size >= required_space + sizeof(BlockHeader) + 8) {
            BlockHeader* rem = reinterpret_cast<BlockHeader*>(reinterpret_cast<uintptr_t>(block) + required_space);
            rem->magic = TLSF_MAGIC;
            rem->size = block->size - required_space;
            rem->requested_size = 0;
            rem->is_free = true;
            rem->prev_phys = block;
            rem->next_phys = block->next_phys;
            if (block->next_phys) {
                block->next_phys->prev_phys = rem;
            }
            block->next_phys = rem;
            block->size = required_space;

            insert_free_block(rem);
        }

        block->is_free = false;
        block->requested_size = size_orig;
        total_free_memory -= (block->size - sizeof(BlockHeader));

        return reinterpret_cast<void*>(block + 1);
    }

public:
    // TLSF 具备即时合并特性，提供 defragment 保持 API 兼容
    void defragment() {
        IrqGuard lock;
        Metrics::inc_heap_defrag();
    }

    // 释放内存：O(1) 物理双向合并与空闲链表归位
    void deallocate(void* ptr) {
        if (!ptr)
            return;
        IrqGuard lock; // CP.20: RAII 线程安全保护

        BlockHeader* block = reinterpret_cast<BlockHeader*>(reinterpret_cast<uintptr_t>(ptr) - sizeof(BlockHeader));

        // 边界及魔数检查：防止越界/非法指针
        uintptr_t block_addr = reinterpret_cast<uintptr_t>(block);
        uintptr_t heap_start = reinterpret_cast<uintptr_t>(head_block);
        uintptr_t heap_end = heap_start + total_size;
        if (block_addr < heap_start || block_addr >= heap_end) {
            return; // 非法指针，拒绝释放
        }
        if (block->magic != TLSF_MAGIC) {
            return; // 内存损坏或未对齐指针，拒绝释放
        }
        if (block->is_free) {
            return; // Double-free detected
        }

        block->is_free = true;
        block->requested_size = 0;
        total_free_memory += (block->size - sizeof(BlockHeader));

        // 1. O(1) 物理后向合并 (Merge with next physical block)
        if (block->next_phys && block->next_phys->is_free && block->next_phys->magic == TLSF_MAGIC) {
            BlockHeader* next = block->next_phys;
            remove_free_block(next);
            block->size += next->size;
            block->next_phys = next->next_phys;
            if (next->next_phys) {
                next->next_phys->prev_phys = block;
            }
        }

        // 2. O(1) 物理前向合并 (Merge with prev physical block)
        if (block->prev_phys && block->prev_phys->is_free && block->prev_phys->magic == TLSF_MAGIC) {
            BlockHeader* prev = block->prev_phys;
            remove_free_block(prev);
            prev->size += block->size;
            prev->next_phys = block->next_phys;
            if (block->next_phys) {
                block->next_phys->prev_phys = prev;
            }
            block = prev;
        }

        insert_free_block(block);
    }

    // 获取已分配内存块的原始请求大小
    size_t get_requested_size(void* ptr) {
        if (!ptr)
            return 0;
        IrqGuard lock;
        BlockHeader* target = reinterpret_cast<BlockHeader*>(reinterpret_cast<uintptr_t>(ptr) - sizeof(BlockHeader));
        // 边界与魔数检查
        uintptr_t target_addr = reinterpret_cast<uintptr_t>(target);
        uintptr_t heap_start = reinterpret_cast<uintptr_t>(head_block);
        uintptr_t heap_end = heap_start + total_size;
        if (target_addr < heap_start || target_addr >= heap_end || target->is_free || target->magic != TLSF_MAGIC) {
            return 0;
        }
        return target->requested_size;
    }
};

#endif // MEMORY_HPP
