#ifndef AURORA_PHOTON_CACHE_HPP
#define AURORA_PHOTON_CACHE_HPP

#include <stdint.h>
#include "../drivers/storage/flash_device.hpp"
#include "mutex.hpp"

struct CachePage {
    uint32_t block_addr;
    uint32_t page_offset;
    uint8_t  data[256]; // 对齐闪存 256B 物理页
    bool     is_valid;
    bool     is_dirty;
    bool     is_reserved;
    uint32_t last_access_tick;
};

class PhotonCacheLayer {
private:
    static constexpr int CACHE_POOL_SIZE = 8; // 在 RAM 中开辟 8 个页缓存槽位 (共 2KB)
    CachePage        pool_[CACHE_POOL_SIZE];
    FlashBlockDevice& flash_;
    Mutex            cache_mutex_;
    uint32_t         tick_counter_;

    // 内部方法：将脏页真正刷入底层闪存
    int flush_page(int index) {
        if (!pool_[index].is_valid || !pool_[index].is_dirty) return 0;
        
        int res = -1;
        // P0 Fix: 失败重试 3 次再上报
        for (int retry = 0; retry < 3; retry++) {
            res = flash_.write_blocks(
                pool_[index].block_addr, 
                pool_[index].page_offset, 
                pool_[index].data, 
                flash_.get_page_size()
            );
            if (res == 0) break;
        }
        
        if (res == 0) pool_[index].is_dirty = false;
        return res;
    }

    // 内部方法：LRU (最近最少使用) 算法淘汰替换缓存槽位
    int get_or_alloc_page(uint32_t block_addr, uint32_t page_offset, bool for_write) {
        int oldest_idx = -1;
        uint32_t oldest_tick = 0xFFFFFFFF;

        // 1. 命中检测
        for (int i = 0; i < CACHE_POOL_SIZE; i++) {
            if (pool_[i].is_valid && pool_[i].block_addr == block_addr && pool_[i].page_offset == page_offset) {
                pool_[i].last_access_tick = ++tick_counter_;
                return i;
            }
            if (!pool_[i].is_valid && !pool_[i].is_reserved) {
                oldest_idx = i;
                oldest_tick = 0;
            } else if (!pool_[i].is_reserved && pool_[i].last_access_tick < oldest_tick) {
                oldest_idx = i;
                oldest_tick = pool_[i].last_access_tick;
            }
        }
        
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
        //    page_size 已被 read()/write() 截断至 ≤256，与 tmp_data 对齐。
        uint8_t tmp_data[256];
        uint32_t page_size = flash_.get_page_size();
        if (page_size > 256) page_size = 256;

        cache_mutex_.unlock();
        flash_.read_blocks(block_addr, page_offset, tmp_data, page_size);
        cache_mutex_.lock();

        // 重新检查是否被其他线程加载 (Bug #2 修复: 防止冗余并避免覆盖)
        for (int i = 0; i < CACHE_POOL_SIZE; i++) {
            if (i != oldest_idx && pool_[i].is_valid && pool_[i].block_addr == block_addr && pool_[i].page_offset == page_offset) {
                pool_[oldest_idx].is_reserved = false;
                pool_[i].last_access_tick = ++tick_counter_;
                return i;
            }
        }

        // 如果在 I/O 期间该块被 erase 擦除，erase 会清除 is_reserved
        if (!pool_[oldest_idx].is_reserved) {
            // 预留被取消（说明刚读出的数据已失效，且块被擦除），直接递归重试
            return get_or_alloc_page(block_addr, page_offset, for_write);
        }

        // 5. I/O 完成后，在持锁的原子步骤里将数据与元数据一并写入槽位，
        //    最后才翻 is_valid 对外可见。
        for (uint32_t i = 0; i < page_size; i++) {
            pool_[oldest_idx].data[i] = tmp_data[i];
        }
        pool_[oldest_idx].block_addr      = block_addr;
        pool_[oldest_idx].page_offset     = page_offset;
        pool_[oldest_idx].last_access_tick = ++tick_counter_;
        pool_[oldest_idx].is_valid        = true; // ← 数据确认有效后才对外可见
        pool_[oldest_idx].is_reserved     = false;

        return oldest_idx;
    }

public:
    PhotonCacheLayer(FlashBlockDevice& flash) : flash_(flash), tick_counter_(0) {
        for (int i = 0; i < CACHE_POOL_SIZE; i++) {
            pool_[i].is_valid = false;
            pool_[i].is_dirty = false;
            pool_[i].is_reserved = false;
        }
    }

    ~PhotonCacheLayer() { sync(); }

    // ========================================================
    // 光子缓存读取：0 闪存 I/O 极速 RAM 命中返回
    // ========================================================
    int read(uint32_t block_addr, uint32_t offset, uint8_t* buf, uint32_t size) {
        cache_mutex_.lock();
        uint32_t page_size = flash_.get_page_size();
        if (page_size > 256) page_size = 256; // P0 Fix: Prevent OOB for larger page sizes
        uint32_t bytes_read = 0;

        while (bytes_read < size) {
            uint32_t curr_offset = offset + bytes_read;
            uint32_t page_offset = curr_offset & ~(page_size - 1);
            uint32_t in_page_idx = curr_offset & (page_size - 1);
            uint32_t chunk = page_size - in_page_idx;
            if (chunk > size - bytes_read) chunk = size - bytes_read;

            int cache_idx = get_or_alloc_page(block_addr, page_offset, false);
            // P0 Fix: Handle -1 safely
            if (cache_idx == -1) {
                cache_mutex_.unlock();
                return -1; 
            }
            
            for (uint32_t i = 0; i < chunk; i++) {
                buf[bytes_read + i] = pool_[cache_idx].data[in_page_idx + i];
            }
            bytes_read += chunk;
        }
        cache_mutex_.unlock();
        return bytes_read;
    }

    // ========================================================
    // 光子缓存写入：将小写聚拢在 RAM 缓冲页，绝不立刻触发闪存磨损
    // ========================================================
    int write(uint32_t block_addr, uint32_t offset, const uint8_t* buf, uint32_t size) {
        cache_mutex_.lock();
        uint32_t page_size = flash_.get_page_size();
        if (page_size > 256) page_size = 256; // P0 Fix: Prevent OOB
        uint32_t bytes_written = 0;

        while (bytes_written < size) {
            uint32_t curr_offset = offset + bytes_written;
            uint32_t page_offset = curr_offset & ~(page_size - 1);
            uint32_t in_page_idx = curr_offset & (page_size - 1);
            uint32_t chunk = page_size - in_page_idx;
            if (chunk > size - bytes_written) chunk = size - bytes_written;

            int cache_idx = get_or_alloc_page(block_addr, page_offset, true);
            // P0 Fix: Handle -1 safely
            if (cache_idx == -1) {
                cache_mutex_.unlock();
                return -1;
            }
            
            for (uint32_t i = 0; i < chunk; i++) {
                pool_[cache_idx].data[in_page_idx + i] = buf[bytes_written + i];
            }
            pool_[cache_idx].is_dirty = true; // 仅标记为脏，延迟物理落盘！
            bytes_written += chunk;
        }
        cache_mutex_.unlock();
        return bytes_written;
    }

    int erase(uint32_t block_addr) {
        cache_mutex_.lock();
        // 如果要擦除某块，先让缓存池中属于该块的所有页失效
        for (int i = 0; i < CACHE_POOL_SIZE; i++) {
            if ((pool_[i].is_valid || pool_[i].is_reserved) && pool_[i].block_addr == block_addr) {
                pool_[i].is_valid = false;
                pool_[i].is_dirty = false;
                pool_[i].is_reserved = false; // 取消其他线程的预留
            }
        }
        cache_mutex_.unlock();
        // P0 Fix: Unlock before I/O
        int res = flash_.erase_block(block_addr);
        return res;
    }

    // ========================================================
    // 显式同步 (Sync)：将所有脏页批量高效刷入闪存
    // ========================================================
    int sync() {
        int final_res = 0;
        cache_mutex_.lock();
        for (int i = 0; i < CACHE_POOL_SIZE; i++) {
            if (pool_[i].is_valid && pool_[i].is_dirty) {
                // To avoid holding lock during flush, we could copy data, but simple solution is to keep lock.
                // It's acceptable for explicit sync to block.
                int res = flush_page(i);
                if (res != 0 && final_res == 0) {
                    final_res = res; // 记录第一次遇到的错误
                }
            }
        }
        cache_mutex_.unlock();
        return final_res;
    }
};

#endif
