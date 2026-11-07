// =============================================================================
// drivers/virtio/virtio_blk.hpp
//
// virtio-blk 块设备驱动：实现 kernel BlockDevice 契约（read_blocks/write_blocks，
// 语义对齐 drivers/storage/flash_device.hpp：块号 + 块内偏移，0 成功 / -1 失败）。
//
// 轮询模式：单请求在途，add_buffer → notify → 自旋 pop_used（带上限，超时
// 返回 -1 不挂死）。块大小固定 512B（= 原生扇区），请求经纯函数
// blk_build_chain() 组装 3 段描述符链（header | data | status）。
// =============================================================================
#ifndef AURORA_VIRTIO_BLK_HPP
#define AURORA_VIRTIO_BLK_HPP

#include <stdint.h>
#include "device.hpp"
#include "mutex.hpp"
#include "virtio_defs.hpp"
#include "virtio_queue.hpp"

namespace auroraos {
namespace virtio {

// 组装 blk 请求的 3 段描述符链（纯函数，host 可测）：
//   out[0] = 请求头（设备只读），out[1] = 数据（读盘时设备写），
//   out[2] = 状态字节（设备写）。data_len 为数据段字节数。
void blk_build_chain(const BlkReqHeader* hdr, void* data, uint32_t data_len,
                     bool is_read, uint8_t* status, BufSeg out[3]) noexcept;

// 字节范围 → (扇区, 扇区内偏移, 本段字节数) 换算（纯函数，host 可测）。
// 返回 false 表示 remaining==0（迭代结束）。
bool blk_next_chunk(uint64_t byte_addr, uint32_t remaining,
                    uint32_t* sector, uint32_t* offset, uint32_t* chunk) noexcept;

class VirtioBlk : public BlockDevice {
public:
    static constexpr uint32_t SECTOR_SIZE = 512;
    static constexpr uint16_t QUEUE_SIZE = 8;
    static constexpr uint32_t MAX_POLL_SPINS = 1000000;

    // 描述符环 / bounce 缓冲区由 init() 在探测成功后统一填写，构造期不预填
    // （静态实例在 .bss 里本就为零）。cppcheck 的 uninitMemberVar 对这类
    // 「延迟初始化」成员是静态误报，逐个抑制。
    // cppcheck-suppress uninitMemberVar
    VirtioBlk(const char* name, IVirtioTransport& transport) noexcept
        : BlockDevice(name), transport_(transport) {}

    // 板级工厂：按 BOARD_VIRTIO_* 常量定位传输口（见 virtio_blk.cpp）
    static VirtioBlk& instance();

    // 探测 + 初始化。设备不存在（probe 未命中）返回 false 且 ready() 保持
    // false；调用方打印 skip warning 继续 boot（fail-safe）。
    bool init() noexcept;

    bool ready() const noexcept {
        return ready_;
    }

    // probe 是否命中（区分「无设备跳过」与「初始化失败」两类日志）
    bool probed() const noexcept {
        return probed_;
    }

    // BlockDevice 契约：block_size == SECTOR_SIZE，块号即扇区号
    int read_blocks(uint32_t block_addr, uint32_t offset, uint8_t* buf, uint32_t size) override;
    int write_blocks(uint32_t block_addr, uint32_t offset, const uint8_t* buf, uint32_t size) override;

    uint64_t capacity_sectors() const noexcept {
        return capacity_sectors_;
    }

    // 请求队列访问（host 测试的设备模拟器绑定环缓冲用）
    VirtQueue& queue() noexcept {
        return queue_;
    }

private:
    // 发起一个扇区请求（data 恰为 SECTOR_SIZE 字节）并等待完成
    int issue(uint32_t type, uint64_t sector, void* data, bool data_device_writable) noexcept;
    int xfer(bool is_write, uint64_t byte_addr, void* buf, uint32_t size) noexcept;
    // 请求异常（孤儿完成/超时）后设备置失败态：在途描述符与缓冲不再复用，
    // 防止设备迟到 DMA 回写打坏新请求或调用方缓冲。
    void fail_device() noexcept;

    IVirtioTransport& transport_;
    VirtQueue queue_;
    alignas(4096) VirtqDesc desc_[QUEUE_SIZE];
    alignas(4096) AvailRing avail_;
    alignas(4096) UsedRing used_;
    // 请求头含 uint64_t 字段，自然对齐已是 8；不能再声明 alignas(4)（小于类型
    // 最小对齐，属错误声明：clang 直接报错，GCC 只是放过）。
    BlkReqHeader req_hdr_;
    alignas(4) uint8_t req_status_ = 0xFF;
    alignas(4) uint8_t bounce_[SECTOR_SIZE];
    Mutex io_mutex_;
    uint64_t capacity_sectors_ = 0;
    bool ready_ = false;
    bool probed_ = false;
};

} // namespace virtio
} // namespace auroraos

#endif // AURORA_VIRTIO_BLK_HPP
