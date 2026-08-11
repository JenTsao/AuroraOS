#ifndef MEMORY_HPP
#define MEMORY_HPP

#include <stdint.h>
#include <stddef.h>
#include "task.hpp"
#include "arch_api.hpp"
#include "../metrics/metrics.hpp"

class KernelHeap {
private:
    struct alignas(8) BlockHeader {
        uint32_t magic;       // 魔数校验，防止越界或非法指针
        size_t size;          // 包含头部在内的总大小
        size_t requested_size;// 原始请求大小（防止 realloc OOB read）
        bool is_free;         // 是否空闲
        BlockHeader* next;    // 指向下一个块的指针
    };
    static constexpr uint32_t HEAP_MAGIC = 0x4D454D4F; // "MEMO"

    BlockHeader* head_block = nullptr;
    size_t total_free_memory = 0;
    size_t total_size = 0;

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

        // 8字节对齐
        start = (start + 7) & ~7;
        end = end & ~7;

        head_block = reinterpret_cast<BlockHeader*>(start);
        head_block->magic = HEAP_MAGIC;
        head_block->size = end - start;
        head_block->requested_size = 0;
        head_block->is_free = true;
        head_block->next = nullptr;

        total_size = end - start;
        total_free_memory = head_block->size - sizeof(BlockHeader);
    }

    size_t get_total_memory() const { return total_size; }
    size_t get_free_memory() const { return total_free_memory; }

    // 分配内存
    void* allocate(size_t size) {
        uint32_t t0 = Arch::get_cycle();
        void* p = allocate_impl(size);
        uint32_t dt = Arch::get_cycle() - t0;
        if (size <= 64) Metrics::record(METRIC_HEAP_64B, dt);
        return p;
    }

private:
    void* allocate_impl(size_t size) {
        IrqGuard lock; // CP.20: RAII 线程安全保护，改用关中断自旋锁避免死锁
        
        if (size > SIZE_MAX - 7 - sizeof(BlockHeader)) {
            return nullptr;
        }
        
        size_t size_orig = size;
        // 8字节对齐
        size = (size + 7) & ~7;
        size_t required_space = size + sizeof(BlockHeader);

        void* result = try_allocate_internal(size_orig, required_space);
        if (result != nullptr) return result;

        // OOM detected! Try lazy defragmentation (coalesce adjacent free blocks)
        defragment_internal();
        
        // Try allocating one more time
        return try_allocate_internal(size_orig, required_space);
    }

private:
    void* try_allocate_internal(size_t size_orig, size_t required_space) {
        BlockHeader* current = head_block;
        while (current != nullptr) {
            if (current->is_free && current->size >= required_space) {
                bool did_split = false;
                // 如果剩余空间足够切分，就分裂该块 (Split Block)
                if (current->size >= required_space + sizeof(BlockHeader) + 8) {
                    did_split = true;
                    BlockHeader* next_block = reinterpret_cast<BlockHeader*>(
                        reinterpret_cast<uintptr_t>(current) + required_space
                    );
                    next_block->magic = HEAP_MAGIC;
                    next_block->size = current->size - required_space;
                    next_block->requested_size = 0;
                    next_block->is_free = true;
                    next_block->next = current->next;

                    current->size = required_space;
                    current->next = next_block;
                }

                current->is_free = false;
                current->requested_size = size_orig; // 记录原始请求大小
                if (did_split) {
                    total_free_memory -= required_space;
                } else {
                    total_free_memory -= (current->size - sizeof(BlockHeader));
                }
                // 返回越过 BlockHeader 之后的实际可用内存地址
                return reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(current) + sizeof(BlockHeader));
            }
            current = current->next;
        }
        return nullptr; // OOM: 内存不足
    }

    void defragment_internal() {
        Metrics::inc_heap_defrag();
        BlockHeader* current = head_block;
        while (current != nullptr && current->next != nullptr) {
            if (current->is_free && current->next->is_free) {
                current->size += current->next->size;
                total_free_memory += sizeof(BlockHeader); // 恢复一个 header 的空间
                current->next = current->next->next;
            } else {
                current = current->next;
            }
        }
    }
public:
    void defragment() {
        IrqGuard lock;
        defragment_internal();
    }

    // 释放内存
    void deallocate(void* ptr) {
        if (!ptr) return;
        IrqGuard lock; // CP.20: RAII 线程安全保护

        BlockHeader* target = reinterpret_cast<BlockHeader*>(
            reinterpret_cast<uintptr_t>(ptr) - sizeof(BlockHeader)
        );

        // 边界及魔数检查：防止越界/非法指针
        uintptr_t target_addr = reinterpret_cast<uintptr_t>(target);
        uintptr_t heap_start = reinterpret_cast<uintptr_t>(head_block);
        uintptr_t heap_end = heap_start + total_size;
        if (target_addr < heap_start || target_addr >= heap_end) {
            return; // 非法指针，拒绝释放
        }
        if (target->magic != HEAP_MAGIC) {
            return; // 内存损坏或未对齐指针，拒绝释放
        }

        // 双重释放检查
        if (target->is_free) {
            return; // Double-free detected
        }

        target->is_free = true;
        target->requested_size = 0;
        total_free_memory += (target->size - sizeof(BlockHeader));
        // O(1) deallocation: lazy coalescing happens during allocate()
    }

    // 获取已分配内存块的原始请求大小
    size_t get_requested_size(void* ptr) {
        if (!ptr) return 0;
        IrqGuard lock;
        BlockHeader* target = reinterpret_cast<BlockHeader*>(
            reinterpret_cast<uintptr_t>(ptr) - sizeof(BlockHeader)
        );
        // 边界与魔数检查
        uintptr_t target_addr = reinterpret_cast<uintptr_t>(target);
        uintptr_t heap_start = reinterpret_cast<uintptr_t>(head_block);
        uintptr_t heap_end = heap_start + total_size;
        if (target_addr < heap_start || target_addr >= heap_end || target->is_free || target->magic != HEAP_MAGIC) {
            return 0; 
        }
        return target->requested_size;
    }
};

#endif
