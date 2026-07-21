#ifndef MEMORY_POOL_HPP
#define MEMORY_POOL_HPP

#include <stdint.h>
#include <stddef.h>
#include "mutex.hpp"

// 裸机环境自定义 placement new（newlib-nano 不提供 <new>）
inline void* operator new(size_t, void* p) noexcept { return p; }
inline void* operator new[](size_t, void* p) noexcept { return p; }

template <typename T, size_t PoolSize>
class MemoryPool {
private:
    // 使用对齐字节存储而非 union，避免 T 有非平凡构造函数时 union 默认构造被删除
    // 空闲时，前 sizeof(void*) 字节复用为 next 指针
    struct alignas(T) Slot {
        char storage[sizeof(T) > sizeof(void*) ? sizeof(T) : sizeof(void*)];
    };

    Slot buffer_[PoolSize];
    uint32_t allocated_[(PoolSize + 31) / 32];
    void* free_list_;   // 指向下一个空闲 Slot
    Mutex pool_mutex_;

    // 获取 Slot 内嵌的 next 指针（空闲链表用）
    static void** next_ptr(Slot* s) {
        return reinterpret_cast<void**>(s);
    }

public:
    MemoryPool() {
        free_list_ = &buffer_[0];
        for (size_t i = 0; i < PoolSize - 1; ++i) {
            *next_ptr(&buffer_[i]) = &buffer_[i + 1];
        }
        *next_ptr(&buffer_[PoolSize - 1]) = nullptr;
        for (size_t i = 0; i < (PoolSize + 31) / 32; ++i) {
            allocated_[i] = 0;
        }
    }

    // Allocate a raw slot from the pool in O(1) time
    T* allocate() {
        LockGuard lock(pool_mutex_);
        if (free_list_ == nullptr) {
            return nullptr; // Pool exhausted
        }
        Slot* slot = static_cast<Slot*>(free_list_);
        free_list_ = *next_ptr(slot);
        size_t index = slot - &buffer_[0];
        allocated_[index / 32] |= (1U << (index % 32));
        return reinterpret_cast<T*>(slot);
    }

    // Deallocate a slot back to the pool in O(1) time
    void deallocate(T* ptr) {
        if (!ptr) return;

        // 边界检查：确保 ptr 在 buffer_ 的范围内
        uintptr_t ptr_addr = reinterpret_cast<uintptr_t>(ptr);
        uintptr_t pool_start = reinterpret_cast<uintptr_t>(&buffer_[0]);
        uintptr_t pool_end = pool_start + sizeof(buffer_);

        if (ptr_addr < pool_start || ptr_addr >= pool_end) {
            return; // 越界指针
        }

        LockGuard lock(pool_mutex_);
        Slot* slot = reinterpret_cast<Slot*>(ptr);
        size_t index = slot - &buffer_[0];

        // Double free check
        if (!(allocated_[index / 32] & (1U << (index % 32)))) {
            return; // Already free!
        }
        allocated_[index / 32] &= ~(1U << (index % 32));

        // 将 Slot 重新链入空闲链表头部
        *next_ptr(slot) = free_list_;
        free_list_ = slot;
    }

    // Placement-new 构造：分配槽位并在其上构造 T 对象
    template <typename... Args>
    T* create(Args&&... args) {
        T* ptr = allocate();
        if (ptr) {
            new (ptr) T(static_cast<Args&&>(args)...);
        }
        return ptr;
    }

    // 析构并归还槽位
    void destroy(T* ptr) {
        if (ptr) {
            ptr->~T();
            deallocate(ptr);
        }
    }

    size_t get_capacity() const {
        return PoolSize;
    }
};

#endif // MEMORY_POOL_HPP
