#ifndef PAGE_ALLOCATOR_HPP
#define PAGE_ALLOCATOR_HPP

#include <stdint.h>
#include <stddef.h>
#include "mutex.hpp"
#include "arch_api.hpp"

namespace auroraos {
namespace kernel {

class PageAllocator {
public:
    static constexpr size_t PAGE_SIZE = 4096;

    static PageAllocator& instance() {
        static PageAllocator allocator;
        return allocator;
    }

    void init(void* start_addr, size_t size) {
        // Align to page boundary
        uintptr_t start = (reinterpret_cast<uintptr_t>(start_addr) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        uintptr_t end = (reinterpret_cast<uintptr_t>(start_addr) + size) & ~(PAGE_SIZE - 1);

        base_addr_ = start;
        total_pages_ = (end - start) / PAGE_SIZE;
        free_pages_ = total_pages_;

        if (total_pages_ == 0) {
            free_list_head_ = nullptr;
            return;
        }

        // Simple free list: each free page contains pointer to next free page
        free_list_head_ = reinterpret_cast<void**>(base_addr_);
        void** current = free_list_head_;
        for (size_t i = 0; i < total_pages_ - 1; ++i) {
            void** next = reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(current) + PAGE_SIZE);
            *current = next;
            current = next;
        }
        *current = nullptr;
    }

    void* alloc_page() {
        IrqGuard lock; // RAII Thread Safety
        if (!free_list_head_)
            return nullptr;

        void* allocated = free_list_head_;
        free_list_head_ = reinterpret_cast<void**>(*free_list_head_);
        free_pages_--;

        // Zero out the page
        char* p = reinterpret_cast<char*>(allocated);
        for (size_t i = 0; i < PAGE_SIZE; ++i)
            p[i] = 0;

        return allocated;
    }

    void free_page(void* page) {
        if (!page)
            return;
        IrqGuard lock;

        // 边界与对齐校验（对齐 KernelHeap::deallocate 的三重防护思路）：
        // 拒绝越界/未对齐指针。
        uintptr_t addr = reinterpret_cast<uintptr_t>(page);
        uintptr_t base = base_addr_;
        uintptr_t end = base_addr_ + total_pages_ * PAGE_SIZE;
        if (total_pages_ == 0 || addr < base || addr >= end || (addr % PAGE_SIZE) != 0) {
            return; // 非法指针，拒绝释放
        }

        // 双重释放校验：遍历空闲链表确认该页尚未在列。
        // 页分配/释放为低频操作，O(n) 扫描可接受；重复释放会使空闲链表
        // 自环、free_pages_ 虚增，最终导致同一页被重复发出。
        // 步数以 free_pages_ 为界，链表遭破坏时也能终止。
        size_t steps = free_pages_;
        void** it = free_list_head_;
        while (it != nullptr && steps-- > 0) {
            if (it == page)
                return; // Double-free detected
            it = reinterpret_cast<void**>(*it);
        }

        void** p = reinterpret_cast<void**>(page);
        *p = free_list_head_;
        free_list_head_ = p;
        free_pages_++;
    }

    size_t get_free_pages() const {
        return free_pages_;
    }

    size_t get_total_pages() const {
        return total_pages_;
    }

private:
    PageAllocator() = default;

    uintptr_t base_addr_{0};
    size_t total_pages_{0};
    size_t free_pages_{0};
    void** free_list_head_{nullptr};
};

} // namespace kernel
} // namespace auroraos

#endif // PAGE_ALLOCATOR_HPP
