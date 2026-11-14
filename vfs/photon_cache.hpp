#ifndef AURORA_PHOTON_CACHE_HPP
#define AURORA_PHOTON_CACHE_HPP

#include <stdint.h>
#include "../drivers/storage/flash_device.hpp"
#include "mutex.hpp"

struct CachePage {
    uint32_t block_addr;
    uint32_t page_offset;
    uint8_t data[256]; // 对齐闪存 256B 物理页
    bool is_valid;
    bool is_dirty;
    bool is_reserved;
    uint32_t last_access_tick;
};

class PhotonCacheLayer {
private:
    static constexpr int CACHE_POOL_SIZE = 8; // 在 RAM 中开辟 8 个页缓存槽位 (共 2KB)
    CachePage pool_[CACHE_POOL_SIZE];
    FlashBlockDevice& flash_;
    mutable Mutex cache_mutex_;
    uint32_t tick_counter_;
    uint32_t hit_count_ = 0;
    uint32_t miss_count_ = 0;
    uint32_t flush_count_ = 0;

    // 内部方法：单调递增访问时间戳，接近 32 位上限时保序重整以防溢出破坏 LRU 淘汰
    uint32_t next_tick_locked() {
        if (tick_counter_ >= 0xFFFF0000u) {
            uint32_t new_ticks[CACHE_POOL_SIZE] = {};
            uint32_t max_rank = 0;
            for (int i = 0; i < CACHE_POOL_SIZE; i++) {
                if (!pool_[i].is_valid) {
                    continue;
                }
                uint32_t rank = 1;
                for (int j = 0; j < CACHE_POOL_SIZE; j++) {
                    if (i == j || !pool_[j].is_valid) {
                        continue;
                    }
                    if (pool_[j].last_access_tick < pool_[i].last_access_tick ||
                        (pool_[j].last_access_tick == pool_[i].last_access_tick && j < i)) {
                        rank++;
                    }
                }
                new_ticks[i] = rank;
                if (rank > max_rank) {
                    max_rank = rank;
                }
            }
            for (int i = 0; i < CACHE_POOL_SIZE; i++) {
                if (pool_[i].is_valid) {
                    pool_[i].last_access_tick = new_ticks[i];
                }
            }
            tick_counter_ = max_rank;
        }
        return ++tick_counter_;
    }

    // 内部方法：将脏页真正刷入底层闪存
    int flush_page(int index) {
        if (!pool_[index].is_valid || !pool_[index].is_dirty)
            return 0;

        uint32_t page_size = flash_.get_page_size();
        if (page_size == 0 || page_size > 256) {
            page_size = 256;
        }

        int res = -1;
        // 失败重试 3 次再上报
        for (int retry = 0; retry < 3; retry++) {
            res = flash_.write_blocks(pool_[index].block_addr, pool_[index].page_offset, pool_[index].data, page_size);
            if (res == 0)
                break;
        }

        if (res == 0) {
            pool_[index].is_dirty = false;
            flush_count_++;
        }
        return res;
    }

    // 内部方法：LRU (最近最少使用) 算法淘汰替换缓存槽位
    int get_or_alloc_page(uint32_t block_addr, uint32_t page_offset, bool for_write) {
        (void)for_write;
        for (int attempt = 0; attempt < 4; attempt++) {
            int oldest_idx = -1;
            uint32_t oldest_tick = 0xFFFFFFFFu;

            // 1. 命中检测
            for (int i = 0; i < CACHE_POOL_SIZE; i++) {
                if (pool_[i].is_valid && pool_[i].block_addr == block_addr && pool_[i].page_offset == page_offset) {
                    pool_[i].last_access_tick = next_tick_locked();
                    hit_count_++;
                    return i;
                }
                if (!pool_[i].is_valid && !pool_[i].is_reserved) {
                    oldest_idx = i;
                    oldest_tick = 0;
                } else if (!pool_[i].is_reserved && (oldest_idx == -1 || pool_[i].last_access_tick < oldest_tick)) {
                    oldest_idx = i;
                    oldest_tick = pool_[i].last_access_tick;
                }
            }

            miss_count_++;

            if (oldest_idx == -1) {
                return -1; // 所有槽位均被其他线程保留
            }

            // 2. 缓存未命中，如果被淘汰的页是脏页，必须先将其落盘保护
            if (pool_[oldest_idx].is_valid && pool_[oldest_idx].is_dirty) {
                int res = flush_page(oldest_idx);
                if (res != 0) {
                    return -1; // 刷盘失败，拒绝覆盖该脏页，向上层报错
                }
            }

            // 3. 将槽位先标记为无效（在解锁 I/O 窗口期间，其他线程对此槽位的
            //    命中检测将得到 is_valid=false，从而不会读到未填充的陈旧数据）。
            pool_[oldest_idx].is_valid = false;
            pool_[oldest_idx].is_dirty = false;
            pool_[oldest_idx].is_reserved = true;
            pool_[oldest_idx].block_addr = block_addr;
            pool_[oldest_idx].page_offset = page_offset;

            // 4. 在持锁状态下解锁，使用栈上临时缓冲区执行 flash 读取，
            //    避免在 I/O 期间 UI 任务因等锁而掉帧。
            uint8_t tmp_data[256];
            uint32_t page_size = flash_.get_page_size();
            if (page_size > 256)
                page_size = 256;

            cache_mutex_.unlock();
            int read_res = flash_.read_blocks(block_addr, page_offset, tmp_data, page_size);
            cache_mutex_.lock();

            if (read_res != 0) {
                // 底层读取失败，严禁将未初始化的 tmp_data 标为有效
                pool_[oldest_idx].is_reserved = false;
                pool_[oldest_idx].is_valid = false;
                pool_[oldest_idx].is_dirty = false;
                pool_[oldest_idx].last_access_tick = 0;
                return -1;
            }

            // 重新检查是否被其他线程加载 (防止冗余并避免覆盖)
            for (int i = 0; i < CACHE_POOL_SIZE; i++) {
                if (i != oldest_idx && pool_[i].is_valid && pool_[i].block_addr == block_addr &&
                    pool_[i].page_offset == page_offset) {
                    pool_[oldest_idx].is_reserved = false;
                    pool_[i].last_access_tick = next_tick_locked();
                    return i;
                }
            }

            // 如果在 I/O 期间该块被 erase 擦除，erase 会清除 is_reserved；以有界循环重试一次
            if (!pool_[oldest_idx].is_reserved) {
                continue;
            }

            // 5. I/O 完成后，在持锁的原子步骤里将数据与元数据一并写入槽位，
            //    最后才翻 is_valid 对外可见。
            for (uint32_t i = 0; i < page_size; i++) {
                pool_[oldest_idx].data[i] = tmp_data[i];
            }
            pool_[oldest_idx].block_addr = block_addr;
            pool_[oldest_idx].page_offset = page_offset;
            pool_[oldest_idx].last_access_tick = next_tick_locked();
            pool_[oldest_idx].is_valid = true; // ← 数据确认有效后才对外可见
            pool_[oldest_idx].is_reserved = false;

            return oldest_idx;
        }
        return -1;
    }

public:
    PhotonCacheLayer(FlashBlockDevice& flash) : flash_(flash), tick_counter_(0),
                                                hit_count_(0), miss_count_(0), flush_count_(0) {
        for (int i = 0; i < CACHE_POOL_SIZE; i++) {
            pool_[i].is_valid = false;
            pool_[i].is_dirty = false;
            pool_[i].is_reserved = false;
        }
    }

    ~PhotonCacheLayer() {
        sync();
    }

    // ========================================================
    // 光子缓存读取：0 闪存 I/O 极速 RAM 命中返回
    // ========================================================
    int read(uint32_t block_addr, uint32_t offset, uint8_t* buf, uint32_t size) {
        if (!buf)
            return -1;
        if (size == 0)
            return 0;

        LockGuard guard(cache_mutex_);
        uint32_t page_size = flash_.get_page_size();
        uint32_t block_size = flash_.get_block_size();
        if (page_size == 0 || (page_size & (page_size - 1)) != 0 || block_size == 0)
            return -1;
        if (page_size > 256)
            page_size = 256;
        if (block_addr >= flash_.get_block_count() || offset > block_size || size > block_size - offset)
            return -1;

        uint32_t bytes_read = 0;

        while (bytes_read < size) {
            uint32_t curr_offset = offset + bytes_read;
            uint32_t page_offset = curr_offset & ~(page_size - 1);
            uint32_t in_page_idx = curr_offset & (page_size - 1);
            uint32_t chunk = page_size - in_page_idx;
            if (chunk > size - bytes_read)
                chunk = size - bytes_read;

            int cache_idx = get_or_alloc_page(block_addr, page_offset, false);
            if (cache_idx == -1) {
                return -1;
            }

            for (uint32_t i = 0; i < chunk; i++) {
                buf[bytes_read + i] = pool_[cache_idx].data[in_page_idx + i];
            }
            bytes_read += chunk;
        }
        return static_cast<int>(bytes_read);
    }

    // ========================================================
    // 光子缓存写入：将小写聚拢在 RAM 缓冲页，延迟物理落盘
    // ========================================================
    int write(uint32_t block_addr, uint32_t offset, const uint8_t* buf, uint32_t size) {
        if (!buf)
            return -1;
        if (size == 0)
            return 0;

        LockGuard guard(cache_mutex_);
        uint32_t page_size = flash_.get_page_size();
        uint32_t block_size = flash_.get_block_size();
        if (page_size == 0 || (page_size & (page_size - 1)) != 0 || block_size == 0)
            return -1;
        if (page_size > 256)
            page_size = 256;
        if (block_addr >= flash_.get_block_count() || offset > block_size || size > block_size - offset)
            return -1;

        uint32_t bytes_written = 0;

        while (bytes_written < size) {
            uint32_t curr_offset = offset + bytes_written;
            uint32_t page_offset = curr_offset & ~(page_size - 1);
            uint32_t in_page_idx = curr_offset & (page_size - 1);
            uint32_t chunk = page_size - in_page_idx;
            if (chunk > size - bytes_written)
                chunk = size - bytes_written;

            int cache_idx = get_or_alloc_page(block_addr, page_offset, true);
            if (cache_idx == -1) {
                return -1;
            }

            for (uint32_t i = 0; i < chunk; i++) {
                pool_[cache_idx].data[in_page_idx + i] = buf[bytes_written + i];
            }
            pool_[cache_idx].is_dirty = true;
            bytes_written += chunk;
        }
        return static_cast<int>(bytes_written);
    }

    int erase(uint32_t block_addr) {
        LockGuard guard(cache_mutex_);
        // 等待同块正在进行的异步读页结束，避免旧数据在擦除后写回缓存
        for (;;) {
            bool busy = false;
            for (int i = 0; i < CACHE_POOL_SIZE; i++) {
                if (pool_[i].is_reserved && pool_[i].block_addr == block_addr) {
                    busy = true;
                    break;
                }
            }
            if (!busy) {
                break;
            }
            cache_mutex_.unlock();
            sys_yield();
            cache_mutex_.lock();
        }

        // 全程持锁完成物理擦除；仅擦除成功后才失效缓存，避免擦除失败造成脏页静默丢失
        int res = flash_.erase_block(block_addr);
        if (res != 0) {
            return res;
        }
        for (int i = 0; i < CACHE_POOL_SIZE; i++) {
            if ((pool_[i].is_valid || pool_[i].is_reserved) && pool_[i].block_addr == block_addr) {
                pool_[i].is_valid = false;
                pool_[i].is_dirty = false;
                pool_[i].is_reserved = false;
                pool_[i].last_access_tick = 0;
            }
        }
        return 0;
    }

    // ========================================================
    // 显式同步 (Sync)：将所有脏页批量高效刷入闪存
    // ========================================================
    int sync() {
        int final_res = 0;
        LockGuard guard(cache_mutex_);
        for (int i = 0; i < CACHE_POOL_SIZE; i++) {
            if (pool_[i].is_valid && pool_[i].is_dirty) {
                int res = flush_page(i);
                if (res != 0 && final_res == 0) {
                    final_res = res;
                }
            }
        }
        return final_res;
    }

    // ---- 性能诊断与度量 ----
    uint32_t get_hit_count() const {
        LockGuard guard(cache_mutex_);
        return hit_count_;
    }

    uint32_t get_miss_count() const {
        LockGuard guard(cache_mutex_);
        return miss_count_;
    }

    uint32_t get_flush_count() const {
        LockGuard guard(cache_mutex_);
        return flush_count_;
    }

    uint32_t get_dirty_count() const {
        LockGuard guard(cache_mutex_);
        uint32_t count = 0;
        for (int i = 0; i < CACHE_POOL_SIZE; i++) {
            if (pool_[i].is_valid && pool_[i].is_dirty) {
                count++;
            }
        }
        return count;
    }

#ifdef AURORA_HOST_TEST
    void set_tick_counter_for_test(uint32_t tick) {
        LockGuard guard(cache_mutex_);
        tick_counter_ = tick;
    }

    uint32_t get_tick_counter_for_test() const {
        LockGuard guard(cache_mutex_);
        return tick_counter_;
    }
#endif
};

#endif
