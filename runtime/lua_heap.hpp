#ifndef AURORA_LUA_HEAP_HPP
#define AURORA_LUA_HEAP_HPP

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "../kernel/core/arch_api.hpp"
#include "../kernel/mm/memory_attributes.hpp"

// =============================================================================
// LuaHeap — Lua 5.4 专属隔离内存池 (16KB ~ 32KB)
//
// 核心设计目标：
//   1. 彻底切断 Lua GC 抖动对内核堆 (KernelHeap) 的碎片化污染；
//   2. 基于紧凑型 TLSF (Two-Level Segregated Fit) 实时分配算法：
//      - 头部仅 8 字节 (4B size_flags + 4B prev_size)，消除 64-bit/32-bit 内存冗余；
//      - 空闲链表指针复用空闲 Payload 区域，将元数据开销降到最低；
//   3. O(1) 原地伸缩与即时物理双向合并 (Boundary Tags)；
//   4. O(1) 瞬时 reset：虚拟机销毁时 1 个操作清空私有池，0 碎片、0 泄漏。
// =============================================================================

template <size_t PoolSize = 32768>
class LuaHeap {
private:
    static constexpr uint32_t FLAG_FREE = 0x01u;
    static constexpr uint32_t FLAG_PREV_FREE = 0x02u;
    static constexpr uint32_t SIZE_MASK = ~0x07u;

    struct alignas(8) BlockHeader {
        uint32_t size_flags; // [31:3]=size, [1]=prev_free, [0]=is_free
        uint32_t prev_size;  // 物理前驱块大小 (用于 O(1) 前向合并)
    };

    struct FreeNode {
        BlockHeader header;
        FreeNode* next_free;
        FreeNode* prev_free;
    };

    static constexpr size_t MIN_BLOCK_SIZE = sizeof(FreeNode); // 24B on 64-bit, 16B on 32-bit
    static constexpr int SL_INDEX_COUNT_LOG2 = 4;
    static constexpr int SL_INDEX_COUNT = 1 << SL_INDEX_COUNT_LOG2; // 16
    static constexpr int FL_INDEX_SHIFT = 5;                        // 2^5 = 32B
    static constexpr int FL_INDEX_MAX = 17;                         // 2^17 = 128KB
    static constexpr int FL_INDEX_COUNT = (FL_INDEX_MAX - FL_INDEX_SHIFT + 1); // 13

    alignas(8) uint8_t storage_[PoolSize]{};
    uint32_t fl_bitmap_{0};
    uint32_t sl_bitmap_[FL_INDEX_COUNT]{};
    FreeNode* blocks_[FL_INDEX_COUNT][SL_INDEX_COUNT]{};

    size_t total_free_memory_{0};
    size_t used_memory_{0};
    size_t peak_memory_{0};
    size_t alloc_count_{0};

    static inline size_t get_size(const BlockHeader* b) noexcept {
        return b->size_flags & SIZE_MASK;
    }

    static inline bool is_free(const BlockHeader* b) noexcept {
        return (b->size_flags & FLAG_FREE) != 0;
    }

    static inline bool is_prev_free(const BlockHeader* b) noexcept {
        return (b->size_flags & FLAG_PREV_FREE) != 0;
    }

    static inline void set_size(BlockHeader* b, size_t size) noexcept {
        b->size_flags = (b->size_flags & ~SIZE_MASK) | (static_cast<uint32_t>(size) & SIZE_MASK);
    }

    static inline void set_free(BlockHeader* b) noexcept {
        b->size_flags |= FLAG_FREE;
    }

    static inline void set_allocated(BlockHeader* b) noexcept {
        b->size_flags &= ~FLAG_FREE;
    }

    static inline void set_prev_free(BlockHeader* b) noexcept {
        b->size_flags |= FLAG_PREV_FREE;
    }

    static inline void set_prev_allocated(BlockHeader* b) noexcept {
        b->size_flags &= ~FLAG_PREV_FREE;
    }

    static inline BlockHeader* get_next_phys(BlockHeader* b) noexcept {
        return reinterpret_cast<BlockHeader*>(reinterpret_cast<uintptr_t>(b) + get_size(b));
    }

    static inline BlockHeader* get_prev_phys(BlockHeader* b) noexcept {
        return reinterpret_cast<BlockHeader*>(reinterpret_cast<uintptr_t>(b) - b->prev_size);
    }

    static void mapping_insert(size_t size, int& fl, int& sl) noexcept {
        if (size < (1u << FL_INDEX_SHIFT)) {
            fl = 0;
            sl = static_cast<int>(size / ((1u << FL_INDEX_SHIFT) / SL_INDEX_COUNT));
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
        if (size >= (1u << FL_INDEX_SHIFT)) {
            const int msb = Arch::find_highest_bit(static_cast<uint32_t>(size));
            const size_t round = (1u << (msb - SL_INDEX_COUNT_LOG2)) - 1;
            size += round;
        }
        mapping_insert(size, fl, sl);
    }

    FreeNode* search_suitable_block(int& fl, int& sl) noexcept {
        if (fl < 0) fl = 0;
        if (fl >= FL_INDEX_COUNT) return nullptr;

        uint32_t sl_map = sl_bitmap_[fl] & (~0u << sl);
        if (sl_map != 0) {
            sl = Arch::find_lowest_bit(sl_map);
            return blocks_[fl][sl];
        }

        if (fl + 1 >= FL_INDEX_COUNT) return nullptr;
        uint32_t fl_map = fl_bitmap_ & (~0u << (fl + 1));
        if (fl_map == 0) return nullptr;

        fl = Arch::find_lowest_bit(fl_map);
        sl = Arch::find_lowest_bit(sl_bitmap_[fl]);
        return blocks_[fl][sl];
    }

    void insert_free_block(FreeNode* node) noexcept {
        set_free(&node->header);
        const size_t sz = get_size(&node->header);

        // 通知物理后驱：其前驱现在是空闲的
        BlockHeader* next_phys = get_next_phys(&node->header);
        if (reinterpret_cast<uintptr_t>(next_phys) < reinterpret_cast<uintptr_t>(&storage_[0]) + PoolSize) {
            set_prev_free(next_phys);
            next_phys->prev_size = static_cast<uint32_t>(sz);
        }

        int fl = 0, sl = 0;
        mapping_insert(sz, fl, sl);

        node->prev_free = nullptr;
        node->next_free = blocks_[fl][sl];
        if (blocks_[fl][sl]) {
            blocks_[fl][sl]->prev_free = node;
        }
        blocks_[fl][sl] = node;

        fl_bitmap_ |= (1u << fl);
        sl_bitmap_[fl] |= (1u << sl);
    }

    void remove_free_block(FreeNode* node) noexcept {
        const size_t sz = get_size(&node->header);
        int fl = 0, sl = 0;
        mapping_insert(sz, fl, sl);

        if (node->prev_free) {
            node->prev_free->next_free = node->next_free;
        } else {
            blocks_[fl][sl] = node->next_free;
        }

        if (node->next_free) {
            node->next_free->prev_free = node->prev_free;
        }

        if (blocks_[fl][sl] == nullptr) {
            sl_bitmap_[fl] &= ~(1u << sl);
            if (sl_bitmap_[fl] == 0) {
                fl_bitmap_ &= ~(1u << fl);
            }
        }

        node->next_free = nullptr;
        node->prev_free = nullptr;
    }

public:
    LuaHeap() {
        reset();
    }

    void reset() noexcept {
        fl_bitmap_ = 0;
        for (int i = 0; i < FL_INDEX_COUNT; ++i) {
            sl_bitmap_[i] = 0;
            for (int j = 0; j < SL_INDEX_COUNT; ++j) {
                blocks_[i][j] = nullptr;
            }
        }

        FreeNode* initial_node = reinterpret_cast<FreeNode*>(&storage_[0]);
        initial_node->header.size_flags = static_cast<uint32_t>(PoolSize) | FLAG_FREE;
        initial_node->header.prev_size = 0;
        initial_node->next_free = nullptr;
        initial_node->prev_free = nullptr;

        total_free_memory_ = PoolSize - sizeof(BlockHeader);
        used_memory_ = 0;
        peak_memory_ = 0;
        alloc_count_ = 0;

        insert_free_block(initial_node);
    }

    void* allocate(size_t size) noexcept {
        if (size == 0 || size > PoolSize - sizeof(BlockHeader)) {
            return nullptr;
        }

        // 8 字节对齐
        size = (size + 7) & ~7;
        size_t required_block_size = size + sizeof(BlockHeader);
        if (required_block_size < MIN_BLOCK_SIZE) {
            required_block_size = MIN_BLOCK_SIZE;
        }

        int fl = 0, sl = 0;
        mapping_search(required_block_size, fl, sl);

        FreeNode* node = search_suitable_block(fl, sl);
        if (!node) {
            return nullptr; // 堆内存耗尽
        }

        remove_free_block(node);

        const size_t current_sz = get_size(&node->header);
        if (current_sz >= required_block_size + MIN_BLOCK_SIZE) {
            // 切割出剩余空闲块
            const size_t remaining_sz = current_sz - required_block_size;
            set_size(&node->header, required_block_size);

            FreeNode* rem = reinterpret_cast<FreeNode*>(reinterpret_cast<uintptr_t>(node) + required_block_size);
            rem->header.size_flags = static_cast<uint32_t>(remaining_sz);
            rem->header.prev_size = static_cast<uint32_t>(required_block_size);
            insert_free_block(rem);
        }

        set_allocated(&node->header);

        // 通知后继物理块：前驱已占用
        BlockHeader* next_phys = get_next_phys(&node->header);
        if (reinterpret_cast<uintptr_t>(next_phys) < reinterpret_cast<uintptr_t>(&storage_[0]) + PoolSize) {
            set_prev_allocated(next_phys);
            next_phys->prev_size = static_cast<uint32_t>(get_size(&node->header));
        }

        const size_t payload_sz = get_size(&node->header) - sizeof(BlockHeader);
        used_memory_ += payload_sz;
        if (total_free_memory_ >= payload_sz) {
            total_free_memory_ -= payload_sz;
        } else {
            total_free_memory_ = 0;
        }
        if (used_memory_ > peak_memory_) {
            peak_memory_ = used_memory_;
        }
        alloc_count_++;

        return reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(node) + sizeof(BlockHeader));
    }

    void deallocate(void* ptr) noexcept {
        if (!ptr) return;

        BlockHeader* block = reinterpret_cast<BlockHeader*>(reinterpret_cast<uintptr_t>(ptr) - sizeof(BlockHeader));
        uintptr_t block_addr = reinterpret_cast<uintptr_t>(block);
        uintptr_t heap_start = reinterpret_cast<uintptr_t>(&storage_[0]);
        uintptr_t heap_end = heap_start + PoolSize;

        if (block_addr < heap_start || block_addr >= heap_end || is_free(block)) {
            return;
        }

        const size_t payload_sz = get_size(block) - sizeof(BlockHeader);
        used_memory_ = (used_memory_ >= payload_sz) ? (used_memory_ - payload_sz) : 0;
        total_free_memory_ += payload_sz;

        // O(1) 物理后向合并
        BlockHeader* next_phys = get_next_phys(block);
        if (reinterpret_cast<uintptr_t>(next_phys) < heap_end && is_free(next_phys)) {
            remove_free_block(reinterpret_cast<FreeNode*>(next_phys));
            set_size(block, get_size(block) + get_size(next_phys));
        }

        // O(1) 物理前向合并
        if (is_prev_free(block)) {
            BlockHeader* prev_phys = get_prev_phys(block);
            remove_free_block(reinterpret_cast<FreeNode*>(prev_phys));
            set_size(prev_phys, get_size(prev_phys) + get_size(block));
            block = prev_phys;
        }

        insert_free_block(reinterpret_cast<FreeNode*>(block));
    }

    void* reallocate(void* ptr, size_t osize, size_t nsize) noexcept {
        if (nsize == 0) {
            if (ptr) {
                deallocate(ptr);
            }
            return nullptr;
        }

        if (!ptr) {
            return allocate(nsize);
        }

        BlockHeader* block = reinterpret_cast<BlockHeader*>(reinterpret_cast<uintptr_t>(ptr) - sizeof(BlockHeader));
        const size_t current_payload = get_size(block) - sizeof(BlockHeader);

        // 原地复用现有块（避免频繁 malloc/free/memcpy）
        if (nsize <= current_payload) {
            return ptr;
        }

        // 尝试就地与下一个空闲块合并
        BlockHeader* next_phys = get_next_phys(block);
        uintptr_t heap_end = reinterpret_cast<uintptr_t>(&storage_[0]) + PoolSize;
        if (reinterpret_cast<uintptr_t>(next_phys) < heap_end && is_free(next_phys)) {
            size_t combined = current_payload + get_size(next_phys);
            if (nsize <= combined) {
                remove_free_block(reinterpret_cast<FreeNode*>(next_phys));
                set_size(block, get_size(block) + get_size(next_phys));

                // 如果合并后过大，切出剩余空间
                size_t aligned_nsize = (nsize + 7) & ~7;
                size_t req_block_size = aligned_nsize + sizeof(BlockHeader);
                if (req_block_size < MIN_BLOCK_SIZE) req_block_size = MIN_BLOCK_SIZE;

                if (get_size(block) >= req_block_size + MIN_BLOCK_SIZE) {
                    size_t rem_sz = get_size(block) - req_block_size;
                    set_size(block, req_block_size);
                    FreeNode* rem = reinterpret_cast<FreeNode*>(reinterpret_cast<uintptr_t>(block) + req_block_size);
                    rem->header.size_flags = static_cast<uint32_t>(rem_sz);
                    rem->header.prev_size = static_cast<uint32_t>(req_block_size);
                    insert_free_block(rem);
                }

                BlockHeader* next_next = get_next_phys(block);
                if (reinterpret_cast<uintptr_t>(next_next) < heap_end) {
                    set_prev_allocated(next_next);
                    next_next->prev_size = static_cast<uint32_t>(get_size(block));
                }

                used_memory_ += (get_size(block) - sizeof(BlockHeader) - current_payload);
                if (used_memory_ > peak_memory_) peak_memory_ = used_memory_;
                return ptr;
            }
        }

        // 分配新块并复制数据
        void* new_ptr = allocate(nsize);
        if (new_ptr) {
            size_t copy_size = (osize < nsize) ? osize : nsize;
            if (copy_size > current_payload) copy_size = current_payload;
            memcpy(new_ptr, ptr, copy_size);
            deallocate(ptr);
        }
        return new_ptr;
    }

    size_t get_used_memory() const noexcept {
        return used_memory_;
    }

    size_t get_free_memory() const noexcept {
        return total_free_memory_;
    }

    size_t get_total_memory() const noexcept {
        return PoolSize;
    }

    size_t get_peak_memory() const noexcept {
        return peak_memory_;
    }
};

#endif // AURORA_LUA_HEAP_HPP
