#ifndef AURORA_FLASH_DEVICE_HPP
#define AURORA_FLASH_DEVICE_HPP

#include <stdint.h>
#include "device.hpp"
#include "mutex.hpp"

// 512KB = 4096 * 128，与默认构造参数保持一致。全局对象 g_nor_flash 的构造
// 发生在调度器/堆分配器就绪之前（甚至在 kernel_main 之前），因此这里不能
// 使用 new[]/堆分配 —— 否则在 CONFIG_NO_DYNAMIC_ALLOCATION 打开、或堆尚未
// 初始化时会直接触发 operator new[] 里的 panic 死循环。
#ifdef CONFIG_BOARD_MIBAND8
#define FLASH_BACKING_STORE_BYTES (4096u * 4u) // 16KB for MiBand8 SRAM limits
#else
#define FLASH_BACKING_STORE_BYTES (4096u * 128u)
#endif

class FlashBlockDevice : public BlockDevice {
private:
    static uint8_t memory_[FLASH_BACKING_STORE_BYTES]; // 静态内嵌存储，非堆分配
    uint32_t block_size_;                              // 扇区擦除大小：4KB (4096 Bytes)
    uint32_t block_count_;                             // 扇区总数：128 个 (总容量 512KB)
    uint32_t page_size_;                               // 物理页编程大小：256 Bytes
    Mutex hw_mutex_;

    uint32_t write_count_ = 0;
    uint32_t erase_count_ = 0;

public:
    FlashBlockDevice(const char* name, uint32_t block_size = 4096,
                     uint32_t block_count = (FLASH_BACKING_STORE_BYTES / 4096u), uint32_t page_size = 256)
        : BlockDevice(name), block_size_(block_size), block_count_(block_count), page_size_(page_size),
          write_count_(0), erase_count_(0) {
        // 容量必须不超过静态储备区，且不同实例不能共用同一块静态内存。
        uint32_t needed = block_size_ * block_count_;
        if (needed > FLASH_BACKING_STORE_BYTES) {
            Arch::disable_interrupts();
            while (true) {} // PANIC: 请求容量超过静态储备区，需调大 FLASH_BACKING_STORE_BYTES
        }
        // 物理闪存出厂默认全为 0xFF
        memset(memory_, 0xFF, needed);
    }

    ~FlashBlockDevice() {
        // memory_ 是静态存储，不需要 delete[]
    }

    int open() override {
        return 0;
    }

    int close() override {
        return 0;
    }

    // ========================================================
    // 闪存读取：快速内存拷贝直接读取物理块与偏移处数据
    // ========================================================
    int read_blocks(uint32_t block_addr, uint32_t offset, uint8_t* buf, uint32_t size) {
        if (!buf || block_addr >= block_count_ || offset + size > block_size_)
            return -1;

        LockGuard guard(hw_mutex_);
        uint32_t phys_addr = (block_addr * block_size_) + offset;
        memcpy(buf, &memory_[phys_addr], size);
        return 0;
    }

    // ========================================================
    // 闪存写入：优化字对齐按位与 (AND) 物理模拟写入
    // ========================================================
    int write_blocks(uint32_t block_addr, uint32_t offset, const uint8_t* buf, uint32_t size) {
        if (!buf || block_addr >= block_count_ || offset + size > block_size_)
            return -1;

        LockGuard guard(hw_mutex_);
        uint32_t phys_addr = (block_addr * block_size_) + offset;

        // 32-bit 对齐加速路径
        uint32_t idx = 0;
        if (((phys_addr & 0x3) == 0) && ((reinterpret_cast<uintptr_t>(buf) & 0x3) == 0)) {
            uint32_t words = size / 4;
            volatile uint32_t* dst32 = reinterpret_cast<volatile uint32_t*>(&memory_[phys_addr]);
            const uint32_t* src32 = reinterpret_cast<const uint32_t*>(buf);
            for (uint32_t w = 0; w < words; ++w) {
                dst32[w] &= src32[w];
            }
            idx = words * 4;
        }

        // 剩余字节按位与
        for (; idx < size; ++idx) {
            memory_[phys_addr + idx] &= buf[idx];
        }

        write_count_++;
        return 0;
    }

    // ========================================================
    // 闪存扇区擦除：将整块 4KB 区域重置为 0xFF
    // ========================================================
    int erase_block(uint32_t block_addr) {
        if (block_addr >= block_count_)
            return -1;

        LockGuard guard(hw_mutex_);
        uint32_t phys_addr = block_addr * block_size_;
        memset(&memory_[phys_addr], 0xFF, block_size_);
        erase_count_++;
        return 0;
    }

    // ========================================================
    // 擦除状态快速检测：检查物理块某段是否全为 0xFF
    // ========================================================
    bool is_erased(uint32_t block_addr, uint32_t offset = 0, uint32_t size = 0) {
        if (block_addr >= block_count_)
            return false;
        if (size == 0)
            size = block_size_ - offset;
        if (offset + size > block_size_)
            return false;

        LockGuard guard(hw_mutex_);
        uint32_t phys_addr = (block_addr * block_size_) + offset;
        for (uint32_t i = 0; i < size; ++i) {
            if (memory_[phys_addr + i] != 0xFF)
                return false;
        }
        return true;
    }

    uint32_t get_block_size() const {
        return block_size_;
    }

    uint32_t get_block_count() const {
        return block_count_;
    }

    uint32_t get_page_size() const {
        return page_size_;
    }

    uint32_t get_total_capacity() const {
        return block_size_ * block_count_;
    }

    uint32_t get_write_count() const {
        return write_count_;
    }

    uint32_t get_erase_count() const {
        return erase_count_;
    }
};

// 静态成员定义（放头文件需要 inline，避免多个 TU include 时重复定义）
inline uint8_t FlashBlockDevice::memory_[FLASH_BACKING_STORE_BYTES];

#endif
